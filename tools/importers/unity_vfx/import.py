#!/usr/bin/env python3
"""Unity particle effects -> RTSEngine particle effects ("make the same effect").

    python3 tools/importers/unity_vfx/import.py --source <file.prefab | folder | file.unitypackage>
        --game templates/<id> [--name fire] [--prefab Explosion] [--scale 1] [--ignore-root-rotation] [--json] [--dry-run]

Reads Unity's serialized YAML (ParticleSystem = class 198, ParticleSystemRenderer = 199, GameObject /
Transform hierarchy, nested prefab instances and variants, force fields, colliders, materials and .meta
files) with the standard library only, maps every ParticleSystem module onto the engine format
(engine/fx/FxEffect.h; mapping.py, mapping_look.py, convert.py) and writes

    <game>/fx/<name>.json             one emitter per Unity ParticleSystem (child systems = layers) + forceFields
    <game>/fx/<name>_<child>.json     sub-emitters as effects of their own
    <game>/fx/textures/*.png          main textures and exported sprites (TGA decoded here; PSD/JPG/TIF/EXR via ImageMagick/ffmpeg)
    <game>/models/<file>              FBX / OBJ / glTF meshes of mesh shapes and mesh particles (copied)

and a report of what was mapped exactly, approximately or dropped (VFX Graph, custom shader code, soft
particles / distortion, custom vertex streams, scripts, ...). Unity space (Y up, left-handed) becomes engine
space by flipping Z; the effect's "direction" frame is the engine's play direction (+Y). Docs:
docs/particles.md ("Unity import").
"""
import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import convert as cv  # noqa: E402
import mapping as mp  # noqa: E402
import mapping_look as ml  # noqa: E402
from materials import Meshes, sanitize, Textures  # noqa: E402
from unity_scene import Assets, Prefab, Report, extract_unitypackage, project_roots, q_inv, q_rot, v_len, v_norm  # noqa: E402
from unity_yaml import get, num  # noqa: E402

CLASS_GAMEOBJECT, CLASS_TRANSFORM, CLASS_RECT_TRANSFORM = 1, 4, 224
CLASS_PS, CLASS_PSR, CLASS_MATERIAL, CLASS_LIGHT, CLASS_MONO = 198, 199, 21, 108, 114
CLASS_PREFAB_INSTANCE = 1001
CLASS_FORCE_FIELD = 330
CLASS_VISUAL_EFFECT = 2083052967
OTHER_COMPONENTS = {
    82: 'AudioSource', 95: 'Animator', 111: 'Animation', 23: 'MeshRenderer', 33: 'MeshFilter', 137: 'SkinnedMeshRenderer',
    212: 'SpriteRenderer', 120: 'LineRenderer', 96: 'TrailRenderer', 54: 'Rigidbody', 65: 'BoxCollider', 135: 'SphereCollider',
    136: 'CapsuleCollider', 64: 'MeshCollider', 1183024399: 'LookAtConstraint', 210: 'SortingGroup',
}


# ------------------------------------------------------------------------------------------ mapping

def map_system(ctx, ps, psr, go_id, wpos, wrot, wscale, path, as_sub):
    """One Unity ParticleSystem -> one engine emitter (dict) + its sub-emitter references."""
    s = mp.System(ctx, ps, psr, go_id, wpos, wrot, wscale, path, as_sub)
    em = {'comment': 'Unity: ' + path}
    mp.map_main(s, em)
    mp.map_emission(s, em)
    spawn, direction = mp.map_shape(s, em)
    rdir = v_norm(direction)
    if v_len(tuple(a - b for a, b in zip(rdir, (0, 1, 0)))) > 1e-3:
        em['direction'] = mp.vec(rdir, 3)
    if not as_sub:
        off = ctx.eff_point(spawn)
        if v_len(off) > 1e-4:
            em['offset'] = mp.vec(off, 3)
    mp.map_velocity(s, em)
    mp.map_forces(s, em)
    mat = ml.map_renderer(s, em)
    ml.map_over_life(s, em, mat.tint)
    ml.model_file_turn(s, em)
    ml.map_noise(s, em)
    ml.map_texture_sheet(s, em, mat.blend)
    ml.map_collision(s, em)
    ml.map_trigger(s, em)
    ml.map_trails(s, em)
    ml.map_lights(s, em)
    subs = ml.map_subs(s)
    ctx.rep.systems.append({'system': path, 'modules': s.log.modules})
    if not s.emits:
        s.log.approx('emission', 'system emits nothing (no rate, distance or bursts): skipped')
        return None, []
    return em, subs


def map_force_fields(ctx, prefab, rel_name):
    """ParticleSystemForceField components of the hierarchy -> effect-level "forceFields"."""
    out = []
    for tr, go, pos, rot, scale, path in prefab.walk():
        for c in prefab.comp_by_go.get(go, []):
            if c.class_id != CLASS_FORCE_FIELD or not int(num(c.data.get('m_Enabled'), 1)):
                continue
            p = c.data.get('m_Parameters') or c.data
            approx = []

            def avg(key, what):
                cu = cv.Curve(p.get(key))
                if cu.curved or cu.random:
                    approx.append('%s curve -> its average' % what)
                return cu.mean()

            smax = max(abs(x) for x in scale) * ctx.scale
            shape = int(num(p.get('m_Shape'), 0))
            f = {'position': mp.vec(ctx.eff_point(pos), 3)}
            if shape == 2:
                f['shape'] = 'cylinder'
                f['length'] = mp.rv(min(1000, num(p.get('m_Length'), 1) * smax))
            elif shape in (1, 3):
                approx.append('%s shape -> sphere' % ('hemisphere' if shape == 1 else 'box'))
            f['radius'] = mp.rv(min(1000, max(0.01, num(p.get('m_EndRange'), 1) * smax)))
            sr = num(p.get('m_StartRange'), 0) * smax
            if sr > 0:
                f['startRadius'] = mp.rv(min(1000, sr))
            dvec = tuple(avg('m_DirectionCurve' + k, 'direction ' + k.lower()) for k in 'XYZ')
            if v_len(dvec) > 1e-6:   # field-local axes
                f['direction'] = mp.vec(tuple(x * ctx.scale for x in cv.mv(ctx.Mw, q_rot(rot, dvec))), 3)
            g = avg('m_GravityCurve', 'gravity')
            if abs(g) > 1e-6:
                f['gravity'] = mp.rv(max(-1000, min(1000, g * ctx.scale)))
                foc = num(p.get('m_GravityFocus'), 0)
                if foc > 0:
                    f['gravityFocus'] = mp.rv(min(1, foc))
            rs = avg('m_RotationSpeedCurve', 'rotation speed')
            if abs(rs) > 1e-6:   # radians/s around the field's Y, a mirrored axial vector: the sign flips
                f['rotation'] = mp.rv(max(-36000, min(36000, -rs * mp.RAD)))
                ra = avg('m_RotationAttractionCurve', 'rotation attraction')
                if ra > 0:
                    f['rotationAttraction'] = mp.rv(min(1, ra))
            drag = avg('m_DragCurve', 'drag')
            if drag > 0:
                f['drag'] = mp.rv(min(100, drag))
            up = cv.mv(ctx.Mw, q_rot(rot, (0.0, 1.0, 0.0)))
            if abs(abs(up[1]) - 1) > 1e-3 and (shape == 2 or abs(rs) > 1e-6):
                approx.append('tilted field: its axis -> the effect Y axis')
            if get(p, 'm_VectorField', 'fileID'):
                approx.append('vector field (3D texture) dropped')
            for a in approx:
                ctx.rep.approx.append('%s: forceField %s: %s' % (rel_name, path, a))
            out.append(f)
    if len(out) > 8:
        ctx.rep.dropped.append('%s: more than 8 force fields (the rest dropped)' % rel_name)
    return out[:8]


# ------------------------------------------------------------------------------------------ effect

ORDER = ['comment', 'texture', 'sprites', 'frames', 'row', 'fps', 'cycles', 'frameOverTime', 'framesBySpeed', 'startFrame', 'flip', 'blend',
         'align', 'meshes', 'pivot', 'sort', 'sortOrder', 'hidden', 'burst', 'bursts', 'rate', 'perMeter', 'delay', 'duration', 'loop',
         'prewarm', 'life', 'speed', 'size', 'sizeY', 'sizeZ', 'aspect', 'rotation', 'rotation3D', 'spin', 'spin3D', 'startColor',
         'direction', 'spread', 'shape', 'mesh', 'emitFrom', 'radius', 'donutRadius', 'length', 'arc', 'arcMode', 'arcSpeed',
         'arcSpread', 'thickness', 'radial', 'box', 'shapeRotation', 'randomDirection', 'sphericalDirection', 'randomPosition',
         'alignToDirection', 'offset', 'space', 'gravity', 'gravityScale', 'drag', 'dragBySize', 'dragBySpeed', 'velocity',
         'velocityOverLife', 'force', 'speedLimit', 'speedLimitAxes', 'limitDampen', 'inheritVelocity', 'lifeBySpeed',
         'externalForces', 'noise', 'stretch', 'cameraStretch', 'sizeOverLife', 'sizeOverLifeMin', 'sizeOverLifeY', 'sizeOverLifeZ',
         'color', 'colorMin', 'colorBySpeed', 'sizeBySpeed', 'spinBySpeed', 'collision', 'trigger', 'trail', 'light', 'subEmitters',
         'maxParticles', 'ringBuffer', 'minScreenSize', 'maxScreenSize', 'minQuality']


def ordered(em):
    out = {k: em[k] for k in ORDER if k in em}
    out.update({k: v for k, v in em.items() if k not in out})
    return out


def dumps_effect(effect):
    """Readable JSON: one emitter key per line, short arrays inline."""
    def enc(v, ind):
        if isinstance(v, dict):
            if not v:
                return '{}'
            inner = ', '.join('"%s": %s' % (k, enc(x, ind)) for k, x in v.items())
            if len(inner) < 100 and not any(isinstance(x, (dict, list)) and len(json.dumps(x)) > 60 for x in v.values()):
                return '{ ' + inner + ' }'
            pad = ' ' * (ind + 2)
            return '{\n' + ',\n'.join('%s"%s": %s' % (pad, k, enc(x, ind + 2)) for k, x in v.items()) + '\n' + ' ' * ind + '}'
        if isinstance(v, list):
            if all(not isinstance(x, dict) for x in v):
                return json.dumps(v)
            pad = ' ' * (ind + 2)
            return '[\n' + ',\n'.join(pad + enc(x, ind + 2) for x in v) + '\n' + ' ' * ind + ']'
        return json.dumps(v)
    return enc(effect, 0) + '\n'


def import_prefab(path, assets, opts, rep, name, game_dir, quiet=False):
    prefab = Prefab(path, assets, rep)
    rel_name = os.path.basename(path)
    if quiet and not any(d.class_id in (CLASS_PS, CLASS_VISUAL_EFFECT) for d in prefab.docs):
        return []   # a prefab instance / legacy prefab without particle systems: nothing to report
    used_refs = set()   # components the particle systems use (trigger colliders, mesh / sprite shape renderers)
    for p in prefab.docs:
        if p.class_id != CLASS_PS:
            continue
        for mod, keys in (('TriggerModule', ['primitives'] + ['collisionShape%d' % i for i in range(6)]),
                          ('ShapeModule', ['m_MeshRenderer', 'm_SkinnedMeshRenderer', 'm_SpriteRenderer'])):
            for k in keys:
                v = get(p.data, mod, k)
                for ref in (v if isinstance(v, list) else [v]):
                    if isinstance(ref, dict) and get(ref, 'fileID'):
                        used_refs.add(get(ref, 'fileID'))
                        comp = prefab.by_id.get(get(ref, 'fileID'))
                        if comp is not None and comp.class_id == 23:   # its MeshFilter too
                            used_refs.update(c.file_id for c in prefab.comp_by_go.get(get(comp.data, 'm_GameObject', 'fileID'), []) if c.class_id == 33)
    for d in prefab.docs:
        if d.class_id == CLASS_VISUAL_EFFECT:
            rep.dropped.append('%s: VisualEffect (VFX Graph) component: VFX Graph effects are GPU node graphs, not ParticleSystems; '
                               'they cannot be converted. Rebuild the look from fx_templates or describe it and edit with fx_set.' % rel_name)
        elif d.class_id == CLASS_MONO:
            script = assets.path(get(d.data, 'm_Script') or {})
            rep.dropped.append('%s: script %s (behaviour is code: bind the effect to game events with fx_bind instead)' %
                               (rel_name, os.path.basename(script) if script else get(d.data, 'm_Script', 'guid', default='?')))
        elif d.class_id in OTHER_COMPONENTS and not d.stripped and d.file_id not in used_refs:
            go = get(d.data, 'm_GameObject', 'fileID')
            rep.dropped.append('%s: %s on %s' % (rel_name, OTHER_COMPONENTS[d.class_id], prefab.name(go)))
        elif d.class_id == CLASS_LIGHT:
            used = any(get(p.data, 'LightsModule', 'light', 'fileID') == d.file_id for p in prefab.docs if p.class_id == CLASS_PS)
            if not used:
                rep.dropped.append('%s: Light component on %s (not a particle light): add a map light or a "light" module' %
                                   (rel_name, prefab.name(get(d.data, 'm_GameObject', 'fileID'))))
    rep.dropped += [x for x in prefab.nested_failed if x not in rep.dropped]
    rep.notes += prefab.nested_notes
    walk = prefab.walk()
    if not walk:
        rep.warnings.append('%s: no GameObject hierarchy' % rel_name)
        return []
    root_tr, _, root_pos, root_rot, _, _ = walk[0]
    root_rot_inv = q_inv(root_rot) if opts.ignore_root_rotation else (0.0, 0.0, 0.0, 1.0)
    if not getattr(opts, 'textures', None):
        opts.textures = Textures(game_dir, assets, rep, opts.dry_run)
    if not getattr(opts, 'meshes', None):
        opts.meshes = Meshes(game_dir, assets, rep, opts.dry_run)
    ctx = mp.Context(opts, rep, assets, opts.textures, opts.meshes, root_rot_inv, prefab, root_pos)
    systems = []  # (ps doc, psr doc, go, pos, rot, scale, path)
    for tr, go, pos, rot, scale, p in walk:
        comps = prefab.comp_by_go.get(go, [])
        ps = next((c for c in comps if c.class_id == CLASS_PS), None)
        if ps is None:
            continue
        psr = next((c for c in comps if c.class_id == CLASS_PSR), None)
        systems.append((ps, psr, go, pos, rot, scale, p))
    if not systems:
        if not quiet and not any(d.class_id == CLASS_VISUAL_EFFECT for d in prefab.docs):
            rep.warnings.append('%s: no ParticleSystem in this prefab' % rel_name)
        return []
    by_ps = {s[0].file_id: s for s in systems}
    sub_ids = set()
    for s in systems:
        if int(num(get(s[0].data, 'SubModule', 'enabled'), 0)):
            for e in get(s[0].data, 'SubModule', 'subEmitters', default=[]) or []:
                t = get(e, 'emitter', 'fileID')
                if t in by_ps:
                    sub_ids.add(t)
    used_names = set(opts.used_names)
    sub_names = {}

    def sub_name(ps_id):
        if ps_id not in sub_names:
            base = sanitize('%s_%s' % (name, prefab.name(by_ps[ps_id][2])), 44)
            n, k = base, 2
            while n in used_names:
                n = '%s_%d' % (base, k)
                k += 1
            used_names.add(n)
            sub_names[ps_id] = n
        return sub_names[ps_id]

    used_names.add(name)
    outputs = []

    def build(effect_name, members, as_sub):
        emitters = []
        for ps, psr, go, pos, rot, scale, p in members:
            em, subs = map_system(ctx, ps, psr, go, pos, rot, scale, p, as_sub)
            if em is None:
                continue
            se = []
            for s in subs:
                if s['target'] not in by_ps:
                    rep.dropped.append('%s: sub-emitter outside this prefab' % p)
                    continue
                o = {'on': s['on'], 'effect': sub_name(s['target'])}
                if s['chance'] < 1:
                    o['chance'] = mp.rv(round(max(0, s['chance']), 3))
                if s['inherit'] == ['color']:
                    o['inheritColor'] = True
                elif s['inherit']:
                    o['inherit'] = s['inherit']
                se.append(o)
            if se:
                em['subEmitters'] = se[:8]
                if len(se) > 8:
                    rep.dropped.append('%s: more than 8 sub-emitters' % p)
            emitters.append(ordered(em))
        if len(emitters) > 16:
            rep.dropped.append('%s: %d systems, the engine keeps 16 per effect (the rest dropped)' % (effect_name, len(emitters)))
            emitters = emitters[:16]
        if not emitters:
            return
        effect = {'comment': 'Imported from Unity %s by tools/importers/unity_vfx (report: what was approximated or dropped).' % rel_name,
                  'emitters': emitters}
        if not as_sub and fields and any('externalForces' in e for e in emitters):
            effect['forceFields'] = fields
        outputs.append((effect_name, effect))

    fields = map_force_fields(ctx, prefab, rel_name)
    if fields and not any(int(num(get(s[0].data, 'ExternalForcesModule', 'enabled'), 0)) for s in systems):
        rep.dropped.append('%s: force fields without a system that has External Forces enabled (nothing would react)' % rel_name)
    build(name, [s for s in systems if s[0].file_id not in sub_ids], False)
    done = set()
    while True:
        pending = [i for i in sub_names if i not in done]
        if not pending:
            break
        for i in pending:
            done.add(i)
            build(sub_names[i], [by_ps[i]], True)
    opts.used_names = used_names
    return outputs


def find_inputs(source, rep):
    """Prefabs to import and VFX Graph assets to explain."""
    prefabs, vfx = [], []
    if os.path.isfile(source):
        if source.lower().endswith('.vfx'):
            vfx.append(source)
        else:
            prefabs.append(source)
        return prefabs, vfx
    for dp, dns, fns in os.walk(source):
        dns[:] = sorted(d for d in dns if d not in ('Library', 'Temp', 'Logs', '.git'))
        for fn in sorted(fns):
            low = fn.lower()
            if low.endswith('.prefab'):
                prefabs.append(os.path.join(dp, fn))
            elif low.endswith('.vfx'):
                vfx.append(os.path.join(dp, fn))
    return prefabs, vfx


def run(argv=None):
    ap = argparse.ArgumentParser(description='Unity particle effects -> RTSEngine fx')
    ap.add_argument('--source', required=True, help='.prefab, a folder with prefabs (+ materials, textures, .meta) or a .unitypackage')
    ap.add_argument('--game', required=True, help='game folder (templates/<id>): writes fx/<name>.json and fx/textures/')
    ap.add_argument('--name', help='effect name (default: the prefab name)')
    ap.add_argument('--prefab', help='only prefabs whose file name contains this')
    ap.add_argument('--scale', type=float, default=1.0, help='Unity metres -> engine units (default 1)')
    ap.add_argument('--ignore-root-rotation', action='store_true', help='treat the prefab root as unrotated (Instantiate with identity)')
    ap.add_argument('--json', action='store_true', help='print the report as JSON')
    ap.add_argument('--dry-run', action='store_true', help='write nothing, only report')
    opts = ap.parse_args(argv)
    rep = Report()
    opts.used_names, opts.textures, opts.meshes = set(), None, None
    if opts.scale <= 0:
        rep.warnings.append('error: --scale must be > 0')
        return finish(rep, opts, 2)
    tmp = None
    source = os.path.abspath(os.path.expanduser(opts.source))
    if not os.path.exists(source):
        rep.warnings.append('error: no such file or folder: %s' % opts.source)
        return finish(rep, opts, 2)
    if source.lower().endswith('.unitypackage'):
        tmp = tempfile.TemporaryDirectory()
        source = extract_unitypackage(source, tmp.name)
        rep.notes.append('unitypackage extracted (%s)' % os.path.basename(opts.source))
    game_dir = os.path.abspath(opts.game)
    if not os.path.isdir(game_dir):
        rep.warnings.append('error: game folder not found: %s' % opts.game)
        return finish(rep, opts, 2)
    assets = Assets(project_roots(source), rep)
    prefabs, vfx = find_inputs(source, rep)
    for v in vfx:
        rep.dropped.append('%s: VFX Graph asset. VFX Graph runs node graphs on the GPU (compute), not the ParticleSystem '
                           'modules, so there is nothing to map; describe the look and build it with fx_templates / fx_set.' % os.path.basename(v))
    if opts.prefab:
        prefabs = [p for p in prefabs if opts.prefab.lower() in os.path.basename(p).lower()]
    with_ps, maybe_empty = [], set()
    for p in prefabs:
        try:
            with open(p, 'r', encoding='utf-8', errors='replace') as f:
                txt = f.read()
        except OSError as e:
            rep.warnings.append('cannot read %s: %s' % (p, e))
            continue
        if not txt.startswith('%YAML'):
            rep.warnings.append('error: %s is not a text prefab (Unity "Force Text" serialization is needed)' % os.path.basename(p))
            continue
        if '--- !u!198 ' in txt or '!u!2083052967' in txt or len(prefabs) == 1:
            with_ps.append(p)
        elif 'm_SourcePrefab:' in txt:   # nested instances / variants: systems may come from the source prefab
            with_ps.append(p)
            maybe_empty.add(p)
    if not with_ps and not vfx:
        rep.warnings.append('error: no prefab with a ParticleSystem found in %s' % opts.source)
    total = 0
    for p in with_ps:
        stem = os.path.splitext(os.path.basename(p))[0]
        name = sanitize(opts.name if opts.name and len(with_ps) == 1 else ('%s_%s' % (opts.name, stem) if opts.name else stem), 40)
        try:
            outs = import_prefab(p, assets, opts, rep, name, game_dir, quiet=p in maybe_empty)
        except Exception as e:  # a broken file must not stop the others
            rep.warnings.append('error: %s: %s: %s' % (os.path.basename(p), type(e).__name__, e))
            continue
        for ename, effect in outs:
            rel = 'fx/%s.json' % ename
            if not opts.dry_run:
                os.makedirs(os.path.join(game_dir, 'fx'), exist_ok=True)
                with open(os.path.join(game_dir, rel), 'w', encoding='utf-8') as f:
                    f.write(dumps_effect(effect))
            rep.effects.append({'name': ename, 'file': rel, 'emitters': len(effect['emitters']), 'source': os.path.basename(p)})
            total += 1
    if total:
        rep.notes.append('Unity Y-up left-handed -> engine (z flipped); scale %g; root rotation %s' %
                         (opts.scale, 'ignored' if opts.ignore_root_rotation else 'kept'))
        rep.notes.append('preview: fx_preview {effect} then view_screenshot; bind to events: fx_bind')
    if tmp:
        tmp.cleanup()
    return finish(rep, opts, 0 if total else 1)


def finish(rep, opts, code):
    if getattr(opts, 'json', False):
        print(json.dumps(rep.as_json(), indent=1, ensure_ascii=False))
    else:
        print(rep.text())
    return code


if __name__ == '__main__':
    sys.exit(run())

#!/usr/bin/env python3
"""Unity particle effects -> RTSEngine particle effects ("make the same effect").

    python3 tools/importers/unity_vfx/import.py --source <file.prefab | folder | file.unitypackage>
        --game templates/<id> [--name fire] [--prefab Explosion] [--scale 1] [--ignore-root-rotation] [--json] [--dry-run]

Reads Unity's serialized YAML (ParticleSystem = class 198, ParticleSystemRenderer = 199, GameObject /
Transform hierarchy, materials and .meta files) with the standard library only, maps every module the
engine format can express (engine/fx/FxEffect.h) and writes

    templates/<id>/fx/<name>.json             one emitter per Unity ParticleSystem (child systems = layers)
    templates/<id>/fx/<name>_<child>.json     sub-emitters (birth / death / collision) as effects of their own
    templates/<id>/fx/textures/*.png          main textures (TGA decoded here; PSD/JPG/TIF/EXR via ImageMagick/ffmpeg)

and a report of what was mapped exactly, approximately or dropped (custom shaders, Shader Graph, VFX Graph,
meshes as particles, scripts, ...). Unity space (Y up, left-handed) becomes engine space by flipping Z; the
effect's "direction" frame is the engine's play direction (+Y). Docs: docs/particles.md ("Unity import").
"""
import argparse
import json
import math
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unity_yaml as uy  # noqa: E402
from materials import read_material, sanitize, Textures  # noqa: E402
from unity_scene import (Assets, Prefab, Report, SysLog, extract_unitypackage, project_roots, q_euler, q_inv, q_mul, q_rot, r, rng, u2rel, u2world, v_len, v_mul, v_norm)  # noqa: E402
from unity_yaml import get, num, mmc_range, mmc_mean, mmc_is_curve, mmc_keys, mmg_state, mmg_keys, hex_color  # noqa: E402

CLASS_GAMEOBJECT, CLASS_TRANSFORM, CLASS_RECT_TRANSFORM = 1, 4, 224
CLASS_PS, CLASS_PSR, CLASS_MATERIAL, CLASS_LIGHT, CLASS_MONO = 198, 199, 21, 108, 114
CLASS_PREFAB_INSTANCE = 1001
CLASS_VISUAL_EFFECT = 2083052967
OTHER_COMPONENTS = {
    82: 'AudioSource', 95: 'Animator', 111: 'Animation', 23: 'MeshRenderer', 33: 'MeshFilter', 137: 'SkinnedMeshRenderer',
    212: 'SpriteRenderer', 120: 'LineRenderer', 96: 'TrailRenderer', 54: 'Rigidbody', 65: 'BoxCollider', 135: 'SphereCollider',
    136: 'CapsuleCollider', 64: 'MeshCollider', 1183024399: 'LookAtConstraint', 210: 'SortingGroup',
}
SHAPES = {0: 'Sphere', 1: 'SphereShell', 2: 'Hemisphere', 3: 'HemisphereShell', 4: 'Cone', 5: 'Box', 6: 'Mesh', 7: 'ConeShell',
          8: 'ConeVolume', 9: 'ConeVolumeShell', 10: 'Circle', 11: 'CircleEdge', 12: 'SingleSidedEdge', 13: 'MeshRenderer',
          14: 'SkinnedMeshRenderer', 15: 'BoxShell', 16: 'BoxEdge', 17: 'Donut', 18: 'Rectangle', 19: 'Sprite', 20: 'SpriteRenderer'}
GRAVITY = 9.81
RAD = 180.0 / math.pi


# ------------------------------------------------------------------------------------------ mapping

LIMITS = {'life': (0.01, 60), 'speed': (0, 500), 'size': (0.001, 100), 'rotation': (-3600, 3600), 'spin': (-36000, 36000),
          'spread': (0, 180), 'radius': (0, 100), 'drag': (0, 50), 'stretch': (0, 10), 'rate': (0, 2000), 'perMeter': (0, 200),
          'burst': (0, 2000), 'delay': (0, 60), 'duration': (0, 600), 'fps': (0, 120), 'aspect': (0.01, 100), 'cycles': (0.01, 1000),
          'speedLimit': (0, 1000), 'maxParticles': (0, 100000), 'arc': (0, 360), 'thickness': (0, 1), 'radial': (0, 1)}


def clampv(key, v, log):
    lo, hi = LIMITS[key]
    if isinstance(v, list):
        a, b = clampv(key, v[0], log), clampv(key, v[1], log)
        return a if a == b else [a, b]
    if v < lo or v > hi:
        log.approx('limits', '%s %g clamped to %g..%g' % (key, v, lo, hi))
    if key in ('burst', 'maxParticles'):
        return int(round(min(hi, max(lo, v))))
    v = r(min(hi, max(lo, v)), 4)
    return int(v) if float(v).is_integer() else v


def color_keys_json(keys):
    out, last = [], None
    for t, c in keys:
        item = [r(t, 3), hex_color(c)]
        if item != last:
            out.append(item)
        last = item
    return out


def tint_keys(keys, tint):
    return [(t, tuple(min(1.0, max(0.0, a * b)) for a, b in zip(c, tint))) for t, c in keys]


class Context:
    def __init__(self, opts, report, assets, textures, root_rot_inv):
        self.opts, self.rep, self.assets, self.tex = opts, report, assets, textures
        self.root_rot_inv = root_rot_inv
        self.scale = opts.scale


def map_system(ctx, prefab, ps, psr, go_id, wpos, wrot, wscale, path, root_pos, as_sub):
    """One Unity ParticleSystem -> one engine emitter (dict) + its sub-emitter references."""
    d = ps.data
    log = SysLog(ctx.rep, path)
    S = ctx.scale
    rot = q_mul(ctx.root_rot_inv, wrot)
    em = {'comment': 'Unity: ' + path}
    main = d.get('InitialModule') or {}
    sim_speed = num(d.get('simulationSpeed'), 1.0) or 1.0
    if abs(sim_speed - 1) > 1e-3:
        log.approx('main', 'simulationSpeed %g folded into times and speeds' % sim_speed)
    # scaling mode: 0 hierarchy, 1 local, 2 shape only
    mode = int(num(d.get('scalingMode'), 1))
    lscale = prefab.local(prefab.tr_by_go[go_id])[2] if go_id in prefab.tr_by_go else (1, 1, 1)
    size_scale = (sum(abs(x) for x in wscale) / 3 if mode == 0 else sum(abs(x) for x in lscale) / 3 if mode == 1 else 1.0) * S
    shape_scale = sum(abs(x) for x in wscale) / 3 * S

    # ---- main
    duration = max(0.05, num(d.get('lengthInSec'), 5.0)) / sim_speed
    looping = bool(int(num(d.get('looping'), 1)))
    if int(num(d.get('prewarm'), 0)):
        log.drop('main', 'prewarm (effect starts empty)')
    delay = mmc_mean(d.get('startDelay')) / sim_speed
    if mmc_state(d.get('startDelay')) == 3:
        log.approx('main', 'random start delay -> its middle')
    lt = mmc_range(main.get('startLifetime'))
    sp = mmc_range(main.get('startSpeed'))
    sz = mmc_range(main.get('startSize'))
    for k, m in (('startLifetime', main.get('startLifetime')), ('startSpeed', main.get('startSpeed')), ('startSize', main.get('startSize'))):
        if mmc_is_curve(m):
            log.approx('main', '%s curve over the system duration -> random between its min and max' % k)
    life = [lt[0] / sim_speed, lt[1] / sim_speed]
    speed = [sp[0] * S * sim_speed, sp[1] * S * sim_speed]
    reverse = False
    if speed[1] <= 0 and speed[0] < 0:
        speed, reverse = [-speed[1], -speed[0]], True
        log.approx('main', 'negative start speed -> reversed emission direction')
    elif speed[0] < 0:
        speed[0] = 0
        log.approx('main', 'start speed range crossing zero -> clamped at 0')
    em['life'] = clampv('life', rng(*life), log)
    em['speed'] = clampv('speed', rng(*speed), log)
    em['size'] = clampv('size', rng(sz[0] * size_scale, sz[1] * size_scale, 4), log)
    if int(num(main.get('size3D'), 0)):
        sy = mmc_mean(main.get('startSizeY'))
        sx = mmc_mean(main.get('startSize'))
        if sx > 1e-6 and abs(sy / sx - 1) > 1e-3:
            em['aspect'] = clampv('aspect', r(sy / sx, 3), log)
        log.approx('main', '3D start size -> width and aspect (z dropped)')
    srot = mmc_range(main.get('startRotation'))
    if srot != (0.0, 0.0):
        em['rotation'] = clampv('rotation', rng(-srot[1] * RAD, -srot[0] * RAD, 1), log)
    if int(num(main.get('rotation3D'), 0)):
        log.approx('main', '3D start rotation -> z axis only')
    log.ok('main', 'duration, looping, lifetime, speed, size, rotation')
    # start colour
    sc = main.get('startColor')
    st = mmg_state(sc)
    if st == 0:
        c = uy.color(get(sc, 'maxColor'))
        if c != (1.0, 1.0, 1.0, 1.0):
            em['startColor'] = hex_color(c)
    elif st == 2:
        em['startColor'] = [hex_color(uy.color(get(sc, 'minColor'))), hex_color(uy.color(get(sc, 'maxColor')))]
    else:
        keys = mmg_keys(sc) if st in (1, 4) else [(t, tuple((a + b) / 2 for a, b in zip(uy.gradient_eval(get(sc, 'minGradient'), t), uy.gradient_eval(get(sc, 'maxGradient'), t)))) for t in (0, 0.25, 0.5, 0.75, 1)]
        em['startColor'] = color_keys_json(keys)
        if st == 1:
            log.approx('main', 'start colour gradient over the system duration -> random colour from the gradient')
        if st == 3:
            log.approx('main', 'two start gradients -> their average, random')
    g = mmc_mean(main.get('gravityModifier'))
    if mmc_is_curve(main.get('gravityModifier')):
        log.approx('main', 'gravity modifier curve -> its average')
    gravity = [0.0, -g * GRAVITY * S * sim_speed * sim_speed, 0.0]
    maxp = int(num(main.get('maxNumParticles'), 1000))
    if maxp < 1000:
        em['maxParticles'] = clampv('maxParticles', maxp, log)
    space = int(num(d.get('moveWithTransform'), 0))
    if space == 0:
        em['space'] = 'local'
    elif space == 2:
        log.approx('main', 'custom simulation space -> world')

    # ---- emission
    emi = d.get('EmissionModule') or {}
    rate = per_m = 0.0
    bursts = []
    if int(num(emi.get('enabled'), 1)):
        rate = mmc_mean(emi.get('rateOverTime')) * sim_speed
        per_m = mmc_mean(emi.get('rateOverDistance')) / max(1e-6, S)
        if mmc_is_curve(emi.get('rateOverTime')):
            log.approx('emission', 'rate over time curve -> its average')
        for b in emi.get('m_Bursts') or []:
            if not isinstance(b, dict):
                continue
            cnt = mmc_range(b.get('countCurve')) if 'countCurve' in b else (num(b.get('minCount'), 0), num(b.get('maxCount'), 0))
            prob = num(b.get('probability'), 1.0)
            if prob < 1:
                log.approx('emission', 'burst probability %g -> fewer particles' % prob)
            cycles = int(num(b.get('cycleCount'), 1))
            interval = num(b.get('repeatInterval'), 0.01) / sim_speed
            t = num(b.get('time'), 0) / sim_speed
            if cycles <= 0:  # infinite
                cycles = max(1, int((duration - t) / max(0.01, interval)) + 1) if looping else 1000
                log.approx('emission', 'infinite burst cycles -> %d per loop' % cycles)
            bursts.append((t, cnt[0] * prob, cnt[1] * prob, min(1000, cycles), interval))
        log.ok('emission', 'rate over time/distance, %d burst(s)' % len(bursts))
    else:
        log.ok('emission', 'disabled (emits nothing on its own)')
    if rate > 0:
        em['rate'] = clampv('rate', r(rate, 2), log)
    if per_m > 0:
        em['perMeter'] = clampv('perMeter', r(per_m, 2), log)
    if len(bursts) == 1 and not looping and bursts[0][0] == 0 and bursts[0][3] == 1 and bursts[0][1] == bursts[0][2]:
        em['burst'] = clampv('burst', int(round(bursts[0][1])), log)
    elif bursts:
        out = []
        for t, lo, hi, cyc, iv in bursts:
            if cyc == 1 and lo == hi:
                out.append([r(t), r(lo, 2)])
            else:
                o = {'time': r(t), 'count': rng(lo, hi, 2)}
                if cyc != 1:
                    o['cycles'] = cyc
                    o['interval'] = r(iv)
                out.append(o)
        em['bursts'] = out
    if delay > 0:
        em['delay'] = clampv('delay', r(delay), log)
    if looping and (rate > 0 or per_m > 0 or bursts):
        em['loop'] = True
        em['duration'] = clampv('duration', r(duration), log)
    elif rate > 0 or bursts:
        em['duration'] = clampv('duration', r(duration), log)
    emits = rate > 0 or per_m > 0 or bursts

    # ---- shape
    sh = d.get('ShapeModule') or {}
    axis = (0.0, 0.0, 1.0)
    shape_rot = (0.0, 0.0, 0.0, 1.0)
    if int(num(sh.get('enabled'), 1)):
        stype = int(num(sh.get('type'), 4))
        shape_rot = q_euler(uy.vec3(sh.get('m_Rotation')))
        sscale = uy.vec3(sh.get('m_Scale'), (1, 1, 1))
        spos = uy.vec3(sh.get('m_Position'))
        radius = num(get(sh, 'radius', 'value'), num(sh.get('radius'), 1.0)) * shape_scale * (sum(abs(x) for x in sscale) / 3)
        arc = num(get(sh, 'arc', 'value'), 360)
        thick = num(sh.get('radiusThickness'), 1.0)
        angle = num(sh.get('angle'), 25.0)
        rnd = num(sh.get('randomDirectionAmount'), 0)
        sph = num(sh.get('sphericalDirectionAmount'), 0)
        name = SHAPES.get(stype, 'type %d' % stype)
        if v_len(spos) > 1e-6:
            wpos = tuple(a + b for a, b in zip(wpos, q_rot(wrot, v_mul(wscale, spos))))
        if stype in (0, 1, 2, 3):
            em['shape'] = 'sphere' if stype < 2 else 'hemisphere'
            em['radius'] = clampv('radius', r(radius, 3), log)
            em['radial'] = clampv('radial', r(1 - rnd, 3), log)
            em['spread'] = 180
            if stype in (1, 3) or thick < 1:
                em['thickness'] = 0 if stype in (1, 3) else clampv('thickness', r(thick, 3), log)
            if arc < 360:
                em['arc'] = r(arc, 1)
            log.ok('shape', '%s r=%g' % (name, radius))
        elif stype in (4, 7, 8, 9):
            em['shape'] = 'cone'
            em['radius'] = clampv('radius', r(radius, 3), log)
            em['spread'] = clampv('spread', r(angle, 2), log)
            if stype in (7, 9) or thick < 1:
                em['thickness'] = 0 if stype in (7, 9) else clampv('thickness', r(thick, 3), log)
            if arc < 360:
                em['arc'] = r(arc, 1)
            if stype in (8, 9):
                log.approx('shape', 'cone volume -> emission from the base')
            if rnd > 0:
                em['radial'] = 0
                log.approx('shape', 'random direction amount %g ignored on cones' % rnd)
            log.ok('shape', '%s angle=%g r=%g' % (name, angle, radius))
        elif stype in (10, 11, 17):
            em['shape'] = 'cone'
            em['radius'] = clampv('radius', r(radius, 3), log)
            em['spread'] = 90
            em['radial'] = clampv('radial', r(1 - rnd, 3), log)
            if stype in (11, 17) or thick < 1:
                em['thickness'] = 0 if stype in (11, 17) else clampv('thickness', r(thick, 3), log)
            if arc < 360:
                em['arc'] = r(arc, 1)
            if stype == 17:
                log.approx('shape', 'donut -> its centre circle (donut radius dropped)')
            log.ok('shape', '%s r=%g' % (name, radius))
        elif stype in (5, 15, 16, 18):
            size_u = v_mul(sscale, (shape_scale, shape_scale, shape_scale))
            if stype == 18:
                size_u = (size_u[0], size_u[1], 0.0)
            world_ext = [0.0, 0.0, 0.0]
            for i, ax in enumerate(((1, 0, 0), (0, 1, 0), (0, 0, 1))):
                v = q_rot(q_mul(rot, shape_rot), tuple(a * size_u[i] for a in ax))
                for j in range(3):
                    world_ext[j] += abs(v[j])
            rel = u2rel(world_ext)
            em['shape'] = 'box'
            em['box'] = [r(abs(x), 3) for x in rel]
            em['spread'] = clampv('spread', r(180 * rnd, 1), log)
            if stype in (15, 16):
                log.approx('shape', '%s -> solid box' % name)
            log.ok('shape', '%s %s' % (name, em['box']))
        elif stype == 12:
            em['shape'] = 'line'
            em['radius'] = clampv('radius', r(radius, 3), log)
            em['spread'] = clampv('spread', r(180 * rnd, 1), log)
            axis = (0.0, 1.0, 0.0)
            log.approx('shape', 'edge -> line along the effect X axis')
        else:
            em['shape'] = 'sphere'
            em['radius'] = clampv('radius', r(max(0.1, radius), 3), log)
            em['radial'] = 1
            em['spread'] = 180
            log.approx('shape', '%s -> sphere of the shape radius' % name)
        if sph > 0 and 'radial' in em:
            em['radial'] = clampv('radial', r(max(em['radial'], sph), 3), log)
        if num(sh.get('randomPositionAmount'), 0) > 0:
            log.drop('shape', 'random position amount')
        if int(num(sh.get('alignToDirection'), 0)):
            log.drop('shape', 'align to direction')
    else:
        em['spread'] = 0
        log.ok('shape', 'disabled: point, straight along +Z')
    wdir = q_rot(q_mul(rot, shape_rot), axis)
    rdir = u2rel(wdir)
    if reverse:
        rdir = tuple(-x for x in rdir)
    rdir = v_norm(rdir)
    if v_len(tuple(a - b for a, b in zip(rdir, (0, 1, 0)))) > 1e-3:
        em['direction'] = [r(x, 3) for x in rdir]
    if not as_sub:
        off = u2rel(q_rot(ctx.root_rot_inv, tuple(a - b for a, b in zip(wpos, root_pos))))
        if v_len(off) > 1e-4:
            em['offset'] = [r(x * S, 3) for x in off]

    # ---- velocity / force / limit / inherit / external
    vel = d.get('VelocityModule') or {}
    if int(num(vel.get('enabled'), 0)):
        v = (mmc_mean(vel.get('x')), mmc_mean(vel.get('y')), mmc_mean(vel.get('z')))
        if any(mmc_is_curve(vel.get(k)) for k in ('x', 'y', 'z')):
            log.approx('velocityOverLifetime', 'curves -> their average')
        world = int(num(vel.get('inWorldSpace'), 0))
        vv = u2rel(v if world else q_rot(rot, v))
        if v_len(vv) > 1e-6:
            em['velocity'] = [r(x * S * sim_speed, 3) for x in vv]
        if any(abs(mmc_mean(vel.get(k))) > 1e-6 for k in ('orbitalX', 'orbitalY', 'orbitalZ', 'radial')):
            log.drop('velocityOverLifetime', 'orbital / radial velocity')
        sm = mmc_mean(vel.get('speedModifier')) if 'speedModifier' in vel else 1.0
        if abs(sm - 1) > 1e-3:
            em['speed'] = clampv('speed', rng(*[x * sm for x in (em['speed'] if isinstance(em['speed'], list) else [em['speed']] * 2)]), log)
            log.approx('velocityOverLifetime', 'speed modifier %g folded into the start speed' % sm)
        log.ok('velocityOverLifetime', 'linear velocity')
    force = d.get('ForceModule') or {}
    if int(num(force.get('enabled'), 0)):
        f = (mmc_mean(force.get('x')), mmc_mean(force.get('y')), mmc_mean(force.get('z')))
        fw = u2world(f if int(num(force.get('inWorldSpace'), 0)) else q_rot(q_mul(ctx.root_rot_inv, wrot), f))
        gravity = [a + b * S * sim_speed * sim_speed for a, b in zip(gravity, fw)]
        if int(num(force.get('randomizePerFrame'), 0)) or any(mmc_state(force.get(k)) in (2, 3) for k in ('x', 'y', 'z')):
            log.approx('forceOverLifetime', 'random force -> its average')
        log.ok('forceOverLifetime', 'added to gravity')
    if any(abs(x) > 1e-6 for x in gravity):
        em['gravity'] = [r(x, 3) for x in gravity]
    clamp = d.get('ClampVelocityModule') or {}
    if int(num(clamp.get('enabled'), 0)):
        if int(num(clamp.get('separateAxis'), 0)):
            log.approx('limitVelocity', 'per-axis limits -> one speed limit')
            lim = max(mmc_mean(clamp.get('x')), mmc_mean(clamp.get('y')), mmc_mean(clamp.get('z')))
        else:
            lim = mmc_mean(clamp.get('magnitude'))
        damp = num(clamp.get('dampen'), 1)
        if lim > 0 and lim < 1e5:
            em['speedLimit'] = clampv('speedLimit', r(lim * S, 3), log)
            if damp < 0.999:
                log.approx('limitVelocity', 'dampen %g -> hard limit' % damp)
        drag = mmc_mean(clamp.get('drag')) if 'drag' in clamp else 0
        if drag > 0:
            em['drag'] = clampv('drag', r(drag, 3), log)
            if int(num(clamp.get('multiplyDragByParticleSize'), 1)) or int(num(clamp.get('multiplyDragByParticleVelocity'), 1)):
                log.approx('limitVelocity', 'drag by size/velocity -> constant drag')
        log.ok('limitVelocity', 'speed limit, drag')
    for mod, label in (('InheritVelocityModule', 'inheritVelocity'), ('ExternalForcesModule', 'externalForces'),
                       ('TriggerModule', 'trigger'), ('CustomDataModule', 'customData'), ('RotationBySpeedModule', 'rotationBySpeed'),
                       ('LifetimeByEmitterSpeedModule', 'lifetimeByEmitterSpeed')):
        if int(num(get(d, mod, 'enabled'), 0)):
            log.drop(label, 'module has no equivalent')

    # ---- colour / size / rotation over life and by speed
    mat_slot = 'renderer'
    rend = psr.data if psr is not None else {}
    mats = rend.get('m_Materials') or []
    mat = read_material(mats[0] if mats else None, ctx.assets, log, mat_slot)
    tint = mat.tint
    col = d.get('ColorModule') or {}
    if int(num(col.get('enabled'), 0)):
        keys = mmg_keys(col.get('gradient'), tint)
        if mmg_state(col.get('gradient')) in (2, 3):
            log.approx('colorOverLifetime', 'random between two -> their average')
        em['color'] = color_keys_json(keys)
        log.ok('colorOverLifetime', '%d keys (colour + alpha)' % len(em['color']))
    else:
        c = hex_color(tuple(min(1.0, x) for x in tint))
        em['color'] = [[0, c], [1, c]]
    size_m = d.get('SizeModule') or {}
    if int(num(size_m.get('enabled'), 0)):
        em['sizeOverLife'] = mmc_keys(size_m.get('curve'))
        if int(num(size_m.get('separateAxes'), 0)):
            log.approx('sizeOverLifetime', 'separate axes -> x curve only')
        if mmc_state(size_m.get('curve')) in (2, 3):
            log.approx('sizeOverLifetime', 'random between two -> their middle')
        log.ok('sizeOverLifetime', '%d keys' % len(em['sizeOverLife']))
    else:
        em['sizeOverLife'] = [[0, 1], [1, 1]]
    rot_m = d.get('RotationModule') or {}
    if int(num(rot_m.get('enabled'), 0)):
        lo, hi = mmc_range(rot_m.get('curve'))
        if num(main.get('randomizeRotationDirection'), 0) > 0:
            m = max(abs(lo), abs(hi))
            lo, hi = -m, m
        em['spin'] = clampv('spin', rng(-hi * RAD, -lo * RAD, 1), log)
        if mmc_is_curve(rot_m.get('curve')):
            log.approx('rotationOverLifetime', 'angular velocity curve -> random constant spin in its range')
        if int(num(rot_m.get('separateAxes'), 0)):
            log.approx('rotationOverLifetime', 'separate axes -> z only')
        log.ok('rotationOverLifetime', 'spin')
    elif num(main.get('randomizeRotationDirection'), 0) > 0 and 'rotation' in em:
        rr = em['rotation'] if isinstance(em['rotation'], list) else [em['rotation'], em['rotation']]
        m = max(abs(rr[0]), abs(rr[1]))
        em['rotation'] = [-m, m]
    cbs = d.get('ColorBySpeedModule') or {}
    if int(num(cbs.get('enabled'), 0)):
        rg = uy.vec3(cbs.get('range'), (0, 1, 0))
        em['colorBySpeed'] = {'speed': rng(rg[0] * S, rg[1] * S), 'color': color_keys_json(mmg_keys(cbs.get('gradient')))}
        log.ok('colorBySpeed', 'gradient over speed')
    sbs = d.get('SizeBySpeedModule') or {}
    if int(num(sbs.get('enabled'), 0)):
        rg = uy.vec3(sbs.get('range'), (0, 1, 0))
        em['sizeBySpeed'] = {'speed': rng(rg[0] * S, rg[1] * S), 'size': mmc_keys(sbs.get('curve'))}
        log.ok('sizeBySpeed', 'curve over speed')

    # ---- noise
    nz = d.get('NoiseModule') or {}
    if int(num(nz.get('enabled'), 0)):
        if int(num(nz.get('separateAxes'), 0)):
            strength = (mmc_mean(nz.get('strength')) + mmc_mean(nz.get('strengthY')) + mmc_mean(nz.get('strengthZ'))) / 3
            log.approx('noise', 'separate axes -> average strength')
        else:
            strength = mmc_mean(nz.get('strength'))
        freq = num(nz.get('frequency'), 0.5)
        em['noise'] = {'strength': r(min(100, max(0, strength * S)), 3), 'frequency': r(min(100, max(0.001, freq / max(1e-6, S))), 4)}
        scroll = mmc_mean(nz.get('scrollSpeed'))
        if abs(scroll) > 1e-6:
            em['noise']['scroll'] = r(max(-100, min(100, scroll)), 3)
        if int(num(nz.get('octaves'), 1)) > 1:
            log.approx('noise', '%d octaves -> one' % int(num(nz.get('octaves'), 1)))
        if any(mmc_mean(nz.get(k)) > 1e-6 for k in ('rotationAmount', 'sizeAmount')):
            log.drop('noise', 'rotation/size amount')
        log.ok('noise', 'strength, frequency, scroll')

    # ---- texture sheet
    uv = d.get('UVModule') or {}
    tex_frames = None
    if int(num(uv.get('enabled'), 0)):
        if int(num(uv.get('mode'), 0)) == 1:
            log.drop('textureSheetAnimation', 'sprite mode')
        else:
            tx, ty = int(num(uv.get('tilesX'), 1)), int(num(uv.get('tilesY'), 1))
            if int(num(uv.get('animationType'), 0)) == 1:
                log.approx('textureSheetAnimation', 'single row animation -> whole sheet')
            if tx * ty > 256:
                log.approx('textureSheetAnimation', '%dx%d tiles > 256 frames: animation dropped' % (tx, ty))
            elif tx * ty > 1:
                tex_frames = [tx, ty]
                em['frames'] = tex_frames
                tmode = int(num(uv.get('timeMode'), 0))
                cycles = num(uv.get('cycles'), 1)
                if tmode == 2:
                    em['fps'] = clampv('fps', r(num(uv.get('fps'), 30), 2), log)
                else:
                    if tmode == 1:
                        log.approx('textureSheetAnimation', 'frames by speed -> over the lifetime')
                    fot = uv.get('frameOverTime')
                    k = mmc_keys(fot) if isinstance(fot, dict) else [[0, 0], [1, 1]]
                    linear = len(k) == 2 and abs(k[0][1]) < 0.02 and abs(k[1][1] - 1) < 0.02
                    if not linear:
                        log.approx('textureSheetAnimation', 'frame over time curve -> linear over the lifetime')
                    if abs(cycles - 1) > 1e-3:
                        em['cycles'] = clampv('cycles', r(cycles, 3), log)
                sf = mmc_range(uv.get('startFrame'))
                n = tx * ty
                if sf[1] > 0:
                    lo, hi = sf
                    if hi <= 1.0 and n > 1 and not float(hi).is_integer():  # older Unity: fraction of the sheet
                        lo, hi = lo * n, hi * n
                    em['startFrame'] = rng(min(n - 1, lo), min(n - 1, hi), 1)
                if int(num(uv.get('flipU'), 0)) or int(num(uv.get('flipV'), 0)):
                    log.drop('textureSheetAnimation', 'random flip U/V')
                log.ok('textureSheetAnimation', '%dx%d tiles' % (tx, ty))

    # ---- renderer
    visible = True
    if psr is None:
        log.drop('renderer', 'no ParticleSystemRenderer: particles invisible')
        visible = False
    else:
        if not int(num(rend.get('m_Enabled'), 1)):
            visible = False
        mode_r = int(num(rend.get('m_RenderMode'), 0))
        if mode_r == 1:
            em['align'] = 'velocity'
            vs = num(rend.get('m_VelocityScale'), 0)
            ls = num(rend.get('m_LengthScale'), 2)
            if vs > 0:
                em['stretch'] = clampv('stretch', r(vs, 4), log)
            if abs(ls - 1) > 1e-3:
                em['aspect'] = clampv('aspect', r(ls * (em.get('aspect', 1) if not isinstance(em.get('aspect', 1), list) else 1), 3), log)
            if num(rend.get('m_CameraVelocityScale'), 0) > 0:
                log.drop('renderer', 'camera velocity scale')
            log.ok('renderer', 'stretched billboard')
        elif mode_r == 2:
            em['align'] = 'ground'
            log.ok('renderer', 'horizontal billboard')
        elif mode_r == 3:
            em['align'] = 'upright'
            log.ok('renderer', 'vertical billboard')
        elif mode_r == 4:
            log.approx('renderer', 'mesh particles -> camera billboards with the material texture')
        elif mode_r == 5:
            visible = False
        else:
            log.ok('renderer', 'billboard')
        if int(num(rend.get('m_RenderAlignment'), 0)) in (1, 2, 3):
            log.approx('renderer', 'render alignment world/local/direction -> camera facing')
        if num(get(rend, 'm_Pivot', 'x'), 0) or num(get(rend, 'm_Pivot', 'y'), 0):
            log.drop('renderer', 'pivot offset')
        if int(num(rend.get('m_SortMode'), 0)) not in (0, 1):
            log.approx('renderer', 'sort mode -> back to front (alpha) / none (additive)')
    em['texture'] = ctx.tex.get(mat)
    em['blend'] = mat.blend
    if not visible:
        em['hidden'] = True
        log.ok('renderer', 'renderer off: particles drive trails / sub-emitters / lights only')

    # ---- collision
    colm = d.get('CollisionModule') or {}
    if int(num(colm.get('enabled'), 0)):
        c = {'bounce': r(min(2, max(0, mmc_mean(colm.get('m_Bounce')))), 3), 'dampen': r(min(1, max(0, mmc_mean(colm.get('m_Dampen')))), 3)}
        ll = mmc_mean(colm.get('m_EnergyLossOnCollision'))
        if ll > 0:
            c['lifeLoss'] = r(min(1, ll), 3)
        mk = num(colm.get('minKillSpeed'), 0)
        if mk > 0:
            c['minKillSpeed'] = r(mk * S, 3)
        if int(num(colm.get('type'), 0)) == 0:
            plane = get(colm, 'plane0', 'fileID') or next((get(colm, 'm_Planes', i, 'fileID') for i in range(1) if get(colm, 'm_Planes', i, 'fileID')), None)
            tr = prefab.transforms.get(plane) if plane else None
            if tr is None and plane:
                tr = next((t for t in prefab.transforms.values() if get(t.data, 'm_GameObject', 'fileID') == plane), None)
            if tr is not None:
                py = next((p for (t, _, p, _, _, _) in prefab.walk() if t is tr), (0, 0, 0))[1]
                c['height'] = r((py - root_pos[1]) * S, 3)
            log.approx('collision', 'planes -> the ground (one horizontal plane)')
        else:
            log.approx('collision', 'world collision -> the ground only')
        em['collision'] = c
        if int(num(colm.get('collisionMode'), 0)) == 1:
            log.approx('collision', '2D collision -> ground plane')

    # ---- trails
    tr_m = d.get('TrailModule') or {}
    if int(num(tr_m.get('enabled'), 0)):
        if int(num(tr_m.get('mode'), 0)) == 1:
            log.drop('trails', 'ribbon mode (particles connected to each other)')
        else:
            mean_life = (life[0] + life[1]) * 0.5
            ratio = mmc_mean(tr_m.get('lifetime')) if 'lifetime' in tr_m else 1.0
            t = {'time': r(min(10, max(0.01, ratio * mean_life)), 3)}
            t['width'] = mmc_keys(tr_m.get('widthOverTrail')) if isinstance(tr_m.get('widthOverTrail'), dict) else [[0, 1], [1, 1]]
            tmat = read_material(mats[1] if len(mats) > 1 else (mats[0] if mats else None), ctx.assets, log, 'trails')
            ckeys = mmg_keys(tr_m.get('colorOverTrail'), tmat.tint) if isinstance(tr_m.get('colorOverTrail'), dict) else [(0, tmat.tint), (1, tmat.tint)]
            if isinstance(tr_m.get('colorOverLifetime'), dict) and mmg_state(tr_m.get('colorOverLifetime')) in (0, 2):
                cl = uy.mmg_eval(tr_m.get('colorOverLifetime'), 0)
                ckeys = tint_keys(ckeys, cl)
            elif isinstance(tr_m.get('colorOverLifetime'), dict):
                log.approx('trails', 'trail colour over lifetime gradient dropped (colour over trail kept)')
            t['color'] = color_keys_json(ckeys)
            t['texture'] = ctx.tex.get(tmat)
            t['blend'] = tmat.blend
            if not int(num(tr_m.get('inheritParticleColor'), 1)):
                t['inheritColor'] = False
            if not int(num(tr_m.get('sizeAffectsWidth'), 1)):
                t['sizeAffectsWidth'] = False
            if num(tr_m.get('ratio'), 1) < 0.999:
                log.approx('trails', 'ratio %g -> every particle has a trail' % num(tr_m.get('ratio'), 1))
            if not int(num(tr_m.get('dieWithParticles'), 1)):
                log.approx('trails', 'trails outliving particles -> die with them')
            if int(num(tr_m.get('textureMode'), 0)) != 0:
                log.approx('trails', 'texture mode -> stretch')
            em['trail'] = t
            log.ok('trails', 'ribbon behind each particle (minQuality medium)')

    # ---- lights
    lm = d.get('LightsModule') or {}
    if int(num(lm.get('enabled'), 0)):
        light = {'color': '#ffffff', 'radius': 3.0, 'intensity': 1.0}
        lref = get(lm, 'light', 'fileID')
        ld = prefab.by_id.get(lref) if lref else None
        if ld is not None and ld.class_id == CLASS_LIGHT:
            c = uy.color(ld.data.get('m_Color'))
            light['color'] = hex_color(c)[:7]
            light['radius'] = num(ld.data.get('m_Range'), 3)
            light['intensity'] = num(ld.data.get('m_Intensity'), 1)
        else:
            log.approx('lights', 'light template not in this prefab: white, radius 3')
        light['radius'] = r(min(100, max(0.01, light['radius'] * S * mmc_mean(lm.get('rangeCurve')) if 'rangeCurve' in lm else light['radius'] * S)), 3)
        light['intensity'] = r(min(20, max(0, light['intensity'] * (mmc_mean(lm.get('intensityCurve')) if 'intensityCurve' in lm else 1))), 3)
        ratio = num(lm.get('ratio'), 0)
        if ratio < 1:
            light['ratio'] = r(ratio, 3)
        light['max'] = int(min(64, max(0, num(lm.get('maxLights'), 20))))
        if not int(num(lm.get('color'), 1)):
            light['useParticleColor'] = False
        if int(num(lm.get('range'), 1)):
            light['sizeAffectsRadius'] = True
        if not int(num(lm.get('intensity'), 1)):
            light['alphaAffectsIntensity'] = False
        em['light'] = light
        log.ok('lights', 'engine point lights (light map)')

    # ---- sub-emitters
    subs = []
    sm = d.get('SubModule') or {}
    if int(num(sm.get('enabled'), 0)):
        for s in sm.get('subEmitters') or []:
            if not isinstance(s, dict):
                continue
            target = get(s, 'emitter', 'fileID')
            typ = int(num(s.get('type'), 0))
            if typ not in (0, 1, 2):
                log.drop('subEmitters', '%s sub-emitter' % {3: 'trigger', 4: 'manual'}.get(typ, 'type %d' % typ))
                continue
            props = int(num(s.get('properties'), 0))
            subs.append({'target': target, 'on': {0: 'birth', 1: 'collision', 2: 'death'}[typ], 'chance': num(s.get('emitProbability'), 1),
                         'inheritColor': bool(props & 1)})
            if props & ~1:
                log.approx('subEmitters', 'inherit size/rotation/lifetime/duration dropped (colour kept)')
        if subs:
            log.ok('subEmitters', '%d (birth/death/collision -> child effects)' % len(subs))
    ctx.rep.systems.append({'system': path, 'modules': log.modules})
    if not emits:
        log.approx('emission', 'system emits nothing (no rate, distance or bursts): skipped')
        return None, []
    return em, subs


def mmc_state(m):
    return uy.mmc_state(m) if isinstance(m, dict) else 0


# ------------------------------------------------------------------------------------------ effect

ORDER = ['comment', 'texture', 'frames', 'fps', 'cycles', 'startFrame', 'blend', 'align', 'hidden', 'burst', 'bursts', 'rate', 'perMeter',
         'delay', 'duration', 'loop', 'life', 'speed', 'size', 'aspect', 'rotation', 'spin', 'startColor', 'direction', 'spread', 'shape',
         'radius', 'arc', 'thickness', 'radial', 'box', 'offset', 'space', 'gravity', 'drag', 'velocity', 'speedLimit', 'noise', 'stretch',
         'sizeOverLife', 'color', 'colorBySpeed', 'sizeBySpeed', 'collision', 'trail', 'light', 'subEmitters', 'maxParticles', 'minQuality']


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


def import_prefab(path, assets, opts, rep, name, game_dir):
    prefab = Prefab(path, assets, rep)
    rel_name = os.path.basename(path)
    for d in prefab.docs:
        if d.class_id == CLASS_VISUAL_EFFECT:
            rep.dropped.append('%s: VisualEffect (VFX Graph) component: VFX Graph effects are GPU node graphs, not ParticleSystems; '
                               'they cannot be converted. Rebuild the look from fx_templates or describe it and edit with fx_set.' % rel_name)
        elif d.class_id == CLASS_MONO:
            script = assets.path(get(d.data, 'm_Script') or {})
            rep.dropped.append('%s: script %s (behaviour is code: bind the effect to game events with fx_bind instead)' %
                               (rel_name, os.path.basename(script) if script else get(d.data, 'm_Script', 'guid', default='?')))
        elif d.class_id in OTHER_COMPONENTS and not d.stripped:
            go = get(d.data, 'm_GameObject', 'fileID')
            rep.dropped.append('%s: %s on %s' % (rel_name, OTHER_COMPONENTS[d.class_id], prefab.name(go)))
        elif d.class_id == CLASS_LIGHT:
            used = any(get(p.data, 'LightsModule', 'light', 'fileID') == d.file_id for p in prefab.docs if p.class_id == CLASS_PS)
            if not used:
                rep.dropped.append('%s: Light component on %s (not a particle light): add a map light or a "light" module' %
                                   (rel_name, prefab.name(get(d.data, 'm_GameObject', 'fileID'))))
    for n in prefab.nested:
        src = assets.path(get(n.data, 'm_SourcePrefab') or {})
        rep.dropped.append('%s: nested prefab %s: import it on its own and play both (overrides are not applied)' %
                           (rel_name, os.path.basename(src) if src else get(n.data, 'm_SourcePrefab', 'guid', default='?')))
    walk = prefab.walk()
    if not walk:
        rep.warnings.append('%s: no GameObject hierarchy' % rel_name)
        return []
    root_tr, _, root_pos, root_rot, _, _ = walk[0]
    root_rot_inv = q_inv(root_rot) if opts.ignore_root_rotation else (0.0, 0.0, 0.0, 1.0)
    ctx = Context(opts, rep, assets, Textures(game_dir, assets, rep, opts.dry_run), root_rot_inv)
    ctx.tex = opts.textures if getattr(opts, 'textures', None) else ctx.tex
    opts.textures = ctx.tex
    systems = []  # (ps doc, psr doc, go, pos, rot, scale, path)
    for tr, go, pos, rot, scale, p in walk:
        comps = prefab.comp_by_go.get(go, [])
        ps = next((c for c in comps if c.class_id == CLASS_PS), None)
        if ps is None:
            continue
        psr = next((c for c in comps if c.class_id == CLASS_PSR), None)
        systems.append((ps, psr, go, pos, rot, scale, p))
    if not systems:
        if not any(d.class_id == CLASS_VISUAL_EFFECT for d in prefab.docs):
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
            em, subs = map_system(ctx, prefab, ps, psr, go, pos, rot, scale, p, root_pos, as_sub)
            if em is None:
                continue
            se = []
            for s in subs:
                if s['target'] not in by_ps:
                    rep.dropped.append('%s: sub-emitter outside this prefab' % p)
                    continue
                o = {'on': s['on'], 'effect': sub_name(s['target'])}
                if s['chance'] < 1:
                    o['chance'] = r(s['chance'], 3)
                if s['inheritColor']:
                    o['inheritColor'] = True
                se.append(o)
            if se:
                em['subEmitters'] = se[:4]
                if len(se) > 4:
                    rep.dropped.append('%s: more than 4 sub-emitters' % p)
            emitters.append(ordered(em))
        if len(emitters) > 16:
            rep.dropped.append('%s: %d systems, the engine keeps 16 per effect (the rest dropped)' % (effect_name, len(emitters)))
            emitters = emitters[:16]
        if not emitters:
            return
        effect = {'comment': 'Imported from Unity %s by tools/importers/unity_vfx (report: what was approximated or dropped).' % rel_name,
                  'emitters': emitters}
        outputs.append((effect_name, effect))

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
    opts.used_names, opts.textures = set(), None
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
    with_ps = []
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
    if not with_ps and not vfx:
        rep.warnings.append('error: no prefab with a ParticleSystem found in %s' % opts.source)
    total = 0
    for p in with_ps:
        stem = os.path.splitext(os.path.basename(p))[0]
        name = sanitize(opts.name if opts.name and len(with_ps) == 1 else ('%s_%s' % (opts.name, stem) if opts.name else stem), 40)
        try:
            outs = import_prefab(p, assets, opts, rep, name, game_dir)
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

"""Unity materials and textures for the effect importer: blend mode, main texture, tint (materials,
custom shaders, Shader Graph), texture conversion into the game's fx/textures folder."""
import os
import re

import images
import unity_yaml as uy
from unity_yaml import get, num

CLASS_MATERIAL = 21
BUILTIN_GUID = '0000000000000000f000000000000000'
TEX_EXT = ('.png', '.tga', '.psd', '.jpg', '.jpeg', '.tif', '.tiff', '.exr', '.hdr', '.bmp', '.gif', '.psb', '.iff', '.pict')


def sanitize(name, limit=40):
    s = re.sub(r'[^a-z0-9_]+', '_', name.lower()).strip('_')
    return (s or 'fx')[:limit].strip('_') or 'fx'


# Built-in shader file ids (Unity's "Resources/unity_builtin_extra"); only used when the material has no
# blend floats. Values seen in serialized materials; anything unknown falls back to name heuristics.
BUILTIN_SHADERS = {
    200: ('Legacy Shaders/Particles/Additive', 'additive'), 201: ('Legacy Shaders/Particles/Additive (Soft)', 'additive'),
    202: ('Legacy Shaders/Particles/Alpha Blended', 'alpha'), 203: ('Legacy Shaders/Particles/Alpha Blended Premultiply', 'premultiplied'),
    204: ('Legacy Shaders/Particles/Multiply', 'alpha'), 205: ('Legacy Shaders/Particles/Multiply (Double)', 'alpha'),
    206: ('Legacy Shaders/Particles/VertexLit Blended', 'alpha'), 207: ('Legacy Shaders/Particles/Anim Alpha Blended', 'alpha'),
    208: ('Legacy Shaders/Particles/~Additive-Multiply', 'premultiplied'), 210: ('Particles/Standard Surface', 'alpha'),
    211: ('Particles/Standard Unlit', 'alpha'), 10720: ('Mobile/Particles/Additive', 'additive'),
    10721: ('Mobile/Particles/Alpha Blended', 'alpha'), 10722: ('Mobile/Particles/Multiply', 'alpha'),
    10723: ('Mobile/Particles/VertexLit Blended', 'alpha'),
}

# ------------------------------------------------------------------------------------------ materials

class MaterialInfo:
    def __init__(self):
        self.blend, self.texture_guid, self.texture_path, self.tint = 'additive', None, None, (1, 1, 1, 1)
        self.name, self.shader, self.builtin_texture = '?', '?', False


def _props(mat, section):
    out = {}
    for item in get(mat, 'm_SavedProperties', section, default=[]) or []:
        if isinstance(item, dict):
            out.update(item)
    return out


def shader_blend_from_source(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as f:
            src = f.read()
    except OSError:
        return None
    if path.endswith('.shadergraph'):
        st = re.search(r'"m_SurfaceType"\s*:\s*(\d+)', src)
        am = re.search(r'"m_AlphaMode"\s*:\s*(\d+)', src)
        if st and st.group(1) == '0':
            return 'alpha'
        mode = am.group(1) if am else '0'
        return {'0': 'alpha', '1': 'premultiplied', '2': 'additive', '3': 'alpha'}.get(mode, 'alpha')
    m = re.search(r'\bBlend\s+(\w+)\s+(\w+)', src)
    if not m:
        return None
    a, b = m.group(1).lower(), m.group(2).lower()
    if b == 'one':
        return 'additive'
    if a == 'one' and b == 'oneminussrcalpha':
        return 'premultiplied'
    return 'alpha'


def blend_from_floats(f):
    if '_SrcBlend' in f and '_DstBlend' in f:
        s, d = int(num(f['_SrcBlend'])), int(num(f['_DstBlend']))
        if d == 1:
            return 'additive', None
        if s == 1 and d == 10:
            return 'premultiplied', None
        if s == 5 and d == 10:
            return 'alpha', None
        if (s, d) in ((2, 0), (0, 3), (2, 3)):
            return 'alpha', 'multiply blending'
        if d == 0 and s in (1, 5):
            return None, None  # opaque surface: decide from the mode / shader
    if '_Blend' in f and '_Surface' in f and int(num(f['_Surface'])) == 1:
        b = int(num(f['_Blend']))
        return {0: ('alpha', None), 1: ('premultiplied', None), 2: ('additive', None), 3: ('alpha', 'multiply blending')}.get(b, ('alpha', None))
    if '_Mode' in f:
        m = int(num(f['_Mode']))
        table = {0: ('alpha', 'opaque particles'), 1: ('alpha', 'cutout'), 2: ('alpha', None), 3: ('premultiplied', None),
                 4: ('additive', None), 5: ('additive', 'subtractive blending'), 6: ('alpha', 'modulate blending')}
        if m in table:
            return table[m]
    return None, None


def read_material(ref, assets, log, slot):
    info = MaterialInfo()
    if not isinstance(ref, dict) or not get(ref, 'fileID'):
        info.texture_path, info.builtin_texture, info.blend, info.name = None, True, 'alpha', '(none)'
        log.approx(slot, 'no material: built-in soft particle')
        return info
    if str(ref.get('guid', '')).lower() == BUILTIN_GUID:
        info.builtin_texture, info.blend, info.name = True, 'alpha', 'Default-Particle'
        log.ok(slot, 'built-in Default-Particle material -> builtin:soft, alpha')
        return info
    path = assets.path(ref)
    if not path:
        info.builtin_texture = True
        log.approx(slot, 'material %s not found in the source folder: builtin:soft, additive' % ref.get('guid'))
        return info
    docs = assets.docs(path)
    mat = next((d.data for d in docs if d.class_id == CLASS_MATERIAL), None)
    if mat is None:
        info.builtin_texture = True
        log.approx(slot, '%s is not a material' % os.path.basename(path))
        return info
    info.name = str(mat.get('m_Name', os.path.basename(path)))
    tex = _props(mat, 'm_TexEnvs')
    floats = _props(mat, 'm_Floats')
    colors = _props(mat, 'm_Colors')
    # blend mode
    blend, why = blend_from_floats(floats)
    shader_ref = mat.get('m_Shader') or {}
    shader_path = assets.path(shader_ref) if str(shader_ref.get('guid', '')).lower() != BUILTIN_GUID else None
    if shader_path:
        info.shader = os.path.basename(shader_path)
        if shader_path.endswith('.shadergraph'):
            log.approx(slot, 'Shader Graph %s: only the surface/blend mode, main texture and colour are kept' % info.shader)
        else:
            log.approx(slot, 'custom shader %s: only its Blend state, main texture and colour are kept' % info.shader)
        if blend is None:
            blend = shader_blend_from_source(shader_path)
    elif str(shader_ref.get('guid', '')).lower() == BUILTIN_GUID:
        sid = int(num(shader_ref.get('fileID'), 0))
        name, b = BUILTIN_SHADERS.get(sid, ('built-in shader %d' % sid, None))
        info.shader = name
        if blend is None and b:
            blend = b
            if 'Multiply' in name:
                why = 'multiply blending'
    elif shader_ref.get('guid'):
        info.shader = 'shader %s (not in the source folder)' % shader_ref.get('guid')
    if blend is None:
        hint = (info.shader + ' ' + info.name).lower()
        blend = 'additive' if 'add' in hint or 'glow' in hint else 'premultiplied' if 'premul' in hint else 'alpha' if ('alpha' in hint or 'blend' in hint or 'smoke' in hint) else 'additive'
        log.approx(slot, 'blend mode guessed from names (%s): %s' % (info.shader, blend))
    if why:
        log.approx(slot, '%s -> %s' % (why, blend))
    info.blend = blend
    # main texture
    for key in ('_MainTex', '_BaseMap', '_BaseColorMap', '_UnlitColorMap', '_MainTexture', '_Texture', '_Tex'):
        t = get(tex, key, 'm_Texture')
        if isinstance(t, dict) and get(t, 'fileID'):
            info.texture_guid = str(t.get('guid', '')).lower()
            sc, of = get(tex, key, 'm_Scale'), get(tex, key, 'm_Offset')
            if uy.vec3(sc, (1, 1, 1))[:2] != (1.0, 1.0) or uy.vec3(of)[:2] != (0.0, 0.0):
                log.drop(slot, 'texture tiling/offset on %s' % key)
            if info.texture_guid == BUILTIN_GUID:
                info.builtin_texture = True
            else:
                info.texture_path = assets.path(t)
                if not info.texture_path:
                    log.approx(slot, 'texture %s not found: builtin:soft' % info.texture_guid)
                    info.builtin_texture = True
            break
    else:
        info.builtin_texture = True
        log.approx(slot, 'material %s has no main texture: builtin:soft' % info.name)
    other = [k for k in tex if k not in ('_MainTex', '_BaseMap', '_BaseColorMap', '_UnlitColorMap') and isinstance(get(tex, k, 'm_Texture'), dict) and get(tex, k, 'm_Texture', 'fileID')]
    if other:
        log.drop(slot, 'extra material textures (%s): distortion/normal/mask maps have no equivalent' % ', '.join(sorted(other)))
    # tint: legacy particle shaders double _TintColor
    if '_TintColor' in colors:
        c = uy.color(colors['_TintColor'])
        info.tint = (c[0] * 2, c[1] * 2, c[2] * 2, c[3] * 2)
    elif '_BaseColor' in colors:
        info.tint = uy.color(colors['_BaseColor'])
    elif '_Color' in colors:
        info.tint = uy.color(colors['_Color'])
    if any(x > 1.001 for x in info.tint[:3]):
        log.approx(slot, 'material tint brighter than white (HDR/2x): clamped to 1')
    if '_EMISSION' in str(mat.get('m_ShaderKeywords', '')) or (num(floats.get('_EmissionEnabled'), 0) > 0):
        log.drop(slot, 'emission colour')
    if num(floats.get('_SoftParticlesEnabled'), 0) > 0 or '_SOFTPARTICLES_ON' in str(mat.get('m_ShaderKeywords', '')):
        log.drop(slot, 'soft particles (no depth reads on weak GPUs)')
    if num(floats.get('_DistortionEnabled'), 0) > 0:
        log.drop(slot, 'distortion')
    if num(floats.get('_FlipbookMode'), 0) > 0:
        log.approx(slot, 'flipbook frame blending: frames switch without blending')
    log.ok(slot, 'material %s: %s%s' % (info.name, info.blend, ', texture' if info.texture_path else ''))
    return info


# ------------------------------------------------------------------------------------------ textures

class Textures:
    def __init__(self, game_dir, assets, report, dry):
        self.game_dir, self.assets, self.rep, self.dry = game_dir, assets, report, dry
        self.done, self.used = {}, set()

    def get(self, info):
        if info.builtin_texture or not info.texture_path:
            return 'builtin:soft'
        src = info.texture_path
        if src in self.done:
            return self.done[src]
        stem = sanitize(os.path.splitext(os.path.basename(src))[0], 48)
        name = stem
        k = 2
        while name in self.used:
            name = '%s_%d' % (stem, k)
            k += 1
        self.used.add(name)
        rel = 'fx/textures/%s.png' % name
        entry = {'source': os.path.basename(src), 'file': rel, 'note': ''}
        img, note = images.load_any(src) if src.lower().endswith(TEX_EXT) else (None, 'not an image')
        if img is None:
            self.rep.warnings.append('texture %s could not be converted (%s): builtin:soft used instead' % (os.path.basename(src), note))
            entry['file'], entry['note'] = None, note
            self.rep.textures.append(entry)
            self.done[src] = 'builtin:soft'
            return 'builtin:soft'
        notes = [note] if note else []
        meta = self.assets.meta(src)
        imp = get(meta, 'TextureImporter', default={}) or {}
        if int(num(get(imp, 'alphaSource'), 1)) == 2 or int(num(get(imp, 'grayScaleToAlpha'), 0)) == 1:
            images.alpha_from_gray(img)
            notes.append('alpha from grayscale (import setting)')
        elif int(num(get(imp, 'alphaSource'), 1)) == 0:
            for i in range(3, len(img.rgba), 4):
                img.rgba[i] = 255
            notes.append('alpha ignored (import setting)')
        if info.blend == 'alpha' and not img.has_alpha():
            self.rep.warnings.append('texture %s has no alpha but is alpha blended: it draws as squares (as in Unity)' % os.path.basename(src))
        if max(img.w, img.h) > 1024:
            notes.append('%dx%d: large for weak GPUs, consider 512' % (img.w, img.h))
        entry['note'] = '; '.join(notes)
        entry['size'] = [img.w, img.h]
        if not self.dry:
            images.write_png(os.path.join(self.game_dir, rel), img)
        self.rep.textures.append(entry)
        self.done[src] = rel
        return rel

"""One Unity ParticleSystem -> one engine emitter, part 2: colour / size / rotation over the lifetime and by
speed, noise, texture sheet, renderer + material, collision, triggers, trails, lights, sub-emitters."""
import convert as cv
import unity_yaml as uy
from convert import Curve, RAD, rv, signed_perm
from mapping import LIMITS, clampv, color_keys_json, nonzero, val, vec
from materials import read_material, sprite_info
from unity_scene import q_rot, v_mul
from unity_yaml import get, hex_color, num

CLASS_LIGHT, CLASS_BOX_COLLIDER, CLASS_SPHERE_COLLIDER, CLASS_CAPSULE_COLLIDER = 108, 65, 135, 136


def tint_keys(keys, tint):
    return [(t, tuple(min(1.0, max(0.0, a * b)) for a, b in zip(c, tint))) for t, c in keys]


# ------------------------------------------------------------------------------------------ over lifetime

def map_over_life(s, em, tint):
    log, sim = s.log, s.sim
    col = s.module('ColorModule')
    if col is not None:
        hi, lo = cv.mmg_pair(col.get('gradient'), tint)
        em['color'] = color_keys_json(hi)
        if lo is not None:
            em['colorMin'] = color_keys_json(lo)
        log.ok('colorOverLifetime', '%d keys (colour + alpha)%s' % (len(em['color']), ', random between two' if lo else ''))
    else:
        c = hex_color(tuple(min(1.0, x) for x in tint))
        em['color'] = [[0, c], [1, c]]
    size_m = s.module('SizeModule')
    if size_m is not None:
        c = Curve(size_m.get('curve'))
        em['sizeOverLife'] = cv.keys_of(size_m.get('curve'), which='hi', lim=LIMITS['sizeKeys'])
        if c.random:
            em['sizeOverLifeMin'] = cv.keys_of(size_m.get('curve'), which='lo', lim=LIMITS['sizeKeys'])
        if int(num(size_m.get('separateAxes'), 0)):
            em['sizeOverLifeY'] = cv.keys_of(size_m.get('y'), lim=LIMITS['sizeKeys'])
            em['sizeOverLifeZ'] = cv.keys_of(size_m.get('z'), lim=LIMITS['sizeKeys'])
            if Curve(size_m.get('y')).random or Curve(size_m.get('z')).random:
                log.approx('sizeOverLifetime', 'random y / z curves -> their middle')
        log.ok('sizeOverLifetime', '%d keys' % len(em['sizeOverLife']))
    else:
        em['sizeOverLife'] = [[0, 1], [1, 1]]
    rot_m = s.module('RotationModule')
    if rot_m is not None:
        spin = val(log, 'spin', rot_m.get('curve'), s.zsign * RAD * sim)
        if nonzero(spin):
            em['spin'] = spin
        if int(num(rot_m.get('separateAxes'), 0)):
            em['spin3D'] = {'x': val(log, 'spin', rot_m.get('x'), -RAD * sim), 'y': val(log, 'spin', rot_m.get('y'), -RAD * sim)}
        log.ok('rotationOverLifetime', 'spin (curve over the lifetime)')
    cbs = s.module('ColorBySpeedModule')
    if cbs is not None:
        rg = uy.vec3(cbs.get('range'), (0, 1, 0))
        em['colorBySpeed'] = {'speed': [rv(rg[0] * s.S), rv(rg[1] * s.S)], 'color': color_keys_json(uy.mmg_keys(cbs.get('gradient')))}
        if uy.mmg_state(cbs.get('gradient')) in (2, 3):
            log.approx('colorBySpeed', 'random between two -> their average')
        log.ok('colorBySpeed', 'gradient over speed')
    sbs = s.module('SizeBySpeedModule')
    if sbs is not None:
        rg = uy.vec3(sbs.get('range'), (0, 1, 0))
        em['sizeBySpeed'] = {'speed': [rv(rg[0] * s.S), rv(rg[1] * s.S)], 'size': cv.keys_of(sbs.get('curve'), lim=LIMITS['sizeKeys'])}
        if int(num(sbs.get('separateAxes'), 0)):
            log.approx('sizeBySpeed', 'separate axes -> x curve')
        log.ok('sizeBySpeed', 'curve over speed')
    rbs = s.module('RotationBySpeedModule')
    if rbs is not None:
        rg = uy.vec3(rbs.get('range'), (0, 1, 0))
        em['spinBySpeed'] = {'speed': [rv(rg[0] * s.S), rv(max(rg[1], rg[0] + 1e-3) * s.S)],
                             'spin': cv.keys_of(rbs.get('curve'), s.zsign * RAD * sim, lim=LIMITS['spin'])}
        if Curve(rbs.get('curve')).random:
            log.approx('rotationBySpeed', 'random between two -> their middle')
        if int(num(rbs.get('separateAxes'), 0)) and (not Curve(rbs.get('x')).is_zero() or not Curve(rbs.get('y')).is_zero()):
            log.approx('rotationBySpeed', 'separate axes -> z only')
        log.ok('rotationBySpeed', 'spin over speed')


# ------------------------------------------------------------------------------------------ noise

def map_noise(s, em):
    log, S = s.log, s.S
    nz = s.module('NoiseModule')
    if nz is None:
        return
    base_m = nz.get('strength')
    axes = (1.0, 1.0, 1.0)
    if int(num(nz.get('separateAxes'), 0)):
        cs = [Curve(nz.get(k)) for k in ('strength', 'strengthY', 'strengthZ')]
        means = [abs(c.mean()) for c in cs]
        bi = max(range(3), key=lambda i: means[i])
        base_m = nz.get(('strength', 'strengthY', 'strengthZ')[bi])
        ref = means[bi] or 1.0
        ua = tuple(m / ref for m in means)
        M = s.Ml if int(num(s.d.get('moveWithTransform'), 0)) == 0 else s.ctx.Mw
        if signed_perm(M):
            axes = tuple(sum(abs(M[i][j]) * ua[j] for j in range(3)) for i in range(3))
        else:
            axes = (sum(ua) / 3,) * 3
            log.approx('noise', 'separate axes under a rotated system -> their average')
        if any(c.curved or c.random for c in cs) and any(not c.is_zero() for i, c in enumerate(cs) if i != bi):
            log.approx('noise', 'separate axis strengths -> the strongest axis curve x per-axis ratios of the averages')
    n = {'strength': val(log, 'strength', base_m, S),
         'frequency': rv(min(100, max(0.001, num(nz.get('frequency'), 0.5) / max(1e-6, S))))}
    if any(abs(a - 1) > 1e-4 for a in axes):
        n['axes'] = vec(axes, 3)
    scroll = Curve(nz.get('scrollSpeed'))
    if not scroll.is_zero():
        n['scroll'] = rv(max(-100, min(100, scroll.mean())))
        if scroll.curved or scroll.random:
            log.approx('noise', 'scroll speed curve -> its average')
    octaves = int(num(nz.get('octaves'), 1))
    if octaves > 1:
        n['octaves'] = min(4, octaves)
        n['octaveMultiplier'] = rv(min(1, max(0, num(nz.get('octaveMultiplier'), 0.5))))
        n['octaveScale'] = rv(min(4, max(1, num(nz.get('octaveScale'), 2))))
        if octaves > 4:
            log.approx('noise', '%d octaves -> 4' % octaves)
    if int(num(nz.get('damping'), 1)):
        n['damping'] = True
    amounts = {}
    for key, ukey, lim, mult in (('position', 'positionAmount', (0, 10), 1.0), ('rotation', 'rotationAmount', LIMITS['noiseRotation'], s.zsign),
                                 ('size', 'sizeAmount', (0, 10), 1.0)):
        c = Curve(nz.get(ukey)) if ukey in nz else Curve(1.0 if key == 'position' else 0.0)
        if c.curved or c.random:
            log.approx('noise', '%s amount curve -> its average' % key)
        amounts[key] = rv(min(lim[1], max(lim[0], c.mean() * mult)))
    if amounts['position'] != 1:
        n['position'] = amounts['position']
    if amounts['rotation']:
        n['rotation'] = amounts['rotation']
    if amounts['size']:
        n['size'] = amounts['size']
    if int(num(nz.get('remapEnabled'), 0)):
        n['remap'] = cv.keys_of(nz.get('remap'), lim=LIMITS['remap'])
        if int(num(nz.get('separateAxes'), 0)) and (cv.keys_of(nz.get('remapY')) != cv.keys_of(nz.get('remap'))):
            log.approx('noise', 'separate remap curves -> the x curve')
    if nonzero(n['strength']) or isinstance(n['strength'], dict):
        em['noise'] = n
    log.ok('noise', 'strength (curve), frequency, scroll, axes, octaves, damping, position / rotation / size amounts, remap')


# ------------------------------------------------------------------------------------------ texture sheet

def map_texture_sheet(s, em, blend):
    log, ctx = s.log, s.ctx
    uv = s.module('UVModule')
    if uv is None:
        return
    n = 1
    if int(num(uv.get('mode'), 0)) == 1:
        sprites = []
        for item in uv.get('sprites') or []:
            ref = get(item, 'sprite') if isinstance(item, dict) else None
            info = sprite_info(ref, ctx.assets) if isinstance(ref, dict) and get(ref, 'fileID') else None
            if info is None:
                log.drop('textureSheetAnimation', 'sprite %s not found in the source folder' % get(ref, 'guid', default='?'))
                continue
            path, rect, name, _ = info
            label = name if rect else None
            sprites.append(ctx.tex.convert(path, blend, rect, label))
        if not sprites:
            return
        em['sprites'] = sprites[:64]
        em['texture'] = sprites[0]
        n = len(em['sprites'])
        log.ok('textureSheetAnimation', 'sprite mode: %d sprite(s) exported as textures' % n)
    else:
        tx, ty = int(num(uv.get('tilesX'), 1)), int(num(uv.get('tilesY'), 1))
        if tx * ty > 256:
            log.approx('textureSheetAnimation', '%dx%d tiles > 256 frames: animation dropped' % (tx, ty))
            return
        if tx * ty <= 1:
            return
        em['frames'] = [tx, ty]
        n = tx * ty
        if int(num(uv.get('animationType'), 0)) == 1:
            row_mode = int(num(uv.get('rowMode'), 1 if int(num(uv.get('randomRow'), 0)) else 0))
            if row_mode == 1:
                em['row'] = 'random'
            else:
                em['row'] = max(0, min(ty - 1, int(num(uv.get('rowIndex'), 0))))
                if row_mode == 2:
                    log.approx('textureSheetAnimation', 'row by mesh index -> row %d' % em['row'])
            n = tx
        log.ok('textureSheetAnimation', '%dx%d tiles%s' % (tx, ty, ', single row' if 'row' in em else ''))
    tmode = int(num(uv.get('timeMode'), 0))
    if tmode == 2:
        em['fps'] = clampv('fps', num(uv.get('fps'), 30), log)
    elif tmode == 1:
        rg = uy.vec3(uv.get('speedRange'), (0, 1, 0))
        em['framesBySpeed'] = [rv(rg[0] * s.S), rv(max(rg[1], rg[0] + 1e-3) * s.S)]
    else:
        fot = uv.get('frameOverTime')
        if isinstance(fot, dict):
            keys = cv.keys_of(fot, which='hi')
            linear = len(keys) == 2 and abs(keys[0][1]) < 0.01 and abs(keys[1][1] - 1) < 0.01 and not Curve(fot).random
            if not linear:
                em['frameOverTime'] = val(log, 'frameOverTime', fot)
        cycles = num(uv.get('cycles'), 1)
        if abs(cycles - 1) > 1e-3:
            em['cycles'] = clampv('cycles', cycles, log)
    sf = Curve(uv.get('startFrame'))
    lo, hi = sf.bounds()
    if hi > 0:
        mult = n if (hi <= 1.0 and n > 1 and not float(hi).is_integer()) else 1.0   # older Unity: a share of the sheet
        v = val(log, 'startFrame', uv.get('startFrame'), mult, 'startFrame')
        if not isinstance(v, dict):
            v = clampv('startFrame', [min(n - 1, x) for x in (v if isinstance(v, list) else [v, v])], log)
        em['startFrame'] = v
    flip = [rv(min(1, max(0, num(uv.get('flipU'), 0)))), rv(min(1, max(0, num(uv.get('flipV'), 0))))]
    rflip = uy.vec3(s.rend.get('m_Flip'))
    flip = [max(flip[0], rv(min(1, abs(rflip[0])))), max(flip[1], rv(min(1, abs(rflip[1]))))]
    if flip[0] or flip[1]:
        em['flip'] = flip


# ------------------------------------------------------------------------------------------ renderer

def _map_range(v, fn):
    """fn applied to every value of a range JSON (number, [min, max] or {curve, curveMin})."""
    if isinstance(v, dict):
        return {k: [[t, rv(fn(x))] for t, x in keys] for k, keys in v.items()}
    if isinstance(v, list):
        a, b = sorted((rv(fn(v[0])), rv(fn(v[1]))))
        return [a, b]
    return rv(fn(v))


def model_file_turn(s, em):
    """Unity mirrors X when it imports a model file; with the z mirror to the engine that is a turn of 180 degrees
    around the model's Y: R Ry(180) = Ry(y + 180) Rx(-x) Rz(-z) (rotation and spin about x and z change sign)."""
    if not getattr(s, 'model_file_meshes', False):
        return
    r3 = em.setdefault('rotation3D', {'x': 0, 'y': 0})
    r3['y'] = _map_range(r3.get('y', 0), lambda x: x + 180)
    r3['x'] = _map_range(r3.get('x', 0), lambda x: -x)
    for key in ('rotation', 'spin'):
        if key in em:
            em[key] = _map_range(em[key], lambda x: -x)
    if 'spin3D' in em:
        em['spin3D']['x'] = _map_range(em['spin3D']['x'], lambda x: -x)
    if 'spinBySpeed' in em:
        em['spinBySpeed']['spin'] = [[t, rv(-x)] for t, x in em['spinBySpeed']['spin']]


SORT = {0: 'none', 1: 'distance', 2: 'oldest', 3: 'youngest', 4: 'distance'}


def map_renderer(s, em):
    """Material + render mode. Returns the material info (tint, blend) and whether particles are drawn."""
    log, ctx, rend = s.log, s.ctx, s.rend
    mats = rend.get('m_Materials') or []
    mat = read_material(mats[0] if mats else None, ctx.assets, log, 'renderer', mesh=s.mesh_mode)
    s.mats = mats
    visible = True
    if s.psr is None:
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
                em['stretch'] = clampv('stretch', vs, log)
            if abs(ls - 1) > 1e-3:
                em['aspect'] = clampv('aspect', ls, log)
            cvs = num(rend.get('m_CameraVelocityScale'), 0)
            if cvs:
                em['cameraStretch'] = clampv('cameraStretch', cvs, log)
            log.ok('renderer', 'stretched billboard')
        elif mode_r == 2:
            em['align'] = 'ground'
            log.ok('renderer', 'horizontal billboard')
        elif mode_r == 3:
            em['align'] = 'upright'
            log.ok('renderer', 'vertical billboard')
        elif mode_r == 4:
            meshes, problems = [], []
            for key in ('m_Mesh', 'm_Mesh1', 'm_Mesh2', 'm_Mesh3'):
                ref = rend.get(key)
                if isinstance(ref, dict) and get(ref, 'fileID'):
                    m, why = ctx.meshes.get(ref)
                    (meshes.append(m) if m else problems.append(why))
            for why in problems:
                log.drop('renderer', 'particle mesh: %s' % why)
            if meshes:
                em['meshes'] = meshes[:4]
                if any(not m.startswith('builtin:') for m in em['meshes']):
                    s.model_file_meshes = True   # turned after the rotations are mapped (model_file_turn)
                    if any(m.startswith('builtin:') for m in em['meshes']):
                        log.approx('renderer', 'built-in and model-file meshes in one emitter: the built-ins get the model-file turn too')
                log.ok('renderer', 'mesh particles: %s' % ', '.join(meshes))
            else:
                log.approx('renderer', 'mesh particles without a usable mesh -> camera billboards')
        elif mode_r == 5:
            visible = False
        else:
            log.ok('renderer', 'billboard')
        if mode_r in (0, 4):
            ra = int(num(rend.get('m_RenderAlignment'), 0))
            if ra == 1:
                em['align'] = 'world'
            elif ra == 2:
                em['align'] = 'local'
            elif ra == 4:
                em['align'] = 'velocity'
                log.approx('renderer', 'render alignment velocity -> stretched along the velocity')
        pv = uy.vec3(rend.get('m_Pivot'))
        if any(abs(x) > 1e-6 for x in pv):
            em['pivot'] = vec((pv[0], pv[1], -pv[2]))
        sort = SORT.get(int(num(rend.get('m_SortMode'), 0)), 'none')
        if sort != 'none' or mat.blend in ('alpha', 'premultiplied', 'multiply', 'subtract'):
            em['sort'] = sort
        order = int(num(rend.get('m_SortingOrder'), 0)) or -int(round(num(rend.get('m_SortingFudge'), 0)))
        if order:
            em['sortOrder'] = max(-100, min(100, order))
        mn, mx = num(rend.get('m_MinParticleSize'), 0), num(rend.get('m_MaxParticleSize'), 0.5)
        if mn > 0:
            em['minScreenSize'] = rv(min(1, mn))
        if abs(mx - 0.5) > 1e-4 and mx > 0:
            em['maxScreenSize'] = rv(min(2, mx))
        if int(num(rend.get('m_UseCustomVertexStreams'), 0)):
            log.drop('renderer', 'custom vertex streams (they only feed custom shaders)')
    em['texture'] = ctx.tex.get(mat)
    em['blend'] = mat.blend
    if not visible:
        em['hidden'] = True
        log.ok('renderer', 'renderer off: particles drive trails / sub-emitters / lights only')
    return mat


# ------------------------------------------------------------------------------------------ collision & triggers

def _avg(log, module, m, what):
    c = Curve(m)
    if c.curved or c.random:
        log.approx(module, '%s curve -> its average' % what)
    return c.mean()


def _ref_ids(mod, list_key, legacy_prefix, count=6):
    refs = [r for r in (mod.get(list_key) or []) if isinstance(r, dict)]
    refs += [mod.get('%s%d' % (legacy_prefix, i)) for i in range(count) if isinstance(mod.get('%s%d' % (legacy_prefix, i)), dict)]
    return [r for r in refs if get(r, 'fileID')]


def map_collision(s, em):
    log, S, ctx = s.log, s.S, s.ctx
    colm = s.module('CollisionModule')
    if colm is None:
        return
    c = {'bounce': rv(min(2, max(0, _avg(log, 'collision', colm.get('m_Bounce'), 'bounce'))))}
    dampen = _avg(log, 'collision', colm.get('m_Dampen'), 'dampen')
    if dampen > 0:
        c['dampen'] = rv(min(1, dampen))
    ll = _avg(log, 'collision', colm.get('m_EnergyLossOnCollision'), 'lifetime loss')
    if ll > 0:
        c['lifeLoss'] = rv(min(1, ll))
    mk = num(colm.get('minKillSpeed'), 0)
    if mk > 0:
        c['minKillSpeed'] = rv(min(1000, mk * S))
    xk = num(colm.get('maxKillSpeed'), 10000)
    if 0 < xk < 10000:
        c['maxKillSpeed'] = rv(min(100000, xk * S))
    rs = num(colm.get('radiusScale'), 1)
    if rs > 0:   # both: collision sphere radius = size / 2 x radius scale
        c['radius'] = rv(min(10, rs))
    pf = ctx.prefab
    if int(num(colm.get('type'), 0)) == 0:
        planes = []
        for ref in _ref_ids(colm, 'm_Planes', 'plane'):
            fid = get(ref, 'fileID')
            tr = pf.transforms.get(fid) or pf.tr_by_go.get(fid)
            if tr is None or get(ref, 'guid'):
                log.drop('collision', 'collision plane outside the prefab')
                continue
            pos, rot, _ = pf.world_of(tr)
            planes.append({'point': vec(ctx.eff_point(pos)), 'normal': vec(cv.mv(ctx.Mw, q_rot(rot, (0.0, 1.0, 0.0))))})
        c['ground'] = False
        if planes:
            c['planes'] = planes[:6]
            if len(planes) > 6:
                log.drop('collision', 'more than 6 planes')
        log.ok('collision', '%d plane(s)' % len(planes))
    else:
        log.approx('collision', 'world collision -> the terrain (other colliders are not particle obstacles)')
    if int(num(colm.get('collisionMode'), 0)) == 1:
        log.approx('collision', '2D collision -> 3D planes / terrain')
    em['collision'] = c


def _collider_sphere(s, doc):
    """Collider component -> (centre in the prefab, radius, approximate?) or None."""
    pf = s.ctx.prefab
    tr = pf.tr_by_go.get(get(doc.data, 'm_GameObject', 'fileID'))
    pos, rot, scale = pf.world_of(tr) if tr is not None else ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0))
    centre = uy.vec3(doc.data.get('m_Center'))
    world = tuple(a + b for a, b in zip(pos, q_rot(rot, v_mul(scale, centre))))
    smax = max(abs(x) for x in scale)
    if doc.class_id == CLASS_SPHERE_COLLIDER:
        return world, num(doc.data.get('m_Radius'), 0.5) * smax, False
    if doc.class_id == CLASS_BOX_COLLIDER:
        size = v_mul(uy.vec3(doc.data.get('m_Size'), (1, 1, 1)), scale)
        return world, 0.5 * sum(x * x for x in size) ** 0.5, True
    if doc.class_id == CLASS_CAPSULE_COLLIDER:
        return world, max(num(doc.data.get('m_Radius'), 0.5), num(doc.data.get('m_Height'), 2) * 0.5) * smax, True
    return None


def map_trigger(s, em):
    log, ctx = s.log, s.ctx
    trg = s.module('TriggerModule')
    if trg is None:
        return
    vols = []
    for ref in _ref_ids(trg, 'primitives', 'collisionShape'):
        doc = ctx.prefab.by_id.get(get(ref, 'fileID')) if not get(ref, 'guid') else None
        if doc is None:
            log.drop('trigger', 'collider outside the prefab (scene objects are not part of the effect)')
            continue
        sph = _collider_sphere(s, doc)
        if sph is None:
            log.drop('trigger', 'collider type %d (only sphere / box / capsule)' % doc.class_id)
            continue
        centre, radius, approx = sph
        if approx:
            log.approx('trigger', '%s collider -> its bounding sphere' % ('box' if doc.class_id == CLASS_BOX_COLLIDER else 'capsule'))
        vols.append({'center': vec(ctx.eff_point(centre)), 'radius': rv(min(1000, radius * s.S))})
    if not vols:
        log.drop('trigger', 'no usable trigger collider: module dropped')
        return
    if len(vols) > 8:
        log.drop('trigger', 'more than 8 colliders')
    names = {0: 'ignore', 1: 'kill', 2: 'callback'}
    t = {'volumes': vols[:8]}
    for key in ('inside', 'outside', 'enter', 'exit'):
        a = names.get(int(num(trg.get(key), 0)), 'ignore')
        if a != 'ignore':
            t[key] = a
    rs = num(trg.get('radiusScale'), 1)
    if rs > 0:
        t['radius'] = rv(min(10, rs))
    em['trigger'] = t
    log.ok('trigger', '%d volume(s)' % len(t['volumes']))


# ------------------------------------------------------------------------------------------ trails, lights, sub-emitters

TEXTURE_MODES = {0: 'stretch', 1: 'tile', 2: 'distribute', 3: 'tile'}


def map_trails(s, em):
    log, ctx = s.log, s.ctx
    tr_m = s.module('TrailModule')
    if tr_m is None:
        return
    t = {}
    if int(num(tr_m.get('mode'), 0)) == 1:
        t['mode'] = 'ribbon'
        rc = int(num(tr_m.get('ribbonCount'), 1))
        if rc != 1:
            t['ribbons'] = max(1, min(16, rc))
    lt = Curve(tr_m.get('lifetime')) if 'lifetime' in tr_m else Curve(1.0)
    frac = min(1.0, max(0.0, lt.mean()))
    if lt.curved or lt.random:
        log.approx('trails', 'trail lifetime curve -> its average')
    t['time'] = rv(min(10, max(0.01, frac * s.life_mean)))
    if t.get('mode') != 'ribbon' and frac > 0:
        t['lifeFraction'] = rv(frac)
    w = tr_m.get('widthOverTrail')
    t['width'] = cv.keys_of(w, lim=LIMITS['sizeKeys']) if isinstance(w, dict) else [[0, 1], [1, 1]]
    if isinstance(w, dict) and Curve(w).random:
        log.approx('trails', 'random width curves -> their middle')
    mats = getattr(s, 'mats', [])
    tmat = read_material(mats[1] if len(mats) > 1 else (mats[0] if mats else None), ctx.assets, log, 'trails')
    cot = tr_m.get('colorOverTrail')
    ckeys = uy.mmg_keys(cot, tmat.tint) if isinstance(cot, dict) else [(0, tmat.tint), (1, tmat.tint)]
    if isinstance(cot, dict) and uy.mmg_state(cot) in (2, 3):
        log.approx('trails', 'random colour over trail -> the average')
    t['color'] = color_keys_json(ckeys)
    col = tr_m.get('colorOverLifetime')
    if isinstance(col, dict):
        lkeys = uy.mmg_keys(col)
        if uy.mmg_state(col) in (2, 3):
            log.approx('trails', 'random trail colour over lifetime -> the average')
        if any(c != (1.0, 1.0, 1.0, 1.0) for _, c in lkeys):
            t['lifeColor'] = color_keys_json(lkeys)
    t['texture'] = ctx.tex.get(tmat)
    t['blend'] = tmat.blend
    if not int(num(tr_m.get('inheritParticleColor'), 1)):
        t['inheritColor'] = False
    if not int(num(tr_m.get('sizeAffectsWidth'), 1)):
        t['sizeAffectsWidth'] = False
    if int(num(tr_m.get('sizeAffectsLifetime'), 0)):
        t['sizeAffectsLifetime'] = True
    ratio = num(tr_m.get('ratio'), 1)
    if ratio < 0.999 and t.get('mode') != 'ribbon':
        t['ratio'] = rv(max(0, ratio))
    md = num(tr_m.get('minVertexDistance'), 0.2) * s.S
    if md > 0:
        t['minDistance'] = clampv('minDistance', md, log)
    tm = TEXTURE_MODES.get(int(num(tr_m.get('textureMode'), 0)), 'stretch')
    if tm != 'stretch':
        t['textureMode'] = tm
    if int(num(tr_m.get('textureMode'), 0)) == 3:
        log.approx('trails', 'texture mode repeat per segment -> tile')
    if not int(num(tr_m.get('dieWithParticles'), 1)):
        t['dieWithParticles'] = False
    if int(num(tr_m.get('attachRibbonsToTransform'), 0)):
        log.approx('trails', 'ribbons attached to the transform -> not attached')
    em['trail'] = t
    log.ok('trails', '%s trail (minQuality medium)' % t.get('mode', 'particle'))


def _light_template(s, ref):
    ctx = s.ctx
    if not isinstance(ref, dict) or not get(ref, 'fileID'):
        return None
    if not get(ref, 'guid') or str(ref.get('guid')).lower() == ctx.prefab.guid:
        d = ctx.prefab.by_id.get(get(ref, 'fileID'))
        if d is None:   # a local reference into the source of a nested prefab instance
            lid = ctx.prefab.origin.get((ctx.prefab.guid, get(ref, 'fileID')))
            d = ctx.prefab.by_id.get(lid) if lid is not None else None
        return d
    d, _ = ctx.assets.object(ref)
    return d


def map_lights(s, em):
    log = s.log
    lm = s.module('LightsModule')
    if lm is None:
        return
    light = {'color': '#ffffff', 'radius': 3.0, 'intensity': 1.0}
    ld = _light_template(s, lm.get('light'))
    if ld is not None and ld.class_id == CLASS_LIGHT:
        light['color'] = hex_color(uy.color(ld.data.get('m_Color')))[:7]
        light['radius'] = num(ld.data.get('m_Range'), 3)
        light['intensity'] = num(ld.data.get('m_Intensity'), 1)
    else:
        log.approx('lights', 'light template not found (prefab and source folder): white, radius 3')
    rc, ic = Curve(lm.get('rangeCurve', 1.0)), Curve(lm.get('intensityCurve', 1.0))
    radius, intensity = light['radius'] * s.S, light['intensity']
    if rc.curved or rc.random:
        light['radiusOverLife'] = cv.keys_of(lm.get('rangeCurve'), lim=LIMITS['lightKeys'])
        if rc.random:
            log.approx('lights', 'random range curves -> their middle')
    else:
        radius *= rc.c_hi
    if ic.curved or ic.random:
        light['intensityOverLife'] = cv.keys_of(lm.get('intensityCurve'), lim=LIMITS['lightKeys'])
        if ic.random:
            log.approx('lights', 'random intensity curves -> their middle')
    else:
        intensity *= ic.c_hi
    light['radius'] = rv(min(100, max(0.01, radius)))
    light['intensity'] = rv(min(20, max(0, intensity)))
    ratio = num(lm.get('ratio'), 0)
    if ratio < 1:
        light['ratio'] = rv(ratio)
    light['max'] = int(min(64, max(0, num(lm.get('maxLights'), 20))))
    if not int(num(lm.get('color'), 1)):
        light['useParticleColor'] = False
    if int(num(lm.get('range'), 1)):
        light['sizeAffectsRadius'] = True
    if not int(num(lm.get('intensity'), 1)):
        light['alphaAffectsIntensity'] = False
    order = ['color', 'radius', 'intensity', 'ratio', 'max', 'useParticleColor', 'sizeAffectsRadius', 'alphaAffectsIntensity',
             'radiusOverLife', 'intensityOverLife']
    em['light'] = {k: light[k] for k in order if k in light}
    log.ok('lights', 'engine point lights (light map)')


SUB_EVENTS = {0: 'birth', 1: 'collision', 2: 'death', 3: 'trigger', 4: 'manual'}
INHERIT = ((1, 'color'), (2, 'size'), (4, 'rotation'), (8, 'lifetime'), (16, 'duration'))


def map_subs(s):
    log = s.log
    subs = []
    sm = s.module('SubModule')
    if sm is None:
        return subs
    for e in sm.get('subEmitters') or []:
        if not isinstance(e, dict):
            continue
        typ = int(num(e.get('type'), 0))
        if typ not in SUB_EVENTS:
            log.drop('subEmitters', 'sub-emitter type %d' % typ)
            continue
        props = int(num(e.get('properties'), 0))
        subs.append({'target': get(e, 'emitter', 'fileID'), 'on': SUB_EVENTS[typ], 'chance': num(e.get('emitProbability'), 1),
                     'inherit': [n for bit, n in INHERIT if props & bit]})
    if subs:
        log.ok('subEmitters', '%d (-> effects of their own)' % len(subs))
    return subs

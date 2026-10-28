"""One Unity ParticleSystem -> one engine emitter (main, emission, shape, velocity and forces).
The visual modules (colour / size / rotation over life, noise, texture sheet, renderer, collision, triggers,
trails, lights, sub-emitters) are in mapping_look.py. Frames and value conversion: convert.py."""
import convert as cv
import unity_yaml as uy
from convert import A_Y_TO_Z, Curve, F, P, RAD, W, combine, euler_zxy, is_identity, mm, mt, mv, q_mat, rv, signed_perm
from materials import sprite_info, sprite_size
from unity_scene import SysLog, q_euler, q_mul, q_rot, v_len, v_mul
from unity_yaml import get, hex_color, num

GRAVITY = 9.81
SHAPES = {0: 'Sphere', 1: 'SphereShell', 2: 'Hemisphere', 3: 'HemisphereShell', 4: 'Cone', 5: 'Box', 6: 'Mesh', 7: 'ConeShell',
          8: 'ConeVolume', 9: 'ConeVolumeShell', 10: 'Circle', 11: 'CircleEdge', 12: 'SingleSidedEdge', 13: 'MeshRenderer',
          14: 'SkinnedMeshRenderer', 15: 'BoxShell', 16: 'BoxEdge', 17: 'Donut', 18: 'Rectangle', 19: 'Sprite', 20: 'SpriteRenderer'}
CLASS_MESH_RENDERER, CLASS_MESH_FILTER, CLASS_SKINNED, CLASS_SPRITE_RENDERER = 23, 33, 137, 212

# engine ranges (engine/fx/FxEffect.cpp)
LIMITS = {'life': (0.01, 60), 'speed': (-500, 500), 'size': (0.001, 100), 'rotation': (-3600, 3600), 'spin': (-36000, 36000),
          'spread': (0, 180), 'radius': (0, 100), 'drag': (0, 50), 'stretch': (0, 10), 'rate': (0, 2000), 'perMeter': (0, 200),
          'burst': (0, 2000), 'count': (0, 2000), 'delay': (0, 60), 'duration': (0, 600), 'fps': (0, 120), 'aspect': (0.01, 100),
          'cycles': (0.01, 1000), 'speedLimit': (0, 1000), 'maxParticles': (0, 100000), 'arc': (0, 360), 'thickness': (0, 1),
          'radial': (-1, 1), 'gravityScale': (-100, 100), 'velocity': (-1000, 1000), 'orbital': (-36000, 36000),
          'speedModifier': (-100, 100), 'inherit': (-10, 10), 'startFrame': (0, 255), 'frameOverTime': (0, 1), 'strength': (0, 100),
          'length': (0, 1000), 'donutRadius': (0, 100), 'randomPosition': (0, 100), 'arcSpeed': (-100, 100), 'cameraStretch': (-100, 100),
          'lightKeys': (0, 100), 'axisLimit': (0, 1e6), 'sizeKeys': (0, 1e6), 'remap': (-1, 1), 'noiseRotation': (-3600, 3600),
          'externalForces': (0, 100), 'interval': (0, 600), 'time': (0, 600), 'minDistance': (0, 100)}


def clampv(key, v, log):
    lo, hi = LIMITS[key]
    if isinstance(v, list):
        a, b = clampv(key, v[0], log), clampv(key, v[1], log)
        return a if a == b else [a, b]
    if v < lo or v > hi:
        log.approx('limits', '%s %g clamped to %g..%g' % (key, v, lo, hi))
    if key in ('burst', 'maxParticles'):
        return int(round(min(hi, max(lo, v))))
    return rv(min(hi, max(cv.safe_lo(lo), v)))


def note_value(log, key, v):
    if v.clamped is not None:
        lo, hi = LIMITS[key]
        log.approx('limits', '%s %g clamped to %g..%g' % (key, v.clamped, lo, hi))
    return v.json


def val(log, key, m, mult=1.0, lim=None):
    """MinMaxCurve -> engine range JSON (clamped to the engine range of `lim` or `key`)."""
    return note_value(log, lim or key, cv.value(m, mult, LIMITS[lim or key]))


def nonzero(v):
    return not (v == 0 or v == 0.0)


def color_keys_json(keys):
    out, last = [], None
    for t, c in keys:
        item = [rv(round(t, 4)), hex_color(c)]
        if item != last:
            out.append(item)
        last = item
    return out


def vec(v, d=4):
    return [rv(round(x, d)) for x in v]


class Context:
    def __init__(self, opts, report, assets, textures, meshes, root_rot_inv, prefab, root_pos):
        self.opts, self.rep, self.assets, self.tex, self.meshes = opts, report, assets, textures, meshes
        self.root_rot_inv, self.prefab, self.root_pos = root_rot_inv, prefab, root_pos
        self.scale = opts.scale
        self.Rinv = q_mat(root_rot_inv)
        self.Mw = mm(P, self.Rinv)            # Unity (prefab) space -> effect frame
        self.Fw = mm(F, self.Rinv)            # Unity (prefab) space -> engine world axes

    def eff_point(self, wpos):
        """Unity prefab-space position -> effect frame (relative to the prefab root, scaled)."""
        return tuple(x * self.scale for x in mv(self.Mw, tuple(a - b for a, b in zip(wpos, self.root_pos))))

    def eff_dir(self, v):
        return mv(self.Mw, v)


class System:
    """Everything one ParticleSystem's mapping needs."""

    def __init__(self, ctx, ps, psr, go_id, wpos, wrot, wscale, path, as_sub):
        self.ctx, self.ps, self.psr, self.go, self.wpos, self.wrot, self.wscale = ctx, ps, psr, go_id, wpos, wrot, wscale
        self.path, self.as_sub = path, as_sub
        self.d = ps.data
        self.log = SysLog(ctx.rep, path)
        self.S = ctx.scale
        self.rot = q_mul(ctx.root_rot_inv, wrot)
        self.Ml = mm(P, q_mat(self.rot))      # Unity system-local -> effect frame
        self.rend = psr.data if psr is not None else {}
        self.mesh_mode = int(num(self.rend.get('m_RenderMode'), 0)) == 4
        self.sim = num(self.d.get('simulationSpeed'), 1.0) or 1.0
        self.main = self.d.get('InitialModule') or {}

    def module(self, name):
        m = self.d.get(name) or {}
        return m if int(num(m.get('enabled'), 0)) else None


# ------------------------------------------------------------------------------------------ main

def map_main(s, em):
    d, main, log, S, sim = s.d, s.main, s.log, s.S, s.sim
    if abs(sim - 1) > 1e-3:
        log.ok('main', 'simulationSpeed %g folded into times and speeds' % sim)
    mode = int(num(d.get('scalingMode'), 1))
    pf = s.ctx.prefab
    lscale = pf.local(pf.tr_by_go[s.go])[2] if s.go in pf.tr_by_go else (1, 1, 1)
    s.size_scale = (sum(abs(x) for x in s.wscale) / 3 if mode == 0 else sum(abs(x) for x in lscale) / 3 if mode == 1 else 1.0) * S
    s.shape_scale = sum(abs(x) for x in s.wscale) / 3 * S
    s.duration = max(0.05, num(d.get('lengthInSec'), 5.0)) / sim
    s.looping = bool(int(num(d.get('looping'), 1)))
    if int(num(d.get('prewarm'), 0)) and s.looping:
        em['prewarm'] = True
    s.delay = val(log, 'delay', d.get('startDelay'), 1 / sim)
    em['life'] = val(log, 'life', main.get('startLifetime'), 1 / sim)
    s.life_mean = Curve(main.get('startLifetime')).mean() / sim
    em['speed'] = val(log, 'speed', main.get('startSpeed'), S * sim)
    em['size'] = val(log, 'size', main.get('startSize'), s.size_scale)
    if int(num(main.get('size3D'), 0)):
        em['sizeY'] = val(log, 'size', main.get('startSizeY'), s.size_scale)
        em['sizeZ'] = val(log, 'size', main.get('startSizeZ'), s.size_scale)
    # rotations: billboards turn the other way round on screen (mirror); meshes use engine world Euler (-x, -y, z)
    s.zsign = 1.0 if s.mesh_mode else -1.0
    rot_z = val(log, 'rotation', main.get('startRotation'), s.zsign * RAD)
    if nonzero(rot_z):
        em['rotation'] = rot_z
    if int(num(main.get('rotation3D'), 0)):
        em['rotation3D'] = {'x': val(log, 'rotation', main.get('startRotationX'), -RAD), 'y': val(log, 'rotation', main.get('startRotationY'), -RAD)}
    log.ok('main', 'duration, looping, prewarm, delay, lifetime, speed, size, rotation (curves over the duration)')
    sc = main.get('startColor')
    st = uy.mmg_state(sc)
    if st == 0:
        c = uy.color(get(sc, 'maxColor'))
        if c != (1.0, 1.0, 1.0, 1.0):
            em['startColor'] = hex_color(c)
    elif st == 2:
        em['startColor'] = [hex_color(uy.color(get(sc, 'minColor'))), hex_color(uy.color(get(sc, 'maxColor')))]
    elif st == 1:
        em['startColor'] = {'gradient': color_keys_json(cv.gradient_keys(get(sc, 'maxGradient'))), 'overTime': True}
    elif st == 3:
        em['startColor'] = {'gradient': color_keys_json(cv.gradient_keys(get(sc, 'maxGradient'))),
                            'gradientMin': color_keys_json(cv.gradient_keys(get(sc, 'minGradient'))), 'overTime': True}
    else:
        em['startColor'] = color_keys_json(cv.gradient_keys(get(sc, 'maxGradient')))
    gm = Curve(main.get('gravityModifier'))
    g_unit = GRAVITY * S * sim * sim
    s.gravity = [0.0, 0.0, 0.0]
    if gm.state == 0:
        s.gravity[1] = -gm.c_hi * g_unit
    else:
        s.gravity[1] = -g_unit
        em['gravityScale'] = val(log, 'gravityScale', main.get('gravityModifier'))
    maxp = int(num(main.get('maxNumParticles'), 1000))
    ring = int(num(d.get('ringBufferMode'), 0))
    if maxp < 1000 or ring:
        em['maxParticles'] = clampv('maxParticles', max(1, maxp), log)
    if ring:
        em['ringBuffer'] = True
    space = int(num(d.get('moveWithTransform'), 0))
    if space == 0:
        em['space'] = 'local'
    elif space == 2:
        log.drop('main', 'custom simulation space (effects have no runtime transform hierarchy): world space')
    s.flip = num(main.get('randomizeRotationDirection'), 0)
    if s.flip > 0:
        log.approx('main', 'flip rotation %g -> rotation / spin ranges made symmetric' % s.flip)


# ------------------------------------------------------------------------------------------ emission

def map_emission(s, em):
    log, S, sim = s.log, s.S, s.sim
    emi = s.d.get('EmissionModule') or {}
    rate = per_m = 0
    bursts = []
    if int(num(emi.get('enabled'), 1)):
        rate = val(log, 'rate', emi.get('rateOverTime'), sim)
        per_m = val(log, 'perMeter', emi.get('rateOverDistance'), 1 / max(1e-6, S))
        for b in emi.get('m_Bursts') or []:
            if not isinstance(b, dict):
                continue
            if 'countCurve' in b:
                cnt = val(log, 'count', b.get('countCurve'))
            else:
                lo, hi = num(b.get('minCount'), 0), num(b.get('maxCount'), 0)
                cnt = rv(lo) if lo == hi else [rv(lo), rv(hi)]
            cycles = max(0, min(1000, int(num(b.get('cycleCount'), 1))))
            interval = max(0.001, num(b.get('repeatInterval'), 0.01) / sim)
            bursts.append({'time': clampv('time', num(b.get('time'), 0) / sim, log), 'count': cnt, 'cycles': cycles,
                           'interval': clampv('interval', interval, log), 'chance': rv(min(1, max(0, num(b.get('probability'), 1.0))))})
        log.ok('emission', 'rate over time / distance (curves over the duration), %d burst(s) (cycles, interval, probability)' % len(bursts))
    else:
        log.ok('emission', 'disabled (emits nothing on its own)')
    if nonzero(rate):
        em['rate'] = rate
    if nonzero(per_m):
        em['perMeter'] = per_m
    simple = lambda b: b['cycles'] == 1 and b['chance'] == 1 and not isinstance(b['count'], (list, dict))  # noqa: E731
    if len(bursts) == 1 and not s.looping and bursts[0]['time'] == 0 and simple(bursts[0]):
        em['burst'] = clampv('burst', bursts[0]['count'], log)
    elif bursts:
        out = []
        for b in bursts:
            if simple(b):
                out.append([b['time'], b['count']])
                continue
            o = {'time': b['time'], 'count': b['count']}
            if b['cycles'] != 1:
                o['cycles'] = b['cycles']
                o['interval'] = b['interval']
            if b['chance'] != 1:
                o['chance'] = b['chance']
            out.append(o)
        em['bursts'] = out
    if nonzero(s.delay):
        em['delay'] = s.delay
    s.emits = nonzero(rate) or nonzero(per_m) or bool(bursts)
    if s.looping and s.emits:
        em['loop'] = True
        em['duration'] = clampv('duration', s.duration, log)
    elif nonzero(rate) or bursts:
        em['duration'] = clampv('duration', s.duration, log)


# ------------------------------------------------------------------------------------------ shape

def _rotation(em, m):
    """shapeRotation of an engine-shape-local -> effect matrix (nothing for identity)."""
    if not is_identity(m):
        em['shapeRotation'] = [rv(round(x, 3)) for x in euler_zxy(m)]


def _axis_rotation(em, m, arc):
    """Shapes symmetric around their +Y axis (full arc): only the axis matters, so the smallest rotation."""
    if arc < 360:
        return _rotation(em, m)
    axis = mv(m, (0.0, 1.0, 0.0))
    _rotation(em, cv.rot_between((0.0, 1.0, 0.0), axis))


def _resolve_mesh(s, sh, stype):
    """Mesh of a mesh / mesh renderer / skinned mesh renderer shape -> (engine mesh, problem)."""
    pf = s.ctx.prefab
    if stype == 6:
        return s.ctx.meshes.get(sh.get('m_Mesh'))
    key = 'm_MeshRenderer' if stype == 13 else 'm_SkinnedMeshRenderer'
    ref = sh.get(key) or {}
    comp = pf.by_id.get(get(ref, 'fileID')) if not get(ref, 'guid') else None
    if comp is None:
        return None, 'the %s is not in this prefab' % ('mesh renderer' if stype == 13 else 'skinned mesh renderer')
    go = get(comp.data, 'm_GameObject', 'fileID')
    if go != s.go:
        s.log.approx('shape', "the renderer's own transform is ignored (the mesh is placed at the system)")
    if stype == 14:
        s.log.approx('shape', 'skinned mesh: bind pose (no animation)')
        return s.ctx.meshes.get(comp.data.get('m_Mesh'))
    mf = next((c for c in pf.comp_by_go.get(go, []) if c.class_id == CLASS_MESH_FILTER), None)
    if mf is None:
        return None, 'the mesh renderer has no MeshFilter'
    return s.ctx.meshes.get(mf.data.get('m_Mesh'))


def _sprite_dims(s, sh, stype, dims):
    ctx, log = s.ctx, s.log
    ref = sh.get('m_Sprite')
    if stype == 20:
        comp = ctx.prefab.by_id.get(get(sh, 'm_SpriteRenderer', 'fileID'))
        ref = get(comp.data, 'm_Sprite') if comp is not None else None
    info = sprite_info(ref, ctx.assets) if isinstance(ref, dict) and get(ref, 'fileID') else None
    if info is None:
        log.drop('shape', '%s: sprite not found in the source folder: 1 x 1 rectangle' % SHAPES[stype])
        w, h = 1.0, 1.0
    else:
        w, h = sprite_size(info)
        log.approx('shape', 'sprite outline -> its rectangle')
    return (w * dims[0], h * dims[1], 0.0)


def map_shape(s, em):
    """Returns (spawn position in Unity prefab space, emission direction in the effect frame)."""
    log = s.log
    sh = s.d.get('ShapeModule') or {}
    wpos = s.wpos
    if not int(num(sh.get('enabled'), 1)):
        em['spread'] = 0
        log.ok('shape', 'disabled: point, straight along +Z')
        return wpos, mv(s.Ml, (0.0, 0.0, 1.0))
    stype = int(num(sh.get('type'), 4))
    name = SHAPES.get(stype, 'type %d' % stype)
    M = mm(P, q_mat(q_mul(s.rot, q_euler(uy.vec3(sh.get('m_Rotation'))))))   # Unity shape-local -> effect frame
    sscale = uy.vec3(sh.get('m_Scale'), (1, 1, 1))
    sscale_mean = sum(abs(x) for x in sscale) / 3
    spos = uy.vec3(sh.get('m_Position'))
    if v_len(spos) > 1e-6:
        wpos = tuple(a + b for a, b in zip(wpos, q_rot(s.wrot, v_mul(s.wscale, spos))))
    radius = num(get(sh, 'radius', 'value'), num(sh.get('radius'), 1.0)) * s.shape_scale * sscale_mean
    arc = num(get(sh, 'arc', 'value'), 360)
    thick = num(sh.get('radiusThickness'), 1.0)
    angle = num(sh.get('angle'), 25.0)
    direction = mv(M, (0.0, 0.0, 1.0))
    round_shape = stype in (0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 17)
    if stype in (0, 1):
        em.update(shape='sphere', radius=clampv('radius', radius, log), radial=1, spread=180)
        direction = (0.0, 1.0, 0.0)   # radial: the axis plays no role
        log.ok('shape', '%s r=%g' % (name, radius))
    elif stype in (2, 3):
        em.update(shape='hemisphere', radius=clampv('radius', radius, log), radial=1, spread=180)
        _axis_rotation(em, mm(M, A_Y_TO_Z), arc)  # engine dome: effect +Y; Unity dome: shape +Z
        if 'shapeRotation' in em:
            direction = (0.0, 1.0, 0.0)
        log.ok('shape', '%s r=%g' % (name, radius))
    elif stype in (4, 7, 8, 9):
        em.update(shape='cone', radius=clampv('radius', radius, log), spread=clampv('spread', angle, log))
        if stype in (8, 9):                        # the cone opens around "direction": its rotation is baked there
            em['length'] = clampv('length', num(sh.get('length'), 5) * s.shape_scale * abs(sscale[2]), log)
            em['emitFrom'] = 'shell' if stype == 9 else 'volume'
        log.ok('shape', '%s angle=%g r=%g' % (name, angle, radius))
    elif stype in (10, 11):
        em.update(shape='cone', radius=clampv('radius', radius, log), spread=90, radial=1)
        log.ok('shape', '%s r=%g' % (name, radius))
    elif stype == 17:
        em.update(shape='donut', radius=clampv('radius', radius, log),
                  donutRadius=clampv('donutRadius', num(sh.get('donutRadius'), 0.2) * s.shape_scale * sscale_mean, log))
        _axis_rotation(em, mm(M, A_Y_TO_Z), arc)  # engine ring: effect XZ plane around +Y (like disc / ring)
        direction = (0.0, 1.0, 0.0)
        log.ok('shape', 'Donut r=%g' % radius)
    elif stype in (5, 15, 16, 18, 19, 20):
        dims = tuple(abs(x) * s.shape_scale for x in sscale)
        if stype == 18:
            dims = (dims[0], dims[1], 0.0)
        if stype in (19, 20):
            dims = _sprite_dims(s, sh, stype, dims)
        em['shape'] = 'box'
        if stype in (15, 16):
            em['emitFrom'] = 'shell' if stype == 15 else 'edge'
        R = mm(M, mt(P))                           # engine box (= Unity box through P) -> effect
        if signed_perm(R):                         # axis-aligned: baked into the box and the direction
            em['box'] = [rv(round(sum(abs(M[i][j]) * dims[j] for j in range(3)), 4)) for i in range(3)]
        else:
            em['box'] = [rv(round(abs(x), 4)) for x in mv(P, dims)]
            _rotation(em, R)
            direction = mv(P, (0.0, 0.0, 1.0))
        em['spread'] = 0
        log.ok('shape', '%s %s' % (name, em['box']))
    elif stype == 12:
        em.update(shape='line', radius=clampv('radius', radius, log), spread=0)
        if abs(abs(M[0][0]) - 1) < 1e-4:           # engine line: effect X; Unity edge: shape X, emitting along shape +Y
            direction = mv(M, (0.0, 1.0, 0.0))
        else:
            _rotation(em, M)
            direction = (0.0, 1.0, 0.0)
        log.ok('shape', 'edge, length %g' % (2 * radius))
    elif stype in (6, 13, 14):
        mesh, problem = _resolve_mesh(s, sh, stype)
        if mesh is None:
            em.update(shape='sphere', radius=clampv('radius', max(0.1, radius), log), radial=1, spread=180)
            log.drop('shape', '%s: %s: sphere of the shape radius' % (name, problem))
        else:
            em.update(shape='mesh', mesh=mesh, emitFrom={0: 'vertex', 1: 'edge', 2: 'triangle'}.get(int(num(sh.get('placementMode'), 0)), 'vertex'))
            # a model vertex f (engine model space = Unity mesh space, z flipped) lands at R * toEffect(f) = R W f;
            # Unity puts it at P Rs F f, so R = P Rs F W (identity for an unrotated shape)
            _rotation(em, mm(mm(M, F), W))
            direction = (0.0, 1.0, 0.0)
            ms = tuple(abs(x) * s.shape_scale for x in sscale)   # "box" scales the mesh (model axes)
            if any(abs(x - 1) > 1e-4 for x in ms):
                em['box'] = vec(ms)
            log.ok('shape', '%s %s (%s), along the surface normals' % (name, mesh, em['emitFrom']))
    else:
        em.update(shape='sphere', radius=clampv('radius', max(0.1, radius), log), radial=1, spread=180)
        log.drop('shape', '%s -> sphere of the shape radius' % name)
    if round_shape:
        _round_options(s, em, sh, stype, arc, thick)
    if int(num(get(sh, 'radius', 'mode'), 0)):
        log.approx('shape', 'radius mode loop / ping-pong / spread -> random along the radius')
    rnd = num(sh.get('randomDirectionAmount'), 0)
    sph = num(sh.get('sphericalDirectionAmount'), 0)
    rpos = num(sh.get('randomPositionAmount'), 0)
    if rnd > 0:
        em['randomDirection'] = rv(min(1, rnd))
    if sph > 0:
        em['sphericalDirection'] = rv(min(1, sph))
    if rpos > 0:
        em['randomPosition'] = clampv('randomPosition', rpos * s.shape_scale, log)
    if int(num(sh.get('alignToDirection'), 0)):
        em['alignToDirection'] = True
    return wpos, direction


def _round_options(s, em, sh, stype, arc, thick):
    log = s.log
    if thick < 1:
        em['thickness'] = clampv('thickness', thick, log)
    if stype in (1, 3, 7, 11):
        em['thickness'] = 0
    if arc < 360:
        em['arc'] = clampv('arc', arc, log)
    amode = int(num(get(sh, 'arc', 'mode'), 0))
    if amode:
        em['arcMode'] = {1: 'loop', 2: 'pingpong', 3: 'spread'}.get(amode, 'random')
        spd = Curve(get(sh, 'arc', 'speed', default=1.0))
        em['arcSpeed'] = clampv('arcSpeed', spd.mean(), log)
        if spd.curved or spd.random:
            log.approx('shape', 'arc speed curve -> its average')
        spread = num(get(sh, 'arc', 'spread'), 0)
        if spread > 0:
            em['arcSpread'] = rv(min(1, spread))


# ------------------------------------------------------------------------------------------ velocity & forces

def _axes(log, module, curves, M, mult, lim_key):
    """Per-axis Unity curves -> 3 engine range JSONs through the matrix M (rows = engine axes)."""
    out, mixed = [], False
    for i in range(3):
        v = combine([(c, M[i][j] * mult) for j, c in enumerate(curves)], LIMITS[lim_key])
        mixed |= v.mixed
        out.append(note_value(log, lim_key, v))
    if mixed:
        log.approx(module, 'random values of rotated axes share one random number')
    return out


def map_velocity(s, em):
    log, S, sim, ctx = s.log, s.S, s.sim, s.ctx
    vel = s.module('VelocityModule')
    if vel is None:
        return
    world = int(num(vel.get('inWorldSpace'), 0))
    lin = [Curve(vel.get(k)) for k in ('x', 'y', 'z')]
    orb = [Curve(vel.get(k)) for k in ('orbitalX', 'orbitalY', 'orbitalZ')]
    radial = Curve(vel.get('radial'))
    smod = Curve(vel.get('speedModifier')) if 'speedModifier' in vel else Curve(1.0)
    plain_mod = smod.state == 0 and abs(smod.c_hi - 1) < 1e-6
    exotic = any(not c.is_zero() for c in orb) or not radial.is_zero() or not plain_mod
    if not world and not exotic and all(c.state == 0 for c in lin):
        v = mv(s.Ml, tuple(c.c_hi for c in lin))
        if v_len(v) > 1e-6:
            em['velocity'] = vec(tuple(x * S * sim for x in v))
        log.ok('velocityOverLifetime', 'constant linear velocity')
        return
    vo = {}
    for k, v in zip('xyz', _axes(log, 'velocityOverLifetime', lin, ctx.Fw if world else s.Ml, S * sim, 'velocity')):
        if nonzero(v):
            vo[k] = v
    if world:
        vo['space'] = 'world'
    # angular velocity = axial vector: Unity radians/s -> effect frame degrees/s, -P under the mirror
    Mo = cv.neg(mm(P, ctx.Rinv if world else q_mat(s.rot)))
    for k, v in zip(('orbitalX', 'orbitalY', 'orbitalZ'), _axes(log, 'velocityOverLifetime', orb, Mo, RAD * sim, 'orbital')):
        if nonzero(v):
            vo[k] = v
    offc = [Curve(vel.get(k)) for k in ('orbitalOffsetX', 'orbitalOffsetY', 'orbitalOffsetZ')]
    if any(c.curved or c.random for c in offc):
        log.approx('velocityOverLifetime', 'orbital offset curves -> their average')
    off = tuple(x * S for x in mv(ctx.Mw if world else s.Ml, tuple(c.mean() for c in offc)))
    if any(nonzero(v) for k, v in vo.items() if k.startswith('orbital')) or not radial.is_zero():
        # the engine orbits around the effect origin + orbitalOffset; Unity around the system's own position
        centre = ctx.eff_point(s.wpos) if not s.as_sub else (0.0, 0.0, 0.0)
        off = tuple(a + b for a, b in zip(off, centre))
    if v_len(off) > 1e-6:
        vo['orbitalOffset'] = vec(off)
    if not radial.is_zero():
        vo['radial'] = val(log, 'velocity', vel.get('radial'), S * sim)
    if not plain_mod:
        vo['speedModifier'] = val(log, 'speedModifier', vel.get('speedModifier'))
    em['velocityOverLife'] = vo
    log.ok('velocityOverLifetime', 'linear (curves), orbital, offset, radial, speed modifier')


def map_forces(s, em):
    log, S, sim, ctx = s.log, s.S, s.sim, s.ctx
    force = s.module('ForceModule')
    if force is not None:
        world = int(num(force.get('inWorldSpace'), 0))
        curves = [Curve(force.get(k)) for k in ('x', 'y', 'z')]
        fo = {}
        for k, v in zip('xyz', _axes(log, 'forceOverLifetime', curves, ctx.Fw if world else s.Ml, S * sim * sim, 'velocity')):
            if nonzero(v):
                fo[k] = v
        fo['space'] = 'world' if world else 'local'
        if int(num(force.get('randomizePerFrame'), 0)):
            fo['randomize'] = True
        em['force'] = fo
        log.ok('forceOverLifetime', 'force (curves over the lifetime, %s space)' % fo['space'])
    if any(abs(x) > 1e-6 for x in s.gravity):
        em['gravity'] = vec(s.gravity)
    clamp = s.module('ClampVelocityModule')
    if clamp is not None:
        if int(num(clamp.get('separateAxis'), 0)):
            world = int(num(clamp.get('inWorldSpace'), 0))
            Mc = ctx.Fw if world else s.Ml
            if not signed_perm(Mc):
                log.approx('limitVelocity', 'per-axis limits under a rotated system -> limits of the nearest axes')
            Ma = tuple(tuple(1.0 if abs(Mc[i][j]) == max(abs(x) for x in Mc[i]) else 0.0 for j in range(3)) for i in range(3))
            lim = _axes(log, 'limitVelocity', [Curve(clamp.get(k)) for k in ('x', 'y', 'z')], Ma, S, 'axisLimit')
            em['speedLimitAxes'] = {'x': lim[0], 'y': lim[1], 'z': lim[2], 'space': 'world' if world else 'local'}
        elif Curve(clamp.get('magnitude')).bounds()[1] < 1e5:
            em['speedLimit'] = val(log, 'speedLimit', clamp.get('magnitude'), S)
        damp = num(clamp.get('dampen'), 1)
        if abs(damp - 1) > 1e-4:
            em['limitDampen'] = rv(min(1, max(0, damp)))
        if 'drag' in clamp and not Curve(clamp.get('drag')).is_zero():
            em['drag'] = val(log, 'drag', clamp.get('drag'))
            if int(num(clamp.get('multiplyDragByParticleSize'), 1)):
                em['dragBySize'] = True
            if int(num(clamp.get('multiplyDragByParticleVelocity'), 1)):
                em['dragBySpeed'] = True
        log.ok('limitVelocity', 'speed limit (curve), per-axis limits, dampen, drag (by size / speed)')
    inh = s.module('InheritVelocityModule')
    if inh is not None and not Curve(inh.get('m_Curve')).is_zero():
        em['inheritVelocity'] = {'mode': 'current' if int(num(inh.get('m_Mode'), 0)) == 1 else 'initial',
                                 'amount': val(log, 'inherit', inh.get('m_Curve'))}
        log.ok('inheritVelocity', em['inheritVelocity']['mode'])
    lbs = s.module('LifetimeByEmitterSpeedModule')
    if lbs is not None:
        rg = uy.vec3(lbs.get('m_Range'), (0, 1, 0))
        em['lifeBySpeed'] = {'speed': [rv(rg[0] * S), rv(max(rg[1], rg[0] + 1e-3) * S)],
                             'factor': cv.keys_of(lbs.get('m_Curve'), lim=LIMITS['lightKeys'])}
        if Curve(lbs.get('m_Curve')).random:
            log.approx('lifetimeByEmitterSpeed', 'random between two curves -> their middle')
        log.ok('lifetimeByEmitterSpeed', 'lifetime factor over the emitter speed')
    ext = s.module('ExternalForcesModule')
    if ext is not None:
        mc = Curve(ext.get('multiplierCurve')) if 'multiplierCurve' in ext else Curve(num(ext.get('multiplier'), 1.0))
        if mc.curved or mc.random:
            log.approx('externalForces', 'multiplier curve -> its average')
        if mc.mean() > 0:
            em['externalForces'] = clampv('externalForces', mc.mean(), log)
        if int(num(ext.get('influenceFilter'), 0)):
            log.approx('externalForces', 'influence filter (layer mask / list) -> every force field of the effect')
        log.ok('externalForces', 'effect forceFields (+ wind)')
    if s.module('CustomDataModule') is not None:
        log.drop('customData', 'custom data only feeds custom shaders (custom vertex streams)')

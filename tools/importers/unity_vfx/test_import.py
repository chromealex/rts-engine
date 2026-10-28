#!/usr/bin/env python3
"""Unit tests of the Unity effect importer (stdlib unittest). Run: python3 tools/importers/unity_vfx/test_import.py
Test material: tests/data/unity_vfx (synthetic prefabs, materials, .meta and generated textures)."""
import io
import json
import math
import os
import shutil
import sys
import tarfile
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import convert as cv  # noqa: E402
import images  # noqa: E402
import unity_scene as us  # noqa: E402
import unity_yaml as uy  # noqa: E402
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location('unity_vfx_import', os.path.join(HERE, 'import.py'))
imp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(imp)

DATA = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'tests', 'data', 'unity_vfx', 'Assets', 'FX'))
MODULES = os.path.normpath(os.path.join(DATA, '..', 'Modules'))


def run_import(*args):
    out = io.StringIO()
    with redirect_stdout(out):
        code = imp.run(list(args) + ['--json'])
    return code, json.loads(out.getvalue())


def by_comment(effect, suffix):
    return next(e for e in effect['emitters'] if e['comment'].endswith(suffix))


class YamlTests(unittest.TestCase):
    def test_documents_flow_sequences_and_wrapped_scalars(self):
        text = '''%YAML 1.1
%TAG !u! tag:unity3d.com,2011:
--- !u!1 &100
GameObject:
  m_Component:
  - component: {fileID: 4}
  - component: {fileID: 198}
  m_Name: 'It''s fire'
  m_ShaderKeywords: _ALPHABLEND_ON _COLOROVERLAY_ON
    _FADING_ON
  m_Empty: []
  nested:
    m_Curve:
    - serializedVersion: 3
      time: 0
      value: -Infinity
    m_Map: {a: {x: 1, y: 2.5}, b: [1, 2]}
--- !u!4 &4 stripped
Transform:
  m_GameObject: {fileID: 100}
'''
        docs = uy.parse_documents(text)
        self.assertEqual([(d.class_id, d.file_id, d.stripped) for d in docs], [(1, 100, False), (4, 4, True)])
        go = docs[0].data
        self.assertEqual(docs[0].type, 'GameObject')
        self.assertEqual([c['component']['fileID'] for c in go['m_Component']], [4, 198])
        self.assertEqual(go['m_Name'], "It's fire")
        self.assertEqual(go['m_ShaderKeywords'], '_ALPHABLEND_ON _COLOROVERLAY_ON _FADING_ON')
        self.assertEqual(go['m_Empty'], [])
        self.assertEqual(go['nested']['m_Curve'][0]['value'], float('-inf'))
        self.assertEqual(go['nested']['m_Map'], {'a': {'x': 1, 'y': 2.5}, 'b': [1, 2]})

    def test_meta_file(self):
        d = uy.parse_documents('fileFormatVersion: 2\nguid: 0123456789abcdef0123456789abcdef\nTextureImporter:\n  alphaSource: 2\n')
        self.assertEqual(d[0].data['guid'], '0123456789abcdef0123456789abcdef')
        self.assertEqual(d[0].data['TextureImporter']['alphaSource'], 2)


class CurveTests(unittest.TestCase):
    def curve(self, *keys):
        return {'m_Curve': [{'time': t, 'value': v, 'inSlope': i, 'outSlope': o} for t, v, i, o in keys]}

    def test_hermite_matches_linear_when_slopes_match(self):
        keys = uy.curve_keys(self.curve((0, 0, 1, 1), (1, 1, 1, 1)))
        for t in (0, 0.25, 0.5, 0.9, 1):
            self.assertAlmostEqual(uy.curve_eval(keys, t), t, places=5)

    def test_hermite_ease_and_step(self):
        keys = uy.curve_keys(self.curve((0, 0, 0, 0), (1, 1, 0, 0)))
        self.assertAlmostEqual(uy.curve_eval(keys, 0.5), 0.5, places=5)
        self.assertLess(uy.curve_eval(keys, 0.1), 0.1)          # ease in
        step = uy.curve_keys(self.curve((0, 2, 0, float('inf')), (1, 5, 0, 0)))
        self.assertEqual(uy.curve_eval(step, 0.7), 2)

    def test_min_max_curve_modes(self):
        self.assertEqual(uy.mmc_range({'minMaxState': 0, 'scalar': 3}), (3, 3))
        self.assertEqual(uy.mmc_range({'minMaxState': 3, 'scalar': 4, 'minScalar': 1}), (1, 4))
        c = {'minMaxState': 1, 'scalar': 2, 'maxCurve': self.curve((0, 0.5, 0, 0), (1, 1, 0, 0))}
        self.assertEqual(uy.mmc_range(c), (1.0, 2.0))
        keys = uy.mmc_keys(c)
        self.assertEqual(keys[0], [0.0, 1.0])
        self.assertEqual(keys[-1], [1.0, 2.0])
        flat = uy.mmc_keys({'minMaxState': 0, 'scalar': 1.5})
        self.assertEqual(flat, [[0.0, 1.5], [1.0, 1.5]])

    def test_gradient_alpha_keys_and_fixed_mode(self):
        g = {'key0': {'r': 1, 'g': 0, 'b': 0, 'a': 0}, 'key1': {'r': 0, 'g': 0, 'b': 1, 'a': 1}, 'key2': {'r': 0, 'g': 0, 'b': 0, 'a': 0},
             'ctime0': 0, 'ctime1': 65535, 'atime0': 0, 'atime1': 32768, 'atime2': 65535, 'm_NumColorKeys': 2, 'm_NumAlphaKeys': 3}
        c = uy.gradient_eval(g, 0.5)
        self.assertAlmostEqual(c[0], 0.5, places=2)
        self.assertAlmostEqual(c[3], 1.0, places=2)
        keys = uy.mmg_keys({'minMaxState': 1, 'maxGradient': g})
        self.assertEqual([round(t, 2) for t, _ in keys], [0.0, 0.5, 1.0])
        g['m_Mode'] = 1
        keys = uy.mmg_keys({'minMaxState': 1, 'maxGradient': g})
        alphas = [(round(t, 2), round(c[3], 2)) for t, c in keys]
        self.assertIn((0.0, 0.0), alphas)
        self.assertIn((0.0, 1.0), alphas)   # step right after the first key
        self.assertIn((0.5, 0.0), alphas)


class ImageTests(unittest.TestCase):
    def test_png_and_tga_round_trip(self):
        img = images.Image(3, 2, bytes(range(24)))
        with tempfile.TemporaryDirectory() as tmp:
            images.write_png(os.path.join(tmp, 'a.png'), img)
            with open(os.path.join(tmp, 'a.png'), 'rb') as f:
                back = images.read_png(f.read())
            self.assertEqual((back.w, back.h, bytes(back.rgba)), (3, 2, bytes(img.rgba)))
            for rle in (False, True):
                images.write_tga(os.path.join(tmp, 'a.tga'), img, rle=rle)
                with open(os.path.join(tmp, 'a.tga'), 'rb') as f:
                    t = images.read_tga(f.read())
                self.assertEqual(bytes(t.rgba), bytes(img.rgba))

    def test_alpha_from_gray(self):
        img = images.alpha_from_gray(images.Image(1, 1, bytes((255, 255, 255, 255))))
        self.assertEqual(img.rgba[3], 255)
        img = images.alpha_from_gray(images.Image(1, 1, bytes((0, 0, 0, 255))))
        self.assertEqual(img.rgba[3], 0)


class ImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.code, cls.rep = run_import('--source', DATA, '--game', cls.tmp)
        cls.fx = {}
        for e in cls.rep['effects']:
            with open(os.path.join(cls.tmp, e['file'])) as f:
                cls.fx[e['name']] = json.load(f)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def test_effects_and_textures_written(self):
        self.assertEqual(self.code, 0)
        self.assertTrue(self.rep['ok'])
        self.assertEqual(sorted(self.fx), ['explosion', 'explosion_sparkpop', 'magicaura'])
        for t in ('fire_sheet', 'smoke', 'spark', 'ring'):
            self.assertTrue(os.path.exists(os.path.join(self.tmp, 'fx', 'textures', t + '.png')), t)
        with open(os.path.join(self.tmp, 'fx', 'textures', 'smoke.png'), 'rb') as f:
            smoke = images.read_png(f.read())
        self.assertTrue(smoke.has_alpha())          # "alpha from grayscale" import setting applied

    def test_hierarchy_becomes_layers(self):
        ex = self.fx['explosion']
        self.assertEqual([e['comment'] for e in ex['emitters']],
                         ['Unity: Explosion', 'Unity: Explosion/Smoke', 'Unity: Explosion/Sparks', 'Unity: Explosion/Shockwave'])
        smoke = by_comment(ex, '/Smoke')
        self.assertEqual(smoke['offset'], [0.0, 0.3, 0.0])  # child at local z 0.3 under a root rotated -90 X = up
        self.assertNotIn('direction', smoke)                 # cone +Z rotated up = the engine's default direction

    def test_main_emission_shape_and_colour(self):
        fb = by_comment(self.fx['explosion'], 'Explosion')
        self.assertEqual(fb['burst'], 25)
        self.assertEqual(fb['life'], [0.5, 0.9])
        self.assertEqual(fb['speed'], [1.0, 4.0])
        self.assertEqual(fb['shape'], 'sphere')
        self.assertEqual(fb['radial'], 1)
        self.assertEqual(fb['frames'], [4, 4])
        self.assertEqual(fb['blend'], 'additive')
        self.assertEqual(fb['texture'], 'fx/textures/fire_sheet.png')
        self.assertEqual(fb['startColor'], ['#ffde87ff', '#ffab45ff'])
        self.assertAlmostEqual(fb['gravity'][1], 0.2 * 9.81, places=2)  # negative gravity modifier: rises
        self.assertEqual(fb['maxParticles'], 100)
        self.assertEqual(fb['color'][0][1], '#ffffe6ff')
        self.assertEqual(fb['color'][-1][1][-2:], '00')      # alpha key 0 at the end
        self.assertGreater(len(fb['sizeOverLife']), 2)       # curve sampled into keys
        smoke = by_comment(self.fx['explosion'], '/Smoke')
        self.assertEqual(smoke['blend'], 'alpha')
        self.assertEqual(smoke['bursts'][1], {'time': 0.3, 'count': [4, 6], 'cycles': 2, 'interval': 0.2})
        self.assertEqual(smoke['shape'], 'cone')
        self.assertEqual(smoke['spread'], 30)
        self.assertEqual(smoke['noise']['frequency'], 0.8)
        self.assertEqual(smoke['force'], {'y': 0.8, 'space': 'local'})  # local force +Z under the rotated root
        self.assertEqual(smoke['delay'], 0.1)
        self.assertEqual((smoke['flipRotation'], smoke['rotation']), (1, [-360, 0]))   # Unity flip rotation: its own key

    def test_renderer_modes_trails_lights_collision_subemitters(self):
        sp = by_comment(self.fx['explosion'], '/Sparks')
        self.assertEqual(sp['align'], 'velocity')
        self.assertEqual(sp['stretch'], 0.05)
        self.assertEqual(sp['shape'], 'hemisphere')
        self.assertEqual(sp['trail']['blend'], 'additive')        # custom shader "Blend SrcAlpha One"
        self.assertEqual(sp['trail']['width'], [[0.0, 1.0], [1.0, 0.0]])
        self.assertEqual(sp['light']['color'], '#ff9940')
        self.assertEqual(sp['light']['max'], 6)
        self.assertEqual(sp['collision']['planes'], [{'point': [0, -0.1, 0], 'normal': [0, 1, 0]}])
        self.assertFalse(sp['collision']['ground'])
        self.assertEqual(sp['subEmitters'], [{'on': 'death', 'effect': 'explosion_sparkpop', 'chance': 0.5, 'inheritColor': True}])
        self.assertEqual(by_comment(self.fx['explosion'], '/Shockwave')['align'], 'ground')
        aura = by_comment(self.fx['magicaura'], 'Magic Aura')
        self.assertEqual(aura['align'], 'upright')
        self.assertEqual(aura['blend'], 'premultiplied')
        self.assertEqual(aura['space'], 'local')
        self.assertEqual(aura['fps'], 12)
        self.assertEqual(aura['startFrame'], [0, 3])
        self.assertTrue(aura['loop'])
        self.assertIn('colorBySpeed', aura)
        self.assertIn('sizeBySpeed', aura)
        self.assertEqual(aura['velocityOverLife'], {'y': {'curve': [[0, 0.5], [1, 1.5]]}, 'orbitalY': -114.59})
        self.assertTrue(aura['prewarm'])
        self.assertEqual((aura['shape'], aura['radial']), ('ring', 1))   # edge circle: outward in its plane
        self.assertNotIn('direction', aura)                  # circle rotated -90 X: its normal is up
        self.assertNotIn('shapeRotation', aura)
        motes = by_comment(self.fx['magicaura'], '/Motes')
        self.assertEqual(motes['size'], [0.08, 0.16])        # hierarchy scaling mode: x2
        self.assertEqual(motes['perMeter'], 5)
        self.assertEqual(motes['blend'], 'additive')         # Shader Graph: alpha mode additive

    def test_report_lists_what_could_not_be_mapped(self):
        dropped = '\n'.join(self.rep['dropped'])
        approx = '\n'.join(self.rep['approximate'])
        self.assertIn('FireGraph.vfx: VFX Graph', dropped)
        self.assertIn('VisualEffect (VFX Graph)', dropped)
        self.assertIn('script ExplosionShake.cs', dropped)
        self.assertIn('AudioSource', dropped)
        self.assertNotIn('orbital', dropped)
        self.assertNotIn('prewarm', dropped)
        self.assertIn('Shader Graph Glow.shadergraph', approx)
        self.assertIn('custom shader TrailGlow.shader', approx)
        self.assertEqual(by_comment(self.fx['magicaura'], '/Motes')['meshes'], ['builtin:cube'])
        systems = {s['system']: s['modules'] for s in self.rep['systems']}
        self.assertIn('trails', systems['Explosion/Sparks'])

    def test_scale_option_and_single_prefab(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, rep = run_import('--source', os.path.join(DATA, 'Prefabs', 'Explosion.prefab'), '--game', tmp, '--name', 'Boom', '--scale', '0.5')
            self.assertEqual(code, 0)
            self.assertEqual([e['name'] for e in rep['effects']], ['boom', 'boom_sparkpop'])
            with open(os.path.join(tmp, 'fx', 'boom.json')) as f:
                fx = json.load(f)
            self.assertEqual(fx['emitters'][0]['size'], [0.5, 0.8])
            self.assertEqual(fx['emitters'][0]['radius'], 0.2)

    def test_unitypackage(self):
        with tempfile.TemporaryDirectory() as tmp:
            pkg = os.path.join(tmp, 'fx.unitypackage')
            with tarfile.open(pkg, 'w:gz') as tf:
                for dp, _, fns in os.walk(DATA):
                    for fn in fns:
                        if fn.endswith('.meta') or fn.endswith('.py'):
                            continue
                        path = os.path.join(dp, fn)
                        meta = path + '.meta'
                        if not os.path.exists(meta):
                            continue
                        with open(meta, 'rb') as f:
                            meta_bytes = f.read()
                        with open(path, 'rb') as f:
                            asset_bytes = f.read()
                        guid = uy.parse_documents(meta_bytes.decode())[0].data['guid']
                        rel = 'Assets/FX/' + os.path.relpath(path, DATA).replace(os.sep, '/')
                        for name, data in (('asset', asset_bytes), ('asset.meta', meta_bytes), ('pathname', rel.encode())):
                            info = tarfile.TarInfo('%s/%s' % (guid, name))
                            info.size = len(data)
                            tf.addfile(info, io.BytesIO(data))
            game = os.path.join(tmp, 'game')
            os.makedirs(game)
            code, rep = run_import('--source', pkg, '--game', game, '--prefab', 'Explosion')
            self.assertEqual(code, 0)
            self.assertEqual(rep['effects'][0]['name'], 'explosion')
            self.assertTrue(os.path.exists(os.path.join(game, 'fx', 'textures', 'fire_sheet.png')))

    def test_errors_are_readable(self):
        with tempfile.TemporaryDirectory() as tmp:
            code, rep = run_import('--source', os.path.join(tmp, 'missing.prefab'), '--game', tmp)
            self.assertNotEqual(code, 0)
            self.assertIn('no such file', rep['warnings'][0])
            binary = os.path.join(tmp, 'bin.prefab')
            with open(binary, 'wb') as f:
                f.write(b'\x00\x01UnityFS binary')
            code, rep = run_import('--source', binary, '--game', tmp)
            self.assertNotEqual(code, 0)
            self.assertIn('Force Text', '\n'.join(rep['warnings']))


class ConversionTests(unittest.TestCase):
    def mmc(self, state, scalar, keys=None, min_keys=None, min_scalar=None):
        m = {'minMaxState': state, 'scalar': scalar}
        if min_scalar is not None:
            m['minScalar'] = min_scalar
        for name, ks in (('maxCurve', keys), ('minCurve', min_keys)):
            if ks:
                m[name] = {'m_Curve': [{'time': t, 'value': v, 'inSlope': i, 'outSlope': o} for t, v, i, o in ks]}
        return m

    def test_min_max_curve_to_engine_ranges(self):
        self.assertEqual(cv.value(self.mmc(0, 2)).json, 2)
        self.assertEqual(cv.value(self.mmc(3, 4, min_scalar=-1)).json, [-1, 4])
        v = cv.value(self.mmc(1, 2, [(0, 0, 1, 1), (1, 1, 1, 1)])).json
        self.assertEqual(v, {'curve': [[0, 0], [1, 2]]})
        v = cv.value(self.mmc(2, 1, [(0, 1, 0, 0), (1, 1, 0, 0)], [(0, 0, 0, 0), (1, 0.5, 0, 0)])).json
        self.assertEqual(v['curve'], [[0, 1], [1, 1]])
        self.assertEqual(v['curveMin'][0], [0, 0])
        v = cv.value(self.mmc(0, 5), lim=(0, 2))
        self.assertEqual((v.json, v.clamped), (2, 5))

    def test_sampling_is_accurate_and_bounded(self):
        # an ease in / out over the lifetime: within 1% of its range; a full wave: 16 keys, within 2%
        for ks, tol in (([(0, 0, 0, 0), (0.4, 1, 0, 0), (1, 0.2, -2, -2)], 0.01), ([(0, 0, 0, 0), (0.3, 1, 0, 0), (0.6, -1, 0, 0), (1, 0.5, 0, 0)], 0.04)):
            m = self.mmc(1, 1, ks)
            keys = cv.value(m).json['curve']
            self.assertLessEqual(len(keys), 16)
            c = cv.Curve(m)
            lo, hi = c.bounds()
            for i in range(1001):
                t = i / 1000.0
                self.assertLess(abs(c.hi(t) - lin(keys, t)), tol * (hi - lo))

    def test_weighted_tangents(self):
        k = uy.curve_keys({'m_Curve': [
            {'time': 0, 'value': 0, 'inSlope': 0, 'outSlope': 0, 'weightedMode': 2, 'outWeight': 1.0},
            {'time': 1, 'value': 1, 'inSlope': 0, 'outSlope': 0, 'weightedMode': 0}]})
        plain = uy.curve_keys({'m_Curve': [{'time': 0, 'value': 0, 'inSlope': 0, 'outSlope': 0}, {'time': 1, 'value': 1, 'inSlope': 0, 'outSlope': 0}]})
        self.assertLess(uy.curve_eval(k, 0.3), uy.curve_eval(plain, 0.3))   # a heavier out weight holds the start longer
        self.assertAlmostEqual(uy.curve_eval(k, 1.0), 1.0)
        third = uy.curve_keys({'m_Curve': [dict(time=0, value=0, inSlope=1, outSlope=1, weightedMode=3, inWeight=1 / 3., outWeight=1 / 3.),
                                           dict(time=1, value=1, inSlope=1, outSlope=1, weightedMode=3, inWeight=1 / 3., outWeight=1 / 3.)]})
        self.assertAlmostEqual(uy.curve_eval(third, 0.4), 0.4, places=4)   # weights of 1/3 = the Hermite curve

    def test_rotated_axes_combine_exactly(self):
        # a Unity local x curve under the frame change lands on the engine z axis unchanged
        curves = [cv.Curve(self.mmc(1, 1, [(0, 0, 1, 1), (1, 1, 1, 1)])), cv.Curve(0.0), cv.Curve(0.0)]
        out = [cv.combine([(c, cv.P[i][j]) for j, c in enumerate(curves)]).json for i in range(3)]
        self.assertEqual(out, [0, 0, {'curve': [[0, 0], [1, 1]]}])

    def test_euler_and_frames(self):
        for e in ((10, 20, 30), (-80, 45, 170), (0, -90, 0)):
            m = cv.mat_zxy(e)
            back = cv.euler_zxy(m)
            for a, b in zip(cv.mat_zxy(back), m):
                for x, y in zip(a, b):
                    self.assertAlmostEqual(x, y, places=6)
        q = us.q_euler((30, 40, 50))
        self.assertEqual([round(x, 6) for x in cv.mv(cv.q_mat(q), (1, 2, 3))], [round(x, 6) for x in us.q_rot(q, (1, 2, 3))])
        self.assertEqual(cv.mv(cv.P, (1, 2, 3)), us.u2rel((1, 2, 3)))

    def test_prefab_modification_paths(self):
        data = {'InitialModule': {'startSize': {'scalar': 1}}, 'EmissionModule': {'m_Bursts': [{'time': 0}]}}
        self.assertTrue(us.set_property(data, 'InitialModule.startSize.scalar', 2))
        self.assertTrue(us.set_property(data, 'EmissionModule.m_Bursts.Array.size', 3))
        self.assertTrue(us.set_property(data, 'EmissionModule.m_Bursts.Array.data[2].time', 0.5))
        self.assertEqual(data['InitialModule']['startSize']['scalar'], 2)
        self.assertEqual([b['time'] for b in data['EmissionModule']['m_Bursts']], [0, 0, 0.5])
        self.assertTrue(us.set_property(data, 'EmissionModule.m_Bursts.Array.size', 1))
        self.assertEqual(len(data['EmissionModule']['m_Bursts']), 1)
        self.assertEqual(uy.scalar('0000000000000000e000000000000000'), '0000000000000000e000000000000000')


def lin(keys, t):
    for a, b in zip(keys, keys[1:]):
        if t <= b[0]:
            f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0
            return a[1] + (b[1] - a[1]) * f
    return keys[-1][1]


def corners(m, dims):
    out = set()
    for sx in (-0.5, 0.5):
        for sy in (-0.5, 0.5):
            for sz in (-0.5, 0.5):
                p = cv.mv(m, (sx * dims[0], sy * dims[1], sz * dims[2]))
                out.add(tuple(round(x, 3) + 0.0 for x in p))
    return out


class ModuleTests(unittest.TestCase):
    """tests/data/unity_vfx/Assets/Modules: every ParticleSystem module, nested prefabs, force fields, meshes, sprites."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        cls.code, cls.rep = run_import('--source', MODULES, '--game', cls.tmp)
        cls.fx = {}
        for e in cls.rep['effects']:
            with open(os.path.join(cls.tmp, e['file'])) as f:
                cls.fx[e['name']] = json.load(f)
        cls.storm = by_comment(cls.fx['storm'], 'Storm')

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def em(self, suffix):
        return by_comment(self.fx['storm'], suffix)

    def test_effects(self):
        self.assertEqual(self.code, 0)
        self.assertEqual(sorted(self.fx), ['base', 'basevariant', 'nested', 'storm', 'storm_sparkle'])
        self.assertEqual(len(self.fx['storm']['emitters']), 7)

    def test_main_module(self):
        e = self.storm
        self.assertTrue(e['prewarm'])
        self.assertEqual(e['delay'], [0.1, 0.5])
        self.assertEqual(e['life'], {'curve': [[0, 1], [1, 2]]})
        self.assertEqual(e['speed'], [-1, 2])                       # crosses zero: passed through
        self.assertEqual(e['size']['curveMin'], [[0, 0.2], [1, 0.4]])
        self.assertEqual((e['size']['curve'][0], e['size']['curve'][-1]), ([0, 0.5], [1, 1]))
        self.assertEqual((e['sizeY'], e['sizeZ']), (0.5, 0.25))
        self.assertEqual(e['rotation'], [-90, 0])                    # billboards: Unity's z rotation mirrored
        self.assertEqual(e['rotation3D'], {'x': -30, 'y': 0})
        self.assertEqual(e['startColor'], {'gradient': [[0, '#ff3333ff'], [1, '#3333ff80']], 'overTime': True})
        self.assertEqual(e['gravity'], [0, -9.81, 0])
        self.assertEqual(e['gravityScale'], {'curve': [[0, 0], [1, 1]]})
        self.assertEqual((e['maxParticles'], e['ringBuffer']), (200, True))
        ring = self.em('/Ring')['startColor']
        self.assertEqual(ring['gradientMin'], [[0, '#ff0000ff'], [1, '#00ff00ff']])
        self.assertTrue(ring['overTime'])

    def test_emission(self):
        e = self.storm
        self.assertEqual(e['bursts'], [{'time': 0.5, 'count': [5, 10], 'cycles': 0, 'interval': 0.5, 'chance': 0.5}])
        rate = e['rate']['curve']
        self.assertEqual((rate[0], rate[-1]), ([0, 40], [1, 40]))
        self.assertIn([0.499, 10], rate)                             # the step key stays sharp
        self.assertLessEqual(len(rate), 16)

    def test_shapes(self):
        e = self.storm
        self.assertEqual((e['shape'], e['emitFrom'], e['length'], e['spread'], e['thickness']), ('cone', 'volume', 2, 20, 0.5))
        self.assertNotIn('direction', e)
        self.assertEqual((e['arc'], e['arcMode'], e['arcSpeed'], e['arcSpread']), (270, 'loop', 2, 0.25))
        # arc < 360: the whole shape turns; the engine arc (from +X towards +Z around +Y) covers Unity's sector
        R = cv.mat_zxy(e['shapeRotation'])
        M = cv.mm(cv.P, cv.q_mat(us.q_euler((-90, 0, 0))))
        rnd = lambda v: [round(x, 4) + 0.0 for x in v]  # noqa: E731
        self.assertEqual(rnd(cv.mv(R, (0, 1, 0))), rnd(cv.mv(M, (0, 0, 1))))   # axis
        for k in range(0, 271, 30):
            a = math.radians(k)
            eng = cv.mv(R, (math.cos(a), 0, math.sin(a)))
            u = math.radians(270 - k)
            self.assertEqual(rnd(eng), rnd(cv.mv(M, (math.cos(u), math.sin(u), 0))))
        self.assertEqual((e['randomDirection'], e['sphericalDirection'], e['randomPosition'], e['alignToDirection']), (0.25, 0.5, 0.1, True))
        ring = self.em('/Ring')
        self.assertEqual((ring['shape'], ring['radius'], ring['donutRadius'], ring['shapeRotation']), ('donut', 1, 0.1, [0, 0, 135]))
        shards = self.em('/Shards')
        self.assertEqual((shards['shape'], shards['mesh'], shards['emitFrom']), ('mesh', 'builtin:cube', 'triangle'))
        self.assertEqual(shards['shapeRotation'], [0, 30, 0])        # built-in mesh: only the shape's own rotation
        hull = self.em('/Hull')
        self.assertEqual((hull['mesh'], hull['emitFrom']), ('models/shard.obj', 'edge'))
        self.assertEqual(hull['box'], [2, 2, 3])                     # the mesh shape's scale (model axes)
        self.assertEqual(hull['shapeRotation'], [0, 180, 0])         # model file: Unity's import mirror = a half turn
        self.assertTrue(os.path.exists(os.path.join(self.tmp, 'models', 'shard.obj')))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, 'models', 'rock.fbx')))
        panel = self.em('/Panel')
        self.assertEqual((panel['shape'], panel['emitFrom']), ('box', 'edge'))
        unity = cv.mm(cv.P, cv.q_mat(us.q_euler((0, 45, 0))))       # the box placed by Unity, in the effect frame
        self.assertEqual(corners(cv.mat_zxy(panel['shapeRotation']), panel['box']), corners(unity, (2, 1, 0.5)))
        d = cv.mv(cv.mat_zxy(panel['shapeRotation']), panel['direction'])
        self.assertEqual([round(x, 3) + 0.0 for x in d], [round(x, 3) + 0.0 for x in cv.mv(unity, (0, 0, 1))])
        badge = self.em('/Badge')
        self.assertEqual((badge['shape'], badge['box']), ('box', [0, 1, 2]))   # 16 px sprite at 16 px/unit, x scale 2, z = 0
        funnel = self.em('/Funnel')
        self.assertEqual(funnel['shape'], 'line')
        self.assertEqual([round(x, 3) + 0.0 for x in cv.mv(cv.mat_zxy(funnel['shapeRotation']), (1, 0, 0))], [0, 1, 0])

    def test_velocity_forces_limits(self):
        e = self.storm
        vo = e['velocityOverLife']
        self.assertEqual(vo['z'], {'curve': [[0, 0], [1, 1]]})    # Unity local x = effect z
        self.assertEqual(vo['y'], 1)
        self.assertEqual(vo['orbitalY'], -57.296)                  # 1 rad/s around Unity y, mirrored
        self.assertEqual(vo['orbitalOffset'], [0, 0, 0.5])
        self.assertEqual(vo['radial'], 0.2)
        self.assertEqual(vo['speedModifier'], {'curve': [[0, 1], [1, 0.5]]})
        self.assertEqual(e['force'], {'y': [1, 2], 'space': 'world', 'randomize': True})
        self.assertEqual(e['speedLimitAxes'], {'x': 3, 'y': 5, 'z': 2, 'space': 'local'})
        self.assertEqual((e['limitDampen'], e['drag'], e['dragBySize']), (0.5, {'curve': [[0, 0], [1, 2]]}, True))
        self.assertNotIn('dragBySpeed', e)
        self.assertEqual(e['inheritVelocity'], {'mode': 'current', 'amount': 0.5})
        self.assertEqual(e['lifeBySpeed'], {'speed': [0, 10], 'factor': [[0, 1], [1, 0.3]]})
        self.assertEqual(e['spinBySpeed'], {'speed': [0, 5], 'spin': [[0, -180], [1, -180]]})
        self.assertEqual(e['externalForces'], 1.5)
        ff = self.fx['storm']['forceFields']
        self.assertEqual(ff, [{'position': [0, 2, 0], 'shape': 'cylinder', 'length': 4, 'radius': 3, 'startRadius': 0.5, 'direction': [0, 1, 0],
                               'gravity': 2, 'gravityFocus': 0.3, 'rotation': -57.296, 'rotationAttraction': 0.5, 'drag': 0.3}])

    def test_over_lifetime_and_noise(self):
        e = self.storm
        self.assertEqual(e['color'], [[0, '#ffcc66ff'], [1, '#ff330000']])
        self.assertEqual(e['colorMin'], [[0, '#ffffffff'], [1, '#80808000']])
        self.assertEqual(e['sizeOverLifeMin'], [[0, 0.5], [1, 0.5]])
        self.assertEqual(e['sizeOverLifeY'], [[0, 1], [1, 0]])
        self.assertEqual(e['spin'], {'curve': [[0, -180], [1, 0]]})
        self.assertEqual(e['spin3D'], {'x': -90, 'y': 0})
        n = e['noise']
        self.assertEqual(n['strength'], {'curve': [[0, 1], [1, 0]]})
        self.assertEqual(n['axes'], [1, 0.5, 1])
        self.assertEqual((n['octaves'], n['octaveMultiplier'], n['octaveScale'], n['damping']), (3, 0.4, 2.5, True))
        self.assertEqual((n['position'], n['rotation'], n['size']), (0.8, -45, 0.2))
        self.assertEqual((n['remap'][0], n['remap'][-1]), ([0, -1], [1, 1]))
        shards = self.em('/Shards')
        # mesh particles from model files: engine world Euler (-x, -y, z) then the import half turn R Ry(180) =
        # Ry(y + 180) Rx(-x) Rz(-z)
        self.assertEqual(shards['rotation'], -45)
        self.assertEqual(shards['rotation3D'], {'x': [0, 180], 'y': 180})
        self.assertEqual(shards['spin'], -114.59)
        self.assertEqual(shards['spin3D'], {'x': 57.296, 'y': [-28.648, 28.648]})
        for x, y, z in ((30, 0, 45), (100, 20, -60)):   # the identity behind it
            a = cv.mm(cv.mat_zxy((x, y, z)), cv.Y180)
            b = cv.mat_zxy((-x, y + 180, -z))
            self.assertTrue(all(abs(a[i][j] - b[i][j]) < 1e-9 for i in range(3) for j in range(3)))

    def test_texture_sheet(self):
        e = self.storm
        self.assertEqual((e['frames'], e['row'], e['cycles'], e['flip']), ([4, 4], 'random', 2, [0.5, 0]))
        fot = e['frameOverTime']['curve']
        self.assertEqual((fot[0], fot[-1]), ([0, 0], [1, 0.9999]))
        ring = self.em('/Ring')
        self.assertEqual(ring['sprites'], ['fx/textures/sheet_star.png', 'fx/textures/sheet_disc.png', 'fx/textures/dot.png'])
        self.assertEqual((ring['fps'], ring['flip']), (10, [0, 0.5]))
        with open(os.path.join(self.tmp, 'fx', 'textures', 'sheet_disc.png'), 'rb') as f:
            disc = images.read_png(f.read())
        self.assertEqual((disc.w, disc.h), (32, 32))
        self.assertGreater(disc.rgba[(16 * 32 + 16) * 4 + 1], 200)     # the green disc, not the red star
        panel = self.em('/Panel')
        self.assertEqual((panel['framesBySpeed'], panel['startFrame']), ([0.5, 4], {'curve': [[0, 0], [1, 3]]}))

    def test_renderer_and_materials(self):
        e = self.storm
        self.assertEqual((e['align'], e['pivot'], e['sort'], e['sortOrder']), ('world', [0, 0.5, -0.25], 'youngest', -5))
        self.assertEqual((e['minScreenSize'], e['maxScreenSize']), (0.01, 0.8))
        shards = self.em('/Shards')
        self.assertEqual((shards['meshes'], shards['blend']), (['models/shard.obj', 'models/rock.fbx'], 'opaque'))
        self.assertEqual(self.em('/Ring')['blend'], 'multiply')
        self.assertEqual(self.em('/Hull')['blend'], 'subtract')
        funnel = self.em('/Funnel')
        self.assertEqual((funnel['align'], funnel['cameraStretch'], funnel['sortOrder']), ('velocity', 0.3, 3))

    def test_collision_trigger_subemitters(self):
        e = self.storm
        c = e['collision']
        self.assertEqual((c['ground'], c['planes'], c['radius'], c['maxKillSpeed']), (False, [{'point': [0, -0.5, 0], 'normal': [0, 1, 0]}], 0.8, 50))
        t = e['trigger']
        self.assertEqual(t['volumes'][0], {'center': [0, 0.5, 2], 'radius': 0.75})
        self.assertEqual((t['inside'], t['enter'], t['radius']), ('kill', 'callback', 1))
        self.assertNotIn('outside', t)
        self.assertEqual(e['subEmitters'], [{'on': 'trigger', 'effect': 'storm_sparkle', 'inherit': ['color', 'size']},
                                            {'on': 'manual', 'effect': 'storm_sparkle', 'chance': 0.25, 'inherit': ['rotation', 'lifetime', 'duration']}])
        self.assertTrue(self.em('/Funnel')['collision'].get('ground', True))

    def test_trails_and_lights(self):
        t = self.storm['trail']
        self.assertEqual((t['mode'], t['ribbons'], t['textureMode'], t['dieWithParticles'], t['sizeAffectsLifetime']), ('ribbon', 2, 'tile', False, True))
        self.assertEqual(t['lifeColor'], [[0, '#ffffffff'], [1, '#3366ff00']])
        self.assertEqual(t['minDistance'], 0.1)
        st = self.em('/Shards')['trail']
        self.assertEqual((st['lifeFraction'], st['ratio'], st['textureMode']), (0.3, 0.5, 'distribute'))
        light = self.storm['light']
        self.assertEqual((light['color'], light['radius'], light['intensity']), ('#66ccff', 4, 3))   # template from another prefab
        self.assertEqual(light['radiusOverLife'], [[0, 1], [1, 0]])

    def test_nested_prefabs_and_variants(self):
        n = self.fx['nested']['emitters']
        self.assertEqual([e['comment'] for e in n], ['Unity: Holder/Mount/Base', 'Unity: Holder/Mount/Base/Glow2', 'Unity: Holder/Mount/Base/Extra'])
        base = n[0]
        self.assertEqual((base['size'], base['bursts'], base['offset']), (1.5, [[0.25, 6], [0.5, 3]], [0, 1, 2]))
        self.assertEqual(n[1]['blend'], 'multiply')                  # objectReference override (material)
        self.assertEqual(n[2]['offset'], [-1, 1, 2])                 # added child under the nested root
        self.assertEqual(self.fx['basevariant']['emitters'][0]['life'], 3)
        self.assertIn('Nested.prefab: nested prefab Base.prefab: 2 system(s), 8 modification(s) applied', self.rep['notes'])
        self.assertFalse(any('Nested.prefab: script' in d for d in self.rep['dropped']))   # removed component

    def test_report(self):
        dropped = '\n'.join(self.rep['dropped'])
        approx = '\n'.join(self.rep['approximate'])
        self.assertIn('customData', dropped)
        self.assertIn('custom vertex streams', dropped)
        self.assertIn('custom simulation space', dropped)
        self.assertIn('collider outside the prefab', dropped)
        self.assertIn('box collider -> its bounding sphere', approx)
        self.assertNotIn('limits', approx)
        self.assertNotIn('SphereCollider', dropped)                 # used by the trigger module


if __name__ == '__main__':
    unittest.main(verbosity=1)

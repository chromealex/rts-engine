#!/usr/bin/env python3
"""Unit tests of the Unity effect importer (stdlib unittest). Run: python3 tools/importers/unity_vfx/test_import.py
Test material: tests/data/unity_vfx (synthetic prefabs, materials, .meta and generated textures)."""
import io
import json
import os
import shutil
import sys
import tarfile
import tempfile
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import images  # noqa: E402
import unity_yaml as uy  # noqa: E402
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location('unity_vfx_import', os.path.join(HERE, 'import.py'))
imp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(imp)

DATA = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'tests', 'data', 'unity_vfx', 'Assets', 'FX'))


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
        self.assertEqual(smoke['gravity'], [0.0, 0.8, 0.0])  # local force +Z under the rotated root
        self.assertEqual(smoke['delay'], 0.1)

    def test_renderer_modes_trails_lights_collision_subemitters(self):
        sp = by_comment(self.fx['explosion'], '/Sparks')
        self.assertEqual(sp['align'], 'velocity')
        self.assertEqual(sp['stretch'], 0.05)
        self.assertEqual(sp['shape'], 'hemisphere')
        self.assertEqual(sp['trail']['blend'], 'additive')        # custom shader "Blend SrcAlpha One"
        self.assertEqual(sp['trail']['width'], [[0.0, 1.0], [1.0, 0.0]])
        self.assertEqual(sp['light']['color'], '#ff9940')
        self.assertEqual(sp['light']['max'], 6)
        self.assertEqual(sp['collision']['height'], -0.1)
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
        self.assertEqual(aura['velocity'], [0.0, 1.0, 0.0])
        self.assertNotIn('direction', aura)                  # circle rotated -90 X: its normal is up
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
        self.assertIn('orbital', dropped)
        self.assertIn('prewarm', dropped)
        self.assertIn('Shader Graph Glow.shadergraph', approx)
        self.assertIn('custom shader TrailGlow.shader', approx)
        self.assertIn('mesh particles', approx)
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


if __name__ == '__main__':
    unittest.main(verbosity=1)

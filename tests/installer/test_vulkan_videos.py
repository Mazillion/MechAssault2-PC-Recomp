"""1.1.3: the enhanced cutscenes through Vulkan (realesrgan-ncnn-vulkan) on graphics cards without CUDA.
The pipeline with synthetic frames and a stand-in upscaler (no GPU needed), the engine wiring in the FMV scripts, the installer's
videos step. Run with the kit's Python:  python -B test_vulkan_videos.py
"""
import inspect, json, os, shutil, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

import numpy as np
from PIL import Image

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
FMV = KIT / 'enhancer' / 'FMV'
sys.path.insert(0, str(KIT / 'installer')); sys.path.insert(0, str(FMV))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))
import install                                                                # noqa: E402
import fmv_pipeline                                                           # noqa: E402
import video_engine_vulkan as vev                                             # noqa: E402


class FakeUpscaler:
    """Stands in for upscale_vulkan.VulkanUpscaler(scale=2): a 2x picture per frame, batches counted."""
    def __init__(self): self.batches = []; self.closed = False
    def prefetch(self, requests): self.batches.append(len(requests)); return len(requests)
    def rgb(self, image, edge_mode='reflect'):
        return image.resize((image.width * 2, image.height * 2), Image.Resampling.NEAREST)
    def close(self): self.closed = True


class VulkanVideos(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-vid-', dir=str(WORK / 'tmp')))
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def frames(self, n=10, size=(24, 16)):
        folder = self.tmp / 'originals'; folder.mkdir()
        rng = np.random.default_rng(1)
        for i in range(n):
            fmv_pipeline.save_png(fmv_pipeline.frame_path(folder, i), Image.fromarray(rng.integers(0, 255, (size[1], size[0], 3), dtype=np.uint8), 'RGB'))
        return folder

    def test_blend_matches_the_realbasicvsr_formula(self):
        original = Image.fromarray(np.full((8, 8, 3), 100, np.uint8), 'RGB'); ai = Image.fromarray(np.full((16, 16, 3), 200, np.uint8), 'RGB')
        self.assertEqual(np.asarray(vev.blend(ai, original, 0.5))[0, 0, 0], 150)
        self.assertTrue((np.asarray(vev.blend(ai, original, 1.0)) == 200).all())
        self.assertTrue((np.asarray(vev.blend(ai, original, 0.0)) == np.asarray(original.resize((16, 16), Image.Resampling.LANCZOS))).all())

    def test_synthetic_clip_through_the_pipeline(self):
        originals = self.frames(10)
        fake = FakeUpscaler()
        engine = vev.VulkanVideoEnhancer(None, 0.5, 2, upscaler=fake)
        chunks = fmv_pipeline.plan_chunks(10, 0, 10, 4, vev.CONTEXT, [])
        out = self.tmp / 'enhanced'
        res = fmv_pipeline.enhance_chunks(originals, out, chunks, 'sig', (48, 32), lambda: engine)
        self.assertEqual(res['new_frames'], 10); self.assertEqual(sum(fake.batches), 10)     # every frame once, one tool start per batch
        for i in range(10):
            with Image.open(fmv_pipeline.frame_path(out, i)) as im: self.assertEqual((im.mode, im.size), ('RGB', (48, 32)))
        res = fmv_pipeline.enhance_chunks(originals, out, chunks, 'sig', (48, 32), lambda: self.fail('resumed work needs no upscaler'))
        self.assertEqual((res['reused_frames'], res['new_frames']), (10, 0))                  # a rerun resumes: nothing redone
        with self.assertRaises(ValueError): vev.VulkanVideoEnhancer(None, 0.5, 4, upscaler=fake)

    def test_fmv_scripts_take_the_engine(self):
        intro = (FMV / 'enhance_intro.py').read_text(encoding='utf-8'); every = (FMV / 'enhance_all_fmvs.py').read_text(encoding='utf-8')
        for text in (intro, every):
            self.assertIn('choices=["pytorch", "vulkan"], default="pytorch"', text); self.assertIn('model_sha256(args.vulkan)', text)
        self.assertIn('"fp16-ncnn" if vulkan else "float32"', intro)               # a Vulkan job never matches a RealBasicVSR one
        self.assertIn('VulkanVideoEnhancer(args.vulkan', intro)
        self.assertIn('if not vulkan and digest(checkpoint) != CHECKPOINT_SHA256', intro)   # no RealBasicVSR model needed
        self.assertNotIn('import torch', (FMV / 'video_engine_vulkan.py').read_text(encoding='utf-8'))
        import enhance_intro                                                         # NVIDIA resume of earlier 2x jobs is unchanged
        self.assertEqual(enhance_intro.inference_code_hash(FMV, 2), json.loads((FMV / 'resume_compatibility.json').read_text())['previous_2x_code_sha256'])

    def installer(self, engine):
        d = self.tmp / ('game-' + engine); movie = d / 'game_files' / 'Movie' / '60' / 'English'; movie.mkdir(parents=True)
        (movie / 'SPIntro.bik').write_bytes(b'x')
        enh = self.tmp / ('enh-' + engine); (enh / 'FMV').mkdir(parents=True); (enh / 'FMV' / 'config.json').write_text('{}')
        inst = install.Installer(None, d, videos=True, log=lambda s: None); inst.work.mkdir(parents=True); inst.vid_engine = engine
        calls, setups = [], []
        inst.enhancer = lambda e='pytorch': (setups.append(e), (enh, Path('py.exe')))[1]
        inst.vulkan_dir = lambda: Path('VK')
        inst.run_cmd = lambda args, **k: calls.append([str(a) for a in args])
        inst.install_enhancer_licenses = lambda videos=False, vulkan=False: calls.append(['licences', str(vulkan)])
        inst.videos()
        return calls, setups

    def test_installer_videos_step(self):
        calls, setups = self.installer('vulkan')
        self.assertEqual(setups, ['vulkan'])                                        # NumPy/Pillow only
        self.assertFalse(any('setup_fmv.py' in ' '.join(c) for c in calls))         # no RealBasicVSR model download
        run = [c for c in calls if any('enhance_all_fmvs.py' in a for a in c)][0]
        self.assertEqual(run[-4:], ['--engine', 'vulkan', '--vulkan', 'VK']); self.assertEqual(calls[-1], ['licences', 'True'])
        calls, setups = self.installer('pytorch')
        self.assertEqual(setups, ['pytorch']); self.assertTrue(any('setup_fmv.py' in ' '.join(c) for c in calls))
        self.assertNotIn('--engine', [a for c in calls for a in c])
        self.assertIn("os.environ.get('MA2_VIDEO_ENGINE')", inspect.getsource(install.Installer.check))


if __name__ == '__main__':
    unittest.main(verbosity=2)

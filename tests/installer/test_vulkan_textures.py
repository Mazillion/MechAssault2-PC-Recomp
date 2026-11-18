"""1.1.2: the enhanced textures on any graphics card with Vulkan (realesrgan-ncnn-vulkan), NVIDIA keeps PyTorch.
Run with the kit's Python:  python -B test_vulkan_textures.py
Optional: MA2_TEST_UPSCALER_DIR = a folder with realesrgan-ncnn-vulkan.exe and models/ (runs the real tool on test images);
MA2_TEST_UPSCALER_ZIP = realesrgan-ncnn-vulkan-20220424-windows.zip (checks the pinned hashes against the real archive).
"""
import hashlib, io, json, os, shutil, subprocess, sys, tempfile, unittest, zipfile
from pathlib import Path

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
import precheck, install, upscaler_fetch                                     # noqa: E402

G = precheck.GB
AMD = [{'name': 'AMD Radeon RX 7800 XT', 'vram': 16 * G, 'vendor': 'amd'}]
INTEL = [{'name': 'Intel(R) Iris(R) Xe Graphics', 'vram': 1 * G, 'vendor': 'intel'}]


class EngineChoice(unittest.TestCase):
    def test_texture_engine(self):
        self.assertEqual(install.texture_engine('NVIDIA GeForce RTX 3090, 24576 MiB', []), 'pytorch')       # NVIDIA: PyTorch, as before
        self.assertEqual(install.texture_engine(None, AMD), 'vulkan')
        self.assertEqual(install.texture_engine(None, INTEL), 'vulkan')
        self.assertIsNone(install.texture_engine(None, []))                                            # no card: refused
        self.assertEqual(install.texture_engine(None, [], allow_cpu=True), 'pytorch')                  # the hidden CPU mode stays
        self.assertEqual(install.texture_engine('NVIDIA GeForce RTX 3090', [], force='vulkan'), 'vulkan')
        self.assertEqual(install.texture_engine(None, AMD, force='PyTorch'), 'pytorch')
        self.assertEqual(install.texture_engine(None, AMD, force='nonsense'), 'vulkan')

    def test_recommend(self):
        for gpus in (AMD, INTEL):
            r = precheck.recommend_textures(gpus)
            self.assertTrue(r['can']); self.assertFalse(r['videos']); self.assertEqual((r['rec'], r['engine']), ('original', 'vulkan'))
            self.assertIn('through Vulkan (new in 1.1.2; not yet tested on AMD or Intel cards)', r['reason'])
        r = precheck.recommend_textures([{'name': 'RTX 4070', 'vram': 12 * G, 'vendor': 'nvidia'}])
        self.assertEqual((r['rec'], r['can']), ('4x', True)); self.assertNotIn('Vulkan', r['reason']); self.assertTrue(r.get('videos', r['can']))
        r = precheck.recommend_textures([{'name': 'GTX 1650', 'vram': 4 * G, 'vendor': 'nvidia'}])
        self.assertEqual((r['rec'], r['can']), ('original', False))                                    # unchanged
        r = precheck.recommend_textures([]); self.assertFalse(r['can']); self.assertIn('NVIDIA', r['reason'])

    def test_needed_space(self):
        o = {'textures': ['2x'], 'videos': False}
        self.assertEqual(install.needed_space(o, set()), install.needed_space(o, set(), 'pytorch'))    # default unchanged
        self.assertEqual(install.needed_space(o, set()) - install.needed_space(o, set(), 'vulkan'), 5.5 * G)
        v = {'textures': ['2x'], 'videos': True}
        self.assertEqual(install.needed_space(v, set(), 'vulkan'), install.needed_space(v, set()))     # videos need PyTorch anyway


class Check(unittest.TestCase):
    """Installer.check() picks the engine from the graphics card (no compiler, no disc: an installed game, add-ons)."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-vk-', dir=str(WORK / 'tmp')))
        self.saved = install.nvidia_gpu, precheck.detect_gpus, os.environ.pop('MA2_TEXTURE_ENGINE', None)
    def tearDown(self):
        install.nvidia_gpu, precheck.detect_gpus, forced = self.saved
        os.environ.pop('MA2_TEXTURE_ENGINE', None)
        if forced is not None: os.environ['MA2_TEXTURE_ENGINE'] = forced
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_check(self, nvidia, gpus, textures=('2x',), videos=False, allow_cpu=False, force=None):
        install.nvidia_gpu = lambda: nvidia
        precheck.detect_gpus = lambda: list(gpus)
        if force: os.environ['MA2_TEXTURE_ENGINE'] = force
        lines = []
        inst = install.Installer(None, self.tmp, textures=textures, videos=videos, allow_cpu=allow_cpu, log=lines.append)
        inst.game_installed = lambda: True; inst.need_disc = lambda: False
        inst.check()
        return inst, '\n'.join(lines)

    def test_nvidia_keeps_pytorch(self):
        inst, log = self.run_check('NVIDIA GeForce RTX 3090, 24576 MiB', [], videos=True)
        self.assertEqual(inst.tex_engine, 'pytorch'); self.assertNotIn('Vulkan', log); self.assertIn('GPU: NVIDIA GeForce RTX 3090', log)

    def test_amd_intel_use_vulkan(self):
        for gpus in (AMD, INTEL):
            inst, log = self.run_check(None, gpus)
            self.assertEqual(inst.tex_engine, 'vulkan')
            self.assertIn('Textures: through Vulkan on %s (realesrgan-ncnn-vulkan; new in 1.1.2; not yet tested on AMD or Intel cards)' % gpus[0]['name'], log)

    def test_no_card_is_refused_unless_cpu_mode(self):
        with self.assertRaises(install.Stop) as cm: self.run_check(None, [])
        self.assertIn('need a graphics card (an NVIDIA GPU with CUDA, or any graphics card with Vulkan)', str(cm.exception))
        inst, log = self.run_check(None, [], allow_cpu=True)
        self.assertEqual(inst.tex_engine, 'pytorch'); self.assertIn('runs on the CPU', log)

    def test_videos_still_need_nvidia(self):
        with self.assertRaises(install.Stop) as cm: self.run_check(None, AMD, textures=(), videos=True)
        self.assertIn('cutscene videos need an NVIDIA GPU', str(cm.exception))
        with self.assertRaises(install.Stop): self.run_check(None, AMD, textures=('2x',), videos=True)
        inst, log = self.run_check(None, AMD, textures=('2x',), videos=True, allow_cpu=True)          # CPU mode: videos on the CPU, textures on Vulkan
        self.assertEqual(inst.tex_engine, 'vulkan'); self.assertIn('runs on the CPU', log)

    def test_forced_engine(self):
        inst, log = self.run_check('NVIDIA GeForce RTX 3090, 24576 MiB', [], force='vulkan')
        self.assertEqual(inst.tex_engine, 'vulkan'); self.assertIn('through Vulkan on NVIDIA GeForce RTX 3090', log)


class TexturesStep(unittest.TestCase):
    """Installer.textures(): the Vulkan path runs enhance.py with --engine vulkan and installs its licence notice; PyTorch as before."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-vkt-', dir=str(WORK / 'tmp')))
        self.forced = os.environ.pop('MA2_TEXTURE_ENGINE', None)
    def tearDown(self):
        if self.forced is not None: os.environ['MA2_TEXTURE_ENGINE'] = self.forced
        shutil.rmtree(self.tmp, ignore_errors=True)

    def make(self, name, engine, enhancer_fails=False):
        dest = self.tmp / name; dest.mkdir()
        inst = install.Installer(None, dest, textures=('2x',), log=lambda s: None)
        inst.tex_engine = engine; inst.state['textures_extracted'] = True
        enh = inst.work / 'enhancer'; enh.mkdir(parents=True)
        calls, setups = [], []
        def enhancer(engine='pytorch'):
            setups.append(engine)
            if enhancer_fails and engine == 'pytorch':
                raise install.Stop('Enhancer setup failed (exit code 1). PyTorch does not support this graphics card yet', 'gpu')
            return enh, Path(sys.executable)
        def run_cmd(args, **kw):
            calls.append([str(a) for a in args])
            pack = enh / 'packs' / '2x'; (pack / 'textures').mkdir(parents=True, exist_ok=True)
            (pack / 'textures' / 'a.dds').write_bytes(b'DDS '); (pack / 'index.json').write_text('{}')
        inst.enhancer = enhancer; inst.run_cmd = run_cmd; inst.vulkan_dir = lambda: self.tmp / 'tool'
        return inst, dest, calls, setups

    def test_vulkan_path(self):
        inst, dest, calls, setups = self.make('vk', 'vulkan')
        inst.textures('2x')
        self.assertEqual(setups, ['vulkan'])
        self.assertEqual(calls[-1][-7:], ['--scale', '2', '--no-preview', '--engine', 'vulkan', '--vulkan', str(self.tmp / 'tool')])
        self.assertTrue((dest / 'game_files/texture_packs/2x/index.json').is_file())
        lic = dest / 'Licenses' / 'Enhancer'
        self.assertTrue((lic / install.VULKAN_NOTICE).is_file()); self.assertTrue((lic / 'Real-ESRGAN.txt').is_file())
        self.assertIn('MIT License', (lic / install.VULKAN_NOTICE).read_text(encoding='utf-8'))
        # an update rebuilds Licenses: the notice stays because textures were made through Vulkan
        inst.install_enhancer_licenses(videos=False, vulkan=False)
        self.assertTrue((lic / install.VULKAN_NOTICE).is_file())

    def test_pytorch_path_unchanged(self):
        inst, dest, calls, setups = self.make('pt', 'pytorch')
        inst.textures('2x')
        self.assertEqual(setups, ['pytorch']); self.assertNotIn('--engine', calls[-1]); self.assertEqual(calls[-1][-3:], ['--scale', '2', '--no-preview'])
        lic = dest / 'Licenses' / 'Enhancer'
        self.assertTrue((lic / 'Real-ESRGAN.txt').is_file()); self.assertFalse((lic / install.VULKAN_NOTICE).exists())

    def test_pytorch_unusable_falls_back_to_vulkan(self):
        inst, dest, calls, setups = self.make('fb', 'pytorch', enhancer_fails=True)
        inst.textures('2x')
        self.assertEqual(setups, ['pytorch', 'vulkan']); self.assertEqual(inst.tex_engine, 'vulkan'); self.assertIn('--engine', calls[-1])

    def test_forced_pytorch_does_not_fall_back(self):
        os.environ['MA2_TEXTURE_ENGINE'] = 'pytorch'
        try:
            inst, dest, calls, setups = self.make('nofb', 'pytorch', enhancer_fails=True)
            with self.assertRaises(install.Stop) as cm: inst.textures('2x')
            self.assertEqual(cm.exception.kind, 'gpu')
        finally:
            os.environ.pop('MA2_TEXTURE_ENGINE', None)

    def test_enhancer_setup_modes(self):
        """enhancer(): 'vulkan' runs setup.py --engine vulkan once; a PyTorch need afterwards runs the full setup."""
        dest = self.tmp / 'su'; dest.mkdir()
        inst = install.Installer(None, dest, log=lambda s: None); inst.epy = sys.executable
        enh = inst.work / 'enhancer'; enh.mkdir(parents=True); (enh / 'setup.py').write_text('')
        venv = enh / '.venv' / 'Scripts'; venv.mkdir(parents=True); (venv / 'python.exe').write_bytes(b'')
        calls = []; inst.run_cmd = lambda args, **kw: calls.append([str(a) for a in args])
        inst.enhancer('vulkan'); self.assertEqual(calls[-1][-2:], ['--engine', 'vulkan']); self.assertEqual(inst.state['enhancer_setup'], 'vulkan')
        inst.enhancer('vulkan'); self.assertEqual(len(calls), 1)                                        # done once
        inst.enhancer(); self.assertTrue(calls[-1][-1].endswith('setup.py')); self.assertIs(inst.state['enhancer_setup'], True)
        inst.enhancer('vulkan'); inst.enhancer(); self.assertEqual(len(calls), 2)                       # the full setup serves both


class ConsolePython(unittest.TestCase):
    """The installer window runs under pythonw.exe: the Enhancer must get the console python.exe beside it, else Windows gives each
    console child (venv, pip) a visible console window whose closing stops it with 0xC000013A."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-pyw-', dir=str(WORK / 'tmp')))
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_console_python(self):
        rt = self.tmp / 'python'; rt.mkdir()
        (rt / 'pythonw.exe').write_bytes(b''); (rt / 'python.exe').write_bytes(b'')
        self.assertEqual(install.console_python(rt / 'pythonw.exe'), str(rt / 'python.exe'))
        self.assertEqual(install.console_python(str(rt / 'PYTHONW.EXE')), str(rt / 'python.exe'))
        self.assertEqual(install.console_python(rt / 'python.exe'), str(rt / 'python.exe'))
        lone = self.tmp / 'lone'; lone.mkdir(); (lone / 'pythonw.exe').write_bytes(b'')
        self.assertEqual(install.console_python(lone / 'pythonw.exe'), str(lone / 'pythonw.exe'))   # no python.exe: unchanged

    def test_enhancer_python_is_never_pythonw(self):
        rt = self.tmp / 'python'; rt.mkdir(); (rt / 'pythonw.exe').write_bytes(b''); (rt / 'python.exe').write_bytes(b'')
        real = sys.executable
        try:
            sys.executable = str(rt / 'pythonw.exe')                                  # as under the installer window
            got = install.enhancer_python()
        finally:
            sys.executable = real
        self.assertEqual(Path(got).name.lower(), 'python.exe'); self.assertEqual(Path(got).parent, rt)
        self.assertNotEqual(Path(install.enhancer_python()).name.lower(), 'pythonw.exe')
        pyw = Path(sys.executable).with_name('pythonw.exe')
        if pyw.is_file():                                                               # this PC's real pythonw.exe
            sys.executable = str(pyw)
            try:
                self.assertEqual(Path(install.enhancer_python()).name.lower(), 'python.exe')
            finally:
                sys.executable = real

    def test_venv_creation_has_no_console(self):
        import importlib.util, types
        spec = importlib.util.spec_from_file_location('ma2_enhancer_setup_q', KIT / 'enhancer' / 'setup.py')
        mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
        self.assertIn('QuietEnvBuilder(with_pip=False', (KIT / 'enhancer' / 'setup.py').read_text(encoding='utf-8'))
        seen = {}
        real = subprocess.check_output
        subprocess.check_output = lambda args, **kw: seen.update(kw)
        try:
            ctx = types.SimpleNamespace(env_exec_cmd=sys.executable, env_dir=str(self.tmp))
            mod.QuietEnvBuilder(with_pip=True)._call_new_python(ctx, '-m', 'ensurepip')
        finally:
            subprocess.check_output = real
        self.assertTrue(seen['creationflags'] & subprocess.CREATE_NO_WINDOW and seen['creationflags'] & subprocess.CREATE_NEW_PROCESS_GROUP)
        self.assertIs(seen['stdin'], subprocess.DEVNULL)


def make_zip(path, files):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items(): z.writestr(name, data)
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


class Fetch(unittest.TestCase):
    FILES = {'realesrgan-ncnn-vulkan.exe': b'MZ tool', 'vcomp140.dll': b'MZ omp', 'models/realesrgan-x4plus.bin': b'\x00' * 999,
             'models/realesrgan-x4plus.param': b'7767517\n'}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-fetch-', dir=str(WORK / 'tmp')))
        self.real = upscaler_fetch.SPEC
        self.env = os.environ.pop('MA2_UPSCALER_SOURCE', None)
        self.good = self.tmp / 'good' / 'realesrgan-ncnn-vulkan-20220424-windows.zip'; self.good.parent.mkdir()
        archive_sha = make_zip(self.good, dict(self.FILES, **{'input.jpg': b'jpg', 'onepiece_demo.mp4': b'mp4'}))
        upscaler_fetch.SPEC = dict(self.real, archive_sha256=archive_sha, urls=['http://127.0.0.1:9/none.zip'],
                                   files={k: hashlib.sha256(v).hexdigest() for k, v in self.FILES.items()},
                                   sizes={k: len(v) for k, v in self.FILES.items()})
    def tearDown(self):
        upscaler_fetch.SPEC = self.real
        os.environ.pop('MA2_UPSCALER_SOURCE', None)
        if self.env is not None: os.environ['MA2_UPSCALER_SOURCE'] = self.env
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_pinned_spec(self):
        s = self.real
        self.assertEqual(s['archive'], 'realesrgan-ncnn-vulkan-20220424-windows.zip')
        self.assertEqual(s['archive_sha256'], 'abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d')
        self.assertEqual(s['urls'], ['https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'])
        self.assertEqual(sorted(s['files']), sorted(self.FILES)); self.assertEqual(sorted(s['sizes']), sorted(self.FILES))
        self.assertTrue(all(len(h) == 64 and int(h, 16) >= 0 for h in s['files'].values()))
        self.assertTrue((KIT / 'enhancer' / 'LICENSES' / install.VULKAN_NOTICE).is_file())

    def test_unpacks_only_the_checked_files(self):
        target = self.tmp / 'deps' / 'tool'
        upscaler_fetch.from_zip(self.good, target)
        self.assertEqual(upscaler_fetch.verify(target), [])
        self.assertEqual(sorted(p.relative_to(target).as_posix() for p in target.rglob('*') if p.is_file()), sorted(self.FILES))

    def test_archive_hash_mismatch_is_a_clear_error(self):
        bad = self.tmp / 'bad.zip'; make_zip(bad, dict(self.FILES, **{'vcomp140.dll': b'MZ other'}))
        with self.assertRaises(upscaler_fetch.MismatchError) as cm: upscaler_fetch.from_zip(bad, self.tmp / 't')
        self.assertIn('is not the published realesrgan-ncnn-vulkan-20220424-windows.zip (its SHA-256 differs)', str(cm.exception))
        self.assertFalse((self.tmp / 't').exists()); self.assertFalse((self.tmp / 't.new').exists())

    def test_file_hash_mismatch_is_a_clear_error(self):
        bad = self.tmp / 'bad.zip'
        upscaler_fetch.SPEC['archive_sha256'] = make_zip(bad, dict(self.FILES, **{'models/realesrgan-x4plus.bin': b'\x01' * 999}))
        with self.assertRaises(upscaler_fetch.MismatchError) as cm: upscaler_fetch.from_zip(bad, self.tmp / 't')
        self.assertIn('not the expected realesrgan-ncnn-vulkan build, e.g. models/realesrgan-x4plus.bin', str(cm.exception))
        self.assertFalse((self.tmp / 't').exists())

    def test_ensure_sources(self):
        log = []
        os.environ['MA2_UPSCALER_SOURCE'] = self.good.as_uri()                       # a URL (file://, as a local mirror)
        target = upscaler_fetch.ensure(self.tmp / 'a' / 'tool', log.append)
        self.assertEqual(upscaler_fetch.verify(target), []); self.assertTrue(any('archive and 4 files checked' in l for l in log))
        self.assertFalse((self.tmp / 'a' / upscaler_fetch.SPEC['archive']).exists())  # the download is deleted once unpacked
        os.environ['MA2_UPSCALER_SOURCE'] = str(self.good)                            # the zip itself
        self.assertEqual(upscaler_fetch.verify(upscaler_fetch.ensure(self.tmp / 'b' / 'tool', log.append)), [])
        os.environ['MA2_UPSCALER_SOURCE'] = str(target)                               # an extracted folder
        self.assertEqual(upscaler_fetch.verify(upscaler_fetch.ensure(self.tmp / 'c' / 'tool', log.append)), [])
        os.environ.pop('MA2_UPSCALER_SOURCE')
        d = self.tmp / 'inst'; d.mkdir(); shutil.copy2(self.good, d / self.good.name)  # the zip in the install folder
        self.assertEqual(upscaler_fetch.verify(upscaler_fetch.ensure(self.tmp / 'd' / 'tool', log.append, look_in=[d])), [])
        (target / 'vcomp140.dll').write_bytes(b'changed')                             # an earlier folder that no longer checks out
        os.environ['MA2_UPSCALER_SOURCE'] = str(self.good)
        upscaler_fetch.ensure(target, log.append); self.assertEqual(upscaler_fetch.verify(target), [])

    def test_ensure_errors(self):
        bad = self.tmp / 'x' / upscaler_fetch.SPEC['archive']; bad.parent.mkdir(); bad.write_bytes(b'not a zip')
        os.environ['MA2_UPSCALER_SOURCE'] = str(bad)
        with self.assertRaises(upscaler_fetch.MismatchError) as cm: upscaler_fetch.ensure(self.tmp / 'e' / 'tool', lambda s: None)
        self.assertIn('not the expected build', str(cm.exception)); self.assertIn('Delete that file, then download exactly', str(cm.exception))
        os.environ.pop('MA2_UPSCALER_SOURCE')
        with self.assertRaises(upscaler_fetch.FetchError) as cm: upscaler_fetch.ensure(self.tmp / 'f' / 'tool', lambda s: None)
        self.assertIn('Could not get the Vulkan texture upscaler', str(cm.exception)); self.assertNotIsInstance(cm.exception, upscaler_fetch.MismatchError)
        # through the installer: a Stop the failure screen can show (the textures step turns it into "the game is installed")
        dest = self.tmp / 'inst2'; dest.mkdir()
        inst = install.Installer(None, dest, log=lambda s: None)
        with self.assertRaises(install.Stop) as cm: inst.vulkan_dir()
        self.assertEqual(cm.exception.kind, 'network')
        os.environ['MA2_UPSCALER_SOURCE'] = str(bad)
        with self.assertRaises(install.Stop) as cm: inst.vulkan_dir()
        self.assertEqual(cm.exception.kind, 'network'); self.assertIn('not the expected build', str(cm.exception))
        self.assertTrue(install.extras_message('textures-2x', cm.exception).startswith('MechAssault 2 is installed. The 2x enhanced textures could not be made: The Vulkan texture upscaler was found'))

    @unittest.skipUnless(os.environ.get('MA2_TEST_UPSCALER_ZIP'), 'MA2_TEST_UPSCALER_ZIP not set (the real archive)')
    def test_real_archive(self):
        upscaler_fetch.SPEC = self.real
        target = self.tmp / 'real' / 'tool'
        upscaler_fetch.from_zip(os.environ['MA2_TEST_UPSCALER_ZIP'], target)
        self.assertEqual(upscaler_fetch.verify(target), [])


class Tool(unittest.TestCase):
    """upscale_vulkan.py and enhance.py --engine vulkan."""
    @classmethod
    def setUpClass(cls):
        try:
            import numpy, PIL                                                         # noqa: F401
        except ImportError:
            raise unittest.SkipTest('needs NumPy and Pillow')
        sys.path.insert(0, str(KIT / 'enhancer'))
        import upscale_vulkan; cls.uv = upscale_vulkan

    def test_no_console_and_exit_codes(self):
        uv = self.uv
        self.assertTrue(uv.NO_CONSOLE & subprocess.CREATE_NO_WINDOW and uv.NO_CONSOLE & subprocess.CREATE_NEW_PROCESS_GROUP)
        text = (KIT / 'enhancer' / 'upscale_vulkan.py').read_text(encoding='utf-8')
        self.assertEqual(text.count('subprocess.run('), text.count('creationflags=NO_CONSOLE'))
        self.assertEqual(uv.exit_text(3221225786), 'exit code 3221225786 = 0xC000013A: stopped from outside: its window was closed or security software ended it')
        self.assertEqual(uv.exit_text(-1073741510)[:34], 'exit code 3221225786 = 0xC000013A:')
        self.assertEqual(uv.exit_text(1), 'exit code 1')
        self.assertEqual(install.explain_codes('realesrgan-ncnn-vulkan failed (%s).' % uv.exit_text(3221225786)), [install.exit_meaning(3221225786)])
        with self.assertRaises(uv.VulkanError): uv.VulkanUpscaler(WORK / 'tmp' / 'no-such-tool', 2, {}, WORK / 'tmp' / 'vk-none')

    def fake_tool(self, root, crash=None):
        """A tool folder with stand-in files and a stand-in subprocess.run that records each run and writes 4x pictures."""
        from PIL import Image
        (root / 'models').mkdir(parents=True)
        for n in ('realesrgan-ncnn-vulkan.exe', 'vcomp140.dll', 'models/realesrgan-x4plus.param', 'models/realesrgan-x4plus.bin'): (root / n).write_bytes(b'x')
        runs = []
        def run(args, **kw):
            runs.append((list(args), kw.get('cwd')))
            if crash: return subprocess.CompletedProcess(args, crash, b'', b'')
            src, dst = Path(args[args.index('-i') + 1]), Path(args[args.index('-o') + 1])
            pairs = [(f, dst / f.name) for f in src.glob('*.png')] if src.is_dir() else [(src, dst)]
            for a, b in pairs:
                with Image.open(a) as im: im.resize((im.width * 4, im.height * 4)).save(b)
            return subprocess.CompletedProcess(args, 0, b'', b'[0 Test GPU]  queueC=0[1]\n')
        return run, runs

    def test_unicode_folders_run_from_a_plain_staging_folder(self):
        """A non-ASCII working folder or path crashes the real tool (0xC0000409): it runs from a plain ASCII copy instead."""
        from PIL import Image
        uv = self.uv; base = Path(tempfile.mkdtemp(prefix='ma2ux-vkuni-', dir=str(WORK / 'tmp')))
        real_run, real_bases = uv.subprocess.run, uv.stage_bases
        try:
            uni = base / '日本語 Ünïcode'
            run, runs = self.fake_tool(uni / 'tool')
            uv.subprocess.run = run; uv.stage_bases = lambda work: [base]            # stage under the test folder (plain)
            self.assertFalse(uv.plain(uni)); self.assertTrue(uv.plain(base))
            up = uv.VulkanUpscaler(uni / 'tool', 2, {'boundary_padding': 4}, uni / 'work')
            self.assertTrue(up.stage and up.stage.parent == base and uv.plain(up.stage))
            img = Image.new('RGBA', (10, 6), (1, 2, 3, 200))
            self.assertEqual(up.enhance(img, 'ai_rgb', 'reflect').size, (20, 12))
            self.assertEqual(up.prefetch(up.requests(img, 'ai_alpha', 'edge')), 1)
            self.assertEqual(up.enhance(img, 'ai_alpha', 'edge').size, (20, 12))
            self.assertGreaterEqual(len(runs), 3)
            for args, cwd in runs:                                                  # every path the tool sees, and its working folder
                self.assertTrue(cwd and uv.plain(cwd) and all(a.isascii() for a in args), (args, cwd))
                self.assertTrue(args[0].startswith(str(up.stage)))
            stage = up.stage; up.close(); self.assertFalse(stage.exists())
            # plain folders: no staging, but still a plain working folder
            run, runs = self.fake_tool(base / 'plain' / 'tool'); uv.subprocess.run = run
            up = uv.VulkanUpscaler(base / 'plain' / 'tool', 4, {}, base / 'plain' / 'work')
            self.assertIsNone(up.stage); self.assertEqual(runs[0][1], str(base / 'plain' / 'tool')); up.close()
        finally:
            uv.subprocess.run, uv.stage_bases = real_run, real_bases
            shutil.rmtree(base, ignore_errors=True)

    def test_tool_crash_is_not_called_a_driver_problem(self):
        uv = self.uv; base = Path(tempfile.mkdtemp(prefix='ma2ux-vkcr-', dir=str(WORK / 'tmp')))
        real_run = uv.subprocess.run
        try:
            run, runs = self.fake_tool(base / 'tool', crash=0xC0000409); uv.subprocess.run = run
            with self.assertRaises(uv.VulkanError) as cm: uv.VulkanUpscaler(base / 'tool', 2, {}, base / 'work')
            self.assertIn('0xC0000409', str(cm.exception)); self.assertIn('not with the graphics driver', str(cm.exception))
            self.assertNotIn('Updating the graphics driver', str(cm.exception))
            run, runs = self.fake_tool(base / 'tool2', crash=1); uv.subprocess.run = run
            with self.assertRaises(uv.VulkanError) as cm: uv.VulkanUpscaler(base / 'tool2', 2, {}, base / 'work2')
            self.assertIn('Updating the graphics driver', str(cm.exception))
        finally:
            uv.subprocess.run = real_run
            shutil.rmtree(base, ignore_errors=True)

    def test_enhance_args(self):
        r = subprocess.run([sys.executable, '-B', str(KIT / 'enhancer' / 'enhance.py'), '--engine', 'vulkan'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2); self.assertIn('--engine vulkan needs --vulkan', r.stderr)
        text = (KIT / 'enhancer' / 'enhance.py').read_text(encoding='utf-8')
        self.assertIn('model_hash = "vulkan:" + file_hash(model)', text)                # a PyTorch image is never taken as a Vulkan one

    @unittest.skipUnless(os.environ.get('MA2_TEST_UPSCALER_DIR'), 'MA2_TEST_UPSCALER_DIR not set (the real tool)')
    def test_real_tool(self):
        from PIL import Image
        uv = self.uv; work = Path(tempfile.mkdtemp(prefix='ma2ux-vkrun-', dir=str(WORK / 'tmp')))
        try:
            img = Image.new('RGBA', (40, 24), (200, 40, 40, 255))
            for x in range(40): img.putpixel((x, 12), (20, 200, 20, x * 6))
            for scale in (2, 4):
                up = uv.VulkanUpscaler(os.environ['MA2_TEST_UPSCALER_DIR'], scale, {'boundary_padding': 16, 'alpha_mode': 'lanczos'}, work / str(scale))
                self.assertTrue(up.gpu_name)
                out = up.enhance(img, 'ai_rgb', 'wrap')
                self.assertEqual((out.size, out.mode), ((40 * scale, 24 * scale), 'RGBA'))
                self.assertEqual(out.getchannel('A').tobytes(), img.getchannel('A').resize(out.size, Image.Resampling.LANCZOS).tobytes())
                mask = up.enhance(img, 'ai_alpha', 'reflect')
                self.assertEqual((mask.size, mask.mode), ((40 * scale, 24 * scale), 'RGBA'))
                self.assertEqual(mask.convert('RGB').getextrema(), ((255, 255), (255, 255), (255, 255)))
                self.assertEqual(up.prefetch(up.requests(img, 'ai_rgb', 'edge') + up.requests(img, 'ai_alpha', 'edge')), 2)
                self.assertEqual(len(up.cache), 2)
                self.assertEqual(up.enhance(img, 'ai_rgb', 'edge').size, (40 * scale, 24 * scale)); self.assertEqual(len(up.cache), 1)
                up.close(); self.assertFalse((work / str(scale)).exists())
        finally:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    unittest.main(verbosity=2)

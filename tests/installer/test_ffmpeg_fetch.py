"""FFmpeg is downloaded on the player's PC (kit/installer/ffmpeg_fetch.py): every file is checked before use, a bad or
missing download stops with what to do, and only the files the build names are ever written.

    python tests/installer/test_ffmpeg_fetch.py     (no internet needed: a local server stands in for the download)

MA2_TEST_FFMPEG_ZIP=<the real ffmpeg-9.0.2-full_build-shared.zip, or a folder with it extracted> also checks the real
build against kit/installer/data/ffmpeg.json.
"""
import functools, hashlib, http.server, os, shutil, sys, tempfile, threading, unittest, zipfile
from pathlib import Path
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'kit' / 'installer'))
import ffmpeg_fetch, install                     # noqa: E402

FILES = {'bin/avcodec-63.dll': b'codec bytes', 'include/libavutil/avutil.h': b'/* header */\n', 'LICENSE': b'GPL text\n'}


def fake_spec():
    spec = dict(ffmpeg_fetch.SPEC)
    spec['files'] = {k: hashlib.sha256(v).hexdigest() for k, v in FILES.items()}
    spec['sizes'] = {k: len(v) for k, v in FILES.items()}
    spec['archive_sha256'] = None
    return spec


def make_corrupt_zip(path):
    """A zip whose first member's deflate stream is damaged (reading it raises zlib.error, not BadZipFile)."""
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for rel, data in FILES.items(): z.writestr(ffmpeg_fetch.NAME + '/' + rel, data * 50)
    with zipfile.ZipFile(path) as z:
        info = z.infolist()[0]
    raw = bytearray(path.read_bytes())
    o = info.header_offset
    start = o + 30 + int.from_bytes(raw[o + 26:o + 28], 'little') + int.from_bytes(raw[o + 28:o + 30], 'little')
    raw[start] = 0xFF                                     # BFINAL=1, BTYPE=11: an invalid deflate block
    path.write_bytes(bytes(raw))
    return path


def make_zip(path, files=FILES, root=ffmpeg_fetch.NAME, extra=()):
    with zipfile.ZipFile(path, 'w') as z:
        for rel, data in files.items(): z.writestr(root + '/' + rel, data)
        for name, data in extra: z.writestr(name, data)
    return path


class Server:
    """A localhost-only HTTP server for one folder (the 'mirror')."""
    def __init__(self, folder):
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(folder))
        handler.log_message = lambda *a: None
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = 'http://127.0.0.1:%d/' % self.httpd.server_address[1]

    def close(self):
        self.httpd.shutdown(); self.httpd.server_close()


class Fetch(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ff-'))
        self.saved = ffmpeg_fetch.SPEC, os.environ.pop('MA2_FFMPEG_SOURCE', None)
        ffmpeg_fetch.SPEC = fake_spec()
        self.target = self.tmp / 'work' / 'deps' / ffmpeg_fetch.NAME
        self.log = []

    def tearDown(self):
        ffmpeg_fetch.SPEC = self.saved[0]
        os.environ.pop('MA2_FFMPEG_SOURCE', None)
        if self.saved[1] is not None: os.environ['MA2_FFMPEG_SOURCE'] = self.saved[1]
        shutil.rmtree(self.tmp, ignore_errors=True)

    def ensure(self, **kw):
        return ffmpeg_fetch.ensure(self.target, self.log.append, **kw)

    def test_download_from_a_mirror_is_checked_and_the_zip_removed(self):
        mirror = self.tmp / 'mirror'; mirror.mkdir(); make_zip(mirror / ffmpeg_fetch.SPEC['archive'])
        srv = Server(mirror)
        try:
            ffmpeg_fetch.SPEC['urls'] = [srv.url + 'missing.zip', srv.url + ffmpeg_fetch.SPEC['archive']]   # first mirror fails
            self.assertEqual(self.ensure(), self.target)
        finally:
            srv.close()
        self.assertEqual(ffmpeg_fetch.verify(self.target), [])
        self.assertEqual((self.target / 'bin' / 'avcodec-63.dll').read_bytes(), FILES['bin/avcodec-63.dll'])
        self.assertFalse(list(self.target.parent.glob('*.zip*')), 'the downloaded archive is deleted after use')
        self.assertTrue(any('not used' in l for l in self.log), 'the failed mirror is logged')

    def test_a_tampered_file_stops_and_nothing_is_used(self):
        bad = dict(FILES); bad['bin/avcodec-63.dll'] = b'something else'
        z = make_zip(self.tmp / 'x.zip', bad)
        os.environ['MA2_FFMPEG_SOURCE'] = str(z)
        with self.assertRaises(ffmpeg_fetch.FetchError) as cm: self.ensure()
        self.assertIn('Resume', str(cm.exception)); self.assertIn('avcodec-63.dll', str(cm.exception))
        self.assertFalse(self.target.exists(), 'no folder is left that could be mistaken for the build')

    def test_archive_hash_is_checked_when_recorded(self):
        z = make_zip(self.tmp / 'x.zip')
        ffmpeg_fetch.SPEC['archive_sha256'] = '0' * 64
        os.environ['MA2_FFMPEG_SOURCE'] = str(z)
        with self.assertRaises(ffmpeg_fetch.FetchError): self.ensure()
        ffmpeg_fetch.SPEC['archive_sha256'] = ffmpeg_fetch.sha256(z)
        self.assertEqual(self.ensure(), self.target)

    def test_only_listed_files_are_written(self):
        z = make_zip(self.tmp / 'x.zip', extra=[('../../evil.dll', b'x'), (ffmpeg_fetch.NAME + '/bin/extra.exe', b'x')])
        os.environ['MA2_FFMPEG_SOURCE'] = str(z)
        self.ensure()
        self.assertFalse((self.tmp / 'evil.dll').exists() or (self.tmp / 'work' / 'evil.dll').exists())
        self.assertEqual(sorted(p.relative_to(self.target).as_posix() for p in self.target.rglob('*') if p.is_file()), sorted(FILES))

    def test_override_is_the_only_source(self):
        ffmpeg_fetch.SPEC['urls'] = ['http://127.0.0.1:9/never-tried.zip']
        os.environ['MA2_FFMPEG_SOURCE'] = str(self.tmp / 'nope.zip')
        with self.assertRaises(ffmpeg_fetch.FetchError) as cm: self.ensure()
        self.assertNotIn('never-tried', str(cm.exception).split('Tried:')[1], 'the published URLs are not tried')

    def test_folder_and_zip_beside_the_installer(self):
        folder = self.tmp / 'x' / ffmpeg_fetch.NAME
        for rel, data in FILES.items(): (folder / rel).parent.mkdir(parents=True, exist_ok=True); (folder / rel).write_bytes(data)
        os.environ['MA2_FFMPEG_SOURCE'] = str(self.tmp / 'x')                 # the folder holding the extracted archive
        self.ensure(); self.assertEqual(ffmpeg_fetch.verify(self.target), [])
        shutil.rmtree(self.target); del os.environ['MA2_FFMPEG_SOURCE']
        ffmpeg_fetch.SPEC['urls'] = ['http://127.0.0.1:9/offline.zip']
        dest = self.tmp / 'dest'; dest.mkdir(); make_zip(dest / ffmpeg_fetch.SPEC['archive'])
        self.ensure(look_in=[dest])
        self.assertEqual(ffmpeg_fetch.verify(self.target), [])
        self.assertTrue((dest / ffmpeg_fetch.SPEC['archive']).is_file(), "the player's own zip is never deleted")

    def test_an_earlier_run_is_reused_without_a_download(self):
        os.environ['MA2_FFMPEG_SOURCE'] = str(make_zip(self.tmp / 'x.zip')); self.ensure()
        os.environ['MA2_FFMPEG_SOURCE'] = str(self.tmp / 'gone.zip')
        self.assertEqual(self.ensure(), self.target)

    def test_offline_failure_is_a_network_stop_for_the_wizard(self):
        ffmpeg_fetch.SPEC['urls'] = ['http://127.0.0.1:9/a.zip']
        saved_kit = install.KIT
        install.KIT = self.tmp / 'kit'                                      # a kit without FFmpeg, as shipped
        try:
            inst = install.Installer(None, self.tmp / 'dest', log=self.log.append)
            with self.assertRaises(install.Stop) as cm: inst.ffmpeg_dir()
        finally:
            install.KIT = saved_kit
        self.assertEqual(cm.exception.kind, 'network')
        self.assertIn(ffmpeg_fetch.SPEC['archive'], str(cm.exception))

    def test_a_damaged_archive_is_a_fetch_error_and_the_next_mirror_is_tried(self):
        bad = make_corrupt_zip(self.tmp / 'bad.zip')
        with self.assertRaises(ffmpeg_fetch.FetchError) as cm:
            ffmpeg_fetch.from_zip(bad, self.target)
        self.assertIn('error', str(cm.exception))         # zlib.error (or the CRC check), never a raw exception
        self.assertFalse(self.target.exists() or self.target.with_name(self.target.name + '.new').exists())
        mirror = self.tmp / 'mirror'; mirror.mkdir()
        shutil.copyfile(bad, mirror / 'bad.zip'); make_zip(mirror / ffmpeg_fetch.SPEC['archive'])
        srv = Server(mirror)
        try:
            ffmpeg_fetch.SPEC['urls'] = [srv.url + 'bad.zip', srv.url + ffmpeg_fetch.SPEC['archive']]
            self.assertEqual(self.ensure(), self.target)
        finally:
            srv.close()
        self.assertEqual(ffmpeg_fetch.verify(self.target), [])
        self.assertTrue(any('could not be read' in l for l in self.log), 'the damaged mirror is logged')

    def test_a_damaged_archive_alone_is_a_network_stop_with_resume(self):
        os.environ['MA2_FFMPEG_SOURCE'] = str(make_corrupt_zip(self.tmp / 'bad.zip'))
        saved_kit = install.KIT
        install.KIT = self.tmp / 'kit'
        try:
            inst = install.Installer(None, self.tmp / 'dest', log=self.log.append)
            with self.assertRaises(install.Stop) as cm: inst.ffmpeg_dir()
        finally:
            install.KIT = saved_kit
        self.assertEqual(cm.exception.kind, 'network'); self.assertIn('Resume', str(cm.exception))

    def test_a_different_build_is_its_own_failure_kind(self):
        bad = dict(FILES); bad['LICENSE'] = b'another version\n'
        os.environ['MA2_FFMPEG_SOURCE'] = str(make_zip(self.tmp / 'x.zip', bad))
        saved_kit = install.KIT
        install.KIT = self.tmp / 'kit'
        try:
            inst = install.Installer(None, self.tmp / 'dest', log=self.log.append)
            with self.assertRaises(install.Stop) as cm: inst.ffmpeg_dir()
        finally:
            install.KIT = saved_kit
        self.assertEqual(cm.exception.kind, 'ffmpeg_mismatch')
        self.assertIn(ffmpeg_fetch.SPEC['urls'][0], str(cm.exception), 'names the exact file to download')
        import gui
        self.assertIn('ffmpeg_mismatch', gui.FAILURES)

    def test_a_member_longer_than_the_real_file_is_refused(self):
        big = dict(FILES); big['bin/avcodec-63.dll'] = FILES['bin/avcodec-63.dll'] + b'\0' * 4096
        os.environ['MA2_FFMPEG_SOURCE'] = str(make_zip(self.tmp / 'x.zip', big))
        with self.assertRaises(ffmpeg_fetch.MismatchError): self.ensure()
        self.assertFalse(self.target.exists())

    def test_a_url_source_works_and_is_not_taken_for_a_path(self):
        self.assertTrue(ffmpeg_fetch.is_url('https://example.invalid/a.zip') and ffmpeg_fetch.is_url('file:///C:/a.zip'))
        self.assertFalse(ffmpeg_fetch.is_url(r'C:\Downloads\a.zip') or ffmpeg_fetch.is_url('a.zip'))
        os.environ['MA2_FFMPEG_SOURCE'] = make_zip(self.tmp / 'x.zip').as_uri()
        self.assertEqual(self.ensure(), self.target)
        self.assertTrue((self.tmp / 'x.zip').is_file(), 'a file: URL source is copied, the original is kept')

    def test_cancel_stops_the_extraction(self):
        os.environ['MA2_FFMPEG_SOURCE'] = str(make_zip(self.tmp / 'x.zip'))
        with self.assertRaises(ffmpeg_fetch.Cancelled): self.ensure(cancelled=lambda: True)
        self.assertFalse(self.target.exists())


class RealBuild(unittest.TestCase):
    def test_the_real_build_matches_the_recorded_hashes(self):
        src = os.environ.get('MA2_TEST_FFMPEG_ZIP')
        if not src: self.skipTest('set MA2_TEST_FFMPEG_ZIP to the real FFmpeg zip (or its extracted folder)')
        tmp = Path(tempfile.mkdtemp(prefix='ma2ffr-'))
        try:
            target = tmp / ffmpeg_fetch.NAME
            (ffmpeg_fetch.from_folder if Path(src).is_dir() else ffmpeg_fetch.from_zip)(src, target)
            self.assertEqual(ffmpeg_fetch.verify(target), [])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_data_file_is_consistent(self):
        spec = ffmpeg_fetch.SPEC
        self.assertTrue(spec['urls'] and all(u.startswith('https://') for u in spec['urls']))
        self.assertTrue(all(len(h) == 64 for h in spec['files'].values()))
        self.assertEqual(set(spec.get('sizes') or {}), set(spec['files']), 'a size for every file')
        self.assertTrue(set(install.FFMPEG_DLLS) <= {Path(f).name for f in spec['files']})
        self.assertEqual(install.FFMPEG, ffmpeg_fetch.NAME)


if __name__ == '__main__':
    unittest.main()

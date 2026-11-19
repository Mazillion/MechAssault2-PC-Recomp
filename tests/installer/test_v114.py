"""1.1.4 reliability core: silent recoveries (memory, build path, downloads, locks, journal, reboot), early detection
(space, graphics adapters, platform), diagnostics.
Run with the kit's Python:  python -B test_v114.py
"""
import inspect, json, os, shutil, sys, tempfile, unittest
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
KIT = WORK.parent / 'kit'
sys.path.insert(0, str(KIT / 'installer'))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
import precheck, install                                                     # noqa: E402

G = precheck.GB
REAL_SLEEP = __import__('time').sleep                         # (Base replaces time.sleep while a test runs)
MANIFEST = ('<assembly><application xmlns="urn:schemas-microsoft-com:asm.v3">\n    <windowsSettings>\n'
            '      <dpiAwareness>PerMonitorV2</dpiAwareness>\n    </windowsSettings>\n  </application></assembly>\n')


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-114-', dir=str(WORK / 'tmp')))
        self.lines = []
        self._sleep = install.time.sleep
        install.time.sleep = lambda s: None
    def tearDown(self):
        install.time.sleep = self._sleep
        shutil.rmtree(self.tmp, ignore_errors=True)

    def installer(self, name='i'):
        d = self.tmp / name; d.mkdir(parents=True, exist_ok=True)
        inst = install.Installer(None, d, log=self.lines.append)
        inst.state = {'done': []}; inst.work.mkdir(parents=True, exist_ok=True)
        return inst


class BuildRecovery(Base):
    """A2 out-of-memory retry with fewer jobs (objects kept); A3 a path failure moves the build to a plain folder."""
    def fake_build(self, inst, outcomes):
        """build() with the kit copy and the toolchain faked; run_cmd follows `outcomes` (a Stop kind + tail, or 'ok')."""
        inst.vcvars, inst.cmake, inst.ninja, inst.vs = Path('vcvars64.bat'), Path('cmake.exe'), Path('ninja.exe'), self.tmp / 'vs'
        inst.jobs = 8
        inst.project = lambda: self.tmp / 'proj'
        real = shutil.copytree
        def copytree(s, d, *a, **k):
            Path(d).mkdir(parents=True, exist_ok=True)
            if Path(d).name == 'game': (Path(d) / 'src').mkdir(); (Path(d) / 'src' / 'app.manifest').write_text(MANIFEST, encoding='utf-8')
        self.calls = []
        def run_cmd(args, cwd=None, env=None, **k):
            script = Path(args[-1]).read_text(encoding='utf-8')
            jobs = int(script.split(' -j ')[1].split()[0])
            build = Path(cwd) / 'build'; build.mkdir(exist_ok=True)              # (CMake makes it)
            self.calls.append((Path(cwd), jobs, sorted(p.name for p in build.iterdir())))
            out = outcomes.pop(0)
            if out == 'ok':
                (build / 'mechassault2_recomp.exe').write_bytes(b'built'); return
            kind, tail = out
            (build / ('obj%d.obj' % len(self.calls))).write_bytes(b'')       # objects finished before the failure
            e = install.Stop('CMake + Ninja failed', kind); e.tail = tail; raise e
        inst.run_cmd = run_cmd
        with mock.patch.object(install.shutil, 'copytree', copytree), \
             mock.patch.object(install.precheck, 'memory_info', lambda: (32 * G, 20 * G)):
            inst.build()
        del real

    def test_out_of_memory_retries_with_half_then_one_job_keeping_objects(self):
        inst = self.installer()
        statuses = []; inst.on_status = statuses.append
        self.fake_build(inst, [('out_of_memory', ['fatal error C1060: compiler is out of heap space']),
                               ('out_of_memory', ['LINK : fatal error LNK1102: out of memory']), 'ok'])
        self.assertEqual([c[1] for c in self.calls], [8, 4, 1])
        self.assertIn('obj1.obj', self.calls[1][2]); self.assertIn('obj2.obj', self.calls[2][2])      # finished objects are kept
        self.assertTrue(any('RECOVERY' in l and 'memory' in l for l in self.lines), self.lines)
        self.assertTrue(statuses and 'low memory' in statuses[0])
        self.assertEqual(inst.state['oom_jobs'], 1)

    def test_out_of_memory_at_one_job_stops(self):
        inst = self.installer(); inst.state['oom_jobs'] = 1
        with self.assertRaises(install.Stop) as cm:
            self.fake_build(inst, [('out_of_memory', ['C1060'])])
        self.assertEqual(cm.exception.kind, 'out_of_memory'); self.assertEqual(self.calls[0][1], 1)

    def test_c1002_is_out_of_memory(self):
        self.assertEqual(install.classify(['fatal error C1002: compiler is out of heap space in pass 2'], 'CMake + Ninja'), 'out_of_memory')

    def test_a_new_run_starts_a_clean_build_as_before(self):
        inst = self.installer()
        self.fake_build(inst, ['ok'])
        (inst.work / 'build' / 'old.obj').write_bytes(b'')
        self.fake_build(inst, ['ok'])                                                  # 1.1.3 behaviour: never reuse an earlier run's objects
        self.assertNotIn('old.obj', self.calls[-1][2])

    def test_path_error_moves_the_build_to_a_plain_folder_once(self):
        inst = self.installer()
        plain = self.tmp / 'plainbase'
        with mock.patch.object(install, 'build_bases', lambda anchor: [plain]):
            self.fake_build(inst, [('compile_error', ["LINK : fatal error LNK1117: syntax error in option 'MANIFESTINPUT:C:\\a,b'"]), 'ok'])
            self.assertEqual(inst.build_root(), self.calls[1][0])                     # and a Resume keeps building there
        self.assertEqual(self.calls[0][0], inst.work)
        self.assertTrue(self.calls[1][0].name.startswith(install.BUILD_PREFIX) and self.calls[1][0].parent == plain)
        self.assertTrue(inst.state['plain_build']); self.assertEqual(inst.state['build_root'], str(self.calls[1][0]))
        self.assertTrue(any('RECOVERY' in l and 'plain folder' in l for l in self.lines))
        self.assertFalse((inst.work / 'build').exists())                              # the failed in-place build does not linger

    def test_other_build_errors_are_not_retried(self):
        inst = self.installer()
        with self.assertRaises(install.Stop):
            self.fake_build(inst, [('compile_error', ['error C2065: undeclared identifier'])])
        self.assertEqual(len(self.calls), 1); self.assertFalse(inst.state.get('plain_build'))


import http.server, threading                                                 # noqa: E402
import download as dl                                                         # noqa: E402

ZIPBYTES = b'PK\x03\x04' + bytes(range(256)) * 400                           # ~100 KB that starts like a zip


class Handler(http.server.BaseHTTPRequestHandler):
    """A local test server: /file (Range aware), /cut (first answer stops half way), /portal, /proxy, /flaky (500 twice), /missing."""
    hits, ranges, state = [], [], {}
    def log_message(self, *a): pass
    def send_body(self, body, status=200, ctype='application/zip', extra=()):
        self.send_response(status); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(body)))
        for k, v in extra: self.send_header(k, v)
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        Handler.hits.append(self.path); rng = self.headers.get('Range'); Handler.ranges.append(rng)
        if self.path == '/portal': return self.send_body(b'<html>Please log in to the Wi-Fi</html>', ctype='text/html')
        if self.path == '/proxy': return self.send_body(b'', 407, 'text/plain')
        if self.path == '/missing': return self.send_body(b'', 404, 'text/plain')
        if self.path == '/flaky':
            n = Handler.state['flaky'] = Handler.state.get('flaky', 0) + 1
            if n <= 2: return self.send_body(b'busy', 500, 'text/plain')
        if self.path == '/notzip': return self.send_body(b'MZ' + ZIPBYTES[2:])
        body = ZIPBYTES
        if rng:
            start = int(rng.split('=')[1].split('-')[0])
            return self.send_body(body[start:], 206, extra=[('Content-Range', 'bytes %d-%d/%d' % (start, len(body) - 1, len(body)))])
        if self.path == '/cut' and not Handler.state.get('cut'):
            Handler.state['cut'] = True
            self.send_response(200); self.send_header('Content-Type', 'application/zip'); self.send_header('Content-Length', str(len(body)))
            self.end_headers(); self.wfile.write(body[:len(body) // 2]); self.wfile.flush(); self.connection.close(); return
        return self.send_body(body)


class Downloads(Base):
    @classmethod
    def setUpClass(cls):
        cls.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=cls.httpd.serve_forever, daemon=True).start()
        cls.url = 'http://127.0.0.1:%d/' % cls.httpd.server_address[1]
    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown(); cls.httpd.server_close()
    def setUp(self):
        super().setUp(); Handler.hits.clear(); Handler.ranges.clear(); Handler.state.clear()
        self.cache = mock.patch.object(dl, 'cache_dir', lambda: self.tmp / 'cache'); self.cache.start()
        self.sleep = mock.patch.object(dl.time, 'sleep', lambda s: None); self.sleep.start()
    def tearDown(self):
        self.cache.stop(); self.sleep.stop(); super().tearDown()

    def get(self, name, **kw):
        kw.setdefault('magic', b'PK\x03\x04')
        return dl.fetch(self.url + name, self.tmp / 'out.zip', self.lines.append, **kw)

    def cause(self, name, **kw):
        with self.assertRaises(dl.DownloadError) as cm: self.get(name, **kw)
        return cm.exception

    def test_plain_download_and_record(self):
        self.assertEqual(self.get('file').read_bytes(), ZIPBYTES)
        rec = (Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs' / 'downloads.log').read_text().splitlines()[-1]
        self.assertEqual(json.loads(rec)['result'], 'ok'); self.assertEqual(json.loads(rec)['file'], 'file')
        self.assertNotIn('?', rec)

    def test_a_stopped_download_resumes_with_range(self):
        self.assertEqual(self.get('cut').read_bytes(), ZIPBYTES)
        self.assertEqual(len(Handler.hits), 2); self.assertIsNone(Handler.ranges[0]); self.assertTrue(Handler.ranges[1].startswith('bytes='))
        self.assertTrue(any('resuming' in l for l in self.lines), self.lines)

    def test_server_errors_are_retried(self):
        self.assertEqual(self.get('flaky').read_bytes(), ZIPBYTES); self.assertEqual(len(Handler.hits), 3)

    def test_portal_proxy_missing_and_wrong_file_are_named_at_once(self):
        e = self.cause('portal'); self.assertEqual(e.cause, 'portal'); self.assertIn('login page', str(e))
        self.assertEqual(self.cause('proxy').cause, 'proxy')
        self.assertEqual(self.cause('missing').cause, 'notfound')
        self.assertEqual(self.cause('notzip').cause, 'bad')
        self.assertEqual(len(Handler.hits), 4)                                    # none of them retried
        self.assertFalse((self.tmp / 'out.zip.part').exists())

    def test_digest_and_cache(self):
        import hashlib
        h = hashlib.sha256(ZIPBYTES).hexdigest()
        self.get('file', digest=h); self.get('file', digest=h)
        self.assertEqual(len(Handler.hits), 1)                                    # the second one came from the cache
        self.assertEqual(self.cause('file', digest='0' * 64).cause, 'bad')

    def test_fetch_and_use_caches_only_https_and_drops_a_refused_copy(self):
        used = []
        dl.fetch_and_use(self.url + 'file', self.tmp / 'a.zip', used.append, self.lines.append, cache_key='k', magic=b'PK\x03\x04')
        self.assertFalse((self.tmp / 'cache').exists() and any((self.tmp / 'cache').iterdir()))     # http: never cached
        self.assertFalse((self.tmp / 'a.zip').exists())
        (self.tmp / 'cache').mkdir(); (self.tmp / 'cache' / 'k').write_bytes(b'stale')
        def use(p):
            if Path(p).read_bytes() == b'stale': raise ValueError('not the build')
            used.append(p)
        with mock.patch.object(dl, 'fetch', lambda url, out, *a, **k: (Path(out).write_bytes(ZIPBYTES), Path(out))[1]):
            dl.fetch_and_use('https://example.invalid/x.zip', self.tmp / 'b.zip', use, self.lines.append, cache_key='k')
        self.assertEqual((self.tmp / 'cache' / 'k').read_bytes(), ZIPBYTES)       # the refused copy was replaced by the good one

    def test_causes_from_exceptions(self):
        import socket, ssl, urllib.error
        self.assertEqual(dl.cause_of(urllib.error.URLError(socket.gaierror(11001, 'getaddrinfo failed'))), 'offline')
        self.assertEqual(dl.cause_of(urllib.error.URLError(ssl.SSLCertVerificationError('CERTIFICATE_VERIFY_FAILED'))), 'tls')
        self.assertEqual(dl.cause_of(urllib.error.URLError(TimeoutError('timed out'))), 'timeout')
        self.assertEqual(dl.cause_of(urllib.error.URLError(ConnectionRefusedError(10061, 'refused'))), 'blocked')

    def test_fault_helper_failure_falls_back_to_the_plain_download(self):
        with mock.patch.object(dl, '_fetch', side_effect=RuntimeError('injected')):
            self.assertEqual(self.get('file').read_bytes(), ZIPBYTES)
        self.assertTrue(any('plain way' in l for l in self.lines))
        with mock.patch.object(dl, 'cached', side_effect=OSError('injected')):            # a broken cache = no cache
            used = []
            dl.fetch_and_use(self.url + 'file', self.tmp / 'c.zip', used.append, self.lines.append, cache_key='k')
            self.assertEqual(len(used), 1)

    def test_a_stale_part_without_a_digest_is_not_resumed(self):
        (self.tmp / 'out.zip.part').write_bytes(b'junk from an earlier run')
        self.assertEqual(self.get('file').read_bytes(), ZIPBYTES)
        self.assertIsNone(Handler.ranges[0])

    # TLS on a fresh Windows install (clean VM 11-19): Python can't verify github.com until Windows fetched the missing root.
    def tls_once(self, fail_times):
        """_once that fails certificate verification `fail_times` times (None: always), then downloads for real."""
        import ssl, urllib.error
        real, calls = dl._once, []
        def once(*a, **k):
            calls.append(1)
            if fail_times is None or len(calls) <= fail_times:
                raise urllib.error.URLError(ssl.SSLCertVerificationError(
                    1, '[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: unable to get local issuer certificate'))
            return real(*a, **k)
        return mock.patch.object(dl, '_once', once), calls

    def windows(self, body=ZIPBYTES, check=True):
        """A fake _windows_fetch: the connection check answers `check`, a download writes `body`."""
        seen = []
        def wf(url, out, max_bytes=None):
            seen.append(out)
            if out is None: return check
            Path(out).write_bytes(body); return True
        return mock.patch.object(dl, '_windows_fetch', wf), seen

    def last_record(self):
        return json.loads((Path(os.environ['LOCALAPPDATA']) / 'MechAssault2' / 'Logs' / 'downloads.log').read_text().splitlines()[-1])

    def test_tls_windows_check_then_python_retry_succeeds(self):
        once, calls = self.tls_once(1); win, seen = self.windows()
        with once, win:
            self.assertEqual(self.get('file').read_bytes(), ZIPBYTES)
        self.assertEqual(seen, [None]); self.assertEqual(len(calls), 2)               # one check, one Python retry, no Windows download
        self.assertEqual(self.last_record()['via'], 'after root update')
        self.assertTrue(any('check the secure connection: ok' in l for l in self.lines), self.lines)

    def test_tls_python_keeps_failing_windows_download_checked_by_sha256(self):
        import hashlib
        h = hashlib.sha256(ZIPBYTES).hexdigest()
        once, calls = self.tls_once(None); win, seen = self.windows()
        with once, win:
            self.assertEqual(self.get('file', digest=h).read_bytes(), ZIPBYTES)
        self.assertEqual(len(seen), 2); self.assertIsNone(seen[0]); self.assertEqual(Path(seen[1]).name, 'out.zip.part')
        self.assertEqual(self.last_record()['via'], 'windows')
        self.assertEqual((self.tmp / 'cache' / dl._cache_name(h)).read_bytes(), ZIPBYTES)          # cached like any other download
        self.assertFalse((self.tmp / 'out.zip.part').exists())

    def test_tls_windows_download_for_caller_checked_files_only(self):
        once, calls = self.tls_once(None); win, seen = self.windows()
        used = []
        with once, win:                                                  # fetch_and_use: the caller checks the file (FFmpeg, upscaler)
            dl.fetch_and_use(self.url + 'file', self.tmp / 'a.zip', lambda p: used.append(Path(p).read_bytes()), self.lines.append,
                             magic=b'PK\x03\x04')
        self.assertEqual(used, [ZIPBYTES])
        once, calls = self.tls_once(None); win, seen = self.windows()
        with once, win:                                                  # nothing checks it: Python's verdict stands
            self.assertEqual(self.cause('file').cause, 'tls')
        self.assertEqual(seen, [None])

    def test_tls_windows_download_with_wrong_content_is_refused(self):
        once, calls = self.tls_once(None)
        for body, kw in ((b'PK\x03\x04' + b'not the file' * 50, {'digest': '0' * 64}),          # SHA-256 differs
                         (b'<html>sign in</html>', {'checked': True})):                          # not even a zip
            win, seen = self.windows(body)
            with once, win:
                e = self.cause('file', **kw)
            self.assertEqual(e.cause, 'bad'); self.assertEqual(len(seen), 2)
            self.assertFalse((self.tmp / 'out.zip').exists()); self.assertFalse((self.tmp / 'out.zip.part').exists())
            self.assertFalse((self.tmp / 'cache').exists() and any((self.tmp / 'cache').iterdir()))
            self.assertEqual(self.last_record()['result'], 'failed: bad')

    def test_fault_windows_helper_raising_keeps_the_tls_error(self):
        import hashlib
        h = hashlib.sha256(ZIPBYTES).hexdigest()
        once, calls = self.tls_once(None)
        with once, mock.patch.object(dl, '_windows_fetch', side_effect=OSError('injected')):
            e = self.cause('file', digest=h)
        self.assertEqual(e.cause, 'tls'); self.assertIn('CERTIFICATE_VERIFY_FAILED', str(e))
        self.assertTrue(any('check unavailable' in l for l in self.lines), self.lines)
        def check_then_fail(url, out, max_bytes=None):
            if out is None: return True
            raise RuntimeError('injected')
        once, calls = self.tls_once(None)
        with once, mock.patch.object(dl, '_windows_fetch', check_then_fail):
            e = self.cause('file', digest=h)
        self.assertEqual(e.cause, 'tls'); self.assertTrue(any('Windows download unavailable' in l for l in self.lines), self.lines)
        self.assertFalse((self.tmp / 'out.zip.part').exists())
        self.assertEqual(len(calls), 2)                                  # first try + one retry after the check; no retry loop

    def test_tls_stop_text_does_not_blame_the_internet_connection(self):
        import upscaler_fetch
        with mock.patch.dict(os.environ, {'MA2_UPSCALER_SOURCE': ''}), \
             mock.patch.object(dl, 'fetch_and_use', side_effect=dl.DownloadError('tls', 'CERTIFICATE_VERIFY_FAILED')):
            with self.assertRaises(upscaler_fetch.FetchError) as cm:
                upscaler_fetch.ensure(self.tmp / 'up' / 'x', self.lines.append)
        msg = str(cm.exception)
        self.assertNotIn('internet connection', msg); self.assertIn('couldn\'t verify the secure connection', msg)
        self.assertIn('already tried Windows\' own connection', msg); self.assertIn(upscaler_fetch.SPEC['archive'], msg)
        with mock.patch.dict(os.environ, {'MA2_UPSCALER_SOURCE': ''}), \
             mock.patch.object(dl, 'fetch_and_use', side_effect=dl.DownloadError('offline', 'getaddrinfo failed')):
            with self.assertRaises(upscaler_fetch.FetchError) as cm:
                upscaler_fetch.ensure(self.tmp / 'up' / 'y', self.lines.append)
        self.assertIn('Check the internet connection', str(cm.exception))         # other causes unchanged
        self.assertIn('checked=True', inspect.getsource(install.install_build_tools))  # signed bootstrapper: Windows download allowed
        try:
            import ffmpeg_fetch                                          # (the public installer only)
        except ImportError:
            return
        with mock.patch.dict(os.environ, {'MA2_FFMPEG_SOURCE': ''}), \
             mock.patch.object(dl, 'fetch_and_use', side_effect=dl.DownloadError('tls', 'CERTIFICATE_VERIFY_FAILED')):
            with self.assertRaises(ffmpeg_fetch.FetchError) as cm:
                ffmpeg_fetch.ensure(self.tmp / 'ff' / 'x', self.lines.append)
        msg = str(cm.exception)
        self.assertNotIn('internet connection', msg); self.assertIn('already tried Windows\' own connection', msg)
        self.assertIn(ffmpeg_fetch.SPEC['archive'], msg); self.assertIn('press Resume', msg)

    def test_windows_fetch_runs_hidden_and_https_only(self):
        runs = []
        def run(args, **k):
            runs.append((args, k)); return mock.Mock(returncode=0)
        with mock.patch('subprocess.run', run):
            self.assertFalse(dl._windows_fetch('http://example.invalid/x', None))
            self.assertTrue(dl._windows_fetch('https://example.invalid/x', None))
        self.assertEqual(len(runs), 1); args, k = runs[0]
        self.assertTrue(args[0].lower().endswith(r'system32\curl.exe')); self.assertEqual(k['creationflags'], 0x08000000)
        self.assertIn('=https', args); self.assertNotIn('--fail', args)              # the check: any HTTP answer counts

    def test_used_by_every_downloader(self):
        import upscaler_fetch
        self.assertIn('dl.fetch_and_use', inspect.getsource(upscaler_fetch.ensure))
        self.assertIn('dl.fetch(', inspect.getsource(install.install_build_tools))
        for f in (KIT / 'enhancer' / 'setup.py', KIT / 'enhancer' / 'FMV' / 'setup_fmv.py'):
            self.assertIn('MA2_INSTALLER_DIR', f.read_text(encoding='utf-8'))
        self.assertEqual(os.environ.get('MA2_INSTALLER_DIR'), str(KIT / 'installer'))


def alive(pid):
    import ctypes
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)                    # PROCESS_QUERY_LIMITED_INFORMATION
    if not h: return False
    code = ctypes.c_ulong(); k.GetExitCodeProcess(h, ctypes.byref(code)); k.CloseHandle(h)
    return code.value == 259                                 # STILL_ACTIVE


class ToolTree(Base):
    """A6: tools run in a Job Object; Cancel ends the tool AND what it started; the exit code is recorded for diagnostics."""
    def test_cancel_ends_the_whole_tree(self):
        import threading
        inst = self.installer()
        pidfile = self.tmp / 'grandchild.pid'
        child = ('import subprocess, sys, time; p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(120)"], '
                 'creationflags=0x08000000); open(sys.argv[1], "w").write(str(p.pid)); print("started", flush=True); time.sleep(120)')
        errors = []
        th = threading.Thread(target=lambda: errors.append(self._run(inst, [sys.executable, '-c', child, pidfile])), daemon=True)
        th.start()
        for _ in range(200):
            if pidfile.is_file() and pidfile.read_text(): break
            REAL_SLEEP(0.05)
        gpid = int(pidfile.read_text())
        self.assertTrue(alive(gpid))
        self.assertTrue(getattr(inst.proc, 'in_job', False))
        inst.cancel(); th.join(20)
        self.assertFalse(th.is_alive())
        self.assertIsInstance(errors[0], install.Stop); self.assertEqual(errors[0].kind, 'cancelled')
        for _ in range(100):
            if not alive(gpid): break
            REAL_SLEEP(0.05)
        self.assertFalse(alive(gpid), 'the grandchild must end with the tool')

    def _run(self, inst, args):
        try:
            inst.run_cmd(args, title='tree test'); return None
        except install.Stop as e:
            return e

    def test_fault_job_object_unavailable_or_failing_still_runs_the_tool(self):
        inst = self.installer()
        with mock.patch.object(install.JobObject, 'popen', side_effect=OSError('injected')):
            inst.run_cmd([sys.executable, '-c', 'print("ran")'], title='plain start')
        self.assertTrue(any('ran' in l for l in self.lines)); self.assertTrue(any('starting it normally' in l for l in self.lines))
        with mock.patch.object(install, '_TOOL_JOB', None), mock.patch.object(install, 'JobObject', side_effect=OSError('no jobs here')):
            self.assertIsNone(install.tool_job())
            inst.run_cmd([sys.executable, '-c', 'print("ran2")'], title='no job')
        self.assertTrue(any('ran2' in l for l in self.lines))

    def test_exit_codes_are_recorded(self):
        inst = self.installer()
        with self.assertRaises(install.Stop):
            inst.run_cmd([sys.executable, '-c', 'import sys; sys.exit(3)'], title='three')
        self.assertTrue(any('three: exit code 3' in e for e in inst.state['events']), inst.state['events'])
        self.assertNotIn('taskkill', inspect.getsource(install.JobObject))


class Space(Base):
    """B1: every drive the install writes is counted; the core can fit while only the extras do not."""
    def plan(self, free, **kw):
        du = lambda p: type('U', (), {'free': free[os.path.splitdrive(os.path.abspath(str(p)))[0].upper()]})()
        with mock.patch.object(install.shutil, 'disk_usage', du), mock.patch.dict(os.environ, {'TEMP': 'C:\\T', 'SystemDrive': 'C:'}):
            return install.space_plan('D:\\Games\\MA2', **kw)

    def test_drives_and_core_vs_extras(self):
        p = self.plan({'D:': 100 * G, 'C:': 50 * G}, opts={'textures': ['4x'], 'videos': False})
        self.assertEqual(sorted(d['drive'] for d in p['drives']), ['C:', 'D:']); self.assertTrue(p['all_fit'])
        p = self.plan({'D:': 6 * G, 'C:': 50 * G}, opts={'textures': ['4x'], 'videos': False})
        self.assertTrue(p['core_fits']); self.assertFalse(p['all_fit']); self.assertEqual(p['short'], ['D:'])
        p = self.plan({'D:': 100 * G, 'C:': 3 * G}, opts={'textures': [], 'videos': False}, build_tools=True)
        self.assertFalse(p['core_fits']); self.assertEqual(p['short'], ['C:'])
        p = self.plan({'D:': 100 * G, 'C:': 1 * G}, opts={'textures': ['2x'], 'videos': False})
        self.assertTrue(p['core_fits']); self.assertEqual(p['short'], ['C:'])           # TEMP short: only the extras are at risk

    def test_fault_unreadable_drive_counts_as_fitting(self):
        with mock.patch.object(install.shutil, 'disk_usage', side_effect=OSError('injected')):
            p = install.space_plan(str(self.tmp), {'textures': ['2x'], 'videos': True})
        self.assertTrue(p['core_fits'] and p['all_fit'])
        with mock.patch.object(install, 'needed_space', side_effect=RuntimeError('injected')):
            self.assertTrue(install.space_plan(str(self.tmp), {'textures': [], 'videos': False})['core_fits'])


class Graphics(Base):
    """B3: DXGI first (real adapters, dedicated memory), registry merged in / used alone on any DXGI problem (1.1.3)."""
    REG = [{'name': 'Intel(R) UHD Graphics', 'vram': 128 << 20, 'vendor': 'intel', 'driver': '31.0'},
           {'name': 'AMD Radeon RX 7600', 'vram': 8 * G, 'vendor': 'amd', 'driver': '32.1'}]

    def detect(self, dx):
        with mock.patch.object(precheck, '_registry_gpus', lambda: [dict(g) for g in self.REG]), \
             mock.patch.object(precheck, '_nvidia_smi', lambda g: g), mock.patch.object(precheck, 'dxgi_adapters', dx):
            return precheck.detect_gpus()

    def test_fault_dxgi_error_or_empty_is_the_registry_list(self):
        self.assertEqual(self.detect(mock.Mock(side_effect=OSError('injected'))), self.REG)
        self.assertEqual(self.detect(lambda: []), self.REG)

    def test_dxgi_first_registry_vendors_kept_and_hybrid_choice(self):
        dx = [{'name': 'Intel(R) UHD Graphics', 'vram': 0, 'vendor': 'intel', 'vendor_id': '8086', 'device_id': '9A49', 'source': 'dxgi'},
              {'name': 'NVIDIA GeForce RTX 4060 Laptop GPU', 'vram': 8 * G, 'vendor': 'nvidia', 'vendor_id': '10DE', 'device_id': '28E0', 'source': 'dxgi'}]
        g = self.detect(lambda: [dict(x) for x in dx])
        self.assertEqual([x['vendor'] for x in g], ['intel', 'nvidia', 'amd'])     # AMD only in the registry: kept
        self.assertEqual(g[0]['driver'], '31.0')                                   # driver version from the registry entry
        self.assertEqual(precheck.recommend_textures(g)['gpu'], 'NVIDIA GeForce RTX 4060 Laptop GPU')

    def test_dxgi_live(self):
        try:
            found = precheck.dxgi_adapters()
        except OSError as e:
            self.skipTest('no DXGI here (%s)' % e)
        for a in found: self.assertTrue(a['name'] and a['vendor_id'] != '1414')


class Platform(Base):
    """B5: native architecture and Insider builds are logged / noted, never blocking."""
    def test_note(self):
        self.assertIn(precheck.native_arch(), ('x64', 'ARM64'))
        with mock.patch.object(precheck, 'native_arch', lambda: 'x64'), mock.patch.object(precheck, 'insider_build', lambda: False):
            self.assertEqual(precheck.platform_note(), '')
        with mock.patch.object(precheck, 'native_arch', lambda: 'ARM64'), mock.patch.object(precheck, 'insider_build', lambda: True):
            n = precheck.platform_note(); self.assertIn('ARM64', n); self.assertIn('Insider', n); self.assertIn('continues', n)
        with mock.patch.object(precheck, 'native_arch', side_effect=RuntimeError('injected')):
            self.assertEqual(precheck.platform_note(), '')
        self.assertTrue(precheck.windows_info()['ok'] in (True, False))
        self.assertIn('platform_note()', inspect.getsource(install.Installer.check))


class Diagnostics(Base):
    """D1 session id; D2 the new system.txt facts (same zip members as 1.1.3; any failing part is left out)."""
    def test_session_id(self):
        with mock.patch.dict(os.environ, {'MA2_SESSION_ID': 'ABCD1234'}):
            self.assertEqual(precheck.session_id(), 'ABCD1234')
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('MA2_SESSION_ID', None)
            sid = precheck.session_id(); self.assertEqual(len(sid), 8); self.assertEqual(os.environ['MA2_SESSION_ID'], sid)
        self.assertIn('precheck.session_id()', inspect.getsource(install.Installer._run))

    def zip_text(self, res):
        import zipfile
        with zipfile.ZipFile(res['path']) as z: return z.read('system.txt').decode('utf-8'), sorted(z.namelist())

    def test_new_facts_in_system_txt(self):
        dest = self.tmp / 'game'; (dest / '_work').mkdir(parents=True)
        (dest / '_work' / 'state.json').write_text(json.dumps({'done': ['check'], 'events': ['10:00:00 tool: CMake + Ninja: exit code 0 (5m 00s)']}))
        logs = precheck.logs_dir(); logs.mkdir(parents=True, exist_ok=True)
        (logs / 'downloads.log').write_text('{"host": "github.com", "result": "ok"}\n')
        with mock.patch.object(precheck, 'security_facts', lambda: {'antivirus': ['Some AV'], 'cfa': 'on', 'sac': 'off'}):
            txt, names = self.zip_text(precheck.collect_diagnostics(dest, self.tmp / 'out', '1.1.4', 'k', [{'name': 'GPU X', 'vram': 8 * G, 'vendor': 'amd', 'vendor_id': '1002', 'driver': '1.2'}]))
        for want in ('Session: ', 'Processor: ', 'Windows SDK: ', 'Drive ', 'Adapter: GPU X, 8.0 GB, vendor 1002, driver 1.2',
                     'Controlled Folder Access on', 'Some AV', 'Download: {"host": "github.com"', 'Event: 10:00:00 tool: CMake + Ninja: exit code 0'):
            self.assertIn(want, txt)
        self.assertNotIn('logs/downloads.log', names)                              # summarised, not a member (the 1.1.3 member list stays)

    def test_fault_a_failing_part_is_left_out(self):
        dest = self.tmp / 'g2'; dest.mkdir()
        with mock.patch.object(precheck, 'security_facts', side_effect=RuntimeError('injected')), \
             mock.patch.object(precheck, 'native_arch', side_effect=RuntimeError('injected')):
            txt, names = self.zip_text(precheck.collect_diagnostics(dest, self.tmp / 'out2', '1', 'k', []))
        self.assertIn('Windows build', txt); self.assertEqual(names, ['system.txt'])
        with mock.patch.object(precheck, 'diagnostics_facts', side_effect=RuntimeError('injected')):
            txt, _ = self.zip_text(precheck.collect_diagnostics(dest, self.tmp / 'out3', '1', 'k', []))
        self.assertIn('Windows build', txt)

    def test_security_facts_shape(self):
        precheck._SECURITY = None
        s = precheck.security_facts()
        self.assertEqual(set(s), {'antivirus', 'cfa', 'sac'}); self.assertIsInstance(s['antivirus'], list)


class PipTls(Base):
    """A5: pip runs exactly as before; only a certificate failure gets the truststore retry (pip 23.3-24.1) or one plain message."""
    def setUp(self):
        super().setUp()
        import importlib.util
        spec = importlib.util.spec_from_file_location('enh_setup_v114', KIT / 'enhancer' / 'setup.py')
        self.m = importlib.util.module_from_spec(spec); spec.loader.exec_module(self.m)
        self.m.child_env = lambda: dict(os.environ)

    def fake(self, code, out):
        class P:
            def __init__(s, *a, **k): s.stdout = [l.encode() + b'\n' for l in out]
            def wait(s): return code
        return P

    def test_success_and_plain_failure_unchanged(self):
        with mock.patch.object(self.m.subprocess, 'Popen', self.fake(0, ['Successfully installed numpy'])):
            self.assertEqual(self.m.pip_install(sys.executable, ['numpy'], 'pip install numpy'), 0)
        with mock.patch.object(self.m.subprocess, 'Popen', self.fake(1, ['ERROR: No matching distribution'])):
            with self.assertRaises(RuntimeError) as cm: self.m.pip_install(sys.executable, ['numpy'], 'pip install numpy')
        self.assertIn('failed (exit code 1)', str(cm.exception))

    def test_certificate_failure(self):
        tls = ['SSLError(SSLCertVerificationError(1, "[SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: self-signed certificate"))']
        runs = []
        with mock.patch.object(self.m.subprocess, 'Popen', self.fake(1, tls)), mock.patch.object(self.m, 'pip_version', lambda py: (24, 0)), \
             mock.patch.object(self.m, 'run', lambda args, what, capture=False: (runs.append(args), 0)[1]):
            self.assertEqual(self.m.pip_install(sys.executable, ['numpy'], 'pip install numpy'), 0)
        self.assertIn('--use-feature=truststore', runs[0])
        with mock.patch.object(self.m.subprocess, 'Popen', self.fake(1, tls)), mock.patch.object(self.m, 'pip_version', lambda py: (25, 1)):
            with self.assertRaises(RuntimeError) as cm: self.m.pip_install(sys.executable, ['numpy'], 'pip install numpy')
        self.assertIn('intercepts secure (HTTPS) connections', str(cm.exception))

    def test_pip_version_fault(self):
        with mock.patch.object(self.m.subprocess, 'run', side_effect=OSError('injected')):
            self.assertEqual(self.m.pip_version(sys.executable), (0, 0))


class SilentSecurity(Base):
    """B2 silent: one notice only when Smart App Control enforces; never on error."""
    def test_notice(self):
        for state, want in (('enforce', precheck.SAC_NOTICE), ('eval', None), ('off', None), (None, None)):
            with mock.patch.object(precheck, 'smart_app_control', lambda s=state: s):
                self.assertEqual(precheck.sac_notice(), want)
        with mock.patch.object(precheck, 'smart_app_control', side_effect=OSError('injected')):
            self.assertIsNone(precheck.sac_notice())
        self.assertIn("can't be turned back on", precheck.SAC_NOTICE)


STUB = WORK.parent / 'installer-exe' / 'stub.exe'


class Stub(Base):
    """D1 + D3 in the native stub: a session id in its log; when Python cannot start, a start-up report in Diagnostics.
    (Runs the public stub built from installer-exe; an older stub without these is skipped. TEMP and LOCALAPPDATA are redirected:
    the stub cleans other MA2RecompInstaller-* folders in TEMP.)"""
    def test_startup_report_when_python_cannot_start(self):
        import struct, subprocess
        if not STUB.is_file(): self.skipTest('no stub.exe')
        stub = STUB.read_bytes()
        if 'MA2-startup-'.encode('utf-16-le') not in stub: self.skipTest('this stub predates the start-up report')
        payload = b'MA2KIT01' + struct.pack('<I', 0)
        exe = self.tmp / 'inst.exe'
        exe.write_bytes(stub + payload + struct.pack('<QQ', len(stub), len(payload)) + b'00000000000000v4' + b'MA2KEND1')
        la, tmp = self.tmp / 'LA', self.tmp / 'T'; la.mkdir(); tmp.mkdir()
        env = dict(os.environ, LOCALAPPDATA=str(la), TEMP=str(tmp), TMP=str(tmp), MA2_SESSION_ID='CAFE1234')
        r = subprocess.run([str(exe), '--help'], capture_output=True, text=True, timeout=120, env=env, creationflags=0x08000000)
        self.assertNotEqual(r.returncode, 0)
        logs = list((la / 'MechAssault2' / 'Logs').glob('stub-*.log'))
        self.assertTrue(logs and 'session CAFE1234' in logs[0].read_text(encoding='utf-8-sig'))
        reports = list((la / 'MechAssault2' / 'Diagnostics').glob('MA2-startup-*.txt'))
        self.assertEqual(len(reports), 1, r.stderr)
        text = reports[0].read_text(encoding='utf-8-sig')
        self.assertIn('Python could not be started', text); self.assertIn('session CAFE1234', text)


class Reboot(Base):
    """A9: a pending restart is only logged before the Build Tools install; a build failure while VS waits for a restart names it;
    the after-restart resume entry (RunOnce) is written / removed without touching anything else (registry faked here)."""
    def test_reboot_pending_and_its_fallback(self):
        with mock.patch.object(install, 'vs_instances', lambda: [{'isRebootRequired': True}]):
            self.assertTrue(install.reboot_pending()['vs'])
        with mock.patch.object(install, 'vs_instances', side_effect=RuntimeError('injected')):
            self.assertFalse(install.reboot_pending()['vs'])

    def test_build_failure_names_the_pending_restart(self):
        br = BuildRecovery(); br.tmp, br.lines = self.tmp, self.lines
        inst = self.installer()
        with mock.patch.object(install, 'reboot_pending', lambda: {'windows': False, 'vs': True}):
            with self.assertRaises(install.Stop) as cm:
                br.fake_build(inst, [('compile_error', ['error C2065: x'])])
        self.assertIn('restart the PC', str(cm.exception)); self.assertEqual(cm.exception.kind, 'compile_error')
        with mock.patch.object(install, 'reboot_pending', lambda: {'windows': True, 'vs': False}):
            with self.assertRaises(install.Stop) as cm:
                br.fake_build(inst, [('compile_error', ['error C2065: x'])])
        self.assertNotIn('restart the PC', str(cm.exception))                    # Windows Update alone: unchanged message

    def test_runonce_entry(self):
        store = {}
        class Key:
            def __enter__(s): return s
            def __exit__(s, *a): return False
        fake = mock.MagicMock()
        fake.CreateKey = lambda root, path: (store.setdefault('path', path), Key())[1]
        fake.OpenKey = lambda root, path, *a: Key()
        fake.SetValueEx = lambda k, name, r, t, v: store.__setitem__(name, v)
        def delete(k, name):
            if name not in store: raise FileNotFoundError(name)
            del store[name]
        fake.DeleteValue = delete
        with mock.patch.dict(sys.modules, {'winreg': fake}):
            self.assertTrue(install.schedule_resume_after_restart('"X.exe" --resume'))
            self.assertEqual(store[install.RESUME_VALUE], '"X.exe" --resume'); self.assertTrue(store['path'].endswith('RunOnce'))
            self.assertTrue(install.clear_resume_after_restart()); self.assertFalse(install.clear_resume_after_restart())
        bad = mock.MagicMock(); bad.CreateKey.side_effect = OSError('injected')
        with mock.patch.dict(sys.modules, {'winreg': bad}):
            self.assertFalse(install.schedule_resume_after_restart('x'))


class LockHolders(Base):
    """A7: a file still locked after the retries names the program holding it (Restart Manager); any error keeps the 1.1.3 error."""
    def hold(self, path):
        import subprocess
        code = 'import sys, time; f = open(sys.argv[1], "rb"); print("held", flush=True); time.sleep(60)'
        p = subprocess.Popen([sys.executable, '-c', code, str(path)], stdout=subprocess.PIPE, creationflags=0x08000000)
        p.stdout.readline()
        self.addCleanup(lambda: (p.kill(), p.wait()))
        return p

    def test_the_holder_is_named(self):
        f = self.tmp / 'MechAssault 2.exe'; f.write_text('x'); p = self.hold(f)
        self.assertTrue(any(('pid %d' % p.pid) in h for h in install.lock_holders([f])), install.lock_holders([f]))
        with self.assertRaises(install.Stop) as cm:
            install.retry_io(lambda: f.rename(self.tmp / 'y'), 'replace MechAssault 2.exe', self.lines.append, path=f)
        self.assertEqual(cm.exception.kind, 'access_denied')
        self.assertIn('using MechAssault 2.exe. Close', str(cm.exception)); self.assertIn('Resume', str(cm.exception))

    def test_fault_restart_manager_error_keeps_the_old_error(self):
        f = self.tmp / 'locked.dll'; f.write_text('x'); self.hold(f)
        with mock.patch.object(install, '_lock_holders', side_effect=OSError('injected')):
            self.assertEqual(install.lock_holders([f]), [])
            with self.assertRaises(OSError) as cm:
                install.retry_io(lambda: f.rename(self.tmp / 'z'), 'replace locked.dll', self.lines.append, path=f)
        self.assertNotIsInstance(cm.exception, install.Stop)
        self.assertEqual(install.lock_holders([self.tmp / 'missing']), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)

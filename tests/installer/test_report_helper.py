"""1.1.6 report helper ("MA2 Report.exe", report-helper/report.c): the program the game's crash and freeze boxes, the in-game
"Report a problem" page and the Start menu entry start. Every run here is hidden and points the helper at its own folders
(MA2_REPORT_DATA_DIR / MA2_REPORT_INSTALL_DIR) and at the local mock relay (MA2_REPORT_RELAY), so nothing real is read or sent.

    report-helper\\build_report.cmd            (build the helper first)
    python -B tests\\installer\\test_report_helper.py

What it proves: the bundle (members, a zip Python opens with right CRCs, the size cap that drops the dump), the privacy of every
member (a planted user / machine / domain name, profile paths, 8.3 names and e-mail addresses in logs, the ini, the stamp, a dump,
a bot recording and the player's own words: privacy_scan finds none of them, the dump keeps its length, the bot recording still
unpacks), every relay answer (sent, 429, 500, ok:false + message, not JSON, redirect not followed, timeout, nothing listening,
relay not set up), the in-game mode's result file, the window (hidden) with Send / Keep waiting / Close the game, and the fault
switches (collect, zip, send): each ends in a saved zip or a clear "not saved", never a crash.
"""
import ctypes, ctypes.wintypes as wt, json, os, re, shutil, struct, subprocess, sys, tempfile, time, unittest, zipfile
from pathlib import Path
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import isolate                                             # noqa: E402  (11-25: never the real %LOCALAPPDATA%\MechAssault2; first, before any installer import)

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
HELPER = REPO / 'report-helper' / 'MA2 Report.exe'
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(REPO / 'tools'))
from mock_relay import MockRelay                                       # noqa: E402
import privacy_scan                                                     # noqa: E402

WORK = HERE.parent / 'tmp'
WORK.mkdir(exist_ok=True)
NO_WINDOW = 0x08000000
FAKE = dict(USERNAME='Zebulon7Q', COMPUTERNAME='QUOKKA-PC9', USERDOMAIN='QUOKKA-PC9')
AT = chr(64)                                     # built at run time: the repo's own privacy scan must not find planted values here
MAILS = ['z.q+x' + AT + 'example.co.uk', 'zq' + AT + 'mail.example.org']
BS = chr(92)
ID_RX = re.compile(r'^MA2-[A-Z2-7]{6}$')


def mszip(raw):
    """Pack one block with the Windows Compression API (what the game's recorder does)."""
    cab = ctypes.WinDLL('cabinet')
    h = ctypes.c_void_p()
    assert cab.CreateCompressor(2, None, ctypes.byref(h))
    out = ctypes.create_string_buffer(len(raw) + 65536); n = ctypes.c_size_t()
    assert cab.Compress(h, raw, ctypes.c_size_t(len(raw)), out, ctypes.c_size_t(len(out)), ctypes.byref(n))
    cab.CloseCompressor(h)
    return out.raw[:n.value]


def unmszip(data, raw_len):
    cab = ctypes.WinDLL('cabinet')
    h = ctypes.c_void_p()
    assert cab.CreateDecompressor(2, None, ctypes.byref(h))
    out = ctypes.create_string_buffer(raw_len + 16); n = ctypes.c_size_t()
    ok = cab.Decompress(h, data, ctypes.c_size_t(len(data)), out, ctypes.c_size_t(raw_len), ctypes.byref(n))
    cab.CloseDecompressor(h)
    assert ok, 'block does not unpack'
    return out.raw[:n.value]


def botrec_blocks(data):
    assert data[:8] == b'MA2BREC1'
    pos, out = 8, []
    while pos + 16 <= len(data):
        magic, raw_len, comp_len, algo = struct.unpack_from('<4I', data, pos)
        assert magic == 0x31425242
        body = data[pos + 16:pos + 16 + comp_len]
        out.append(unmszip(body, raw_len) if algo else body)
        pos += 16 + comp_len
    return out


class Helper(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HELPER.is_file():
            raise unittest.SkipTest('build the helper first: report-helper\\build_report.cmd')
        cls.relay = MockRelay().start()

    @classmethod
    def tearDownClass(cls):
        cls.relay.stop()

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2rep-', dir=str(WORK)))
        self.relay.mode = 'ok'; self.relay.message = ''; self.relay.requests.clear()
        prof = self.tmp / 'Users' / FAKE['USERNAME']
        self.local = prof / 'AppData' / 'Local'
        self.data = self.local / 'MechAssault2'
        self.logs = self.data / 'Logs'; self.logs.mkdir(parents=True)
        self.install = self.tmp / 'Games' / 'MechAssault 2'; self.install.mkdir(parents=True)
        self.prof = prof
        self.env = dict(os.environ)
        for k in list(self.env):
            if k.startswith(('MA2_REPORT', 'RECOMP_')): self.env.pop(k)
        self.env.update(FAKE, USERPROFILE=str(prof), LOCALAPPDATA=str(self.local), APPDATA=str(prof / 'AppData' / 'Roaming'),
                        MA2_REPORT_DATA_DIR=str(self.data), MA2_REPORT_INSTALL_DIR=str(self.install), MA2_REPORT_RELAY=self.relay.url)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # -- planted files
    def plant(self, big=False):
        p = str(self.prof)
        short = 'C:' + BS + 'Users' + BS + 'ZEBULO~1'
        stamp = {'kit_id': 'abcdef0123456789', 'public_version': '1.1.6', 'shortcuts': [p + BS + 'Desktop' + BS + 'MechAssault 2 (Recomp).lnk']}
        (self.install / 'recomp-install.json').write_text(json.dumps(stamp, indent=1), encoding='utf-8')
        (self.data / 'pc_settings.ini').write_text('[Display]\nWindowWidth=1920\nWindowHeight=1080\nFrameLimit=144\n[Audio]\nThreads=auto\n'
                                                   '; saved by ' + FAKE['USERNAME'] + ' ' + MAILS[0] + '\n', encoding='utf-8')
        log = ('[LAUNCH] build version=1.1.6\n[LAUNCH] settings=' + str(self.local) + BS + 'MechAssault2' + BS + 'pc_settings.ini\n'
               'opened ' + short + BS + 'AppData' + BS + 'x.ini on ' + FAKE['COMPUTERNAME'] + ' by ' + FAKE['USERNAME'] + ' mail ' + MAILS[1] + '\n'
               '[HEALTH] t=60 why=periodic fps=60.0 p50=16.5 p99=20.0 underruns=0\n')
        if big:
            log += os.urandom(3 * 1024 * 1024).hex()      # 6 MB of text that does not pack
        for i, name in enumerate(('20261121-100000-11.log', '20261121-110000-22.log', '20261121-120000-33.log', '20261120-090000-44.log')):
            f = self.logs / name; f.write_text(log + ('\nlog %d\n' % i), encoding='utf-8')
            os.utime(f, (time.time() - 3600 + (i if i < 3 else -10) * 60,) * 2)
        (self.logs / 'install-20261120-1.log').write_text('install log at ' + p + ' for ' + FAKE['USERNAME'] + '\n', encoding='utf-8')
        # a dump: names in UTF-16 and ASCII, a profile path, an 8.3 path, an e-mail, some binary around them
        blob = bytearray(os.urandom(4096) if not big else os.urandom(10 * 1024 * 1024))
        for i, s in enumerate((p, short + BS + 'a', FAKE['USERNAME'], FAKE['COMPUTERNAME'], MAILS[0])):
            blob += b'\0' * 8 + s.encode('utf-16-le') + b'\0\0' + s.encode('ascii') + b'\0' + bytes(range(i, i + 16))
        (self.logs / 'crash-4242.dmp').write_bytes(bytes(blob))
        self.dump_len = len(blob)
        # a bot recording: the header names the exe and the file with full paths; two packed blocks + one stored
        info = ('version=1\nexe=' + str(self.install) + BS + 'MechAssault 2.exe\npath=' + str(self.logs) + BS + 'x.botrec\nnote=' + FAKE['USERNAME'] + '\n').encode('latin-1')
        rec1 = struct.pack('<BBHI', 1, 0, 0, len(info)) + info + bytes(2000)
        rec2 = bytes(range(256)) * 40 + p.encode('utf-16-le') + bytes(500)
        data = bytearray(b'MA2BREC1')
        for raw, packed in ((rec1, True), (rec2, True), (b'tail' * 100, False)):
            body = mszip(raw) if packed else raw
            data += struct.pack('<4I', 0x31425242, len(raw), len(body), 2 if packed else 0) + body
        (self.logs / '20261121-120000-33.botrec').write_bytes(bytes(data))
        self.rec_raw = [rec1, rec2, b'tail' * 100]

    def run_helper(self, *args, timeout=120, env=None):
        return subprocess.run([str(HELPER)] + list(args), env=env or self.env, timeout=timeout, creationflags=NO_WINDOW,
                              capture_output=True)

    def game_mode(self, text='The game froze after a jump.', env=None, extra=()):
        tf = self.tmp / 'text.txt'; tf.write_text(text, encoding='utf-8')
        rf = self.tmp / 'result.txt'
        r = self.run_helper('--game', '--text-file', str(tf), '--result', str(rf), *extra, env=env)
        res = dict(l.split('=', 1) for l in rf.read_text(encoding='utf-8').splitlines() if '=' in l) if rf.is_file() else {}
        return r.returncode, res

    def saved_zips(self):
        return sorted((self.data / 'Diagnostics').glob('MA2-report-*.zip')) if (self.data / 'Diagnostics').is_dir() else []

    def assert_private(self, zbytes):
        """privacy_scan's patterns (with the planted user / machine name) plus the planted e-mails, over every member and every
        unpacked bot recording block."""
        path = self.tmp / 'check.zip'; path.write_bytes(zbytes)
        old = dict(os.environ)
        try:
            os.environ.update(FAKE)
            pats = privacy_scan.patterns()
            with zipfile.ZipFile(path) as z:
                self.assertIsNone(z.testzip())
                for n in z.namelist():
                    data = z.read(n)
                    blobs = [data] + (botrec_blocks(data) if n.startswith('botrec/') else [])
                    for blob in blobs:
                        for t in privacy_scan.texts(blob) + [blob.decode('utf-16-le', 'ignore'), blob[1:].decode('utf-16-le', 'ignore')]:
                            for rx in pats:
                                m = rx.search(t)
                                self.assertIsNone(m, '%s: %r' % (n, m and m.group(0)))
                            for bad in [FAKE['USERNAME'], FAKE['USERNAME'].lower(), 'QUOKKA', 'quokka', 'ZEBULO~1'] + MAILS:
                                self.assertNotIn(bad, t, n)
        finally:
            os.environ.clear(); os.environ.update(old)

    # ------------------------------------------------------------------ the bundle and privacy
    def test_sent_report_bundle_and_privacy(self):
        self.plant()
        code, res = self.game_mode('It froze when ' + FAKE['USERNAME'] + ' (' + MAILS[0] + ') jumped at ' + str(self.prof) + BS + 'x')
        self.assertEqual(code, 0, res); self.assertEqual(res['ok'], '1'); self.assertRegex(res['id'], ID_RX)
        self.assertEqual(res['saved'], '1')
        self.assertEqual(len(self.relay.requests), 1)
        rq = self.relay.requests[0]
        self.assertEqual(rq['path'], '/v1/report')
        h = rq['headers']
        self.assertEqual(h['x-ma2-client'], 'game'); self.assertEqual(h['x-ma2-kind'], 'manual'); self.assertEqual(h['x-ma2-version'], '1.1.6')
        self.assertEqual(h['x-ma2-build'], 'abcdef0123456789'); self.assertIn('MechAssault2-Recomp-Report/1.1.6', h['user-agent'])
        f = rq['fields']
        for k in ('client', 'version', 'build', 'kind', 'signature', 'text', 'summary'): self.assertIn(k, f)
        self.assertIn('froze', f['text']); self.assertIn('%USERNAME%', f['text']); self.assertNotIn(FAKE['USERNAME'], f['text'])
        self.assertIn('MechAssault 2 Recomp 1.1.6', f['summary']); self.assertIn('[HEALTH] t=60', f['summary'])
        self.assertTrue(rq['zip'].startswith(b'PK\x03\x04'))
        zips = self.saved_zips(); self.assertEqual(len(zips), 1)
        self.assertEqual(zips[0].read_bytes(), rq['zip'], 'the saved copy is what was sent')
        with zipfile.ZipFile(zips[0]) as z:
            names = z.namelist()
            self.assertIsNone(z.testzip())
            for want in ('system.txt', 'recomp-install.json', 'pc_settings.ini', 'what-happened.txt', 'dumps/crash-4242.dmp',
                         'botrec/20261121-120000-33.botrec', 'logs/install-20261120-1.log'):
                self.assertIn(want, names)
            self.assertEqual(len([n for n in names if re.match(r'logs/2026.*\.log$', n)]), 3, names)   # the newest three game logs
            self.assertNotIn('logs/20261120-090000-44.log', names)
            self.assertEqual(len(z.read('dumps/crash-4242.dmp')), self.dump_len, 'the dump keeps its length')
            blocks = botrec_blocks(z.read('botrec/20261121-120000-33.botrec'))
            self.assertEqual([len(b) for b in blocks], [len(b) for b in self.rec_raw], 'every block unpacks to its length')
            self.assertIn(b'version=1', blocks[0])
            system = z.read('system.txt').decode('utf-8')
            self.assertIn('Included:', system); self.assertIn('private name(s)', system)
        self.assert_private(rq['zip'])
        self.assert_private(z_bytes := zips[0].read_bytes())
        self.assertTrue(z_bytes)

    def test_crash_window_sends_kind_crash(self):
        self.plant()
        env = dict(self.env, MA2_REPORT_HIDDEN='1')
        dump = self.logs / 'crash-4242.dmp'
        p = subprocess.Popen([str(HELPER), '--crash', str(dump), str(self.logs / '20261121-120000-33.log'), '--sig', '0xC0000005:rva1A2B3C'],
                             env=env, creationflags=NO_WINDOW)
        try:
            w = Window(p.pid)
            self.assertIn('stopped unexpectedly', w.text(ID_HEAD))
            w.set_text(ID_EDIT, 'crashed on load')
            w.click(ID_SEND)
            head = w.wait_text(ID_HEAD, 'Report sent', 60)
            self.assertRegex(head.split('number is ')[1].rstrip('.'), ID_RX)
            w.click(ID_CLOSE)
            self.assertEqual(p.wait(20), 0)
        finally:
            if p.poll() is None: p.kill()
        f = self.relay.requests[-1]['fields']
        self.assertEqual(f['kind'], 'crash'); self.assertEqual(f['client'], 'helper'); self.assertEqual(f['signature'], '0xC0000005:rva1A2B3C')
        self.assertIn('crashed on load', f['text']); self.assertIn('Failure: the game crashed', f['summary'])

    def test_hang_window_buttons(self):
        self.plant()
        env = dict(self.env, MA2_REPORT_HIDDEN='1')
        for button, code in ((ID_WAIT, 11), (ID_CLOSE, 10)):
            p = subprocess.Popen([str(HELPER), '--hang', str(self.logs / 'crash-4242.dmp'), str(self.logs / '20261121-120000-33.log')],
                                 env=env, creationflags=NO_WINDOW)
            try:
                w = Window(p.pid)
                self.assertIn('stopped responding', w.text(ID_HEAD))
                self.assertEqual(w.text(ID_CLOSE), 'Close the game'); self.assertEqual(w.text(ID_WAIT), 'Keep waiting')
                if button == ID_CLOSE:                       # send first, then close the game from the result page
                    w.click(ID_SEND); w.wait_text(ID_HEAD, 'Report sent', 60)
                w.click(button)
                self.assertEqual(p.wait(20), code)
            finally:
                if p.poll() is None: p.kill()
        self.assertEqual(self.relay.requests[-1]['fields']['kind'], 'hang')
        self.assertEqual(self.relay.requests[-1]['fields']['signature'], 'hang')

    def test_manual_window_relay_unset_saves_only(self):
        self.plant()
        env = dict(self.env, MA2_REPORT_HIDDEN='1'); env.pop('MA2_REPORT_RELAY')
        p = subprocess.Popen([str(HELPER)], env=env, creationflags=NO_WINDOW)
        try:
            w = Window(p.pid)
            self.assertEqual(w.text(ID_HEAD), 'Report a problem')
            self.assertEqual(w.text(ID_SEND), 'Save report')
            w.click(ID_SEND)
            self.assertEqual(w.wait_text(ID_HEAD, 'Report saved', 60), 'Report saved.')
            self.assertIn('Diagnostics', w.text(ID_RESULT))
            w.click(ID_CLOSE); self.assertEqual(p.wait(20), 0)
        finally:
            if p.poll() is None: p.kill()
        self.assertEqual(self.relay.requests, [])
        self.assertEqual(len(self.saved_zips()), 1)

    # ------------------------------------------------------------------ relay answers
    def test_relay_answers(self):
        self.plant()
        cases = [('429', 'rate_limited', 'Too many reports'), ('500', 'server', ''), ('okfalse', 'refused', 'Please update'),
                 ('badjson', 'server', ''), ('redirect', 'server', '')]
        for mode, cause, msg in cases:
            with self.subTest(mode=mode):
                self.relay.mode = mode; n = len(self.relay.requests)
                code, res = self.game_mode()
                self.assertEqual(code, 1, res); self.assertEqual(res['ok'], '0'); self.assertEqual(res['cause'], cause)
                self.assertEqual(res['saved'], '1'); self.assertIn('Diagnostics', res['folder'])
                if msg: self.assertIn(msg, res['message'])
                self.assertEqual(len(self.relay.requests), n + 1, 'one request, no redirect followed')
        self.assertEqual(len(self.saved_zips()), len(cases))

    def test_timeout_and_nothing_listening(self):
        self.plant()
        self.relay.mode = 'slow'; self.relay.slow_seconds = 6
        code, res = self.game_mode(env=dict(self.env, MA2_REPORT_TIMEOUT_MS='1500'))
        self.assertEqual((code, res['cause']), (1, 'timeout'))
        import socket
        s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
        code, res = self.game_mode(env=dict(self.env, MA2_REPORT_RELAY='http://127.0.0.1:%d' % port))
        self.assertEqual(code, 1); self.assertIn(res['cause'], ('blocked', 'offline', 'server')); self.assertEqual(res['saved'], '1')

    def test_relay_unset_and_can_send(self):
        self.plant()
        env = dict(self.env); env.pop('MA2_REPORT_RELAY')
        self.assertEqual(self.run_helper('--can-send', env=env).returncode, 3)
        self.assertEqual(self.run_helper('--can-send').returncode, 0)
        for bad in ('http://example.com', 'ftp://127.0.0.1', 'file:///c:/x'):        # only https or http on this PC
            self.assertEqual(self.run_helper('--can-send', env=dict(self.env, MA2_REPORT_RELAY=bad)).returncode, 3, bad)
        self.assertEqual(self.run_helper('--can-send', env=dict(env, RECOMP_REPORT_RELAY=self.relay.url)).returncode, 0, 'RECOMP_ alias')
        code, res = self.game_mode(env=env)
        self.assertEqual((code, res['cause'], res['saved']), (1, 'unset', '1'))
        self.assertEqual(self.relay.requests, [])

    def test_oversized_drops_the_dump(self):
        self.plant(big=True)
        code, res = self.game_mode()
        self.assertEqual(code, 0, res)
        z = self.relay.requests[-1]['zip']
        self.assertLessEqual(len(z), 20 * 1024 * 1024)
        path = self.tmp / 'big.zip'; path.write_bytes(z)
        with zipfile.ZipFile(path) as zz:
            names = zz.namelist(); system = zz.read('system.txt').decode('utf-8')
        self.assertNotIn('dumps/crash-4242.dmp', names)
        self.assertIn('left out: the report would be larger than 20 MB', system)
        self.assertIn('only its newest 6 MB', system)

    def test_faults_land_safely(self):
        self.plant()
        for step, saved, sent in (('collect', '0', 0), ('zip', '0', 0), ('send', '1', 0)):
            with self.subTest(step=step):
                n = len(self.relay.requests)
                code, res = self.game_mode(env=dict(self.env, MA2_REPORT_FAULT=step))
                self.assertEqual(res['saved'], saved); self.assertEqual(res['ok'], '0')
                self.assertEqual(code, 1 if saved == '1' else 2)
                self.assertEqual(len(self.relay.requests), n, 'nothing sent after a fault')

    def test_nothing_installed(self):
        """Works with nothing there at all (the game never started): a zip with system.txt and the notes."""
        code, res = self.game_mode('')
        self.assertEqual(code, 0, res)
        path = self.tmp / 'e.zip'; path.write_bytes(self.relay.requests[-1]['zip'])
        with zipfile.ZipFile(path) as z:
            self.assertEqual(z.namelist(), ['system.txt'])
            s = z.read('system.txt').decode('utf-8')
        self.assertIn('no game logs found', s); self.assertIn('recomp-install.json was not found', s)

    def test_binary_privacy_scan(self):
        r = subprocess.run([sys.executable, str(REPO / 'tools' / 'privacy_scan.py'), '--binaries', str(HELPER)], capture_output=True, text=True,
                           creationflags=NO_WINDOW)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


# ---------------------------------------------------------------------- the installer side
class InstallerSide(unittest.TestCase):
    """install.py: the helper is copied beside the game, the Start menu gets "Report a problem (MechAssault 2)" (no arguments)
    only with the game's Start menu entry, Uninstall removes it, and the Done screen's shortcut line is unchanged."""
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2rep-inst-', dir=str(WORK)))
        self.old = dict(os.environ)
        os.environ['MA2_SHORTCUT_DIR'] = str(self.tmp / 'Shortcuts'); os.environ['LOCALAPPDATA'] = str(self.tmp / 'Local')
        sys.path.insert(0, str(REPO / 'kit' / 'installer'))
        import install; self.install = install

    def tearDown(self):
        os.environ.clear(); os.environ.update(self.old)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def fake(self, want, helper=True):
        import types
        dest = self.tmp / 'Game'; dest.mkdir(exist_ok=True)
        for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
            (dest / f).parent.mkdir(parents=True, exist_ok=True); (dest / f).write_bytes(b'x')
        if helper: (dest / 'MA2 Report.exe').write_bytes(b'MZ')
        elif (dest / 'MA2 Report.exe').exists(): (dest / 'MA2 Report.exe').unlink()
        st = {}
        return types.SimpleNamespace(dest=dest, shortcuts=tuple(want), state=st, save=lambda: None, log=lambda s: None)

    def test_start_menu_entry_and_uninstall(self):
        inst = self.install
        for want, expect in ((('desktop', 'start'), True), (('start',), True), (('desktop',), False), ((), False)):
            with self.subTest(want=want):
                shutil.rmtree(self.tmp / 'Shortcuts', ignore_errors=True)
                me = self.fake(want)
                inst.Installer.make_shortcuts(me)
                made = [Path(p).name for p in me.state['shortcuts']]
                self.assertEqual(inst.REPORT_LNK in made, expect, made)
                if expect:
                    lnk = Path(os.environ['MA2_SHORTCUT_DIR']) / 'start' / inst.REPORT_LNK
                    self.assertTrue(lnk.is_file())
                    self.assertNotIn(b'--', lnk.read_bytes().replace(b'\0', b''), 'no arguments')
        me = self.fake(('start',), helper=False); inst.Installer.make_shortcuts(me)
        self.assertNotIn(inst.REPORT_LNK, [Path(p).name for p in me.state['shortcuts']], 'no helper, no entry')
        # Uninstall takes it with the others
        me = self.fake(('desktop', 'start')); inst.Installer.make_shortcuts(me)
        (me.dest / 'recomp-install.json').write_text(json.dumps({'shortcuts': me.state['shortcuts']}), encoding='utf-8')
        plan = inst.uninstall_plan(me.dest)
        self.assertIn(inst.REPORT_LNK, [p.name for p in plan['shortcuts']])
        self.assertEqual(len(plan['shortcuts']), 3)

    def test_done_screen_line_ignores_the_report_entry(self):
        import gui, types
        start = Path(os.environ['MA2_SHORTCUT_DIR']) / 'start'
        fake = types.SimpleNamespace(inst=types.SimpleNamespace(state={'shortcuts': [str(start / 'MechAssault 2 (Recomp).lnk'), str(start / self.install.REPORT_LNK)]}),
                                     sc={'start': True}, action=lambda: 'install')
        self.assertEqual(gui.App.shortcut_line(fake) if hasattr(gui, 'App') else None, "You'll find it in the Start menu.")


# ---------------------------------------------------------------------- driving the hidden window
ID_HEAD, ID_BODY, ID_LABEL, ID_EDIT, ID_PRIVACY, ID_NOTE, ID_LINKS, ID_STATUS, ID_PROGRESS, ID_SEND, ID_CLOSE, ID_WAIT, ID_COPY, ID_OPEN, ID_RESULT = range(100, 115)
user32 = ctypes.WinDLL('user32', use_last_error=True)
EnumProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
user32.SendMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]; user32.SendMessageW.restype = ctypes.c_ssize_t
user32.PostMessageW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
user32.GetDlgItem.argtypes = [wt.HWND, ctypes.c_int]; user32.GetDlgItem.restype = wt.HWND


class Window:
    def __init__(self, pid, timeout=20):
        end = time.time() + timeout; self.hwnd = None
        while time.time() < end and not self.hwnd:
            found = []
            def cb(h, _):
                p = wt.DWORD(); user32.GetWindowThreadProcessId(h, ctypes.byref(p))
                buf = ctypes.create_unicode_buffer(64); user32.GetClassNameW(h, buf, 64)
                if p.value == pid and buf.value == 'MA2ReportWindow': found.append(h)
                return True
            user32.EnumWindows(EnumProc(cb), 0)
            self.hwnd = found[0] if found else None
            if not self.hwnd: time.sleep(0.1)
        assert self.hwnd, 'no report window'
        assert not user32.IsWindowVisible(self.hwnd), 'the test window must stay hidden'

    def text(self, cid):
        h = user32.GetDlgItem(self.hwnd, cid)
        n = user32.SendMessageW(h, 0x000E, 0, 0)                                  # WM_GETTEXTLENGTH
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.SendMessageW(h, 0x000D, n + 1, ctypes.cast(buf, ctypes.c_void_p).value)   # WM_GETTEXT
        return buf.value

    def set_text(self, cid, s):
        buf = ctypes.create_unicode_buffer(s)
        user32.SendMessageW(user32.GetDlgItem(self.hwnd, cid), 0x000C, 0, ctypes.cast(buf, ctypes.c_void_p).value)

    def click(self, cid):
        user32.PostMessageW(self.hwnd, 0x0111, cid, user32.GetDlgItem(self.hwnd, cid))   # WM_COMMAND, BN_CLICKED

    def wait_text(self, cid, want, timeout):
        end = time.time() + timeout
        while time.time() < end:
            t = self.text(cid)
            if want in t: return t
            time.sleep(0.2)
        raise AssertionError('%r never showed %r (last %r)' % (cid, want, self.text(cid)))


if __name__ == '__main__':
    unittest.main(verbosity=2)

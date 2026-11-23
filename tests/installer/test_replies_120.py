"""1.2.0 report conversations, installer side: the reply screen shown first, the answer with and without logs, shown once.

    py -3.13 -B tests/installer/test_replies_120.py [--shots <folder>]

Runs the real installer window (hidden, off screen, tests/installer/rig.py) against the local relay harness
(relay/test/local-relay.mjs, node on PATH): a report sent with a reply key, the developer's reply through the owner API, the
window started, the answer sent. Nothing leaves this PC. With --shots, pictures of each state are saved there.
"""
import json, os, subprocess, sys, tempfile, time, unittest, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PORT = 8811
BASE = 'http://127.0.0.1:%d' % PORT
KEY = 'local-owner-key'
NO_WINDOW = 0x08000000
SHOTS = None
os.environ['MA2_REPORT_RELAY'] = BASE

import rig                                                     # noqa: E402  (sets LOCALAPPDATA to a temp folder, off-screen window)
from rig import pump, until, snap                              # noqa: E402
import report, precheck                                        # noqa: E402

gui = rig.gui


def owner(path, body=None):
    req = urllib.request.Request(BASE + path, data=None if body is None else json.dumps(body).encode(), method='GET' if body is None else 'POST',
                                 headers={'X-MA2-Owner-Key': KEY, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())


def screen_text(app):
    cv = app.cv
    out = [cv.itemcget(i, 'text') for i in cv.find_withtag('s') if cv.type(i) == 'text']
    for w in app.w.values():
        for a in ('text',):
            v = getattr(w, a, None)
            if isinstance(v, str): out.append(v)
    return ' | '.join(out)


def shot(root, name):
    if SHOTS:
        SHOTS.mkdir(parents=True, exist_ok=True)
        snap(root, SHOTS / name)


class Replies(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.relay = subprocess.Popen(['node', str(REPO / 'relay' / 'test' / 'local-relay.mjs'), str(PORT)], cwd=str(REPO / 'relay'), env=dict(os.environ, PER_IP_PER_HOUR='50'),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=NO_WINDOW)
        assert cls.relay.stdout.readline().decode().startswith('LISTENING')
        cls.tmp = Path(tempfile.mkdtemp(prefix='ma2-replies-', dir=str(REPO / 'tests' / 'tmp')))

    @classmethod
    def tearDownClass(cls):
        cls.relay.kill(); cls.relay.wait()

    def send_report(self):
        zp = self.tmp / 'r.zip'
        import zipfile
        with zipfile.ZipFile(zp, 'w') as z: z.writestr('system.txt', 'Version: 1.2.0\n')
        res = report.make_and_send(str(self.tmp / 'no-install'), str(self.tmp / 'out'), 'The build stopped at the link step.',
                                   'install-fail', 'build:link', 'installer', '1.2.0', 'test', [], 'build', 'link failed', [])
        self.assertTrue(res['sent'] and res['sent']['ok'], res['sent'])
        return res['sent']['id']

    def test_1_make_and_send_keeps_the_key(self):
        rid = self.send_report()
        entries = report.store_read()
        self.assertIn(rid, [e['id'] for e in entries])
        self.assertTrue(str(report.store_path()).startswith(os.environ['LOCALAPPDATA']))
        conv = owner('/v1/report/%s/conversation' % rid)
        self.assertTrue(conv['replies'])
        self.assertNotIn(entries[-1]['secret'], json.dumps(conv))
        head, body = report.outcome_text({'ok': True, 'id': rid, 'message': ''}, 'X')
        self.assertIn(report.REPLY_HINT, body)

    def test_2_reply_screen_first_answer_with_logs_then_shown_once(self):
        rid = self.send_report()
        owner('/v1/report/%s/reply' % rid, {'text': 'Thanks for the report. Does the build still stop if you choose\n'
                                                    '"Keep my PC responsive" in Install options?'})
        root, app = rig.make()
        try:
            self.assertTrue(until(root, lambda: app.screen == 'reply', 20), app.screen)
            pump(root, 0.5)
            t = screen_text(app)
            self.assertIn(gui.RP_TITLE, t)
            self.assertIn(gui.RP_HEAD % (rid, report.short_date(report.today())), t)
            self.assertIn(gui.RP_ATTACH, t)
            msg = [w for w in app.cv.winfo_children()]
            self.assertTrue(app.rp['attach'], 'Attach my latest logs is on by default')
            self.assertEqual(getattr(app.focus_now(), 'key', None), 'rp_text')
            shot(root, 'installer-1-reply.png')
            # shown = read: the relay no longer returns it
            self.assertTrue(until(root, lambda: owner('/v1/report/%s/conversation' % rid)['conversation']['messages'][0]['seen'], 10))
            # empty answer: a note, nothing sent
            app.do_answer(); pump(root, 0.3)
            self.assertIn(gui.RP_EMPTY, screen_text(app))
            box = app.w['rp_text']; box.insert('1.0', 'Yes, it still stops. I tried it twice.'); app.reply_text(); pump(root, 0.3)
            self.assertNotIn(gui.RP_EMPTY, screen_text(app), 'the note goes once there is text')
            self.assertEqual(app.rp['text'], 'Yes, it still stops. I tried it twice.')
            shot(root, 'installer-2-answer-typed.png')
            app.do_answer()
            self.assertTrue(until(root, lambda: app.rp['result'] is not None, 120))
            pump(root, 0.3)
            self.assertTrue(app.rp['result']['ok'], app.rp['result'])
            self.assertIn(gui.RP_SENT_LOGS, screen_text(app))
            shot(root, 'installer-3-answer-sent.png')
            conv = owner('/v1/report/%s/conversation' % rid)['conversation']
            self.assertEqual(conv['messages'][-1]['from'], 'player')
            self.assertEqual(conv['messages'][-1]['text'], 'Yes, it still stops. I tried it twice.')
            self.assertGreater(conv['messages'][-1]['logs_bytes'], 100)
            with urllib.request.urlopen(BASE + '/__mock/comments', timeout=5) as h:
                comments = [c for c in json.loads(h.read()) if rid in c['body']]
            self.assertEqual(len(comments), 1)
            self.assertIn('/v1/open/%s/logs/2' % rid, comments[0]['body'])
            app.reply_close(); pump(root, 0.3)
            self.assertEqual(app.screen, 'setup')
        finally:
            root.destroy()
        root, app = rig.make()
        try:
            pump(root, 3)
            self.assertNotEqual(app.screen, 'reply', 'each reply is shown once')
        finally:
            root.destroy()

    def test_3_failed_send_and_a_reply_that_cannot_be_answered(self):
        rid = self.send_report()
        owner('/v1/report/%s/reply' % rid, {'text': 'First question.'})
        owner('/v1/report/%s/reply' % rid, {'text': 'First question, edited.', 'edit': 1})
        root, app = rig.make()
        try:
            self.assertTrue(until(root, lambda: app.screen == 'reply', 20))
            # the developer's next message cannot come while this one is unseen; a second report's reply queues behind it
            app.w['rp_text'].insert('1.0', 'An answer.'); app.reply_text()
            os.environ['MA2_REPORT_RELAY'] = 'http://127.0.0.1:9'   # nothing answers: shown as a failure, Try again offered
            try:
                app.rp['attach'] = False
                app.do_answer()
                self.assertTrue(until(root, lambda: app.rp['result'] is not None, 30))
            finally:
                os.environ['MA2_REPORT_RELAY'] = BASE
            pump(root, 0.3)
            self.assertFalse(app.rp['result']['ok'])
            t = screen_text(app)
            self.assertIn('Could not send your answer', t)
            self.assertEqual(app.primary.text, gui.RP_TRY.upper())
            shot(root, 'installer-4-answer-failed.png')
            app.do_answer()
            self.assertTrue(until(root, lambda: app.rp['result'] is not None and not app.rp['busy'], 30))
            self.assertTrue(app.rp['result']['ok'], app.rp['result'])
            self.assertEqual(app.rp['result']['logs'], '')
            pump(root, 0.3)
            self.assertIn(gui.RP_SENT, screen_text(app))
        finally:
            root.destroy()

    def test_4_no_entries_no_request(self):
        before = owner('/v1/admin/conversations')
        p = report.store_path()
        saved = p.read_text(encoding='utf-8') if p.exists() else None
        try:
            if p.exists(): p.unlink()
            calls = []
            real = report._post
            report._post = lambda *a, **k: (calls.append(a), real(*a, **k))[1]
            try:
                self.assertEqual(report.check_replies(), [])
            finally:
                report._post = real
            self.assertEqual(calls, [], 'no entry: nothing is asked')
        finally:
            if saved is not None: p.write_text(saved, encoding='utf-8')
        self.assertEqual(before, owner('/v1/admin/conversations'))


class GameSide(unittest.TestCase):
    """The game's reply box: the helper result parser (C unit test, built when the MSVC tools are found) and the wiring."""
    def test_parser_c_unit_test(self):
        import test_update_notice
        bat = test_update_notice.vcvars()
        if not bat: self.skipTest('MSVC x64 tools not found')
        out = Path(tempfile.mkdtemp(prefix='ma2-replies-c-', dir=str(REPO / 'tests' / 'tmp')))
        src = REPO / 'tests' / 'game' / 'report_replies_test.c'
        script = out / 'build.cmd'
        script.write_text('@echo off\r\ncall "%s" >nul || exit /b 1\r\ncl /nologo /W4 /WX /D_CRT_SECURE_NO_WARNINGS "%s" /Fo"%s\\\\" /Fe"%s" >nul || exit /b 1\r\n"%s"\r\n'
                          % (bat, src, out, out / 't.exe', out / 't.exe'), encoding='ascii')
        r = subprocess.run(['cmd', '/d', '/c', str(script)], capture_output=True, text=True, creationflags=NO_WINDOW, timeout=300)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('report_replies_test: all passed', r.stdout)

    def test_wiring(self):
        cpp = (REPO / 'kit' / 'source' / 'game' / 'src' / 'pc_native_settings.cpp').read_text(encoding='utf-8')
        self.assertEqual(cpp.count('reply_check_start();'), 1)
        self.assertIn('vtable==0x481B70', cpp[cpp.index('static void reply_pulse'):])          # the main menu only
        start = cpp[cpp.index('static void reply_check_start'):cpp.index('static DWORD WINAPI reply_seen_thread')]
        self.assertIn('report-conversations.txt', start)                                         # no file = no helper run
        self.assertLess(start.index('GetFileAttributesW'), start.index('CreateThread'))
        self.assertIn('getenv("RECOMP_SAVE_DIR")', start)                                        # test runs need a test relay
        h = (REPO / 'kit' / 'source' / 'game' / 'src' / 'pc_update_check.h').read_text(encoding='utf-8')
        self.assertNotIn('replies', h.lower(), '/v1/latest stays a plain version check')


HELPER = REPO / 'report-helper' / 'MA2 Report.exe'


class HelperReplies(unittest.TestCase):
    """MA2 Report.exe: the key goes with every report and into the store; --replies / --seen / --answer (what the game runs)."""
    @classmethod
    def setUpClass(cls):
        if not HELPER.is_file():
            raise unittest.SkipTest('build the helper first: report-helper\\build_report.cmd')
        cls.relay = subprocess.Popen(['node', str(REPO / 'relay' / 'test' / 'local-relay.mjs'), str(PORT + 1)], cwd=str(REPO / 'relay'),
                                     env=dict(os.environ, PER_IP_PER_HOUR='50'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     creationflags=NO_WINDOW)
        assert cls.relay.stdout.readline().decode().startswith('LISTENING')
        cls.base = 'http://127.0.0.1:%d' % (PORT + 1)

    @classmethod
    def tearDownClass(cls):
        cls.relay.kill(); cls.relay.wait()

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2-helper-replies-', dir=str(REPO / 'tests' / 'tmp')))
        self.data = self.tmp / 'MechAssault2'; (self.data / 'Logs').mkdir(parents=True)
        (self.data / 'Logs' / '20261123-100000-1.log').write_text('[LAUNCH] build version=1.2.0\n[HEALTH] t=60 fps=60\n', encoding='utf-8')
        self.install = self.tmp / 'game'; self.install.mkdir()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('MA2_REPORT', 'RECOMP_'))}
        self.env.update(MA2_REPORT_DATA_DIR=str(self.data), MA2_REPORT_INSTALL_DIR=str(self.install), MA2_REPORT_RELAY=self.base)

    def run_helper(self, *args, env=None):
        return subprocess.run([str(HELPER)] + list(args), env=env or self.env, timeout=120, creationflags=NO_WINDOW, capture_output=True)

    def result(self, *args, env=None):
        rf = self.tmp / 'result.txt'
        if rf.exists(): rf.unlink()
        r = self.run_helper(*args, '--result', str(rf), env=env)
        lines = rf.read_text(encoding='utf-8').splitlines() if rf.is_file() else []
        return r.returncode, lines

    def owner(self, path, body=None):
        req = urllib.request.Request(self.base + path, data=None if body is None else json.dumps(body).encode(),
                                     method='GET' if body is None else 'POST', headers={'X-MA2-Owner-Key': KEY, 'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())

    def test_helper_conversation(self):
        tf = self.tmp / 'text.txt'; tf.write_text('The game froze after a jump.', encoding='utf-8')
        code, lines = self.result('--game', '--text-file', str(tf))
        res = dict(l.split('=', 1) for l in lines if '=' in l)
        self.assertEqual(res.get('ok'), '1', lines)
        rid = res['id']
        store = self.data / 'report-conversations.txt'
        entries = report.parse_store(store.read_text(encoding='utf-8'))
        self.assertEqual([e['id'] for e in entries], [rid], 'the helper keeps the key in the shared format')
        self.assertEqual(entries[0]['sent'], report.today())
        self.assertTrue(self.owner('/v1/report/%s/conversation' % rid)['replies'])
        code, lines = self.result('--replies')
        self.assertEqual((code, lines), (0, ['count=0', 'end=1']))
        self.owner('/v1/report/%s/reply' % rid, {'text': 'Thanks! Does it freeze in "mission 3" too?\nTell me \\ please. \u00e9t\u00e9'})
        code, lines = self.result('--replies')
        self.assertEqual(code, 0)
        self.assertEqual(lines[0], 'reply=%s 1 %s 1 Thanks! Does it freeze in "mission 3" too?\\nTell me \\\\ please. \u00e9t\u00e9'
                         % (rid, report.today().isoformat()), lines)
        self.assertEqual(self.run_helper('--seen', rid, '1').returncode, 0)
        self.assertEqual(self.result('--replies')[1], ['count=0', 'end=1'], 'shown once')
        # the answer, scrubbed, with a new bundle
        user = 'Jane' + 'Doe'
        af = self.tmp / 'answer.txt'; af.write_text('Yes, in mission 3 too. Files: C:\\Users\\%s\\x' % user, encoding='utf-8')
        env = dict(self.env, USERNAME=user)
        code, lines = self.result('--answer', rid, '--text-file', str(af), '--attach', env=env)
        res = dict(l.split('=', 1) for l in lines if '=' in l)
        self.assertEqual((code, res.get('ok'), res.get('logs')), (0, '1', '1'), lines)
        conv = self.owner('/v1/report/%s/conversation' % rid)['conversation']
        self.assertNotIn(user, conv['messages'][-1]['text'])
        self.assertTrue(conv['messages'][-1]['text'].startswith('Yes, in mission 3 too.'))
        self.assertGreater(conv['messages'][-1]['logs_bytes'], 50)
        code, lines = self.result('--answer', rid, '--text-file', str(af))
        self.assertIn('cause=closed', lines, 'strict alternation')
        ef = self.tmp / 'empty.txt'; ef.write_text('  \n', encoding='utf-8')
        self.assertIn('cause=empty', self.result('--answer', rid, '--text-file', str(ef))[1])
        # 61 days after the last message the entry is dropped and nothing is asked
        later = (report.today() + __import__('datetime').timedelta(days=61)).isoformat()
        code, lines = self.result('--replies', env=dict(self.env, MA2_REPORT_TODAY=later))
        self.assertEqual(code, 1)
        self.assertEqual(lines, ['count=0', 'end=1'])
        self.assertTrue(store.is_file(), 'reading never prunes')
        tf.write_text('second', encoding='utf-8')
        self.result('--game', '--text-file', str(tf), env=dict(self.env, MA2_REPORT_TODAY=later))
        self.assertNotIn(rid, store.read_text(encoding='utf-8'), 'the old entry was dropped on the next write')


if __name__ == '__main__':
    if '--shots' in sys.argv:
        i = sys.argv.index('--shots'); SHOTS = Path(sys.argv[i + 1]); del sys.argv[i:i + 2]
    (REPO / 'tests' / 'tmp').mkdir(exist_ok=True)
    unittest.main(verbosity=2)

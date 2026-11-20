"""1.1.6 installer: privacy hardening and the report bundle (R1), the Send report client (R3, against a local mock relay), the build
heartbeat (I1), pre-set /cgthreads1 for the known heavy files on PCs with less than 24 GB (I2), the power-loss marker (I3) and the
gentler default priority on small or battery PCs (I4). Every new call site is also made to fail: it must land on 1.1.5's behaviour.
Run with the kit's Python:  python -B test_v116.py          (MA2_SLOW_TESTS=1 adds the real 150 s heartbeat run)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time, unittest, zipfile
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
REPO = WORK.parent
KIT = REPO / 'kit'
sys.path.insert(0, str(KIT / 'installer')); sys.path.insert(0, str(REPO / 'tools')); sys.path.insert(0, str(Path(__file__).resolve().parent))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))      # never touch the real settings/logs
os.environ.pop('MA2_REPORT_RELAY', None)
import precheck, install, report, privacy_scan                                # noqa: E402
from mock_relay import MockRelay, parse_multipart                            # noqa: E402
import test_v115                                                             # noqa: E402  (its build fakes)

G = precheck.GB
FAKE = dict(USERNAME='Zebulon7Q', COMPUTERNAME='QUOKKA-PC9', USERDOMAIN='QUOKKA-PC9')
AT = chr(64)                                     # (built at run time: the repo's own privacy scan must not find planted values here)
MAIL1, MAIL2, MAIL3 = 'z.q+x' + AT + 'example.co.uk', 'zq' + AT + 'mail.example.org', 'zeb.q' + AT + 'example.com'
PROFILE = 'C:' + chr(92) + 'Users' + chr(92) + FAKE['USERNAME']
SHORT = 'C:' + chr(92) + 'Users' + chr(92) + 'ZEBULO~1'          # an 8.3 short profile name


def zip_of(path, members):
    with zipfile.ZipFile(path, 'w') as z:
        for n, d in members.items(): z.writestr(n, d)
    return path


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-116-', dir=str(WORK / 'tmp')))
    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def planted_env(self):
        """A fake profile (user Zebulon7Q on QUOKKA-PC9) whose LOCALAPPDATA is a real folder here."""
        prof = self.tmp / 'Users' / FAKE['USERNAME']
        local = prof / 'AppData' / 'Local'; (local / 'MechAssault2' / 'Logs').mkdir(parents=True)
        env = dict(FAKE, USERPROFILE=str(prof), LOCALAPPDATA=str(local), APPDATA=str(prof / 'AppData' / 'Roaming'))
        return env, prof, local / 'MechAssault2'


# ---------------------------------------------------------------------------- R1: redaction and the binary scrub

class Redaction(Tmp):
    def test_machine_domain_and_email_are_replaced(self):
        with mock.patch.dict(os.environ, dict(FAKE, USERPROFILE=PROFILE)):
            t = precheck.redact('user Zebulon7Q on QUOKKA-PC9 (quokka-pc9), mail ' + MAIL1 + ', ' + PROFILE + '\\x, '
                                'https://u:p' + AT + 'host.example/a?t=1 \\\\QUOKKA-PC9\\share')
        for bad in ('Zebulon7Q', 'QUOKKA', 'quokka', 'example.co.uk', 'u:p@'):
            self.assertNotIn(bad, t)
        self.assertIn('%USERNAME%', t); self.assertIn('%COMPUTERNAME%', t); self.assertIn('<email>', t); self.assertIn('%USERPROFILE%', t)

    def test_names_only_as_whole_words(self):
        with mock.patch.dict(os.environ, dict(USERNAME='user', COMPUTERNAME='PC1', USERDOMAIN='PC1')):
            t = precheck.redact('user32.dll loaded by user; PC1x stays, PC1 goes')
        self.assertIn('user32.dll', t); self.assertIn('PC1x', t); self.assertIn('by %USERNAME%;', t); self.assertIn('%COMPUTERNAME% goes', t)

    def test_scrub_keeps_length_and_offsets(self):
        with mock.patch.dict(os.environ, dict(FAKE, USERPROFILE=PROFILE, LOCALAPPDATA=PROFILE + r'\AppData\Local',
                                              APPDATA=PROFILE + r'\AppData\Roaming')):
            s = PROFILE + r'\AppData\Local\MechAssault2 zebulon7q QUOKKA-PC9 ' + SHORT + r'\Temp ' + MAIL2 + ' user32.dll'
            data = b'MDMP' + os.urandom(64) + s.encode('utf-8') + b'\0' + s.encode('utf-16-le') + b'\0\0' + s.upper().encode('utf-16-le') + b'\0\0' + os.urandom(64)
            out, n = precheck.scrub_bytes(data)
        self.assertEqual(len(out), len(data)); self.assertGreaterEqual(n, 10)
        self.assertEqual(out[:68], data[:68]); self.assertEqual(out[-64:], data[-64:])          # nothing else moves or changes
        for enc in ('latin-1', 'utf-16-le'):
            text = out.decode(enc, 'ignore') + out[1:].decode(enc, 'ignore')
            for bad in ('zebulon', 'quokka', 'zebulo~1', 'mail.example'):
                self.assertNotIn(bad, text.lower(), enc)
        self.assertIn(b'user32.dll', out); self.assertIn('USER32.DLL'.encode('utf-16-le'), out)

    def test_public_and_default_profile_folders_stay(self):
        text = chr(92).join(['C:', 'Users', 'Public', 'Documents']) + ' ' + chr(92).join(['C:', 'Users', 'Default', 'x'])
        out, n = precheck.scrub_bytes(text.encode('utf-16-le'), names=[])
        self.assertEqual(out.decode('utf-16-le'), text); self.assertEqual(n, 0)


# ---------------------------------------------------------------------------- R1: the bundle, the facts, the summary

class Bundle(Tmp):
    def plant(self):
        env, prof, local = self.planted_env()
        dest = prof / 'Games' / 'MechAssault 2'; (dest / '_work').mkdir(parents=True)
        logs = local / 'Logs'
        leak = 'user Zebulon7Q on QUOKKA-PC9 mail ' + MAIL3 + ' path %s\\Games \\\\QUOKKA-PC9\\share C:\\Users\\ZEBULO~1\\Temp' % prof
        (logs / '20261120-120000-1.log').write_text('[LAUNCH] build version=1.1.6 %s\n[HEALTH] t=60 fps=60 p99=17 underruns=0\n'
                                                    '[CRASH] 0xC0000005 at 0x0011A2B3 %s\n' % (leak, leak), encoding='utf-8')
        (logs / 'install-20261120-110000.log').write_text('install %s\n' % leak, encoding='utf-8')
        (logs / 'stub-20261120-110000.log').write_text('stub %s\n' % leak, encoding='utf-8')
        (local / 'pc_settings.ini').write_text('; %s\n[Display]\nFrameLimit=144\nVSync=0\n[Audio]\nThreads=low\n' % leak, encoding='utf-8')
        (dest / 'recomp-install.json').write_text(json.dumps({'public_version': '1.1.6', 'kit_id': 'abc', 'shortcuts': [str(prof / 'Desktop' / 'x.lnk')],
                                                              'low_memory_build': {'one_codegen_thread': 172, 'lighter_optimisation': {'src/recomp/gen/recomp_0076.c': '/O1 /Ob0'}}}), encoding='utf-8')
        s = leak + ' ' + str(local)
        (logs / 'crash-20261120-120001.dmp').write_bytes(b'MDMP' + os.urandom(4000) + s.encode('utf-16-le') + os.urandom(100) + s.encode('mbcs') + os.urandom(100))
        (logs / '20261120-120000-1.botrec').write_bytes(b'MA2BREC1' + s.encode('utf-8'))
        return env, dest, local

    def scan_zip(self, path, env, extra_texts=()):
        out = self.tmp / ('unzipped-' + Path(path).stem); out.mkdir()
        with zipfile.ZipFile(path) as z: z.extractall(out)
        files = [p for p in out.rglob('*') if p.is_file()]
        for i, t in enumerate(extra_texts):
            f = out / ('field%d.txt' % i); f.write_text(t, encoding='utf-8'); files.append(f)
        with mock.patch.dict(os.environ, env):
            return privacy_scan.scan(files, rel_to=out), sorted(p.relative_to(out).as_posix() for p in files)

    def test_planted_identity_report_bundle_scans_clean(self):
        env, dest, local = self.plant()
        with mock.patch.dict(os.environ, env):
            res = report.make_and_send(dest, local / 'Diagnostics', text='It crashed. I am Zebulon7Q, write to ' + MAIL3,
                                       kind='crash', version='1.1.6', build='abc', gpus=[], send_it=False)
            sent_text = precheck.redact('It crashed. I am Zebulon7Q, write to ' + MAIL3)      # what send() posts as `text`
        self.assertIsNone(res['sent'])
        hits, names = self.scan_zip(res['path'], env, ['\n'.join(res['summary']), sent_text])
        self.assertEqual(hits, [], hits)
        self.assertIn('dumps/crash-20261120-120001.dmp', names)
        self.assertFalse(any(n.startswith('botrec/') for n in names), names)            # a recording is compressed: never in a report
        self.assertIn('what-happened.txt', names)
        with zipfile.ZipFile(res['path']) as z:
            dump = z.read('dumps/crash-20261120-120001.dmp'); system = z.read('system.txt').decode('utf-8')
        self.assertEqual(len(dump), (Path(env['LOCALAPPDATA']) / 'MechAssault2' / 'Logs' / 'crash-20261120-120001.dmp').stat().st_size)
        self.assertIn('private name(s) or folder(s) overwritten', system)
        self.assertIn('Low-memory build: 172 file(s)', system)                              # from recomp-install.json (state.json is gone)
        for want in ('CPU: ', 'Memory: ', 'commit limit', 'Power: '): self.assertIn(want, system)

    def test_planted_identity_collect_diagnostics_scans_clean_without_the_recording(self):
        """Collect diagnostics (saved on this PC) with the dump: also clean. The bot recording, asked for, is the only exception: it is
        the raw file, as in 1.1.5, and stays on this PC."""
        env, dest, local = self.plant()
        with mock.patch.dict(os.environ, env):
            res = precheck.collect_diagnostics(dest, local / 'Diagnostics', '1.1.6', 'abc', [], include_dump=True, include_botrec=True)
        hits, names = self.scan_zip(res['path'], env)
        self.assertEqual([h for h in hits if not h[0].startswith('botrec/')], [], hits)
        self.assertIn('botrec/20261120-120000-1.botrec', names)

    def test_members_are_1_1_5_plus_system_lines(self):
        env, dest, local = self.plant()
        with mock.patch.dict(os.environ, env):
            res = precheck.collect_diagnostics(dest, local / 'Diagnostics', '1.1.6', 'abc', [])
        self.assertEqual(res['members'], ['logs/20261120-120000-1.log', 'logs/install-20261120-110000.log', 'logs/stub-20261120-110000.log',
                                          'pc_settings.ini', 'recomp-install.json', 'system.txt'])
        self.assertEqual(set(res['sizes']), set(res['members']))

    def test_summary_lines(self):
        env, dest, local = self.plant()
        st = {'done': ['check', 'disc', 'extract'], 'last_step': 'build', 'in_progress': 'build'}
        with mock.patch.dict(os.environ, env):
            lines = precheck.report_summary(dest, st, '1.1.6', 'abc', [{'name': 'GPU X', 'vram': 8 * G}], 'build', 'out_of_memory: ran out', 'install-fail')
        text = '\n'.join(lines)
        self.assertLessEqual(len(lines), 20); self.assertGreaterEqual(len(lines), 10)
        for want in ('MechAssault 2 Recomp 1.1.6 (build abc) | report: install-fail', 'Installed: 1.1.6', 'CPU: ', 'Memory: ', 'GPU: GPU X 8 GB',
                     'Low-memory build: 172', '[Display] VSync=0 FrameLimit=144', '[Audio] Threads=low', 'Failure: build: out_of_memory',
                     'Last run: ended without a stop during build', '[HEALTH] t=60', '[CRASH] 0xC0000005'):
            self.assertIn(want, text)
        self.assertNotIn('Zebulon7Q', text); self.assertNotIn('QUOKKA', text); self.assertNotIn('example.com', text)

    def test_size_cap_drops_the_recording_then_the_dump(self):
        env, dest, local = self.plant()
        logs = local / 'Logs'
        (logs / 'crash-20261120-120001.dmp').write_bytes(b'MDMP' + os.urandom(3 << 20))
        (logs / '20261120-120000-1.botrec').write_bytes(os.urandom(2 << 20))
        with mock.patch.dict(os.environ, env):
            res = precheck.collect_diagnostics(dest, local / 'Diagnostics', '1.1.6', 'abc', [], include_dump=True, include_botrec=True, max_bytes=4 << 20)
            self.assertEqual(res['dropped'], ['botrec/20261120-120000-1.botrec'])
            res2 = precheck.collect_diagnostics(dest, local / 'Diagnostics', '1.1.6', 'abc', [], include_dump=True, include_botrec=True, max_bytes=1 << 20)
        self.assertEqual(sorted(res2['dropped']), ['botrec/20261120-120000-1.botrec', 'dumps/crash-20261120-120001.dmp'])
        self.assertLessEqual(res2['path'].stat().st_size, 1 << 20)
        with zipfile.ZipFile(res2['path']) as z: system = z.read('system.txt').decode('utf-8')
        self.assertIn('left out: the report would be larger than 1 MB', system)
        self.assertNotIn('dumps/', ' '.join(res2['members']))

    def test_report_dump_limit_is_15_mb(self):
        env, dest, local = self.plant()
        (local / 'Logs' / 'crash-20261120-120001.dmp').write_bytes(b'MDMP' + b'\0' * (16 << 20))
        with mock.patch.dict(os.environ, env):
            res = report.make_and_send(dest, local / 'Diagnostics', send_it=False, gpus=[])
        self.assertNotIn('dumps/crash-20261120-120001.dmp', res['members'])
        self.assertTrue(any('larger than 15 MB' in n for n in res['notes']), res['notes'])

    def test_fault_the_scrub_leaves_the_dump_out(self):
        env, dest, local = self.plant()
        with mock.patch.dict(os.environ, env), mock.patch.object(precheck, 'scrub_bytes', side_effect=RuntimeError('boom')):
            res = report.make_and_send(dest, local / 'Diagnostics', send_it=False, gpus=[])
        self.assertFalse(any(m.startswith('dumps/') for m in res['members']))
        self.assertTrue(any('could not be checked for private data' in n for n in res['notes']))

    def test_fault_each_new_fact_and_summary_part(self):
        env, dest, local = self.plant()
        with mock.patch.dict(os.environ, env):
            for name in ('cpu_model', 'memory_status', 'power_status', 'installed_memory', 'read_stamp'):
                with mock.patch.object(precheck, name, side_effect=RuntimeError(name)):
                    facts = precheck.diagnostics_facts(dest, {'in_progress': 'build'}, [])
                    lines = precheck.report_summary(dest, None, '1.1.6', 'abc', [])
                self.assertTrue(any(l.startswith('Session:') for l in facts), name)
                self.assertTrue(any('1.1.6' in l for l in lines), name)
                self.assertTrue(any('Last run: ended without a stop' in l for l in facts), name)

    def test_unclean_end_line_in_system_facts(self):
        facts = precheck.diagnostics_facts(self.tmp, {'last_unclean': {'step': 'build', 'time': '2026-11-20T10:00:00'}}, [])
        self.assertIn('Last run: ended without a stop (power loss, shutdown or a closed window) during build (started 2026-11-20T10:00:00)', facts)


# ---------------------------------------------------------------------------- R3: the report client against a mock relay

class Client(Tmp):
    def setUp(self):
        super().setUp()
        self.relay = MockRelay().start()
        self.zip = zip_of(self.tmp / 'r.zip', {'system.txt': 'x' * 100})

    def tearDown(self):
        self.relay.stop(); super().tearDown()

    def send(self, **kw):
        a = dict(text='It froze.', summary='line 1\nline 2', kind='install-fail', sig='build:out_of_memory', version='1.1.6', build='k1', url=self.relay.url)
        a.update(kw)
        return report.send(self.zip, **a)

    def test_sent_and_the_wire_format(self):
        r = self.send()
        self.assertTrue(r['ok']); self.assertEqual(r['cause'], 'sent'); self.assertRegex(r['id'], r'^MA2-[A-Z2-7]{6}$'); self.assertEqual(r['http'], 200)
        q = self.relay.requests[-1]
        self.assertEqual(q['path'], '/v1/report')
        h = q['headers']
        self.assertEqual((h['x-ma2-client'], h['x-ma2-version'], h['x-ma2-build'], h['x-ma2-kind'], h['x-ma2-signature']),
                         ('installer', '1.1.6', 'k1', 'install-fail', 'build:out_of_memory'))
        self.assertTrue(h['content-type'].startswith('multipart/form-data; boundary='))
        f = q['fields']
        self.assertEqual((f['client'], f['version'], f['build'], f['kind'], f['signature'], f['text'], f['summary']),
                         ('installer', '1.1.6', 'k1', 'install-fail', 'build:out_of_memory', 'It froze.', 'line 1\nline 2'))
        self.assertEqual(f['zip'], self.zip.read_bytes())

    def test_relay_busy_429(self):
        self.relay.mode = '429'
        r = self.send()
        self.assertFalse(r['ok']); self.assertEqual((r['cause'], r['http']), ('rate_limited', 429)); self.assertIn('Too many reports', r['message'])

    def test_server_error_500(self):
        self.relay.mode = '500'
        r = self.send(); self.assertEqual((r['ok'], r['cause'], r['http']), (False, 'server', 500))

    def test_timeout(self):
        self.relay.mode = 'slow'; self.relay.slow_seconds = 3
        r = self.send(timeout=0.5); self.assertEqual((r['ok'], r['cause']), (False, 'timeout'))

    def test_offline_no_dns(self):
        with mock.patch.object(socket, 'getaddrinfo', side_effect=socket.gaierror(11001, 'getaddrinfo failed')):
            r = report.send(self.zip, url='https://relay.example.invalid')
        self.assertEqual((r['ok'], r['cause']), (False, 'offline')); self.assertEqual(len(self.relay.requests), 0)

    def test_nothing_listening(self):
        s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
        r = report.send(self.zip, url='http://127.0.0.1:%d' % port)
        self.assertFalse(r['ok']); self.assertIn(r['cause'], ('blocked', 'server', 'offline'))

    def test_connection_dropped(self):
        self.relay.mode = 'close'
        r = self.send(); self.assertFalse(r['ok'])

    def test_relay_says_no_with_a_message(self):
        self.relay.mode = 'okfalse'; self.relay.message = 'Please update to 1.1.7.\x07'
        r = self.send(); self.assertEqual((r['ok'], r['cause'], r['message']), (False, 'refused', 'Please update to 1.1.7.'))

    def test_not_json_and_redirect_are_not_sent(self):
        self.relay.mode = 'badjson'; self.assertEqual(self.send()['cause'], 'server')
        self.relay.mode = 'redirect'; r = self.send(); self.assertFalse(r['ok']); self.assertEqual(len(self.relay.requests), 2)

    def test_unset_relay_sends_nothing(self):
        with mock.patch.object(report, 'CONFIG', self.tmp / 'none.json'):
            self.assertEqual(report.relay_url(), ''); self.assertFalse(report.available())
            r = report.send(self.zip)
        self.assertEqual((r['ok'], r['cause']), (False, 'unset')); self.assertEqual(self.relay.requests, [])

    def test_oversized_zip_is_not_sent(self):
        big = self.tmp / 'big.zip'
        with zipfile.ZipFile(big, 'w', zipfile.ZIP_STORED) as z: z.writestr('x', os.urandom(report.MAX_ZIP))
        r = report.send(big, url=self.relay.url)
        self.assertEqual((r['ok'], r['cause']), (False, 'too_large')); self.assertEqual(self.relay.requests, [])

    def test_text_and_signature_are_limited(self):
        self.send(text='a' * 5000, sig='x' * 300 + '<script>\n@all')
        f = self.relay.requests[-1]['fields']
        self.assertEqual(len(f['text']), report.MAX_TEXT); self.assertTrue(len(f['signature']) <= 120 and '<' not in f['signature'] and '\n' not in f['signature'])

    def test_relay_address_rules(self):
        cfg = self.tmp / 'report.json'
        with mock.patch.object(report, 'CONFIG', cfg):
            cfg.write_text('{"relay_url": "https://relay.example.org/"}'); self.assertEqual(report.relay_url(), 'https://relay.example.org')
            cfg.write_text('{"relay_url": "http://relay.example.org"}'); self.assertEqual(report.relay_url(), '')          # https only
            cfg.write_text('not json'); self.assertEqual(report.relay_url(), '')
            with mock.patch.dict(os.environ, {'MA2_REPORT_RELAY': 'http://evil.example.org'}):
                self.assertEqual(report.relay_url(), '')                                                                   # http only on this PC
            with mock.patch.dict(os.environ, {'MA2_REPORT_RELAY': self.relay.url}):
                self.assertEqual(report.relay_url(), self.relay.url)
        shipped = json.loads((KIT / 'installer' / 'data' / 'report.json').read_text(encoding='utf-8'))
        self.assertIn('relay_url', shipped)

    def test_make_and_send_saves_first_then_sends(self):
        out = self.tmp / 'Diagnostics'
        res = report.make_and_send(self.tmp / 'nothing', out, text='hello', kind='manual', version='1.1.6', build='k1', gpus=[], url=self.relay.url)
        self.assertTrue(res['sent']['ok']); self.assertTrue(Path(res['path']).is_file()); self.assertEqual(Path(res['path']).parent, out)
        self.assertTrue(Path(res['path']).name.startswith('MA2-report-'))
        q = self.relay.requests[-1]
        self.assertEqual(q['zip'], Path(res['path']).read_bytes()); self.assertIn('MechAssault 2 Recomp 1.1.6', q['fields']['summary'])
        self.relay.mode = '500'
        res = report.make_and_send(self.tmp / 'nothing', out, version='1.1.6', gpus=[], url=self.relay.url)
        self.assertFalse(res['sent']['ok']); self.assertTrue(Path(res['path']).is_file())               # not sent: still saved
        head, body = report.outcome_text(res['sent'], 'X')
        self.assertTrue(head.startswith('Could not send') and 'saved to X' in body)

    def test_fault_the_sender_itself_never_raises(self):
        with mock.patch.object(report, 'multipart', side_effect=RuntimeError('boom')):
            r = self.send()
        self.assertEqual((r['ok'], r['cause']), (False, 'error'))

    def test_no_request_without_send(self):
        report.make_and_send(self.tmp / 'nothing', self.tmp / 'D', gpus=[], url=self.relay.url, send_it=False)
        self.assertEqual(self.relay.requests, [])


# ---------------------------------------------------------------------------- I1: heartbeat

class Heartbeats(Tmp):
    def inst(self):
        i = install.Installer(None, self.tmp / 'dest', log=self.lines.append)
        return i

    def setUp(self):
        super().setUp(); self.lines = []

    def run_tool(self, inst, code):
        inst.run_cmd([sys.executable, '-c', code], title='Fake tool')
        return [l for l in self.lines if 'still working' in l]

    def test_heartbeats_while_the_tool_is_silent(self):
        i = self.inst(); i.HEARTBEAT = 0.5
        beats = self.run_tool(i, 'import time; print("a", flush=True); time.sleep(1.8); print("b", flush=True)')
        self.assertGreaterEqual(len(beats), 2, self.lines); self.assertLessEqual(len(beats), 4, self.lines)
        self.assertIn('Fake tool', beats[0]); self.assertIn('GB of memory free', beats[0])
        self.assertFalse(re.search(r'\d+\s*/\s*\d+', ' '.join(beats)))      # never looks like a progress count to the window
        self.assertIn('  a', self.lines); self.assertIn('  b', self.lines)

    def test_no_heartbeat_while_output_flows(self):
        i = self.inst(); i.HEARTBEAT = 0.5
        beats = self.run_tool(i, 'import time\nfor n in range(12): print(n, flush=True); time.sleep(0.15)')
        self.assertEqual(beats, [])

    def test_fault_heartbeat_cannot_start(self):
        i = self.inst(); i.HEARTBEAT = 0.2
        real = install.Heartbeat
        class Broken(real):
            def __init__(self, *a, **k): raise RuntimeError('no threads')
        with mock.patch.object(install, 'Heartbeat', Broken):
            self.run_tool(i, 'print("x")')
        self.assertTrue(any('no progress heartbeat' in l for l in self.lines)); self.assertIn('  x', self.lines)

    def test_fault_heartbeat_thread_throws(self):
        def log(s):
            if 'still working' in s: raise RuntimeError('log broke')
            self.lines.append(s)
        i = install.Installer(None, self.tmp / 'dest', log=log); i.HEARTBEAT = 0.2
        with mock.patch.object(install.precheck, 'memory_info', side_effect=RuntimeError('x')):
            i.run_cmd([sys.executable, '-c', 'import time; time.sleep(0.8); print("done")'], title='Fake tool')
        self.assertIn('  done', self.lines)

    def test_a_failing_tool_still_stops_as_before(self):
        i = self.inst(); i.HEARTBEAT = 0.3
        with self.assertRaises(install.Stop):
            self.run_tool(i, 'import sys, time; time.sleep(0.7); sys.exit(3)')

    @unittest.skipUnless(os.environ.get('MA2_SLOW_TESTS'), 'MA2_SLOW_TESTS=1: the real 150 s run')
    def test_real_150_seconds_gives_two_heartbeats(self):
        beats = self.run_tool(self.inst(), 'import time; time.sleep(150)')
        self.assertEqual(len(beats), 2, beats)


# ---------------------------------------------------------------------------- I2: pre-set /cgthreads1

class Preseed(test_v115.Base):
    HEAVY = ['src/recomp/gen/recomp_0101.c']             # (of the six, test_v115's fake source tree has recomp_0101.c)

    def build_on(self, installed, total, outcomes=('ok',), jobs=8, free=None):
        inst = self.installer()
        with mock.patch.object(install.precheck, 'installed_memory', lambda: installed):
            self.fake_build(inst, list(outcomes), jobs=jobs, mem=lambda: (total, free or total / 2))
        return inst

    def test_24_gb_and_more_is_exactly_1_1_5(self):
        for installed, total in ((24 * G, 23.8 * G), (32 * G, 31.8 * G), (None, 23.0 * G)):
            self.calls = []
            inst = self.build_on(installed, total)
            c = self.calls[0]
            self.assertIsNone(c['include'], installed); self.assertNotIn('CMAKE_PROJECT', c['script'])
            self.assertFalse((c['root'] / install.LOWMEM_FILE).exists()); self.assertIsNone(inst.lowmem_record())

    def test_16_gb_presets_the_heavy_files_only(self):
        inst = self.build_on(16 * G, 15.8 * G)
        c = self.calls[0]
        self.assertIn('-DCMAKE_PROJECT_mechassault2_recomp_INCLUDE=', c['script'])
        self.assertIn('set_property(SOURCE "src/recomp/gen/recomp_0101.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o})', c['include'])
        self.assertNotIn('recomp_0000.c', c['include'])
        self.assertIsNone(inst.lowmem_record()); self.assertNotIn('lowmem', inst.state)            # not a "low-memory build"
        self.assertTrue(any('start with one code-generation thread' in l for l in self.lines))

    def test_8_gb_presets_every_game_file(self):
        inst = self.build_on(8 * G, 7.8 * G, jobs=2)
        inc = self.calls[0]['include']
        for n in test_v115.GEN: self.assertIn('src/recomp/gen/%s" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o}' % n, inc)
        self.assertIsNone(inst.lowmem_record())

    def test_the_ladder_still_works_on_top(self):
        inst = self.build_on(8 * G, 7.8 * G, jobs=1, free=3 * G, outcomes=[
            ('out_of_memory', test_v115.oom_tail('recomp_0101.c'), ['recomp_0000.c']), ('out_of_memory', test_v115.oom_tail('recomp_0101.c'), []), 'ok'])
        self.assertEqual(len(self.calls), 3)
        # every file already has the first rung (/cgthreads1): the first out-of-memory stop goes straight to /O1, never the same settings again
        self.assertIn('"src/recomp/gen/recomp_0101.c" PROPERTY COMPILE_OPTIONS /cgthreads1 /O1 ${_o}', self.calls[1]['include'])
        self.assertIn('"src/recomp/gen/recomp_0101.c" PROPERTY COMPILE_OPTIONS /cgthreads1 /O1 /Ob0 ${_o}', self.calls[2]['include'])
        self.assertIn('"src/recomp/gen/recomp_0002.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o}', self.calls[2]['include'])
        self.assertEqual(inst.lowmem_record()['lighter_optimisation'], {'src/recomp/gen/recomp_0101.c': '/O1 /Ob0'})

    def test_16_gb_ladder_first_rung_still_covers_the_other_files(self):
        self.build_on(16 * G, 15.8 * G, jobs=1, free=3 * G, outcomes=[('out_of_memory', test_v115.oom_tail('recomp_0002.c'), ['recomp_0000.c']), 'ok'])
        inc = self.calls[1]['include']
        self.assertIn('"src/recomp/gen/recomp_0002.c" PROPERTY COMPILE_OPTIONS /cgthreads1 ${_o}', inc)      # the normal first rung
        self.assertNotIn('/O1', inc)

    def test_fault_preseed_decision(self):
        with mock.patch.object(install.Installer, 'preseed_units', side_effect=RuntimeError('boom')):
            self.build_on(16 * G, 15.8 * G)
        self.assertIsNone(self.calls[0]['include']); self.assertTrue(any('pre-set low-memory settings are not used' in l for l in self.lines))

    def test_fault_writing_the_include(self):
        with mock.patch.object(install, 'write_lowmem_cmake', side_effect=OSError('disk')):
            self.build_on(16 * G, 15.8 * G)
        self.assertIsNone(self.calls[0]['include']); self.assertNotIn('CMAKE_PROJECT', self.calls[0]['script'])

    def test_installed_memory_reads_this_pc(self):
        v = precheck.installed_memory()
        self.assertTrue(v is None or v >= 1 * G, v)


# ---------------------------------------------------------------------------- I3: power-loss marker

CHILD = r'''
import sys, time, json
from pathlib import Path
sys.path.insert(0, sys.argv[1])
import install
dest, flag, mode = Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
install.clean_stale_build_folders = lambda bases: []
inst = install.Installer(None, dest, log=lambda s: None, keep_awake=False)
def build():
    flag.write_text('building')
    if mode == 'hang': time.sleep(120)
    if mode == 'fail': raise install.Stop('simulated', 'build_failed')
inst.steps = lambda: [('check', 'Checking requirements', lambda: None), ('extract', 'Extracting game files', lambda: None),
                      ('build', 'Compiling the game', build), ('finish', 'Finishing', lambda: None)]
inst.stale_done = lambda: None
try:
    inst.run()
    print('FINISHED', flush=True)
except install.Stop as e:
    print('STOPPED', e.kind, flush=True)
'''


class PowerLoss(Tmp):
    def child(self, dest, flag, mode, wait=True):
        script = self.tmp / 'child.py'; script.write_text(CHILD, encoding='utf-8')
        env = dict(os.environ, LOCALAPPDATA=str(self.tmp / 'local'))
        args = [sys.executable, '-B', str(script), str(KIT / 'installer'), str(dest), str(flag), mode]
        flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
        if wait: return subprocess.run(args, env=env, capture_output=True, text=True, timeout=120, creationflags=flags)
        return subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=flags)

    def test_marker_is_saved_during_a_step_and_cleared_by_success_and_by_a_stop(self):
        inst = install.Installer(None, self.tmp / 'd', log=lambda s: None, keep_awake=False)
        inst.state = {'done': []}; inst.work.mkdir(parents=True)
        seen = []
        inst.steps = lambda: [('check', 'c', lambda: None), ('build', 'Compiling the game',
                                                            lambda: seen.append(json.loads((inst.work / 'state.json').read_text())['in_progress']))]
        inst.stale_done = lambda: None
        inst.run_steps()
        self.assertEqual(seen, ['build']); self.assertNotIn('in_progress', json.loads((inst.work / 'state.json').read_text()))
        dest = self.tmp / 'G' / 'MA2'
        r = self.child(dest, self.tmp / 'f', 'fail')
        self.assertIn('STOPPED build_failed', r.stdout, r.stdout + r.stderr)
        st = json.loads((dest / '_work' / 'state.json').read_text())
        self.assertNotIn('in_progress', st); self.assertEqual(st['last_step'], 'build')          # a stop is not a power loss

    def test_killed_during_the_build_then_started_again(self):
        """A real process is killed while 'building'; the next run logs the unclean end once, Resume carries on, the record is in the
        diagnostics facts, and the finished install has no marker left."""
        dest = self.tmp / 'Games' / 'MA2'; flag = self.tmp / 'flag.txt'
        p = self.child(dest, flag, 'hang', wait=False)
        t0 = time.time()
        while not flag.is_file() and time.time() - t0 < 60: time.sleep(0.1)
        self.assertTrue(flag.is_file(), p.stdout.read() if p.poll() is not None else 'still running')
        p.kill(); p.wait(30); p.stdout.close()                 # (this test's own child only)
        st = json.loads((dest / '_work' / 'state.json').read_text())
        self.assertEqual(st['in_progress'], 'build'); self.assertEqual(st['last_step'], 'build')
        with mock.patch.dict(os.environ, {'LOCALAPPDATA': str(self.tmp / 'local')}):
            facts = precheck.diagnostics_facts(dest, precheck.read_state(dest), [])
        self.assertTrue(any(l.startswith('Last run: ended without a stop') and 'during build' in l for l in facts), facts)
        flag.unlink()
        r = self.child(dest, flag, 'quick')
        self.assertIn('FINISHED', r.stdout, r.stdout + r.stderr)
        logs = sorted((self.tmp / 'local' / 'MechAssault2' / 'Logs').glob('install-*.log'))
        text = logs[-1].read_text(encoding='utf-8')
        self.assertIn('The last run ended without a stop during "build"', text)
        self.assertIn('[2/4] Extracting game files: already done', text)                # Resume as in 1.1.5
        self.assertEqual(text.count('ended without a stop'), 1)
        st = json.loads((dest / '_work' / 'state.json').read_text())
        self.assertNotIn('in_progress', st); self.assertIn('build', st['done'])
        self.assertEqual(st['last_unclean']['step'], 'build'); self.assertTrue(any('unclean' in e for e in st['events']))

    def test_fault_marker_save_fails(self):
        inst = install.Installer(None, self.tmp / 'd', log=lambda s: None, keep_awake=False)
        inst.state = {'done': []}; inst.work.mkdir(parents=True)
        ran = []
        inst.steps = lambda: [('build', 'Compiling the game', lambda: ran.append(1))]; inst.stale_done = lambda: None
        real = inst.save
        calls = {'n': 0}
        def save():
            calls['n'] += 1
            if calls['n'] == 1: raise OSError('locked')
            real()
        inst.save = save
        inst.run_steps()
        self.assertEqual(ran, [1])

    def test_fault_marker_read_fails(self):
        inst = install.Installer(None, self.tmp / 'd', log=lambda s: None)
        inst.state = mock.MagicMock(); inst.state.pop.side_effect = RuntimeError('x')
        inst.note_unclean_end()                                   # never raises


# ---------------------------------------------------------------------------- I4: gentler default

class Gentle(unittest.TestCase):
    def setUp(self):
        import gui; self.gui = gui

    def test_battery_or_small_cpu_preselects_keep_responsive(self):
        g = self.gui
        with mock.patch.object(g.precheck, 'on_battery', lambda: True), mock.patch.object(g.os, 'cpu_count', lambda: 24):
            self.assertTrue(g.gentle_default())
        with mock.patch.object(g.precheck, 'on_battery', lambda: False), mock.patch.object(g.os, 'cpu_count', lambda: 4):
            self.assertTrue(g.gentle_default())
        with mock.patch.object(g.precheck, 'on_battery', lambda: False), mock.patch.object(g.os, 'cpu_count', lambda: 6):
            self.assertFalse(g.gentle_default())

    def test_fault_falls_back_to_normal(self):
        g = self.gui
        with mock.patch.object(g.precheck, 'on_battery', side_effect=OSError('x')):
            self.assertFalse(g.gentle_default())

    def test_this_test_pc_keeps_normal(self):
        self.assertEqual(self.gui.gentle_default(), bool(precheck.on_battery() or (os.cpu_count() or 0) <= 4))


if __name__ == '__main__':
    unittest.main(verbosity=2)

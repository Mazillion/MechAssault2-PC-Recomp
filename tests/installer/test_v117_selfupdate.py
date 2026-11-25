"""1.1.7 self-updating installer (selfupdate.py, install.py --self-update / --no-self-update, the installer copy and the Update
MechAssault 2 entry): every failure case (offline, relay down, garbage, hash mismatch, GitHub down, a redirect outside GitHub, size
caps, antivirus quarantine, cancel, start failure), the happy path against the local mocks (tests/installer/mock_update.py), what is
sent (the version only), the off switches, and each new method failing safe. No real relay and no real GitHub are ever contacted.
Run with the kit's Python:  python -B test_v117_selfupdate.py
"""
import io, json, os, re, shutil, subprocess, sys, tempfile, unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

WORK = Path(__file__).resolve().parents[1]
REPO = WORK.parent
sys.path.insert(0, str(REPO / 'kit' / 'installer'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
(WORK / 'tmp').mkdir(exist_ok=True)
os.environ['LOCALAPPDATA'] = tempfile.mkdtemp(prefix='ma2ux-local-', dir=str(WORK / 'tmp'))
for v in ('MA2_SELF_UPDATE', 'MA2_SELF_UPDATE_TEST', 'MA2_UPDATE_RELAY', 'MA2_SELF_UPDATE_GITHUB',
          'MA2_INSTALLER_EXE', 'MA2_SELF_UPDATE_DEST', 'MA2_HANDOFF_WAIT'):
    os.environ.pop(v, None)
# 11-25 (REPORTS-1124 G2): MA2_REPORT_RELAY is never removed at import (that sent other suites' test reports in the same
# process to the real relay); a closed loopback port instead. Tests that need it unset remove it inside os.environ's patch.
os.environ['MA2_REPORT_RELAY'] = 'http://127.0.0.1:9'
import selfupdate, install, precheck                         # noqa: E402
from mock_update import MockUpdate, ASSET, OLD_ASSET, RELAY_LABEL   # noqa: E402

CLOSED = 'http://127.0.0.1:9'           # nothing listens there (connection refused)
boom = lambda *a, **k: (_ for _ in ()).throw(RuntimeError('injected fault'))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='ma2ux-su-', dir=str(WORK / 'tmp')))
        self.env = mock.patch.dict(os.environ, {'MA2_SELF_UPDATE_TEST': '1', 'MA2_SHORTCUT_DIR': str(self.tmp / 'lnk')})
        self.env.start()
        self.srv = MockUpdate(b'MZ' + os.urandom(2 * 1024 * 1024 - 2), latest='1.1.8').start()
        os.environ['MA2_UPDATE_RELAY'] = self.srv.base
        os.environ['MA2_SELF_UPDATE_GITHUB'] = self.srv.base
        self.tempdir = self.tmp / 'temp'; self.tempdir.mkdir()
        self.tp = mock.patch.object(tempfile, 'tempdir', str(self.tempdir)); self.tp.start()
        selfupdate._switched_off = False
        self.lines = []

    def tearDown(self):
        mock.patch.stopall()
        self.srv.stop()
        selfupdate._switched_off = False
        shutil.rmtree(self.tmp, ignore_errors=True)

    def info(self):
        r = selfupdate.check('1.1.7', log=self.lines.append)
        self.assertTrue(r['ok'], r)
        return r

    def leftovers(self):
        return [p for p in self.tempdir.glob(selfupdate.TEMP_PREFIX + '*')]

    def fail_download(self, cause, **kw):
        info = self.info()
        n = len(self.srv.requests)
        with self.assertRaises(selfupdate.UpdateError) as c:
            selfupdate.download(info, '1.1.7', log=self.lines.append, **kw)
        self.assertEqual(c.exception.cause, cause, c.exception.detail)
        self.assertEqual(self.leftovers(), [], 'the download folder is deleted after a failure')
        self.assertTrue(selfupdate.text(cause).endswith('nothing was changed.'))
        return self.srv.requests[n:]


# ---------------------------------------------------------------------------- the relay check

class Check(Base):
    def test_newer_with_hash_offers(self):
        r = self.info()
        self.assertEqual((r['latest'], r['sha256'], r['size'], r['asset']), ('1.1.8', self.srv.sha, len(self.srv.payload), ASSET))

    def test_only_the_version_is_sent(self):
        self.info()
        req = self.srv.requests[-1]
        self.assertEqual(req['path'], '/v1/latest')
        names = {k.lower() for k in req['headers']}
        self.assertLessEqual(names, {'host', 'user-agent', 'accept', 'accept-encoding', 'connection'})
        self.assertEqual(req['headers']['User-Agent'], 'MechAssault2-Recomp-Installer/1.1.7')

    def test_same_or_older_is_current(self):
        self.assertEqual(selfupdate.check('1.1.8')['cause'], 'current')
        self.assertEqual(selfupdate.check('1.2.0')['cause'], 'current')
        self.srv.relay = 'older'
        self.assertEqual(selfupdate.check('1.1.7')['cause'], 'current')

    def test_preview_order(self):
        self.assertTrue(selfupdate.is_newer('1.1.7-preview.2', '1.1.7-preview.1'))
        self.assertTrue(selfupdate.is_newer('1.1.7', '1.1.7-preview.9'))
        self.assertFalse(selfupdate.is_newer('1.1.7-preview.9', '1.1.7'))
        self.assertTrue(selfupdate.is_newer('1.1.10', '1.1.9'))
        self.assertFalse(selfupdate.is_newer('garbage', '1.1.7'))
        self.assertFalse(selfupdate.is_newer('1.1.8', 'dev'))

    def test_a_preview_is_announced_only_in_test_mode(self):
        self.srv.latest = '1.1.8-preview.1'
        self.assertTrue(selfupdate.check('1.1.7')['ok'])
        with mock.patch.object(selfupdate, 'relay_base', lambda: self.srv.base), mock.patch.object(selfupdate, 'test_mode', lambda: False):
            self.assertEqual(selfupdate.check('1.1.7')['cause'], 'current')

    def test_relay_failures_never_offer(self):
        # nolabel: an answer without "asset" offers nothing, as before 1.2.0 (the relay always sends the label)
        for relay, cause in (('down', 'check'), ('garbage', 'check'), ('nohash', 'nodata'), ('bigsize', 'nodata'), ('wrongasset', 'nodata'),
                             ('nolabel', 'nodata')):
            self.srv.relay = relay
            r = selfupdate.check('1.1.7')
            self.assertFalse(r['ok'], relay); self.assertEqual(r['cause'], cause, relay)

    def test_relay_label_old_or_new_name(self):
        # The live relay validates and returns the OLD name as its "asset" label (1.1.7 clients require exactly that string), and it
        # stays unchanged: a client accepts the old label, the new one, and nothing else. The label never picks the file.
        self.assertEqual(RELAY_LABEL, 'MechAssault 2 Recomp Installer.exe')
        self.assertEqual(selfupdate.ASSET_LABELS, (ASSET, OLD_ASSET))
        for relay in ('ok', 'newlabel'):
            self.srv.relay = relay
            r = selfupdate.check('1.1.7')
            self.assertTrue(r['ok'], relay); self.assertEqual(r['asset'], ASSET, relay)
        self.srv.relay = 'ok'
        exe = selfupdate.download(self.info(), '1.1.7')
        self.assertEqual(exe.name, ASSET, 'saved under the new name whatever the label says')

    def test_relay_offline(self):
        os.environ['MA2_UPDATE_RELAY'] = CLOSED
        r = selfupdate.check('1.1.7', timeout=3)
        self.assertEqual((r['ok'], r['cause']), (False, 'check'))
        os.environ['MA2_UPDATE_RELAY'] = 'https://nothing.invalid'
        self.assertFalse(selfupdate.check('1.1.7', timeout=3)['ok'])

    def test_relay_redirect_is_not_followed(self):
        with mock.patch.object(self.srv, 'latest_answer', lambda: (302, b'')):
            self.assertFalse(selfupdate.check('1.1.7')['ok'])

    def test_off_switches_send_nothing(self):
        n = len(self.srv.requests)
        with mock.patch.dict(os.environ, {'MA2_SELF_UPDATE': '0'}):
            self.assertEqual(selfupdate.check('1.1.7')['cause'], 'off')
            with self.assertRaises(selfupdate.UpdateError) as c:
                selfupdate.download({'ok': True, 'latest': '1.1.8', 'sha256': 'a' * 64, 'size': 2 << 20}, '1.1.7')
            self.assertEqual(c.exception.cause, 'off')
        selfupdate.switch_off()
        self.assertEqual(selfupdate.check('1.1.7')['cause'], 'off')
        self.assertEqual(len(self.srv.requests), n, 'nothing was sent')

    def test_overrides_never_fall_back_to_the_real_relay(self):
        os.environ['MA2_SELF_UPDATE_TEST'] = ''
        os.environ['MA2_UPDATE_RELAY'] = self.srv.base                # loopback http without the test switch: no check at all
        self.assertEqual(selfupdate.relay_base(), '')
        self.assertEqual(selfupdate.check('1.1.7')['cause'], 'unset')
        os.environ.pop('MA2_UPDATE_RELAY'); os.environ['MA2_REPORT_RELAY'] = CLOSED   # the test suites' guard
        self.assertEqual(selfupdate.relay_base(), '')
        os.environ['MA2_REPORT_RELAY'] = 'https://relay.example.test'
        self.assertEqual(selfupdate.relay_base(), 'https://relay.example.test')
        os.environ.pop('MA2_REPORT_RELAY')
        with mock.patch('report.relay_url', lambda: 'https://ma2-report-relay.example.test'):
            self.assertEqual(selfupdate.relay_base(), 'https://ma2-report-relay.example.test')
        with mock.patch('report.relay_url', lambda: ''):
            self.assertEqual(selfupdate.relay_base(), '')

    def test_check_never_raises(self):
        with mock.patch.object(selfupdate, 'relay_base', boom):
            self.assertFalse(selfupdate.check('1.1.7')['ok'])


# ---------------------------------------------------------------------------- where downloads may come from

class Hosts(Base):
    def test_production_address_and_hosts(self):
        os.environ['MA2_SELF_UPDATE_TEST'] = ''
        rel = 'https://github.com/Mazillion/MechAssault2-PC-Recomp/releases/download/v1.1.8/'
        self.assertEqual(selfupdate.download_urls('1.1.8'),
                         [rel + 'MechAssault.2.Expanded.Installer.exe', rel + 'MechAssault%202%20Expanded%20Installer.exe',
                          rel + 'MechAssault.2.Recomp.Installer.exe', rel + 'MechAssault%202%20Recomp%20Installer.exe'])
        self.assertEqual(selfupdate.download_url('1.1.8'), selfupdate.download_urls('1.1.8')[0])
        ok = selfupdate._url_ok
        H = selfupdate.DOWNLOAD_HOSTS
        for u in ('https://github.com/x', 'https://release-assets.githubusercontent.com/a?sig=1', 'https://objects.githubusercontent.com/a'):
            self.assertTrue(ok(u, H), u)
        for u in ('http://github.com/x', 'https://evil.example/x', 'https://github.com.evil.example/x', 'https://evilgithub.com/x',
                  'https://user:pw' + '@' + 'github.com/x', 'ftp://github.com/x', 'http://127.0.0.1:1/x', 'https://127.0.0.1/x', 'file:///C:/x'):
            self.assertFalse(ok(u, H), u)

    def test_github_override_only_in_test_mode(self):
        self.assertEqual(selfupdate.github_base(), self.srv.base)
        os.environ['MA2_SELF_UPDATE_TEST'] = ''
        self.assertEqual(selfupdate.github_base(), 'https://github.com')
        os.environ['MA2_SELF_UPDATE_TEST'] = '1'; os.environ['MA2_SELF_UPDATE_GITHUB'] = 'http://evil.example'
        self.assertEqual(selfupdate.github_base(), 'https://github.com')


# ---------------------------------------------------------------------------- GitHub's asset naming (1.2.1)

class AssetName(Base):
    def downloads(self):
        return [r['path'].rsplit('/', 1)[1] for r in self.srv.requests if '/releases/download/' in r['path']]

    def test_names(self):
        self.assertEqual(ASSET, 'MechAssault 2 Expanded Installer.exe')
        self.assertEqual(selfupdate.OLD_ASSETS, ('MechAssault 2 Recomp Installer.exe',))
        self.assertEqual(selfupdate.INSTALLER_COPY, ASSET)
        self.assertEqual(selfupdate.OLD_INSTALLER_COPIES, selfupdate.OLD_ASSETS)

    def test_github_naming_rule(self):
        # The real releases (v1.1.7, v1.2.0): uploaded "MechAssault 2 Recomp Installer.exe", published "MechAssault.2.Recomp.Installer.exe".
        self.assertEqual(selfupdate.github_asset_name('MechAssault 2 Recomp Installer.exe'), 'MechAssault.2.Recomp.Installer.exe')
        self.assertEqual(selfupdate.github_asset_name('MechAssault 2 Expanded Installer.exe'), 'MechAssault.2.Expanded.Installer.exe')
        self.assertEqual(selfupdate.github_asset_name('MechAssault2-PC-Recomp-v1.2.0-Windows-x64.zip'),
                         'MechAssault2-PC-Recomp-v1.2.0-Windows-x64.zip')       # published unchanged
        self.assertEqual(selfupdate.github_asset_name('SHA256SUMS.txt'), 'SHA256SUMS.txt')
        self.assertEqual(selfupdate.github_asset_name('a b(c)+d_e-f.exe'), 'a.b.c.+d_e-f.exe')

    def test_order_of_attempts(self):
        self.assertEqual(selfupdate.asset_names(), ['MechAssault.2.Expanded.Installer.exe', 'MechAssault 2 Expanded Installer.exe',
                                                    'MechAssault.2.Recomp.Installer.exe', 'MechAssault 2 Recomp Installer.exe'])
        with mock.patch.object(selfupdate, 'OLD_ASSETS', ('Same.exe', 'MechAssault 2 Expanded Installer.exe', 'Same.exe')), \
             mock.patch.object(selfupdate, 'ASSET', 'Same.exe'):
            self.assertEqual(selfupdate.asset_names(), ['Same.exe', 'MechAssault.2.Expanded.Installer.exe',
                                                        'MechAssault 2 Expanded Installer.exe'], 'deduplicated')

    def test_published_new_name_is_the_only_request(self):
        exe = selfupdate.download(self.info(), '1.1.7', log=self.lines.append)
        self.assertEqual((exe.name, exe.read_bytes()), (ASSET, self.srv.payload))
        self.assertEqual(self.downloads(), ['MechAssault.2.Expanded.Installer.exe'])

    def test_fallback_to_uploaded_name_on_404(self):
        self.srv.published = 'raw'
        exe = selfupdate.download(self.info(), '1.1.7', log=self.lines.append)
        self.assertEqual((exe.name, exe.read_bytes()), (ASSET, self.srv.payload))
        self.assertEqual(self.downloads(), ['MechAssault.2.Expanded.Installer.exe', 'MechAssault%202%20Expanded%20Installer.exe'])
        self.assertTrue(any('trying the next one' in l for l in self.lines))

    def test_fallback_to_the_old_name(self):
        # a release published before the rename (the asset is "MechAssault.2.Recomp.Installer.exe"): found, saved under the new name
        for pub, tried in (('old', ['MechAssault.2.Expanded.Installer.exe', 'MechAssault%202%20Expanded%20Installer.exe',
                                    'MechAssault.2.Recomp.Installer.exe']),
                           ('oldraw', ['MechAssault.2.Expanded.Installer.exe', 'MechAssault%202%20Expanded%20Installer.exe',
                                       'MechAssault.2.Recomp.Installer.exe', 'MechAssault%202%20Recomp%20Installer.exe'])):
            self.srv.published = pub; self.srv.requests.clear()
            exe = selfupdate.download(self.info(), '1.1.7', log=self.lines.append)
            self.assertEqual((exe.name, exe.read_bytes()), (ASSET, self.srv.payload), pub)
            self.assertEqual(self.downloads(), tried, pub)
            shutil.rmtree(exe.parent, ignore_errors=True)

    def test_404_on_all_names_is_a_server_error(self):
        self.srv.mode = '404'
        paths = self.fail_download('server')
        self.assertEqual(len([r for r in paths if '/releases/download/' in r['path']]), 4)

    def test_only_a_404_moves_on(self):
        for mode, cause in (('500', 'server'), ('evil', 'host')):
            self.srv.mode = mode; self.srv.requests.clear()
            self.fail_download(cause)
            self.assertEqual(self.downloads(), ['MechAssault.2.Expanded.Installer.exe'], mode)

    def test_the_mock_is_like_github(self):
        # The gate's suites rely on this: the mock serves the release asset ONLY under the published (dotted) name by default,
        # so every self-update suite goes through the dotted path, and a client asking for the name with spaces gets a 404.
        import mock_update, urllib.request, urllib.error
        self.assertEqual(mock_update.served_name('github'), 'MechAssault.2.Expanded.Installer.exe')
        self.assertEqual(self.srv.published, 'github')
        with self.assertRaises(urllib.error.HTTPError) as c:
            urllib.request.urlopen(self.srv.base + '/Mazillion/MechAssault2-PC-Recomp/releases/download/v1.1.8/MechAssault%202%20Expanded%20Installer.exe')
        self.assertEqual(c.exception.code, 404)


# ---------------------------------------------------------------------------- the download

class Download(Base):
    def test_happy_path(self):
        info = self.info()
        seen = []
        exe = selfupdate.download(info, '1.1.7', progress=lambda d, t: seen.append((d, t)), log=self.lines.append)
        self.assertEqual(exe.name, ASSET)
        self.assertTrue(exe.parent.name.startswith(selfupdate.TEMP_PREFIX) and exe.parent.parent == self.tempdir)
        self.assertEqual(sorted(p.name for p in exe.parent.iterdir()), [ASSET], 'a fresh folder with only the verified file')
        self.assertEqual(exe.read_bytes(), self.srv.payload)
        self.assertEqual(seen[-1], (len(self.srv.payload), len(self.srv.payload)))
        paths = [r['path'] for r in self.srv.requests]
        self.assertEqual([p for p in paths if '/releases/download/' in p][0], '/Mazillion/MechAssault2-PC-Recomp/releases/download/v1.1.8/MechAssault.2.Expanded.Installer.exe',
                         'the published (dotted) name first, as GitHub stores it')
        self.assertEqual(len([p for p in paths if '/releases/download/' in p]), 1, 'found at the first name, nothing else asked')
        self.assertTrue(any(p.startswith('/assets/1') for p in paths), 'the GitHub-style redirect was followed')
        for r in self.srv.requests:
            self.assertNotIn('Cookie', r['headers']); self.assertNotIn('Authorization', r['headers'])
            self.assertEqual(r['headers']['User-Agent'], 'MechAssault2-Recomp-Installer/1.1.7')

    def test_without_redirect(self):
        self.srv.mode = 'nodirect'
        self.assertEqual(selfupdate.download(self.info(), '1.1.7').read_bytes(), self.srv.payload)

    def test_hash_mismatch_deletes_and_refuses(self):
        self.srv.mode = 'mismatch'
        self.fail_download('mismatch')

    def test_redirect_outside_github_is_refused_before_contact(self):
        self.srv.mode = 'evil'
        reqs = self.fail_download('host')
        self.assertFalse(any('evil' in r['path'] for r in reqs))

    def test_redirect_to_plain_http_is_refused(self):
        self.srv.mode = 'plainhttp'; self.fail_download('host')

    def test_redirect_with_a_user_name_is_refused(self):
        self.srv.mode = 'userinfo'; self.fail_download('host')

    def test_redirect_loop(self):
        self.srv.mode = 'loop'; self.fail_download('server')

    def test_size_caps(self):
        for mode in ('long', 'short', 'lenbad'):
            self.srv.mode = mode
            self.fail_download('size')

    def test_announced_size_above_the_cap(self):
        info = self.info(); info['size'] = selfupdate.MAX_BYTES + 1
        with self.assertRaises(selfupdate.UpdateError) as c: selfupdate.download(info, '1.1.7')
        self.assertEqual(c.exception.cause, 'nodata')

    def test_github_down(self):
        for mode in ('404', '500'):
            self.srv.mode = mode; self.fail_download('server')

    def test_github_unreachable(self):
        os.environ['MA2_SELF_UPDATE_GITHUB'] = CLOSED
        self.fail_download('blocked', timeout=3)

    def test_timeout(self):
        self.srv.mode = 'slow'
        with mock.patch.object(selfupdate, 'BLOCK', 1 << 20):
            info = self.info()
            with self.assertRaises(selfupdate.UpdateError) as c:
                selfupdate.download(info, '1.1.7', timeout=0.05)
            self.assertIn(c.exception.cause, ('timeout', 'server', 'size'))
        self.assertEqual(self.leftovers(), [])

    def test_cancel(self):
        self.srv.mode = 'slow'
        seen = []
        self.fail_download('cancelled', progress=lambda d, t: seen.append(d), cancelled=lambda: bool(seen))

    def test_antivirus_quarantine_after_download(self):
        with mock.patch.object(selfupdate, 'file_sha256', lambda p: '0' * 64):
            self.fail_download('quarantine')
        def gone(p): raise PermissionError(13, 'blocked')
        with mock.patch.object(selfupdate, 'file_sha256', gone):
            self.fail_download('quarantine')

    def test_disk_full(self):
        with mock.patch.object(tempfile, 'mkdtemp', lambda **k: (_ for _ in ()).throw(OSError(28, 'no space'))):
            with self.assertRaises(selfupdate.UpdateError) as c: selfupdate.download(self.info(), '1.1.7')
        self.assertEqual(c.exception.cause, 'disk')

    def test_every_cause_has_one_plain_sentence(self):
        for cause, t in selfupdate.CAUSE_TEXT.items():
            self.assertEqual(t.count('. '), 0, cause); self.assertTrue(t.endswith('.'), cause)
            self.assertNotRegex(t.lower(), r'sha|hash|relay|http|redirect|json|cache|reddit|log\b')
        self.assertEqual(selfupdate.RELEASES_PAGE, 'https://github.com/Mazillion/MechAssault2-PC-Recomp/releases')


# ---------------------------------------------------------------------------- the hand-over

class Launch(Base):
    def exe(self):
        exe = selfupdate.download(self.info(), '1.1.7')
        return exe

    def test_launch_window_and_command_line(self):
        exe = self.exe()
        calls = []
        class P:
            pid = 4242
        with mock.patch.object(selfupdate.subprocess, 'Popen', lambda cmd, **kw: (calls.append((cmd, kw)), P())[1]):
            os.environ['MA2_INSTALLER_EXE'] = r'C:\old\installer.exe'
            selfupdate.launch(exe, self.srv.sha, r'C:\Games\MA2', '1.1.7')
            selfupdate.launch(exe, self.srv.sha, r'C:\Games\MA2', '1.1.7', cli_args=['--update', '--dest', r'C:\Games\MA2'])
        (c1, k1), (c2, k2) = calls
        self.assertEqual(c1, [str(exe)])
        self.assertEqual(c2, [str(exe), '--update', '--dest', r'C:\Games\MA2'])
        for k in (k1, k2):
            self.assertEqual(k['env']['MA2_HANDOFF_WAIT'], '1')
            self.assertEqual(k['env']['MA2_SELF_UPDATE_DEST'], r'C:\Games\MA2')
            self.assertNotIn('MA2_INSTALLER_EXE', k['env'])
        self.assertTrue(k1['creationflags'] & 0x8)          # detached: the window version outlives this one

    def test_changed_before_start_is_not_run(self):
        exe = self.exe()
        exe.write_bytes(b'tampered')
        with mock.patch.object(selfupdate.subprocess, 'Popen', boom):
            with self.assertRaises(selfupdate.UpdateError) as c: selfupdate.launch(exe, self.srv.sha, 'x', '1.1.7')
        self.assertEqual(c.exception.cause, 'quarantine')

    def test_start_blocked(self):
        exe = self.exe()
        for code, cause in ((225, 'quarantine'), (2, 'quarantine'), (193, 'start')):
            e = OSError(code, 'x'); e.winerror = code
            with mock.patch.object(selfupdate.subprocess, 'Popen', lambda *a, **k: (_ for _ in ()).throw(e)):
                with self.assertRaises(selfupdate.UpdateError) as c: selfupdate.launch(exe, self.srv.sha, 'x', '1.1.7')
            self.assertEqual(c.exception.cause, cause, code)

    def test_handoff_dest_is_read_once(self):
        os.environ['MA2_SELF_UPDATE_DEST'] = r'C:\Games\MA2'; os.environ['MA2_HANDOFF_WAIT'] = '1'
        self.assertEqual(selfupdate.handoff_dest(), r'C:\Games\MA2')
        self.assertEqual(selfupdate.handoff_dest(), '')
        self.assertNotIn('MA2_HANDOFF_WAIT', os.environ)

    def test_cleanup_old_keeps_the_running_one(self):
        a = self.tempdir / 'MA2Update-old'; b = self.tempdir / 'MA2Update-mine'; c = self.tempdir / 'Other'
        for d in (a, b, c): d.mkdir(); (d / ASSET).write_bytes(b'x')
        os.environ['MA2_INSTALLER_EXE'] = str(b / ASSET)
        self.assertEqual(selfupdate.cleanup_old(), 1)
        self.assertFalse(a.exists()); self.assertTrue(b.exists()); self.assertTrue(c.exists())

    def test_shortcut_kinds(self):
        kf = lambda w: self.tmp / 'lnk' / w
        stamp = {'shortcuts': [str(kf('desktop') / 'MechAssault 2 (Recomp).lnk'), str(kf('start') / 'MechAssault 2 (Recomp).lnk'),
                               str(kf('start') / 'MA2 Scenario Editor.lnk'), str(kf('start') / 'Update MechAssault 2.lnk')]}
        self.assertEqual(selfupdate.shortcut_kinds(stamp, kf), ['desktop', 'start', 'editor'])
        self.assertEqual(selfupdate.shortcut_kinds({}, kf), [])


# ---------------------------------------------------------------------------- install.py

def fake_install(dest, version='1.1.7', shortcuts=()):
    for f in ('MechAssault 2.exe', 'game_files/ma2.xbe', 'pc_assets/common.mgf'):
        p = Path(dest) / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b'x')
    (Path(dest) / precheck.STAMP).write_text(json.dumps({'kit_id': 'k', 'public_version': version, 'shortcuts': list(shortcuts)}))


class Cli(Base):
    def run_su(self, dest):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = install.run_self_update(str(dest), log=lambda s: print(s))
        return code, out.getvalue(), err.getvalue()

    def test_happy_path_hands_over_with_the_shortcuts(self):
        dest = self.tmp / 'Games' / 'MA2'
        fake_install(dest, shortcuts=[str(self.tmp / 'lnk' / 'start' / 'MechAssault 2 (Recomp).lnk')])
        calls = []
        with mock.patch.object(install, 'PUBLIC_VERSION', '1.1.7'), \
             mock.patch.object(selfupdate, 'launch', lambda *a, **k: calls.append((a, k))):
            code, out, err = self.run_su(dest)
        self.assertEqual(code, 0, err)
        (exe, sha, d, own), kw = calls[0]
        self.assertEqual((Path(exe).read_bytes(), sha, Path(d), own), (self.srv.payload, self.srv.sha, dest.resolve(), '1.1.7'))
        self.assertEqual(kw['cli_args'], ['--update', '--dest', str(dest.resolve()), '--shortcuts', 'start'])
        self.assertIn('Version 1.1.8 is available', out)

    def test_low_priority_and_jobs_are_passed_on(self):
        dest = self.tmp / 'MA2'; fake_install(dest)
        calls = []
        with mock.patch.object(install, 'PUBLIC_VERSION', '1.1.7'), \
             mock.patch.object(selfupdate, 'launch', lambda *a, **k: calls.append(k)):
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(io.StringIO()):
                self.assertEqual(install.run_self_update(str(dest), log=lambda s: None, extra=['--low-priority', '--jobs', '2']), 0)
        self.assertEqual(calls[0]['cli_args'][-3:], ['--low-priority', '--jobs', '2'])
        src = (REPO / 'kit' / 'installer' / 'install.py').read_text(encoding='utf-8')
        self.assertIn("(['--low-priority'] if a.low_priority else [])", src)

    def test_current_is_exit_3(self):
        dest = self.tmp / 'MA2'; fake_install(dest)
        with mock.patch.object(install, 'PUBLIC_VERSION', '1.1.8'):
            self.assertEqual(self.run_su(dest)[0], 3)

    def test_failures_print_one_sentence_and_the_releases_page(self):
        dest = self.tmp / 'MA2'; fake_install(dest)
        self.srv.mode = 'mismatch'
        with mock.patch.object(install, 'PUBLIC_VERSION', '1.1.7'), mock.patch.object(selfupdate, 'launch', boom):
            code, out, err = self.run_su(dest)
        self.assertEqual(code, 1)
        self.assertIn(selfupdate.text('mismatch'), err); self.assertIn(selfupdate.RELEASES_PAGE, err)
        self.assertEqual(self.leftovers(), [])
        os.environ['MA2_UPDATE_RELAY'] = CLOSED
        with mock.patch.object(install, 'PUBLIC_VERSION', '1.1.7'):
            code, out, err = self.run_su(dest)
        self.assertEqual(code, 1); self.assertIn(selfupdate.text('check'), err)

    def test_refusals(self):
        self.assertEqual(self.run_su(self.tmp / 'nothing')[0], 1)
        dest = self.tmp / 'MA2'; fake_install(dest)
        (dest / '_work').mkdir(); (dest / '_work' / 'state.json').write_text('{}')
        code, out, err = self.run_su(dest)
        self.assertEqual(code, 1); self.assertIn('not finished', err)
        (dest / '_work' / 'state.json').unlink()
        with mock.patch.dict(os.environ, {'MA2_SELF_UPDATE': '0'}):
            code, out, err = self.run_su(dest)
        self.assertEqual(code, 1); self.assertIn(selfupdate.text('off'), err)
        self.assertEqual([r for r in self.srv.requests], [], 'nothing was sent')

    def test_cli_flags(self):
        src = (REPO / 'kit' / 'installer' / 'install.py').read_text(encoding='utf-8')
        self.assertIn("'--self-update'", src); self.assertIn("'--no-self-update'", src)
        self.assertIn('if a.no_self_update: selfupdate.switch_off()', src)


class InstallFolder(Base):
    def inst(self, shortcuts=('start',)):
        dest = self.tmp / 'Games' / 'MA2'; fake_install(dest)
        i = install.Installer(None, dest, shortcuts=list(shortcuts), log=self.lines.append)
        i.state = {'done': []}
        return i

    def runner(self, data=b'MZ installer'):
        exe = self.tmp / 'dl' / ASSET; exe.parent.mkdir(parents=True, exist_ok=True); exe.write_bytes(data)
        os.environ['MA2_INSTALLER_EXE'] = str(exe)
        return exe

    def test_copy_and_update_entry(self):
        i = self.inst(); self.runner()
        self.assertTrue(i.copy_installer())
        self.assertEqual((i.dest / ASSET).read_bytes(), b'MZ installer')
        self.assertFalse(i.copy_installer(), 'same bytes: left alone')
        self.runner(b'MZ newer'); self.assertTrue(i.copy_installer())
        self.assertEqual((i.dest / ASSET).read_bytes(), b'MZ newer')
        self.assertFalse((i.dest / (ASSET + '.new')).exists())
        i.make_shortcuts()
        lnk = Path(os.environ['MA2_SHORTCUT_DIR']) / 'start' / install.UPDATE_LNK
        self.assertTrue(lnk.is_file()); self.assertIn(str(lnk), i.state['shortcuts'])

    def test_no_start_menu_no_update_entry(self):
        i = self.inst(shortcuts=('desktop',)); self.runner(); i.copy_installer(); i.make_shortcuts()
        self.assertFalse(any(install.UPDATE_LNK in s for s in i.state['shortcuts']))
        self.assertTrue((i.dest / ASSET).is_file(), 'the copy is there either way')

    def test_running_from_the_copy_is_left_alone(self):
        i = self.inst(); (i.dest / ASSET).write_bytes(b'MZ copy')
        os.environ['MA2_INSTALLER_EXE'] = str(i.dest / ASSET)
        self.assertFalse(i.copy_installer())

    def test_off_switch_no_copy_no_entry(self):
        i = self.inst(); self.runner()
        with mock.patch.dict(os.environ, {'MA2_SELF_UPDATE': '0'}):
            self.assertFalse(i.copy_installer()); i.make_shortcuts()
        self.assertFalse((i.dest / ASSET).exists())
        self.assertFalse(any(install.UPDATE_LNK in s for s in i.state['shortcuts']))
        selfupdate.switch_off(); self.assertFalse(i.copy_installer())

    def test_not_run_from_the_exe(self):
        i = self.inst(); os.environ.pop('MA2_INSTALLER_EXE', None)
        self.assertFalse(i.copy_installer())

    def test_copy_failure_is_logged_and_the_install_goes_on(self):
        src = (REPO / 'kit' / 'installer' / 'install.py').read_text(encoding='utf-8')
        self.assertIn("try: self.copy_installer()", src)
        self.assertIn("except Exception as e: self.log('  (the installer was not copied into the install folder: %s)' % e)", src)
        i = self.inst(); self.runner()
        with mock.patch.object(install.shutil, 'copy2', lambda *a, **k: (_ for _ in ()).throw(OSError(5, 'denied'))):
            with self.assertRaises(OSError): i.copy_installer()
        self.assertFalse((i.dest / (ASSET + '.new')).exists())

    def test_enabled_failing_keeps_the_other_shortcuts(self):
        i = self.inst(); self.runner(); i.copy_installer()
        with mock.patch.object(selfupdate, 'enabled', boom):
            i.make_shortcuts()
        names = [Path(s).name for s in i.state['shortcuts']]
        self.assertIn(install.GAME_LNK, names); self.assertNotIn(install.UPDATE_LNK, names)

    def test_manifest_leaves_the_copy_out(self):
        i = self.inst(); self.runner(); i.copy_installer()
        self.assertNotIn(ASSET, install.manifest_files(i.dest)); self.assertIn('MechAssault 2.exe', install.manifest_files(i.dest))

    def test_uninstall_removes_copy_and_entry(self):
        i = self.inst(); self.runner(); i.copy_installer(); i.make_shortcuts(); i.write_stamp(i.dest / 'pc_settings.ini')
        lnk = Path(os.environ['MA2_SHORTCUT_DIR']) / 'start' / install.UPDATE_LNK
        plan = install.uninstall_plan(i.dest)
        self.assertIn(lnk, plan['shortcuts'])
        res = install.uninstall(i.dest, log=self.lines.append)
        self.assertEqual(res['failed'], []); self.assertFalse(lnk.exists()); self.assertFalse(i.dest.exists())

    def test_uninstall_from_the_running_copy(self):
        i = self.inst(); (i.dest / ASSET).write_bytes(b'MZ')
        os.environ['MA2_INSTALLER_EXE'] = str(i.dest / ASSET)
        me, dest = str((i.dest / ASSET).resolve()), str(i.dest.resolve())
        calls = []
        with mock.patch.object(install.subprocess, 'Popen', lambda cmd, **kw: calls.append((cmd, kw))):
            self.assertEqual(install.later_for_running_copy(i.dest, [(me, 'in use'), (dest, 'not empty')], self.lines.append), [])
            other = [(me, 'in use'), (str(i.dest / 'game_files'), 'in use')]
            self.assertEqual(install.later_for_running_copy(i.dest, other, self.lines.append), other)
        self.assertEqual(len(calls), 1)
        cmd, kw = calls[0]
        self.assertTrue(cmd.startswith('cmd.exe /d /s /c "for /l')); self.assertIn(me, cmd); self.assertTrue(kw['creationflags'] & 0x08000000)
        # cmd puts everything after "if ..." into the if: the wait must come before it, or the loop spins 90 times at once
        self.assertLess(cmd.index('ping -n 2'), cmd.index('if not exist'))
        self.assertTrue(cmd.rstrip('"').endswith('exit 0)'))

    def test_deferred_delete_really_deletes(self):
        i = self.inst(); (i.dest / ASSET).write_bytes(b'MZ')
        os.environ['MA2_INSTALLER_EXE'] = str(i.dest / ASSET)
        for p in list(i.dest.iterdir()):
            if p.name != ASSET: shutil.rmtree(p) if p.is_dir() else p.unlink()
        self.assertEqual(install.later_for_running_copy(i.dest, [(str((i.dest / ASSET).resolve()), 'in use'), (str(i.dest.resolve()), 'x')],
                                                        self.lines.append), [])
        import time
        t = time.time()
        while i.dest.exists() and time.time() - t < 20: time.sleep(0.3)
        self.assertFalse(i.dest.exists(), 'the hidden command deleted the copy and the folder')
        os.environ['MA2_INSTALLER_EXE'] = str(self.tmp / 'elsewhere.exe')
        self.assertIsNone(install.running_copy(i.dest))

    def test_uninstall_survives_a_fault_in_the_new_step(self):
        i = self.inst()
        with mock.patch.object(install, 'later_for_running_copy', boom):
            res = install.uninstall(i.dest, log=self.lines.append)
        self.assertEqual(res['failed'], []); self.assertFalse(i.dest.exists())


class Stub(unittest.TestCase):
    def test_stub_waits_only_for_a_handoff(self):
        src = (REPO / 'installer-exe' / 'stub.cpp').read_text(encoding='utf-8')
        self.assertIn('MA2_HANDOFF_WAIT', src)
        self.assertIn('WaitForSingleObject(single, 120000)', src)
        self.assertIn('SetEnvironmentVariableW(L"MA2_HANDOFF_WAIT", nullptr);', src)
        self.assertRegex(src, re.escape('fail(L"The MechAssault 2 ') + r'[A-Za-z ]+' + re.escape(' Installer is already running.", console); return 1; }'))


class Faults(unittest.TestCase):
    def test_fault_entries(self):
        faults = json.loads((Path(__file__).with_name('gate_faults.json')).read_text(encoding='utf-8'))['faults']
        ids = {f['target'] for f in faults}
        for t in ('Installer.copy_installer', 'enabled', 'cleanup_old', 'running_copy', 'later_for_running_copy', 'check', 'download', 'launch',
                  'handoff_dest'):
            self.assertIn(t, ids)
        for f in faults:
            mod = install if f['module'] == '__main__' else __import__(f['module'])
            obj = mod
            for part in f['target'].split('.'): obj = getattr(obj, part)


if __name__ == '__main__':
    unittest.main(verbosity=2)

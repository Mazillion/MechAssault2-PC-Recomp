"""Self-update (1.1.7): an installer started on an existing install can fetch the newest installer and hand the update to it.

Nothing here runs without the player's click (the window's "Update to X", or the command line's --self-update), never in the
background, and never on a fresh install. The flow:

  1. check()     GET <relay>/v1/latest (the same anonymous request the game makes; only the version in the User-Agent). The relay
                 answers {"ok": true, "latest": "1.1.8", "sha256": "<64 hex>", "size": <bytes>, "asset": "<ASSET>"}.
  2. download()  GET https://github.com/<REPO>/releases/download/v<latest>/<ASSET> into a NEW folder in %TEMP% (MA2Update-*):
                 HTTPS only, redirects followed only to GitHub's own hosts (DOWNLOAD_HOSTS), at most `size` bytes (and never more
                 than MAX_BYTES), Cancel between blocks. The file must have exactly the relay's size and SHA-256: the GitHub release
                 and the relay are two channels, and both must agree, or the file is deleted and nothing runs.
  3. launch()    checks the file's SHA-256 once more, starts it (window: no arguments; command line: --update --dest <install>) with
                 MA2_HANDOFF_WAIT=1 (its stub waits until this installer has closed) and MA2_SELF_UPDATE_DEST=<install>, then this
                 installer exits. The new installer updates the same folder through its normal update path (the kept build).

Any failure leaves the install untouched and is reported as one plain sentence (CAUSE_TEXT) plus RELEASES_PAGE.

Off switch: MA2_SELF_UPDATE=0 (or the command line's --no-self-update, which calls switch_off()). Test hooks (only with
MA2_SELF_UPDATE_TEST=1): MA2_UPDATE_RELAY / MA2_REPORT_RELAY may be a plain http address on this PC, and MA2_SELF_UPDATE_GITHUB
replaces https://github.com with a test server on this PC (loopback hosts are then also allowed as redirect targets). Without the
test switch an override that is not https disables the check (it never falls back to the real relay).

Standard library only."""
import hashlib, json, os, re, shutil, socket, ssl, subprocess, tempfile, time, urllib.error, urllib.parse, urllib.request
from pathlib import Path

REPO = 'Mazillion/MechAssault2-PC-Recomp'
ASSET = 'MechAssault 2 Recomp Installer.exe'
GITHUB = 'https://github.com'
RELEASES_PAGE = 'https://github.com/%s/releases' % REPO
# Where a release download may go (GitHub docs, "Communication requirements for GitHub-hosted / self-hosted runners": release
# assets are served from release-assets.githubusercontent.com; older releases from objects / github-releases.githubusercontent.com).
DOWNLOAD_HOSTS = frozenset(('github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com',
                            'objects-origin.githubusercontent.com', 'github-releases.githubusercontent.com'))
LOOPBACK = frozenset(('127.0.0.1', 'localhost', '::1'))
MIN_BYTES = 1024 * 1024
MAX_BYTES = 200 * 1024 * 1024
MAX_ANSWER = 4096
MAX_REDIRECTS = 5
CHECK_TIMEOUT = 10
DOWNLOAD_TIMEOUT = 60
BLOCK = 256 * 1024
TEMP_PREFIX = 'MA2Update-'
INSTALLER_COPY = ASSET                 # the copy kept in the install folder (install.py)
SHA_RE = re.compile(r'^[0-9a-f]{64}$')
RELEASE_RE = re.compile(r'^\d{1,4}(\.\d{1,4}){1,3}$')
TEST_RE = re.compile(r'^\d{1,4}(\.\d{1,4}){1,3}(-preview\.\d{1,4})?$')

CAUSE_TEXT = {
    'off': 'Updating from the installer is switched off on this PC, so nothing was changed.',
    'unset': 'This installer cannot look for updates, so nothing was changed.',
    'check': 'The newest version could not be checked right now, so nothing was changed.',
    'current': 'This installer is already the newest version, so nothing was changed.',
    'nodata': 'The new version cannot be downloaded from here yet, so nothing was changed.',
    'offline': 'The update could not be downloaded (no internet connection), so nothing was changed.',
    'timeout': 'The update could not be downloaded (the connection timed out), so nothing was changed.',
    'tls': 'The update could not be downloaded (Windows could not verify the secure connection), so nothing was changed.',
    'blocked': 'The update could not be downloaded (the connection was blocked), so nothing was changed.',
    'server': 'The update could not be downloaded (GitHub did not answer properly), so nothing was changed.',
    'host': 'The download was sent somewhere other than GitHub, so it was stopped and nothing was changed.',
    'size': 'The download was not the expected size, so it was deleted and nothing was changed.',
    'mismatch': 'The downloaded installer did not match the published one, so it was deleted and nothing was changed.',
    'quarantine': 'Windows or your antivirus removed or blocked the downloaded installer, so nothing was changed.',
    'disk': 'The update could not be saved in the temporary folder (is the drive full?), so nothing was changed.',
    'start': 'The new installer could not be started, so nothing was changed.',
    'cancelled': 'The update was cancelled, so nothing was changed.',
    'error': 'The update did not work, so nothing was changed.',
}
LINK_TEXT = 'Open the releases page'

_switched_off = False


class UpdateError(Exception):
    def __init__(self, cause, detail=''):
        super().__init__(detail or cause)
        self.cause = cause if cause in CAUSE_TEXT else 'error'
        self.detail = detail


def switch_off():
    """The command line's --no-self-update."""
    global _switched_off
    _switched_off = True


def enabled():
    return not _switched_off and os.environ.get('MA2_SELF_UPDATE', '1').strip() != '0'


def test_mode():
    return os.environ.get('MA2_SELF_UPDATE_TEST', '').strip() == '1'


def text(cause):
    return CAUSE_TEXT.get(cause, CAUSE_TEXT['error'])


def _url_ok(url, hosts):
    """https to one of `hosts`; in test mode also http/https to this PC."""
    try:
        u = urllib.parse.urlparse(url)
        host = (u.hostname or '').lower()
    except ValueError:
        return False
    if u.username or u.password: return False
    if u.scheme == 'https' and host in hosts: return True
    return test_mode() and u.scheme in ('http', 'https') and host in LOOPBACK


def relay_base():
    """The relay's address, or '' (no check). MA2_UPDATE_RELAY, else MA2_REPORT_RELAY, override it: https always, a loopback http
    address only in test mode; any other override switches the check off instead of falling back to the real relay."""
    for var in ('MA2_UPDATE_RELAY', 'MA2_REPORT_RELAY'):
        v = (os.environ.get(var) or '').strip()
        if not v: continue
        u = urllib.parse.urlparse(v)
        if u.scheme == 'https' and u.hostname: return v.rstrip('/')
        if test_mode() and u.scheme == 'http' and (u.hostname or '') in LOOPBACK: return v.rstrip('/')
        return ''
    try:
        import report
        url = report.relay_url()
    except Exception:
        return ''
    return url if url.lower().startswith('https://') else ''


def github_base():
    v = (os.environ.get('MA2_SELF_UPDATE_GITHUB') or '').strip()
    if v and test_mode() and _url_ok(v, ()): return v.rstrip('/')
    return GITHUB


def version_key(v):
    """(numbers, final) for comparing: 1.1.7-preview.2 < 1.1.7 < 1.1.8. None when it does not parse."""
    m = re.match(r'^(\d{1,4}(?:\.\d{1,4}){1,3})(?:-preview\.(\d{1,4}))?$', str(v or '').strip())
    if not m: return None
    nums = tuple(int(x) for x in m.group(1).split('.'))
    nums = nums + (0,) * (4 - len(nums))
    return nums + ((1, 0) if m.group(2) is None else (0, int(m.group(2))))


def is_newer(latest, own):
    a, b = version_key(latest), version_key(own)
    return bool(a and b and a > b)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None                    # urllib then raises HTTPError with the 3xx status and its Location header


def _opener():
    return urllib.request.build_opener(_NoRedirect)


def _cause(e):
    if isinstance(e, urllib.error.HTTPError): return 'server'
    reason = getattr(e, 'reason', e)
    if isinstance(reason, ssl.SSLError) or isinstance(e, ssl.SSLError) or 'CERTIFICATE_VERIFY_FAILED' in str(e): return 'tls'
    if isinstance(reason, socket.gaierror) or getattr(reason, 'winerror', None) in (11001, 11002, 11004): return 'offline'
    if isinstance(reason, (socket.timeout, TimeoutError)) or isinstance(e, (socket.timeout, TimeoutError)) or 'timed out' in str(reason): return 'timeout'
    if isinstance(reason, ConnectionRefusedError) or getattr(reason, 'winerror', None) in (10013, 10061): return 'blocked'
    if isinstance(reason, OSError) and getattr(reason, 'winerror', None) in (10051, 10065, 10050): return 'offline'
    return 'server'


def user_agent(own):
    return 'MechAssault2-Recomp-Installer/%s' % (own or '-')


def check(own, timeout=CHECK_TIMEOUT, log=lambda s: None):
    """Ask the relay for the newest release. Never raises. Returns {'ok', 'cause', 'latest', 'sha256', 'size', 'asset'}:
    ok True only when a newer version than `own` is out AND the relay gave its installer's SHA-256 and size."""
    res = {'ok': False, 'cause': 'check', 'latest': None, 'sha256': '', 'size': 0, 'asset': ASSET}
    try:
        if not enabled():
            res['cause'] = 'off'; return res
        base = relay_base()
        if not base:
            res['cause'] = 'unset'; return res
        req = urllib.request.Request(base + '/v1/latest', headers={'User-Agent': user_agent(own), 'Accept': 'application/json'})
        try:
            with _opener().open(req, timeout=timeout) as r:
                raw = r.read(MAX_ANSWER + 1)
        except Exception as e:
            log('self-update: the relay did not answer (%s)' % _cause(e))
            return res
        if len(raw) > MAX_ANSWER: return res
        try: j = json.loads(raw.decode('utf-8'))
        except ValueError: return res
        if not isinstance(j, dict) or j.get('ok') is not True: return res
        latest = j.get('latest')
        if not isinstance(latest, str) or not (TEST_RE if test_mode() else RELEASE_RE).match(latest):
            res['cause'] = 'current'; return res
        res['latest'] = latest
        if not is_newer(latest, own):
            res['cause'] = 'current'; return res
        sha, size, asset = j.get('sha256'), j.get('size'), j.get('asset')
        if not (isinstance(sha, str) and SHA_RE.match(sha) and isinstance(size, int) and not isinstance(size, bool)
                and MIN_BYTES <= size <= MAX_BYTES and asset == ASSET):
            res['cause'] = 'nodata'; return res
        res.update(ok=True, cause='newer', sha256=sha, size=size)
        return res
    except Exception as e:                 # never break the caller
        log('self-update: check failed (%s: %s)' % (type(e).__name__, e))
        return res


def download_url(version):
    return '%s/%s/releases/download/v%s/%s' % (github_base(), REPO, urllib.parse.quote(version), urllib.parse.quote(ASSET))


def _hosts():
    return DOWNLOAD_HOSTS


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def _open_following(url, own, timeout, log):
    """Open `url`, following at most MAX_REDIRECTS redirects, each checked against DOWNLOAD_HOSTS before it is contacted."""
    for hop in range(MAX_REDIRECTS + 1):
        if not _url_ok(url, _hosts()):
            log('self-update: refused a download address outside GitHub (%s)' % (urllib.parse.urlparse(url).hostname or '?'))
            raise UpdateError('host')
        # No cookies, no tokens, no ids: the address and a version-only User-Agent.
        req = urllib.request.Request(url, headers={'User-Agent': user_agent(own), 'Accept': 'application/octet-stream'})
        try:
            return _opener().open(req, timeout=timeout)
        except urllib.error.HTTPError as e:
            if e.code in (301, 302, 303, 307, 308):
                loc = e.headers.get('Location') or ''
                e.close()
                if not loc: raise UpdateError('server', 'redirect without an address')
                url = urllib.parse.urljoin(url, loc)
                continue
            raise UpdateError('server', 'HTTP %d' % e.code)
        except UpdateError:
            raise
        except Exception as e:
            raise UpdateError(_cause(e), str(e))
    raise UpdateError('server', 'too many redirects')


def download(info, own, progress=lambda done, total: None, cancelled=lambda: False, timeout=DOWNLOAD_TIMEOUT, log=lambda s: None,
             temp_root=None):
    """Download and verify the installer of `info` (a check() result with ok True). Returns the verified file's Path, in a new
    MA2Update-* folder. Raises UpdateError; on any failure the folder is deleted."""
    if not enabled(): raise UpdateError('off')
    if not info or not info.get('ok'): raise UpdateError('nodata')
    size, want = int(info['size']), str(info['sha256']).lower()
    if not (MIN_BYTES <= size <= MAX_BYTES) or not SHA_RE.match(want): raise UpdateError('nodata')
    try:
        folder = Path(tempfile.mkdtemp(prefix=TEMP_PREFIX, dir=temp_root))
    except OSError as e:
        raise UpdateError('disk', str(e))
    part = folder / (ASSET + '.part')
    final = folder / ASSET
    ok = False
    try:
        url = download_url(info['latest'])
        log('self-update: downloading %s from GitHub (%d bytes expected)' % (info['latest'], size))
        tried_windows = False
        while True:
            try:
                r = _open_following(url, own, timeout, log)
                break
            except UpdateError as e:
                if e.cause == 'tls' and not tried_windows:
                    tried_windows = True       # a fresh Windows fetches missing root certificates only for its own connections
                    try:
                        import download as dl
                        if dl._windows_fetch(GITHUB + '/', None):
                            log('self-update: Windows checked the secure connection; trying again'); continue
                    except Exception:
                        pass
                raise
        h = hashlib.sha256(); done = 0
        with r:
            length = r.headers.get('Content-Length')
            if length is not None:
                try:
                    if int(length) != size: raise UpdateError('size', 'Content-Length %s' % length)
                except ValueError:
                    raise UpdateError('server', 'bad Content-Length')
            try:
                with open(part, 'wb') as f:
                    while True:
                        if cancelled(): raise UpdateError('cancelled')
                        try:
                            b = r.read(BLOCK)
                        except Exception as e:
                            raise UpdateError(_cause(e), str(e))
                        if not b: break
                        done += len(b)
                        if done > size: raise UpdateError('size', 'more bytes than announced')
                        h.update(b); f.write(b)
                        progress(done, size)
            except OSError as e:
                raise UpdateError('quarantine' if getattr(e, 'winerror', None) in (5, 225) else 'disk', str(e))
        if cancelled(): raise UpdateError('cancelled')
        if done != size: raise UpdateError('size', '%d of %d bytes' % (done, size))
        if h.hexdigest() != want:
            log('self-update: the download\'s SHA-256 does not match the relay\'s (deleted)')
            raise UpdateError('mismatch')
        try:
            part.replace(final)
            # Read it back from disk: an antivirus that quarantined or changed it shows here, before anything runs.
            if file_sha256(final) != want: raise UpdateError('quarantine', 'changed on disk')
        except UpdateError:
            raise
        except OSError as e:
            raise UpdateError('quarantine', str(e))
        log('self-update: %s verified (SHA-256 and size match the relay)' % info['latest'])
        ok = True
        return final
    finally:
        if not ok: shutil.rmtree(folder, ignore_errors=True)


def handoff_env(dest, own):
    env = dict(os.environ)
    env['MA2_HANDOFF_WAIT'] = '1'
    env['MA2_SELF_UPDATE_DEST'] = str(dest)
    env['MA2_SELF_UPDATE_FROM'] = str(own)
    for v in ('MA2_INSTALLER_EXE', 'MA2_INSTALLER_DIR', 'PYTHONHOME', 'PYTHONPATH'):     # the new stub sets its own
        env.pop(v, None)
    return env


def launch(exe, want_sha, dest, own, cli_args=None, log=lambda s: None):
    """Start the verified installer `exe` for an update of `dest` (window when cli_args is None, else the command line with
    cli_args) and return its Popen. Checks the SHA-256 again right before. Raises UpdateError."""
    exe = Path(exe)
    try:
        if file_sha256(exe) != str(want_sha).lower(): raise UpdateError('quarantine', 'changed before start')
    except OSError as e:
        raise UpdateError('quarantine', str(e))
    cmd = [str(exe)] + list(cli_args or [])
    flags = 0x00000200                      # CREATE_NEW_PROCESS_GROUP: Ctrl+C here does not reach it
    kw = {}
    if cli_args is None:
        kw.update(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
        flags |= 0x00000008                 # DETACHED_PROCESS
    else:
        kw.update(stdin=subprocess.DEVNULL)  # the command line run keeps writing to the same output
        try:
            import ctypes
            if not ctypes.windll.kernel32.GetConsoleWindow(): flags |= 0x08000000     # CREATE_NO_WINDOW
        except Exception:
            flags |= 0x08000000
    try:
        p = subprocess.Popen(cmd, env=handoff_env(dest, own), cwd=str(exe.parent), creationflags=flags, **kw)
    except OSError as e:
        code = getattr(e, 'winerror', None)
        raise UpdateError('quarantine' if code in (2, 3, 5, 225, 4551) else 'start', str(e))
    log('self-update: started the %s installer (process %d); this installer closes now' % (exe.name, p.pid))
    return p


def cleanup_old(log=lambda s: None):
    """Delete MA2Update-* folders in %TEMP% left by earlier self-updates (not the one this installer runs from)."""
    mine = os.path.normcase(str(Path(os.environ.get('MA2_INSTALLER_EXE') or '.').resolve().parent))
    n = 0
    try:
        for p in Path(tempfile.gettempdir()).glob(TEMP_PREFIX + '*'):
            if p.is_dir() and os.path.normcase(str(p.resolve())) != mine:
                shutil.rmtree(p, ignore_errors=True)
                if not p.exists(): n += 1
    except OSError:
        pass
    if n: log('  (%d earlier update download folder(s) removed)' % n)
    return n


def handoff_dest():
    """The install folder an older installer handed to this one (MA2_SELF_UPDATE_DEST), read once; '' when none."""
    d = (os.environ.pop('MA2_SELF_UPDATE_DEST', '') or '').strip()
    os.environ.pop('MA2_HANDOFF_WAIT', None)
    return d


def shortcut_kinds(stamp, known_folder):
    """The --shortcuts kinds an install had (desktop, start, editor) from its stamp, for the command-line handoff."""
    kinds = []
    for s in (stamp or {}).get('shortcuts') or []:
        p = Path(str(s))
        for where in ('desktop', 'start'):
            try:
                if os.path.normcase(str(p.parent)) == os.path.normcase(str(known_folder(where))) and where not in kinds: kinds.append(where)
            except Exception:
                pass
        if p.name == 'MA2 Scenario Editor.lnk' and 'editor' not in kinds: kinds.append('editor')
    return kinds

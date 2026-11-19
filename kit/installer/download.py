"""One download helper for everything the installer fetches (FFmpeg, the Vulkan upscaler, the Enhancer's models, the Build Tools
bootstrapper). It recovers on its own where it safely can and, when it can't, names the cause in one plain line.

  - resume: a stopped download continues from its .part file (HTTP Range); a server without Range support starts again
  - retry: timeouts, dropped connections and server errors are tried again (2 s, 5 s, 10 s); a 404 or a refused answer is not
  - checks: a file that does not start with the expected bytes (`magic`) is refused at once (a web page instead of it: named as a
    Wi-Fi login page), as is one far larger than expected; an https address is never redirected to plain http
  - fallback: any unexpected problem in this helper logs one line and downloads the plain 1.1.3 way (plain_download)
  - causes: no internet, Wi-Fi login page, proxy sign-in (407), secure connection not verified (TLS), blocked, server error
  - TLS: a fresh Windows install fetches missing root certificates only for Windows\' own connections, so on a certificate failure
    Windows connects once (curl.exe, Schannel) and Python tries again; if Python still can't, a checked file comes through Windows
  - cache: a file with a known SHA-256 (or a caller's `cache_key`) is kept in %LOCALAPPDATA%\\MechAssault2\\Cache, so a second
    install, a Start over or a reinstall does not download it again; entries unused for 60 days are removed
  - record: one line per download in Logs\\downloads.log (host, result, bytes, seconds; no query strings) for Collect diagnostics

Standard library only: the Enhancer's setup.py imports it too (through MA2_INSTALLER_DIR).
"""
import hashlib, http.client, json, os, shutil, socket, ssl, time, urllib.error, urllib.parse, urllib.request
from pathlib import Path

USER_AGENT = 'MechAssault2-Recomp-Installer'
RETRY_DELAYS = (2, 5, 10)
CACHE_DAYS = 60
CAUSES = {
    'offline': 'no internet connection (the server name could not be found)',
    'portal': 'the network answered with a web page instead of the file (a Wi-Fi login page or a filter); sign in to the network first',
    'proxy': 'a proxy server on this network asks for a sign-in',
    'tls': ('Windows couldn\'t verify the secure connection to the download site, also through Windows\' own connection (the PC clock '
            'may be wrong, or something on this network or an antivirus inspects secure connections)'),
    'blocked': 'the connection was refused or blocked (a firewall or filter)',
    'server': 'the server did not answer properly',
    'notfound': 'the file is not on the server any more',
    'bad': 'what was downloaded is not the expected file',
    'timeout': 'the connection timed out',
}


class DownloadError(Exception):
    """A download failed; `cause` is a key of CAUSES, str(e) says what happened in one line."""
    def __init__(self, cause, detail=''):
        self.cause, self.detail = cause, detail
        super().__init__(CAUSES.get(cause, cause) + (' (%s)' % detail if detail else ''))


class Cancelled(Exception):
    pass


def _local():
    return Path(os.environ.get('LOCALAPPDATA') or Path.home() / 'AppData' / 'Local') / 'MechAssault2'


def cache_dir():
    return _local() / 'Cache' / 'downloads'


def host(url):
    try:
        return urllib.parse.urlparse(url).netloc.rsplit('@', 1)[-1] or url
    except ValueError:
        return '?'


def record(url, result, nbytes=0, seconds=0.0, via=''):
    """One line in Logs\\downloads.log (newest 400 kept): what Collect diagnostics reports about downloads."""
    try:
        d = _local() / 'Logs'; d.mkdir(parents=True, exist_ok=True)
        f = d / 'downloads.log'
        line = json.dumps({'time': time.strftime('%Y-%m-%dT%H:%M:%S'), 'host': host(url), 'file': Path(urllib.parse.urlparse(url).path).name,
                           'result': result, 'bytes': nbytes, 'seconds': round(seconds, 1), 'via': via})
        old = f.read_text(encoding='utf-8').splitlines()[-399:] if f.is_file() else []
        f.write_text('\n'.join(old + [line]) + '\n', encoding='utf-8')
    except (OSError, ValueError):
        pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''): h.update(block)
    return h.hexdigest()


def _cache_name(key):
    return ''.join(c if c.isalnum() or c in '-_.' else '_' for c in key)[:120]


def cached(key, digest=None):
    """The cached file for `key` (checked against `digest` when given; a damaged one is removed), or None."""
    p = cache_dir() / _cache_name(key)
    if not p.is_file(): return None
    if digest and sha256(p) != digest:
        drop_cache(key); return None
    try: os.utime(p)                                   # used: it stays another CACHE_DAYS
    except OSError: pass
    return p


def drop_cache(key):
    try: (cache_dir() / _cache_name(key)).unlink()
    except OSError: pass


def _store(key, path):
    try:
        d = cache_dir(); d.mkdir(parents=True, exist_ok=True)
        now = time.time()
        for old in d.iterdir():
            try:
                if old.is_file() and now - old.stat().st_mtime > CACHE_DAYS * 86400: old.unlink()
            except OSError:
                pass
        tmp = d / (_cache_name(key) + '.tmp')
        shutil.copyfile(path, tmp); os.replace(tmp, d / _cache_name(key))
    except OSError:
        pass                                           # the cache is a convenience only


class _HttpsOnlyRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if req.full_url.lower().startswith('https:') and not newurl.lower().startswith('https:'):
            raise DownloadError('blocked', 'redirected to a non-HTTPS address')
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def cause_of(e):
    """The CAUSES key for an exception from urllib / sockets / ssl."""
    if isinstance(e, DownloadError): return e.cause
    if isinstance(e, urllib.error.HTTPError):
        if e.code == 407: return 'proxy'
        if e.code == 404 or e.code == 410: return 'notfound'
        if e.code in (401, 403, 451): return 'blocked'
        return 'server'
    reason = getattr(e, 'reason', e)
    if isinstance(reason, ssl.SSLError) or isinstance(e, ssl.SSLError) or 'CERTIFICATE_VERIFY_FAILED' in str(e): return 'tls'
    if isinstance(reason, socket.gaierror) or getattr(reason, 'winerror', None) in (11001, 11002, 11004): return 'offline'
    if isinstance(reason, (socket.timeout, TimeoutError)) or 'timed out' in str(reason): return 'timeout'
    if 'Tunnel connection failed: 407' in str(e) or '407 Proxy' in str(e): return 'proxy'
    if isinstance(reason, ConnectionRefusedError) or getattr(reason, 'winerror', None) in (10013, 10061): return 'blocked'
    if isinstance(reason, OSError) and getattr(reason, 'winerror', None) in (10051, 10065, 10050): return 'offline'
    return 'server'


TRANSIENT = ('timeout', 'server')


def fetch(url, out, log=print, cancelled=lambda: False, max_bytes=None, magic=None, digest=None, cache_key=None, timeout=60,
          progress_step=16 << 20, delays=RETRY_DELAYS, checked=False):
    """Download `url` to `out` (a file path) and return `out`. `digest`: the expected SHA-256 (checked; also the cache key);
    `cache_key`: cache a file whose content the caller checks itself (drop_cache(key) when it turns out wrong). `file:` URLs work too.
    `checked`: the caller checks the content itself (recorded file hashes, a signature); like `digest`, it allows the download through
    Windows\' own HTTPS when Python cannot verify the secure connection but Windows can (a fresh Windows install's root certificates).
    Raises DownloadError (with a cause) or Cancelled. If the helper itself fails in an unexpected way, one log line is written and the
    file is downloaded the plain 1.1.3 way instead (no resume, no retries, no cache)."""
    try:
        return _fetch(url, out, log, cancelled, max_bytes, magic, digest, cache_key, timeout, progress_step, delays, checked)
    except (DownloadError, Cancelled):
        raise
    except Exception as e:
        log('    (download helper problem: %s: %s; downloading the plain way)' % (type(e).__name__, e))
        return plain_download(url, out, cancelled, max_bytes, digest)


def plain_download(url, out, cancelled=lambda: False, max_bytes=None, digest=None):
    """The 1.1.3 download: one request through out.part, nothing else. URL and HTTP errors are turned into DownloadError."""
    out = Path(out); part = out.with_name(out.name + '.part')
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT})
    try:
        with urllib.request.build_opener(_HttpsOnlyRedirect).open(req, timeout=60) as r, open(part, 'wb') as f:
            total = int(r.headers.get('Content-Length') or 0); got = 0
            for block in iter(lambda: r.read(1 << 20), b''):
                if cancelled(): raise Cancelled()
                f.write(block); got += len(block)
                if max_bytes and got > max_bytes: raise DownloadError('bad', 'far larger than expected')
        if total and got != total: raise DownloadError('timeout', 'stopped early at %d of %d bytes' % (got, total))
        if digest and sha256(part) != digest: raise DownloadError('bad', 'its SHA-256 differs from the published file')
        os.replace(part, out)
        return out
    except (DownloadError, Cancelled):
        try: part.unlink()
        except OSError: pass
        raise
    except Exception as e:
        try: part.unlink()
        except OSError: pass
        raise DownloadError(cause_of(e), _short(e)) from e


def _fetch(url, out, log, cancelled, max_bytes, magic, digest, cache_key, timeout, progress_step, delays, checked=False):
    out = Path(out); out.parent.mkdir(parents=True, exist_ok=True)
    key = digest or cache_key
    t0 = time.monotonic()
    part = out.with_name(out.name + '.part')
    if part.exists() and not digest:
        part.unlink()                                  # a part from an earlier run is resumed only when the result can be checked
    if key:
        c = cached(key, digest)
        if c:
            shutil.copyfile(c, out)
            log('    (from the download cache)'); record(url, 'cache', out.stat().st_size, 0, 'cache'); return out
    attempt, last, via = 0, None, ''
    win = None                                         # TLS recovery: None = not tried, True/False = Windows could connect
    win_download_tried = False
    while True:
        if cancelled(): raise Cancelled()
        try:
            got = _once(url, part, log, cancelled, max_bytes, magic, timeout, progress_step)
            if digest and sha256(part) != digest:
                part.unlink(missing_ok=True)
                raise DownloadError('bad', 'its SHA-256 differs from the published file')
            os.replace(part, out)
            record(url, 'ok', got, time.monotonic() - t0, via or ('resumed' if attempt else ''))
            if key: _store(key, out)
            return out
        except Cancelled:
            record(url, 'cancelled', part.stat().st_size if part.is_file() else 0, time.monotonic() - t0); raise
        except (OSError, http.client.HTTPException, DownloadError) as e:   # network trouble (anything else: fetch() falls back)
            cause = cause_of(e)
            last = e if isinstance(e, DownloadError) else DownloadError(cause, _short(e))
            if cause in ('bad', 'portal'): part.unlink(missing_ok=True)       # never resume onto something wrong
            if cause == 'tls' and win is None:
                # A fresh Windows install fetches trusted root certificates on demand, but only for Windows\' own
                # connections; Python reads the store as it is. Let Windows connect to the same address once
                # (that adds the missing root), then let Python try again (the loop: every check as usual).
                try:
                    win = bool(_windows_fetch(url, None))
                    log('    (asked Windows to check the secure connection: %s)' % ('ok' if win else 'failed'))
                except Exception as pe:                                         # never let the helper itself stop the install
                    win = False; log('    (Windows connection check unavailable: %s: %s)' % (type(pe).__name__, pe))
                if win:
                    via = 'after root update'; continue
            elif cause == 'tls' and win and not win_download_tried and (digest or checked):
                # Windows connects but Python still can't: download through Windows itself, for a file whose content is
                # checked (its SHA-256 here, or the caller's own checks: an archive's recorded file hashes, a signature).
                win_download_tried = True
                try:
                    if _windows_fetch(url, part, max_bytes):
                        problem = _check_file(part, digest, magic)
                        if problem:
                            part.unlink(missing_ok=True); log('    (the Windows download was refused: %s)' % problem)
                            last = DownloadError('bad', problem)
                        else:
                            os.replace(part, out); record(url, 'ok', out.stat().st_size, time.monotonic() - t0, 'windows')
                            if key: _store(key, out)
                            log('    (downloaded through Windows)'); return out
                    else:
                        log('    (the Windows download failed too)')
                except Exception as we:                                         # the original error stands
                    log('    (Windows download unavailable: %s: %s)' % (type(we).__name__, we))
                part.unlink(missing_ok=True)                                    # nothing of it is resumed later
            if cause not in TRANSIENT or attempt >= len(delays):
                record(url, 'failed: %s' % last.cause, part.stat().st_size if part.is_file() else 0, time.monotonic() - t0)
                raise last from e
            log('    (download interrupted: %s; trying again in %d s)' % (last, delays[attempt]))
            for _ in range(int(delays[attempt] * 10)):
                if cancelled(): raise Cancelled()
                time.sleep(0.1)
            attempt += 1


def _windows_fetch(url, out, max_bytes=None):
    """Use Windows\' own HTTPS stack (System32 curl.exe, Schannel), which builds the certificate chain with Windows and so
    fetches a missing trusted root on demand. out=None: just open the connection (headers only; any HTTP answer counts, since
    only the secure connection matters, and redirects are followed so the file host's root is fetched too). Returns True on
    success. Runs without a window; https only (never redirected to plain http)."""
    import subprocess
    exe = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'curl.exe'
    if not exe.is_file() or not url.lower().startswith('https://'): return False
    args = [str(exe), '-sS', '-L', '--proto', '=https', '--proto-redir', '=https', '--ssl-revoke-best-effort',
            '--connect-timeout', '30', '-A', USER_AGENT]
    if out:
        args += ['--fail', '-o', str(out), '--max-time', '3600'] + (['--max-filesize', str(max_bytes)] if max_bytes else [])
    else:
        args += ['-I', '-o', os.devnull, '--max-time', '60']
    r = subprocess.run(args + [url], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       creationflags=0x08000000, timeout=3700 if out else 90)           # CREATE_NO_WINDOW
    return r.returncode == 0 and (out is None or Path(out).is_file())


def _check_file(path, digest, magic):
    """What is wrong with a file downloaded outside _once (None: nothing)."""
    if magic:
        with open(path, 'rb') as f:
            if not f.read(len(magic)).startswith(magic): return 'it does not start like the expected file'
    if digest and sha256(path) != digest: return 'its SHA-256 differs from the published file'
    return None


def fetch_and_use(url, out, use, log=print, cancelled=lambda: False, cache_key=None, **kw):
    """Download `url` to `out`, then call use(path) (unpack and check it); `out` is deleted afterwards. With `cache_key` (only for
    https addresses: the published files) a file that `use` accepted is kept in the cache and tried first next time; a cached
    file that `use` refuses is dropped and the file downloaded again."""
    out = Path(out)
    try:
        c = cached(cache_key) if cache_key and url.lower().startswith('https:') else None
    except Exception as e:                             # the cache is a convenience: any problem with it means "no cache"
        log('    (download cache not used: %s)' % e); c = cache_key = None
    if cache_key and url.lower().startswith('https:'):
        if c:
            try:
                use(c); log('    (from the download cache)'); record(url, 'cache', c.stat().st_size, 0, 'cache'); return
            except Cancelled:
                raise
            except Exception as e:
                if type(e).__name__ == 'Cancelled': raise                     # the caller's own Cancelled
                log('    (the cached copy was not used: %s)' % e); drop_cache(cache_key)
    else:
        cache_key = None
    kw.setdefault('checked', True)                     # use() checks the file (and may refuse it) before anything relies on it
    fetch(url, out, log, cancelled, **kw)
    try:
        use(out)
        if cache_key: _store(cache_key, out)
    finally:
        try: out.unlink()
        except OSError: pass


def _short(e):
    s = str(getattr(e, 'reason', '') or e)
    return s if len(s) < 160 else s[:157] + '...'


def _once(url, part, log, cancelled, max_bytes, magic, timeout, progress_step):
    """One request: appends to `part` when the server honours Range, else rewrites it. Returns the file's total size."""
    have = part.stat().st_size if part.is_file() else 0
    headers = {'User-Agent': USER_AGENT}
    if have and not url.lower().startswith('file:'): headers['Range'] = 'bytes=%d-' % have
    req = urllib.request.Request(url, headers=headers)
    opener = urllib.request.build_opener(_HttpsOnlyRedirect)
    try:
        r = opener.open(req, timeout=timeout)
    except urllib.error.HTTPError as e:
        if e.code == 416 and have:                     # the part is already complete (or wrong): start again
            part.unlink(missing_ok=True); raise DownloadError('server', 'range not satisfiable')
        raise
    with r:
        status = getattr(r, 'status', 200)
        resumed = status == 206 and have
        length = int(r.headers.get('Content-Length') or 0)
        total = (have + length) if resumed else length
        if max_bytes and total > max_bytes: raise DownloadError('bad', 'the server offers %d MB' % (total >> 20))
        if resumed: log('    resuming at %d MB' % (have >> 20))
        mode = 'ab' if resumed else 'wb'
        got = have if resumed else 0
        first = b''
        next_report = (got // progress_step + 1) * progress_step if progress_step else None
        with open(part, mode) as f:
            for block in iter(lambda: r.read(1 << 20), b''):
                if cancelled(): raise Cancelled()
                if not resumed and len(first) < 16: first += block[:16 - len(first)]
                if not resumed and magic and len(first) >= len(magic) and got == 0 and not first.startswith(magic):
                    raise DownloadError('portal' if first.lstrip()[:1] == b'<' else 'bad', 'it does not start like the expected file')
                f.write(block); got += len(block)
                if max_bytes and got > max_bytes: raise DownloadError('bad', 'far larger than expected')
                if next_report and got >= next_report and total > progress_step:
                    log('    %d of %d MB' % (got >> 20, total >> 20)); next_report += progress_step
    if total and got != total: raise DownloadError('timeout', 'stopped early at %d of %d bytes' % (got, total))
    if magic and not resumed and got < len(magic): raise DownloadError('bad', 'empty answer')
    return got

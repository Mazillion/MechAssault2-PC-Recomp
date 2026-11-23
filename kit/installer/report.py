"""Send report (1.1.6): one click sends a problem report to the project's relay, which files it for the developer and answers with a
report number (MA2-XXXXXX) the player can quote. Nothing is sent without that click. When no relay address is set, or the send
fails, the report stays on this PC as a zip (the window says where), exactly like Collect diagnostics.

The relay address comes from ONE place: data/report.json {"relay_url": "https://..."} next to this file (set before the release
build; empty = no Send button, Save only). MA2_REPORT_RELAY overrides it for tests and support, but only with an https:// address or a
plain http:// address on this PC (127.0.0.1 / localhost: a test relay).

Wire protocol (the relay implements the same):
  POST <relay_url>/v1/report                       (no redirects are followed: a redirect counts as a failed send)
  headers  Content-Type: multipart/form-data; boundary=...
           User-Agent:      MechAssault2-Recomp-Report/<version> (<client>)
           X-MA2-Client:    installer | game | helper
           X-MA2-Version:   the public version, e.g. 1.1.6
           X-MA2-Build:     the build id (the installer's kit id; the game's build hash)
           X-MA2-Kind:      install-fail | crash | hang | manual
           X-MA2-Signature: a short title signature, at most 120 characters of [A-Za-z0-9 ._:/+-] (installer: "<stage>:<failure kind>",
                            the game: "<exception code>@<guest address>"); empty for a manual report
  body     form fields client, version, build, kind, signature (the same values again), text (the player's words, UTF-8, at most
           2,000 characters), summary (the report_summary lines, UTF-8, at most 16 KB), and the file part zip
           (filename "report.zip", Content-Type application/zip, at most 20 MB; it starts with PK\\x03\\x04)
  answer   JSON {"ok": true|false, "id": "MA2-7F3K9Q", "message": "..."}: ok + an id of the form MA2- and 6 base32 letters = sent;
           anything else (ok false, HTTP 4xx/5xx, 429 = too many reports, no JSON) = not sent, and `message` (when given) is shown
           to the player as the relay's own words (an old client can be told to update without a client change).

Standard library only. The installer window uses send_report(); a standalone helper (R4, MA2 Report.exe) implements the same protocol
(this docstring) and can be tested against the same mock relay (tests/installer/mock_relay.py)."""
import json, os, re, socket, ssl, time, urllib.error, urllib.parse, urllib.request, uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = HERE / 'data' / 'report.json'
ENDPOINT = '/v1/report'
CLIENTS = ('installer', 'game', 'helper')
KINDS = ('install-fail', 'crash', 'hang', 'manual')
MAX_TEXT = 2000
MAX_SUMMARY = 16 * 1024
MAX_ZIP = 20 * 1024 * 1024
TIMEOUT = 60                                     # seconds per network operation (connect, each block sent, the answer)
REPORT_ID = re.compile(r'^MA2-[A-Z2-7]{6}$')
SIGNATURE_CHARS = re.compile(r'[^A-Za-z0-9 ._:/+-]')
CAUSES = {
    'sent': 'sent',
    'unset': 'sending is not set up in this version',
    'too_large': 'the report is larger than 20 MB',
    'rate_limited': 'the report service is busy (too many reports right now)',
    'refused': 'the report service did not take the report',
    'server': 'the report service did not answer properly',
    'offline': 'no internet connection',
    'timeout': 'the connection timed out',
    'tls': 'Windows couldn\'t verify the secure connection to the report service',
    'blocked': 'the connection was refused or blocked (a firewall or filter)',
    'error': 'the report could not be sent',
}


def relay_url():
    """The relay's base address ('' = not set up). MA2_REPORT_RELAY (tests, support) wins when it is https or a loopback http address."""
    env = (os.environ.get('MA2_REPORT_RELAY') or '').strip()
    if env and _allowed(env): return env.rstrip('/')
    try:
        url = str((json.loads(CONFIG.read_text(encoding='utf-8')) or {}).get('relay_url') or '').strip()
    except (OSError, ValueError, AttributeError):
        return ''
    return url.rstrip('/') if url.lower().startswith('https://') else ''


def _allowed(url):
    u = urllib.parse.urlparse(url)
    if u.scheme == 'https' and u.hostname: return True
    return u.scheme == 'http' and u.hostname in ('127.0.0.1', 'localhost', '::1')


def available():
    """True when this build can send (a relay address is set)."""
    return bool(relay_url())


def signature(*parts):
    """A title signature from plain parts (installer stage, failure kind ...): allowed characters only, at most 120."""
    s = ':'.join(str(p) for p in parts if p not in (None, ''))
    return SIGNATURE_CHARS.sub('', s)[:120]


def clean_message(text, n=300):
    """The relay's message, safe to show: printable characters only, one paragraph, at most n characters."""
    s = re.sub(r'[\x00-\x1f\x7f]+', ' ', str(text or '')).strip()
    return s if len(s) <= n else s[:n - 1] + '…'


def multipart(fields, zip_bytes, boundary=None):
    """(content type, body bytes): the form fields (UTF-8 text) and the zip as the file part 'zip'."""
    boundary = boundary or ('ma2-' + uuid.uuid4().hex)
    out = []
    for name, value in fields:
        out.append(('--%s\r\nContent-Disposition: form-data; name="%s"\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n' % (boundary, name)).encode('ascii'))
        out.append(str(value).encode('utf-8') + b'\r\n')
    if zip_bytes is not None:                               # 1.2.0: an answer without logs has no zip part
        out.append(('--%s\r\nContent-Disposition: form-data; name="zip"; filename="report.zip"\r\nContent-Type: application/zip\r\n\r\n' % boundary).encode('ascii'))
        out.append(zip_bytes + b'\r\n')
    out.append(('--%s--\r\n' % boundary).encode('ascii'))
    return 'multipart/form-data; boundary=%s' % boundary, b''.join(out)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise urllib.error.HTTPError(req.full_url, code, 'redirect refused', headers, fp)


def _cause(e):
    if isinstance(e, urllib.error.HTTPError):
        return 'rate_limited' if e.code == 429 else 'too_large' if e.code == 413 else 'refused' if 400 <= e.code < 500 else 'server'
    reason = getattr(e, 'reason', e)
    if isinstance(reason, ssl.SSLError) or isinstance(e, ssl.SSLError) or 'CERTIFICATE_VERIFY_FAILED' in str(e): return 'tls'
    if isinstance(reason, socket.gaierror) or getattr(reason, 'winerror', None) in (11001, 11002, 11004): return 'offline'
    if isinstance(reason, (socket.timeout, TimeoutError)) or isinstance(e, (socket.timeout, TimeoutError)) or 'timed out' in str(reason): return 'timeout'
    if isinstance(reason, ConnectionRefusedError) or getattr(reason, 'winerror', None) in (10013, 10061): return 'blocked'
    if isinstance(reason, OSError) and getattr(reason, 'winerror', None) in (10051, 10065, 10050): return 'offline'
    return 'server' if isinstance(e, (OSError, urllib.error.URLError)) else 'error'


def _answer(raw):
    try:
        j = json.loads(raw.decode('utf-8', 'replace')) if raw else None
    except ValueError:
        return None
    return j if isinstance(j, dict) else None


def send(zip_path, text='', summary='', kind='manual', sig='', client='installer', version='', build='', url=None, timeout=None,
         log=lambda s: None, reply_secret=None):
    """POST one report. Never raises: returns {'ok', 'cause' (a CAUSES key), 'id', 'message' (the relay's words, safe to show),
    'http' (status or None), 'seconds'}. `url`: the relay base address (default relay_url()); '' = not set up.
    reply_secret (1.2.0): 64 hex characters sent as the form field reply_secret; the relay keeps only its SHA-256."""
    t0 = time.monotonic()
    res = {'ok': False, 'cause': 'error', 'id': '', 'message': '', 'http': None, 'seconds': 0.0}
    try:
        base = relay_url() if url is None else (url.rstrip('/') if url and _allowed(url) else '')
        if not base:
            res['cause'] = 'unset'; return res
        data = Path(zip_path).read_bytes()
        if len(data) > MAX_ZIP:
            res['cause'] = 'too_large'; return res
        if not data.startswith(b'PK\x03\x04'):
            res['cause'] = 'error'; res['message'] = ''; log('report: the zip is not a zip'); return res
        client = client if client in CLIENTS else 'installer'
        kind = kind if kind in KINDS else 'manual'
        sig = signature(sig)
        meta = [('client', client), ('version', version), ('build', build), ('kind', kind), ('signature', sig)]
        if reply_secret and SECRET_RE.match(str(reply_secret)): meta.append(('reply_secret', reply_secret))
        ctype, body = multipart(meta + [('text', str(text or '')[:MAX_TEXT]), ('summary', str(summary or '')[:MAX_SUMMARY])], data)
        headers = {'Content-Type': ctype, 'User-Agent': 'MechAssault2-Recomp-Report/%s (%s)' % (version or '-', client),
                   'X-MA2-Client': client, 'X-MA2-Version': version, 'X-MA2-Build': build, 'X-MA2-Kind': kind, 'X-MA2-Signature': sig,
                   'Accept': 'application/json'}
        target = base + ENDPOINT
        tried_windows = False
        while True:
            req = urllib.request.Request(target, data=body, headers=headers, method='POST')
            try:
                with urllib.request.build_opener(_NoRedirect).open(req, timeout=TIMEOUT if timeout is None else timeout) as r:
                    res['http'] = getattr(r, 'status', 200)
                    j = _answer(r.read(64 * 1024))
                break
            except urllib.error.HTTPError as e:
                res['http'] = e.code
                try: j = _answer(e.read(64 * 1024))
                except Exception: j = None
                res['cause'] = _cause(e)
                if j and j.get('message'): res['message'] = clean_message(j.get('message'))
                return res
            except Exception as e:
                cause = _cause(e)
                if cause == 'tls' and not tried_windows and target.lower().startswith('https://'):
                    # A fresh Windows install fetches missing root certificates only for Windows' own connections (download.py):
                    # let Windows connect once, then try again.
                    tried_windows = True
                    try:
                        import download
                        if download._windows_fetch(base + '/', None):
                            log('report: Windows checked the secure connection; sending again'); continue
                    except Exception as we:
                        log('report: Windows connection check unavailable (%s)' % type(we).__name__)
                res['cause'] = cause; return res
        if j and j.get('message'): res['message'] = clean_message(j.get('message'))
        rid = str((j or {}).get('id') or '')
        if j and j.get('ok') is True and REPORT_ID.match(rid):
            res.update(ok=True, cause='sent', id=rid)
        else:
            res['cause'] = 'refused' if j and j.get('ok') is False else 'server'
        return res
    except Exception as e:                                   # never let the sender break its caller
        res['cause'] = 'error'; log('report: send failed (%s: %s)' % (type(e).__name__, e))
        return res
    finally:
        res['seconds'] = round(time.monotonic() - t0, 2)


def make_and_send(dest, out_dir, text='', kind='manual', sig='', client='installer', version='', build='', gpus=None, stage='',
                  failure='', log_lines=None, url=None, send_it=True, log=lambda s: None, store=None):
    """Collect a report bundle (precheck.collect_diagnostics with the report settings: the newest dump scrubbed when it is 15 MB or
    less, no bot recording, 20 MB at most) and send it. The zip is ALWAYS saved in out_dir first (it stays there: the player's local
    copy). Returns {'path', 'members', 'sizes', 'notes', 'summary', 'sent': send() result or None when not sent}."""
    import precheck
    dest = dest or precheck.default_install_folder()
    state = precheck.read_state(dest) if Path(dest).is_dir() else None
    summary = precheck.report_summary(dest, state, version, build, gpus, stage, failure, kind)
    if text and str(text).strip(): summary = summary + ['', 'What happened (the player\'s words) is in the text field.']
    res = precheck.collect_diagnostics(dest, out_dir, version, build, gpus, include_dump=True, include_botrec=False, stage=stage,
                                       failure=failure, log_lines=log_lines, scrub=True, dump_limit=precheck.REPORT_DUMP_LIMIT,
                                       max_bytes=precheck.REPORT_MAX_BYTES, text=text, name_prefix='MA2-report')
    res['summary'] = summary
    res['sent'] = None
    if send_it:
        secret = new_secret()                               # 1.2.0: the key to this report's conversation (kept on this PC)
        res['sent'] = send(res['path'], precheck.redact(str(text or '').strip())[:MAX_TEXT], '\n'.join(summary), kind, sig, client,
                           version, build, url=url, log=log, reply_secret=secret)
        if res['sent'].get('ok'):
            try: store_add(res['sent']['id'], secret, path=store)
            except Exception as e: log('report: the reply key could not be kept (%s)' % type(e).__name__)
    return res


def outcome_text(sent, folder):
    """(headline, body) for the window after a send attempt."""
    if sent and sent.get('ok'):
        body = 'Press Copy number if you want to share it directly with the developer. ' + REPLY_HINT
        if sent.get('message'): body = sent['message'] + ' ' + body
        return 'Report sent. Your report number is %s.' % sent['id'], body
    body = 'The report was saved to %s.' % folder
    if sent is None or sent.get('cause') == 'unset': return 'Report saved.', body + ' Attach it to a problem report.'
    why = CAUSES.get(sent.get('cause'), CAUSES['error'])
    m = sent.get('message') or ''
    if m: body = m + ' ' + (('It is in %s.' % folder) if 'saved' in m.lower() else body)     # (the relay's refusals already say "saved")
    return 'Could not send (%s).' % why, body


# ---------------------------------------------------------------------------- 1.2.0: report conversations
# After a report is sent, this PC keeps {report number, date sent, date of the last message, a random key} in
# %LOCALAPPDATA%\MechAssault2\report-conversations.txt (the game and MA2 Report.exe read and write the same file). The key
# (256 random bits, 64 hex characters) went to the relay with the report; the relay keeps only its SHA-256, so only this
# PC can read the developer's reply or answer it. Entries are dropped 60 days after their last message. Nothing is asked
# of the relay unless an entry is younger than that: check_replies() then sends the numbers and keys, nothing else.
#
# File format (UTF-8, one entry per line, '#' lines are comments):
#   MA2-XXXXXX <sent yyyy-mm-dd> <last yyyy-mm-dd> <64 hex>
import datetime, secrets as _secrets

REPLY_HINT = 'If the developer replies, you\'ll see it the next time you start the game or this installer.'
STORE_NAME = 'report-conversations.txt'
STORE_HEADER = ('# MechAssault 2: reports sent from this PC that the developer can reply to (number, sent, last message, key).\n'
                '# Each line is removed 60 days after its last message. Deleting this file stops the game and the installer\n'
                '# from looking for replies.\n')
KEEP_DAYS = 60
MAX_MESSAGE = 1000
SECRET_RE = re.compile(r'^[0-9a-f]{64}$')
_LINE = re.compile(r'^(MA2-[A-Z2-7]{6}) (\d{4}-\d{2}-\d{2}) (\d{4}-\d{2}-\d{2}) ([0-9a-f]{64})$')
REPLIES_ENDPOINT = '/v1/replies'
ANSWER_ENDPOINT = '/v1/answer'
MONTHS = ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec')


def new_secret():
    return _secrets.token_hex(32)


def today():
    return datetime.datetime.now(datetime.timezone.utc).date()


def store_path():
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'MechAssault2' / STORE_NAME


def parse_store(text):
    """[{id, sent, last, secret}] from the file's text; malformed lines are skipped."""
    out = []
    for line in str(text or '').splitlines():
        m = _LINE.match(line.strip())
        if not m: continue
        try:
            sent, last = datetime.date.fromisoformat(m.group(2)), datetime.date.fromisoformat(m.group(3))
        except ValueError:
            continue
        out.append({'id': m.group(1), 'sent': sent, 'last': max(last, sent), 'secret': m.group(4)})
    return out


def format_store(entries):
    return STORE_HEADER + ''.join('%s %s %s %s\n' % (e['id'], e['sent'].isoformat(), e['last'].isoformat(), e['secret']) for e in entries)


def live(entries, now=None):
    """The entries whose last message is less than KEEP_DAYS days ago."""
    now = now or today()
    return [e for e in entries if (now - e['last']).days < KEEP_DAYS]


def store_read(path=None):
    p = Path(path) if path else store_path()
    try:
        return parse_store(p.read_text(encoding='utf-8'))
    except (OSError, UnicodeDecodeError):
        return []


def store_write(entries, path=None, now=None):
    """Writes the live entries (dropping old ones); deletes the file when none is left."""
    p = Path(path) if path else store_path()
    keep = live(entries, now)
    if not keep:
        try: p.unlink()
        except OSError: pass
        return keep
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_name(p.name + '.part')
    tmp.write_text(format_store(keep), encoding='utf-8', newline='\n')
    os.replace(tmp, p)
    return keep


def store_add(rid, secret, path=None, now=None):
    if not REPORT_ID.match(str(rid)) or not SECRET_RE.match(str(secret)): return
    now = now or today()
    entries = [e for e in store_read(path) if e['id'] != rid]
    entries.append({'id': rid, 'sent': now, 'last': now, 'secret': secret})
    store_write(entries, path, now)


def store_touch(rid, when=None, path=None):
    """A message in this conversation (a reply arrived, an answer was sent): its 60 days start again."""
    when = when or today()
    entries = store_read(path)
    for e in entries:
        if e['id'] == rid and when > e['last']: e['last'] = when
    store_write(entries, path)


def short_date(d):
    return '%s %d' % (MONTHS[d.month - 1], d.day)


def _post(base, endpoint, body, ctype, timeout, extra=None):
    """(HTTP status or None, JSON answer or None, cause). Never raises."""
    headers = {'Content-Type': ctype, 'Accept': 'application/json', 'User-Agent': 'MechAssault2-Recomp-Report'}
    headers.update(extra or {})
    req = urllib.request.Request(base + endpoint, data=body, headers=headers, method='POST')
    try:
        with urllib.request.build_opener(_NoRedirect).open(req, timeout=timeout) as r:
            return getattr(r, 'status', 200), _answer(r.read(256 * 1024)), 'sent'
    except urllib.error.HTTPError as e:
        try: j = _answer(e.read(64 * 1024))
        except Exception: j = None
        return e.code, j, _cause(e)
    except Exception as e:
        return None, None, _cause(e)


def check_replies(url=None, path=None, timeout=15, log=lambda s: None):
    """The developer's replies not seen yet: [{id, n, text, at, can_answer, sent (date)}], oldest report first.
    No entry younger than 60 days = no request at all. Any failure = []."""
    try:
        entries = live(store_read(path))
        if not entries: return []
        base = relay_url() if url is None else (url.rstrip('/') if url and _allowed(url) else '')
        if not base: return []
        body = json.dumps({'reports': [{'id': e['id'], 'secret': e['secret']} for e in entries[-20:]]}).encode('utf-8')
        status, j, cause = _post(base, REPLIES_ENDPOINT, body, 'application/json', timeout)
        if status != 200 or not j or j.get('ok') is not True:
            log('replies: none checked (%s)' % (status or cause)); return []
        sent = {e['id']: e['sent'] for e in entries}
        out = []
        for r in j.get('replies') or []:
            if not isinstance(r, dict) or r.get('id') not in sent or not isinstance(r.get('n'), int): continue
            text = clean_reply(r.get('text'))
            if not text: continue
            out.append({'id': r['id'], 'n': r['n'], 'text': text, 'at': str(r.get('at') or ''), 'can_answer': r.get('can_answer') is True,
                        'sent': sent[r['id']]})
        out.sort(key=lambda r: (r['sent'], r['id'], r['n']))
        if out:
            for rid in {r['id'] for r in out}: store_touch(rid, path=path)
        log('replies: %d waiting' % len(out))
        return out
    except Exception as e:
        log('replies: check failed (%s)' % type(e).__name__)
        return []


def clean_reply(text):
    """The developer's words, safe to show: printable characters and line breaks only, at most MAX_MESSAGE characters."""
    s = re.sub(r'[\x00-\x08\x0b-\x1f\x7f-\x9f\u2028\u2029\u202a-\u202e\u2066-\u2069]', '', str(text or '').replace('\r\n', '\n').replace('\t', ' '))
    return s.strip()[:MAX_MESSAGE]


def mark_seen(rid, n, url=None, path=None, timeout=15):
    """Tells the relay that reply n of report rid was shown (it is not returned again; the developer can no longer edit it)."""
    try:
        e = next((x for x in store_read(path) if x['id'] == rid), None)
        base = relay_url() if url is None else (url.rstrip('/') if url and _allowed(url) else '')
        if not e or not base: return False
        body = json.dumps({'reports': [{'id': rid, 'secret': e['secret'], 'seen': int(n)}]}).encode('utf-8')
        status, j, _ = _post(base, REPLIES_ENDPOINT, body, 'application/json', timeout)
        return status == 200 and bool(j and j.get('ok'))
    except Exception:
        return False


def send_answer(rid, text, attach=False, dest=None, out_dir=None, client='installer', version='', build='', gpus=None, url=None,
                path=None, timeout=None, log=lambda s: None):
    """The player's answer to the developer's newest reply. Scrubbed like report text, at most MAX_MESSAGE characters. With
    attach, a new report bundle (as Send report makes it; also saved in out_dir) goes with it. Never raises: returns
    {'ok', 'cause' (a CAUSES key, or 'empty' / 'closed'), 'message', 'http', 'logs' (zip path or '')}."""
    import precheck
    res = {'ok': False, 'cause': 'error', 'message': '', 'http': None, 'logs': ''}
    try:
        words = precheck.redact(str(text or '').strip())[:MAX_MESSAGE].strip()
        if not words:
            res['cause'] = 'empty'; return res
        e = next((x for x in store_read(path) if x['id'] == rid), None)
        base = relay_url() if url is None else (url.rstrip('/') if url and _allowed(url) else '')
        if not base:
            res['cause'] = 'unset'; return res
        if not e:
            res['cause'] = 'closed'; return res
        fields = [('id', rid), ('secret', e['secret']), ('text', words)]
        data = None
        if attach and out_dir:
            dest = dest or precheck.default_install_folder()
            state = precheck.read_state(dest) if Path(dest).is_dir() else None
            summary = precheck.report_summary(dest, state, version, build, gpus, '', '', 'manual')
            bundle = precheck.collect_diagnostics(dest, out_dir, version, build, gpus, include_dump=True, include_botrec=False,
                                                  scrub=True, dump_limit=precheck.REPORT_DUMP_LIMIT, max_bytes=precheck.REPORT_MAX_BYTES,
                                                  text=words, name_prefix='MA2-report')
            res['logs'] = bundle['path']
            data = Path(bundle['path']).read_bytes()
            if len(data) > MAX_ZIP: res['cause'] = 'too_large'; return res
            fields.append(('summary', '\n'.join(summary)[:MAX_SUMMARY]))
        ctype, body = multipart(fields, data)
        status, j, cause = _post(base, ANSWER_ENDPOINT, body, ctype, TIMEOUT if timeout is None else timeout,
                                 {'X-MA2-Client': client, 'X-MA2-Version': version, 'X-MA2-Build': build})
        res['http'] = status
        if j and j.get('message'): res['message'] = clean_message(j.get('message'))
        if status == 200 and j and j.get('ok') is True:
            res.update(ok=True, cause='sent')
            store_touch(rid, path=path)
        elif status in (404, 409):
            res['cause'] = 'closed'
        else:
            res['cause'] = cause if cause != 'sent' else 'server'
        return res
    except Exception as ex:
        log('answer: failed (%s: %s)' % (type(ex).__name__, ex))
        return res

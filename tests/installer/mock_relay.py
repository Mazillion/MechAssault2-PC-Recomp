"""A local stand-in for the report relay (kit/installer/report.py documents the protocol), for tests only: it listens on 127.0.0.1,
records every request (headers, form fields, the zip) and answers in the way the test sets. test_v116.py and ui_flow.py use it; a
standalone report helper can be tested against it the same way (point MA2_REPORT_RELAY at its url).

    relay = MockRelay(); relay.start()          # relay.url = 'http://127.0.0.1:<port>'
    relay.mode = 'ok' | '429' | '500' | 'okfalse' | 'badjson' | 'redirect' | 'slow' | 'close'
    ... relay.requests[-1]['fields']['kind'], relay.requests[-1]['zip'] ...
    relay.stop()

    python mock_relay.py [port]                 # run it by hand (Ctrl+C stops it), mode 'ok'
"""
import email.parser, email.policy, json, random, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'


def parse_multipart(ctype, body):
    """{name: str or bytes} from a multipart/form-data body (the zip part stays bytes)."""
    msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(b'Content-Type: ' + ctype.encode('latin-1') + b'\r\n\r\n' + body)
    out = {}
    for part in msg.iter_parts():
        name = part.get_param('name', header='content-disposition')
        data = part.get_payload(decode=True)
        out[name] = data if part.get_content_type() == 'application/zip' else data.decode('utf-8')
    return out


class MockRelay:
    def __init__(self, port=0):
        self.mode = 'ok'; self.message = ''; self.slow_seconds = 5.0; self.requests = []; self.ids = []
        relay = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a): pass

            def answer(self, code, obj=None, raw=None, headers=()):
                data = raw if raw is not None else json.dumps(obj).encode('utf-8')
                try:
                    self.send_response(code)
                    for k, v in headers: self.send_header(k, v)
                    self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(data)))
                    self.end_headers(); self.wfile.write(data)
                except OSError:
                    pass                              # the client gave up (a timeout test)

            def do_GET(self):
                self.answer(404, {'ok': False, 'message': 'not here'})

            def do_HEAD(self):
                self.send_response(200); self.end_headers()

            def do_POST(self):
                n = int(self.headers.get('Content-Length') or 0)
                body = self.rfile.read(n) if n else b''
                rec = {'path': self.path, 'headers': {k.lower(): v for k, v in self.headers.items()}, 'size': len(body), 'time': time.time()}
                try:
                    rec['fields'] = parse_multipart(self.headers.get('Content-Type', ''), body)
                except Exception as e:
                    rec['fields'] = {}; rec['parse_error'] = str(e)
                rec['zip'] = rec['fields'].get('zip') if isinstance(rec['fields'].get('zip'), bytes) else None
                relay.requests.append(rec)
                mode = relay.mode
                if mode == 'slow': time.sleep(relay.slow_seconds); mode = 'ok'
                if mode == 'close': self.close_connection = True; self.connection.close(); return
                if self.path != '/v1/report': return self.answer(404, {'ok': False, 'message': 'unknown path'})
                if mode == '429': return self.answer(429, {'ok': False, 'message': relay.message or 'Too many reports right now. Try again later.'})
                if mode == '500': return self.answer(500, raw=b'internal error')
                if mode == 'okfalse': return self.answer(200, {'ok': False, 'id': '', 'message': relay.message or 'Please update to the newest version.'})
                if mode == 'badjson': return self.answer(200, raw=b'<html>portal</html>')
                if mode == 'redirect': return self.answer(302, {'ok': False}, headers=[('Location', 'http://127.0.0.1:1/elsewhere')])
                if not (rec['zip'] or b'').startswith(b'PK\x03\x04'): return self.answer(400, {'ok': False, 'message': 'not a zip'})
                rid = 'MA2-' + ''.join(random.choice(B32) for _ in range(6)); relay.ids.append(rid)
                self.answer(200, {'ok': True, 'id': rid, 'message': relay.message})

        self.server = ThreadingHTTPServer(('127.0.0.1', port), H)
        self.server.daemon_threads = True
        self.url = 'http://127.0.0.1:%d' % self.server.server_address[1]
        self.thread = None

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True); self.thread.start()
        return self

    def stop(self):
        self.server.shutdown(); self.server.server_close()


if __name__ == '__main__':
    r = MockRelay(int(sys.argv[1]) if len(sys.argv) > 1 else 0).start()
    print('mock relay on %s (Ctrl+C stops it)' % r.url, flush=True)
    try:
        while True: time.sleep(1)
    except KeyboardInterrupt:
        r.stop()

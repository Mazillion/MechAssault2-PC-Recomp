"""A local stand-in for the two update channels (1.1.7 self-update tests): the relay's GET /v1/latest and GitHub's release download
(/<owner>/<repo>/releases/download/v<version>/<asset>, answered with a redirect to /assets/<n> as GitHub does). Loopback only, no
packages. The installer reaches it only with MA2_SELF_UPDATE_TEST=1 (MA2_UPDATE_RELAY / MA2_SELF_UPDATE_GITHUB = this server).

In-process:   srv = MockUpdate(payload=b'...', latest='1.1.8'); srv.start(); ...; srv.stop()
Stand-alone:  python mock_update.py --installer <exe> --latest 1.1.7-preview.2 [--port 0]   (prints "LISTENING <port>")

Modes (srv.mode): ok, mismatch (other bytes), evil (redirect to a host outside GitHub), plainhttp (redirect to http GitHub host),
loop (endless redirects), long (more bytes than announced, no Content-Length), short (fewer bytes, no Content-Length), lenbad
(a Content-Length other than the size), slow (one block per 0.2 s), 404, 500, nodirect (the file at once, no redirect).
srv.relay: ok, down (500), garbage, nohash, older, bigsize, wrongasset."""
import hashlib, json, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ASSET = 'MechAssault 2 Recomp Installer.exe'
REPO = 'Mazillion/MechAssault2-PC-Recomp'


class MockUpdate:
    def __init__(self, payload=b'x' * (2 * 1024 * 1024), latest='1.1.8', port=0):
        self.payload = payload; self.latest = latest; self.port = port
        self.mode = 'ok'; self.relay = 'ok'; self.requests = []; self.server = None

    @property
    def sha(self): return hashlib.sha256(self.payload).hexdigest()

    @property
    def base(self): return 'http://127.0.0.1:%d' % self.server.server_address[1]

    def latest_answer(self):
        r = self.relay
        if r == 'garbage': return 200, b'<html>portal</html>'
        if r == 'down': return 500, b'{}'
        body = {'ok': True, 'latest': '0.0.1' if r == 'older' else self.latest}
        if r != 'nohash':
            body.update(sha256=self.sha, size=len(self.payload), asset=ASSET)
        if r == 'bigsize': body['size'] = 500 * 1024 * 1024
        if r == 'wrongasset': body['asset'] = 'other.exe'
        return 200, json.dumps(body).encode()

    def start(self):
        outer = self

        class H(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *a): pass

            def send(self, code, body=b'', headers=()):
                self.send_response(code)
                for k, v in headers: self.send_header(k, v)
                if not any(k.lower() == 'content-length' for k, _ in headers) and body is not None:
                    self.send_header('Content-Length', str(len(body)))
                self.send_header('Connection', 'close')
                self.end_headers()
                if body: self.wfile.write(body)

            def do_GET(self):
                outer.requests.append({'path': self.path, 'headers': dict(self.headers.items())})
                if self.path == '/v1/latest':
                    code, body = outer.latest_answer()
                    return self.send(code, body, [('Content-Type', 'application/json')])
                want = '/%s/releases/download/v%s/%s' % (REPO, outer.latest, ASSET.replace(' ', '%20'))
                m = outer.mode
                if self.path == want:
                    if m == '404': return self.send(404, b'Not Found')
                    if m == '500': return self.send(500, b'error')
                    if m == 'evil': return self.send(302, b'', [('Location', 'https://evil.example.test/installer.exe')])
                    if m == 'plainhttp': return self.send(302, b'', [('Location', 'http://objects.githubusercontent.com/x')])
                    if m == 'userinfo': return self.send(302, b'', [('Location', 'http://user:pw@127.0.0.1:%d/assets/1' % self.server.server_address[1])])
                    if m == 'loop': return self.send(302, b'', [('Location', want)])
                    if m != 'nodirect': return self.send(302, b'', [('Location', '/assets/1?sig=abc')])
                    return self.file()
                if self.path.startswith('/assets/1'): return self.file()
                return self.send(404, b'Not Found')

            def file(self):
                m, data = outer.mode, outer.payload
                if m == 'mismatch': data = b'y' * len(data)
                if m in ('long', 'short'):
                    data = data + b'z' * 4096 if m == 'long' else data[:-4096]
                    self.send_response(200); self.send_header('Connection', 'close'); self.end_headers()
                    self.wfile.write(data); self.close_connection = True; return
                if m == 'lenbad': return self._lenbad(data)
                if m == 'slow':
                    self.send_response(200); self.send_header('Content-Length', str(len(data))); self.send_header('Connection', 'close')
                    self.end_headers()
                    try:
                        for i in range(0, len(data), 256 * 1024):
                            self.wfile.write(data[i:i + 256 * 1024]); self.wfile.flush(); time.sleep(0.2)
                    except OSError:
                        pass
                    return
                self.send(200, data, [('Content-Type', 'application/octet-stream'), ('Content-Length', str(len(data)))])

            def _lenbad(self, data):
                self.send_response(200); self.send_header('Content-Length', str(len(data) + 1)); self.send_header('Connection', 'close')
                self.end_headers(); self.wfile.write(data[:1024]); self.close_connection = True

        self.server = ThreadingHTTPServer(('127.0.0.1', self.port), H)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return self

    def stop(self):
        if self.server:
            self.server.shutdown(); self.server.server_close(); self.server = None


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--installer', required=True)
    ap.add_argument('--latest', required=True)
    ap.add_argument('--port', type=int, default=0)
    ap.add_argument('--log', default='')
    a = ap.parse_args()
    srv = MockUpdate(open(a.installer, 'rb').read(), a.latest, a.port).start()
    print('LISTENING %d' % srv.server.server_address[1], flush=True)
    try:
        while True:
            time.sleep(1)
            if a.log:
                with open(a.log, 'w', encoding='utf-8') as f: json.dump(srv.requests, f, indent=1)
    except KeyboardInterrupt:
        srv.stop()

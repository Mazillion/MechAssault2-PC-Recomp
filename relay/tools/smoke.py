"""Send test reports to a relay (staging first). Python 3.8+, standard library only.

    python smoke.py --relay https://ma2-report-relay-staging.<sub>.workers.dev send
    python smoke.py --relay ... send --size 20        a 20 MB report (checks the Worker's CPU/size limits)
    python smoke.py --relay ... ratelimit             sends until the relay answers 429 (expect 429 on report 6)
    python smoke.py --relay ... dailycap              same, for the daily cap (deploy staging with a small cap first)

Every test report has kind "manual", text "SMOKE TEST" and a zip with one small text file (plus random filler for
--size), so it is easy to recognise and close in the reports repo. Build "deploy-check": the relay refuses the builds
"test" and "smoke" with 403 (test traffic), so `send` with build smoke would no longer be accepted.
"""
import argparse
import io
import json
import os
import sys
import urllib.error
import urllib.request
import uuid
import zipfile


def make_zip(size_mb):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_STORED) as z:
        z.writestr("system.txt", "SMOKE TEST report from tools/smoke.py\n")
        if size_mb > 0:
            z.writestr("filler.bin", os.urandom(int(size_mb * 1048576) - 200))
    return b.getvalue()


def send(relay, size_mb=0, text="SMOKE TEST", version="1.1.6"):
    boundary = uuid.uuid4().hex
    parts = []
    for name, value in (("text", text), ("summary", "SMOKE TEST summary\nVersion: " + version)):
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="zip"; filename="report.zip"\r\n'
                 "Content-Type: application/zip\r\n\r\n".encode() + make_zip(size_mb) + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    req = urllib.request.Request(relay.rstrip("/") + "/v1/report", data=b"".join(parts), method="POST", headers={
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "User-Agent": "ma2-smoke",
        "X-MA2-Client": "helper", "X-MA2-Version": version, "X-MA2-Build": "deploy-check",   # the relay refuses build "smoke" / "test" (11-25)
        "X-MA2-Kind": "manual", "X-MA2-Signature": "smoke test",
    })
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        raw = e.read()
        try:
            return e.code, json.loads(raw)
        except ValueError:
            return e.code, {"raw": raw[:300].decode("utf-8", "replace")}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--relay", default=os.environ.get("MA2_RELAY_URL", ""), required=not os.environ.get("MA2_RELAY_URL"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("send"); p.add_argument("--size", type=float, default=0)
    sub.add_parser("ratelimit")
    sub.add_parser("dailycap")
    a = ap.parse_args()
    if a.cmd == "send":
        st, b = send(a.relay, a.size)
        print(st, json.dumps(b))
        return 0 if st == 200 and b.get("ok") else 1
    for n in range(1, 400):
        st, b = send(a.relay)
        print(n, st, json.dumps(b))
        if st == 429:
            print(f"limit reached after {n - 1} accepted reports")
            return 0
        if st != 200:
            return 1
    print("no limit reached in 400 reports")
    return 1


if __name__ == "__main__":
    sys.exit(main())

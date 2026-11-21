"""Cross-check: the installer's real sender (public-repo-staging kit/installer/report.py, read-only import) against this relay.

Run:  python test/test_client_compat.py [path-to-kit/installer]     (node on PATH; no network; runs hidden)
Skips (exit 0 with a note) when report.py is not found.
"""
import io
import json
import os
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# relay/ sits at the top of the public repo, beside kit/.
DEFAULT_KIT = os.path.normpath(os.path.join(ROOT, "..", "kit", "installer"))
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def start(port, **env):
    p = subprocess.Popen(["node", os.path.join(HERE, "local-relay.mjs"), str(port)], cwd=ROOT,
                         env=dict(os.environ, **env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=CREATE_NO_WINDOW)
    assert p.stdout.readline().decode().startswith("LISTENING")
    return p


def main():
    kit = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_KIT
    if not os.path.exists(os.path.join(kit, "report.py")):
        print("test_client_compat: SKIPPED (no report.py at %s)" % kit)
        return 0
    sys.path.insert(0, kit)
    import report  # the installer's sender, unchanged

    tmp = tempfile.mkdtemp()
    zp = os.path.join(tmp, "r.zip")
    with zipfile.ZipFile(zp, "w") as z:
        z.writestr("system.txt", "Version: 1.1.6\n")
    checks = 0
    procs = []
    try:
        procs.append(start(8801, PER_IP_PER_HOUR="2"))
        base = "http://127.0.0.1:8801"
        r = report.send(zp, text="It stopped.\nTwice @someone `x`", summary="Version: 1.1.6\nCPU: test", kind="install-fail",
                        sig=report.signature("build", "link LNK1102"), client="installer", version="1.1.6", build="kit93a6e80b",
                        url=base)
        assert r["ok"] and r["cause"] == "sent" and report.REPORT_ID.match(r["id"]), r; checks += 1
        with urllib.request.urlopen(base + "/__mock/issues", timeout=5) as h:
            issues = json.loads(h.read())
        assert issues[0]["title"] == "[%s] install-fail 1.1.6 build:link LNK1102" % r["id"], issues[0]["title"]; checks += 1
        body = issues[0]["body"]   # 1.1.7: the player's text stays with the report, not in the issue
        assert "someone" not in body and "**Player's text**: yes" in body, body; checks += 1
        r = report.send(zp, kind="manual", client="helper", version="", build="", url=base)   # empty version still accepted
        assert r["ok"], r; checks += 1
        r = report.send(zp, kind="manual", client="game", version="1.1.6", url=base)
        assert not r["ok"] and r["cause"] == "rate_limited" and r["http"] == 429 and "saved on your PC" in r["message"], r; checks += 1

        procs.append(start(8802, ACCEPT_REPORTS="off", OFF_MESSAGE="Please update to 1.1.7."))
        r = report.send(zp, kind="crash", client="game", version="1.1.6", url="http://127.0.0.1:8802")
        assert not r["ok"] and r["http"] == 503 and r["message"] == "Please update to 1.1.7.", r; checks += 1

        procs.append(start(8803, MAX_UPLOAD_BYTES="100"))
        r = report.send(zp, kind="crash", client="game", version="1.1.6", url="http://127.0.0.1:8803")
        assert not r["ok"] and r["cause"] == "too_large" and r["http"] == 413, r; checks += 1

        procs.append(start(8804, NOTICE="Thanks, fixed in 1.1.7."))
        r = report.send(zp, kind="hang", client="helper", version="1.1.6", url="http://127.0.0.1:8804")
        assert r["ok"] and r["message"] == "Thanks, fixed in 1.1.7.", r; checks += 1
        print("test_client_compat: %d checks passed (installer report.py vs relay)" % checks)
        return 0
    finally:
        for p in procs:
            p.kill(); p.wait()


if __name__ == "__main__":
    sys.exit(main())

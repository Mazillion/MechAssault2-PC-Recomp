"""triage.py announce (1.1.7): the release check through a MOCK GitHub API, the summary and the question, the post to the local
mock relay, --show and --undo, and what the game and the self-updating installer then read from /v1/latest. Never contacts GitHub,
the staging relay or the production relay.

Run from anywhere:  python test/test_announce.py      (needs node on PATH; runs hidden)
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import urllib.request
from contextlib import redirect_stdout
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage  # noqa: E402

PORT = 8798
BASE = f"http://127.0.0.1:{PORT}"
KEY = "local-owner-key"
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
ASSET = "MechAssault 2 Recomp Installer.exe"


class FakeGitHub:
    """GET /repos/<owner>/<repo>/releases/tags/v<version> answered from self.releases (metadata only)."""

    def __init__(self):
        self.releases, self.calls = {}, []
        outer = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_GET(self):
                outer.calls.append((self.path, dict(self.headers.items())))
                prefix = "/repos/Mazillion/MechAssault2-PC-Recomp/releases/tags/"
                rel = outer.releases.get(self.path[len(prefix):]) if self.path.startswith(prefix) else None
                body = json.dumps(rel if rel else {"message": "Not Found"}).encode()
                self.send_response(200 if rel else 404)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]

    def release(self, version, size, draft=False, pre=False, asset=ASSET, digest=None, state="uploaded"):
        a = {"name": asset, "size": size, "state": state}
        if digest:
            a["digest"] = digest
        self.releases["v" + version] = {"tag_name": "v" + version, "draft": draft, "prerelease": pre, "assets": [a]}


def latest():
    with urllib.request.urlopen(BASE + "/v1/latest", timeout=5) as r:
        return json.loads(r.read())


def run(argv, answers=()):
    """triage.main with input() answered from `answers`; returns (exit text or 0, printed text, the questions asked)."""
    asked = []
    it = iter(answers)
    import builtins
    old = builtins.input
    builtins.input = lambda prompt="": (asked.append(prompt), next(it))[1]
    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            try:
                res = triage.main(["--relay", BASE, "--key", KEY] + argv)
            except SystemExit as e:
                res = str(e)
    finally:
        builtins.input = old
    return res, buf.getvalue(), asked


def main():
    checks = 0
    gh = FakeGitHub()
    os.environ["MA2_GITHUB_API"] = gh.url
    tmp = tempfile.mkdtemp(prefix="ma2-announce-")
    exe = os.path.join(tmp, ASSET)
    data = b"MZ" + os.urandom(3 * 1024 * 1024)
    with open(exe, "wb") as f:
        f.write(data)
    sha, size = hashlib.sha256(data).hexdigest(), len(data)
    env = dict(os.environ, LATEST_VERSION="1.1.7", UPDATE_NOTICE="")
    proc = subprocess.Popen(["node", os.path.join(HERE, "local-relay.mjs"), str(PORT)], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
    try:
        assert proc.stdout.readline().decode().startswith("LISTENING")
        assert latest() == {"ok": True, "latest": "1.1.7"}, "env fallback before any announce"; checks += 1

        # GitHub check refusals: nothing is posted, nothing asked.
        cases = [("missing", None, "no published release v1.1.8"),
                 ("draft", dict(draft=True), "still a draft"),
                 ("prerelease", dict(pre=True), "pre-release"),
                 ("no asset", dict(asset="other.exe"), "no asset named"),
                 ("size", dict(size=size + 1), "bytes, this file is"),
                 ("digest", dict(digest="sha256:" + "0" * 64), "different SHA-256"),
                 ("uploading", dict(state="starter"), "not finished uploading")]
        for name, kw, words in cases:
            gh.releases.clear()
            if kw is not None:
                kw = dict(kw)
                gh.release("1.1.8", kw.pop("size", size), **kw)
            res, out, asked = run(["announce", "1.1.8", "--installer", exe])
            assert isinstance(res, str) and res.startswith("Not announced:") and words in res, (name, res)
            assert asked == [], name
            assert latest()["latest"] == "1.1.7", name
            checks += 1

        # Bad input.
        for argv, words in ((["announce", "1.1.8-preview.1", "--installer", exe], "X.Y.Z"),
                            (["announce", "1.1.8", "--installer", os.path.join(tmp, "nope.exe")], "--installer")):
            res, out, asked = run(argv)
            assert words in str(res), res; checks += 1

        # A good release: the summary, the question; "n" changes nothing.
        gh.releases.clear()
        gh.release("1.1.8", size, digest="sha256:" + sha)
        res, out, asked = run(["announce", "1.1.8", "--installer", exe], ["n"])
        assert res == "Not announced." and asked == [triage.ANNOUNCE_QUESTION], (res, asked); checks += 1
        for want in ("Version   1.1.8", f"Size      {size:,} bytes", f"SHA-256   {sha}",
                     "Release   https://github.com/Mazillion/MechAssault2-PC-Recomp/releases/tag/v1.1.8"):
            assert want in out, (want, out)
        checks += 1
        assert latest()["latest"] == "1.1.7"; checks += 1
        # The GitHub request carried no token.
        assert all("Authorization" not in h for _, h in gh.calls); checks += 1

        # "y" announces; the public answer carries the installer facts at once.
        res, out, asked = run(["announce", "1.1.8", "--installer", exe, "--notice", "Please update to 1.1.8."], ["y"])
        assert res == 0, (res, out); checks += 1
        live = latest()
        assert live == {"ok": True, "latest": "1.1.8", "notice": "Please update to 1.1.8.", "sha256": sha, "size": size,
                        "asset": ASSET}, live; checks += 1
        assert "Announced 1.1.8" in out; checks += 1

        # --show
        res, out, asked = run(["announce", "--show"])
        assert res == 0 and '"latest": "1.1.8"' in out and "(from the announcement)" in out, out; checks += 1

        # The game's view (the same rules as kit/source/game/src/pc_update_check.h: ok true, "latest" a plain release).
        raw = urllib.request.urlopen(urllib.request.Request(BASE + "/v1/latest", headers={"User-Agent": "MechAssault2-Recomp/1.1.7"}),
                                     timeout=5).read()
        assert len(raw) < 4096 and b'"ok":true' in raw and b'"latest":"1.1.8"' in raw; checks += 1

        # The self-updating installer's view (kit/installer/selfupdate.py against this relay, test mode).
        kit = os.path.join(os.path.dirname(ROOT), "kit", "installer")
        if os.path.isfile(os.path.join(kit, "selfupdate.py")):
            sys.path.insert(0, kit)
            import selfupdate
            os.environ.update(MA2_SELF_UPDATE_TEST="1", MA2_UPDATE_RELAY=BASE)
            r = selfupdate.check("1.1.7")
            assert r["ok"] and (r["latest"], r["sha256"], r["size"]) == ("1.1.8", sha, size), r; checks += 1
            assert selfupdate.check("1.1.8")["cause"] == "current"; checks += 1
        else:
            print("test_announce: kit/installer not beside relay/, the installer's view was not checked")

        # --force over a failing check: warns, asks, posts.
        gh.releases.clear()
        res, out, asked = run(["announce", "1.1.9", "--installer", exe, "--force"], ["y"])
        assert res == 0 and "WARNING (--force)" in out and latest()["latest"] == "1.1.9", (res, out); checks += 1
        # --yes: no question (the lead, after the owner's plain yes).
        gh.release("1.2.0", size)
        res, out, asked = run(["announce", "1.2.0", "--installer", exe, "--yes"])
        assert res == 0 and asked == [] and latest()["latest"] == "1.2.0"; checks += 1

        # --undo goes back one step (asks first; --yes skips it).
        res, out, asked = run(["announce", "--undo"], ["n"])
        assert res == "Nothing changed." and asked == [triage.UNDO_QUESTION] and latest()["latest"] == "1.2.0"; checks += 1
        res, out, asked = run(["announce", "--undo"], ["y"])
        assert res == 0 and latest()["latest"] == "1.1.9" and "Goes back to: 1.1.9" in out, out; checks += 1

        # A wrong key is refused with the usual hint, and changes nothing.
        gh.release("1.3.0", size)
        try:
            triage.main(["--relay", BASE, "--key", "wrong", "announce", "1.3.0", "--installer", exe, "--yes"])
            raise AssertionError("wrong key accepted")
        except SystemExit as e:
            assert str(e) == triage.KEY_REJECTED, e
        assert latest()["latest"] == "1.1.9"; checks += 1

        # The key is asked for only after the question (Credential Manager / clipboard come from resolve_key).
        order = []
        real = triage.resolve_key
        triage.resolve_key = lambda k: (order.append("key"), (KEY, "saved"))[1]
        try:
            import builtins
            old = builtins.input
            builtins.input = lambda p="": (order.append("question"), "y")[1]
            with redirect_stdout(io.StringIO()):
                triage.main(["--relay", BASE, "announce", "1.3.0", "--installer", exe])
            builtins.input = old
        finally:
            triage.resolve_key = real
        assert order == ["question", "key"], order; checks += 1
        print(f"test_announce: {checks} checks passed")
    finally:
        proc.kill()
        proc.wait()
        gh.server.shutdown()
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

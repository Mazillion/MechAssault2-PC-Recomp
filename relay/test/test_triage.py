"""End-to-end check of triage.py and of a Python multipart client against the local mock relay.

Run from anywhere:  python test/test_triage.py      (needs node on PATH; no network; runs hidden)
"""
import builtins
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import uuid
import zipfile
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import triage  # noqa: E402

PORT = 8799
BASE = f"http://127.0.0.1:{PORT}"
KEY = "local-owner-key"
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0


def make_zip():
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_STORED) as z:
        z.writestr("system.txt", "Version: 1.1.6\nCPU: test\n")
        z.writestr("logs/game-1.log", "boot\n[HEALTH] t=60 fps=144 underruns=0\n[CRASH] c0000005 at 0019236B\n")
    return b.getvalue()


def post_report(text, kind="crash"):
    boundary = uuid.uuid4().hex
    parts = []
    for name, value in (("text", text), ("summary", "Version: 1.1.6\nWindows build: 22631")):
        parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
    parts.append(f'--{boundary}\r\nContent-Disposition: form-data; name="zip"; filename="report.zip"\r\n'
                 f"Content-Type: application/zip\r\n\r\n".encode() + make_zip() + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    body = b"".join(parts)
    req = urllib.request.Request(BASE + "/v1/report", data=body, method="POST", headers={
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "X-MA2-Client": "installer", "X-MA2-Version": "1.1.6", "X-MA2-Build": "abc1234",
        "X-MA2-Kind": kind, "X-MA2-Signature": "build:link LNK1102",
    })
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


_U, _E = "Jane" + "Doe", "jane.doe" + "@" + "example.com"  # built at run time so the repo privacy scan stays clean
PRIVATE_TEXT = ("It crashed in C:\\Users\\" + _U + "\\Games\\MA2 after I mailed " + _E + " from 192.168.1.20, "
                "share \\\\HOMEPC\\games, mac 00:1A:2B:3C:4D:5E, serial 0123456789abcdef0123456789abcdef, "
                "see https://u:p" + "@" + "example.com/x?token=abc and @Mazillion ```js``` C:/Users/" + _U + "/x")


def check_draft_unit():
    """draft_issue / scrub on their own: every private item replaced, header present, nothing left."""
    info = {"id": "MA2-ABCDEF", "kind": "crash", "version": "1.1.6", "received": "2026-11-21T10:00:00Z",
            "signature": "c0000005 0019236B", "text": PRIVATE_TEXT,
            "summary": "Version: 1.1.6\nLog: C:\\Users\\" + _U + "\\AppData\\Local\\MechAssault2\\Logs\\game.log\n"
                       "Download: https://relay.example/v1/report/MA2-ABCDEF\nKey reports/2026-11/MA2-ABCDEF.zip"}
    md, n = triage.draft_issue(info, "MA2-ABCDEF")
    for bad in (_U, _E, "192.168.1.20", "HOMEPC", "00:1A:2B:3C:4D:5E",
                "0123456789abcdef0123456789abcdef", "u:p@", "token=abc", "@Mazillion", "```js", "/v1/report/MA2",
                "reports/2026-11"):
        assert bad not in md, (bad, md)
    assert "Needs owner review" in md and "NEEDS OWNER REVIEW" in md, md
    assert md.count("```") == 4, md
    assert "%USERPROFILE%" in md and "<e-mail>" in md and "<ip>" in md and "<mac>" in md and "<hex id>" in md, md
    assert n >= 10, n
    assert triage.leftovers(md) == [], triage.leftovers(md)
    assert "**Title:** crash in 1.1.6: c0000005 0019236B" in md, md
    return 1


def check_key_handling():
    """--key > env > Credential Manager > prompt; paste works; whitespace trimmed; save offered after a prompt only;
    production relay by default. Credential Manager, clipboard and input are replaced by fakes."""
    n = 0
    no_saved = lambda: None
    never = lambda *a: (_ for _ in ()).throw(AssertionError("asked"))
    # order and trimming
    assert triage.resolve_key(" k1 \r\n", env={"MA2_RELAY_KEY": "k2"}, read=lambda: "k3", ask=never) == ("k1", "argument"); n += 1
    assert triage.resolve_key("", env={"MA2_RELAY_KEY": "k2\n"}, read=lambda: "k3", ask=never) == ("k2", "environment"); n += 1
    assert triage.resolve_key("", env={}, read=lambda: " k3\r\n", ask=never) == ("k3", "saved"); n += 1
    prompts, clip_reads = [], []
    def asked(answer):
        return lambda p: prompts.append(p) or answer
    def clip(value):
        return lambda: clip_reads.append(1) or value
    buf = io.StringIO()
    with redirect_stdout(buf):
        # typed (with trailing space / CRLF): used as typed, the clipboard is not read
        assert triage.resolve_key("", env={}, read=no_saved, ask=asked(KEY + " \r\n"), clip=clip("other")) == (KEY, "prompt"); n += 1
        assert clip_reads == []; n += 1
        # Enter alone: the clipboard key (with a trailing CRLF) is used
        assert triage.resolve_key("", env={}, read=no_saved, ask=asked(""), clip=clip(KEY + "\r\n")) == (KEY, "prompt"); n += 1
        # Ctrl+V arriving as its control character \x16: nothing typed, so the clipboard is used
        assert triage.resolve_key("", env={}, read=no_saved, ask=asked("\x16"), clip=clip(KEY)) == (KEY, "prompt"); n += 1
        # a bracketed paste from Windows Terminal / PowerShell 7: the markers are removed
        assert triage.prompt_key(ask=asked("\x1b[200~" + KEY + "\x1b[201~\r\n"), clip=clip("x")) == KEY; n += 1
        assert triage.prompt_key(ask=asked("\x1b[200~\x16\x1b[201~"), clip=clip(" " + KEY + "\n")) == KEY; n += 1
    assert prompts and all(p == "Press Enter to use the owner key on your clipboard (or type it): " for p in prompts); n += 1
    assert KEY not in buf.getvalue(), "the key is never echoed"; n += 1
    assert triage.clean_key("\x1b[200~ab\tc\x00d\x7f \r\n") == "abcd"; n += 1
    # an empty key: refused locally with "No key entered" and no request is made
    made = []
    orig_init = triage.Relay.__init__
    triage.Relay.__init__ = lambda self, *a, **k: made.append(a)
    old_read, old_clip, old_input = triage.cred_read, triage.clipboard_text, builtins.input
    try:
        triage.cred_read, triage.clipboard_text, builtins.input = (lambda target=None: None), (lambda: "\r\n"), (lambda p="": "  \x16 ")
        with redirect_stdout(io.StringIO()):
            triage.main(["--relay", BASE, "--out", tempfile.gettempdir(), "pending"])
        raise AssertionError("empty key accepted")
    except SystemExit as e:
        assert str(e) == "No key entered" and made == [], (e, made); n += 1
    finally:
        triage.Relay.__init__ = orig_init
        triage.cred_read, triage.clipboard_text, builtins.input = old_read, old_clip, old_input
    # offer_save: yes saves, no does not
    saved = []
    with redirect_stdout(io.StringIO()):
        triage.offer_save(KEY, ask=lambda p: "", write=lambda k: saved.append(k) or True)
        triage.offer_save(KEY, ask=lambda p: "n", write=lambda k: saved.append(k) or True)
    assert saved == [KEY]; n += 1
    # a pasted key with a trailing space / CRLF works against the relay (the relay trims too)
    for k in (KEY + " ", KEY + "\r\n"):
        assert triage.Relay(BASE, k).call("GET", "/v1/admin/pending")["ok"] is True; n += 1
    # the save offer runs after the first accepted answer, once; not after a rejected key
    calls = []
    r = triage.Relay(BASE, KEY, on_success=lambda: calls.append(1))
    r.call("GET", "/v1/admin/pending"); r.call("GET", "/v1/admin/pending")
    assert calls == [1]; n += 1
    calls = []
    try:
        triage.Relay(BASE, "wrong", on_success=lambda: calls.append(1)).call("GET", "/v1/admin/pending")
    except SystemExit:
        pass
    assert calls == []; n += 1
    # main(): a prompted key is offered for saving; production relay by default (checked without a network call)
    old = (triage.cred_read, triage.cred_write, triage.clipboard_text, builtins.input)
    try:
        triage.cred_read = lambda target=None: None
        stored = []
        triage.cred_write = lambda k, target=None: stored.append(k) or True
        answers = iter([KEY + "\r\n", "y"])
        builtins.input = lambda p="": next(answers)
        with redirect_stdout(io.StringIO()), tempfile.TemporaryDirectory() as out:
            triage.main(["--relay", BASE, "--out", out, "pending"])
        assert stored == [KEY]; n += 1
        env_url = os.environ.pop("MA2_RELAY_URL", None)
        seen = []
        orig_init = triage.Relay.__init__
        def spy(self, url, key, timeout=60, on_success=None):
            seen.append(url); raise SystemExit("stop")
        triage.Relay.__init__ = spy
        try:
            triage.main(["--key", "x", "--out", tempfile.gettempdir(), "pending"])
        except SystemExit:
            pass
        finally:
            triage.Relay.__init__ = orig_init
            if env_url is not None:
                os.environ["MA2_RELAY_URL"] = env_url
        assert seen == [triage.DEFAULT_RELAY] and triage.DEFAULT_RELAY.startswith("https://"); n += 1
    finally:
        triage.cred_read, triage.cred_write, triage.clipboard_text, builtins.input = old
    return n


def check_reports_helper(rid):
    """tools/ma2_reports.pyw with a fake clipboard, fake explorer/startfile/message box and a temporary home folder."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("ma2_reports", os.path.join(ROOT, "tools", "ma2_reports.pyw"))
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    n = 0
    opened, popen, msgs, browsed = [], [], [], []
    saved = (helper.triage.clipboard_text, helper.subprocess.Popen, getattr(helper.os, "startfile", None), helper.message,
             helper.webbrowser.open, os.environ.get("USERPROFILE"), os.environ.get("HOME"), os.environ.get("MA2_RELAY_URL"), os.environ.get("MA2_RELAY_KEY"),
             helper.triage.cred_read)
    with tempfile.TemporaryDirectory() as home:
        try:
            helper.subprocess.Popen = lambda *a, **k: popen.append(a[0])
            helper.os.startfile = lambda p: opened.append(p)
            helper.message = lambda text, error=False: msgs.append(text)
            helper.webbrowser.open = lambda u: browsed.append(u)
            helper.triage.cred_read = lambda target=None: None
            os.environ["USERPROFILE"] = home
            os.environ["HOME"] = home          # expanduser on Linux (CI)
            os.environ["MA2_RELAY_URL"] = BASE
            os.environ["MA2_RELAY_KEY"] = KEY + "\r\n"
            helper.triage.clipboard_text = lambda: f"see {rid.lower()} please"
            assert helper.main() == 0, msgs; n += 1
            out = os.path.join(home, "MA2-Reports")
            assert os.path.exists(os.path.join(out, rid + ".zip")); n += 1
            assert opened == [os.path.join(out, rid + "-summary.txt")] and "system.txt" in open(opened[0], encoding="utf-8").read(); n += 1
            assert popen and popen[0][0] == "explorer"; n += 1
            helper.triage.clipboard_text = lambda: "nothing here"
            assert helper.main() == 1 and "Copy a report number" in msgs[-1]; n += 1
            del os.environ["MA2_RELAY_KEY"]
            helper.triage.clipboard_text = lambda: rid
            assert helper.main() == 0 and browsed == [f"{BASE}/v1/open/{rid}"]; n += 1   # no saved key: the browser link
        finally:
            (helper.triage.clipboard_text, helper.subprocess.Popen, st, helper.message, helper.webbrowser.open, up, hm, ru, rk,
             helper.triage.cred_read) = saved
            if st is not None:
                helper.os.startfile = st
            for k, v in (("USERPROFILE", up), ("HOME", hm), ("MA2_RELAY_URL", ru), ("MA2_RELAY_KEY", rk)):
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
    return n


def run_triage(*args, out):
    buf = io.StringIO()
    with redirect_stdout(buf):
        triage.main(["--relay", BASE, "--key", KEY, "--out", out, *args])
    return buf.getvalue()


def main():
    env = dict(os.environ, PER_IP_PER_HOUR="3")
    proc = subprocess.Popen(["node", os.path.join(HERE, "local-relay.mjs"), str(PORT)], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
    try:
        line = proc.stdout.readline().decode()
        assert line.startswith("LISTENING"), line
        checks = 0

        st, b = post_report("It stopped at the link step.")
        assert st == 200 and b["ok"] and triage.ID_RE.match(b["id"]), (st, b); checks += 1
        rid = b["id"]
        st2, b2 = post_report("second", kind="manual")
        assert st2 == 200; checks += 1

        with tempfile.TemporaryDirectory() as out:
            listing = run_triage("list", out=out)
            assert rid in listing and "2 new" in listing, listing; checks += 1
            shown = run_triage("show", rid, out=out)
            assert "It stopped at the link step." in shown, shown
            assert "[CRASH] c0000005" in shown and "system.txt" in shown, shown; checks += 1
            assert os.path.exists(os.path.join(out, rid + ".zip")); checks += 1
            assert "1 new" in run_triage("list", out=out); checks += 1
            newout = run_triage("new", out=out)
            assert b2["id"] in newout; checks += 1
            assert "no new reports" in run_triage("new", out=out); checks += 1

            run_triage("switch", "off", "--message", "Paused.", out=out)
            st, b = post_report("while off")
            assert st == 503 and b["ok"] is False and b["message"] == "Paused.", (st, b); checks += 1
            run_triage("switch", "on", out=out)

            st, b = post_report("third")
            assert st == 200; checks += 1
            st, b = post_report("fourth: over the 3/hour test limit")
            assert st == 429 and b["ok"] is False, (st, b); checks += 1
            assert "0 pending" in run_triage("pending", out=out); checks += 1

        with urllib.request.urlopen(BASE + "/__mock/issues", timeout=5) as r:
            issues = json.loads(r.read())
        assert len(issues) == 3 and issues[0]["title"].startswith(f"[{rid}] crash 1.1.6"), issues; checks += 1

        # draft-issue: from the relay (no local copy), then from the local copy without the relay or key.
        checks += check_draft_unit()
        st, b = post_report(PRIVATE_TEXT, kind="manual")
        assert st == 429, (st, b)       # over the 3/hour limit: use the first report, its text is plain
        with tempfile.TemporaryDirectory() as out:
            said = run_triage("draft-issue", rid, out=out)
            path = os.path.join(out, rid + "-public-draft.md")
            assert "NEEDS OWNER REVIEW" in said and os.path.exists(path), said; checks += 1
            md = open(path, encoding="utf-8").read()
            assert "It stopped at the link step." in md and "Needs owner review" in md, md; checks += 1
            os.remove(path)
            with open(os.path.join(out, rid + ".json"), "w", encoding="utf-8") as f:
                json.dump({"id": rid, "kind": "crash", "version": "1.1.6", "received": "2026-11-21T10:00:00Z",
                           "text": PRIVATE_TEXT, "summary": ""}, f)
            buf = io.StringIO()
            with redirect_stdout(buf):
                triage.main(["--relay", "", "--key", "", "--out", out, "draft-issue", rid])
            md = open(path, encoding="utf-8").read()
            assert _U not in md and "<e-mail>" in md, md; checks += 1

        # delete: a wrong confirmation deletes nothing; --yes deletes on the relay, the issue and the local copies.
        with tempfile.TemporaryDirectory() as out:
            run_triage("get", rid, out=out)
            relay = triage.Relay(BASE, KEY)
            try:
                triage.delete_report(relay, rid, out, ask=lambda prompt: "MA2-ZZZZZZ")
                raise AssertionError("deleted without the right confirmation")
            except SystemExit as e:
                assert "not deleted" in str(e); checks += 1
            said = run_triage("delete", rid, "--yes", out=out)
            res = json.loads(said[:said.rindex("}") + 1])
            assert res["zip"] and res["info"] and res["issue"] == "emptied", res; checks += 1
            assert res["local_removed"] == [rid + ".zip", rid + ".json"], res; checks += 1
            assert "edit history" in said; checks += 1
            try:
                triage.main(["--relay", BASE, "--key", KEY, "--out", out, "get", rid])
                raise AssertionError("download still works after delete")
            except SystemExit as e:
                assert "not found" in str(e), e; checks += 1
        with urllib.request.urlopen(BASE + "/__mock/issues", timeout=5) as r:
            gone = [i for i in json.loads(r.read()) if i["title"] == f"[{rid}] removed"]
        assert len(gone) == 1 and gone[0]["state"] == "closed" and gone[0].get("locked"), gone; checks += 1
        assert '"objects"' in run_triage("expire", out=tempfile.gettempdir()); checks += 1

        # /v1/latest: no LATEST_VERSION set on this mock -> latest null, cacheable; POST refused.
        with urllib.request.urlopen(BASE + "/v1/latest", timeout=5) as r:
            assert r.status == 200 and r.headers.get("Cache-Control") == "public, max-age=3600"
            assert json.loads(r.read()) == {"ok": True, "latest": None}; checks += 1

        # Wrong key is refused, with the exact hint.
        try:
            triage.main(["--relay", BASE, "--key", "wrong", "--out", tempfile.gettempdir(), "list"])
            raise AssertionError("wrong key accepted")
        except SystemExit as e:
            assert str(e) == triage.KEY_REJECTED, e; checks += 1
        assert triage.KEY_REJECTED == ("The relay rejected the key. Check it matches OWNER_KEY in Cloudflare "
                                       "(Workers > ma2-report-relay > Settings > Variables and Secrets)."); checks += 1

        # 11-22 key handling (Credential Manager, clipboard and input are fakes: nothing on this PC is read or written).
        checks += check_key_handling()
        checks += check_reports_helper(b2["id"])

        print(f"test_triage: {checks} checks passed")
    finally:
        proc.kill()
        proc.wait()

    # /v1/latest with the vars set, as the game reads it.
    env = dict(os.environ, LATEST_VERSION="1.1.7", UPDATE_NOTICE="Please update to 1.1.7.")
    proc = subprocess.Popen(["node", os.path.join(HERE, "local-relay.mjs"), str(PORT)], cwd=ROOT, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=CREATE_NO_WINDOW)
    try:
        assert proc.stdout.readline().decode().startswith("LISTENING")
        req = urllib.request.Request(BASE + "/v1/latest", headers={"User-Agent": "MechAssault2-Recomp/1.1.6"})
        with urllib.request.urlopen(req, timeout=5) as r:
            assert json.loads(r.read()) == {"ok": True, "latest": "1.1.7", "notice": "Please update to 1.1.7."}
        print("test_triage: /v1/latest with LATEST_VERSION ok")
    finally:
        proc.kill()
        proc.wait()
    return 0


if __name__ == "__main__":
    sys.exit(main())

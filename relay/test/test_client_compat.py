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
        if hasattr(report, "check_replies"):
            procs.append(start(8805))
            print("test_client_compat: conversations %d checks passed" % conversation_checks(report, zp, "http://127.0.0.1:8805", tmp))
        return 0
    finally:
        for p in procs:
            p.kill(); p.wait()


def owner_reply(base, rid, text, edit=None):
    body = {"text": text} if edit is None else {"text": text, "edit": edit}
    req = urllib.request.Request(base + "/v1/report/%s/reply" % rid, data=json.dumps(body).encode(), method="POST",
                                 headers={"X-MA2-Owner-Key": "local-owner-key", "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def conversation_checks(report, zp, base, tmp):
    """1.2.0: the client store and the reply / seen / answer calls of the installer's report.py against the relay."""
    import datetime
    n = 0
    store = os.path.join(tmp, "report-conversations.txt")
    # the store: format, 60 days after the last message, nothing asked without a live entry
    day = datetime.date(2026, 11, 22)
    e = [{"id": "MA2-AAAAAA", "sent": day, "last": day, "secret": "1" * 64},
         {"id": "MA2-BBBBBB", "sent": day - datetime.timedelta(days=90), "last": day - datetime.timedelta(days=60), "secret": "2" * 64}]
    text = report.format_store(e)
    assert "MA2-AAAAAA 2026-11-22 2026-11-22 " + "1" * 64 in text and text.startswith("#"); n += 1
    assert report.parse_store(text + "garbage line\nMA2-CCCCCC 2026-13-01 2026-13-01 " + "3" * 64 + "\n") == e; n += 1
    assert [x["id"] for x in report.live(e, day)] == ["MA2-AAAAAA"]; n += 1
    report.store_write(e, store, now=day)
    assert [x["id"] for x in report.store_read(store)] == ["MA2-AAAAAA"], "an entry 60 days after its last message is dropped"; n += 1
    report.store_write([], store, now=day)
    assert not os.path.exists(store), "no live entry: the file goes"; n += 1
    assert report.check_replies(url="http://127.0.0.1:9", path=store) == [], "no entry: no request"; n += 1
    # a report sent with a key; only its hash reaches the relay
    secret = report.new_secret()
    assert report.SECRET_RE.match(secret); n += 1
    r = report.send(zp, kind="manual", client="installer", version="1.2.0", url=base, reply_secret=secret)
    assert r["ok"], r
    rid = r["id"]
    report.store_add(rid, secret, path=store)
    with open(store, encoding="utf-8") as f:
        assert rid in f.read(); n += 1
    assert report.check_replies(url=base, path=store) == [], "no reply yet"; n += 1
    owner_reply(base, rid, "Thanks for the report.\nDoes it happen with HD textures off?")
    got = report.check_replies(url=base, path=store)
    assert len(got) == 1 and got[0]["n"] == 1 and got[0]["can_answer"] and got[0]["text"].startswith("Thanks"), got; n += 1
    assert report.short_date(got[0]["sent"]) == "%s %d" % (report.MONTHS[got[0]["sent"].month - 1], got[0]["sent"].day); n += 1
    owner_reply(base, rid, "Thanks for the report. Does it happen with HD textures off?", edit=1)
    assert report.check_replies(url=base, path=store)[0]["text"] == "Thanks for the report. Does it happen with HD textures off?"; n += 1
    assert report.mark_seen(rid, 1, url=base, path=store); n += 1
    assert report.check_replies(url=base, path=store) == [], "shown once"; n += 1
    # answers: empty refused locally; a scrubbed answer goes; a second one is refused (strict alternation)
    assert report.send_answer(rid, "   ", url=base, path=store)["cause"] == "empty"; n += 1
    user = "Jane" + "Doe"
    a = report.send_answer(rid, "Still happens. My files are in C:\\Users\\%s\\Games" % user, url=base, path=store, version="1.2.0")
    assert a["ok"] and a["cause"] == "sent", a; n += 1
    with urllib.request.urlopen(base + "/__mock/comments", timeout=5) as h:
        comments = json.loads(h.read())
    assert len(comments) == 1 and "Still happens." in comments[0]["body"] and user not in comments[0]["body"], comments; n += 1
    a2 = report.send_answer(rid, "again", url=base, path=store)
    assert not a2["ok"] and a2["cause"] == "closed" and a2["http"] == 409, a2; n += 1
    # a report from another PC (no store entry) cannot be answered from here
    assert report.send_answer("MA2-ZZZZZZ", "x", url=base, path=store)["cause"] == "closed"; n += 1
    # no answer from the relay (offline, or a relay without the route): nothing shown, nothing breaks
    owner_reply(base, rid, "Follow-up")
    assert report.check_replies(url="http://127.0.0.1:9", path=store, timeout=3) == []; n += 1
    return n


if __name__ == "__main__":
    sys.exit(main())

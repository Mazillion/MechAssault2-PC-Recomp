"""MechAssault 2 report triage (owner side). Python 3.8+, standard library only.

Setup once (PowerShell):
    $env:MA2_RELAY_URL = "https://ma2-report-relay.<your-subdomain>.workers.dev"
    $env:MA2_RELAY_KEY = "<the OWNER_KEY you set on the Worker>"
(or pass --relay / --key; if the key is missing you are asked for it, and it is not echoed).

Commands:
    python triage.py new                 list reports not seen yet, download and summarise each
    python triage.py list [--days 7]     list reports of the last N days (n = new, P = issue still pending)
    python triage.py get MA2-XXXXXX      download one zip
    python triage.py show MA2-XXXXXX     download (if needed) and summarise one report
    python triage.py pending             reports whose GitHub issue has not been opened yet
    python triage.py retry               ask the relay to open the pending issues now
    python triage.py switch              show the off switch
    python triage.py switch off --message "Reports are paused, please update to 1.1.7."
    python triage.py switch on
    python triage.py delete MA2-XXXXXX   delete one report for good (a player asked): the zip, its text and summary on
                                         the relay, its GitHub issue (deleted, or emptied and locked), and the
                                         copies in the reports folder. Asks you to type the number again (or --yes)
    python triage.py expire              run the relay's 60-day clean-up now (it also runs every hour)
    python triage.py draft-issue MA2-XXXXXX   write a PUBLIC issue draft (Markdown, never posted) from the report's
                                         text and summary, with private details removed; read it before using it

Reports are saved to %USERPROFILE%\\MA2-Reports (change with --out). Zips are never extracted to disk: they are read
in memory and only text members are printed, so nothing inside a report is ever run.
"""

import argparse
import getpass
import io
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ID_RE = re.compile(r"^MA2-[A-Z2-7]{6}$")
INTERESTING = re.compile(r"\[(CRASH|HANG|HEALTH|XA2|STOP|ERROR|FATAL)\]|Traceback|Exception|error C\d+|out of memory", re.I)
TEXT_EXT = (".txt", ".log", ".ini", ".json", ".csv", ".md")


class Relay:
    def __init__(self, url, key, timeout=60):
        u = urllib.parse.urlparse(url)
        local = u.hostname in ("localhost", "127.0.0.1", "::1")
        if u.scheme != "https" and not (u.scheme == "http" and local):
            raise SystemExit("relay URL must be https:// (http:// only for localhost testing)")
        self.base = url.rstrip("/")
        self.key = key
        self.timeout = timeout

    def call(self, method, path, body=None, raw=False):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("X-MA2-Owner-Key", self.key)
        req.add_header("User-Agent", "ma2-triage")
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                payload = r.read()
        except urllib.error.HTTPError as e:
            msg = e.read()[:300].decode("utf-8", "replace")
            if e.code == 401:
                raise SystemExit("relay said 401: the owner key is wrong")
            if e.code == 404 and raw:
                return None
            raise SystemExit(f"relay error {e.code}: {msg}")
        except urllib.error.URLError as e:
            raise SystemExit(f"cannot reach the relay: {e.reason}")
        return payload if raw else json.loads(payload)


def seen_path(out):
    return os.path.join(out, "seen.json")


def load_seen(out):
    try:
        with open(seen_path(out), encoding="utf-8") as f:
            return set(json.load(f))
    except (OSError, ValueError):
        return set()


def save_seen(out, seen):
    os.makedirs(out, exist_ok=True)
    tmp = seen_path(out) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(sorted(seen), f)
    os.replace(tmp, seen_path(out))


def check_id(rid):
    rid = rid.strip().upper()
    if not ID_RE.match(rid):
        raise SystemExit(f"not a report number: {rid!r} (expected MA2- and 6 letters/digits)")
    return rid


def fmt_row(r, seen):
    flag = ("n" if r["id"] not in seen else " ") + ("P" if r.get("issue") == "pending" else " ")
    return (f"{flag} {r['id']}  {r.get('received', '')[:16].replace('T', ' ')}  {r.get('kind', ''):12} "
            f"{r.get('client', ''):9} {r.get('version', ''):8} {r['bytes'] / 1048576:6.1f} MB  {r.get('signature', '')}")


def download(relay, rid, out):
    path = os.path.join(out, rid + ".zip")
    if os.path.exists(path):
        return path
    data = relay.call("GET", f"/v1/report/{rid}", raw=True)
    if data is None:
        raise SystemExit(f"{rid}: not found (reports are kept 60 days)")
    os.makedirs(out, exist_ok=True)
    with open(path + ".part", "wb") as f:
        f.write(data)
    os.replace(path + ".part", path)
    info = relay.call("GET", f"/v1/report/{rid}/info", raw=True)
    if info is not None:
        with open(os.path.join(out, rid + ".json"), "wb") as f:
            f.write(info)
    return path


def summarise_zip(data, max_lines=12):
    """Return printable lines describing a report zip (bytes). Never writes members to disk."""
    lines = []
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile:
        return ["  (not a valid zip)"]
    total = 0
    for i in z.infolist():
        total += i.file_size
        lines.append(f"  {i.file_size:>12,}  {i.filename}")
    lines.append(f"  {total:>12,}  total ({len(z.infolist())} files)")
    for i in z.infolist():
        name = i.filename.lower()
        if not name.endswith(TEXT_EXT) or i.file_size > 64 * 1024 * 1024:
            continue
        text = z.read(i).decode("utf-8", "replace")
        if os.path.basename(name) == "system.txt":
            lines.append("  --- system.txt")
            lines += ["    " + l for l in text.splitlines()[:40]]
            continue
        hits = [l.strip() for l in text.splitlines() if INTERESTING.search(l)]
        if hits:
            lines.append(f"  --- {i.filename}: {len(hits)} notable lines, last {min(len(hits), max_lines)}")
            lines += ["    " + h[:220] for h in hits[-max_lines:]]
    return lines


def show(relay, rid, out):
    path = download(relay, rid, out)
    print(f"== {rid}  ({path})")
    info_path = os.path.join(out, rid + ".json")
    if os.path.exists(info_path):
        with open(info_path, encoding="utf-8") as f:
            info = json.load(f)
        print(f"  {info.get('kind')} | {info.get('client')} {info.get('version')} build {info.get('build')} | "
              f"{info.get('received')} | {info.get('signature')}")
        print("  --- player's text")
        for l in (info.get("text") or "(none)").splitlines():
            print("    " + l)
        print("  --- summary")
        for l in (info.get("summary") or "(none)").splitlines():
            print("    " + l)
    with open(path, "rb") as f:
        for l in summarise_zip(f.read()):
            print(l)
    print()


# ---------------------------------------------------------------- public issue draft (1.1.7)
#
# The same patterns as the public repo's tools/privacy_scan.py (profile paths, this PC's user and machine name, e-mail
# addresses other than GitHub noreply ones) plus what a player's text or a system summary can carry that has no place
# in a public issue: IP and MAC addresses, network share host names, URL credentials and query strings, long hex ids
# (serials, keys), report-store keys and owner download links. The draft is a starting point for a public issue the
# owner writes; this tool never posts anything.

PROFILE_RES = [
    (re.compile(r"(?i)\b[A-Z]:\\Users\\(?!Public\b)[^\\\s\"'<>]+"), "%USERPROFILE%"),
    (re.compile(r"(?i)\b[A-Z]:/Users/(?!Public\b)[^/\s\"'<>]+"), "%USERPROFILE%"),
    (re.compile(r"/home/[a-z_][a-z0-9_-]*"), "/home/<user>"),
]
PRIVATE_RES = [
    (re.compile(r"\\\\(?![?.]\\)[^\\/\s\"'<>|]+(?=\\)"), r"\\\\<host>"),                       # \\server\share
    (re.compile(r"(?i)\b(https?://[^\s?#\"'<>]+)\?[^\s\"'<>]*"), r"\1?<query removed>"),        # URL queries
    (re.compile(r"(?i)\b((?:https?|ftp)://)[^/\s:@\"'<>]+:[^/\s@\"'<>]+@"), r"\1<hidden>@"),    # user:password@
    (re.compile(r"(?i)https?://[^\s\"'<>]*/v1/report/MA2-[A-Z2-7]{6}\S*"), "<owner link>"),
    (re.compile(r"(?i)reports/\d{4}-\d{2}/MA2-[A-Z2-7]{6}\.(?:zip|json)"), "<report store key>"),
    (re.compile(r"[A-Za-z0-9._%+-]+@(?!users\.noreply\.github\.com\b)[A-Za-z0-9-]+\.[A-Za-z0-9.-]*[A-Za-z]{2,}"), "<e-mail>"),
    (re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])"), "<ip>"),
    (re.compile(r"(?i)(?<![0-9a-f:])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f:])"), "<mac>"),
    (re.compile(r"(?i)(?<![0-9a-f:])(?:[0-9a-f]{1,4}:){4,7}[0-9a-f]{1,4}(?![0-9a-f:])"), "<ip>"),   # IPv6
    (re.compile(r"(?i)(?<![0-9a-z])[0-9a-f]{24,}(?![0-9a-z])"), "<hex id>"),
]


def private_names():
    """This PC's user and machine name (privacy_scan does the same): a report read on the owner's PC must not carry them."""
    out = []
    for var in ("USERNAME", "COMPUTERNAME"):
        v = os.environ.get(var, "")
        if len(v) > 2:
            out.append(re.compile(r"(?i)\b" + re.escape(v) + r"\b"))
    return out


def scrub(text):
    """(clean text, number of replacements). Profile paths first, so a user name inside a path goes with the path."""
    n = 0
    for rx, repl in PROFILE_RES + PRIVATE_RES:
        text, k = rx.subn(repl, text)
        n += k
    for rx in private_names():
        text, k = rx.subn("<name>", text)
        n += k
    return text, n


def leftovers(text):
    """Private-looking text still present after scrub (should be none)."""
    hits = []
    for rx, _ in PROFILE_RES + PRIVATE_RES:
        hits += [m.group(0) for m in rx.finditer(text) if "<" not in m.group(0) and m.group(0) != "%USERPROFILE%"]
    for rx in private_names():
        hits += [m.group(0) for m in rx.finditer(text)]
    return hits


def fence(text):
    """Text for a fenced block in a public issue: no closing fence, no @mention, no control or bidi characters."""
    text = str(text or "").replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub("[\u0000-\u0008\u000B-\u001F\u007F\u202A-\u202E\u2066-\u2069]", "", text)
    text = text.replace("`", "\u02cb").replace("@", "\uff20")
    return text.strip() or "(none)"


def draft_issue(info, rid):
    """Markdown for a public issue from a report's stored info (the /info JSON). Returns (markdown, replacements)."""
    text, n1 = scrub(str(info.get("text") or ""))
    summary, n2 = scrub(str(info.get("summary") or ""))
    sig, n3 = scrub(str(info.get("signature") or ""))
    kind = re.sub(r"[^a-z-]", "", str(info.get("kind") or "")) or "report"
    version = re.sub(r"[^0-9A-Za-z.+-]", "", str(info.get("version") or "")) or "unknown"
    day = str(info.get("received") or "")[:10]
    if not re.match(r"^\d{4}-\d{2}-\d{2}$", day):
        day = "unknown"
    sig = " ".join(fence(sig).split()) if sig.strip() else ""
    title = f"{kind} in {version}" + (f": {sig}" if sig else "")
    n = n1 + n2 + n3
    lines = [
        "<!-- NEEDS OWNER REVIEW: drafted by triage.py draft-issue from a private report; nothing was posted.",
        f"     {n} private details were removed by pattern. Read every line before posting, then delete this comment. -->",
        "",
        "> **Needs owner review.** Draft of a public issue made from a private report. Check that it holds no names,",
        "> paths, addresses or anything else the player would not want public before posting it.",
        "",
        f"**Title:** {title[:120]}",
        "",
        f"Report `{rid}` | {kind} | version {version} | received {day}",
        "",
        "**What the player wrote**",
        "",
        "```text",
        fence(text),
        "```",
        "",
        "**System summary**",
        "",
        "```text",
        fence(summary),
        "```",
        "",
    ]
    return "\n".join(lines), n


def write_draft(relay, rid, out):
    """Reads the report's info (the local copy from get/show, else from the relay); writes <out>/<id>-public-draft.md."""
    info_path = os.path.join(out, rid + ".json")
    info = None
    if os.path.exists(info_path):
        with open(info_path, encoding="utf-8") as f:
            info = json.load(f)
    elif relay is not None:
        raw = relay.call("GET", f"/v1/report/{rid}/info", raw=True)
        if raw is None:
            raise SystemExit(f"{rid}: not found (reports are kept 60 days)")
        info = json.loads(raw)
    if info is None:
        raise SystemExit(f"{rid}: no local copy and no relay to ask")
    md, n = draft_issue(info, rid)
    left = leftovers(md)
    if left:
        raise SystemExit(f"{rid}: the draft still holds private-looking text, not written: {sorted(set(left))[:5]}")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, rid + "-public-draft.md")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(md)
    return path, n


def delete_report(relay, rid, out, yes=False, ask=input):
    """DELETE /v1/report/<id> on the relay, then the local copies. Returns the relay's answer."""
    if not yes:
        again = ask(f"This deletes {rid} for good (relay, GitHub issue, local copies). Type the number again to go on: ")
        if again.strip().upper() != rid:
            raise SystemExit("not deleted: the number did not match")
    res = relay.call("DELETE", f"/v1/report/{rid}")
    removed = []
    for name in (rid + ".zip", rid + ".json", rid + "-public-draft.md"):
        path = os.path.join(out, name)
        if os.path.exists(path):
            os.remove(path)
            removed.append(name)
    res["local_removed"] = removed
    return res


def main(argv=None):
    ap = argparse.ArgumentParser(description="MechAssault 2 report triage")
    ap.add_argument("--relay", default=os.environ.get("MA2_RELAY_URL", ""))
    ap.add_argument("--key", default=os.environ.get("MA2_RELAY_KEY", ""))
    ap.add_argument("--out", default=os.path.join(os.path.expanduser("~"), "MA2-Reports"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("--days", type=int, default=7)
    p = sub.add_parser("new"); p.add_argument("--days", type=int, default=14)
    sub.add_parser("get").add_argument("id")
    sub.add_parser("show").add_argument("id")
    sub.add_parser("pending")
    sub.add_parser("retry")
    sub.add_parser("draft-issue").add_argument("id")
    p = sub.add_parser("delete"); p.add_argument("id"); p.add_argument("--yes", action="store_true")
    sub.add_parser("expire")
    p = sub.add_parser("switch"); p.add_argument("state", nargs="?", choices=["on", "off"])
    p.add_argument("--message", default="")
    a = ap.parse_args(argv)

    if a.cmd == "draft-issue" and os.path.exists(os.path.join(a.out, check_id(a.id) + ".json")):
        # A report already fetched with get/show needs neither the relay nor the key.
        path, n = write_draft(None, check_id(a.id), a.out)
        print(f"{path}  ({n} private details removed; NEEDS OWNER REVIEW, nothing was posted)")
        return 0
    if not a.relay:
        raise SystemExit("set MA2_RELAY_URL or pass --relay")
    key = a.key or getpass.getpass("Owner key: ")
    relay = Relay(a.relay, key)
    seen = load_seen(a.out)

    if a.cmd == "list":
        reps = relay.call("GET", f"/v1/admin/reports?days={a.days}")["reports"]
        for r in reps:
            print(fmt_row(r, seen))
        print(f"{len(reps)} reports, {sum(r['id'] not in seen for r in reps)} new")
    elif a.cmd == "new":
        reps = [r for r in relay.call("GET", f"/v1/admin/reports?days={a.days}")["reports"] if r["id"] not in seen]
        if not reps:
            print("no new reports")
        for r in reversed(reps):
            show(relay, r["id"], a.out)
            seen.add(r["id"])
            save_seen(a.out, seen)
    elif a.cmd == "get":
        rid = check_id(a.id)
        print(download(relay, rid, a.out))
        seen.add(rid); save_seen(a.out, seen)
    elif a.cmd == "show":
        rid = check_id(a.id)
        show(relay, rid, a.out)
        seen.add(rid); save_seen(a.out, seen)
    elif a.cmd == "pending":
        pend = relay.call("GET", "/v1/admin/pending")["pending"]
        for x in pend:
            print(f"{x['id']}  received {x['received'][:16]}  attempts {x['attempts']}  last status {x['last_status']}")
        print(f"{len(pend)} pending" + ("  (status 400/401/403 = the GitHub token is malformed, expired or lacks Issues access)" if pend else ""))
    elif a.cmd == "retry":
        print(json.dumps(relay.call("POST", "/v1/admin/retry"), indent=1))
    elif a.cmd == "delete":
        res = delete_report(relay, check_id(a.id), a.out, yes=a.yes)
        print(json.dumps(res, indent=1))
        issue = res.get("issue")
        print({"deleted": "The GitHub issue was deleted.",
               "emptied": "The GitHub issue was emptied, closed and locked. GitHub keeps its earlier text in the issue's "
                          "edit history; delete the issue on github.com to remove that too.",
               "gone": "The GitHub issue was already gone.",
               "none": "No GitHub issue was found for this report.",
               "queued": "GitHub could not be reached: the relay retires the issue at its next hourly run."}.get(issue, ""))
    elif a.cmd == "expire":
        print(json.dumps(relay.call("POST", "/v1/admin/expire"), indent=1))
    elif a.cmd == "draft-issue":
        rid = check_id(a.id)
        path, n = write_draft(relay, rid, a.out)
        print(f"{path}  ({n} private details removed; NEEDS OWNER REVIEW, nothing was posted)")
    elif a.cmd == "switch":
        if a.state:
            print(json.dumps(relay.call("POST", "/v1/admin/switch", {"on": a.state == "on", "message": a.message}), indent=1))
        else:
            print(json.dumps(relay.call("GET", "/v1/admin/switch"), indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())

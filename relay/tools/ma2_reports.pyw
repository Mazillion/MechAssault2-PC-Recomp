"""MA2 Reports: one click for the developer. Copy a report number (MA2-XXXXXX) anywhere, run this, and the report is
downloaded into the reports folder (%USERPROFILE%\\MA2-Reports), summarised into MA2-XXXXXX-summary.txt, and the folder
opens with the zip selected. Runs without a console window (pythonw). Developer tool only: it is not part of the game or
the installer.

It uses the owner key saved in Windows Credential Manager (run `python triage.py pending` once and answer yes to "Save
this key"). Without a saved key it opens the report's download link in the browser instead, which asks for the key there.
Shortcut: target  pythonw.exe "<this folder>\\ma2_reports.pyw"   (name it "MA2 Reports"; pin it where you like).
"""
import contextlib
import ctypes
import io
import os
import re
import subprocess
import sys
import webbrowser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import triage  # noqa: E402

TITLE = "MA2 Reports"


def message(text, error=False):
    ctypes.windll.user32.MessageBoxW(None, text, TITLE, 0x10 if error else 0x40)


def main():
    m = re.search(r"MA2-[A-Z2-7]{6}", (triage.clipboard_text() or "").upper())
    if not m:
        message("Copy a report number such as MA2-7F3K2Q first, then run MA2 Reports again.", error=True)
        return 1
    rid = m.group(0)
    relay_url = os.environ.get("MA2_RELAY_URL", "") or triage.DEFAULT_RELAY
    key = (os.environ.get("MA2_RELAY_KEY", "") or triage.cred_read() or "").strip()
    if not key:
        webbrowser.open(f"{relay_url}/v1/open/{rid}")
        return 0
    out = os.path.join(os.path.expanduser("~"), "MA2-Reports")
    try:
        relay = triage.Relay(relay_url, key)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            triage.show(relay, rid, out)
        summary = os.path.join(out, rid + "-summary.txt")
        with open(summary, "w", encoding="utf-8") as f:
            f.write(buf.getvalue())
    except SystemExit as e:
        message(f"{rid}: {e}", error=True)
        return 1
    zip_path = os.path.join(out, rid + ".zip")
    subprocess.Popen(["explorer", "/select,", zip_path], creationflags=0x08000000)
    os.startfile(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())

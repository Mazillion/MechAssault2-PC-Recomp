"""Drive the MA2 Scenario Editor through its automation pipe (editor flag --automation).

Nothing here touches the real mouse, keyboard or window focus: input goes into the editor's
ImGui queue, screenshots come from its back buffer, and OS dialogs are answered by `dialogs`.

  edctl.py launch [FILE] [--show] [--pipe NAME]   start an editor (hidden unless --show)
  edctl.py state | dump | ping
  edctl.py shot OUT.png
  edctl.py click X Y [--button right] [--double]  |  edctl.py click "Units and Props/Enemy" [--button right]
  edctl.py hover "Label path"  |  edctl.py find "Label path"  |  edctl.py items [FILTER] [--all]
  edctl.py move X Y | down X Y | up X Y [--button B] [--mods ctrl+shift]
  edctl.py drag X1 Y1 X2 Y2 [--button right] [--frames 20] [--mods ctrl+shift]
  edctl.py wheel DELTA [--at X Y] [--mods ctrl]
  (--mods on any mouse command holds the modifiers for that action only; click / double / hover take it too)
  edctl.py sel | objects [FILTER] [--near X Z R] [--limit N] | ground X Z | release_all
  edctl.py key NAME [--mods ctrl+shift] [--action press|down|up] [--hold FRAMES]
  edctl.py text "typed text"
  edctl.py wait FRAMES
  edctl.py script --flag [ARG]          the editor's developer flags (--select-class MAMech, --tab 3...)
  edctl.py open FILE.ma2map
  edctl.py dialogs [--answer yes|no|cancel] [--next-file PATH]
  edctl.py quit [--force]
  edctl.py raw '{"cmd": "...", ...}'

Coordinates are client pixels (the hidden window is 1600x900). Item paths: the last part is the
visible label; earlier parts narrow it to a window or editor tab ("Units and Props/Enemy", "Preferences/Hotkeys").
Prints the JSON reply; exits 1 when the editor reports ok=false. Protocol: AUTOMATION.md.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EDITOR = os.environ.get('MA2SE_EDITOR') or os.path.join(HERE, '..', 'out', 'MA2 Scenario Editor.exe')
DEFAULT_PIPE = os.environ.get('MA2SE_PIPE', 'ma2se')


def pipe_path(name):
    return r'\\.\pipe\%s' % name


def send(cmd, pipe=DEFAULT_PIPE, timeout=60.0):
    """Sends one command (a dict) and returns the reply dict."""
    end = time.monotonic() + timeout
    while True:
        try:
            f = open(pipe_path(pipe), 'r+b', buffering=0)
            break
        except OSError as e:
            if time.monotonic() > end:
                raise RuntimeError('no editor on pipe %s (start one with: edctl.py launch)' % pipe) from e
            time.sleep(0.2)
    with f:
        f.write((json.dumps(cmd) + '\n').encode('utf-8'))
        buf = b''
        while not buf.endswith(b'\n'):
            chunk = f.read(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode('utf-8') or '{"ok": false, "error": "no reply"}')


def launch(file=None, show=False, pipe=DEFAULT_PIPE, editor=EDITOR, settings=None, wait=60.0, extra=None):
    """Starts an editor with --automation and waits until its pipe answers. Returns the Popen.
    extra: more editor args, e.g. ['--from-game', '0', '--saved-list', path] (in-game editor tests)."""
    args = [os.path.abspath(editor), '--automation', pipe]
    if show:
        args.append('--automation-show')
    if settings:
        args += ['--settings', settings]
    args += list(extra or [])
    if file:
        args.append(os.path.abspath(file))
    proc = subprocess.Popen(args, cwd=os.path.dirname(os.path.abspath(editor)),
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    end = time.monotonic() + wait
    while time.monotonic() < end:
        if proc.poll() is not None:
            raise RuntimeError('editor exited (%s): %s' % (proc.returncode, proc.stderr.read().decode(errors='replace')))
        try:
            if send({'cmd': 'ping'}, pipe, timeout=0.5).get('ok'):
                return proc
        except RuntimeError:
            pass
        time.sleep(0.3)
    raise RuntimeError('editor did not answer on pipe %s' % pipe)


def _num(s):
    try:
        float(s)
        return True
    except ValueError:
        return False


def main(argv):
    # "script --flag arg": everything after the verb is the editor's own flag, passed as it is.
    if 'script' in argv:
        i = argv.index('script')
        pipe = argv[argv.index('--pipe') + 1] if '--pipe' in argv[:i] else DEFAULT_PIPE
        rest = argv[i + 1:]
        r = send({'cmd': 'script', 'flag': rest[0] if rest else '', 'arg': ' '.join(rest[1:])}, pipe)
        print(json.dumps(r))
        return 0 if r.get('ok') else 1
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--pipe', default=DEFAULT_PIPE)
    ap.add_argument('verb')
    ap.add_argument('args', nargs='*')
    ap.add_argument('--button', default='left')
    ap.add_argument('--double', action='store_true')
    ap.add_argument('--frames', type=int, default=20)
    ap.add_argument('--at', nargs=2, type=float)
    ap.add_argument('--near', nargs=3, type=float, metavar=('X', 'Z', 'R'))
    ap.add_argument('--limit', type=int)
    ap.add_argument('--mods', default='')
    ap.add_argument('--action', default='press')
    ap.add_argument('--hold', type=int, default=0)
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--show', action='store_true')
    ap.add_argument('--answer')
    ap.add_argument('--next-file')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--settings')
    a, extra = ap.parse_known_args(argv)
    v, p = a.verb, a.args
    if v == 'launch':
        proc = launch(p[0] if p else None, a.show, a.pipe, settings=a.settings)
        print(json.dumps({'ok': True, 'pid': proc.pid, 'pipe': a.pipe}))
        return 0
    if v in ('state', 'dump', 'ping'):
        cmd = {'cmd': v}
    elif v == 'shot':
        cmd = {'cmd': 'screenshot', 'path': os.path.abspath(p[0])}
    elif v == 'click' and len(p) >= 2 and _num(p[0]) and _num(p[1]):
        cmd = {'cmd': 'mouse', 'action': 'double' if a.double else 'click', 'x': float(p[0]), 'y': float(p[1]), 'button': a.button}
        if a.mods:
            cmd['mods'] = a.mods
    elif v == 'click':
        cmd = {'cmd': 'click_item', 'item': ' '.join(p), 'button': a.button, 'double': a.double}
        if a.mods:
            cmd['mods'] = a.mods
    elif v in ('hover', 'find'):
        cmd = {'cmd': v + '_item', 'item': ' '.join(p)}
    elif v == 'items':
        cmd = {'cmd': 'items', 'filter': ' '.join(p), 'all': a.all}
        if a.limit is not None:
            cmd['limit'] = a.limit
    elif v in ('move', 'down', 'up'):
        cmd = {'cmd': 'mouse', 'action': v, 'button': a.button}
        if len(p) >= 2:
            cmd.update(x=float(p[0]), y=float(p[1]))
        if a.mods:
            cmd['mods'] = a.mods
    elif v == 'drag':
        cmd = {'cmd': 'mouse', 'action': 'drag', 'x': float(p[0]), 'y': float(p[1]), 'x2': float(p[2]), 'y2': float(p[3]),
               'button': a.button, 'frames': a.frames}
        if a.mods:
            cmd['mods'] = a.mods
    elif v == 'wheel':
        cmd = {'cmd': 'mouse', 'action': 'wheel', 'delta': float(p[0])}
        if a.at:
            cmd.update(x=a.at[0], y=a.at[1])
        if a.mods:
            cmd['mods'] = a.mods
    elif v == 'key':
        cmd = {'cmd': 'key', 'key': p[0], 'mods': a.mods, 'action': a.action, 'hold_frames': a.hold}
    elif v in ('sel', 'release_all'):
        cmd = {'cmd': v}
    elif v == 'ground':
        cmd = {'cmd': 'ground', 'x': float(p[0]), 'z': float(p[1])}
    elif v == 'objects':
        cmd = {'cmd': 'objects', 'filter': ' '.join(p)}
        if a.near:
            cmd.update(cx=a.near[0], cz=a.near[1], r=a.near[2])
        if a.limit is not None:
            cmd['limit'] = a.limit
    elif v == 'text':
        cmd = {'cmd': 'text', 'text': ' '.join(p)}
    elif v == 'wait':
        cmd = {'cmd': 'wait_frames', 'n': int(p[0]) if p else 1}
    elif v == 'script':
        flags = p + extra
        cmd = {'cmd': 'script', 'flag': flags[0], 'arg': ' '.join(flags[1:])}
    elif v == 'open':
        cmd = {'cmd': 'open', 'path': os.path.abspath(p[0])}
    elif v == 'dialogs':
        cmd = {'cmd': 'dialogs'}
        if a.answer:
            cmd['answer'] = a.answer
        if a.next_file is not None:
            cmd['next_file'] = os.path.abspath(a.next_file) if a.next_file else ''
    elif v == 'quit':
        cmd = {'cmd': 'quit', 'force': a.force}
    elif v == 'raw':
        cmd = json.loads(' '.join(p))
    else:
        ap.error('unknown verb ' + v)
    r = send(cmd, a.pipe)
    print(json.dumps(r, indent=1) if v in ('state', 'items') else json.dumps(r))
    return 0 if r.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

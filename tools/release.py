"""Release tooling: the decision sheet, the owner's sign-off, preview and release-candidate builds, and the gates.

    py -3.13 tools/release.py versions [--set X.Y.Z [--date YYYY.MM.DD]]
    py -3.13 tools/release.py sheet <base-ref> [--head REF] [--notes FILE] [--out FILE]
    py -3.13 tools/release.py sign <sheet.md> [--by NAME]
    py -3.13 tools/release.py preview --config FILE [--n N] [--base REF] [--notes FILE] [--keep-install DIR]
    py -3.13 tools/release.py rc --config FILE [--keep-install DIR]
    py -3.13 tools/release.py gate {task,rc} --config FILE [--only a,b] [--skip a,b] [--with a,b] [--ref REF]
                                                           [--installer EXE] [--keep-installs]
    py -3.13 tools/release.py steps

The process these commands serve is in docs/releasing.md. In short:
  versions  checks the places that carry the version (or writes all of them from one number).
  sheet     lists everything a player will see that changed since <base-ref>: texts, menu order, defaults, addresses,
            numbers in the docs, version stamps and the release notes. Writes Markdown + its SHA-256.
  sign      records the owner's sign-off of a sheet (its hash and the date) in dist/release/signoff.json (not committed).
  preview   builds an installer that calls itself X.Y.Z-preview.N (same code otherwise), the sheet, a pre-release notes
            draft and the exact `gh release create --prerelease` command in a text file. It never runs gh.
  rc        refuses unless the signed sheet still matches the changes since its base; then runs `gate rc`, which builds
            the release candidate and tests it.
  gate      one command, every check hidden, one result.json + result.md in the work folder (see `steps`).

The configuration file (JSON, kept OUTSIDE the repository: it names folders on your PC) is described in
docs/releasing.md. Every program the gate starts runs without a window. Every install it runs gets its own
LOCALAPPDATA, TEMP, Documents and shortcut folders, and MA2_REPORT_RELAY pointing at a closed loopback port, so no
test can send a report anywhere or touch your own settings, saves or shortcuts.
"""
import argparse, ast, ctypes, datetime, difflib, hashlib, io, json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / 'dist' / 'release'                  # sheets, sign-off (dist/ is ignored by git)
SIGNOFF = OUT / 'signoff.json'
NW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
INSTALLER_NAME = 'MechAssault 2 Recomp Installer.exe'
SIGNED_END = '<!-- release.py: end of the signed part -->'
MUTEX = 'Local\\MechAssault2RecompInstaller'      # installer-exe/stub.cpp: one installer per Windows session


# =====================================================================================================================
# small helpers

def say(msg):
    print(msg, flush=True)


def sha_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()


def git(*args, cwd=None, check=True):
    r = subprocess.run(['git'] + list(args), cwd=str(cwd or REPO), capture_output=True, creationflags=NW)
    if check and r.returncode:
        raise SystemExit('git %s failed: %s' % (' '.join(args), r.stderr.decode('utf-8', 'replace').strip()))
    return r.stdout.decode('utf-8', 'replace')


def show(ref, path, cwd=None):
    """The file at ref (text), or None when it does not exist there."""
    r = subprocess.run(['git', 'show', '%s:%s' % (ref, path)], cwd=str(cwd or REPO), capture_output=True, creationflags=NW)
    return r.stdout.decode('utf-8', 'replace') if r.returncode == 0 else None


def run_logged(cmd, log, cwd=None, env=None, timeout=None, shell=False):
    """Run a program hidden, output to `log`. Returns (exit code, seconds). A timeout stops only this process tree."""
    t0 = time.time()
    Path(log).parent.mkdir(parents=True, exist_ok=True)
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        f.write('$ %s\n' % (cmd if isinstance(cmd, str) else subprocess.list2cmdline([str(c) for c in cmd])))
        f.flush()
        p = subprocess.Popen(cmd if shell else [str(c) for c in cmd], cwd=str(cwd) if cwd else None, env=env, stdout=f,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=NW, shell=shell)
        try:
            code = p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(p.pid); p.wait()
            code = -9
            f.write('\n[release.py] stopped after %d s (time limit)\n' % timeout)
    return code, round(time.time() - t0, 1)


def kill_tree(pid):
    """Stop one process tree that this tool started (never anything else)."""
    subprocess.run(['taskkill', '/T', '/F', '/PID', str(pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=NW)


def tail(path, n=15):
    try:
        return Path(path).read_text(encoding='utf-8', errors='replace').splitlines()[-n:]
    except OSError:
        return []


def is_link(p):
    try:
        return p.is_symlink() or (hasattr(p, 'is_junction') and p.is_junction())
    except OSError:
        return True


def manifest(dest, skip=('_work/', '_previous/')):
    """{relative path: [size, sha256]} of an install folder; links are recorded, never followed."""
    out, dest = {}, Path(dest)
    stack = [dest]
    while stack:
        d = stack.pop()
        for p in sorted(d.iterdir()):
            rel = p.relative_to(dest).as_posix()
            if any((rel + '/').startswith(s) for s in skip):
                continue
            if is_link(p):
                out[rel] = 'LINK'
            elif p.is_dir():
                stack.append(p)
            elif p.is_file():
                out[rel] = [p.stat().st_size, sha_file(p)]
    return dict(sorted(out.items()))


def copy_tree(src, dst):
    """Copy an install folder without following links (a link to the disc files would be copied as the whole disc)."""
    links = []
    for root, dirs, files in os.walk(src):
        r = Path(root)
        rel = r.relative_to(src)
        (Path(dst) / rel).mkdir(parents=True, exist_ok=True)
        for d in list(dirs):
            if is_link(r / d):
                links.append(str((rel / d).as_posix())); dirs.remove(d)
        for f in files:
            if is_link(r / f):
                links.append(str((rel / f).as_posix())); continue
            shutil.copy2(r / f, Path(dst) / rel / f)
    return links


def match_any(rel, globs):
    import fnmatch
    return any(fnmatch.fnmatch(rel, g) or fnmatch.fnmatch(rel.lower(), g.lower()) for g in globs)


def differing_bytes(a, b):
    try:
        da, db = Path(a).read_bytes(), Path(b).read_bytes()
    except OSError:
        return ''
    if len(da) != len(db):
        return 'sizes %d vs %d' % (len(da), len(db))
    return '%d of %d bytes differ' % (sum(1 for x, y in zip(da, db) if x != y), len(da))


def compare_manifests(a, b):
    only_a = sorted(set(a) - set(b)); only_b = sorted(set(b) - set(a))
    diff = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    return only_a, only_b, diff


# =====================================================================================================================
# version: the places that must agree (one source: VERSION; `versions --set` writes the rest)

VERSION_PLACES = [
    ('VERSION', 'VERSION', 'line 1: the public version; line 2: the release date'),
    ('release.json', 'kit/installer/data/release.json', '"version" and "date" (the installer, its window and the install stamp)'),
    ('game', 'kit/source/game/src/ma2_release.h', 'MA2_PUBLIC_VERSION and MA2_VERSION_MAJOR/MINOR/PATCH (game title screen, exe resource)'),
    ('editor', 'editor/src/app/version.h', 'MA2SE_VERSION (editor About box)'),
    ('editor.rc', 'editor/res/editor.rc', 'FileVersion/ProductVersion strings and FILEVERSION/PRODUCTVERSION numbers'),
    ('Changelog', 'kit/defaults/Docs/Changelog.txt', 'the newest entry\'s heading'),
    ('bug_report', '.github/ISSUE_TEMPLATE/bug_report.yml', 'the Version field\'s placeholder'),
]
DERIVED = ('installer-exe/build_stub.cmd and report-helper/build_report.cmd read VERSION at build time; the installed '
           'README.txt and Docs get {{VERSION}} from release.json at install time.')
CORE_RX = re.compile(r'^(\d+)\.(\d+)\.(\d+)(?:-preview\.(\d+))?$')


def read_versions(get):
    """get(path) -> text or None. Returns {name: value} for every version place."""
    v = {}
    t = get('VERSION') or ''
    lines = [l.strip() for l in t.splitlines() if l.strip()]
    v['VERSION'] = lines[0] if lines else None
    v['VERSION date'] = lines[1] if len(lines) > 1 else None
    try:
        rel = json.loads(get('kit/installer/data/release.json') or '{}')
    except ValueError:
        rel = {}
    v['release.json version'] = rel.get('version'); v['release.json date'] = rel.get('date')
    g = get('kit/source/game/src/ma2_release.h') or ''
    m = re.search(r'MA2_PUBLIC_VERSION\s+"([^"]+)"', g); v['game string'] = m and m.group(1)
    m = re.search(r'MA2_VERSION_MAJOR\s+(\d+).*?MA2_VERSION_MINOR\s+(\d+).*?MA2_VERSION_PATCH\s+(\d+)', g, re.S)
    v['game numbers'] = m and '.'.join(m.groups())
    m = re.search(r'MA2SE_VERSION\s+"([^"]+)"', get('editor/src/app/version.h') or ''); v['editor string'] = m and m.group(1)
    rc = get('editor/res/editor.rc') or ''
    for key in ('FileVersion', 'ProductVersion'):
        m = re.search(r'VALUE "%s", "([^"\\]*)' % key, rc); v['editor.rc %s' % key] = m and m.group(1)
    for key in ('FILEVERSION', 'PRODUCTVERSION'):
        m = re.search(r'^\s*%s (\d+),(\d+),(\d+),0' % key, rc, re.M); v['editor.rc %s' % key] = m and '.'.join(m.groups())
    m = re.search(r'^(\d+\.\d+\.\d+)\s*\r?\n-{3,}', get('kit/defaults/Docs/Changelog.txt') or '', re.M)
    v['Changelog newest entry'] = m and m.group(1)
    m = re.search(r'label:\s*Version.*?placeholder:\s*"([^"]*)"', get('.github/ISSUE_TEMPLATE/bug_report.yml') or '', re.S)
    v['bug_report placeholder'] = m and m.group(1)
    return v


def check_versions(v):
    """Problems (list of strings) with a read_versions() result. Strings carry the full version (a preview's suffix
    too); numbers, the Changelog heading and the issue template carry X.Y.Z."""
    probs = []
    m = CORE_RX.match(v.get('VERSION') or '')
    if not m:
        return ['VERSION line 1 is %r, not X.Y.Z or X.Y.Z-preview.N' % v.get('VERSION')]
    full, core = v['VERSION'], '.'.join(m.groups()[:3])
    for k in ('release.json version', 'game string', 'editor string', 'editor.rc FileVersion', 'editor.rc ProductVersion'):
        if v.get(k) != full:
            probs.append('%s is %r, VERSION says %r' % (k, v.get(k), full))
    for k in ('game numbers', 'editor.rc FILEVERSION', 'editor.rc PRODUCTVERSION', 'Changelog newest entry', 'bug_report placeholder'):
        if v.get(k) != core:
            probs.append('%s is %r, expected %r' % (k, v.get(k), core))
    if v.get('release.json date') != v.get('VERSION date'):
        probs.append('release.json date %r differs from VERSION line 2 %r' % (v.get('release.json date'), v.get('VERSION date')))
    return probs


def _sub(path, rx, repl, count=1, flags=0):
    p = Path(path)
    raw = p.read_bytes()
    text = raw.decode('utf-8')
    new, n = re.subn(rx, repl, text, count=count, flags=flags)
    if n == 0:
        raise SystemExit('%s: no match for %s' % (path, rx))
    p.write_bytes(new.encode('utf-8'))


def stamp_versions(root, full, date=None, strings_only=False):
    """Write the version into every place (line endings kept). strings_only: a preview (the numbers stay X.Y.Z)."""
    root = Path(root)
    m = CORE_RX.match(full)
    if not m:
        raise SystemExit('version must be X.Y.Z or X.Y.Z-preview.N: %r' % full)
    nums = m.groups()[:3]
    raw = (root / 'VERSION').read_bytes().decode('utf-8')
    nl = '\r\n' if '\r\n' in raw else '\n'
    lines = raw.splitlines()
    lines[0] = full
    if date:
        lines[1:2] = [date]
    (root / 'VERSION').write_bytes((nl.join(lines) + nl).encode('utf-8'))
    _sub(root / 'kit/installer/data/release.json', r'("version":\s*")[^"]*(")', r'\g<1>%s\2' % full)
    if date:
        _sub(root / 'kit/installer/data/release.json', r'("date":\s*")[^"]*(")', r'\g<1>%s\2' % date)
    _sub(root / 'kit/source/game/src/ma2_release.h', r'(#define MA2_PUBLIC_VERSION\s+")[^"]*(")', r'\g<1>%s\2' % full)
    _sub(root / 'editor/src/app/version.h', r'(#define MA2SE_VERSION\s+")[^"]*(")', r'\g<1>%s\2' % full)
    for key in ('FileVersion', 'ProductVersion'):
        _sub(root / 'editor/res/editor.rc', r'(VALUE "%s", ")[^"\\]*' % key, r'\g<1>%s' % full)
    if not strings_only:
        for name, n in zip(('MAJOR', 'MINOR', 'PATCH'), nums):
            _sub(root / 'kit/source/game/src/ma2_release.h', r'(#define MA2_VERSION_%s\s+)\d+' % name, r'\g<1>%s' % n)
        for key in ('FILEVERSION', 'PRODUCTVERSION'):
            _sub(root / 'editor/res/editor.rc', r'(^\s*%s )\d+,\d+,\d+(,0)' % key, r'\g<1>%s\2' % ','.join(nums), flags=re.M)
        _sub(root / '.github/ISSUE_TEMPLATE/bug_report.yml', r'(label:\s*Version.*?placeholder:\s*")[^"]*(")',
             r'\g<1>%s\2' % '.'.join(nums), flags=re.S)


def cmd_versions(a):
    if a.set:
        stamp_versions(REPO, a.set, a.date)
        say('wrote %s into the version places (commit them yourself)' % a.set)
    v = read_versions(lambda p: (REPO / p).read_text(encoding='utf-8') if (REPO / p).is_file() else None)
    say('Version places:')
    for name, path, what in VERSION_PLACES:
        say('  %-12s %-42s %s' % (name, path, what))
    say('  derived: ' + DERIVED)
    for k, val in v.items():
        say('  %-28s %s' % (k, val))
    probs = check_versions(v)
    for p in probs:
        say('MISMATCH ' + p)
    if a.set and any('Changelog' in p for p in probs):
        say('Add the Changelog entry for %s (kit/defaults/Docs/Changelog.txt) by hand.' % a.set)
    say('versions: %s' % ('OK' if not probs else '%d problem(s)' % len(probs)))
    return 1 if probs else 0


# =====================================================================================================================
# the decision sheet

PY_WINDOW = ('kit/installer/gui.py', 'kit/installer/gui_legacy.py', 'kit/installer/ui_kit.py', 'kit/installer/ui_theme.py',
             'kit/installer/ui_win.py', 'kit/installer/progress.py')
GAME_TEXT_DIRS = ('kit/source/game/src/', 'kit/source/runtime/src/', 'report-helper/', 'installer-exe/')
EDITOR_DIRS = ('editor/src/',)
SHIPPED_DOCS = ('kit/defaults/',)
REPO_PAGES = ('README.md', 'CONTRIBUTING.md', 'LEGAL.md', 'THIRD-PARTY-NOTICES.md', 'docs/', '.github/')
C_EXT = ('.c', '.cpp', '.h', '.hpp', '.rc')
LOG_CALL = re.compile(r'\b(?:f?w?printf|fputs|puts|perror|OutputDebugString\w*|\w*_log\w*|log\w*|slog|LOG\w*|trace\w*|TRACE\w*)\s*\(')
NUM_RX = re.compile(r'(?<![\w.])\d+(?:[.,]\d+)*\s?(?:%|percent|days?|hours?|minutes?|seconds?|ms|GB|MB|KB|TB|fps|Hz|files?|'
                    r'reports?|times?|threads?|cores?|sessions?|bots?|blocks?|characters?|steps?|lines?|tries|attempts?)(?![A-Za-z])', re.I)
URL_RX = re.compile(r'https?://[^\s"\'<>)\]}`,]+')


NOT_TEXT = re.compile(r'(?i)content-(?:type|disposition)|boundary=|^x-ma2-|\\r\\n|^--|^-[a-z]|^%[-\w.]*s$')


def texty(s, lang='py'):
    """A string a person reads (not a key, path, pattern, header or format)."""
    t = s.strip()
    if len(t) < 2 or not re.search(r'[A-Za-z]{2}', t) or NOT_TEXT.search(t):
        return False
    if re.search(r'\(\?|\\[dswbSWDB]|\[\^|^\^|\$$', t):            # a regular expression
        return False
    words = re.findall(r'[A-Za-z][A-Za-z\']+', t)
    if ' ' not in t.strip():
        if lang == 'c' and t.isupper():               # Win32 class names, environment variable names
            return False
        return bool(re.fullmatch(r"[A-Z][a-z]+[.!?:]?|[A-Z][A-Z]{2,}[.!?:]?|[A-Z][a-z]+(?:-[A-Za-z]+)+", t))
    if len(words) < 2:
        return False
    if re.fullmatch(r'[\w./\\:%{}<>#@=,-]+(?:\s+[\w./\\:%{}<>#@=,-]+)?', t) and re.search(r'[_/\\=%{}<>]', t):
        return False                                      # two code-like tokens ("key=%s", "a/b c_d")
    return True


def py_literals(text):
    """[(line, text, kind)] of the string constants in Python source; docstrings left out; f-strings with {...}."""
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return []
    lines = text.splitlines()
    skip = set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)) and node.body:
            first = node.body[0]
            if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant) and isinstance(first.value.value, str):
                skip.add(id(first.value))
    out = []
    for node in ast.walk(tree):
        if isinstance(node, ast.JoinedStr):
            parts = []
            for v in node.values:
                skip.add(id(v))
                if isinstance(v, ast.Constant):
                    parts.append(str(v.value))
                else:
                    parts.append('{...}')
                    for sub in ast.walk(v):
                        skip.add(id(sub))
            out.append((node.lineno, ''.join(parts)))
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in skip:
            out.append((node.lineno, node.value))
    res = []
    for ln, s in out:
        ctx = lines[ln - 1] if 0 < ln <= len(lines) else ''
        kind = 'log' if (re.search(r'\b(?:self\.)?log\w*\s*\(|\bprint\s*\(|\bevent\s*\(', ctx)
                         or re.match(r'[a-z]+: ', s)) else 'text'
        res.append((ln, s, kind))
    return res


def c_literals(text):
    """[(line, text, kind)] of the string literals in C/C++ source (adjacent literals joined, comments skipped)."""
    out, i, n, line = [], 0, len(text), 1
    lines = text.splitlines()
    cur = None                                         # [start line, parts]

    def flush():
        nonlocal cur
        if cur:
            ln, parts = cur
            ctx = lines[ln - 1] if 0 < ln <= len(lines) else ''
            s = ''.join(parts)
            kind = 'log' if LOG_CALL.search(ctx) or re.match(r'\s*\[[A-Z0-9_-]{2,}\]', s) else 'text'
            if not ctx.lstrip().startswith('#include'):
                out.append((ln, s, kind))
        cur = None
    while i < n:
        c = text[i]
        if c == '\n':
            line += 1; i += 1; continue
        if c in ' \t\r\f\v':
            i += 1; continue
        if text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2); seg = text[i:n if j < 0 else j + 2]; line += seg.count('\n')
            i = n if j < 0 else j + 2; continue
        if c == '"':
            j, buf = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == '\\' and j + 1 < n:
                    buf.append(text[j:j + 2]); j += 2; continue
                if text[j] == '\n':
                    break
                buf.append(text[j]); j += 1
            if cur is None:
                cur = [line, []]
            cur[1].append(''.join(buf))
            i = j + 1; continue
        if c == "'":
            j = i + 1
            while j < n and text[j] not in "'\n":
                j += 2 if text[j] == '\\' else 1
            flush(); i = j + 1; continue
        if c in 'LuU' and i + 1 < n and text[i + 1] in '"8' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            i += 2 if text[i + 1] == '8' else 1   # L"..", u8".." prefixes: the literal follows
            continue
        flush()
        if c.isalnum() or c == '_':
            while i < n and (text[i].isalnum() or text[i] == '_'):
                i += 1
        else:
            i += 1
    flush()
    return out


def literals_of(path, text):
    """[(line, text, kind)] of the readable strings of a source file; everything in the runtime counts as a log line."""
    if text is None:
        return []
    if path.endswith('.py'):
        out, lang = py_literals(text), 'py'
    elif path.endswith(C_EXT):
        out, lang = c_literals(text), 'c'
    else:
        return []
    if path.startswith('kit/source/runtime/'):
        out = [(ln, s, 'log') for ln, s, _ in out]
    return [x for x in out if texty(x[1], lang)]


def changed(base, head):
    """[(status, path, old path)] between two refs (renames followed)."""
    out = []
    for l in git('diff', '--name-status', '-M', '%s..%s' % (base, head)).splitlines():
        parts = l.split('\t')
        st = parts[0][0]
        if st == 'R':
            out.append(('R', parts[2], parts[1]))
        else:
            out.append((st, parts[1], parts[1]))
    return out


def string_changes(base, head, files):
    """For the given changed files: added/changed and removed texts. Returns (added, removed, pairs) where added and
    removed are [(path, line, text, kind)] and pairs maps an added entry index to the removed text it most resembles."""
    added, removed = [], []
    for st, path, old in files:
        new_l = literals_of(path, show(head, path)) if st != 'D' else []
        old_l = literals_of(old, show(base, old)) if st != 'A' else []
        old_set = {s for _, s, _ in old_l}; new_set = {s for _, s, _ in new_l}
        seen = set()
        for ln, s, k in new_l:
            if s not in old_set and s not in seen:
                seen.add(s); added.append((path, ln, s, k))
        seen = set()
        for ln, s, k in old_l:
            if s not in new_set and s not in seen:
                seen.add(s); removed.append((old, ln, s, k))
    pairs = {}
    pool = list(range(len(removed)))
    for ai, (p, _, s, _) in enumerate(added):
        best, score = None, 0.6
        for ri in pool:
            if removed[ri][0] != p:
                continue
            r = difflib.SequenceMatcher(None, s, removed[ri][2]).ratio()
            if r > score:
                best, score = ri, r
        if best is not None:
            pairs[ai] = best; pool.remove(best)
    return added, removed, pairs


def md_text(s, limit=600):
    s = s.replace('\r', '').replace('\n', '\\n')
    if len(s) > limit:
        s = s[:limit] + ' ...'
    return s.replace('|', '\\|')


def render_strings(added, removed, pairs, kinds=('text',)):
    lines = []
    paired = set(pairs.values())
    rows = [(i, a) for i, a in enumerate(added) if a[3] in kinds]
    for i, (p, ln, s, k) in rows:
        lines.append('- `%s:%d` "%s"' % (p, ln, md_text(s)))
        if i in pairs:
            lines.append('  - was: "%s"' % md_text(removed[pairs[i]][2]))
    gone = [r for j, r in enumerate(removed) if r[3] in kinds and j not in paired]
    if gone:
        lines.append('')
        lines.append('Removed:')
        for p, ln, s, k in gone:
            lines.append('- `%s:%d` "%s"' % (p, ln, md_text(s)))
    return lines or ['(no change)']


def diff_lines(base, head, path, old=None):
    """[(sign, text)] of the changed lines of a text file (no line numbers, so the signed part survives moves)."""
    r = subprocess.run(['git', 'diff', '-U0', '--no-color', '-M', '%s..%s' % (base, head), '--', old or path, path], cwd=str(REPO),
                       capture_output=True, creationflags=NW)
    out, hunk, ctx = [], [], [None]

    def flush():                       # a line only re-saved (line ending, trailing blanks) is no change
        same = {t.rstrip() for s, t in hunk if s == '-'} & {t.rstrip() for s, t in hunk if s == '+'}
        kept = [(s, t) for s, t in hunk if t.rstrip() not in same]
        if kept:
            out.append(('@', ctx[0]))
            out.extend(kept)
        hunk.clear()
    for l in r.stdout.decode('utf-8', 'replace').splitlines():
        if l.startswith(('+++', '---', 'diff ', 'index ', 'similarity', 'rename ', 'new file', 'deleted file')):
            continue
        if l.startswith('@@'):
            flush()
            ctx[0] = l.split('@@')[-1].strip()
        elif l[:1] in '+-':
            hunk.append((l[0], l[1:].rstrip('\r')))
    flush()
    return out


def ini_map(text):
    out, sec = {}, ''
    for l in (text or '').splitlines():
        s = l.strip()
        if not s or s.startswith((';', '#')):
            continue
        if s.startswith('[') and s.endswith(']'):
            sec = s[1:-1]; continue
        if '=' in s:
            k, v = s.split('=', 1)
            out['[%s] %s' % (sec, k.strip())] = v.strip()
    return out


SWITCH_RXS = [
    ('pc_switch', re.compile(r'pc_switch\(\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*([^)]*?)\)')),
    ('ini', re.compile(r'ma2_pc_ini_get_\w+\(\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*([^)]*?)\)')),
    ('env', re.compile(r'getenv\(\s*"((?:RECOMP|MA2)_\w+)"\s*\)')),
    ('pyenv', re.compile(r'environ(?:\.get\(|\[)\s*[\'"]((?:RECOMP|MA2)_\w+)[\'"]')),
    ('option', re.compile(r'add_argument\(\s*[\'"](--[\w-]+)[\'"]')),
]


def switches(text):
    out = {}
    for kind, rx in SWITCH_RXS:
        for m in rx.finditer(text or ''):
            g = m.groups()
            if kind == 'pc_switch':
                out['[%s] %s (env %s)' % (g[1], g[2], g[0])] = g[3].strip()
            elif kind == 'ini':
                out['[%s] %s' % (g[0], g[1])] = g[2].strip()
            elif kind in ('env', 'pyenv'):
                out.setdefault('env %s' % g[0], '')
            else:
                out['installer option %s' % g[0]] = ''
    return out


def numbers_in(lines):
    out = []
    for where, text in lines:
        for m in NUM_RX.finditer(text):
            out.append((where, m.group(0)))
    return out


def under(path, prefixes):
    return any(path == p or path.startswith(p) for p in prefixes)


def make_sheet(base, head='HEAD', notes=None):
    base_sha = git('rev-parse', '--short=12', base).strip()
    head_sha = git('rev-parse', '--short=12', head).strip()
    files = [f for f in changed(base, head)]
    paths = [p for _, p, _ in files]
    S = []
    S.append('# Decision sheet: changes since %s' % base)
    S.append('')
    S.append('Base `%s` (`%s`). Everything below the line up to "end of the signed part" is what the owner signs: every text '
             'a player can see that was added or changed, menu order, defaults and switches, addresses, numbers in the '
             'player-facing text, the docs, the version stamps and the release notes. Made by `tools/release.py sheet`; '
             'signed with `tools/release.py sign`.' % (base, base_sha))
    S.append('')
    S.append('---')

    # 1 versions
    vb = read_versions(lambda p: show(base, p)); vh = read_versions(lambda p: show(head, p))
    S += ['', '## 1. Version stamps', '', '| Place | Before | Now |', '|---|---|---|']
    for k in vh:
        S.append('| %s | %s | %s |' % (k, vb.get(k), vh.get(k)))
    probs = check_versions(vh)
    S += ['', 'Consistency: ' + ('all places agree.' if not probs else '**%d problem(s)**: %s' % (len(probs), '; '.join(probs)))]

    # 2-5 strings
    def group(pred):
        return [f for f in files if pred(f[1])]
    win = group(lambda p: p in PY_WINDOW)
    eng = group(lambda p: p.startswith('kit/installer/') and p.endswith('.py') and p not in PY_WINDOW)
    game = group(lambda p: under(p, GAME_TEXT_DIRS) and p.endswith(C_EXT))
    edit = group(lambda p: under(p, EDITOR_DIRS) and p.endswith(C_EXT))
    a_win = string_changes(base, head, win); a_eng = string_changes(base, head, eng)
    a_game = string_changes(base, head, game); a_edit = string_changes(base, head, edit)
    S += ['', '## 2. Installer window texts', '', 'New or changed texts in the installer window (with the text they replace).', '']
    S += render_strings(*a_win)
    S += ['', '## 3. Installer messages', '', 'Messages the installer engine shows in its window, stop screens and the '
          'command line (log lines are in the appendix).', '']
    S += render_strings(*a_eng)
    S += ['', '## 4. Game, report helper and installer shell texts', '', 'Menus, pages, message boxes (game log lines are '
          'in the appendix).', '']
    S += render_strings(*a_game)
    S += ['', '## 5. Scenario editor texts', '']
    S += render_strings(*a_edit)

    # 6 menus
    S += ['', '## 6. Menu positions and order', '', 'Changes in code that builds or orders menus. The order itself is decided '
          'at run time, so check each of these in the preview build.', '']
    menu_rx = re.compile(r'\bmenu|pause|_item\b|\bitems?\b|\border\b|panel|button', re.I)
    hits = []
    for st, p, old in files:
        if not (under(p, GAME_TEXT_DIRS + EDITOR_DIRS) or p in PY_WINDOW) or not p.endswith(C_EXT + ('.py',)):
            continue
        cur_ctx, n = None, 0
        for sign, text in diff_lines(base, head, p, old):
            if sign == '@':
                cur_ctx = text; continue
            if menu_rx.search(text) and (p, cur_ctx) not in hits:
                hits.append((p, cur_ctx))
    subjects = [l for l in git('log', '--format=%s', '%s..%s' % (base, head)).splitlines() if menu_rx.search(l)]
    for p, ctx in hits:
        S.append('- `%s`: `%s`' % (p, md_text(ctx or '(top of file)', 160)))
    if subjects:
        S += ['', 'Commits that name a menu, item or order:'] + ['- %s' % s for s in subjects]
    if not hits and not subjects:
        S.append('(no change)')

    # 7 defaults and switches
    S += ['', '## 7. Defaults and switches', '']
    ini_b = ini_map(show(base, 'kit/defaults/pc_settings.ini')); ini_h = ini_map(show(head, 'kit/defaults/pc_settings.ini'))
    rows = []
    for k in sorted(set(ini_b) | set(ini_h)):
        if ini_b.get(k) != ini_h.get(k):
            rows.append('| pc_settings.ini %s | %s | %s |' % (k, ini_b.get(k, '(none)'), ini_h.get(k, '(removed)')))
    sw_b, sw_h = {}, {}
    for st, p, old in files:
        if p.endswith(C_EXT + ('.py',)) and not p.startswith('tests/') and not p.startswith('tools/'):
            if st != 'D':
                for k, v in switches(show(head, p)).items(): sw_h.setdefault(k, (v, p))
            if st != 'A':
                for k, v in switches(show(base, old)).items(): sw_b.setdefault(k, (v, old))
    for k in sorted(set(sw_b) | set(sw_h)):
        b, h = sw_b.get(k), sw_h.get(k)
        if (b and b[0]) != (h and h[0]) or bool(b) != bool(h):
            rows.append('| %s | %s | %s |' % (k, (b[0] or 'present') if b else '(none)', (h[0] or 'present') if h else '(removed)'))
    S += (['| Setting | Before | Now |', '|---|---|---|'] + rows) if rows else ['(no change)']

    # 8 addresses
    S += ['', '## 8. Addresses', '']
    rel_b = show(base, 'kit/installer/data/report.json'); rel_h = show(head, 'kit/installer/data/report.json')
    def relay(t):
        try:
            return json.loads(t or '{}').get('relay_url', '')
        except ValueError:
            return '(unreadable)'
    sup = lambda t: (re.search(r'MA2_SUPPORT_URL\s+"([^"]*)"', t or '') or [None, None])[1]
    S.append('- Report relay (kit/installer/data/report.json, compiled into MA2 Report.exe): %s%s'
             % (relay(rel_h) or '(empty: Save only)', '' if relay(rel_b) == relay(rel_h) else ' (was: %s)' % (relay(rel_b) or 'empty')))
    sb, sh = sup(show(base, 'kit/source/game/src/ma2_release.h')), sup(show(head, 'kit/source/game/src/ma2_release.h'))
    S.append('- Support address (ma2_release.h): %s%s' % (sh, '' if sb == sh else ' (was: %s)' % sb))
    url_rows = []
    for st, p, old in files:
        if p.startswith(('docs/wiki/', 'tests/')):
            continue
        ub = set(URL_RX.findall(show(base, old) or '')) if st != 'A' else set()
        uh = set(URL_RX.findall(show(head, p) or '')) if st != 'D' else set()
        for u in sorted(uh - ub):
            url_rows.append('- added in `%s`: %s' % (p, u))
        for u in sorted(ub - uh):
            url_rows.append('- removed from `%s`: %s' % (old, u))
    S += url_rows or ['- no other address added or removed']

    # 9 numbers
    S += ['', '## 9. Numbers in player-facing text', '', 'Numbers with a unit in added or changed docs lines and texts '
          '(check each against what the build really does).', '']
    doc_files = [f for f in files if under(f[1], SHIPPED_DOCS + REPO_PAGES) and not f[1].startswith('docs/wiki/')
                 and f[1].endswith(('.txt', '.md', '.yml', '.ini'))]
    num_now, num_before = [], []
    for st, p, old in doc_files:
        for sign, text in diff_lines(base, head, p, old):
            if sign == '+':
                num_now.append((p, text))
            elif sign == '-':
                num_before.append((old, text))
    for a in (a_win, a_eng, a_game, a_edit):
        num_now += [(p, s) for p, _, s, k in a[0] if k == 'text']
        num_before += [(p, s) for p, _, s, k in a[1] if k == 'text']
    now, before = numbers_in(num_now), numbers_in(num_before)
    bset = {n for _, n in before}; nset = {n for _, n in now}
    rows = []
    for p, n in now:
        if n not in bset and ('- `%s`: **%s**' % (p, n)) not in rows:
            rows.append('- `%s`: **%s**' % (p, n))
    gone = sorted({'%s (`%s`)' % (n, p) for p, n in before if n not in nset})
    S += rows or ['(no new number)']
    if gone:
        S += ['', 'No longer said: ' + '; '.join(gone)]

    # 10, 11 docs
    def doc_block(title, pred):
        out = ['', title, '']
        any_ = False
        for st, p, old in files:
            if not pred(p) or p.startswith('docs/wiki/'):
                continue
            if not p.endswith(('.txt', '.md', '.yml', '.yaml', '.ini')):
                continue
            d = diff_lines(base, head, p, old)
            if not d:
                continue
            any_ = True
            out.append('### %s%s' % (p, '' if st == 'M' else ' (%s)' % {'A': 'new', 'D': 'deleted', 'R': 'renamed from ' + old}.get(st, st)))
            out.append('```diff')
            for sign, text in d:
                out.append('@@ %s' % text if sign == '@' else sign + text)
            out.append('```')
        return out if any_ else out + ['(no change)']
    S += doc_block('## 10. Docs shipped with the game', lambda p: under(p, SHIPPED_DOCS))
    S += doc_block('## 11. Repository pages', lambda p: under(p, REPO_PAGES))
    wiki = [p for p in paths if p.startswith('docs/wiki/')]
    if wiki:
        S += ['', '%d file(s) of the published wiki copy changed (docs/wiki/; generated, not listed).' % len(wiki)]

    # 12 notes
    S += ['', '## 12. Release notes', '']
    if notes:
        S += ['From `%s`:' % Path(notes).name, '', '````markdown', Path(notes).read_text(encoding='utf-8').rstrip(), '````']
    else:
        S.append('(no release notes given: pass --notes FILE)')
    S += ['', SIGNED_END]

    # appendix
    S += ['', '## Appendix (not signed): commits, files, log lines', '', 'Head `%s`.' % head_sha, '', 'Commits:']
    S += ['- `%s` %s' % tuple(l.split(' ', 1)) for l in git('log', '--format=%h %s', '%s..%s' % (base, head)).splitlines() if ' ' in l]
    S += ['', 'Files: %d changed (%s)' % (len(files), ', '.join('%s %d' % (k, sum(1 for f in files if f[0] == k))
                                                              for k in 'AMDR' if any(f[0] == k for f in files)))]
    logs = [x for a in (a_win, a_eng, a_game, a_edit) for x in a[0] if x[3] == 'log']
    if logs:
        S += ['', '<details><summary>%d new or changed log lines</summary>' % len(logs), '']
        S += ['- `%s:%d` "%s"' % (p, ln, md_text(s, 200)) for p, ln, s, k in logs]
        S += ['', '</details>']
    return '\n'.join(S) + '\n'


def signed_part(text):
    """The signed part with line numbers taken out of `path:line` references, so a fix that only moves lines keeps the sign-off."""
    part = text.split(SIGNED_END)[0]
    return re.sub(r'(`[^`\n]+?):\d+`', r'\1`', part).replace('\r\n', '\n')


def sheet_hash(text):
    return hashlib.sha256(signed_part(text).encode('utf-8')).hexdigest()


def write_sheet(base, head, notes, out):
    text = make_sheet(base, head, notes)
    h = sheet_hash(text)
    text += '\nSigned-part SHA-256: `%s`\n' % h
    out = Path(out); out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(text.encode('utf-8'))
    fh = sha_file(out)
    out.with_name(out.name + '.sha256').write_text('%s *%s\nsigned-part %s\n' % (fh, out.name, h), encoding='ascii')
    meta = {'base': base, 'base_sha': git('rev-parse', base).strip(), 'head_sha': git('rev-parse', head).strip(),
            'notes': str(Path(notes).resolve()) if notes else None, 'file_sha256': fh, 'signed_part_sha256': h}
    out.with_name(out.name + '.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    return out, fh, h


def safe_ref(ref):
    return re.sub(r'[^\w.-]+', '_', ref)


def cmd_sheet(a):
    out = a.out or OUT / ('sheet-%s.md' % safe_ref(a.base))
    out, fh, h = write_sheet(a.base, a.head, a.notes, out)
    say('%s\nsha256 %s\nsigned-part sha256 %s' % (out, fh, h))
    return 0


def cmd_sign(a):
    p = Path(a.sheet)
    text = p.read_text(encoding='utf-8')
    meta_p = p.with_name(p.name + '.json')
    meta = json.loads(meta_p.read_text(encoding='utf-8')) if meta_p.is_file() else {}
    if not meta.get('base'):
        m = re.search(r'^Base `([^`]+)`', text, re.M)
        meta['base'] = m and m.group(1)
    rec = dict(meta, sheet=str(p.resolve()), file_sha256=sha_file(p), signed_part_sha256=sheet_hash(text),
               signed=datetime.date.today().isoformat(), by=a.by or '')
    OUT.mkdir(parents=True, exist_ok=True)
    data = json.loads(SIGNOFF.read_text(encoding='utf-8')) if SIGNOFF.is_file() else {'signoffs': []}
    data['signoffs'].append(rec)
    SIGNOFF.write_text(json.dumps(data, indent=1), encoding='utf-8')
    say('signed: %s\nsheet sha256 %s, signed part %s, base %s, %s\nrecorded in %s'
        % (p.name, rec['file_sha256'], rec['signed_part_sha256'], rec.get('base'), rec['signed'], SIGNOFF))
    return 0


def signoff_status(head='HEAD'):
    """(ok, message, record): the newest sign-off still matches the changes from its base to head."""
    if not SIGNOFF.is_file():
        return False, 'no signed sheet (run `release.py sheet <base>`, show it to the owner, then `release.py sign <sheet>`)', None
    rec = json.loads(SIGNOFF.read_text(encoding='utf-8'))['signoffs'][-1]
    notes = rec.get('notes')
    if notes and not Path(notes).is_file():
        return False, 'the release notes the sheet was made with are gone: %s' % notes, rec
    now = make_sheet(rec['base'], head, notes)
    h = sheet_hash(now)
    if h == rec['signed_part_sha256']:
        return True, 'the signed sheet (%s, %s) matches %s..%s' % (Path(rec['sheet']).name, rec['signed'], rec['base'], head), rec
    old = Path(rec['sheet']).read_text(encoding='utf-8') if Path(rec['sheet']).is_file() else ''
    d = [l for l in difflib.unified_diff(signed_part(old).splitlines(), signed_part(now).splitlines(), 'signed', 'now', n=0, lineterm='')
         if l[:1] in '+-' and not l.startswith(('+++', '---'))]
    return False, ('the changes since %s no longer match the signed sheet (%d line(s) differ; first: %s). Make a new sheet, '
                   'get it signed, or undo the change (an RC reset: see docs/releasing.md)' % (rec['base'], len(d), ' | '.join(d[:6]))), rec


# =====================================================================================================================
# configuration

def load_config(path):
    if not path:
        raise SystemExit('--config FILE is needed (a JSON file outside the repository; see docs/releasing.md)')
    c = json.loads(Path(path).read_text(encoding='utf-8'))
    if not c.get('work_dir'):
        raise SystemExit('%s: "work_dir" is required' % path)
    c.setdefault('build_dir', str(Path(c['work_dir']) / 'build'))
    for k in ('work_dir', 'build_dir'):
        p = str(Path(c[k]).resolve())
        try:
            Path(p).resolve().relative_to(REPO)
            raise SystemExit('%s must be outside the repository' % k)
        except ValueError:
            pass
    c.setdefault('install_timeout_minutes', 240)
    c.setdefault('suite_timeout_minutes', 60)
    c.setdefault('wait_for_installer_minutes', 180)
    c.setdefault('grep_words', [])
    c.setdefault('relay_forbidden', ['-staging'])
    c.setdefault('known_failures', {})
    c.setdefault('identity_files', [])
    c.setdefault('ab_allowed_removed', [])
    c.setdefault('fastpath_marker', r'kept build: used')            # the installer's log line when it reuses the kept build
    c.setdefault('fallback_env', {'MA2_TEST_FINGERPRINT_SALT': 'compiler=1,patches=2'})   # the format the installer reads
    c.setdefault('fallback_marker', r'kept build: not used')        # its log line when it makes everything again
    c['_path'] = str(Path(path).resolve())
    return c


# =====================================================================================================================
# the gate

IDENTITY = ['MechAssault 2.exe', 'MA2 Scenario Editor.exe', 'MA2 Report.exe', 'recomp-install.json', 'recomp-manifest.json',
            'README.txt', 'Controls.txt', 'Docs/*',                 # files that carry the version or the build identity
            'MechAssault 2 Recomp Installer.exe']                   # 1.1.7+: the installer copy kept in the install folder
BUILD_IDENTITY = ['MechAssault 2.exe', 'recomp-install.json', 'recomp-manifest.json']   # differ between two builds of one source
PLAYER_FILES = ['pc_settings.ini', 'saves/*', 'Saves/*']
FAULT_MAY_LACK = ['MechAssault 2 Recomp Installer.exe']     # 1.1.7 self-update: the installer copy (fault run: may be missing)


class Gate:
    def __init__(self, mode, cfg, ref='HEAD', installer=None, keep=False):
        self.mode, self.cfg, self.keep = mode, cfg, keep
        self.sha = git('rev-parse', ref).strip()
        self.ref = ref
        stamp = time.strftime('%Y%m%d-%H%M%S')
        self.dir = Path(cfg['work_dir']) / ('gate-%s-%s' % (mode, stamp))
        self.dir.mkdir(parents=True, exist_ok=True)
        self.logs = self.dir / 'logs'; self.logs.mkdir(exist_ok=True)
        self.clone = self.dir / 'repo'
        self.installer = Path(installer) if installer else None
        self.results = []
        self.state = {}
        self.guard = closed_port()
        self.t0 = time.time()

    # ---------------------------------------------------------------- environment for every program the gate starts
    def env(self, case, extra=None):
        base = self.dir / 'env' / case
        e = {k: v for k, v in os.environ.items() if not k.upper().startswith(('MA2_', 'RECOMP_', 'PYTHON', 'PIP_', 'CONDA_', 'VIRTUAL_ENV'))}
        dirs = {}
        for d in ('appdata', 'temp', 'docs', 'lnk'):
            dirs[d] = base / d; dirs[d].mkdir(parents=True, exist_ok=True)
        e.update(LOCALAPPDATA=str(dirs['appdata']), TEMP=str(dirs['temp']), TMP=str(dirs['temp']),
                 MA2_DOCUMENTS_DIR=str(dirs['docs']), MA2_SHORTCUT_DIR=str(dirs['lnk']),
                 MA2_REPORT_RELAY='http://127.0.0.1:%d' % self.guard)
        e.update(extra or {})
        return e, base

    def need(self, *keys):
        miss = [k for k in keys if not self.cfg.get(k) or not Path(self.cfg[k]).exists()]
        return miss

    # ---------------------------------------------------------------- one real install with an installer exe
    def install(self, case, exe, args, extra_env=None, dest=None):
        e, base = self.env(case, extra_env)
        dest = Path(dest) if dest else base / 'MechAssault 2'
        args = [a if a != '{dest}' else str(dest) for a in args]
        tries = 0
        while True:
            waited = wait_mutex(self.cfg['wait_for_installer_minutes'] * 60)
            if waited is None:
                return {'code': -1, 'seconds': 0, 'dest': dest, 'base': base, 'note': 'another installer kept running'}
            log = base / ('out%s.txt' % ('' if not tries else '-%d' % tries))
            code, secs = run_logged([exe] + args, log, env=e, timeout=self.cfg['install_timeout_minutes'] * 60)
            text = log.read_text(encoding='utf-8', errors='replace')
            if code != 0 and 'already running' in text and tries < 3:
                tries += 1; continue
            return {'code': code, 'seconds': secs, 'dest': dest, 'base': base, 'log': log,
                    'note': ('waited %d s for another installer; ' % waited if waited else '')}

    def install_args(self, dest='{dest}'):
        return ['--xiso', self.cfg['xiso'], '--dest', dest, '--ffmpeg', self.cfg['ffmpeg']]

    # ---------------------------------------------------------------- run the steps
    def run(self, names):
        say('gate %s: %s at %s -> %s' % (self.mode, self.ref, self.sha[:12], self.dir))
        say('steps: ' + ', '.join(names))
        before = real_profile_installer_logs()
        for name in names:
            fn = STEPS[name][0]
            say('== %s' % name)
            t = time.time()
            try:
                r = fn(self) or {}
            except Exception as ex:                      # a broken step is a failed step, never a crashed gate
                import traceback
                (self.logs / ('%s-error.txt' % name)).write_text(traceback.format_exc(), encoding='utf-8')
                r = {'status': 'FAIL', 'summary': 'step raised %s: %s' % (type(ex).__name__, ex)}
            r.setdefault('status', 'PASS')
            r['name'] = name; r['seconds'] = round(time.time() - t, 1)
            self.results.append(r); self.state[name] = r
            say('   %s %s (%.0f s)' % (r['status'], r.get('summary', ''), r['seconds']))
            self.save()
        after = real_profile_installer_logs()
        new = sorted(set(after) - set(before))
        self.results.append({'name': 'isolation', 'seconds': 0,
                             'status': 'WARN' if new else 'PASS',
                             'summary': ('%d new installer log(s) in your own %%LOCALAPPDATA%%\\MechAssault2\\Logs during the gate '
                                         '(another installer run on this PC, or a redirection leak): %s' % (len(new), ', '.join(new)))
                             if new else 'your own %LOCALAPPDATA%\\MechAssault2 got no installer log during the gate'})
        if not self.keep:
            self.cleanup()
        self.save(final=True)
        return 0 if self.overall() != 'FAIL' else 1

    def overall(self):
        st = [r['status'] for r in self.results]
        return 'FAIL' if 'FAIL' in st else 'WARN' if 'WARN' in st else 'PASS'

    def cleanup(self):
        """Delete the installs this run made (keep logs, manifests and results)."""
        freed = 0
        for d in (self.dir / 'env').glob('*/MechAssault 2'):
            for p in d.rglob('*'):
                if p.is_file() and not is_link(p):
                    freed += p.stat().st_size
            shutil.rmtree(d, ignore_errors=True)
        for d in (self.dir / 'env').glob('*/temp'):
            shutil.rmtree(d, ignore_errors=True)
        self.state['cleanup'] = 'deleted the installs (%.1f GB); --keep-installs keeps them' % (freed / 2**30)

    def save(self, final=False):
        res = {'mode': self.mode, 'ref': self.ref, 'sha': self.sha, 'work': str(self.dir), 'config': self.cfg.get('_path'),
               'started': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(self.t0)), 'seconds': round(time.time() - self.t0),
               'finished': final, 'overall': self.overall(), 'steps': self.results, 'cleanup': self.state.get('cleanup')}
        (self.dir / 'result.json').write_text(json.dumps(res, indent=1, default=str), encoding='utf-8')
        md = ['# Gate %s: %s' % (self.mode, res['overall']), '',
              'Commit `%s` (%s). Started %s, %d s%s. Work folder `%s`.' % (self.sha[:12], self.ref, res['started'], res['seconds'],
                                                                         '' if final else ' (still running)', self.dir), '',
              '| Step | Result | Time | Summary |', '|---|---|---|---|']
        for r in self.results:
            md.append('| %s | %s | %d s | %s |' % (r['name'], r['status'], r.get('seconds', 0), md_text(str(r.get('summary', '')), 400)))
        for r in self.results:
            det = r.get('details')
            if det:
                md += ['', '## %s' % r['name'], '']
                md += ['- ' + md_text(str(x), 500) for x in (det if isinstance(det, list) else [det])]
        if res['cleanup']:
            md += ['', res['cleanup']]
        (self.dir / 'result.md').write_text('\n'.join(md) + '\n', encoding='utf-8')


def closed_port():
    """A loopback port nothing listens on (MA2_REPORT_RELAY for every test and install: a send can only fail)."""
    for _ in range(20):
        s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
        c = socket.socket(); c.settimeout(1)
        try:
            c.connect(('127.0.0.1', port))
        except OSError:
            return port
        finally:
            c.close()
    raise SystemExit('no closed loopback port found')


def wait_mutex(limit):
    """Wait while another installer runs (the stub allows one per Windows session). Seconds waited, or None on timeout."""
    if os.name != 'nt':
        return 0
    k32 = ctypes.windll.kernel32
    k32.OpenMutexW.restype = ctypes.c_void_p
    t0 = time.time()
    while True:
        h = k32.OpenMutexW(0x00100000, False, MUTEX)
        if not h:
            return round(time.time() - t0)
        k32.CloseHandle(ctypes.c_void_p(h))
        if time.time() - t0 > limit:
            return None
        time.sleep(3)          # short: another tool that runs installers back to back leaves only a few seconds free


def real_profile_installer_logs():
    """Names of installer logs in YOUR own profile (read only; used to prove the gate's installs stayed redirected)."""
    d = Path(os.environ.get('LOCALAPPDATA', '')) / 'MechAssault2' / 'Logs'
    try:
        return [p.name for p in d.glob('install-*.log')]
    except OSError:
        return []


def packed(installer):
    sys.path.insert(0, str(REPO / 'tools'))
    import kit_diff
    sys.path.pop(0)
    return kit_diff.read_kit(installer)


def count_in(b, s):
    return b.count(s.encode('ascii')) + b.count(s.encode('utf-16-le'))


# ---------------------------------------------------------------- steps

def st_clone(g):
    dirty = git('status', '--porcelain', '--untracked-files=no').strip()
    code, secs = run_logged(['git', 'clone', '--quiet', '--no-checkout', str(REPO), str(g.clone)], g.logs / 'clone.txt')
    if code:
        return {'status': 'FAIL', 'summary': 'git clone failed (%s)' % (g.logs / 'clone.txt')}
    code, _ = run_logged(['git', '-c', 'advice.detachedHead=false', 'checkout', '--quiet', g.sha], g.logs / 'checkout.txt', cwd=g.clone)
    if code:
        return {'status': 'FAIL', 'summary': 'checkout failed'}
    return {'status': 'WARN' if dirty else 'PASS',
            'summary': 'fresh clone at %s%s' % (g.sha[:12], '; NOTE: your checkout has uncommitted changes, which are NOT in this gate' if dirty else '')}


def st_versions(g):
    v = read_versions(lambda p: (g.clone / p).read_text(encoding='utf-8') if (g.clone / p).is_file() else None)
    probs = check_versions(v)
    g.state['version'] = v.get('VERSION')
    return {'status': 'FAIL' if probs else 'PASS', 'summary': ('version %s in all %d places' % (v.get('VERSION'), len(v)))
            if not probs else '; '.join(probs), 'details': ['%s = %s' % kv for kv in v.items()]}


def st_deps(g):
    miss = g.need('capstone')
    if miss:
        return {'status': 'FAIL', 'summary': 'config needs %s (an extracted copy, checked against the lock)' % miss}
    args = [sys.executable, 'tools/fetch_deps.py', '--from', 'capstone=%s' % g.cfg['capstone']]
    if g.cfg.get('ffmpeg') and Path(g.cfg['ffmpeg']).is_dir():
        args += ['--from', 'ffmpeg=%s' % g.cfg['ffmpeg']]
    code, secs = run_logged(args, g.logs / 'deps.txt', cwd=g.clone)
    return {'status': 'PASS' if code == 0 else 'FAIL', 'summary': 'fetch_deps (local copies, hash-checked) exit %d' % code}


def st_build(g):
    if g.installer:
        g.state['installer'] = g.installer
        return {'status': 'PASS', 'summary': 'using the given installer %s (sha256 %s)' % (g.installer.name, sha_file(g.installer)[:16])}
    want = json.loads((g.clone / 'tools' / 'deps.lock.json').read_text(encoding='utf-8'))['python']['version']
    if '%d.%d.%d' % sys.version_info[:3] != want:
        return {'status': 'FAIL', 'summary': 'run release.py with Python %s (the build bundles it)' % want}
    code, secs = run_logged([sys.executable, 'tools/build_release.py', '--build-dir', g.cfg['build_dir']], g.logs / 'build.txt', cwd=g.clone)
    exe = g.clone / 'dist' / INSTALLER_NAME
    if code or not exe.is_file():
        return {'status': 'FAIL', 'summary': 'build_release exit %d' % code, 'details': tail(g.logs / 'build.txt')}
    g.state['installer'] = exe
    zips = sorted((g.clone / 'dist').glob('MechAssault2-PC-Recomp-v*-Windows-x64.zip'))
    if zips:
        g.state['zip'] = zips[-1]                 # the RecompLauncher zip (checked by launcher_zip)
    _, kid = packed(exe)
    return {'status': 'PASS', 'summary': '%s %d bytes sha256 %s kit id %s (%d s)%s' % (exe.name, exe.stat().st_size, sha_file(exe), kid, secs,
                                                                                    '; %s' % zips[-1].name if zips else '')}


def need_installer(g):
    exe = g.state.get('installer')
    return exe if exe and Path(exe).is_file() else None


def need_zip(g):
    z = g.state.get('zip')
    return z if z and Path(z).is_file() else None


def binaries(g):
    exe = need_installer(g)
    out = [exe] if exe else []
    if need_zip(g):
        out.append(need_zip(g))                   # privacy_scan --binaries opens it: every member, the installer's packed files
    for rel in ('kit/editor/MA2 Scenario Editor.exe', 'installer-exe/stub.exe', 'kit/report/MA2 Report.exe'):
        if (g.clone / rel).is_file():
            out.append(g.clone / rel)
    return out


def build_tools(g):
    """The clone's tools/build_release.py as a module (the zip name and its checks live there)."""
    sys.path.insert(0, str(g.clone / 'tools'))
    try:
        import build_release
        return build_release
    finally:
        sys.path.pop(0)


def st_launcher_zip(g):
    exe = need_installer(g)
    if not exe:
        return {'status': 'SKIP', 'summary': 'needs the build'}
    if not (g.clone / 'recomplauncher.json').is_file():
        return {'status': 'SKIP', 'summary': 'this commit has no recomplauncher.json'}
    if g.installer:
        return {'status': 'SKIP', 'summary': 'a given installer (--installer) comes without its zip'}
    if g.installer:
        return {'status': 'SKIP', 'summary': 'a given installer (--installer) comes without its zip'}
    br = build_tools(g)
    z = need_zip(g)
    if not z:
        return {'status': 'FAIL', 'summary': 'no %s in dist' % br.launcher_zip_name('<version>')}
    probs = br.check_launcher_zip(z, exe)
    schema = g.cfg.get('recomplauncher_schema')
    note = 'schema not configured'
    if schema and Path(schema).is_file():
        try:
            import jsonschema, zipfile
            with zipfile.ZipFile(z) as zf:
                prof = json.loads(zf.read(br.LAUNCHER_JSON).decode('utf-8'))
            errs = list(jsonschema.Draft7Validator(json.loads(Path(schema).read_text(encoding='utf-8'))).iter_errors(prof))
            probs += ['schema: %s at %s' % (e.message[:160], '/'.join(map(str, e.path))) for e in errs]
            note = 'schema %d error(s)' % len(errs)
        except ImportError:
            note = 'schema configured, jsonschema not installed'
    return {'status': 'FAIL' if probs else 'PASS', 'summary': '%s %d bytes sha256 %s: %s; %s' % (
        z.name, z.stat().st_size, sha_file(z), 'members, installer bytes and profile OK' if not probs else '%d problem(s)' % len(probs), note),
        'details': probs or None}


def st_privacy_tree(g):
    code, _ = run_logged([sys.executable, 'tools/privacy_scan.py'], g.logs / 'privacy-tree.txt', cwd=g.clone)
    return {'status': 'PASS' if code == 0 else 'FAIL', 'summary': (tail(g.logs / 'privacy-tree.txt', 1) or ['?'])[0],
            'details': tail(g.logs / 'privacy-tree.txt', 20) if code else None}


def st_privacy_bin(g):
    b = binaries(g)
    if not need_installer(g):
        return {'status': 'SKIP', 'summary': 'needs the build'}
    code, _ = run_logged([sys.executable, 'tools/privacy_scan.py', '--binaries'] + b, g.logs / 'privacy-binaries.txt', cwd=g.clone)
    return {'status': 'PASS' if code == 0 else 'FAIL', 'summary': '%s (%s)' % ((tail(g.logs / 'privacy-binaries.txt', 1) or ['?'])[0],
                                                                         ', '.join(p.name for p in b)),
            'details': tail(g.logs / 'privacy-binaries.txt', 20) if code else None}


def st_grepbin(g):
    exe = need_installer(g)
    if not exe:
        return {'status': 'SKIP', 'summary': 'needs the build'}
    words = [w.lower() for w in g.cfg['grep_words']] + ['c:\\users\\']
    for var in ('USERNAME', 'COMPUTERNAME'):
        if len(os.environ.get(var, '')) > 2:
            words.append(os.environ[var].lower())
    words = sorted(set(words))
    sys.path.insert(0, str(g.clone / 'tools'))
    import privacy_scan
    sys.path.pop(0)
    hits, n = [], 0
    blobs = [(p.name, p.read_bytes()) for p in binaries(g)]
    files, _ = packed(exe)
    # The bundled python.org runtime (python/) is unmodified third-party code with time-zone tables, example paths and
    # credits; privacy_scan --binaries checks it with its own rules (names next to a path only). Everything else counts.
    # Third-party packages whose every file matches the hash in tools/deps.lock.json (credits name their authors) are
    # unmodified upstream files, also left to privacy_bin.
    locked = {}
    try:
        lock = json.loads((g.clone / 'tools' / 'deps.lock.json').read_text(encoding='utf-8'))
        for pkg in lock.values():
            if isinstance(pkg, dict) and pkg.get('dest', '').startswith('kit/') and isinstance(pkg.get('files'), dict):
                for rel, h in pkg['files'].items():
                    locked['%s/%s' % (pkg['dest'][4:], rel)] = h
    except (OSError, ValueError):
        pass
    upstream = lambda k, v: k.startswith('python/') or (k in locked and hashlib.sha256(v).hexdigest() == locked[k])
    runtime = [k for k, v in files.items() if upstream(k, v)]
    blobs += [('packed ' + k, v) for k, v in files.items() if not upstream(k, v)]
    z = need_zip(g)
    if z:                                         # the RecompLauncher zip's members (its installer is the exe above, byte for byte)
        import zipfile
        with zipfile.ZipFile(z) as zf:
            exe_sha = sha_file(exe)
            blobs += [('%s!%s' % (z.name, m), zf.read(m)) for m in zf.namelist()
                      if hashlib.sha256(zf.read(m)).hexdigest() != exe_sha]
    for name, data in blobs:
        n += 1
        low = data.lower()
        for mail in privacy_scan.THIRD_PARTY_EMAILS:              # licence authors' addresses (privacy_scan's own list)
            low = low.replace(mail.encode('ascii'), b'').replace(mail.encode('utf-16-le'), b'')
        for w in words:
            c = count_in(low, w)
            if c:
                hits.append('%s: %d x word #%d' % (name, c, words.index(w)))
    (g.logs / 'grepbin.txt').write_text('\n'.join(hits) + '\n', encoding='utf-8')
    return {'status': 'FAIL' if hits else 'PASS', 'summary': '%d words (config + profile path + user/PC name) in %d binaries and packed '
            'files: %d hit(s) (%d unmodified upstream files, python.org runtime and lock-verified packages, left to '
            'privacy_bin)' % (len(words), n, len(hits), len(runtime)),
            'details': hits[:40] or None}


def relay_expected(g):
    try:
        return json.loads((g.clone / 'kit/installer/data/report.json').read_text(encoding='utf-8')).get('relay_url', '')
    except (OSError, ValueError):
        return ''


def relay_counts(data, url, forbidden):
    return {'expected': count_in(data, url) if url else 0, 'forbidden': sum(count_in(data, f) for f in forbidden),
            'empty': count_in(data, '"relay_url": ""') + count_in(data, '"relay_url":""')}


def st_relay_packed(g):
    exe = need_installer(g)
    if not exe:
        return {'status': 'SKIP', 'summary': 'needs the build'}
    url = relay_expected(g)
    files, _ = packed(exe)
    tot = {'expected': 0, 'forbidden': 0, 'empty': 0}; where = []
    for k, v in files.items():
        c = relay_counts(v, url, g.cfg['relay_forbidden'])
        if any(c.values()):
            where.append('%s %s' % (k, c))
        for x in tot:
            tot[x] += c[x]
    has_json = any('report.json' in w for w in where if "'expected': 0" not in w)
    has_helper = any('MA2 Report.exe' in w for w in where if "'expected': 0" not in w)
    if not url:
        st = 'FAIL' if g.mode == 'rc' else 'WARN'
        return {'status': st, 'summary': 'kit/installer/data/report.json has no relay address: this build can only save reports', 'details': where}
    ok = has_json and has_helper and not tot['forbidden'] and not tot['empty']
    return {'status': 'PASS' if ok else 'FAIL', 'summary': 'relay %s: in report.json %s, in MA2 Report.exe %s; forbidden %d, empty relay_url %d (%d packed files)'
            % (url, has_json, has_helper, tot['forbidden'], tot['empty'], len(files)), 'details': where}


def st_helper_build(g):
    """The suites' own report helper, built in the clone WITHOUT a relay address (test_report_helper starts it)."""
    e = {k: v for k, v in os.environ.items() if k.upper() != 'MA2_RELAY_URL'}
    code, _ = run_logged('"%s"' % (g.clone / 'report-helper' / 'build_report.cmd'), g.logs / 'helper-build.txt',
                         cwd=g.clone / 'report-helper', env=e, shell=True)
    exe = g.clone / 'report-helper' / 'MA2 Report.exe'
    if code or not exe.is_file():
        return {'status': 'FAIL', 'summary': 'build_report.cmd exit %d' % code, 'details': tail(g.logs / 'helper-build.txt')}
    data = exe.read_bytes()
    url = relay_expected(g)
    bad = (count_in(data, url) if url else 0) + sum(count_in(data, f) for f in g.cfg['relay_forbidden']) + count_in(data, 'workers.dev')
    return {'status': 'FAIL' if bad else 'PASS', 'summary': 'test helper built; relay addresses compiled in: %d (must be 0)' % bad}


def suite_env(g, name):
    e, base = g.env('suite-' + name)
    for k in ('xbe', 'game_files', 'xiso', 'ffmpeg', 'recomplauncher_schema'):
        if g.cfg.get(k):
            e['MA2_TEST_%s' % {'xbe': 'XBE', 'game_files': 'GAME_FILES', 'xiso': 'XISO', 'ffmpeg': 'FFMPEG_ZIP',
                               'recomplauncher_schema': 'RECOMPLAUNCHER_SCHEMA'}[k]] = g.cfg[k]
    return e


def parse_suite(text):
    ran = re.findall(r'^Ran (\d+) tests?', text, re.M)
    checks = re.findall(r'^(\d+) checks, (\d+) failed', text, re.M)
    fails = re.findall(r'^(?:FAIL|ERROR): (\w+)', text, re.M)
    skipped = re.findall(r'skipped=(\d+)', text)
    if checks:
        return '%s checks, %s failed' % checks[-1], fails or (['%s check(s)' % checks[-1][1]] if checks[-1][1] != '0' else [])
    if ran:
        return 'ran %s%s, %s' % (ran[-1], ' (%s skipped)' % skipped[-1] if skipped else '',
                                 'OK' if re.search(r'^OK\b', text, re.M) else 'FAILED %s' % ','.join(fails)), fails
    return (text.strip().splitlines() or ['no output'])[-1][:200], fails


def run_suites(g, pattern):
    tests = sorted((g.clone / 'tests' / 'installer').glob(pattern))
    if pattern == 'test_*.py':
        tests = [t for t in tests if t.name not in ('test_fake.py',)]
    rows, bad, warn = [], [], []
    for t in tests:
        log = g.logs / ('suite-%s.txt' % t.stem)
        for attempt in range(2):
            wait_mutex(g.cfg['wait_for_installer_minutes'] * 60)      # the stub tests start the real stub (one per session)
            code, secs = run_logged([sys.executable, '-B', str(t)], log, cwd=g.clone, env=suite_env(g, t.stem),
                                    timeout=g.cfg['suite_timeout_minutes'] * 60)
            text = log.read_text(encoding='utf-8', errors='replace')
            if not (code and 'Installer is already running' in text):
                break
            shutil.copy2(log, log.with_name(log.stem + '-busy.txt'))   # another installer started meanwhile: once more
        summ, fails = parse_suite(text)
        if attempt:
            summ += ' (run again: another installer was running)'
        known = g.cfg['known_failures'].get(t.stem, [])
        line = '%s: exit %d, %s (%d s)' % (t.stem, code, summ, secs)
        if code:
            if fails and known and set(fails) <= set(known):
                warn.append(t.stem); line += ' [known: %s]' % ', '.join(fails)
            else:
                bad.append(t.stem)
        rows.append(line)
        say('   ' + line)
    st = 'FAIL' if bad else 'WARN' if warn else 'PASS'
    return {'status': st, 'summary': '%d suite(s): %d failed%s' % (len(tests), len(bad), ', %d known failure(s)' % len(warn) if warn else ''),
            'details': rows}


def st_suites(g):
    return run_suites(g, 'test_*.py')


def st_ui_flow(g):
    return run_suites(g, 'ui_flow*.py')


def st_relay_tests(g):
    rd = g.cfg.get('relay_dir')
    if not rd or not Path(rd).is_dir():
        return {'status': 'SKIP', 'summary': 'no "relay_dir" in the config (the relay is not in this repository)'}
    rd = Path(rd); rows, bad = [], 0
    e, _ = g.env('relay')
    for t in sorted((rd / 'test').glob('*.test.mjs')):
        log = g.logs / ('relay-%s.txt' % t.stem)
        code, secs = run_logged([g.cfg.get('node', 'node'), '--test', str(t.relative_to(rd))], log, cwd=rd, env=e, timeout=1800)
        txt = log.read_text(encoding='utf-8', errors='replace')
        m = re.findall(r'^# (pass|fail) (\d+)', txt, re.M)
        rows.append('%s: exit %d %s' % (t.name, code, ' '.join('%s %s' % x for x in m))); bad += code != 0
    for t in sorted((rd / 'test').glob('test_*.py')):
        log = g.logs / ('relay-%s.txt' % t.stem)
        args = [sys.executable, '-B', str(t)] + ([str(g.clone / 'kit' / 'installer')] if 'client_compat' in t.name else [])
        code, secs = run_logged(args, log, cwd=rd, env=e, timeout=1800)
        rows.append('%s: exit %d %s' % (t.name, code, (tail(log, 1) or [''])[0][:160])); bad += code != 0
    return {'status': 'FAIL' if bad else 'PASS', 'summary': '%d relay test file(s), %d failed' % (len(rows), bad), 'details': rows}


def st_sheet(g):
    base = g.cfg.get('base')
    if not base:
        return {'status': 'SKIP', 'summary': 'no "base" ref in the config'}
    out, fh, h = write_sheet(base, g.sha, g.cfg.get('notes'), g.dir / ('sheet-%s.md' % safe_ref(base)))
    return {'status': 'PASS', 'summary': '%s: sha256 %s, signed part %s' % (out.name, fh[:16], h[:16])}


def st_signoff(g):
    ok, msg, rec = signoff_status(g.sha)
    return {'status': 'PASS' if ok else 'FAIL', 'summary': msg}


def st_kit_diff(g):
    exe = need_installer(g)
    prev = g.cfg.get('previous_installer')
    if not exe or not prev or not Path(prev).is_file():
        return {'status': 'SKIP', 'summary': 'needs the build and "previous_installer"'}
    a, ia = packed(prev); b, ib = packed(exe)
    only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
    diff = sorted(k for k in set(a) & set(b) if a[k] != b[k])
    det = ['differs: ' + k for k in diff] + ['only in the previous: ' + k for k in only_a] + ['new: ' + k for k in only_b]
    (g.logs / 'kit-diff.txt').write_text('\n'.join(det) + '\n', encoding='utf-8')
    return {'status': 'PASS', 'summary': 'packed files vs the previous installer (kit %s -> %s): %d same, %d differ, %d removed, %d new (review list)'
            % (ia, ib, len(set(a) & set(b)) - len(diff), len(diff), len(only_a), len(only_b)), 'details': det[:80]}


def changed_defaults(g):
    """Installed paths of the files under kit/defaults that changed since the base (pc_settings.ini, Docs/...). They differ
    on purpose, and their text is in the signed sheet."""
    base = g.cfg.get('base')
    if not base:
        return []
    out = git('diff', '--name-only', base, g.sha, '--', 'kit/defaults', check=False)
    return [l[len('kit/defaults/'):] for l in out.splitlines() if l.startswith('kit/defaults/')]


def ab_verdict(g, prev_m, cand_m, base_prev, base_cand):
    only_p, only_c, diff = compare_manifests(prev_m, cand_m)
    ident = IDENTITY + g.cfg['identity_files'] + changed_defaults(g)
    det, bad = [], []
    for k in only_p:
        det.append('only in the previous release: ' + k)
        if not match_any(k, g.cfg['ab_allowed_removed']):
            bad.append(k)
    for k in only_c:
        det.append('new: ' + k)
    for k in diff:
        how = differing_bytes(base_prev / k, base_cand / k) if prev_m[k] != 'LINK' else 'link'
        det.append('differs: %s (%s)%s' % (k, how, '' if match_any(k, ident) else '  <- not an identity file'))
        if not match_any(k, ident):
            bad.append(k)
    return only_p, only_c, diff, det, bad


def st_ab_fresh(g):
    exe = need_installer(g)
    miss = g.need('previous_installer', 'xiso', 'ffmpeg')
    if not exe or miss:
        return {'status': 'SKIP', 'summary': 'needs the build and %s' % (miss or 'the installer')}
    runs = {}
    for case, x in (('fresh-previous', g.cfg['previous_installer']), ('fresh-candidate', exe)):
        say('   %s install ...' % case)
        r = g.install(case, x, g.install_args())
        r['manifest'] = manifest(r['dest']) if Path(r['dest']).is_dir() else {}
        (g.logs / ('manifest-%s.json' % case)).write_text(json.dumps(r['manifest'], indent=0), encoding='utf-8')
        runs[case] = r
        say('   %s exit %d in %.0f s, %d files' % (case, r['code'], r['seconds'], len(r['manifest'])))
    p, c = runs['fresh-previous'], runs['fresh-candidate']
    g.state['fresh'] = c
    if p['code'] or c['code']:
        return {'status': 'FAIL', 'summary': 'install exit codes: previous %d, candidate %d' % (p['code'], c['code']),
                'details': tail(c.get('log', ''), 10)}
    only_p, only_c, diff, det, bad = ab_verdict(g, p['manifest'], c['manifest'], p['dest'], c['dest'])
    return {'status': 'FAIL' if bad else 'PASS',
            'summary': 'previous %d files in %.0f s, candidate %d files in %.0f s: %d differ, %d removed, %d new%s'
            % (len(p['manifest']), p['seconds'], len(c['manifest']), c['seconds'], len(diff), len(only_p), len(only_c),
               '; unexpected: %s' % ', '.join(bad[:8]) if bad else ' (only identity files differ)'), 'details': det}


def st_relay_installed(g):
    c = g.state.get('fresh')
    if not c or c['code']:
        return {'status': 'SKIP', 'summary': 'needs a fresh candidate install (ab_fresh)'}
    url = relay_expected(g)
    tot = {'expected': 0, 'forbidden': 0, 'empty': 0}; where = []
    for p in Path(c['dest']).rglob('*'):
        if p.is_file() and not is_link(p) and '_work' not in p.parts:
            try:
                cc = relay_counts(p.read_bytes(), url, g.cfg['relay_forbidden'])
            except OSError:
                continue
            if any(cc.values()):
                where.append('%s %s' % (p.relative_to(c['dest']), cc))
            for k in tot:
                tot[k] += cc[k]
    ok = (tot['expected'] > 0 if url else True) and not tot['forbidden'] and not tot['empty']
    return {'status': 'PASS' if ok else 'FAIL', 'summary': 'installed tree: relay %d, forbidden %d, empty relay_url %d' % (
        tot['expected'], tot['forbidden'], tot['empty']), 'details': where}


def scan_zip(path):
    sys.path.insert(0, str(REPO / 'tools'))
    import privacy_scan
    sys.path.pop(0)
    import zipfile
    pats = [p for p in privacy_scan.patterns() if p.pattern != re.escape(str(REPO))]
    hits, members = [], []
    with zipfile.ZipFile(path) as z:
        for m in z.namelist():
            members.append(m)
            data = z.read(m)
            for t in privacy_scan.texts(data):
                for rx in pats:
                    mm = rx.search(t)
                    if mm and not privacy_scan.ok_match(mm):
                        hits.append('%s: %s' % (m, mm.group(0)[:60])); break
    return members, hits


def st_diagnostics(g):
    c, exe = g.state.get('fresh'), need_installer(g)
    if not c or c['code'] or not exe:
        return {'status': 'SKIP', 'summary': 'needs a fresh candidate install (ab_fresh)'}
    r = g.install('diagnostics', exe, ['--diagnostics', '--dest', str(c['dest'])])
    zips = sorted(r['base'].rglob('MA2-diagnostics-*.zip'))
    if r['code'] or not zips:
        return {'status': 'FAIL', 'summary': '--diagnostics exit %d, %d zip(s)' % (r['code'], len(zips))}
    members, hits = scan_zip(zips[-1])
    return {'status': 'FAIL' if hits else 'PASS', 'summary': '--diagnostics exit 0 in %.0f s; zip members %s; privacy hits %d'
            % (r['seconds'], ', '.join(members), len(hits)), 'details': hits or None}


def st_update(g):
    exe = need_installer(g)
    miss = g.need('previous_install', 'xiso', 'ffmpeg')
    if not exe or miss:
        return {'status': 'SKIP', 'summary': 'needs the build and %s' % (miss or 'the installer')}
    src = Path(g.cfg['previous_install'])
    if not (src / 'recomp-install.json').is_file():
        return {'status': 'FAIL', 'summary': 'previous_install has no recomp-install.json'}
    e, base = g.env('update')
    dest = base / 'MechAssault 2'
    if dest.exists():
        shutil.rmtree(dest)
    t = time.time()
    links = copy_tree(src, dest)                         # a COPY: the kept install itself is only read
    pre = manifest(dest)
    say('   copied the previous install (%d files, %.0f s)%s' % (len(pre), time.time() - t, '; links left out: %s' % links if links else ''))
    r = g.install('update', exe, g.install_args() + ['--update', '--shortcuts', 'desktop,start,editor', '--low-priority'], dest=dest)
    post = manifest(dest) if dest.is_dir() else {}
    (g.logs / 'manifest-update-before.json').write_text(json.dumps(pre, indent=0), encoding='utf-8')
    (g.logs / 'manifest-update-after.json').write_text(json.dumps(post, indent=0), encoding='utf-8')
    g.state['update_dest'] = dest; g.state['update_first'] = r
    if r['code']:
        return {'status': 'FAIL', 'summary': 'update exit %d after %.0f s' % (r['code'], r['seconds']), 'details': tail(r.get('log', ''), 12)}
    stamp = json.loads((dest / 'recomp-install.json').read_text(encoding='utf-8'))
    want = json.loads((g.clone / 'kit/installer/data/release.json').read_text(encoding='utf-8'))['version']
    removed, added, diff = compare_manifests(pre, post)
    det = ['added: ' + k for k in added] + ['changed: ' + k for k in diff] + ['removed: ' + k for k in removed]
    bad = []
    if g.installer and re.fullmatch(re.escape(want) + r'-preview\.\d+', str(stamp.get('public_version'))):
        want = stamp.get('public_version')        # the gate run on a preview installer (--installer): it stamps X.Y.Z-preview.N
    if stamp.get('public_version') != want:
        bad.append('stamp says %s, not %s' % (stamp.get('public_version'), want))
    fresh = g.state.get('fresh')
    if fresh and fresh.get('manifest'):
        fm = fresh['manifest']
        missing = [k for k in fm if k not in post and not match_any(k, PLAYER_FILES)]
        other = [k for k in fm if k in post and fm[k] != post[k] and not match_any(k, IDENTITY + PLAYER_FILES + g.cfg['identity_files'])]
        if missing:
            bad.append('missing after the update (a fresh install has them): %s' % ', '.join(missing[:8]))
        det += ['vs a fresh install, differs: %s' % k for k in other]
        if other:
            bad.append('%d file(s) differ from a fresh install beyond identity and player files' % len(other))
    lnk =sorted(p.relative_to(base / 'lnk').as_posix() for p in (base / 'lnk').rglob('*.lnk'))
    det.append('shortcuts made (redirected): %s' % ', '.join(lnk))
    return {'status': 'FAIL' if bad else 'PASS', 'summary': 'update from the previous release exit 0 in %.0f s: %d added, %d changed, %d removed%s'
            % (r['seconds'], len(added), len(diff), len(removed), '; ' + '; '.join(bad) if bad else ''), 'details': det}


def capability(g, needle_rx):
    exe = need_installer(g)
    if not exe:
        return None
    files, _ = packed(exe)
    src = files.get('installer/install.py', b'').decode('utf-8', 'replace')
    return bool(re.search(needle_rx, src))


def install_log_text(r):
    parts = []
    try:
        parts.append(Path(r['log']).read_text(encoding='utf-8', errors='replace'))
    except (OSError, KeyError, TypeError):
        pass
    logs = sorted((Path(r['base']) / 'appdata' / 'MechAssault2' / 'Logs').glob('install-*.log'), key=lambda p: p.stat().st_mtime)
    if logs:
        parts.append(logs[-1].read_text(encoding='utf-8', errors='replace'))
    return '\n'.join(parts)


def st_fastpath(g):
    cap = capability(g, r'cache\.json')
    if cap is None:
        return {'status': 'SKIP', 'summary': 'needs the build'}
    if not cap:
        return {'status': 'SKIP', 'summary': 'not available in this kit (the installer keeps no update cache)'}
    dest, first = g.state.get('update_dest'), g.state.get('update_first')
    if not dest or not first or first['code']:
        return {'status': 'SKIP', 'summary': 'needs a finished update (step update)'}
    cache = Path(dest) / '_work' / 'cache.json'
    try:
        c = json.loads(cache.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {'status': 'FAIL', 'summary': 'the first update left no readable %s' % cache}
    if not c.get('complete'):
        return {'status': 'FAIL', 'summary': 'the first update left cache.json with complete=%r' % c.get('complete')}
    r = g.install('update2', need_installer(g), g.install_args() + ['--update'], dest=dest)
    fast = bool(re.search(g.cfg['fastpath_marker'], install_log_text(r)))
    g.state['update_second'] = r
    ok = r['code'] == 0 and fast
    return {'status': 'PASS' if ok else 'FAIL', 'summary': 'second update exit %d in %.0f s (first: %.0f s, %.0f%%); fast path %s'
            % (r['code'], r['seconds'], first['seconds'], 100.0 * r['seconds'] / max(first['seconds'], 1), 'taken' if fast else 'NOT taken')}


def st_fallback(g):
    cap = capability(g, r'cache\.json')
    if cap is None:
        return {'status': 'SKIP', 'summary': 'needs the build'}
    if not cap:
        return {'status': 'SKIP', 'summary': 'not available in this kit (the installer keeps no update cache)'}
    dest = g.state.get('update_dest')
    if not dest or not g.state.get('update_second'):
        return {'status': 'SKIP', 'summary': 'needs the fast-path update (step fastpath)'}
    r = g.install('update3', need_installer(g), g.install_args() + ['--update'], extra_env=g.cfg['fallback_env'], dest=dest)
    text = install_log_text(r)
    fast = bool(re.search(g.cfg['fastpath_marker'], text))
    clean = bool(re.search(g.cfg['fallback_marker'], text))
    exe_ok = (Path(dest) / 'MechAssault 2.exe').is_file()
    ok = r['code'] == 0 and not fast and clean and exe_ok
    return {'status': 'PASS' if ok else 'FAIL', 'summary': 'forced fingerprint change (%s): exit %d in %.0f s, fast path %s, game exe %s'
            % (', '.join(g.cfg['fallback_env']), r['code'], r['seconds'], 'taken (WRONG)' if fast else 'not taken (clean build)' if clean else 'not taken, but no clean-build line in the log',
               'present' if exe_ok else 'MISSING')}


SITECUSTOMIZE = r'''"""Release-gate fault injection (written by tools/release.py into an unpacked copy of the kit; never shipped)."""
import argparse, importlib, os, sys, threading
HIT = %(hit)r
FAULTS = %(faults)r
_lock = threading.Lock()
def _note(line):
    with _lock:
        with open(HIT, 'a', encoding='utf-8') as fh: fh.write(line + '\n')
def _boom(name, exc):
    def f(*a, **k):
        _note('HIT ' + name)
        raise exc('injected fault: ' + name)
    return f
_orig = argparse.ArgumentParser.parse_args
def _patch_then_parse(self, *a, **k):
    m = sys.modules.get('__main__')
    if m is not None and hasattr(m, 'Installer') and not getattr(m, '_GATE_PATCHED', False):
        m._GATE_PATCHED = True
        for f in FAULTS:
            try:
                mod = m if f['module'] == '__main__' else importlib.import_module(f['module'])
                *path, last = f['target'].split('.')
                obj = mod
                for p in path: obj = getattr(obj, p)
                getattr(obj, last)
                setattr(obj, last, _boom(f['id'], {'OSError': OSError}.get(f['exc'], RuntimeError)))
            except Exception as e:
                _note('ABSENT %%s (%%s)' %% (f['id'], type(e).__name__))
        _note('PATCHED pid %%d %%s' %% (os.getpid(), ' '.join(sys.argv[1:2])))
    return _orig(self, *a, **k)
argparse.ArgumentParser.parse_args = _patch_then_parse
'''


def hooked_kit(g, exe, temp, hit, faults):
    """Unpack the installer's kit into `temp` the way the stub does, plus the fault hooks; the stub then reuses it."""
    data = Path(exe).read_bytes()
    i = data.rfind(b'MA2KEND1')
    kid = data[i - 16:i].decode('ascii')
    kit = Path(temp) / ('MA2RecompInstaller-' + kid)
    if kit.exists():
        shutil.rmtree(kit)
    files, _ = packed(exe)
    for name, blob in files.items():
        p = kit / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(blob)
    sp = kit / 'python' / 'Lib' / 'site-packages'
    sp.mkdir(parents=True, exist_ok=True)
    (sp / 'sitecustomize.py').write_text(SITECUSTOMIZE % {'hit': str(hit), 'faults': faults}, encoding='utf-8')
    (kit / '.unpacked').write_bytes(b'')
    return kit


def st_fault(g):
    exe = need_installer(g)
    miss = g.need('xiso', 'ffmpeg')
    fj = g.clone / 'tests' / 'installer' / 'gate_faults.json'
    if not exe or miss:
        return {'status': 'SKIP', 'summary': 'needs the build and %s' % (miss or 'the installer')}
    if not fj.is_file():
        return {'status': 'SKIP', 'summary': 'not available in this kit (no tests/installer/gate_faults.json)'}
    faults = json.loads(fj.read_text(encoding='utf-8'))['faults']
    r = fault_run(g, exe, faults, 'fault')
    if r['not_fired']:
        # A fault that raises can stop the code before a later hooked method is reached (plan_reuse calls fingerprint,
        # reuse_mode and write_cache; copy_installer calls selfupdate.enabled). Each of those gets a hooked install of its own.
        alone = []
        for fid in list(r['not_fired']):
            one = fault_run(g, exe, [f for f in faults if f['id'] == fid], 'fault-' + re.sub(r'\W+', '_', fid))
            alone.append(one)
            r['det'].append('alone: %s: install exit %d, --diagnostics exit %d, %s%s' % (
                fid, one['code'], one['dcode'], 'fired' if not one['not_fired'] else 'NOT fired',
                '; other differences: ' + ', '.join(one['extra'][:8]) if one['extra'] else ''))
            if not one['not_fired']:
                r['not_fired'].remove(fid)
        r['bad'] = r['bad'] or any(o['code'] or o['dcode'] or o['patched'] < 2 or o['extra'] for o in alone)
    if r['not_fired']:
        r['det'].append('expected but not fired: ' + ', '.join(r['not_fired']))
    st = 'FAIL' if r['bad'] or r['not_fired'] else 'WARN' if (r['absent'] or r['extra']) else 'PASS'
    return {'status': st, 'summary': 'hooked install exit %d (%.0f s), hooked --diagnostics exit %d; %d of %d expected faults fired, %d absent'
            % (r['code'], r['seconds'], r['dcode'], r['want'] - len(r['not_fired']), r['want'], len(set(r['absent']))),
            'details': r['det']}


def fault_run(g, exe, faults, case):
    """One hooked install plus one hooked --diagnostics with these faults."""
    e, base = g.env(case)
    hit = base / 'faults-hit.txt'
    if hit.exists():
        hit.unlink()
    hooked_kit(g, exe, base / 'temp', hit, faults)
    r = g.install(case, exe, g.install_args() + ['--shortcuts', 'desktop,start,editor'])
    m = manifest(r['dest']) if Path(r['dest']).is_dir() else {}
    (g.logs / ('manifest-%s.json' % case)).write_text(json.dumps(m, indent=0), encoding='utf-8')
    _, dbase = g.env(case + '-diag')
    hooked_kit(g, exe, dbase / 'temp', hit, faults)                # the stub deleted the first copy when the install ended
    d = g.install(case + '-diag', exe, ['--diagnostics', '--dest', str(r['dest'])])
    lines = hit.read_text(encoding='utf-8').splitlines() if hit.is_file() else []
    fired = {l[4:] for l in lines if l.startswith('HIT ')}
    absent = [l[7:] for l in lines if l.startswith('ABSENT ')]
    patched = sum(1 for l in lines if l.startswith('PATCHED'))
    want = [f['id'] for f in faults if f['when'] in ('install', 'diagnostics', 'both')]
    not_fired = [w for w in want if w not in fired]
    det = ['fired: ' + ', '.join(sorted(fired)), 'hooked runs: %d' % patched]
    if absent:
        det.append('absent in this kit: ' + ', '.join(sorted(set(absent))))
    fresh = g.state.get('fresh')
    extra = []
    if fresh and fresh.get('manifest') and m:
        a, b, diff = compare_manifests(fresh['manifest'], m)
        # (the installer copy kept for later updates is a fail-safe extra: a faulted copy step leaves it out, as 1.1.6 did)
        extra = [k for k in a + b + diff if not match_any(k, BUILD_IDENTITY + FAULT_MAY_LACK)]
        det.append('vs the fresh candidate install: %d differ (%s)%s' % (len(diff), ', '.join(diff[:6]),
                                                                       '; other differences: %s' % ', '.join(extra[:8]) if extra else ''))
    return {'code': r['code'], 'dcode': d['code'], 'seconds': r['seconds'], 'patched': patched, 'want': len(want),
            'not_fired': not_fired, 'absent': absent, 'extra': extra, 'det': det,
            'bad': r['code'] != 0 or d['code'] != 0 or patched < 2}


STEPS = {                       # name: (function, modes, what it checks)
    'clone': (st_clone, 'task rc', 'a fresh clone of the commit under test (uncommitted changes are never tested)'),
    'versions': (st_versions, 'task rc', 'every version place agrees (VERSION is the source)'),
    'deps': (st_deps, 'task rc', 'Capstone (and FFmpeg for the tests) from local copies, hash-checked against the lock'),
    'build': (st_build, 'task rc', 'tools/build_release.py: editor, stub, report helper, pack, privacy check, SHA-256'),
    'privacy_tree': (st_privacy_tree, 'task rc', 'tools/privacy_scan.py over the tree'),
    'privacy_bin': (st_privacy_bin, 'task rc', 'privacy_scan --binaries: installer (+ every packed file), the RecompLauncher zip (every member), stub, editor, helper'),
    'grepbin': (st_grepbin, 'task rc', 'configured words + profile path + user/PC name in every binary, packed file and zip member (ASCII, UTF-16)'),
    'launcher_zip': (st_launcher_zip, 'task rc', 'the RecompLauncher zip: recomplauncher.json + the installer byte for byte, nothing else; '
                     'the profile parses (and passes the schema in recomplauncher_schema, when configured)'),
    'relay_packed': (st_relay_packed, 'task rc', 'the relay address from report.json is in the packed report.json and MA2 Report.exe; no staging or empty address'),
    'helper_build': (st_helper_build, 'task rc', 'the suites\' own report helper, built without any relay address'),
    'suites': (st_suites, 'task rc', 'every tests/installer/test_*.py'),
    'ui_flow': (st_ui_flow, 'task rc', 'every tests/installer/ui_flow*.py (installer windows, transparent and off screen)'),
    'relay_tests': (st_relay_tests, 'task rc', 'the relay\'s own tests (node) and its Python cross-checks against the kit'),
    'kit_diff': (st_kit_diff, 'task rc', 'packed files vs the previous release\'s installer (review list)'),
    'sheet': (st_sheet, 'task rc', 'the decision sheet for <base>..commit'),
    'signoff': (st_signoff, 'rc', 'the signed sheet still matches the changes since its base'),
    'ab_fresh': (st_ab_fresh, 'rc', 'fresh install with the previous and the new installer: same files except identity files'),
    'relay_installed': (st_relay_installed, 'rc', 'relay address in the installed tree (MA2 Report.exe); no staging or empty address'),
    'diagnostics': (st_diagnostics, 'rc', '--diagnostics on the new install: zip made, members, privacy scan of every member'),
    'update': (st_update, 'rc', 'update a COPY of the kept previous install: exit 0, stamp, nothing missing vs a fresh install'),
    'fastpath': (st_fastpath, 'rc', 'second update takes the fast path (when the kit has the update cache)'),
    'fallback': (st_fallback, 'rc', 'forced fingerprint change: clean build, working install (when the kit has the update cache)'),
    'fault': (st_fault, 'rc', 'fault injection (tests/installer/gate_faults.json) in a real install + --diagnostics'),
}
NEEDS = {'deps': ['clone'], 'versions': ['clone'], 'build': ['deps'], 'privacy_tree': ['clone'], 'privacy_bin': ['build'], 'grepbin': ['build'],
         'launcher_zip': ['build'],
         'relay_packed': ['build'], 'helper_build': ['clone'], 'suites': ['build', 'helper_build'], 'ui_flow': ['deps'],
         'relay_tests': ['clone'], 'kit_diff': ['build'], 'sheet': [], 'signoff': [], 'ab_fresh': ['build'], 'relay_installed': ['ab_fresh'],
         'diagnostics': ['ab_fresh'], 'update': ['build'], 'fastpath': ['update'], 'fallback': ['fastpath'], 'fault': ['build']}


def plan_steps(mode, only=None, skip=None, extra=None):
    order = list(STEPS)
    if only:
        want = set()
        def add(n):
            if n not in STEPS:
                raise SystemExit('unknown step %s (see `release.py steps`)' % n)
            if n not in want:
                want.add(n)
                for d in NEEDS.get(n, []):
                    add(d)
        for n in only:
            add(n)
        add('clone')
    else:
        want = {n for n in order if mode in STEPS[n][1].split()} | set(extra or [])
    want -= set(skip or [])
    return [n for n in order if n in want]


def cmd_gate(a):
    cfg = load_config(a.config)
    split = lambda s: [x.strip() for x in s.split(',') if x.strip()] if s else []
    g = Gate(a.mode, cfg, a.ref, a.installer, a.keep_installs)
    code = g.run(plan_steps(a.mode, split(a.only), split(a.skip), split(a.with_)))
    say('gate %s: %s -> %s' % (a.mode, g.overall(), g.dir / 'result.md'))
    return code


def cmd_steps(a):
    for n, (_, modes, what) in STEPS.items():
        say('%-16s %-8s %s' % (n, modes, what))
    return 0


# =====================================================================================================================
# preview and rc

def keep_install(cfg, exe, dest, label):
    """One kept install for the owner to try: hidden CLI, redirected appdata/temp/docs/shortcuts, no shortcuts."""
    dest = Path(dest)
    if dest.exists():
        raise SystemExit('%s exists: pick a new folder (this tool never replaces an install)' % dest)
    base = Path(cfg['work_dir']) / ('keep-' + label)
    e = {k: v for k, v in os.environ.items() if not k.upper().startswith(('MA2_', 'RECOMP_', 'PYTHON'))}
    for d in ('appdata', 'temp', 'docs', 'lnk'):
        (base / d).mkdir(parents=True, exist_ok=True)
    e.update(LOCALAPPDATA=str(base / 'appdata'), TEMP=str(base / 'temp'), TMP=str(base / 'temp'), MA2_DOCUMENTS_DIR=str(base / 'docs'),
             MA2_SHORTCUT_DIR=str(base / 'lnk'), MA2_REPORT_RELAY='http://127.0.0.1:%d' % closed_port())
    if wait_mutex(cfg['wait_for_installer_minutes'] * 60) is None:
        raise SystemExit('another installer kept running')
    code, secs = run_logged([exe, '--xiso', cfg['xiso'], '--dest', str(dest), '--ffmpeg', cfg['ffmpeg']], base / 'out.txt', env=e,
                            timeout=cfg['install_timeout_minutes'] * 60)
    say('kept install %s: exit %d in %.0f s (log %s)' % (dest, code, secs, base / 'out.txt'))
    return code


def cmd_preview(a):
    cfg = load_config(a.config)
    head = git('rev-parse', 'HEAD').strip()
    core = CORE_RX.match(read_versions(lambda p: show('HEAD', p)).get('VERSION') or '')
    if not core or core.group(4):
        raise SystemExit('VERSION must be X.Y.Z at HEAD')
    ver0 = '.'.join(core.groups()[:3])
    root = Path(cfg['work_dir'])
    n = a.n or 1 + max([int(m.group(1)) for p in root.glob('preview-%s-preview.*' % ver0)
                        for m in [re.search(r'preview\.(\d+)$', p.name)] if m] or [0])
    ver = '%s-preview.%d' % (ver0, n)
    out = root / ('preview-' + ver)
    if out.exists():
        raise SystemExit('%s exists' % out)
    clone = out / 'repo'
    say('preview %s from %s -> %s' % (ver, head[:12], out))
    for cmd, cwd in ((['git', 'clone', '--quiet', str(REPO), str(clone)], None),
                     (['git', '-c', 'advice.detachedHead=false', 'checkout', '--quiet', head], clone)):
        if run_logged(cmd, out / 'logs' / 'git.txt', cwd=cwd)[0]:
            raise SystemExit('git failed (%s)' % (out / 'logs' / 'git.txt'))
    stamp_versions(clone, ver, strings_only=True)
    for cmd in (['git', 'add', '-u'], ['git', '-c', 'user.name=preview', '-c', 'user.email=preview@localhost', 'commit', '-q', '-m',
                                        'Preview %s (version strings only)' % ver]):
        if run_logged(cmd, out / 'logs' / 'stamp.txt', cwd=clone)[0]:
            raise SystemExit('stamp commit failed')
    args = [sys.executable, 'tools/fetch_deps.py', '--from', 'capstone=%s' % cfg['capstone']]
    if run_logged(args, out / 'logs' / 'deps.txt', cwd=clone)[0]:
        raise SystemExit('fetch_deps failed (%s)' % (out / 'logs' / 'deps.txt'))
    code, secs = run_logged([sys.executable, 'tools/build_release.py', '--build-dir', cfg['build_dir']], out / 'logs' / 'build.txt', cwd=clone)
    exe = clone / 'dist' / INSTALLER_NAME
    if code or not exe.is_file():
        raise SystemExit('build failed (%s)' % (out / 'logs' / 'build.txt'))
    final = out / INSTALLER_NAME
    shutil.copy2(exe, final)
    h, size = sha_file(final), final.stat().st_size
    zips = sorted((clone / 'dist').glob('MechAssault2-PC-Recomp-v*-Windows-x64.zip'))     # the RecompLauncher zip (build_release step 6)
    zfinal = out / zips[-1].name if zips else None
    if zfinal:
        shutil.copy2(zips[-1], zfinal)
    (out / 'SHA256SUMS.txt').write_text(''.join('%s *%s\n' % (sha_file(f), f.name) for f in (final, zfinal) if f), encoding='ascii')
    assets = '"%s" %s"%s"' % (final, '"%s" ' % zfinal if zfinal else '', out / 'SHA256SUMS.txt')
    base = a.base or cfg.get('base')
    sheet = None
    if base:
        sheet, fh, sh = write_sheet(base, head, a.notes or cfg.get('notes'), out / ('sheet-%s.md' % safe_ref(base)))
    notes = out / 'prerelease-notes-draft.md'
    notes.write_text('\n'.join([
        '# MechAssault 2: Lone Wolf Expanded %s (preview)' % ver, '',
        'This is a preview of %s for testing. It may change before the release. Install it into a new folder, not over '
        'your normal install.' % ver0, '',
        '- `%s`: %d bytes' % (INSTALLER_NAME, size),
        '- SHA-256: `%s`' % h, '']), encoding='utf-8')
    gh = out / 'gh-release-command.txt'
    gh.write_text('\n'.join([
        'NOT RUN BY THE TOOL. Only after the owner says yes, and after the branch with commit %s is on GitHub:' % head, '',
        'gh release create "v%s" %s --prerelease --target %s --title "MechAssault 2: Lone Wolf Expanded %s (preview)" --notes-file "%s"'
        % (ver, assets, head, ver, notes), '']), encoding='utf-8')
    say('preview installer %s\n  %d bytes sha256 %s\nnotes draft %s\ncommand file %s%s'
        % (final, size, h, notes, gh, '\nsheet %s (signed part %s)' % (sheet, sh) if sheet else ''))
    if a.keep_install:
        return keep_install(cfg, final, a.keep_install, 'preview-' + ver)
    return 0


def cmd_rc(a):
    cfg = load_config(a.config)
    if git('status', '--porcelain', '--untracked-files=no').strip():
        raise SystemExit('refusing: the checkout has uncommitted changes (an RC is built from commits only)')
    ok, msg, rec = signoff_status('HEAD')
    say(msg)
    if not ok:
        raise SystemExit('refusing to build the release candidate: ' + msg)
    cfg.setdefault('base', rec['base'])
    if rec.get('notes'):
        cfg.setdefault('notes', rec['notes'])
    g = Gate('rc', cfg, 'HEAD')
    code = g.run(plan_steps('rc'))
    exe = g.state.get('installer')
    if exe and Path(exe).is_file():
        ver = g.state.get('version') or 'rc'
        out = Path(cfg['work_dir']) / ('rc-%s-%s' % (ver, g.sha[:8]))
        out.mkdir(parents=True, exist_ok=True)
        shutil.copy2(exe, out / INSTALLER_NAME)
        files = [out / INSTALLER_NAME]
        if need_zip(g):                           # the RecompLauncher zip goes up next to the exe
            shutil.copy2(need_zip(g), out / need_zip(g).name); files.append(out / need_zip(g).name)
        (out / 'SHA256SUMS.txt').write_text(''.join('%s *%s\n' % (sha_file(f), f.name) for f in files), encoding='ascii')
        shutil.copy2(g.dir / 'result.md', out / 'gate-result.md'); shutil.copy2(g.dir / 'result.json', out / 'gate-result.json')
        say('release candidate %s (gate %s)' % (out / INSTALLER_NAME, g.overall()))
        if code == 0 and a.keep_install:
            return keep_install(cfg, out / INSTALLER_NAME, a.keep_install, 'rc-%s' % ver)
    return code


# =====================================================================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('versions'); p.add_argument('--set'); p.add_argument('--date'); p.set_defaults(fn=cmd_versions)
    p = sub.add_parser('sheet'); p.add_argument('base'); p.add_argument('--head', default='HEAD'); p.add_argument('--notes')
    p.add_argument('--out'); p.set_defaults(fn=cmd_sheet)
    p = sub.add_parser('sign'); p.add_argument('sheet'); p.add_argument('--by'); p.set_defaults(fn=cmd_sign)
    p = sub.add_parser('preview'); p.add_argument('--config'); p.add_argument('--n', type=int); p.add_argument('--base')
    p.add_argument('--notes'); p.add_argument('--keep-install'); p.set_defaults(fn=cmd_preview)
    p = sub.add_parser('rc'); p.add_argument('--config'); p.add_argument('--keep-install'); p.set_defaults(fn=cmd_rc)
    p = sub.add_parser('gate'); p.add_argument('mode', choices=['task', 'rc']); p.add_argument('--config')
    p.add_argument('--only'); p.add_argument('--skip'); p.add_argument('--with', dest='with_'); p.add_argument('--ref', default='HEAD')
    p.add_argument('--installer'); p.add_argument('--keep-installs', action='store_true'); p.set_defaults(fn=cmd_gate)
    p = sub.add_parser('steps'); p.set_defaults(fn=cmd_steps)
    a = ap.parse_args()
    sys.exit(a.fn(a))


if __name__ == '__main__':
    main()

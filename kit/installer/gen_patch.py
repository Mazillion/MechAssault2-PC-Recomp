"""Patch set for the generated code: from the deterministic toolkit output to the shipped sources.

The xboxrecomp pipeline turns the user's ma2.xbe into C (tools.recomp plus the two bringup
generators). The PC release then carries a small set of hand fixes and PC features on top.
This module records and replays those edits, strictly. The patch files hold only the lines the
release WROTE and where they go; none of the generated code (no context, no removed line, no
moved or repeated line) is in the kit, so the kit carries nothing recompiled from the game:

  make   base_dir final_dir patch_dir   write diff/<file>.json for every changed file, copy files
                                        that exist only in final_dir, and manifest.json
                                        (base and final SHA-256 of every file). Output that is
                                        not part of the new patch set (an old diff/*.diff) is removed.
  apply  gen_dir patch_dir              check each base file's SHA-256, replay each file's edits,
                                        add the extra files, then check each result's SHA-256

A diff/<file>.json is a list of edits [base_start, delete_count, [new lines]]: replace
delete_count lines of the base file, starting at line index base_start, by the new lines.
A new line is either text the release wrote, or a reference to a line of the BASE file
(format edits-2): an integer (that line as it is) or [index, indent] (that line with its
leading whitespace replaced by indent). When an edit moves, re-indents or repeats a line that
the toolkit generated, the patch stores only that line number and apply copies the line from
the user's own generated file. apply replays the edits from the end of the file backwards, so earlier indexes stay valid.

A base mismatch means the XBE or the toolkit is not the one this release was made from; the
installer stops instead of building something different. Line endings are kept byte for byte
(lines are read as Latin-1 with their line ends).
"""
import difflib, hashlib, json, os, shutil, sys
from pathlib import Path

FORMAT = 'edits-2'
FORMATS = ('edits-1', 'edits-2')         # edits-1: text lines only (older kits)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def lines(path):
    return Path(path).read_bytes().decode('latin-1').splitlines(keepends=True)

def trivial(line):
    """Blank lines and lone braces are not worth a reference."""
    return len(line.strip()) <= 2

def to_refs(a, ops):
    """Replace every new line that already exists in base file a (as it is, or re-indented) by a reference
    to its nearest copy in a."""
    where, body = {}, {}
    for i, l in enumerate(a):
        if not trivial(l):
            where.setdefault(l, []).append(i)
            body.setdefault(l.lstrip(' 	'), []).append(i)
    near = lambda c, start: min(c, key=lambda i: (abs(i - start), i))
    out = []
    for start, count, added in ops:
        new = []
        for x in added:
            if isinstance(x, str) and x in where:
                x = near(where[x], start)
            elif isinstance(x, str) and x.lstrip(' 	') in body:
                x = [near(body[x.lstrip(' 	')], start), x[:len(x) - len(x.lstrip(' 	'))]]
            new.append(x)
        out.append([start, count, new])
    return out

def edits(a, b):
    """[[base_start, delete_count, [new lines or base indexes]], ...] turning line list a into line list b."""
    out = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b).get_opcodes():
        if tag != 'equal': out.append([i1, i2 - i1, b[j1:j2]])
    return to_refs(a, out)

def replay(base_lines, ops, name='?'):
    """The patched text: ops applied from the end of the file backwards."""
    out = list(base_lines)
    end = len(out) + 1
    for start, count, added in reversed(ops):
        if not (0 <= start and count >= 0 and start + count <= len(base_lines) and start + count <= end):
            raise ValueError('%s: edit [%d, %d] does not fit the base file (%d lines)' % (name, start, count, len(base_lines)))
        new = []
        for x in added:
            if not isinstance(x, str):
                i, lead = (x, None) if isinstance(x, int) else x
                if not 0 <= i < len(base_lines):
                    raise ValueError('%s: line reference %d is outside the base file (%d lines)' % (name, i, len(base_lines)))
                x = base_lines[i] if lead is None else lead + base_lines[i].lstrip(' 	')
            new.append(x)
        out[start:start + count] = new
        end = start
    return ''.join(out)

def make(base_dir, final_dir, patch_dir):
    base_dir, final_dir, patch_dir = Path(base_dir), Path(final_dir), Path(patch_dir)
    if patch_dir.exists(): shutil.rmtree(patch_dir)
    (patch_dir / 'diff').mkdir(parents=True); (patch_dir / 'add').mkdir()
    manifest = {'format': FORMAT, 'base': {}, 'final': {}, 'patched': [], 'added': []}
    changed = 0
    for f in sorted(os.listdir(base_dir)):
        manifest['base'][f] = sha(base_dir / f)
    for f in sorted(os.listdir(final_dir)):
        manifest['final'][f] = sha(final_dir / f)
        if f not in manifest['base']:
            shutil.copy2(final_dir / f, patch_dir / 'add' / f); manifest['added'].append(f); continue
        if manifest['base'][f] == manifest['final'][f]: continue
        a, b = lines(base_dir / f), lines(final_dir / f)
        ops = edits(a, b)
        if replay(a, ops, f) != ''.join(b): raise SystemExit('%s: the edit list does not reproduce the final file' % f)
        (patch_dir / 'diff' / (f + '.json')).write_text(json.dumps(ops, separators=(',', ':')), encoding='ascii')   # ensure_ascii: Latin-1 as \u00xx
        manifest['patched'].append(f); changed += 1
    removed = sorted(set(manifest['base']) - set(manifest['final']))
    manifest['removed'] = removed
    (patch_dir / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    for stale in patch_dir.rglob('*.diff'): stale.unlink()                 # nothing in the old unified-diff form may remain
    print('patch set: %d patched, %d added, %d removed, %d unchanged' % (
        changed, len(manifest['added']), len(removed), len(manifest['final']) - changed - len(manifest['added'])))

def convert(base_dir, patch_dir):
    """Rewrite an edits-1 patch set in place as edits-2 (base lines become references); returns the count."""
    base_dir, patch_dir = Path(base_dir), Path(patch_dir)
    manifest = json.loads((patch_dir / 'manifest.json').read_text(encoding='utf-8'))
    n = 0
    for f in manifest['patched']:
        if sha(base_dir / f) != manifest['base'][f]:
            raise SystemExit('%s: base file does not match the patch set' % f)
        j = patch_dir / 'diff' / (f + '.json')
        a = lines(base_dir / f)
        ops = json.loads(j.read_text(encoding='ascii'))
        new = to_refs(a, ops)
        if replay(a, new, f) != replay(a, ops, f): raise SystemExit('%s: conversion changed the result' % f)
        n += sum(1 for e0, e1 in zip(ops, new) for x, y in zip(e0[2], e1[2]) if x != y)
        j.write_text(json.dumps(new, separators=(',', ':')), encoding='ascii')
    manifest['format'] = FORMAT
    (patch_dir / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    return n

def base_mismatches(gen_dir, patch_dir):
    """Files of the patch set whose generated text is neither the release's base nor its patched result."""
    gen_dir, patch_dir = Path(gen_dir), Path(patch_dir)
    manifest = json.loads((patch_dir / 'manifest.json').read_text(encoding='utf-8'))
    final = manifest['final']
    return [f for f, h in manifest['base'].items()
            if not (gen_dir / f).exists() or sha(gen_dir / f) not in (h, final.get(f))]

def apply(gen_dir, patch_dir, log=print):
    """Replay the patch set. Each file is written to <file>.tmp and renamed, so a run that is stopped part-way
    leaves every file either as generated or fully patched; a later apply skips the files that already equal
    their patched result (resume)."""
    gen_dir, patch_dir = Path(gen_dir), Path(patch_dir)
    manifest = json.loads((patch_dir / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('format') not in FORMATS:
        raise SystemExit('This patch set is in an unknown format (%r). The installer and its patches do not belong together.' % manifest.get('format'))
    final = manifest['final']
    state = {f: (sha(gen_dir / f) if (gen_dir / f).exists() else None) for f in manifest['base']}
    done = [f for f, h in state.items() if h is not None and h != manifest['base'][f] and h == final.get(f)]
    bad = [f for f, h in state.items() if h != manifest['base'][f] and f not in done]
    if bad:
        if done:
            raise SystemExit('The generated code was left part-patched by an installer run that was stopped (%d of %d files '
                             'differ, e.g. %s). Run the installer again: it translates the game program again first.' %
                             (len(bad), len(manifest['base']), ', '.join(bad[:3])))
        raise SystemExit('The generated code does not match this release (%d of %d files differ, e.g. %s). '
                         'The disc image must be the retail MechAssault 2 this release supports.' %
                         (len(bad), len(manifest['base']), ', '.join(bad[:3])))
    def put(target, data):
        tmp = target.with_name(target.name + '.tmp')
        tmp.write_bytes(data); os.replace(tmp, target)
    for f in manifest['patched']:
        if f in done: continue
        ops = json.loads((patch_dir / 'diff' / (f + '.json')).read_text(encoding='ascii'))
        put(gen_dir / f, replay(lines(gen_dir / f), ops, f).encode('latin-1'))
    for f in manifest['added']:
        put(gen_dir / f, (patch_dir / 'add' / f).read_bytes())
    for f in manifest.get('removed', []):
        if (gen_dir / f).exists(): (gen_dir / f).unlink()
    wrong = [f for f, h in final.items() if not (gen_dir / f).exists() or sha(gen_dir / f) != h]
    if wrong:
        raise SystemExit('Patched files do not match the release (%s).' % ', '.join(wrong[:5]))
    log('Generated code patched: %d files edited, %d added; all %d verified%s.' % (
        len(manifest['patched']), len(manifest['added']), len(final),
        ' (%d already patched by an earlier run)' % len(done) if done else ''))

if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'make' and len(sys.argv) == 5: make(*sys.argv[2:])
    elif cmd == 'apply' and len(sys.argv) == 4: apply(*sys.argv[2:])
    elif cmd == 'convert' and len(sys.argv) == 4: print('%d lines now copied from the base file' % convert(*sys.argv[2:]))
    else: sys.exit(__doc__)

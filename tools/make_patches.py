"""Turn your edits of the translated game code into the kit's patch set (kit/patches).

    py -3.13 tools/make_patches.py --base <pristine gen> --edited <your gen> [--out kit/patches]

--base    the toolkit's output before any patch: run the installer with `--keep-work --until generate`
          and copy <install>/_work/toolkit/MechAssault2/src/recomp/gen somewhere safe.
--edited  your working copy of the generated code with your changes, as built (it may contain the
          FastCode copies recomp_fast_*.c and RECOMP_FAST_ENTRY lines; they are handled here).

What it writes (nothing from the translated code itself: see docs/patches.md):
  <out>/diff/*.json, <out>/add/*, <out>/manifest.json   the edits, as line number + the lines you wrote; a
                                                         generated line you moved or re-indented is stored
                                                         as its line number in --base (format edits-2)
  <out>/fastcode/fastcode.json                           SHA-256 of every file FastCode will generate
  <out>/fastcode/post/                                   only if a FastCode copy needs a hand edit that
                                                         `force` cannot express (normally empty edits)
Then it replays the whole install path (patch + FastCode) on a copy of --base and checks that the result
equals --edited byte for byte (line endings aside). Any line it would store that looks like translated
code without one of our ma2_ hooks is reported, so you can decide whether it belongs in the PC layer.
"""
import argparse, hashlib, json, re, shutil, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'kit' / 'installer'))
import gen_patch  # noqa: E402

ENTRY = re.compile(rb'^    RECOMP_FAST_ENTRY\(sub_[0-9A-F]{8}\);\r?\n', re.M)
GENLIKE = re.compile(r'\b(MEM(8|16|32)|PUSH32|POP32)\(|\bloc_[0-9A-F]{8}\b')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def lf(b):
    return b.replace(b'\r\n', b'\n')


def pre_fastcode(edited, base, out):
    """The edited tree as it was before FastCode ran (no recomp_fast_*.c, no RECOMP_FAST_ENTRY lines),
    with each file's line endings matched to the base file (editors and git may change them)."""
    shutil.copytree(edited, out)
    for f in out.glob('recomp_fast_*.c'):
        f.unlink()
    for f in sorted(out.iterdir()):
        b = f.read_bytes()
        nb = ENTRY.sub(b'', b) if f.name.startswith('recomp_') and f.suffix == '.c' else b
        if (base / f.name).is_file():
            nb = lf(nb)
            if b'\r\n' in (base / f.name).read_bytes():
                nb = nb.replace(b'\n', b'\r\n')
        if nb != b:
            f.write_bytes(nb)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', required=True)
    ap.add_argument('--edited', required=True)
    ap.add_argument('--out', default=str(REPO / 'kit' / 'patches'))
    ap.add_argument('--max-flagged', type=int, default=250,
                    help='stop if more stored lines than this look like translated code (default 250)')
    a = ap.parse_args()
    base, edited, out = Path(a.base), Path(a.edited), Path(a.out)
    fc_src = REPO / 'kit' / 'patches' / 'fastcode'
    spec_old = json.loads((fc_src / 'fastcode.json').read_text(encoding='utf-8'))
    whole, force = spec_old['whole'], spec_old.get('force', [])
    tmp = Path(tempfile.mkdtemp(prefix='ma2patch-'))
    try:
        # 1. the patch set proper
        pre_fastcode(edited, base, tmp / 'final')
        gen_patch.make(base, tmp / 'final', tmp / 'patches')
        flagged = []
        for j in sorted((tmp / 'patches' / 'diff').glob('*.json')):
            for e in json.loads(j.read_text(encoding='ascii')):
                flagged += ['%s:%d %s' % (j.stem, e[0], l.strip()[:100]) for l in e[2]
                            if isinstance(l, str) and GENLIKE.search(l) and 'ma2_' not in l and 'pc_' not in l]
        if len(flagged) > a.max_flagged:
            raise SystemExit('%d stored lines look like translated game code (limit %d): the patch set would carry '
                             'game code. Is --base the right toolkit output? Nothing was written.'
                             % (len(flagged), a.max_flagged))
        fc = tmp / 'patches' / 'fastcode'
        fc.mkdir()
        for n in ('fastcode.py', 'fastcode_hot.txt'):
            shutil.copy2(fc_src / n, fc / n)
        hot = set()
        for l in (fc / 'fastcode_hot.txt').read_text().splitlines():
            l = l.split('#')[0].split()
            if l:
                hot.add(l[0])
        inputs = whole + sorted(hot - set(whole))
        # 2. replay the installer: patch, LF the FastCode inputs, run FastCode
        gen = tmp / 'gen'
        shutil.copytree(base, gen)
        gen_patch.apply(gen, tmp / 'patches')
        for nn in inputs:
            p = gen / ('recomp_%s.c' % nn)
            p.write_bytes(lf(p.read_bytes()))
        before = {f.name: sha(f) for f in gen.iterdir()}
        cmd = [sys.executable, '-I', str(fc / 'fastcode.py'), str(gen)] + whole + ['--hot', str(fc / 'fastcode_hot.txt')]
        for n in force:
            cmd += ['--force', n]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('fastcode.py failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])

        def differ():
            return [f.name for f in sorted(gen.iterdir()) if (edited / f.name).exists()
                    and lf(f.read_bytes()) != lf((edited / f.name).read_bytes())]
        bad = differ()
        if bad:                                   # hand edits inside FastCode output: strict post patch
            pb, pf = tmp / 'pb', tmp / 'pf'
            pb.mkdir(); pf.mkdir()
            for f in bad:
                cur, want = (gen / f).read_bytes(), lf((edited / f).read_bytes())
                if b'\r\n' in cur:
                    want = want.replace(b'\n', b'\r\n')
                shutil.copy2(gen / f, pb / f); (pf / f).write_bytes(want)
            gen_patch.make(pb, pf, fc / 'post')
            gen_patch.apply(gen, fc / 'post')
            for j in sorted((fc / 'post' / 'diff').glob('*.json')):
                for e in json.loads(j.read_text(encoding='ascii')):
                    flagged += ['post/%s:%d %s' % (j.stem, e[0], l.strip()[:100]) for l in e[2]
                                if isinstance(l, str) and GENLIKE.search(l) and 'ma2_' not in l]
            bad = differ()
        missing = sorted(f.name for f in edited.iterdir() if f.is_file() and not (gen / f.name).exists())
        if bad or missing:
            raise SystemExit('the replay does not reproduce --edited: differ %s, missing %s' % (bad[:8], missing[:8]))
        after = {f.name: sha(f) for f in gen.iterdir()}
        spec = {'whole': whole, 'hot': 'fastcode_hot.txt', 'force': force, 'lf': ['recomp_%s.c' % n for n in inputs],
                'final': {f: after[f] for f in sorted(after) if before.get(f) != after[f]}}
        if (fc / 'post').exists():
            spec['post'] = 'post'
        (fc / 'fastcode.json').write_text(json.dumps(spec, indent=1) + '\n', encoding='utf-8')
        # 3. publish
        if out.exists():
            shutil.rmtree(out)
        shutil.copytree(tmp / 'patches', out)
        n_added = sum(sum(isinstance(l, str) for l in e[2]) for j in (out / 'diff').glob('*.json') for e in json.loads(j.read_text(encoding='ascii')))
        print('patch set written to %s: %d files patched, %d lines added; replay reproduces --edited exactly'
              % (out, len(list((out / 'diff').glob('*.json'))), n_added))
        if flagged:
            print('%d stored line(s) look like translated code without an ma2_/pc_ hook (review them):' % len(flagged))
            for l in flagged[:40]:
                print('  ' + l)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()

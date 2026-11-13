#!/usr/bin/env python3
"""fastcode.py - T1-2 FastCode generator (pipeline Track 1, O1 + O2b).

    python tools/fastcode.py <gen-dir> <nnnn> [<nnnn> ...] [--hot tools/fastcode_hot.txt]

T1-7 (--hot): a file named only in the hot list ('<nnnn> sub_X' lines, from
the game-thread sampling profile) gets fast copies of the listed functions
only; the rest of that file keeps its original bodies with no entry line.

For every listed gen file recomp_<nnnn>.c (the patched gen C, LF) it:
  * writes recomp_fast_<nnnn>.c: a copy of the file in which every eligible
    `void sub_X(void)` becomes `void sub_X__fast(void)`, compiled with the
    plain guest pointer (no tank-scope check) and a function-local copy of
    g_xbox_mem_offset (O1), and the inline flat-table ICALL probe (O2b).
    Direct calls `sub_Y()` and `RECOMP_ABI_CALL(va, sub_Y)` to another fast
    function go straight to `sub_Y__fast`. Ineligible functions are left out
    (their preprocessor lines are kept, so the fp_* macro state matches).
  * rewrites recomp_<nnnn>.c in place with one `RECOMP_FAST_ENTRY(sub_X);`
    line at the top of each eligible function: with FastCode=0 the original
    body runs exactly as before; with FastCode=1 it tail-calls the fast copy.
Idempotent: existing RECOMP_FAST_ENTRY lines are stripped first.

Deny (the function keeps its original body only): files 0094-0098; the
FPU-fix functions; bodies with ma2_/MA2_ hooks, setjmp/longjmp, SEH bridges,
MMIO helpers or save probes; *_unused_lift and any non-`sub_X(void)` function.
Self-check: each fast body, with the renames undone and the prologue removed,
must equal the original body byte for byte (same guest memory-op sequence).

T1-3/T1-4 tier 2 (register promotion, O4a + O11), compiled only when
RECOMP_FAST_REGS is 1 (0 = tier 1, the same tokens as before): the guest GPRs
a fast body names become locals (_r_eax ...), loaded at entry. Every "sync
line" (a call, ICALL, ITAIL, return, any g_<reg> access, any call not on the
PURE list) is wrapped: publish the locals to the TLS registers, switch the
register names back to the TLS globals for that line (recomp_fast_rg.h), then
switch back (recomp_fast_rl.h) and reload. A body whose sync line is not a
whole statement stays at tier 1 (deny). The x87 stack, XMM/MMX, flags and ebp
are unchanged. tools/fastcode_verify.py checks the preprocessed result: no
TLS register access and no impure call outside a sync region.
"""
import re, sys, os, hashlib

REGTOK = re.compile(r'\b(eax|ecx|edx|esp|ebx|esi|edi)\b')
GREG = re.compile(r'\bg_(eax|ecx|edx|esp|ebx|esi|edi)\b')
CALLTOK = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
PURE = re.compile(r'^(MEM\w*|SMEM\w*|MMX_\w+|LO8|HI8|LO16|SET_\w+|ZX\d+|SX\d+|PUSH32|POP32|PUSH16|POP16|'
                  r'CMP_\w+|TEST_\w+|XMM_\w+|fp_\w+|RECOMP_FP_PC|RECOMP_DF_STEP|RECOMP_FCMP|RECOMP_FCMP_CC|'
                  r'RECOMP_PARITY8|XBOX_PTR|ROL32|ROR32|RC_ROT|sqrt|sqrtf|fabs|sin|cos|tan|atan2|log2|ldexp|'
                  r'floor|ceil|memcpy|memset|memmove|if|for|while|switch|sizeof)$')
LABEL = re.compile(r'\b(loc_[0-9A-Fa-f]+|case\b[^:]*|default)\s*:(?!:)')
STMT_END = (';', '{', '}')
REGS = ('eax', 'ecx', 'edx', 'esp', 'ebx', 'esi', 'edi')


def _mode(regs, to_local):
    return ''.join('#undef %s\n#define %s %s%s\n' % (r, r, '_r_' if to_local else 'g_', r) for r in regs)


def promote(fbody):
    """Tier 2 text for one fast body, or (None, reason). Only adds lines that
    are empty at RECOMP_FAST_REGS 0 (preprocessor lines inside #if blocks)."""
    lines = fbody.splitlines(True)
    st, codes, kinds, cont = [False], [], [], False
    for l in lines:
        c = strip_code(l, st)
        if cont or l.lstrip().startswith('#'):
            kinds.append('dir'); cont = l.rstrip('\n').endswith('\\'); codes.append(''); continue
        codes.append(c)
        if not c.strip():
            kinds.append('blank'); continue
        sync = bool(GREG.search(c)) or re.search(r'\breturn\b', c) is not None or \
            any(not PURE.match(m.group(1)) for m in CALLTOK.finditer(c))
        kinds.append('sync' if sync else 'code')
    used = sorted({m.group(1) for c, k in zip(codes, kinds) if k == 'code' for m in REGTOK.finditer(c)},
                  key=REGS.index)
    if not used:
        return None, 'noregs'
    prev = '{'
    for c, k in zip(codes, kinds):
        if k in ('dir', 'blank'):
            continue
        if k == 'sync':
            if LABEL.search(c):
                return None, 'sync-label'
            if not prev.endswith(STMT_END) or not c.rstrip().endswith(STMT_END):
                return None, 'sync-stmt'
        prev = c.rstrip()
    if not prev.endswith(STMT_END):
        return None, 'tail'
    pub = '    RP_VB ' + ' '.join('g_%s = _r_%s;' % (r, r) for r in used) + '\n'
    load = '    ' + ' '.join('_r_%s = g_%s;' % (r, r) for r in used) + ' RP_VE\n'
    out = ['#if RECOMP_FAST_REGS\n    RP_VB uint32_t ' + ', '.join('_r_%s = g_%s' % (r, r) for r in used) +
           '; RP_VE\n' + _mode(used, True) + '#endif\n']
    nsync = 0
    for l, k in zip(lines, kinds):
        if k != 'sync':
            out.append(l); continue
        nsync += 1
        out += ['#if RECOMP_FAST_REGS\n', pub, _mode(used, False), '#endif\n', l,
                '#if RECOMP_FAST_REGS\n', _mode(used, True), load, '#endif\n']
    out += ['#if RECOMP_FAST_REGS\n', pub.rstrip('\n') + ' RP_VE\n', _mode(used, False), '#endif\n']
    return ''.join(out), 'ok:%d:%d' % (len(used), nsync)

DENY_FILES = {'0094', '0095', '0096', '0097', '0098'}
DENY_FUNCS = {'sub_0030C659', 'sub_0030C709', 'sub_0031DF5C'}
DENY_BODY = re.compile(r'\bma2_|\bMA2_|setjmp|longjmp|__try|__except|recomp_seh|'
                       r'mmio|MMIO|xbox_save_probe|ma2_tank_step_scope|\b_mo\b|__fast\b')
HDR = re.compile(r'^void (sub_[0-9A-F]{8})\(void\)$')
ANYFN = re.compile(r'^[A-Za-z_][\w \*]*\b([A-Za-z_]\w*)\s*\([^;]*\)\s*$')
ENTRY = re.compile(r'^    RECOMP_FAST_ENTRY\(sub_[0-9A-F]{8}\);\n')
ONELINE_DEF = re.compile(r'^(?!static\b)[A-Za-z_][\w \*]*\b[A-Za-z_]\w*\s*\([^;{]*\)\s*\{.*\}\s*$')
PROLOGUE = '    RECOMP_FAST_PROLOGUE\n'
CALL1 = re.compile(r'\b(sub_[0-9A-F]{8})\(\)')
CALL2 = re.compile(r'(RECOMP_ABI_CALL\(0x[0-9A-Fa-f]+u?, )(sub_[0-9A-F]{8})\)')
PREAMBLE_MARK = '#include "recomp_funcs.h"\n'
FAST_BLOCK = (
    '/* T1-2 FastCode copy (tools/fastcode.py): runs only when\n'
    ' * g_recomp_fastcode is set. Plain guest pointer (the tank step swaps the\n'
    ' * clock block in guest memory instead), local memory offset, inline\n'
    ' * flat-table ICALL probe. Generated; do not edit. */\n'
    '#undef XBOX_PTR\n'
    '#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + _mo)\n'
    '#undef RECOMP_ICALL_RESOLVE\n'
    '#define RECOMP_ICALL_RESOLVE(_fn, _va) RECOMP_ICALL_RESOLVE_FLAT(_fn, _va)\n'
    '#define RECOMP_FAST_PROLOGUE const ptrdiff_t _mo = g_xbox_mem_offset; (void)_mo;\n'
    '#include "recomp_fast_cfg.h" /* RECOMP_FAST_REGS: tier 2 register promotion */\n'
    '#ifdef RECOMP_FAST_VERIFY\n#define RP_VB __rp_b;\n#define RP_VE __rp_e;\n'
    '#else\n#define RP_VB\n#define RP_VE\n#endif\n')


def strip_code(line, state):
    """Return the line with comments/strings removed; state = in-block-comment."""
    out, i, n = [], 0, len(line)
    while i < n:
        if state[0]:
            j = line.find('*/', i)
            if j < 0:
                return ''.join(out)
            state[0] = False; i = j + 2; continue
        c = line[i]
        if line.startswith('/*', i):
            state[0] = True; i += 2; continue
        if line.startswith('//', i):
            break
        if c in '"\'':
            j = i + 1
            while j < n and line[j] != c:
                j += 2 if line[j] == '\\' else 1
            i = j + 1; continue
        out.append(c); i += 1
    return ''.join(out)


def split_chunks(lines):
    """Top-level chunks: ('fn', name, hdr_idx, open_idx, close_idx) or ('top', idx)."""
    chunks, depth, st, i, n = [], 0, [False], 0, len(lines)
    while i < n:
        code = strip_code(lines[i], st)
        if depth == 0 and not st[0] and i + 1 < n and lines[i + 1] == '{\n' and ANYFN.match(lines[i].rstrip('\n')):
            name = ANYFN.match(lines[i].rstrip('\n')).group(1)
            j, d, st2 = i + 1, 0, [False]
            while j < n:
                c = strip_code(lines[j], st2)
                d += c.count('{') - c.count('}')
                if d == 0:
                    break
                j += 1
            if j >= n or lines[j] != '}\n':
                raise SystemExit('unbalanced function %s at line %d' % (name, i + 1))
            chunks.append(('fn', name, i, i + 1, j))
            i = j + 1; st = [False]; continue
        depth += code.count('{') - code.count('}')
        chunks.append(('top', i))
        i += 1
    if depth != 0:
        raise SystemExit('unbalanced top level')
    return chunks


def read_hot(path):
    """T1-7 hot list: lines '<nnnn> sub_XXXXXXXX' (# comments). Returns {nnnn: set(names)}."""
    hot = {}
    for l in open(path):
        l = l.split('#')[0].split()
        if not l:
            continue
        if len(l) != 2 or not re.match(r'^\d{4}$', l[0]) or not HDR.match('void %s(void)' % l[1]):
            raise SystemExit('bad hot-list line: %s' % ' '.join(l))
        hot.setdefault(l[0], set()).add(l[1])
    return hot


def main():
    a = sys.argv[1:]
    hot = {}
    if '--hot' in a:
        k = a.index('--hot')
        hot = read_hot(a[k + 1])
        del a[k:k + 2]
    force = set()
    while '--force' in a:                     # --force sub_XXXXXXXX: allow a body with our hooks
        k = a.index('--force')
        force.add(a[k + 1])
        del a[k:k + 2]
    gd, nums = a[0], a[1:]
    # Whole files (positional) get a copy of every eligible function. Files that
    # appear only in the hot list get copies of the listed functions only, so
    # their fast objects (and the kit compile time) stay small.
    hot = {nn: s for nn, s in hot.items() if nn not in nums}
    nums = nums + sorted(hot)
    files = {}
    for nn in nums:
        if nn in DENY_FILES:
            raise SystemExit('deny-listed file %s' % nn)
        p = os.path.join(gd, 'recomp_%s.c' % nn)
        txt = open(p, newline='').read()
        if '\r' in txt:
            raise SystemExit('CRLF in %s' % p)
        lines = [l for l in txt.splitlines(True) if not ENTRY.match(l)]
        if PREAMBLE_MARK not in lines:
            raise SystemExit('no preamble include in %s' % p)
        files[nn] = (p, lines, split_chunks(lines))
    # Eligibility pass (global fast set first, for the call renames).
    fast, why = set(), {}
    for nn, (p, lines, ch) in files.items():
        for c in ch:
            if c[0] != 'fn':
                continue
            name = c[1]
            body = ''.join(lines[c[3] + 1:c[4]])
            hm = HDR.match(lines[c[2]].rstrip('\n'))
            if nn in hot and name not in hot[nn]:
                why[name] = 'cold'
            elif not hm:
                why[name] = 'header'
            elif name in DENY_FUNCS:
                why[name] = 'fpu'
            elif DENY_BODY.search(body) and name not in force:
                why[name] = 'body:' + DENY_BODY.search(body).group(0)
            else:
                fast.add(name)
    for nn, names in hot.items():
        have = {c[1] for c in files[nn][2] if c[0] == 'fn'}
        if names - have:
            raise SystemExit('hot list names missing from recomp_%s.c: %s' % (nn, sorted(names - have)[:5]))
    total_d, t2 = 0, {}
    for nn, (p, lines, ch) in files.items():
        orig, fst, decl, nf, digest = [], [], set(), 0, hashlib.sha256()
        for c in ch:
            if c[0] == 'top':
                l = lines[c[1]]
                orig.append(l)
                if ONELINE_DEF.match(l.rstrip('\n')):
                    # T1-7: a one-line external definition (e.g. 0026's
                    # `void sub_00109536(void) { ma2_tank_fixed_update(...); }`)
                    # stays in the original file only (no duplicate symbol).
                    continue
                if l.lstrip().startswith('#include') and fst and FAST_BLOCK in fst:
                    # a header pulled in after the fast block keeps the default
                    # (scoped) accessor for its own inline helpers
                    fst += ['#undef XBOX_PTR\n',
                            '#define XBOX_PTR(addr) ma2_tank_scoped_pointer((uint32_t)(addr))\n',
                            l, '#undef XBOX_PTR\n',
                            '#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + _mo)\n']
                    continue
                fst.append(l)
                if l == PREAMBLE_MARK:
                    fst.append(FAST_BLOCK)
                continue
            _, name, h, o, e = c
            seg = lines[h:e + 1]
            if name not in fast:
                orig += seg
                cont = False  # keep directives (with their continuation lines)
                for l in lines[o + 1:e]:
                    if cont or l.lstrip().startswith('#'):
                        fst.append(l)
                        cont = l.rstrip('\n').endswith('\\')
                continue
            nf += 1
            orig += [lines[h], lines[o], '    RECOMP_FAST_ENTRY(%s);\n' % name] + lines[o + 1:e + 1]
            body = ''.join(lines[o + 1:e])

            def r1(m):
                if m.group(1) in fast:
                    decl.add(m.group(1)); return m.group(1) + '__fast()'
                return m.group(0)

            def r2(m):
                if m.group(2) in fast:
                    decl.add(m.group(2)); return m.group(1) + m.group(2) + '__fast)'
                return m.group(0)
            fbody = CALL2.sub(r2, CALL1.sub(r1, body))
            back = fbody.replace('__fast()', '()').replace('__fast)', ')')
            if back != body:
                raise SystemExit('self-check failed: %s' % name)
            digest.update(body.encode())
            pb, why2 = promote(fbody)
            k2 = why2.split(':')[0]
            t2[k2] = t2.get(k2, 0) + 1
            if pb is not None:
                t2['sync'] = t2.get('sync', 0) + int(why2.split(':')[2])
                fbody = pb
            fst += ['void %s__fast(void)\n' % name, '{\n', PROLOGUE, fbody, '}\n']
        # forward declarations of every renamed callee, after the fast block
        k = fst.index(FAST_BLOCK) + 1
        fst[k:k] = ['void %s__fast(void);\n' % d for d in sorted(decl)]
        open(p, 'w', newline='').write(''.join(orig))
        fp = os.path.join(gd, 'recomp_fast_%s.c' % nn)
        open(fp, 'w', newline='').write(''.join(fst))
        den = sum(1 for c in ch if c[0] == 'fn' and c[1] not in fast)
        total_d += den
        print('%s fast=%d denied=%d digest=%s' % (nn, nf, den, digest.hexdigest()[:12]))
    reasons = {}
    for v in why.values():
        k = v.split(':')[0] if not v.startswith('body') else v
        reasons[k] = reasons.get(k, 0) + 1
    print('total fast=%d denied=%d reasons=%s' % (len(fast), total_d, reasons))
    print('tier2 (RECOMP_FAST_REGS): %s' % t2)


if __name__ == '__main__':
    main()

"""MA2 source wiki, step 3: merge harvested facts + deterministic indexes into wiki/wiki.json and wiki/WIKI.md.

wiki.json:
  topics[pack]       title, page (markdown article), fact ids
  facts[id]          subject, addresses, related, fact, how_to_use, status, confidence, source, tags, date, pack
  addresses[VA]      function info (size, category, recomp file, owners, strings, callers) + fact ids + doc mentions
  env[VAR]           where it is read / documented + fact ids
  sources[path]      hand-written file: functions, addresses, header + fact ids
  objects[name]      class or property name from catalog.json -> fact ids (links the two dictionaries)
"""
from collections import defaultdict
from pathlib import Path
import json, re

HERE = Path(__file__).resolve().parent
W = HERE / 'wiki'
CONF = {'verified-in-game': 4, 'code-confirmed': 3, 'documented': 2, 'inferred': 1}


def norm_addr(a):
    m = re.fullmatch(r'(?:sub_|loc_|0x)?([0-9A-Fa-f]{5,8})', str(a).strip())
    return f'{int(m.group(1), 16):08X}' if m else None


def main():
    funcs = json.loads((W / 'raw/functions.json').read_text(encoding='utf-8'))
    starts = sorted(int(f['va'], 16) for f in funcs)
    fmap = {f['va']: f for f in funcs}
    import bisect

    def owner_fn(a):
        v = int(a, 16)
        i = bisect.bisect_right(starts, v) - 1
        if i >= 0:
            f = fmap[f'{starts[i]:08X}']
            if v < starts[i] + f['size']:
                return f['va']
        return None

    mentions, env, srcs = {}, {}, {}   # public copy: no raw source/env indexes
    cat = {'classes': {}, 'properties': {}}   # public copy: no object dictionary
    objnames = set(cat['classes']) | {p['name'] for p in cat['properties'].values()}

    wiki = {'topics': {}, 'facts': {}, 'addresses': {}, 'env': {}, 'sources': {}, 'objects': defaultdict(list)}
    bad = []
    src_cache = {}
    for fp in sorted((W / 'facts').glob('*.json')):
        try:
            d = json.loads(fp.read_text(encoding='utf-8'))
        except Exception as ex:
            bad.append(f'{fp.name}: {ex}'); continue
        pack = d.get('pack') or fp.stem
        page = W / 'pages' / f'{pack}.md'
        ids = []
        for i, f in enumerate(d.get('facts', [])):
            if not isinstance(f, dict) or not f.get('fact'):
                continue
            fid = re.sub(r'[^a-z0-9-]+', '-', str(f.get('id') or f.get('title') or i).lower()).strip('-')[:60] or str(i)
            fid = f'{pack.split("_")[0]}.{fid}'
            while fid in wiki['facts']:
                fid += '+'
            f['pack'] = pack
            addrs = []
            for a in f.get('addresses') or []:
                n = norm_addr(a)
                if n:
                    addrs.append(n)
            subj = f.get('subject') or {}
            n = norm_addr(subj.get('id', '')) if subj.get('kind') in ('function', 'address', 'vtable', 'global') else None
            if n and n not in addrs and subj.get('kind') != 'global':
                addrs.append(n)
            # Integrity: keep an address only if it is in the XBE image range and appears in a cited file.
            texts = []
            if isinstance(f.get('source'), str):   # free text: one source, not one per character
                f['source'] = [{'file': f['source'], 'line': ''}]
            for s in f.get('source') or []:
                p = Path(str(s.get('file', '')) if isinstance(s, dict) else str(s))
                if p not in src_cache:
                    try:
                        src_cache[p] = p.read_text(encoding='utf-8', errors='replace').upper() if p.is_file() else ''
                    except OSError:
                        src_cache[p] = ''
                texts.append(src_cache[p])
            good, unver = [], []
            for a in addrs:
                v = int(a, 16)
                if 0x10000 <= v < 0x600000 and a not in (f.get('addresses_unverified') or []):   # public copy: cited files are not shipped; the private build already vetted
                    good.append(a)
                else:
                    unver.append(a)
            f['addresses'] = good
            if unver:
                f['addresses_unverified'] = unver
            wiki['facts'][fid] = f
            ids.append(fid)
        wiki['topics'][pack] = {'title': d.get('title', pack), 'page': page.read_text(encoding='utf-8') if page.exists() else '',
                                'facts': ids}

    # Curated reconciliation (wiki/overrides.json).
    ov = W / 'overrides.json'
    if ov.exists():
        for fid, e in json.loads(ov.read_text(encoding='utf-8')).items():
            f = wiki['facts'].get(fid)
            if not f:
                continue
            for k, v in e.items():
                if k == 'append':
                    f['fact'] = f['fact'].rstrip() + ' ' + v
                else:
                    f[k] = v

    # Indexes.
    A = wiki['addresses']

    def addr_entry(va):
        if va not in A:
            fn = fmap.get(va) or (fmap.get(owner_fn(va)) if owner_fn(va) else None)
            A[va] = {'function': fn['va'] if fn else None,
                     'info': {k: fn[k] for k in ('size', 'cat', 'file', 'owners') if k in fn} if fn else None,
                     'facts': [], 'mentions': []}
        return A[va]

    for fid, f in wiki['facts'].items():
        for va in f['addresses']:
            addr_entry(va)['facts'].append(fid)
            fn = owner_fn(va)
            if fn and fn != va:
                addr_entry(fn)['facts'].append(fid)
        subj = f.get('subject') or {}
        sid = str(subj.get('id', ''))
        for name in [sid] + [str(r) for r in f.get('related') or []]:
            base = name.split('.')[0]
            for nm in {name, base, name.split('.')[-1]}:
                if nm in objnames:
                    wiki['objects'][nm].append(fid)
        # Names in the text: classes, and distinctive CamelCase property names (two or more humps).
        for tok in set(re.findall(r'\b[A-Za-z][A-Za-z0-9_]{5,}\b', f.get('title', '') + ' ' + f.get('fact', ''))):
            if tok in objnames and (tok in cat['classes'] or len(re.findall(r'[A-Z][a-z0-9]+', tok)) >= 2):
                wiki['objects'][tok].append(fid)
        if subj.get('kind') == 'env_var' or re.fullmatch(r'(RECOMP|MA2|PC)_[A-Z0-9_]+', sid):
            wiki['env'].setdefault(sid, {'read_in': [], 'mentioned_in': [], 'facts': []})['facts'].append(fid)
        for s in f.get('source') or []:
            p = str(s.get('file', '')) if isinstance(s, dict) else str(s)
            for sp in srcs:
                if p.endswith(sp.split('\\')[-1]) and sp.split('\\')[-1] in p:
                    wiki['sources'].setdefault(sp, dict(srcs[sp], facts=[]))['facts'].append(fid)
    for va, ms in mentions.items():
        addr_entry(va)['mentions'] = ms[:25]
    for k, v in env.items():
        e = wiki['env'].setdefault(k, {'facts': []})
        e['read_in'] = v.get('read_in', [])[:6]; e['mentioned_in'] = v.get('mentioned_in', [])[:6]
    for sp, s in srcs.items():
        wiki['sources'].setdefault(sp, dict(s, facts=[]))
    wiki['objects'] = {k: sorted(set(v)) for k, v in sorted(wiki['objects'].items())}
    for a in A.values():
        a['facts'] = sorted(set(a['facts']))

    stats = {'topics': len(wiki['topics']), 'facts': len(wiki['facts']),
             'addresses_with_facts': sum(1 for a in A.values() if a['facts']),
             'addresses_documented': len(A), 'env_vars': len(wiki['env']),
             'env_vars_with_facts': sum(1 for e in wiki['env'].values() if e.get('facts')),
             'sources': len(wiki['sources']), 'objects_linked': len(wiki['objects']),
             'functions_total': len(funcs), 'confidence': {}, 'bad_fact_files': bad,
             'addresses_unverified': sum(len(f.get('addresses_unverified') or []) for f in wiki['facts'].values())}
    for f in wiki['facts'].values():
        c = f.get('confidence', '?')
        stats['confidence'][c] = stats['confidence'].get(c, 0) + 1
    wiki['stats'] = stats
    (W / 'wiki.json').write_text(json.dumps(wiki, indent=1, ensure_ascii=False, default=str), encoding='utf-8')
    write_md(wiki)
    print(json.dumps(stats, indent=1))


def write_md(wiki):
    L = ['# MechAssault 2 source wiki', '',
         'One reference for the MA2 PC static recompilation: original machine code, generated C, and everything '
         'added since (hooks, PC port, renderer, audio, bots, scenarios, fixes). Machine-readable twin of this file: `wiki.json`.', '',
         f"Facts: {wiki['stats']['facts']} · documented addresses: {wiki['stats']['addresses_documented']} "
         f"({wiki['stats']['addresses_with_facts']} with facts) · env vars: {wiki['stats']['env_vars']} · "
         f"functions indexed: {wiki['stats']['functions_total']}", '', '## Topics', '']
    for k, t in sorted(wiki['topics'].items()):
        L.append(f"- [{t['title']}](#{k}) ({len(t['facts'])} facts)")
    for k, t in sorted(wiki['topics'].items()):
        L += ['', f'<a id="{k}"></a>', '', f"## {t['title']}", '']
        if t['page']:
            L += [re.sub(r'^(#+)', r'#\1', t['page'], flags=re.M), '']
        L += ['### Facts', '']
        for fid in t['facts']:
            f = wiki['facts'][fid]
            ad = (' `' + '`, `'.join(f['addresses'][:4]) + '`') if f['addresses'] else ''
            src = f.get('source') or []
            s0 = src[0] if src and isinstance(src[0], dict) else {}
            where = f" — {Path(str(s0.get('file', ''))).name}:{s0.get('line', '')}" if s0 else ''
            L.append(f"- **{f.get('title', fid)}**{ad} [{f.get('confidence', '')}{', ' + f['status'] if f.get('status') not in (None, 'current') else ''}] "
                     f"{f['fact']}{(' *How:* ' + f['how_to_use']) if f.get('how_to_use') else ''}{where}")
    L += ['', '## Address index (addresses with facts)', '', '| Address | Function | Owner | Facts |', '|---|---|---|---|']
    for va, a in sorted(wiki['addresses'].items()):
        if not a['facts']:
            continue
        info = a.get('info') or {}
        L.append(f"| `{va}` | {a.get('function') or ''} {info.get('file', '')} | {', '.join((info.get('owners') or [])[:2])} | "
                 + '; '.join(wiki['facts'][x].get('title', x) for x in a['facts'][:4]).replace('|', '/') + ' |')
    L += ['', '## Environment variables', '']
    for k, e in sorted(wiki['env'].items()):
        t = '; '.join(wiki['facts'][x].get('title', x) for x in e.get('facts', [])[:3])
        r = e.get('read_in') or e.get('mentioned_in') or []
        where = Path(r[0]['file']).name if r else ''
        L.append(f'- `{k}`' + (f' — {t}' if t else '') + (f' ({where})' if where else ''))
    (W / 'WIKI.md').write_text('\n'.join(L), encoding='utf-8')


if __name__ == '__main__':
    main()

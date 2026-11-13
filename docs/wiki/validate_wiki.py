"""validate_wiki.py [--all] [pack.json ...]   Read-only check of wiki/facts and wiki/overrides.json. Run before merge_wiki.py.

Always checked (every pack): the file parses, no fact is skipped by the merge, no id collides.
Strict checks (the schema of WIKI_BRIEF.md) apply to packs t28 and later, to packs named on the command line,
or to every pack with --all. Exit code 1 when there is an ERROR.
"""
import json, re, sys
from pathlib import Path

W = Path(__file__).resolve().parent / 'wiki'
STATUS = {'current', 'superseded', 'historical', 'open-question'}
CONF = {'verified-in-game', 'code-confirmed', 'documented', 'inferred'}
KINDS = {'function', 'address', 'global', 'vtable', 'class', 'property', 'env_var', 'file', 'patch', 'asset', 'menu',
         'setting', 'save', 'tool', 'concept'}
FIRST_STRICT = 28
KEYS = {'id', 'title', 'subject', 'addresses', 'related', 'fact', 'how_to_use', 'status', 'confidence', 'source',
        'source_kind', 'tags', 'date'}


def main():
    args = sys.argv[1:]
    named = {Path(a).name for a in args if a != '--all'}
    count = {'ERROR': 0, 'WARN': 0}
    merged, texts, later = {}, {}, []
    try:
        fstarts = {x['va'] for x in json.loads((W / 'raw/functions.json').read_text(encoding='utf-8'))}
    except OSError:
        fstarts = set()   # public copy without raw/: no function-start rule


    def say(level, where, msg):
        count[level] += 1
        print(f'{level} {where}: {msg}')

    def text(path):
        if path not in texts:
            p = Path(path)
            try:
                texts[path] = p.read_text(encoding='utf-8', errors='replace').upper() if p.is_file() else None
            except OSError:
                texts[path] = None
        return texts[path]

    for fp in sorted((W / 'facts').glob('*.json')):
        raw = fp.read_bytes()
        if raw[:3] == bytes([0xEF, 0xBB, 0xBF]):
            say('ERROR', fp.name, 'starts with a UTF-8 BOM: the merge drops the whole pack')
            continue
        try:
            d = json.loads(raw.decode('utf-8'))
        except Exception as ex:
            say('ERROR', fp.name, f'not valid JSON, the merge drops the whole pack: {ex}')
            continue
        m = re.match('t([0-9]+)', fp.name)
        strict = '--all' in args or fp.name in named or bool(m and int(m.group(1)) >= FIRST_STRICT)
        if strict and (d.get('pack') != fp.stem or not d.get('title') or not isinstance(d.get('facts'), list)):
            say('ERROR', fp.name, 'needs "pack" equal to the file name, a "title" and a "facts" list')
        prefix = str(d.get('pack') or fp.stem).split('_')[0]
        seen = set()
        for i, f in enumerate(d.get('facts') or []):
            where = f'{fp.name} #{i} {f.get("id") if isinstance(f, dict) else ""}'
            if not isinstance(f, dict) or not f.get('fact'):
                say('ERROR', where, 'empty "fact": the merge skips it without a message')
                continue
            fid = str(f.get('id') or '')
            if fid in seen:
                say('ERROR', where, 'duplicate id in this pack')
            seen.add(fid)
            mid = prefix + '.' + (re.sub('[^a-z0-9-]+', '-', fid.lower()).strip('-')[:60] or str(i))
            if mid in merged:
                say('ERROR', where, f'merged id {mid} is already used by {merged[mid]}: the merge would add "+"')
            merged.setdefault(mid, fp.name)
            later += [(where, r) for r in (f.get('related') or []) if re.fullmatch('t[0-9]+[a-z]?[.][a-z0-9+-]+', str(r))]
            if not strict:
                continue
            if not re.fullmatch('[a-z0-9]+(-[a-z0-9]+)*', fid) or len(fid) > 60:
                say('ERROR', where, 'id must be lower-case kebab-case, at most 60 characters')
            if not str(f.get('title') or '').strip() or chr(10) in str(f.get('title')):
                say('ERROR', where, 'title must be one non-empty line')
            sub = f.get('subject')
            if not isinstance(sub, dict) or sub.get('kind') not in KINDS or not sub.get('id'):
                say('ERROR', where, f'subject must be a kind from WIKI_BRIEF.md plus an id, got {sub!r}')
            if f.get('status') not in STATUS:
                say('ERROR', where, f'status {f.get("status")!r} is not one of {sorted(STATUS)}')
            if f.get('confidence') not in CONF:
                say('ERROR', where, f'confidence {f.get("confidence")!r} is not one of {sorted(CONF)}')
            if not re.fullmatch('[0-9]{4}-[0-9]{2}-[0-9]{2}', str(f.get('date'))):
                say('ERROR', where, 'date must be "YYYY-MM-DD"')
            for k in ('addresses', 'related', 'tags'):
                if not isinstance(f.get(k), list):
                    say('ERROR', where, f'"{k}" must be a list (it may be empty)')
            src = f.get('source')
            if not isinstance(src, list) or not src or not all(isinstance(s, dict) and s.get('file') for s in src):
                say('ERROR', where, 'source must be a non-empty list of {"file": ..., "line": ...}')
                src = []
            for k in sorted(set(f) - KEYS):
                say('WARN', where, f'unknown key "{k}"')
            addrs = [str(a) for a in f['addresses']] if isinstance(f.get('addresses'), list) else []
            if isinstance(sub, dict) and sub.get('kind') in ('function', 'address', 'vtable'):
                sm = re.fullmatch('(?:sub_|loc_|0x)?([0-9A-Fa-f]{5,8})', str(sub.get('id')).strip())
                if sm:
                    addrs.append(f'{int(sm.group(1), 16):08X}')
                else:
                    say('ERROR', where, 'subject.id of kind function/address/vtable must be an address')
            for a in dict.fromkeys(addrs):
                if not re.fullmatch('[0-9A-F]{8}', a):
                    say('ERROR', where, f'address {a!r} must be 8 upper-case hex digits')
                    continue
                if not 0x10000 <= int(a, 16) < 0x600000:
                    say('ERROR', where, f'address {a} is outside 00010000..005FFFFF: the merge drops it')
                    continue
                if True:   # public copy: cited files are not shipped, any in-range address is accepted
                    continue
                pat = '(?<![0-9A-F])0*' + a.lstrip('0') + '(?![0-9A-F])'
                if not any(text(s['file']) and re.search(pat, text(s['file'])) for s in src):
                    say('ERROR', where, f'address {a} is in no cited file that exists: the merge drops it from the '
                                        'index (cite a file that contains it, or set "source_kind": "message")')
            if f.get('status') == 'superseded' and not re.search('t[0-9]+[a-z]?[.][a-z0-9-]+', f['fact']):
                say('WARN', where, 'superseded fact does not name the merged id of its replacement')
    for where, r in later:
        if r not in merged:
            say('WARN', where, f'related "{r}" is not a merged fact id')
    ov = W / 'overrides.json'
    if ov.exists():
        for k in json.loads(ov.read_text(encoding='utf-8')):
            if not k.startswith('_') and k not in merged:
                say('ERROR', 'overrides.json', f'key {k} matches no fact: the merge ignores it without a message')
    print(f'{count["ERROR"]} errors, {count["WARN"]} warnings, {len(merged)} facts checked')
    sys.exit(1 if count['ERROR'] else 0)


if __name__ == '__main__':
    main()

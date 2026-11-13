"""wq.py <regex> [chars]: search the wiki facts (case-insensitive). Prints id, status, confidence, date, title, snippet."""
import json, re, sys
from pathlib import Path
wiki = json.loads((Path(__file__).resolve().parent / 'wiki/wiki.json').read_text(encoding='utf-8'))
pat = re.compile(sys.argv[1], re.I); lim = int(sys.argv[2]) if len(sys.argv) > 2 else 240
n = 0
for fid, f in wiki['facts'].items():
    blob = ' '.join(str(f.get(k, '')) for k in ('id', 'title', 'fact', 'how_to_use', 'related', 'addresses', 'addresses_unverified'))
    m = pat.search(blob)
    if m:
        n += 1
        full = str(f.get('fact', '')) + ' || HOW: ' + str(f.get('how_to_use', ''))
        i = full.find(m.group(0)); s = max(0, i - lim // 2) if i >= 0 else 0
        print(f"[{fid}] {f.get('status')} | {f.get('confidence')} | {f.get('date')} | {str(f.get('title'))[:120]}\n     ...{full[s:s + lim]}")
print('matches', n)

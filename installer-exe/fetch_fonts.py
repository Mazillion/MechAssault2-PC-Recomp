"""Maintainer-only: fetch the two OFL fonts the installer theme uses, unmodified, with their licence files.

  python fetch_fonts.py            (needs internet; run once on the dev PC, then repack)

  Rajdhani (SemiBold, Bold)  and  Share Tech Mono (Regular)  -- SIL Open Font License 1.1, from the Google Fonts repository.
  They go to  kit/installer/data/fonts/  (loaded privately by the installer window) and their licence texts to
  kit/defaults/Licenses/  (shipped beside the game).

Unmodified means: not renamed, not subsetted. Without these files the installer falls back to Bahnschrift and Consolas.
"""
import hashlib, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
KIT = HERE.parent / 'kit'
RAW = 'https://raw.githubusercontent.com/google/fonts/main/ofl/'
FILES = [('rajdhani/Rajdhani-SemiBold.ttf', KIT / 'installer/data/fonts/Rajdhani-SemiBold.ttf'),
         ('rajdhani/Rajdhani-Bold.ttf', KIT / 'installer/data/fonts/Rajdhani-Bold.ttf'),
         ('rajdhani/OFL.txt', KIT / 'defaults/Licenses/Rajdhani-OFL.txt'),
         ('sharetechmono/ShareTechMono-Regular.ttf', KIT / 'installer/data/fonts/ShareTechMono-Regular.ttf'),
         ('sharetechmono/OFL.txt', KIT / 'defaults/Licenses/ShareTechMono-OFL.txt')]

for rel, dest in FILES:
    with urllib.request.urlopen(RAW + rel, timeout=60) as r: data = r.read()
    if dest.suffix == '.ttf' and data[:4] not in (b'\x00\x01\x00\x00', b'true', b'OTTO'):
        raise SystemExit('%s is not a TrueType font' % rel)
    dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(data)
    print('%-44s %7d bytes  sha256 %s' % (dest.name, len(data), hashlib.sha256(data).hexdigest()[:16]))

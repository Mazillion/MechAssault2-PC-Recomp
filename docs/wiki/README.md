# MechAssault 2 recompilation wiki

Facts about how the recompiled MechAssault 2 game code works: what a function does, which global holds what, which
setting or environment variable changes which behaviour, what a bug was, how it was fixed and whether the fix shipped.
3472 facts in 72 topics.

Addresses are virtual addresses in the Xbox executable `ma2.xbe` of the US/NTSC retail disc (range `00010000`..`005FFFFF`,
8 hex digits, for example `003AE754`). `sub_0018ABB5` is the function that starts at that address.

## What this is not
It contains no game data: no models, textures, sounds, levels, movies, dialogue, string dumps, disc file listings or
program code of the game. Only names, addresses, offsets, class and property names, short ids and explanations. You need
your own copy of the game disc to make use of any of it. This is an unofficial fan project and is not affiliated with or
endorsed by the publisher or the original developers.

## Layout
| Path | What it is |
|---|---|
| `wiki/WIKI.md` | The readable wiki: topic articles, each followed by its facts, then an address index and a list of environment variables. Large: search it, do not open it whole. |
| `wiki/wiki.json` | The same, machine-readable. |
| `wiki/facts/<pack>.json` | The source of the facts, one pack per topic. The only place facts are edited. |
| `wiki/pages/<pack>.md` | Optional topic article for a pack. |
| `wiki/overrides.json` | Corrections applied on top of the packs (a changed status, an appended CORRECTION or UPDATE sentence). |
| `wiki/raw/functions.json` | Function start addresses, sizes, categories and owner names of `ma2.xbe` (no strings). |
| `merge_wiki.py` | Builds `wiki/wiki.json` and `wiki/WIKI.md` from packs, pages and overrides. |
| `validate_wiki.py` | Checks packs and overrides before a merge. |
| `wq.py` | Looks facts up. |
| `WIKI_BRIEF.md` | Contributor brief: fact schema and the add / correct procedure. |

Every fact has an id (`t28.ba-walker-bit-cleared-by-jet` = pack prefix + id), a subject, addresses, a plain-language text, how
to use or test it, a status (`current`, `superseded`, `historical`, `open-question`), a confidence (`verified-in-game`,
`code-confirmed`, `documented`, `inferred`), its sources, tags and a date.

## Looking things up
```
python wq.py "BaLand"          # facts that mention BaLand (id, status, confidence, date, title, snippet)
python wq.py "003AACE7"        # everything known about an address
python wq.py "frame ?limit" 0  # titles only
```
`wq.py` needs Python 3 and nothing else. Or search `wiki/WIKI.md` with any text tool.

## Contributing
1. Add an object to a pack in `wiki/facts/` (or start a new pack); correct an old fact with an entry in `wiki/overrides.json`.
2. `python validate_wiki.py` must print `0 errors, 0 warnings`.
3. `python merge_wiki.py` must print `"bad_fact_files": []` and one more fact per fact you added.
4. Send the changed pack / overrides files (the rebuilt `wiki/wiki.json` and `wiki/WIKI.md` are optional).

Never add game data. Say how you found the fact and how sure you are (the `confidence` field). Full rules: `WIKI_BRIEF.md`.

## Source citations
`source` entries use repository paths: `kit/source/game/src/...` (game-side code, bots in `bots/`, scenario code in
`scenario/`), `kit/source/runtime/src/...` (xboxrecomp runtime), `kit/installer/...`, `kit/patches/...`, `editor/...`,
`installer-exe/...`. Generated code is cited as `generated recomp_XXXX.c`; it is made at install time and is not in the
repository. Entries like `tools/<name> (maintainer tool, not in the repository)` name scripts that are not published.
Many facts have no citation: their evidence was a run log or a measurement.

## Left out
- Facts that came from a published strategy guide.
- The object and property dictionary of the scenario editor, raw source and env-var indexes and the HTML search page.
- Process-only material (who did what, test scheduling) and unpublished scratch evidence.

## Licence
MIT, see `LICENSE`. MechAssault is a trademark of its owners; no game assets are included.

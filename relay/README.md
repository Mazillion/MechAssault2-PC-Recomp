# Report relay

The small Cloudflare Worker that receives reports from "Report a problem" (see [PRIVACY.md](../PRIVACY.md)).

- `src/worker.js`: the whole service. It stores the report file in Cloudflare R2 and opens an entry in a private GitHub repository. The connecting IP is only used, scrambled under a key that is replaced daily, to limit reports per hour; it is never written anywhere.
  - Retention: an hourly job deletes every report older than 60 days (`KEEP_DAYS`; the bucket's lifecycle rule is a second net) and retires its GitHub entry: deleted when the token has the rights, otherwise emptied, closed and locked. The player's typed text is not copied into the entry (`ISSUE_PLAYER_TEXT`), because GitHub keeps an edited entry's earlier text in its history.
  - `DELETE /v1/report/<id>` (owner key) deletes one report and its entry on request (`python tools/triage.py delete MA2-XXXXXX`).
  - `GET /v1/latest` answers the game's once-per-start update check and the installer's "Update to X" with the newest release: its version and, for the installer, the SHA-256 and size of the release's installer file. It reads nothing from the request.
  - The newest release is set after a GitHub release is published with `python tools/triage.py announce X.Y.Z --installer <the release's installer exe>` (it checks the GitHub release first, shows a summary and asks; `--show`, `--undo`). It is stored in the bucket (`config/latest.json`), so no dashboard step or redeploy is needed; the `LATEST_*` vars apply only while nothing was announced.
- `wrangler.toml` / `wrangler.staging.toml`: the deploy settings. Workers Logs is off (`[observability] enabled = false`). Secrets (the GitHub token and the owner key) are never in these files.
- `GET /v1/open/<id>` (the Download link in each issue): asks the developer for the owner key once per browser, then sets a cookie that only allows downloading reports (`HttpOnly`, `Secure`, `SameSite=Lax`; derived from the key, so changing the key signs every browser out). Every other owner action still needs the key in a header.
- `tools/triage.py`: the developer's tool for reading reports (also `delete`, `expire`, `draft-issue`); it uses the production relay by default and can keep the owner key in Windows Credential Manager. `tools/ma2_reports.pyw`: the developer's one-click helper (report number on the clipboard -> downloaded, summarised, folder opened). `tools/smoke.py`: deploy checks.
- Deploys run from `.github/workflows/relay-deploy.yml`: tests, staging, then production after the owner approves.
- `test/`: `node --test test/relay.test.mjs`, `python test/test_triage.py` and `python test/test_announce.py`.

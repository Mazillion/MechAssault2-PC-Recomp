# Report relay

The small Cloudflare Worker that receives reports from "Report a problem" (see [PRIVACY.md](../PRIVACY.md)).

- `src/worker.js`: the whole service. It stores the report file in Cloudflare R2, which deletes it after 60 days (`KEEP_DAYS`, plus a bucket lifecycle rule), and opens an entry in a private GitHub repository. The connecting IP is only used, scrambled under a key that is replaced daily, to limit reports per hour; it is never written anywhere.
- `wrangler.toml` / `wrangler.staging.toml`: the deploy settings. Workers Logs is off (`[observability] enabled = false`). Secrets (the GitHub token and the owner key) are never in these files.
- `tools/triage.py`: the developer's tool for reading reports; `tools/smoke.py`: deploy checks.
- `test/`: `node --test test/relay.test.mjs` and `python test/test_triage.py`.

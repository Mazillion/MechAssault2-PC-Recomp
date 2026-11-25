// MechAssault 2 report relay (Cloudflare Worker, plain ES module, no build step).
//
// Protocol (what the installer, the game and MA2 Report.exe send):
//   POST /v1/report   multipart/form-data: text, summary, zip
//                     headers X-MA2-Client, X-MA2-Version, X-MA2-Build, X-MA2-Kind, X-MA2-Signature
//                     (the same five names are also accepted as form fields: client, version, build, kind, signature)
//   reply             JSON {ok, id, message}; ok:false always comes with a non-2xx status.
//   GET /v1/latest    {ok: true, latest: "1.1.7" | null, notice?: "..."} from LATEST_VERSION / UPDATE_NOTICE (1.1.7).
//                     The game asks once per launch (only its version in the User-Agent). The route reads two vars and
//                     nothing else: no limiter, no IP, no storage, no log line. Cacheable for an hour.
//                     1.1.7 self-update: plus sha256, size, asset (the release installer) once announced; the values come
//                     from config/latest.json (POST /v1/admin/latest, `triage.py announce`), else from the vars.
//
// Privacy: the connecting IP is used only inside the rate limiter, as an HMAC under a random key that is
// created per UTC day and deleted after it. No IP, hash of an IP, country or user agent is written to R2,
// to GitHub or to a log line. Workers Logs is switched off in wrangler.toml.
//
// Owner routes (header X-MA2-Owner-Key = the OWNER_KEY secret; all return 404 when OWNER_KEY is not set):
//   GET  /v1/report/<id>         the zip
//   GET  /v1/report/<id>/info    the stored text, summary and metadata (JSON)
//   GET  /v1/admin/reports?days=N   list of recent reports
//   GET  /v1/admin/pending       reports whose GitHub issue is not open yet
//   POST /v1/admin/retry         retry the pending issues now
//   POST /v1/admin/expire        run the retention pass now (the cron runs it every hour)
//   GET|POST /v1/admin/switch    read or set the off switch {on, message}
//   GET|POST /v1/admin/latest    read or announce the newest release {version, sha256, size, asset, notice}, or {undo: true}
//   DELETE /v1/report/<id>       delete one report now: zip, stored text and summary, pending marker, and its issue
//                                (deleted if the token may, else emptied, closed and locked). 1.1.7.
//
// One-click download for the owner (11-22), the link in every issue:
//   GET  /v1/open/<id>           with the owner cookie: the zip (attachment MA2-XXXXXX.zip); without it: a small page
//                                that asks for the owner key (the page looks the same whether the report exists or not)
//   POST /v1/open/<id>           form: key, remember. Right key: sets the cookie, 303 back to the GET. Wrong key: the
//                                same page with a generic error. Same-origin only; tries are rate-limited per connection.
//   GET  /v1/open/forget         clears the cookie on this browser
// The cookie value is HMAC-SHA256(OWNER_KEY, "owner-cookie-v1"): rotating OWNER_KEY revokes every cookie. It is
// HttpOnly, Secure, SameSite=Lax (Lax so that a click on the link in a github.com issue still carries it), Path=/v1/.
// It authorizes report downloads and (1.2.0) the conversation page with its reply and edit forms only: every other owner route still needs the X-MA2-Owner-Key header. No key or cookie
// value is ever put in a URL or a log line.
//
// Retention (1.1.7): every report is deleted KEEP_DAYS (60) days after it arrived. The hourly cron deletes the R2
// objects itself (the bucket's lifecycle rule is a second net) and retires the report's issue: GraphQL deleteIssue when
// the token has the rights, else the issue's title and body are replaced by a "removed" line and it is closed and
// locked. GitHub keeps an edited issue's earlier text in its edit history (visible only to people with access to the
// private reports repo) until the issue is deleted; that is why the player's own text is NOT put into issues by default
// (ISSUE_PLAYER_TEXT = "on" restores the 1.1.6 issue body). The issue then holds technical data only: report number,
// kind, game version, build, crash signature and the system summary.
//
// Report conversations (1.2.0): the owner can reply to a report and the player can answer.
//   POST /v1/report     a 1.2.0 client adds the form field reply_secret (64 hex = 256 random bits it made and keeps in
//                       %LOCALAPPDATA%\MechAssault2\report-conversations.txt). Only SHA-256(secret) is stored, in
//                       conv/<id>.json. A 1.1.x client sends no secret: its report works as before, without replies.
//   POST /v1/replies    JSON {reports: [{id, secret, seen?}]} (at most 20). For each pair whose secret matches: the owner
//                       messages with n <= seen are marked seen, then the owner messages not seen yet are returned:
//                       {ok, replies: [{id, n, text, at, can_answer}]}. A wrong secret or unknown id returns nothing for
//                       that entry (the answer looks the same). Rate-limited per connection (REPLY_POLLS_PER_HOUR, the
//                       same scrambled-IP limiter). The clients call it only when they hold an entry under 60 days old;
//                       /v1/latest never does any of this.
//   POST /v1/answer     multipart: id, secret, text (at most 1000 characters, already scrubbed by the client), and
//                       optionally summary + zip (a new report bundle: "Attach my latest logs"). Same limits as a report
//                       (per connection per hour, daily cap, size). Only when the newest message is the owner's (strict
//                       alternation). The answer becomes a comment on the report's private issue (the owner's notification).
//   Owner: GET /v1/open/<id> (key cookie) is a page with the download link, the conversation and a reply box;
//          POST /v1/open/<id>/reply {text}, POST /v1/open/<id>/edit {n, text} (only while the player has not seen it);
//          GET /v1/open/<id>/zip, GET /v1/open/<id>/logs/<n>. Header-key twins for triage.py:
//          GET /v1/report/<id>/conversation, POST /v1/report/<id>/reply {text, edit?}, GET /v1/report/<id>/logs/<n>,
//          GET /v1/admin/conversations.
// Expiry: KEEP_DAYS after the conversation's LAST message (a report without messages: after it arrived). conv/<id>.json
// carries {last} (also as R2 custom metadata, so the sweep lists it without reading each file). The hourly sweep deletes a
// report, its answers and its conversation once last + 60 days has passed. The bucket's lifecycle rule (60 days after
// upload, prefixes reports/ and answers/) stays as the second net: the sweep re-writes (copies onto itself) every object
// of a live conversation once it is REFRESH_DAYS (45) old, so the rule never reaches a conversation that is still active.
// conv/ objects are re-written on every message, so a 60-day rule on conv/ is safe as well.

const CLIENTS = new Set(["installer", "game", "helper"]);
const KINDS = new Set(["install-fail", "crash", "hang", "manual"]);
const MAX_TEXT = 2000;
const MAX_SUMMARY = 16384;
const ID_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"; // RFC 4648 base32 (no 0, 1, 8, 9): what the installer client checks
const ID_RE = /^MA2-[A-Z2-7]{6}$/;
const KEEP_DAYS = 60;
const GITHUB_API = "https://api.github.com";
const USER_AGENT = "ma2-report-relay";
const RELEASE_RE = /^\d{1,4}(\.\d{1,4}){1,3}$/; // a plain release number: what /v1/latest may announce
// GitHub statuses that mean "this token will not work" (401 bad credentials, 403 no rights, 400 malformed token):
// a retry run stops at the first one instead of trying every pending report.
const TOKEN_DEAD = new Set([400, 401, 403]);
const RETIRE_PER_RUN = 40;      // issues retired per cron run (GitHub's secondary rate limits favour small batches)
const EXPIRE_PER_RUN = 1000;    // R2 objects deleted per cron run
// 1.2.0 report conversations
const MAX_MESSAGE = 1000;       // characters per owner reply or player answer
const SECRET_RE = /^[0-9a-f]{64}$/;
const MAX_POLL = 20;            // {id, secret} pairs per /v1/replies call
const REFRESH_DAYS = 45;        // a live conversation's objects are re-written at this age (the lifecycle rule is at 60)
const REFRESH_PER_RUN = 50;
const COMMENTS_PER_RUN = 20;
// Test traffic (11-25): the test suites send build "test" and tools/smoke.py sent build "smoke". The relay refuses both
// (403, nothing stored, no issue), so a test that loses its local mock relay can never file a report here. Only the
// local test harness (test/local-relay.mjs) sets ACCEPT_TEST_BUILDS = "on"; neither wrangler file has it.
const TEST_BUILDS = /^(test|smoke)$/i;
const testBuild = (env, build) => TEST_BUILDS.test(String(build ?? "").trim()) && String(env.ACCEPT_TEST_BUILDS || "").toLowerCase() !== "on";
// An empty helper report (11-25): automated malware-analysis sandboxes run MA2 Report.exe on its own and press Send.
// Their reports are kept in R2 like any other, but open no issue and no conversation (see quietReport).
const QUIET_ZIP_MAX = 64 * 1024;

const MSG = {
  off: "Reports are switched off right now. Your report was saved on your PC.",
  busy: "The report service has had too many reports today. Your report was saved on your PC; please try again tomorrow.",
  ip: "Too many reports from this connection in the last hour. Your report was saved on your PC; please try again later.",
  big: "The report is too large to send. It was saved on your PC.",
  bad: "The report could not be read by the server. It was saved on your PC.",
  store: "The server could not store the report. It was saved on your PC; please try again later.",
  test: "This report service does not take test reports. The report was saved on your PC.",
};

// ---------------------------------------------------------------- helpers

function json(obj, status = 200, extra = {}) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store", ...extra },
  });
}

function fail(status, message) {
  return json({ ok: false, id: null, message }, status);
}

function intVar(env, name, dflt) {
  const v = parseInt(env[name] ?? "", 10);
  return Number.isFinite(v) && v > 0 ? v : dflt;
}

function newId() {
  const b = new Uint8Array(6);
  crypto.getRandomValues(b);
  let s = "MA2-";
  for (const x of b) s += ID_ALPHABET[x & 31];
  return s;
}

function monthOf(d) {
  return d.toISOString().slice(0, 7);
}

function lastMonths(n, now = new Date()) {
  const out = [];
  const d = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), 1));
  for (let i = 0; i < n; i++) {
    out.push(monthOf(d));
    d.setUTCMonth(d.getUTCMonth() - 1);
  }
  return out;
}

// Player text and the summary go into a fenced code block. Backticks cannot close the fence, @ cannot mention,
// and control characters (other than newline and tab) are dropped.
export function neutralize(s) {
  s = String(s ?? "").replace(/\r\n?/g, "\n");
  s = s.replace(/[\u0000-\u0008\u000B-\u001F\u007F‪-‮⁦-⁩]/g, "");
  s = s.replace(/`/g, "ˋ").replace(/@/g, "＠");
  return s.trim() === "" ? "(none)" : s;
}

// Metadata fields: strict character sets; anything else makes the request malformed (signature is cleaned).
function cleanMeta(m) {
  const out = {};
  out.client = String(m.client ?? "").trim().toLowerCase();
  out.kind = String(m.kind ?? "").trim().toLowerCase();
  out.version = String(m.version ?? "").trim() || "unknown";
  out.build = String(m.build ?? "").trim();
  out.signature = String(m.signature ?? "")
    .replace(/[^A-Za-z0-9 _.:+\-\/=#]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 120);
  if (!CLIENTS.has(out.client)) return { error: "unknown client kind" };
  if (!KINDS.has(out.kind)) return { error: "unknown report kind" };
  if (!/^[0-9A-Za-z.+\-]{1,32}$/.test(out.version)) return { error: "bad version" };
  if (!/^[0-9A-Za-z.+\-]{0,64}$/.test(out.build)) return { error: "bad build id" };
  return { meta: out };
}

// The member names of a zip, from its central directory; null when the directory cannot be read.
export function zipNames(b) {
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  let e = -1;
  for (let i = b.length - 22; i >= 0 && i >= b.length - 22 - 65535; i--) {
    if (dv.getUint32(i, true) === 0x06054b50) {
      e = i;
      break;
    }
  }
  if (e < 0) return null;
  const count = dv.getUint16(e + 10, true);
  let p = dv.getUint32(e + 16, true);
  const names = [];
  for (let n = 0; n < count; n++) {
    if (p + 46 > e || dv.getUint32(p, true) !== 0x02014b50) return null;
    const nl = dv.getUint16(p + 28, true), xl = dv.getUint16(p + 30, true), cl = dv.getUint16(p + 32, true);
    if (p + 46 + nl > e) return null;
    names.push(new TextDecoder().decode(b.subarray(p + 46, p + 46 + nl)));
    p += 46 + nl + xl + cl;
  }
  return names;
}

// G1 (REPORTS-1124): a report from MA2 Report.exe run on its own on a machine with no game, the sandbox pattern
// (client helper, kind manual, no words, "Installed: no recomp-install.json ...", "Game log: none", and a zip that
// holds system.txt and nothing else). Stored like any report; no GitHub issue and no conversation. Anything more
// (text, an install, a log, any other file in the zip, another client or kind) opens an issue as before.
export async function quietReport(meta, text, summary, zip) {
  if (meta.client !== "helper" || meta.kind !== "manual") return false;
  if (String(text ?? "").trim() !== "") return false;
  const s = String(summary ?? "");
  if (!/^Installed: no recomp-install\.json /m.test(s) || !/^Game log: none\s*$/m.test(s)) return false;
  if (!zip || zip.size > QUIET_ZIP_MAX) return false;
  const names = zipNames(new Uint8Array(await zip.arrayBuffer()));
  return !!names && names.length === 1 && names[0].toLowerCase() === "system.txt";
}

function versionLess(a, b) {
  const pa = String(a).split(/[.\-+]/).map((x) => parseInt(x, 10) || 0);
  const pb = String(b).split(/[.\-+]/).map((x) => parseInt(x, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const x = pa[i] ?? 0, y = pb[i] ?? 0;
    if (x !== y) return x < y;
  }
  return false;
}

async function sha256(s) {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s)));
}

async function sameSecret(a, b) {
  const [x, y] = await Promise.all([sha256(a), sha256(b)]);
  let d = 0;
  for (let i = 0; i < x.length; i++) d |= x[i] ^ y[i];
  return d === 0;
}

// The owner key as set on Cloudflare, without leading/trailing whitespace: a secret pasted with a trailing newline or
// space still matches, and so does a key typed or pasted with one (11-22).
function ownerKey(env) {
  return String(env.OWNER_KEY ?? "").trim();
}

async function ownerOk(request, env) {
  const key = (request.headers.get("x-ma2-owner-key") || "").trim();
  return ownerKey(env).length > 0 && key.length > 0 && (await sameSecret(key, ownerKey(env)));
}

function gate(env) {
  return env.GATE.get(env.GATE.idFromName("gate"));
}

async function gateCall(env, path, body) {
  const r = await gate(env).fetch("https://gate" + path, {
    method: body === undefined ? "GET" : "POST",
    headers: { "content-type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return r.json();
}

// ---------------------------------------------------------------- GitHub

function issueTitle(id, meta) {
  return `[${id}] ${meta.kind} ${meta.version} ${meta.signature}`.trim().slice(0, 200);
}

function issueBody(rec, env) {
  const base = (env.PUBLIC_URL || "").replace(/\/+$/, "");
  const open = base ? `${base}/v1/open/${rec.id}` : `/v1/open/${rec.id}`;
  const withText = String(env.ISSUE_PLAYER_TEXT || "off").toLowerCase() === "on";
  const textPart = withText
    ? ["**Player's text**", "", "```text", neutralize(rec.text), "```", ""]
    : [`**Player's text**: ${String(rec.text || "").trim() ? "yes" : "none"} (kept with the report, not here: \`python triage.py show ${rec.id}\`)`, ""];
  return [
    `**Report** \`${rec.id}\` | client \`${rec.client}\` | version \`${rec.version}\` | build \`${rec.build || "-"}\` | received ${rec.received}`,
    "",
    ...textPart,
    "**System summary**",
    "",
    "```text",
    neutralize(rec.summary),
    "```",
    "",
    `**Zip** (${rec.zip_bytes} bytes; the report and this issue are deleted ${KEEP_DAYS} days after the last message): R2 key \`${rec.key}\``,
    "",
    `Download: ${open} (opens in your browser; asks for your owner key the first time)`,
    "",
    rec.replies
      ? `Replies: the player can read a reply and answer it. Reply on the same page.`
      : `Replies: not possible (sent by a version without replies).`,
  ].join("\n");
}

function ghHeaders(env) {
  return {
    authorization: `Bearer ${env.GITHUB_TOKEN}`,
    accept: "application/vnd.github+json",
    "x-github-api-version": "2022-11-28",
    "user-agent": USER_AGENT,
    "content-type": "application/json",
  };
}

// Opens the issue for one pending marker. Deletes the marker on success. Returns {ok, status}.
export async function openIssue(env, marker, { search = false } = {}) {
  if (!env.GITHUB_TOKEN || !env.GITHUB_REPO) return { ok: false, status: 0 };
  try {
    if (search) {
      const q = encodeURIComponent(`repo:${env.GITHUB_REPO} in:title "${marker.id}"`);
      const s = await fetch(`${GITHUB_API}/search/issues?q=${q}`, {
        headers: ghHeaders(env),
        signal: AbortSignal.timeout(10000),
      });
      if (TOKEN_DEAD.has(s.status)) return await markFailed(env, marker, s.status);
      if (s.ok) {
        const found = await s.json();
        if ((found.total_count ?? 0) > 0) {
          await rememberIssue(env, marker, found.items && found.items[0]);
          await env.REPORTS.delete(`pending/${marker.id}.json`);
          return { ok: true, status: 200, existing: true };
        }
      }
    }
    const r = await fetch(`${GITHUB_API}/repos/${env.GITHUB_REPO}/issues`, {
      method: "POST",
      headers: ghHeaders(env),
      body: JSON.stringify({ title: marker.title, body: marker.body, labels: marker.labels }),
      signal: AbortSignal.timeout(10000),
    });
    if (r.status === 201) {
      let made = null;
      try {
        made = await r.json();
      } catch (e) {
        /* the issue exists; without its number the cron finds it by title later */
      }
      await rememberIssue(env, marker, made);
      await env.REPORTS.delete(`pending/${marker.id}.json`);
      return { ok: true, status: 201 };
    }
    return await markFailed(env, marker, r.status);
  } catch (e) {
    return await markFailed(env, marker, 0);
  }
}

// issues/<id>.json = {id, received, number, node_id}: what the retention cron and the delete route need.
async function rememberIssue(env, marker, issue) {
  const rec = { id: marker.id, received: marker.received, number: issue?.number ?? null, node_id: issue?.node_id ?? null };
  try {
    await env.REPORTS.put(`issues/${marker.id}.json`, JSON.stringify(rec), { httpMetadata: { contentType: "application/json" } });
  } catch (e) {
    /* the cron falls back to a title search */
  }
}

async function findIssue(env, id) {
  const s = await fetch(`${GITHUB_API}/search/issues?q=${encodeURIComponent(`repo:${env.GITHUB_REPO} in:title "${id}"`)}`, {
    headers: ghHeaders(env),
    signal: AbortSignal.timeout(10000),
  });
  if (!s.ok) return { status: s.status };
  const found = await s.json();
  const hit = (found.items || []).find((x) => String(x.title || "").includes(`[${id}]`));
  return { status: 200, issue: hit ? { number: hit.number, node_id: hit.node_id } : null };
}

// Deletes the issue when the token may (GraphQL deleteIssue needs admin rights on the repo), else replaces its title and
// body, closes and locks it. Returns {ok, how: "deleted"|"emptied"|"gone"|"none"|"failed", status}.
export async function retireIssue(env, id, rec, why) {
  if (!env.GITHUB_TOKEN || !env.GITHUB_REPO) return { ok: false, how: "failed", status: 0 };
  try {
    let issue = rec && rec.number ? { number: rec.number, node_id: rec.node_id } : null;
    if (!issue) {
      const f = await findIssue(env, id);
      if (f.status !== 200) return { ok: false, how: "failed", status: f.status };
      if (!f.issue) return { ok: true, how: "none", status: 200 };
      issue = f.issue;
    }
    if (issue.node_id && String(env.DELETE_ISSUES || "on").toLowerCase() !== "off") {
      const g = await fetch(`${GITHUB_API}/graphql`, {
        method: "POST",
        headers: ghHeaders(env),
        body: JSON.stringify({ query: "mutation($id:ID!){deleteIssue(input:{issueId:$id}){clientMutationId}}", variables: { id: issue.node_id } }),
        signal: AbortSignal.timeout(10000),
      });
      if (g.ok) {
        const b = await g.json().catch(() => ({}));
        if (!b.errors) return { ok: true, how: "deleted", status: 200 };
        if (b.errors.some((e) => e.type === "NOT_FOUND")) return { ok: true, how: "gone", status: 200 };
      } else if (g.status === 401) return { ok: false, how: "failed", status: 401 };
      // no rights to delete (FORBIDDEN): empty it instead
    }
    // The player's answers were comments: delete them first (an emptied issue would otherwise keep them).
    for (const cid of (rec && Array.isArray(rec.comments) ? rec.comments : [])) {
      const d = await fetch(`${GITHUB_API}/repos/${env.GITHUB_REPO}/issues/comments/${Number(cid)}`, {
        method: "DELETE",
        headers: ghHeaders(env),
        signal: AbortSignal.timeout(10000),
      });
      if (!d.ok && d.status !== 404 && d.status !== 410) return { ok: false, how: "failed", status: d.status };
    }
    const base = `${GITHUB_API}/repos/${env.GITHUB_REPO}/issues/${issue.number}`;
    const u = await fetch(base, {
      method: "PATCH",
      headers: ghHeaders(env),
      body: JSON.stringify({ title: `[${id}] removed`, body: `Report ${id} was removed (${why}).`, state: "closed", labels: [] }),
      signal: AbortSignal.timeout(10000),
    });
    if (u.status === 404 || u.status === 410) return { ok: true, how: "gone", status: u.status };
    if (!u.ok) return { ok: false, how: "failed", status: u.status };
    await fetch(`${base}/lock`, {
      method: "PUT",
      headers: ghHeaders(env),
      body: JSON.stringify({ lock_reason: "resolved" }),
      signal: AbortSignal.timeout(10000),
    }).catch(() => null);
    return { ok: true, how: "emptied", status: 200 };
  } catch (e) {
    return { ok: false, how: "failed", status: 0 };
  }
}

// Deletes every stored object of one report: zip, sidecar, pending marker. Returns what was there.
async function deleteStored(env, id) {
  const out = { zip: false, info: false, pending: false };
  const conv = await readConv(env, id);
  if (conv) {
    await dropConversation(env, id, conv);
    if (conv.key) for (const k of [conv.key, conv.key.replace(/\.zip$/, ".json")]) {
      if (await env.REPORTS.head(k)) {
        await env.REPORTS.delete(k);
        out[k.endsWith(".zip") ? "zip" : "info"] = true;
      }
    }
  }
  for (const m of lastMonths(5)) {
    for (const [ext, k] of [["zip", "zip"], ["json", "info"]]) {
      const key = `reports/${m}/${id}.${ext}`;
      if (await env.REPORTS.head(key)) {
        await env.REPORTS.delete(key);
        out[k] = true;
      }
    }
  }
  if (await env.REPORTS.head(`pending/${id}.json`)) {
    await env.REPORTS.delete(`pending/${id}.json`);
    out.pending = true;
  }
  return out;
}

async function readJson(env, key) {
  const o = await env.REPORTS.get(key);
  if (!o) return null;
  try {
    return await o.json();
  } catch (e) {
    return {};
  }
}

// The retention run: report objects older than KEEP_DAYS are deleted, their issues retired; issues queued by a delete
// request that could not reach GitHub are retried. Stops early when the token is dead (the rest would fail the same way).
export async function expireOld(env, now = Date.now()) {
  const res = { objects: 0, issues: 0, emptied: 0, deleted: 0, failed: 0, stopped: false, conversations: 0, refreshed: 0, comments: 0 };
  const cutoff = now - KEEP_DAYS * 86400e3;
  const refreshAt = now - REFRESH_DAYS * 86400e3;
  // 0. Conversations (1.2.0): a report with a conversation lives until its last message + KEEP_DAYS. Ended ones are
  //    dropped here with everything they hold (report zip and sidecar included, even when a refresh made them look new).
  const live = new Set();
  let cc;
  do {
    const l = await env.REPORTS.list({ prefix: "conv/", cursor: cc, limit: 1000, include: ["customMetadata"] });
    for (const o of l.objects) {
      const id = o.key.slice(5, -5);
      let last = Date.parse((o.customMetadata || {}).last || "");
      let conv = null;
      if (!Number.isFinite(last)) {
        conv = await readConv(env, id);
        last = Date.parse(conv?.last || "") || new Date(o.uploaded).getTime();
      }
      if (last < cutoff) {
        conv = conv || (await readConv(env, id));
        await dropConversation(env, id, conv);
        if (conv && conv.key) await env.REPORTS.delete([conv.key, conv.key.replace(/\.zip$/, ".json")]);
        res.conversations++;
        continue;
      }
      live.add(id);
      if (res.comments < COMMENTS_PER_RUN && (o.customMetadata || {}).comments === "pending") {
        conv = conv || (await readConv(env, id));
        if (conv) res.comments += await postPendingComments(env, conv);
      }
    }
    cc = l.truncated ? l.cursor : undefined;
  } while (cc);
  // 1. R2 report objects (zip + sidecar) and answer bundles. A live conversation's objects are kept, and re-written once
  //    they are REFRESH_DAYS old so the bucket's 60-day lifecycle rule never deletes them.
  for (const prefix of ["reports/", "answers/"]) {
    let c;
    do {
      const l = await env.REPORTS.list({ prefix, cursor: c, limit: 1000 });
      for (const o of l.objects) {
        if (res.objects >= EXPIRE_PER_RUN) break;
        const id = prefix === "reports/" ? o.key.split("/").pop().replace(/\.(zip|json)$/, "") : o.key.split("/")[1];
        const up = new Date(o.uploaded).getTime();
        if (live.has(id)) {
          if (up < refreshAt && res.refreshed < REFRESH_PER_RUN) {
            await refreshObject(env, o.key);
            res.refreshed++;
          }
          continue;
        }
        // answers/ without a live conversation are left-overs of a deleted one
        if (prefix === "answers/" || up < cutoff) {
          await env.REPORTS.delete(o.key);
          res.objects++;
        }
      }
      c = l.truncated && res.objects < EXPIRE_PER_RUN ? l.cursor : undefined;
    } while (c);
  }
  // 2. Issues: queued deletions first, then the expired ones.
  const work = [];
  for (const prefix of ["retire/", "issues/"]) {
    const l = await env.REPORTS.list({ prefix, limit: 1000 });
    for (const o of l.objects) {
      if (work.length >= RETIRE_PER_RUN) break;
      const rec = await readJson(env, o.key);
      if (!rec) continue;
      const id = o.key.slice(prefix.length, -5);
      if (prefix === "issues/" && (!(Date.parse(rec.received) < cutoff) || live.has(id))) continue;
      work.push({ key: o.key, id, rec, why: prefix === "retire/" ? "deleted on request" : `after ${KEEP_DAYS} days` });
    }
  }
  for (const w of work) {
    const r = await retireIssue(env, w.id, w.rec, w.why);
    if (r.ok) {
      await env.REPORTS.delete(w.key);
      if (w.key.startsWith("retire/")) await env.REPORTS.delete(`issues/${w.id}.json`);
      res.issues++;
      if (r.how === "deleted") res.deleted++;
      if (r.how === "emptied") res.emptied++;
    } else {
      res.failed++;
      if (TOKEN_DEAD.has(r.status) || (r.status === 0 && !env.GITHUB_TOKEN)) {
        res.stopped = true;
        break;
      }
    }
  }
  return res;
}

async function markFailed(env, marker, status) {
  const m = { ...marker, attempts: (marker.attempts || 0) + 1, last_status: status, last_try: new Date().toISOString() };
  try {
    await env.REPORTS.put(`pending/${marker.id}.json`, JSON.stringify(m), {
      httpMetadata: { contentType: "application/json" },
    });
  } catch (e) {
    /* the old marker stays; the next run tries again */
  }
  return { ok: false, status };
}

export async function retryPending(env, limit = 50) {
  const res = { tried: 0, opened: 0, failed: 0, expired: 0, stopped: false };
  const list = await env.REPORTS.list({ prefix: "pending/", limit });
  const cutoff = Date.now() - KEEP_DAYS * 86400e3;
  for (const o of list.objects) {
    const obj = await env.REPORTS.get(o.key);
    if (!obj) continue;
    let marker;
    try {
      marker = await obj.json();
    } catch (e) {
      await env.REPORTS.delete(o.key);
      continue;
    }
    if (Date.parse(marker.received) < cutoff) {
      await env.REPORTS.delete(o.key); // the zip is gone by now too
      res.expired++;
      continue;
    }
    res.tried++;
    const r = await openIssue(env, marker, { search: true });
    if (r.ok) res.opened++;
    else {
      res.failed++;
      if (TOKEN_DEAD.has(r.status) || (r.status === 0 && !env.GITHUB_TOKEN)) {
        res.stopped = true; // token missing, expired or lacking rights: the rest would fail the same way
        break;
      }
    }
  }
  return res;
}

// ---------------------------------------------------------------- upload

async function handleUpload(request, env, ctx) {
  const maxBytes = intVar(env, "MAX_UPLOAD_BYTES", 25 * 1024 * 1024);
  if (String(env.ACCEPT_REPORTS || "on").toLowerCase() === "off") return fail(503, env.OFF_MESSAGE || MSG.off);

  const ctype = (request.headers.get("content-type") || "").toLowerCase();
  if (!ctype.startsWith("multipart/form-data")) return fail(415, MSG.bad);
  const lenHeader = request.headers.get("content-length");
  if (lenHeader === null) return fail(411, MSG.bad);
  if (!/^\d+$/.test(lenHeader)) return fail(400, MSG.bad);
  if (Number(lenHeader) > maxBytes) return fail(413, MSG.big);

  const hdr = {
    client: request.headers.get("x-ma2-client"),
    version: request.headers.get("x-ma2-version"),
    build: request.headers.get("x-ma2-build"),
    kind: request.headers.get("x-ma2-kind"),
    signature: request.headers.get("x-ma2-signature"),
  };
  // Reject wrong client kinds before spending the rate budget or reading the body.
  if (hdr.client !== null && !CLIENTS.has(hdr.client.trim().toLowerCase())) return fail(400, MSG.bad);
  if (hdr.kind !== null && !KINDS.has(hdr.kind.trim().toLowerCase())) return fail(400, MSG.bad);
  if (hdr.build !== null && testBuild(env, hdr.build)) return fail(403, MSG.test);

  const ip = request.headers.get("cf-connecting-ip") || "unknown";
  let admit;
  try {
    admit = await gateCall(env, "/admit", {
      ip,
      cap: intVar(env, "DAILY_CAP", 300),
      perHour: intVar(env, "PER_IP_PER_HOUR", 5),
    });
  } catch (e) {
    return fail(503, MSG.store);
  }
  if (!admit.ok) {
    if (admit.reason === "off") return fail(503, admit.message || env.OFF_MESSAGE || MSG.off);
    return fail(429, admit.reason === "daily" ? MSG.busy : MSG.ip);
  }

  let form;
  try {
    form = await request.formData();
  } catch (e) {
    return fail(400, MSG.bad);
  }
  const field = (k) => {
    const v = form.get(k);
    return typeof v === "string" ? v : null;
  };
  const checked = cleanMeta({
    client: hdr.client ?? field("client"),
    version: hdr.version ?? field("version"),
    build: hdr.build ?? field("build"),
    kind: hdr.kind ?? field("kind"),
    signature: hdr.signature ?? field("signature"),
  });
  if (checked.error) return fail(400, MSG.bad);
  const meta = checked.meta;
  if (testBuild(env, meta.build)) return fail(403, MSG.test);

  const zip = form.get("zip");
  if (!zip || typeof zip === "string" || typeof zip.arrayBuffer !== "function") return fail(400, MSG.bad);
  if (zip.size > maxBytes) return fail(413, MSG.big);
  if (zip.size < 22) return fail(400, MSG.bad);
  const head = new Uint8Array(await zip.slice(0, 4).arrayBuffer());
  const isZip = head[0] === 0x50 && head[1] === 0x4b &&
    ((head[2] === 3 && head[3] === 4) || (head[2] === 5 && head[3] === 6));
  if (!isZip) return fail(400, MSG.bad);

  const textRaw = form.get("text");
  const sumRaw = form.get("summary");
  if ((textRaw !== null && typeof textRaw !== "string") || (sumRaw !== null && typeof sumRaw !== "string"))
    return fail(400, MSG.bad);
  // Multipart turns newlines into CRLF; store plain LF.
  const text = (textRaw || "").replace(/\r\n?/g, "\n").slice(0, MAX_TEXT);
  const summary = (sumRaw || "").replace(/\r\n?/g, "\n").slice(0, MAX_SUMMARY);
  // 1.2.0: the client's conversation key. Absent (1.1.x) or malformed = a report without replies, as before.
  const secretRaw = field("reply_secret");
  const secret = secretRaw && SECRET_RE.test(secretRaw.trim().toLowerCase()) ? secretRaw.trim().toLowerCase() : null;

  const live = await currentLatest(env);
  if (env.MIN_VERSION && versionLess(meta.version, env.MIN_VERSION))
    return fail(503, env.UPDATE_NOTICE || live.notice || MSG.off);
  const quiet = await quietReport(meta, text, summary, zip);

  const now = new Date();
  const month = monthOf(now);
  let id, key;
  try {
    for (let i = 0; i < 5; i++) {
      id = newId();
      key = `reports/${month}/${id}.zip`;
      if (!(await env.REPORTS.head(key))) break;
      id = null;
    }
    if (!id) return fail(500, MSG.store);
    const received = now.toISOString();
    const custom = { ...meta, received, issue: quiet ? "none" : "pending" };
    await env.REPORTS.put(key, zip, {
      httpMetadata: { contentType: "application/zip" },
      customMetadata: custom,
    });
    const rec = { id, key, received, ...meta, text, summary, zip_bytes: zip.size, replies: !!secret && !quiet };
    if (quiet) rec.issue = "none";
    await env.REPORTS.put(`reports/${month}/${id}.json`, JSON.stringify(rec), {
      httpMetadata: { contentType: "application/json" },
    });
    if (secret && !quiet)
      await writeConv(env, { v: 1, id, key, received, last: received, secret_hash: await sha256hex(secret), messages: [] });
    if (!quiet) {
      const marker = {
        id,
        received,
        title: issueTitle(id, meta),
        body: issueBody(rec, env),
        labels: [meta.kind, `v${meta.version}`, meta.client],
        attempts: 0,
      };
      await env.REPORTS.put(`pending/${id}.json`, JSON.stringify(marker), {
        httpMetadata: { contentType: "application/json" },
      });
      // The player gets the id now; the issue is opened after the reply. If that fails the marker stays and the
      // cron trigger retries it, so an expired token or a GitHub outage never loses a report.
      ctx.waitUntil(openIssue(env, marker));
    }
  } catch (e) {
    return fail(500, MSG.store);
  }

  let message = admit.message || env.NOTICE || "";
  if (live.version && live.notice && versionLess(meta.version, live.version))
    message = live.notice;
  return json({ ok: true, id, message });
}

// ---------------------------------------------------------------- report conversations (1.2.0)

const MAX_MESSAGES = 40;        // per conversation (20 rounds)
const CONV_MSG = {
  gone: "This conversation is not available any more.",
  turn: "There is no new message from the developer to answer.",
  empty: "The answer is empty.",
  long: "The answer is longer than 1000 characters.",
  full: "This conversation has too many messages.",
};
const MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

async function sha256hex(s) {
  return Array.from(await sha256(s), (x) => x.toString(16).padStart(2, "0")).join("");
}

// Plain text for a message: LF line ends, no control or bidi characters (tab becomes a space), trimmed.
export function cleanMessage(s) {
  return String(s ?? "")
    .replace(/\r\n?/g, "\n")
    .replace(/\t/g, " ")
    .replace(/[\u0000-\u0009\u000B-\u001F\u007F-\u009F\u2028\u2029\u202A-\u202E\u2066-\u2069]/g, "")
    .trim();
}

function fmtDate(iso, time = true) {
  const d = new Date(iso);
  if (!Number.isFinite(d.getTime())) return "";
  const day = `${MONTHS[d.getUTCMonth()]} ${d.getUTCDate()}`;
  return time ? `${day}, ${String(d.getUTCHours()).padStart(2, "0")}:${String(d.getUTCMinutes()).padStart(2, "0")} UTC` : day;
}

async function readConv(env, id) {
  if (!ID_RE.test(String(id))) return null;
  const c = await readJson(env, `conv/${id}.json`);
  return c && Array.isArray(c.messages) ? c : null;
}

// conv/<id>.json; custom metadata {last, comments: "pending"|""} lets the hourly sweep list activity without reading files.
async function writeConv(env, conv) {
  const pending = conv.messages.some((m) => m.comment === "pending") ? "pending" : "";
  await env.REPORTS.put(`conv/${conv.id}.json`, JSON.stringify(conv), {
    httpMetadata: { contentType: "application/json" },
    customMetadata: { last: conv.last, comments: pending },
  });
}

async function secretOk(conv, secret) {
  const s = String(secret ?? "").trim().toLowerCase();
  return !!conv && typeof conv.secret_hash === "string" && SECRET_RE.test(s) && (await sameSecret(await sha256hex(s), conv.secret_hash));
}

// Deletes a conversation and its answer bundles. The ids of the answer comments move into the issue record, so retiring
// the issue (deleted, or emptied + closed + locked) also deletes the comments that hold the player's words.
async function dropConversation(env, id, conv) {
  let c;
  do {
    const l = await env.REPORTS.list({ prefix: `answers/${id}/`, cursor: c, limit: 1000 });
    if (l.objects.length) await env.REPORTS.delete(l.objects.map((o) => o.key));
    c = l.truncated ? l.cursor : undefined;
  } while (c);
  await env.REPORTS.delete(`conv/${id}.json`);
  const ids = (conv?.messages || []).map((m) => m.comment).filter((x) => Number.isSafeInteger(x));
  if (ids.length) {
    const rec = await readJson(env, `issues/${id}.json`);
    if (rec) {
      rec.comments = [...new Set([...(rec.comments || []), ...ids])];
      await env.REPORTS.put(`issues/${id}.json`, JSON.stringify(rec), { httpMetadata: { contentType: "application/json" } });
    }
  }
}

// Copies an object onto itself (same bytes and metadata): a new upload time for the bucket's lifecycle rule.
async function refreshObject(env, key) {
  const o = await env.REPORTS.get(key);
  if (!o) return;
  await env.REPORTS.put(key, await o.arrayBuffer(), { httpMetadata: o.httpMetadata, customMetadata: o.customMetadata });
}

function commentBody(env, conv, m) {
  const base = (env.PUBLIC_URL || "").replace(/\/+$/, "");
  const prev = [...conv.messages].reverse().find((x) => x.from === "owner" && x.n < m.n);
  return [
    `**Player's answer** to your message of ${fmtDate(prev ? prev.at : conv.received, false)} (report \`${conv.id}\`)`,
    "",
    "```text",
    neutralize(m.text),
    "```",
    "",
    m.zip_key ? `Logs attached (.zip, ${Math.max(1, Math.round((m.zip_bytes || 0) / 1024))} KB): ${base}/v1/open/${conv.id}/logs/${m.n}` : "No logs attached.",
    "",
    `Reply: ${base}/v1/open/${conv.id}`,
  ].join("\n");
}

// Posts the player's answers that are not comments yet. Needs the issue's number (issues/<id>.json): while the issue
// itself is still pending, the answers wait and the hourly sweep tries again. Returns the number posted.
export async function postPendingComments(env, conv) {
  const todo = conv.messages.filter((m) => m.comment === "pending");
  if (!todo.length || !env.GITHUB_TOKEN || !env.GITHUB_REPO) return 0;
  const rec = await readJson(env, `issues/${conv.id}.json`);
  if (!rec || !rec.number) return 0;
  let posted = 0;
  for (const m of todo) {
    try {
      const r = await fetch(`${GITHUB_API}/repos/${env.GITHUB_REPO}/issues/${rec.number}/comments`, {
        method: "POST",
        headers: ghHeaders(env),
        body: JSON.stringify({ body: commentBody(env, conv, m) }),
        signal: AbortSignal.timeout(10000),
      });
      if (r.status !== 201) break;
      const made = await r.json().catch(() => ({}));
      m.comment = Number.isSafeInteger(made.id) ? made.id : "posted";
      posted++;
    } catch (e) {
      break;
    }
  }
  if (posted) {
    // Re-read so a message added meanwhile is not lost; only the comment fields change.
    const now = (await readConv(env, conv.id)) || conv;
    for (const m of now.messages) {
      const done = todo.find((t) => t.n === m.n && t.comment !== "pending");
      if (done) m.comment = done.comment;
    }
    await writeConv(env, now);
  }
  return posted;
}

// POST /v1/replies {reports: [{id, secret, seen?}]}: marks seen, returns the unseen owner messages of matching pairs.
export async function handleReplies(request, env) {
  if (request.method !== "POST") return fail(405, "POST only");
  const len = request.headers.get("content-length");
  if (len !== null && (!/^\d+$/.test(len) || Number(len) > 16384)) return json({ ok: false, replies: [] }, 413);
  let admit;
  try {
    admit = await gateCall(env, "/poll", { ip: request.headers.get("cf-connecting-ip") || "unknown", perHour: intVar(env, "REPLY_POLLS_PER_HOUR", 30) });
  } catch (e) {
    return json({ ok: false, replies: [] }, 503);
  }
  if (!admit.ok) return json({ ok: false, replies: [] }, 429);
  let b;
  try {
    const t = await request.text();
    if (t.length > 16384) return json({ ok: false, replies: [] }, 413);
    b = JSON.parse(t);
  } catch (e) {
    return json({ ok: false, replies: [] }, 400);
  }
  if (!b || !Array.isArray(b.reports)) return json({ ok: false, replies: [] }, 400);
  const out = [], state = [];
  const now = new Date().toISOString();
  for (const e of b.reports.slice(0, MAX_POLL)) {
    if (!e || typeof e !== "object" || !ID_RE.test(String(e.id))) continue;
    const conv = await readConv(env, e.id);
    if (!(await secretOk(conv, e.secret))) continue;
    const seen = Number.isSafeInteger(e.seen) ? e.seen : 0;
    let changed = false;
    for (const m of conv.messages)
      if (m.from === "owner" && !m.seen && m.n <= seen) {
        m.seen = now;
        changed = true;
      }
    if (changed) await writeConv(env, conv);
    const newest = conv.messages[conv.messages.length - 1];
    for (const m of conv.messages)
      if (m.from === "owner" && !m.seen)
        out.push({ id: conv.id, n: m.n, text: m.text, at: m.at, can_answer: m === newest });
    // 1.2.0 inbox: what the other client (installer / game) already did, so both show the same read and answered state.
    const ownerSeen = conv.messages.filter((m) => m.from === "owner" && m.seen).map((m) => m.n);
    state.push({ id: conv.id, seen_upto: ownerSeen.length ? Math.max(...ownerSeen) : 0, newest: newest ? newest.n : 0,
                 newest_from: newest ? newest.from : "" });
  }
  return json({ ok: true, replies: out, state });
}

// POST /v1/answer (multipart id, secret, text, [summary, zip]): the player's answer to the newest owner message.
export async function handleAnswer(request, env, ctx) {
  if (request.method !== "POST") return fail(405, "POST only");
  const maxBytes = intVar(env, "MAX_UPLOAD_BYTES", 25 * 1024 * 1024);
  if (String(env.ACCEPT_REPORTS || "on").toLowerCase() === "off") return fail(503, env.OFF_MESSAGE || MSG.off);
  const ctype = (request.headers.get("content-type") || "").toLowerCase();
  if (!ctype.startsWith("multipart/form-data")) return fail(415, MSG.bad);
  const lenHeader = request.headers.get("content-length");
  if (lenHeader === null) return fail(411, MSG.bad);
  if (!/^\d+$/.test(lenHeader)) return fail(400, MSG.bad);
  if (Number(lenHeader) > maxBytes) return fail(413, MSG.big);
  let admit;
  try {
    admit = await gateCall(env, "/admit", {
      ip: request.headers.get("cf-connecting-ip") || "unknown",
      cap: intVar(env, "DAILY_CAP", 300),
      perHour: intVar(env, "PER_IP_PER_HOUR", 5),
    });
  } catch (e) {
    return fail(503, MSG.store);
  }
  if (!admit.ok) {
    if (admit.reason === "off") return fail(503, admit.message || env.OFF_MESSAGE || MSG.off);
    return fail(429, admit.reason === "daily" ? MSG.busy : MSG.ip);
  }
  let form;
  try {
    form = await request.formData();
  } catch (e) {
    return fail(400, MSG.bad);
  }
  const field = (k) => {
    const v = form.get(k);
    return typeof v === "string" ? v : null;
  };
  const id = String(field("id") || "").trim();
  const conv = ID_RE.test(id) ? await readConv(env, id) : null;
  if (!(await secretOk(conv, field("secret")))) return fail(404, CONV_MSG.gone);
  const newest = conv.messages[conv.messages.length - 1];
  if (!newest || newest.from !== "owner") return fail(409, CONV_MSG.turn);
  if (conv.messages.length >= MAX_MESSAGES) return fail(409, CONV_MSG.full);
  const text = cleanMessage(field("text"));
  if (!text) return fail(400, CONV_MSG.empty);
  if (text.length > MAX_MESSAGE) return fail(400, CONV_MSG.long);
  const now = new Date().toISOString();
  const m = { n: newest.n + 1, from: "player", text, at: now, comment: "pending" };
  const zip = form.get("zip");
  if (zip !== null && zip !== undefined && typeof zip !== "string") {
    if (typeof zip.arrayBuffer !== "function") return fail(400, MSG.bad);
    if (zip.size > maxBytes) return fail(413, MSG.big);
    if (zip.size < 22) return fail(400, MSG.bad);
    const head = new Uint8Array(await zip.slice(0, 4).arrayBuffer());
    if (!(head[0] === 0x50 && head[1] === 0x4b && ((head[2] === 3 && head[3] === 4) || (head[2] === 5 && head[3] === 6)))) return fail(400, MSG.bad);
    const version = String(request.headers.get("x-ma2-version") || field("version") || "").trim();
    const build = String(request.headers.get("x-ma2-build") || field("build") || "").trim();
    const custom = { received: now, answer: String(m.n) };
    if (/^[0-9A-Za-z.+\-]{1,32}$/.test(version)) custom.version = version;
    if (/^[0-9A-Za-z.+\-]{1,64}$/.test(build)) custom.build = build;
    m.zip_key = `answers/${id}/${m.n}.zip`;
    m.zip_bytes = zip.size;
    m.summary = String(field("summary") || "").replace(/\r\n?/g, "\n").slice(0, MAX_SUMMARY);
    try {
      await env.REPORTS.put(m.zip_key, zip, { httpMetadata: { contentType: "application/zip" }, customMetadata: custom });
    } catch (e) {
      return fail(500, MSG.store);
    }
  }
  if (!newest.seen) newest.seen = now;     // answering it means it was read
  conv.messages.push(m);
  conv.last = now;
  try {
    await writeConv(env, conv);
  } catch (e) {
    return fail(500, MSG.store);
  }
  ctx.waitUntil(postPendingComments(env, conv).catch(() => 0));
  return json({ ok: true, id, n: m.n, message: "" });
}

// The owner's reply or edit. Returns {ok} or {error: key of OPEN_TEXT}.
export async function ownerPost(env, id, text, editN = null) {
  const conv = await readConv(env, id);
  if (!conv) return { error: (await findReport(env, id, "json")) ? "noreplies" : "missing" };
  const t = cleanMessage(text);
  if (!t) return { error: "empty" };
  if (t.length > MAX_MESSAGE) return { error: "toolong" };
  const now = new Date().toISOString();
  if (editN !== null && editN !== undefined && editN !== "") {
    const m = conv.messages.find((x) => x.n === Number(editN) && x.from === "owner");
    if (!m || m.seen) return { error: "seen_err" };
    m.text = t;
    m.edited = now;
  } else {
    if (conv.messages.some((x) => x.from === "owner" && !x.seen)) return { error: "wait" };
    if (conv.messages.length >= MAX_MESSAGES) return { error: "full" };
    const n = conv.messages.length ? conv.messages[conv.messages.length - 1].n + 1 : 1;
    conv.messages.push({ n, from: "owner", text: t, at: now });
  }
  conv.last = now;
  await writeConv(env, conv);
  return { ok: true };
}

// What the owner sees of a conversation (no secret hash).
function convView(conv) {
  if (!conv) return null;
  return {
    id: conv.id, received: conv.received, last: conv.last,
    messages: conv.messages.map((m) => ({
      n: m.n, from: m.from, text: m.text, at: m.at, ...(m.edited ? { edited: m.edited } : {}),
      ...(m.from === "owner" ? { seen: m.seen || null } : {}),
      ...(m.zip_key ? { logs_bytes: m.zip_bytes } : {}),
    })),
  };
}

function esc(s) {
  return String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
}

function kb(n) {
  return n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round((n || 0) / 1024))} KB`;
}

async function convPage(env, id, error = "", status = 200) {
  const info = await findReport(env, id, "json");
  const zip = await findReport(env, id, "zip", true);
  const conv = await readConv(env, id);
  if (!info && !zip && !conv) return page(404, `<p>${OPEN_TEXT.missing}</p><p class="small"><a href="/v1/open/forget">${OPEN_TEXT.forget}</a></p>`);
  let rec = {};
  try {
    rec = info ? await info.json() : {};
  } catch (e) {
    rec = {};
  }
  const parts = [];
  parts.push(`<p class="small">${esc(OPEN_TEXT.received(fmtDate(rec.received || conv?.received), rec.version, rec.kind))}</p>`);
  if (zip) parts.push(`<p><a class="dl" href="/v1/open/${id}/zip">${esc(OPEN_TEXT.download(kb(zip.size)))}</a></p>`);
  parts.push(`<h2>${OPEN_TEXT.conv}</h2>`);
  if (!conv) {
    parts.push(`<p class="note">${OPEN_TEXT.noreplies_note}</p>`);
  } else {
    if (!conv.messages.length) parts.push(`<p class="small">${OPEN_TEXT.none}</p>`);
    for (const m of conv.messages) {
      const who = m.from === "owner" ? OPEN_TEXT.you : OPEN_TEXT.player;
      let meta = `${who} · ${fmtDate(m.at)}`;
      if (m.edited) meta += ` · ${OPEN_TEXT.edited}`;
      if (m.from === "owner") meta += ` · ${m.seen ? OPEN_TEXT.seen(fmtDate(m.seen)) : OPEN_TEXT.unseen}`;
      let h = `<div class="msg ${m.from}"><div class="meta">${esc(meta)}</div><div class="text">${esc(m.text)}</div>`;
      if (m.zip_key) h += `<div class="small"><a href="/v1/open/${id}/logs/${m.n}">${esc(OPEN_TEXT.logs(kb(m.zip_bytes)))}</a></div>`;
      if (m.from === "owner" && !m.seen)
        h += `<details><summary>${OPEN_TEXT.edit}</summary><form method="post" action="/v1/open/${id}/edit">` +
          `<input type="hidden" name="n" value="${m.n}"><textarea name="text" maxlength="${MAX_MESSAGE}" rows="5" required>${esc(m.text)}</textarea>` +
          `<button type="submit">${OPEN_TEXT.save}</button></form></details>`;
      parts.push(h + `</div>`);
    }
    if (error) parts.push(`<p class="err" role="alert">${esc(typeof OPEN_TEXT[error] === "string" ? OPEN_TEXT[error] : OPEN_TEXT.wrongreply)}</p>`);
    if (conv.messages.some((x) => x.from === "owner" && !x.seen)) parts.push(`<p class="small">${OPEN_TEXT.wait}</p>`);
    else if (conv.messages.length >= MAX_MESSAGES) parts.push(`<p class="small">${OPEN_TEXT.full}</p>`);
    else
      parts.push(`<form method="post" action="/v1/open/${id}/reply"><label for="r">${OPEN_TEXT.reply}</label><br>` +
        `<textarea id="r" name="text" maxlength="${MAX_MESSAGE}" rows="6" required></textarea>` +
        `<p class="small">${OPEN_TEXT.reply_hint}</p><button type="submit">${OPEN_TEXT.send}</button></form>`);
  }
  parts.push(`<p class="small">${OPEN_TEXT.keep}</p><p class="small"><a href="/v1/open/forget">${OPEN_TEXT.forget}</a></p>`);
  return page(status, parts.join(""), {}, OPEN_TEXT.page_title(id));
}

// ---------------------------------------------------------------- one-click owner download (11-22)

const COOKIE = "ma2_owner";
const COOKIE_LABEL = "owner-cookie-v1";
const OPEN_RE = /^\/v1\/open\/([^/]+)(?:\/(zip|reply|edit|logs\/(\d{1,3})))?$/;
const PAGE_HEADERS = {
  "content-type": "text/html; charset=utf-8",
  "cache-control": "no-store",
  "x-frame-options": "DENY",
  "content-security-policy": "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'",
  // same-origin (not no-referrer): under no-referrer a browser sends "Origin: null" on the key form's POST,
  // which sameOrigin() refused, so the page never worked in a real browser. Nothing is sent to other sites.
  "referrer-policy": "same-origin",
  "x-content-type-options": "nosniff",
};
export const OPEN_TEXT = {
  title: "MechAssault 2 report download",
  ask: "This link downloads a player's report. Enter the owner key to continue.",
  key: "Owner key",
  remember: "Remember on this browser",
  button: "Download",
  wrong: "That key did not work.",
  busy: "Too many tries from this connection. Try again in an hour.",
  forget: "Forget this browser",
  forgotten: "This browser no longer remembers the owner key.",
  missing: "This report is not here. Reports are deleted 60 days after the last message.",
  // 1.2.0: the conversation page (owner only)
  page_title: (id) => `MechAssault 2 report ${id}`,
  received: (when, version, kind) => ["Received " + (when || "?"), version ? "game " + version : "", kind || ""].filter(Boolean).join(" · "),
  download: (size) => `Download the report (.zip, ${size})`,
  conv: "Conversation",
  none: "No messages yet.",
  you: "You",
  player: "Player",
  edited: "edited",
  seen: (when) => `seen ${when}`,
  unseen: "not seen yet",
  logs: (size) => `Logs attached: download (.zip, ${size})`,
  edit: "Edit",
  save: "Save changes",
  reply: "Reply to the player",
  reply_hint: "Plain text, at most 1000 characters. The player sees it the next time they start the game or the installer.",
  send: "Send reply",
  wait: "Your last message has not been seen yet. You can still edit it.",
  noreplies_note: "This report came from a version without replies, so the player can't see a reply.",
  full: "This conversation has reached its message limit.",
  keep: "Reports and conversations are deleted 60 days after the last message.",
  empty: "The reply is empty.",
  toolong: "The reply is longer than 1000 characters.",
  noreplies: "This report came from a version without replies, so the player can't see a reply.",
  seen_err: "The player has already seen that message, so it can't be changed any more.",
  wrongreply: "The reply was not saved.",
};

export async function ownerCookieValue(env) {
  const k = await crypto.subtle.importKey("raw", new TextEncoder().encode(ownerKey(env)), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const mac = new Uint8Array(await crypto.subtle.sign("HMAC", k, new TextEncoder().encode(COOKIE_LABEL)));
  return Array.from(mac, (x) => x.toString(16).padStart(2, "0")).join("");
}

function cookieOf(request, name) {
  for (const part of (request.headers.get("cookie") || "").split(";")) {
    const i = part.indexOf("=");
    if (i > 0 && part.slice(0, i).trim() === name) return part.slice(i + 1).trim();
  }
  return "";
}

async function cookieOk(request, env) {
  const v = cookieOf(request, COOKIE);
  return v.length > 0 && (await sameSecret(v, await ownerCookieValue(env)));
}

function page(status, body, extra = {}, title = OPEN_TEXT.title) {
  return new Response(
    `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">` +
      `<meta name="robots" content="noindex"><title>${esc(title)}</title><style>` +
      `body{font:16px/1.5 system-ui,sans-serif;max-width:40rem;margin:3rem auto;padding:0 1rem;color:#1a1a1a;background:#fff}` +
      `input[type=password],textarea{width:100%;padding:.5rem;font:inherit;box-sizing:border-box}button{margin-top:1rem;padding:.5rem 1.2rem;font:inherit}` +
      `.err{color:#a00}.small{font-size:.9rem;color:#555}h2{font-size:1.1rem;margin-top:2rem}` +
      `.msg{border-left:3px solid #8aa;padding:.4rem .8rem;margin:.8rem 0;background:#f5f7f8}.msg.player{border-color:#c80}` +
      `.meta{font-size:.85rem;color:#555}.text{white-space:pre-wrap;overflow-wrap:anywhere}details{margin-top:.4rem}.note{color:#555}` +
      `@media(prefers-color-scheme:dark){body{background:#16181c;color:#e8e8e8}.small,.meta,.note{color:#aaa}.err{color:#f77}` +
      `.msg{background:#20242a}a{color:#8cf}}` +
      `</style></head><body><h1 style="font-size:1.3rem">${esc(title)}</h1>${body}</body></html>`,
    { status, headers: { ...PAGE_HEADERS, ...extra } },
  );
}

function askPage(id, status, error) {
  return page(
    status,
    `<p>${OPEN_TEXT.ask}</p><p class="small">${id}</p>` +
      (error ? `<p class="err" role="alert">${error}</p>` : "") +
      `<form method="post" action="/v1/open/${id}"><label for="k">${OPEN_TEXT.key}</label><br>` +
      `<input id="k" name="key" type="password" autocomplete="current-password" required autofocus>` +
      `<p><label><input name="remember" type="checkbox" value="1" checked> ${OPEN_TEXT.remember}</label></p>` +
      `<button type="submit">${OPEN_TEXT.button}</button></form>`,
  );
}

function sameOrigin(request, url) {
  const origin = request.headers.get("origin");
  const site = request.headers.get("sec-fetch-site");
  // "null" is what a browser sends under a no-referrer policy (e.g. a page cached before the policy changed);
  // accept it only when the browser itself says the request came from this site.
  if (origin === "null") return site === "same-origin";
  if (origin !== null && origin !== url.origin) return false;
  return !(site && site !== "same-origin" && site !== "none");
}

export async function handleOpen(request, env, url) {
  if (!ownerKey(env)) return fail(404, "not found");
  const m = url.pathname.match(OPEN_RE);
  if (!m) return fail(404, "not found");
  const cookieBase = `${COOKIE}=; HttpOnly; Secure; SameSite=Lax; Path=/v1/`;
  if (m[1] === "forget") {
    if (request.method !== "GET" || m[2]) return fail(405, "GET only");
    return page(200, `<p>${OPEN_TEXT.forgotten}</p>`, { "set-cookie": `${cookieBase}; Max-Age=0` });
  }
  const id = m[1];
  if (!ID_RE.test(id)) return fail(404, "not found");
  const forgetLink = `<p class="small"><a href="/v1/open/forget">${OPEN_TEXT.forget}</a></p>`;
  const sub = m[2] || "";

  if (request.method === "GET" || request.method === "HEAD") {
    if (sub === "reply" || sub === "edit") return fail(405, "POST only");
    if (!(await cookieOk(request, env))) return askPage(id, 200, "");
    if (!sub) return convPage(env, id);          // 1.2.0: the conversation page, with the download link
    let obj, name;
    if (sub === "zip") {
      obj = await findReport(env, id, "zip");
      name = `${id}.zip`;
    } else {
      obj = await env.REPORTS.get(`answers/${id}/${Number(m[3])}.zip`);
      name = `${id}-answer-${Number(m[3])}.zip`;
    }
    if (!obj) return page(404, `<p>${OPEN_TEXT.missing}</p>${forgetLink}`);
    return new Response(request.method === "HEAD" ? null : obj.body, {
      headers: {
        "content-type": "application/zip",
        "content-disposition": `attachment; filename="${name}"`,
        "cache-control": "no-store",
        "referrer-policy": "no-referrer",
        "x-content-type-options": "nosniff",
      },
    });
  }

  if (request.method !== "POST") return fail(405, "GET or POST only");
  if (!sameOrigin(request, url)) return fail(403, "not from this page");
  const ctype = (request.headers.get("content-type") || "").toLowerCase();
  if (!ctype.startsWith("application/x-www-form-urlencoded")) return fail(415, "form expected");
  if (sub === "reply" || sub === "edit") {
    // The owner's reply or edit: needs the cookie (Lax: never sent on a POST from another site) and this page's origin.
    if (!(await cookieOk(request, env))) return askPage(id, 401, "");
    let form;
    try {
      form = await request.formData();
    } catch (e) {
      return convPage(env, id, "wrongreply", 400);
    }
    const r = await ownerPost(env, id, form.get("text"), sub === "edit" ? form.get("n") : null);
    if (r.error) return convPage(env, id, r.error, r.error === "missing" ? 404 : 400);
    return new Response(null, { status: 303, headers: { location: `/v1/open/${id}`, "cache-control": "no-store", "referrer-policy": "no-referrer" } });
  }
  if (sub) return fail(405, "GET only");
  let admit;
  try {
    admit = await gateCall(env, "/attempt", { ip: request.headers.get("cf-connecting-ip") || "unknown", perHour: intVar(env, "KEY_TRIES_PER_HOUR", 10) });
  } catch (e) {
    return askPage(id, 503, OPEN_TEXT.busy);
  }
  if (!admit.ok) return askPage(id, 429, OPEN_TEXT.busy);
  let form;
  try {
    form = await request.formData();
  } catch (e) {
    return askPage(id, 400, OPEN_TEXT.wrong);
  }
  const key = String(form.get("key") ?? "").trim();
  if (!key || key.length > 512 || !(await sameSecret(key, ownerKey(env)))) return askPage(id, 401, OPEN_TEXT.wrong);
  const remember = form.get("remember") === "1";
  const cookie = `${COOKIE}=${await ownerCookieValue(env)}; HttpOnly; Secure; SameSite=Lax; Path=/v1/` + (remember ? "; Max-Age=31536000" : "");
  return new Response(null, {
    status: 303,
    headers: { location: `/v1/open/${id}`, "set-cookie": cookie, "cache-control": "no-store", "referrer-policy": "no-referrer" },
  });
}

// ---------------------------------------------------------------- latest version (1.1.7)

// Where the newest release comes from (1.1.7, self-updating installer): the owner announces it with
// `triage.py announce` (POST /v1/admin/latest), which writes config/latest.json in the bucket; no dashboard step and no
// redeploy per release. When that file is absent or invalid, the vars LATEST_VERSION, LATEST_SHA256, LATEST_SIZE and
// UPDATE_NOTICE are used (the 1.1.7 first-deploy behaviour). sha256 + size describe the release's installer asset
// (INSTALLER_ASSET on the GitHub release v<latest>): the installer downloads that file from GitHub only and runs it only
// when its SHA-256 and size equal these values, so GitHub and this relay (two channels) must agree.
// INSTALLER_ASSET is a LABEL, not the file name: 1.1.7 installers require exactly this string in "asset", so it stays (and
// triage.py announce keeps posting it). The installer file is "MechAssault 2 Expanded Installer.exe" since the 1.2.0 in-place
// fix; GitHub publishes it as "MechAssault.2.Expanded.Installer.exe" (spaces become dots). Fixed installers accept this label or
// the new name and try, on a 404 only: the new name published, as uploaded, then this old name published
// ("MechAssault.2.Recomp.Installer.exe") and as uploaded. 1.1.7 and the first 1.2.0 ask only for the old name with spaces (404).
export const INSTALLER_ASSET = "MechAssault 2 Recomp Installer.exe";
export const INSTALLER_MIN_BYTES = 1024 * 1024;
export const INSTALLER_MAX_BYTES = 200 * 1024 * 1024;
const ANNOUNCE_RE = /^\d{1,4}\.\d{1,4}\.\d{1,4}$/;    // what an announce may set: a plain X.Y.Z release
const SHA256_RE = /^[0-9a-f]{64}$/;
const LATEST_KEY = "config/latest.json";
const LATEST_PREV_KEY = "config/latest.prev.json";
const LATEST_TTL_MS = 60 * 1000;
const NOTICE_MAX = 200;
const NOTES_MAX = 6000;                                // the Changelog section of one release (1.2.0 Messages)
const latestCache = new WeakMap();                     // env -> {at, value}: one bucket read per minute per isolate

function validSize(n) {
  return Number.isSafeInteger(n) && n >= INSTALLER_MIN_BYTES && n <= INSTALLER_MAX_BYTES;
}

// {value} or {error}: the strict check of an announce (and of the stored file when it is read back).
export function validateLatest(b) {
  if (!b || typeof b !== "object" || Array.isArray(b)) return { error: "JSON body {version, sha256, size, asset, notice} expected" };
  const version = typeof b.version === "string" ? b.version.trim() : "";
  if (!ANNOUNCE_RE.test(version)) return { error: "version must be X.Y.Z (plain numbers, no preview)" };
  const sha256 = typeof b.sha256 === "string" ? b.sha256.trim().toLowerCase() : "";
  if (!SHA256_RE.test(sha256)) return { error: "sha256 must be 64 hex digits" };
  const size = typeof b.size === "number" ? b.size : NaN;
  if (!validSize(size)) return { error: `size must be a whole number of bytes from ${INSTALLER_MIN_BYTES} to ${INSTALLER_MAX_BYTES}` };
  if (b.asset !== undefined && b.asset !== INSTALLER_ASSET) return { error: `asset must be "${INSTALLER_ASSET}"` };
  let notice = "";
  if (b.notice !== undefined && b.notice !== null) {
    if (typeof b.notice !== "string") return { error: "notice must be text" };
    notice = b.notice.trim();
    if (notice.length > NOTICE_MAX) return { error: `notice must be at most ${NOTICE_MAX} characters` };
    if (/[\u0000-\u001F\u007F-\u009F\u2028\u2029\u202A-\u202E\u2066-\u2069]/.test(notice)) return { error: "notice must not contain control characters" };
  }
  // 1.2.0: the release's Changelog section (plain text, line breaks kept), shown in the game's Messages.
  let notes = "";
  if (b.notes !== undefined && b.notes !== null) {
    if (typeof b.notes !== "string") return { error: "notes must be text" };
    notes = b.notes.replace(/\r\n?/g, "\n").replace(/\t/g, "  ").trim();
    if (notes.length > NOTES_MAX) return { error: `notes must be at most ${NOTES_MAX} characters` };
    if (/[\u0000-\u0009\u000B-\u001F\u007F-\u009F\u2028\u2029\u202A-\u202E\u2066-\u2069]/.test(notes)) return { error: "notes must not contain control characters" };
  }
  const value = { version, sha256, size, asset: INSTALLER_ASSET };
  if (notice) value.notice = notice;
  if (notes) value.notes = notes;
  return { value };
}

function latestFromEnv(env) {
  const v = String(env.LATEST_VERSION ?? "").trim();
  const out = { source: "env", version: RELEASE_RE.test(v) ? v : null };
  const notice = String(env.UPDATE_NOTICE ?? "").trim();
  if (notice) out.notice = notice.slice(0, 300);
  const sha = String(env.LATEST_SHA256 ?? "").trim().toLowerCase();
  const size = Number(String(env.LATEST_SIZE ?? "").trim() || NaN);
  if (out.version && SHA256_RE.test(sha) && validSize(size)) Object.assign(out, { sha256: sha, size });
  return out;
}

async function readLatestFile(env, key = LATEST_KEY) {
  try {
    const obj = await env.REPORTS.get(key);
    if (!obj) return null;
    const r = validateLatest(JSON.parse(await obj.text()));
    return r.value ? { ...r.value, set_at: null } : null;
  } catch (e) {
    return null;
  }
}

// The live newest release: {source: "file"|"env", version|null, sha256?, size?, notice?}.
export async function currentLatest(env, fresh = false) {
  const c = latestCache.get(env);
  if (!fresh && c && Date.now() - c.at < LATEST_TTL_MS) return c.value;
  const file = await readLatestFile(env);
  const value = file
    ? { source: "file", version: file.version, sha256: file.sha256, size: file.size, ...(file.notice ? { notice: file.notice } : {}),
        ...(file.notes ? { notes: file.notes } : {}) }
    : latestFromEnv(env);
  latestCache.set(env, { at: Date.now(), value });
  return value;
}

function latestBody(cur) {
  const body = { ok: true, latest: cur.version || null };
  if (body.latest && cur.notice) body.notice = cur.notice;
  if (body.latest && cur.sha256 && cur.size) Object.assign(body, { sha256: cur.sha256, size: cur.size, asset: INSTALLER_ASSET });
  if (body.latest && cur.notes) body.notes = cur.notes;   // 1.2.0: older clients ignore it
  return body;
}

// Public and anonymous: the answer depends only on the announced release, so it carries no per-player data and may be cached.
export async function handleLatest(request, env) {
  if (request.method !== "GET" && request.method !== "HEAD") return fail(405, "GET only");
  return json(latestBody(await currentLatest(env)), 200, { "cache-control": "public, max-age=3600" });
}

// Owner route /v1/admin/latest (header key only, like every admin route; the browser cookie never reaches it).
//   GET                      {ok, live, source, file, prev}
//   POST {version, sha256, size, asset?, notice?}   announce: the stored value moves to config/latest.prev.json
//   POST {undo: true}        back to the previous announce (or, with none, to the vars)
async function handleAdminLatest(request, env) {
  const view = async (extra = {}) => {
    const cur = await currentLatest(env, true);
    return json({ ok: true, live: latestBody(cur), source: cur.source, file: await readLatestFile(env),
                  prev: await readLatestFile(env, LATEST_PREV_KEY), ...extra });
  };
  if (request.method === "GET") return view();
  if (request.method !== "POST") return fail(405, "GET or POST");
  let b;
  try {
    b = await request.json();
  } catch (e) {
    return fail(400, "JSON body {version, sha256, size, asset, notice} expected");
  }
  const put = (key, v) => env.REPORTS.put(key, JSON.stringify({ ...v, set_at: new Date().toISOString() }),
                                          { httpMetadata: { contentType: "application/json" } });
  const strip = (v) => { const { set_at, ...rest } = v; return rest; };
  const now = await readLatestFile(env);
  if (b && b.undo === true) {
    const prev = await readLatestFile(env, LATEST_PREV_KEY);
    if (prev) {
      await put(LATEST_KEY, strip(prev));
      if (now) await put(LATEST_PREV_KEY, strip(now));
      else await env.REPORTS.delete(LATEST_PREV_KEY);
    } else {
      await env.REPORTS.delete(LATEST_KEY);
    }
    return view({ undone: true });
  }
  const r = validateLatest(b);
  if (r.error) return fail(400, r.error);
  if (now) await put(LATEST_PREV_KEY, strip(now));
  await put(LATEST_KEY, r.value);
  return view({ announced: r.value.version });
}

// ---------------------------------------------------------------- owner routes

async function findReport(env, id, ext, headOnly = false) {
  const look = (k) => (headOnly ? env.REPORTS.head(k) : env.REPORTS.get(k));
  // 1.2.0: a report with a conversation may be older than the months below; its conversation names its key.
  const conv = await readJson(env, `conv/${id}.json`).catch(() => null);
  if (conv && typeof conv.key === "string" && conv.key.startsWith("reports/")) {
    const obj = await look(conv.key.replace(/\.zip$/, `.${ext}`));
    if (obj) return obj;
  }
  for (const m of lastMonths(5)) {
    const obj = await look(`reports/${m}/${id}.${ext}`);
    if (obj) return obj;
  }
  return null;
}

async function handleOwner(request, env, url) {
  if (!ownerKey(env)) return fail(404, "not found");
  if (!(await ownerOk(request, env))) return fail(401, "owner key required");
  const p = url.pathname;

  let d = p.match(/^\/v1\/report\/(MA2-[0-9A-Z]{6})$/);
  if (d && request.method === "DELETE") {
    if (!ID_RE.test(d[1])) return fail(404, "not found");
    const id = d[1];
    const stored = await deleteStored(env, id);
    const rec = await readJson(env, `issues/${id}.json`);
    const r = await retireIssue(env, id, rec, "deleted on request");
    if (r.ok) await env.REPORTS.delete(`issues/${id}.json`);
    else
      // GitHub unreachable or the token expired: the cron finishes it (retire/ queue).
      await env.REPORTS.put(`retire/${id}.json`, JSON.stringify(rec || { id, received: new Date().toISOString() }), {
        httpMetadata: { contentType: "application/json" },
      });
    return json({ ok: true, id, ...stored, issue: r.ok ? r.how : "queued" });
  }

  // 1.2.0 conversations (triage.py reply / conversations)
  let cm = p.match(/^\/v1\/report\/(MA2-[A-Z2-7]{6})\/(conversation|reply|logs\/(\d{1,3}))$/);
  if (cm) {
    const id = cm[1];
    if (cm[2] === "conversation" && request.method === "GET") {
      const conv = await readConv(env, id);
      if (!conv && !(await findReport(env, id, "json", true))) return fail(404, "not found");
      return json({ ok: true, id, replies: !!conv, conversation: convView(conv) });
    }
    if (cm[2] === "reply" && request.method === "POST") {
      let b;
      try {
        b = await request.json();
      } catch (e) {
        return fail(400, "JSON body {text, edit?} expected");
      }
      const r = await ownerPost(env, id, b && b.text, b && b.edit !== undefined ? b.edit : null);
      if (r.error) return json({ ok: false, error: r.error, message: typeof OPEN_TEXT[r.error] === "string" ? OPEN_TEXT[r.error] : r.error }, r.error === "missing" ? 404 : 409);
      return json({ ok: true, id, conversation: convView(await readConv(env, id)) });
    }
    if (cm[3] && request.method === "GET") {
      const obj = await env.REPORTS.get(`answers/${id}/${Number(cm[3])}.zip`);
      if (!obj) return fail(404, "not found");
      return new Response(obj.body, { headers: { "content-type": "application/zip", "content-disposition": `attachment; filename="${id}-answer-${Number(cm[3])}.zip"`, "cache-control": "no-store" } });
    }
  }
  if (p === "/v1/admin/conversations" && request.method === "GET") {
    const out = [];
    let c;
    do {
      const l = await env.REPORTS.list({ prefix: "conv/", cursor: c, limit: 1000, include: ["customMetadata"] });
      for (const o of l.objects) {
        const conv = await readConv(env, o.key.slice(5, -5));
        if (!conv || !conv.messages.length) continue;
        const newest = conv.messages[conv.messages.length - 1];
        out.push({ id: conv.id, received: conv.received, last: conv.last, messages: conv.messages.length,
                   waiting: newest.from === "player" ? "you" : newest.seen ? "player" : "player (not seen yet)" });
      }
      c = l.truncated ? l.cursor : undefined;
    } while (c);
    out.sort((a, b) => (a.last < b.last ? 1 : -1));
    return json({ ok: true, conversations: out });
  }

  let m = p.match(/^\/v1\/report\/(MA2-[0-9A-Z]{6})(\/info)?$/);
  if (m && request.method === "GET") {
    if (!ID_RE.test(m[1])) return fail(404, "not found");
    const obj = await findReport(env, m[1], m[2] ? "json" : "zip");
    if (!obj) return fail(404, "not found (reports are kept 60 days)");
    if (m[2]) return new Response(obj.body, { headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" } });
    return new Response(obj.body, {
      headers: {
        "content-type": "application/zip",
        "content-disposition": `attachment; filename="${m[1]}.zip"`,
        "cache-control": "no-store",
      },
    });
  }

  if (p === "/v1/admin/reports" && request.method === "GET") {
    const days = Math.min(Math.max(parseInt(url.searchParams.get("days") || "7", 10) || 7, 1), 92);
    const since = Date.now() - days * 86400e3;
    const pending = new Set();
    let cur;
    do {
      const l = await env.REPORTS.list({ prefix: "pending/", cursor: cur, limit: 1000 });
      for (const o of l.objects) pending.add(o.key.slice(8, -5));
      cur = l.truncated ? l.cursor : undefined;
    } while (cur);
    const out = [];
    for (const mo of lastMonths(Math.ceil(days / 28) + 1)) {
      let c;
      do {
        const l = await env.REPORTS.list({ prefix: `reports/${mo}/`, cursor: c, limit: 1000, include: ["customMetadata"] });
        for (const o of l.objects) {
          if (!o.key.endsWith(".zip")) continue;
          const t = new Date(o.uploaded).getTime();
          if (t < since) continue;
          const id = o.key.split("/").pop().slice(0, -4);
          const cm = o.customMetadata || {};
          out.push({
            id, key: o.key, bytes: o.size, received: cm.received || new Date(o.uploaded).toISOString(),
            client: cm.client, version: cm.version, build: cm.build, kind: cm.kind, signature: cm.signature,
            issue: pending.has(id) ? "pending" : cm.issue === "none" ? "none" : "opened",
          });
        }
        c = l.truncated ? l.cursor : undefined;
      } while (c);
    }
    out.sort((a, b) => (a.received < b.received ? 1 : -1));
    return json({ ok: true, reports: out });
  }

  if (p === "/v1/admin/pending" && request.method === "GET") {
    const l = await env.REPORTS.list({ prefix: "pending/", limit: 1000 });
    const out = [];
    for (const o of l.objects) {
      const obj = await env.REPORTS.get(o.key);
      if (!obj) continue;
      const mk = await obj.json();
      out.push({ id: mk.id, received: mk.received, attempts: mk.attempts || 0, last_status: mk.last_status ?? null });
    }
    return json({ ok: true, pending: out });
  }

  if (p === "/v1/admin/latest") return await handleAdminLatest(request, env);

  if (p === "/v1/admin/expire" && request.method === "POST") {
    return json({ ok: true, ...(await expireOld(env)) });
  }

  if (p === "/v1/admin/retry" && request.method === "POST") {
    return json({ ok: true, ...(await retryPending(env)) });
  }

  if (p === "/v1/admin/switch") {
    if (request.method === "GET") return json({ ok: true, env: env.ACCEPT_REPORTS || "on", ...(await gateCall(env, "/switch")) });
    if (request.method === "POST") {
      let b;
      try {
        b = await request.json();
      } catch (e) {
        return fail(400, "JSON body {on, message} expected");
      }
      if (typeof b.on !== "boolean") return fail(400, "JSON body {on, message} expected");
      const message = typeof b.message === "string" ? b.message.slice(0, 500) : "";
      return json({ ok: true, ...(await gateCall(env, "/switch", { on: b.on, message })) });
    }
  }
  return fail(404, "not found");
}

// ---------------------------------------------------------------- entry points

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    try {
      if (url.pathname === "/v1/latest") return await handleLatest(request, env);
      if (url.pathname === "/v1/replies") return await handleReplies(request, env);
      if (url.pathname === "/v1/answer") return await handleAnswer(request, env, ctx);
      if (url.pathname.startsWith("/v1/open/")) return await handleOpen(request, env, url);
      if (url.pathname === "/v1/report") {
        if (request.method !== "POST") return fail(405, "POST only");
        return await handleUpload(request, env, ctx);
      }
      if (url.pathname.startsWith("/v1/report/") || url.pathname.startsWith("/v1/admin/"))
        return await handleOwner(request, env, url);
      return fail(404, "not found");
    } catch (e) {
      return fail(500, MSG.store);
    }
  },

  async scheduled(event, env, ctx) {
    ctx.waitUntil(retryPending(env));
    ctx.waitUntil(expireOld(env));
  },
};

// ---------------------------------------------------------------- limiter (Durable Object, one instance)
//
// Storage keys:  switch -> {on, message}
//                day:<yyyy-mm-dd> -> accepted count (global daily cap)
//                key:<yyyy-mm-dd> -> random 32-byte HMAC key, deleted when the day is over
//                ip:<hmac hex> -> timestamps of this connection's reports in the last hour, deleted after the hour
//                kt:<hmac hex> -> timestamps of this connection's owner-key tries on /v1/open (same rule)
//                rp:<hmac hex> -> timestamps of this connection's reply checks on /v1/replies (same rule, 1.2.0)
// The raw IP never reaches storage.

export class Gate {
  constructor(state, env) {
    this.state = state;
    this.env = env;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const body = request.method === "POST" ? await request.json() : {};
    if (url.pathname === "/admit") return json(await this.admit(body));
    if (url.pathname === "/attempt") return json(await this.attempt(body));
    if (url.pathname === "/poll") return json(await this.attempt(body, "rp"));
    if (url.pathname === "/switch") {
      if (request.method === "POST") await this.state.storage.put("switch", { on: body.on, message: body.message || "" });
      const sw = (await this.state.storage.get("switch")) || { on: true, message: "" };
      return json({ switch: sw });
    }
    return json({ ok: false }, 404);
  }

  async ipHash(ip, day) {
    const st = this.state.storage;
    let k = await st.get(`key:${day}`);
    if (!k) {
      const raw = new Uint8Array(32);
      crypto.getRandomValues(raw);
      k = Array.from(raw);
      await st.put(`key:${day}`, k);
    }
    const hk = await crypto.subtle.importKey("raw", new Uint8Array(k), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
    const mac = new Uint8Array(await crypto.subtle.sign("HMAC", hk, new TextEncoder().encode(String(ip))));
    return Array.from(mac.slice(0, 16), (x) => x.toString(16).padStart(2, "0")).join("");
  }

  // Owner-key tries on the download page: perHour per connection (kt:<hmac>), independent of the report counters.
  // 1.2.0: the same counter for reply checks (/v1/replies, prefix rp:).
  async attempt({ ip, perHour }, prefix = "kt") {
    const st = this.state.storage;
    const now = Date.now();
    const h = await this.ipHash(ip, new Date(now).toISOString().slice(0, 10));
    const recent = ((await st.get(`${prefix}:${h}`)) || []).filter((t) => t > now - 3600e3);
    if (recent.length >= perHour) return { ok: false };
    recent.push(now);
    await st.put(`${prefix}:${h}`, recent);
    if ((await st.getAlarm()) === null) await st.setAlarm(now + 3600e3);
    return { ok: true };
  }

  async admit({ ip, cap, perHour }) {
    const st = this.state.storage;
    const sw = await st.get("switch");
    if (sw && sw.on === false) return { ok: false, reason: "off", message: sw.message || "" };
    const now = Date.now();
    const day = new Date(now).toISOString().slice(0, 10);
    const count = (await st.get(`day:${day}`)) || 0;
    if (count >= cap) return { ok: false, reason: "daily" };

    let k = await st.get(`key:${day}`);
    if (!k) {
      const raw = new Uint8Array(32);
      crypto.getRandomValues(raw);
      k = Array.from(raw);
      await st.put(`key:${day}`, k);
    }
    const hk = await crypto.subtle.importKey("raw", new Uint8Array(k), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
    const mac = new Uint8Array(await crypto.subtle.sign("HMAC", hk, new TextEncoder().encode(String(ip))));
    const h = Array.from(mac.slice(0, 16), (x) => x.toString(16).padStart(2, "0")).join("");

    const recent = ((await st.get(`ip:${h}`)) || []).filter((t) => t > now - 3600e3);
    if (recent.length >= perHour) return { ok: false, reason: "ip" };
    recent.push(now);
    await st.put(`ip:${h}`, recent);
    await st.put(`day:${day}`, count + 1);
    if ((await st.getAlarm()) === null) await st.setAlarm(now + 3600e3);
    return { ok: true, message: sw && sw.message ? sw.message : "" };
  }

  async alarm() {
    const st = this.state.storage;
    const now = Date.now();
    const today = new Date(now).toISOString().slice(0, 10);
    const weekAgo = new Date(now - 7 * 86400e3).toISOString().slice(0, 10);
    let left = 0;
    for (const [k, v] of [...(await st.list({ prefix: "ip:" })), ...(await st.list({ prefix: "kt:" })), ...(await st.list({ prefix: "rp:" }))]) {
      const keep = (v || []).filter((t) => t > now - 3600e3);
      if (keep.length === 0) await st.delete(k);
      else {
        left++;
        if (keep.length !== v.length) await st.put(k, keep);
      }
    }
    for (const [k] of await st.list({ prefix: "key:" })) if (k.slice(4) !== today) await st.delete(k);
    for (const [k] of await st.list({ prefix: "day:" })) if (k.slice(4) < weekAgo) await st.delete(k);
    if (left > 0 || (await st.list({ prefix: "key:" })).size > 0) await st.setAlarm(now + 3600e3);
  }
}

// Run: node --test test/      (from the relay folder; Node 18+; no packages, no network)
import { test, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import worker, { neutralize } from "../src/worker.js";
import { makeEnv, MockCtx, MockGitHub, reportRequest, tinyZip } from "./mocks.mjs";

const ID_RE = /^MA2-[A-Z2-7]{6}$/;
let gh, env, ctx;

beforeEach(() => {
  gh = new MockGitHub();
  gh.install();
  env = makeEnv();
  ctx = new MockCtx();
});
afterEach(() => gh.uninstall());

async function send(req, e = env) {
  const res = await worker.fetch(req, e, ctx);
  await ctx.drain();
  const body = await res.json().catch(() => null);
  return { status: res.status, body };
}

function owner(path, init = {}, key = env.OWNER_KEY) {
  const h = { ...(init.headers || {}) };
  if (key !== null) h["x-ma2-owner-key"] = key;
  return new Request("https://relay.example.test" + path, { ...init, headers: h });
}

// ---------------------------------------------------------------- normal upload

test("normal upload: id returned, zip + sidecar stored, issue opened, marker cleared", async () => {
  const zip = tinyZip(1000);
  const r = await send(await reportRequest({ zip, text: "Hi @Mazillion ```js\nalert(1)``` [x](http://evil)" }));
  assert.equal(r.status, 200);
  assert.equal(r.body.ok, true);
  assert.match(r.body.id, ID_RE);
  assert.equal(r.body.message, "");

  const month = new Date().toISOString().slice(0, 7);
  const zkey = `reports/${month}/${r.body.id}.zip`;
  const stored = await env.REPORTS.get(zkey);
  assert.ok(stored, "zip stored");
  assert.deepEqual(new Uint8Array(await stored.arrayBuffer()), zip);
  assert.equal(stored.customMetadata.kind, "crash");
  assert.equal(stored.customMetadata.version, "1.1.6");
  assert.equal(stored.customMetadata.signature, "c0000005 0019236B"); // '@' is not in the signature charset

  const side = await (await env.REPORTS.get(`reports/${month}/${r.body.id}.json`)).json();
  assert.equal(side.text, "Hi @Mazillion ```js\nalert(1)``` [x](http://evil)");
  assert.equal(side.zip_bytes, zip.length);

  assert.equal(gh.issues.length, 1);
  const is = gh.issues[0];
  assert.equal(is.title, `[${r.body.id}] crash 1.1.6 c0000005 0019236B`);
  assert.deepEqual(is.labels, ["crash", "v1.1.6", "installer"]);
  assert.ok(!is.body.includes("@Mazillion"), "mention neutralised");
  // Exactly the 4 fences the relay writes; the player's backticks cannot add or close one.
  assert.equal((is.body.match(/```/g) || []).length, 4);
  assert.ok(is.body.includes(zkey), "R2 key in the body");
  assert.ok(is.body.includes(`https://relay.example.test/v1/report/${r.body.id}`), "owner link in the body");
  assert.equal(gh.calls[0].auth, "Bearer github_pat_TEST");
  assert.equal(await env.REPORTS.get(`pending/${r.body.id}.json`), null, "pending marker removed");
});

test("normal upload at the client's 20 MB limit is accepted intact", async () => {
  const zip = tinyZip(20 * 1024 * 1024 - 100);
  const r = await send(await reportRequest({ zip }));
  assert.equal(r.status, 200);
  const month = new Date().toISOString().slice(0, 7);
  const stored = await env.REPORTS.get(`reports/${month}/${r.body.id}.zip`);
  assert.equal(stored.size, zip.length);
});

test("privacy: the connecting IP is stored nowhere (R2, DO storage, issue)", async () => {
  const ip = "198.51.100.23";
  const r = await send(await reportRequest({ ip }));
  assert.equal(r.status, 200);
  assert.ok(!env.REPORTS.allBytes().includes(ip), "not in R2");
  assert.ok(!env.GATE.storageOf().dump().includes(ip), "not in limiter storage");
  assert.ok(!JSON.stringify(gh.issues).includes(ip), "not in the issue");
});

test("metadata may also come as form fields (header names missing)", async () => {
  const req = await reportRequest({
    omit: ["x-ma2-client", "x-ma2-version", "x-ma2-build", "x-ma2-kind", "x-ma2-signature"],
    fields: { client: "helper", version: "1.1.6", build: "abc", kind: "hang", signature: "freeze 30s" },
  });
  const r = await send(req);
  assert.equal(r.status, 200);
  assert.deepEqual(gh.issues[0].labels, ["hang", "v1.1.6", "helper"]);
});

test("player text is cut at 2,000 characters", async () => {
  const r = await send(await reportRequest({ text: "a".repeat(5000) }));
  assert.equal(r.status, 200);
  const month = new Date().toISOString().slice(0, 7);
  const side = await (await env.REPORTS.get(`reports/${month}/${r.body.id}.json`)).json();
  assert.equal(side.text.length, 2000);
});

// ---------------------------------------------------------------- oversized

test("oversized: Content-Length over the limit gives 413 before anything is read or counted", async () => {
  const req = await reportRequest({ headers: { "content-length": String(30 * 1024 * 1024) } });
  const r = await send(req);
  assert.equal(r.status, 413);
  assert.equal(r.body.ok, false);
  assert.equal(env.REPORTS.map.size, 0);
  assert.equal(env.GATE.storageOf(), undefined, "limiter not touched");
});

test("oversized: zip part bigger than the limit gives 413 (small limit for the test)", async () => {
  env = makeEnv({ MAX_UPLOAD_BYTES: "4000" });
  // The client lies about the length; the relay still checks the real part size.
  const req = await reportRequest({ zip: tinyZip(4100), headers: { "content-length": "3999" } });
  const r = await send(req, env);
  assert.equal(r.status, 413);
  assert.equal(env.REPORTS.map.size, 0);
});

test("missing Content-Length gives 411", async () => {
  const r = await send(await reportRequest({ omit: ["content-length"] }));
  assert.equal(r.status, 411);
});

// ---------------------------------------------------------------- rate limits

test("rate limit: 5 per connection per hour, 6th gets 429, another connection still works", async () => {
  for (let i = 0; i < 5; i++) assert.equal((await send(await reportRequest())).status, 200);
  const sixth = await send(await reportRequest());
  assert.equal(sixth.status, 429);
  assert.equal(sixth.body.ok, false);
  assert.match(sixth.body.message, /saved on your PC/);
  assert.equal((await send(await reportRequest({ ip: "203.0.113.99" }))).status, 200);
  assert.equal(gh.issues.length, 6);
});

test("rate limit: an hour later the same connection may send again", async () => {
  for (let i = 0; i < 5; i++) await send(await reportRequest());
  const st = env.GATE.storageOf();
  for (const [k, v] of st.map) if (k.startsWith("ip:")) st.map.set(k, v.map((t) => t - 3601e3));
  assert.equal((await send(await reportRequest())).status, 200);
});

test("daily cap: after DAILY_CAP reports everyone gets 429", async () => {
  env = makeEnv({ DAILY_CAP: "3" });
  for (let i = 0; i < 3; i++) assert.equal((await send(await reportRequest({ ip: `192.0.2.${i}` }), env)).status, 200);
  const r = await send(await reportRequest({ ip: "192.0.2.200" }), env);
  assert.equal(r.status, 429);
  assert.match(r.body.message, /too many reports today/);
});

test("limiter alarm drops expired connection entries and old day keys", async () => {
  await send(await reportRequest());
  const st = env.GATE.storageOf();
  for (const [k, v] of st.map) if (k.startsWith("ip:")) st.map.set(k, v.map((t) => t - 3601e3));
  st.map.set("key:2000-01-01", [1, 2, 3]);
  await env.GATE.objOf().alarm();
  assert.equal([...st.map.keys()].filter((k) => k.startsWith("ip:")).length, 0);
  assert.ok(!st.map.has("key:2000-01-01"));
});

// ---------------------------------------------------------------- GitHub down, token expired, retry

for (const mode of ["down", "expired", "malformed", "offline"]) {
  test(`GitHub ${mode}: player still gets an id; the cron retry opens the issue later`, async () => {
    gh.mode = mode;
    const r = await send(await reportRequest());
    assert.equal(r.status, 200);
    assert.equal(r.body.ok, true);
    assert.match(r.body.id, ID_RE);
    const mk = await (await env.REPORTS.get(`pending/${r.body.id}.json`)).json();
    assert.equal(mk.attempts, 1);
    assert.equal(gh.issues.length, 0);

    // Still broken at the first cron run: marker kept, attempts counted.
    await worker.scheduled({}, env, ctx);
    await ctx.drain();
    assert.ok(await env.REPORTS.get(`pending/${r.body.id}.json`));

    gh.mode = "up";
    await worker.scheduled({}, env, ctx);
    await ctx.drain();
    assert.equal(gh.issues.length, 1);
    assert.ok(gh.issues[0].title.startsWith(`[${r.body.id}]`));
    assert.equal(await env.REPORTS.get(`pending/${r.body.id}.json`), null);
  });
}

test("retry never duplicates: an issue that exists already just clears the marker", async () => {
  const r = await send(await reportRequest());
  assert.equal(gh.issues.length, 1);
  // Simulate "issue created but the marker delete was lost".
  await env.REPORTS.put(`pending/${r.body.id}.json`, JSON.stringify({
    id: r.body.id, received: new Date().toISOString(), title: gh.issues[0].title, body: "x", labels: [],
  }));
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  assert.equal(gh.issues.length, 1);
  assert.equal(await env.REPORTS.get(`pending/${r.body.id}.json`), null);
});

test("expired token stops a retry run after the first failure (no hammering)", async () => {
  gh.mode = "expired";
  for (let i = 0; i < 4; i++) await send(await reportRequest({ ip: `192.0.2.${i}` }));
  gh.calls.length = 0;
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  assert.equal(gh.calls.length, 1, "one search call, then the run stops");
});

test("malformed token (GitHub 400) stops a retry run after the first failure, like 401/403", async () => {
  gh.mode = "malformed";
  for (let i = 0; i < 4; i++) await send(await reportRequest({ ip: `192.0.2.${i}` }));
  gh.calls.length = 0;
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  assert.equal(gh.calls.length, 1, "one search call, then the run stops");
  const pend = await (await worker.fetch(owner("/v1/admin/pending"), env, ctx)).json();
  assert.equal(pend.pending.length, 4, "every report kept pending");
  assert.ok(pend.pending.every((x) => x.last_status === 400));
});

test("a 400 on the duplicate search is not mistaken for 'no issue yet' (no POST follows)", async () => {
  gh.mode = "down";
  const r = await send(await reportRequest());
  gh.mode = "malformed";
  gh.calls.length = 0;
  const res = await worker.fetch(owner("/v1/admin/retry", { method: "POST" }), env, ctx);
  const b = await res.json();
  assert.equal(b.opened, 0);
  assert.equal(b.stopped, true);
  assert.deepEqual(gh.calls.map((c) => c.method), ["GET"]);
  assert.ok(await env.REPORTS.get(`pending/${r.body.id}.json`));
});

test("missing token: report accepted and kept pending", async () => {
  env = makeEnv({ GITHUB_TOKEN: "" });
  const r = await send(await reportRequest(), env);
  assert.equal(r.status, 200);
  assert.ok(await env.REPORTS.get(`pending/${r.body.id}.json`));
  assert.equal(gh.calls.length, 0);
});

test("owner retry route opens pending issues on demand", async () => {
  gh.mode = "down";
  await send(await reportRequest());
  gh.mode = "up";
  const res = await worker.fetch(owner("/v1/admin/retry", { method: "POST" }), env, ctx);
  const b = await res.json();
  assert.equal(b.opened, 1);
  assert.equal(gh.issues.length, 1);
});

// ---------------------------------------------------------------- off switch and notices

test("off switch (env ACCEPT_REPORTS=off): 503 ok:false with the owner's message, nothing stored", async () => {
  env = makeEnv({ ACCEPT_REPORTS: "off", OFF_MESSAGE: "Please update to 1.1.7." });
  const r = await send(await reportRequest(), env);
  assert.equal(r.status, 503);
  assert.deepEqual(r.body, { ok: false, id: null, message: "Please update to 1.1.7." });
  assert.equal(env.REPORTS.map.size, 0);
});

test("off switch (owner route, no redeploy): off then on again", async () => {
  let res = await worker.fetch(owner("/v1/admin/switch", { method: "POST", body: JSON.stringify({ on: false, message: "Back soon." }) }), env, ctx);
  assert.equal(res.status, 200);
  let r = await send(await reportRequest());
  assert.equal(r.status, 503);
  assert.equal(r.body.message, "Back soon.");
  res = await worker.fetch(owner("/v1/admin/switch", { method: "POST", body: JSON.stringify({ on: true, message: "" }) }), env, ctx);
  r = await send(await reportRequest());
  assert.equal(r.status, 200);
});

test("notices: NOTICE on success; UPDATE_NOTICE for older versions; MIN_VERSION refuses", async () => {
  env = makeEnv({ NOTICE: "Thanks!", LATEST_VERSION: "1.1.7", UPDATE_NOTICE: "Please update to 1.1.7.", MIN_VERSION: "1.1.6" });
  let r = await send(await reportRequest({ headers: { "x-ma2-version": "1.1.7" } }), env);
  assert.equal(r.body.message, "Thanks!");
  r = await send(await reportRequest({ headers: { "x-ma2-version": "1.1.6" } }), env);
  assert.equal(r.status, 200);
  assert.equal(r.body.message, "Please update to 1.1.7.");
  r = await send(await reportRequest({ headers: { "x-ma2-version": "1.1.5" } }), env);
  assert.equal(r.status, 503);
  assert.equal(r.body.ok, false);
});

// ---------------------------------------------------------------- malformed input

const malformed = [
  ["GET on /v1/report", async () => new Request("https://relay.example.test/v1/report"), 405],
  ["unknown path", async () => new Request("https://relay.example.test/upload", { method: "POST" }), 404],
  ["unknown client kind", () => reportRequest({ headers: { "x-ma2-client": "browser" } }), 400],
  ["unknown report kind", () => reportRequest({ headers: { "x-ma2-kind": "spam" } }), 400],
  ["bad version characters", () => reportRequest({ headers: { "x-ma2-version": "1.1.6<script>" } }), 400],
  ["no metadata at all", () => reportRequest({ omit: ["x-ma2-client", "x-ma2-version", "x-ma2-kind"] }), 400],
  ["not multipart", async () => new Request("https://relay.example.test/v1/report", {
    method: "POST", headers: { "content-type": "application/json", "content-length": "2", "x-ma2-client": "game" }, body: "{}",
  }), 415],
  ["garbage multipart body", async () => new Request("https://relay.example.test/v1/report", {
    method: "POST",
    headers: { "content-type": "multipart/form-data; boundary=XYZ", "content-length": "11", "x-ma2-client": "game", "x-ma2-kind": "crash", "x-ma2-version": "1.1.6" },
    body: "not a form!",
  }), 400],
  ["zip missing", () => reportRequest({ zip: null }), 400],
  ["zip is a text field", () => reportRequest({ zip: null, fields: { zip: "PK\u0003\u0004 not a file" } }), 400],
  ["not a zip (wrong magic)", () => reportRequest({ zip: new TextEncoder().encode("MZ" + "\0".repeat(100)) }), 400],
  ["zip too short", () => reportRequest({ zip: new Uint8Array([0x50, 0x4b, 3, 4]) }), 400],
  ["Content-Length not a number", () => reportRequest({ headers: { "content-length": "12abc" } }), 400],
];
for (const [name, make, status] of malformed) {
  test(`malformed: ${name} -> ${status}, nothing stored, no issue`, async () => {
    const r = await send(await make());
    assert.equal(r.status, status);
    assert.equal(r.body.ok, false);
    assert.equal(r.body.id, null);
    assert.equal(typeof r.body.message, "string");
    assert.equal([...env.REPORTS.map.keys()].filter((k) => k.startsWith("reports/")).length, 0);
    assert.equal(gh.issues.length, 0);
  });
}

test("R2 failure: 500 ok:false so the client saves locally", async () => {
  env.REPORTS.failPut = true;
  const r = await send(await reportRequest());
  assert.equal(r.status, 500);
  assert.equal(r.body.ok, false);
});

test("neutralize keeps newlines, drops control and bidi characters", () => {
  assert.equal(neutralize("a\r\nb\u0007c‮d`@"), "a\nbcdˋ＠");
  assert.equal(neutralize("   "), "(none)");
});

// ---------------------------------------------------------------- owner download

test("owner download: no key 401, wrong key 401, right key returns the exact zip", async () => {
  const zip = tinyZip(500);
  const r = await send(await reportRequest({ zip }));
  const path = `/v1/report/${r.body.id}`;
  assert.equal((await worker.fetch(owner(path, {}, null), env, ctx)).status, 401);
  assert.equal((await worker.fetch(owner(path, {}, "wrong"), env, ctx)).status, 401);
  const ok = await worker.fetch(owner(path), env, ctx);
  assert.equal(ok.status, 200);
  assert.equal(ok.headers.get("content-type"), "application/zip");
  assert.deepEqual(new Uint8Array(await ok.arrayBuffer()), zip);
  const info = await (await worker.fetch(owner(path + "/info"), env, ctx)).json();
  assert.equal(info.id, r.body.id);
  assert.equal((await worker.fetch(owner("/v1/report/MA2-000000"), env, ctx)).status, 404);
});

test("owner list shows reports with issue state; pending list", async () => {
  await send(await reportRequest());
  gh.mode = "down";
  const p = await send(await reportRequest({ headers: { "x-ma2-kind": "hang" } }));
  const list = await (await worker.fetch(owner("/v1/admin/reports?days=7"), env, ctx)).json();
  assert.equal(list.reports.length, 2);
  assert.equal(list.reports.find((x) => x.id === p.body.id).issue, "pending");
  const pend = await (await worker.fetch(owner("/v1/admin/pending"), env, ctx)).json();
  assert.deepEqual(pend.pending.map((x) => x.id), [p.body.id]);
});

test("owner routes are absent when OWNER_KEY is not set", async () => {
  env = makeEnv({ OWNER_KEY: "" });
  const res = await worker.fetch(owner("/v1/admin/reports", {}, "anything"), env, ctx);
  assert.equal(res.status, 404);
});

// ---------------------------------------------------------------- latest version (1.1.7)

function latestReq(method = "GET", headers = {}) {
  return new Request("https://relay.example.test/v1/latest", { method, headers: { "cf-connecting-ip": "198.51.100.77", ...headers } });
}

test("latest: LATEST_VERSION and UPDATE_NOTICE are returned, cacheable for an hour", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7", UPDATE_NOTICE: "Please update to 1.1.7." });
  const res = await worker.fetch(latestReq(), env, ctx);
  assert.equal(res.status, 200);
  assert.equal(res.headers.get("cache-control"), "public, max-age=3600");
  assert.match(res.headers.get("content-type"), /^application\/json/);
  assert.deepEqual(await res.json(), { ok: true, latest: "1.1.7", notice: "Please update to 1.1.7." });
});

test("latest: no notice key when UPDATE_NOTICE is empty", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7", UPDATE_NOTICE: "" });
  const b = await (await worker.fetch(latestReq(), env, ctx)).json();
  assert.deepEqual(b, { ok: true, latest: "1.1.7" });
});

test("latest: unset or malformed LATEST_VERSION gives latest null (and no notice)", async () => {
  for (const v of [undefined, "", "  ", "1.1.7-preview.1", "v1.1.7", "1", "1.1.7<b>", "1.1.7.1.2"]) {
    const e = makeEnv({ UPDATE_NOTICE: "x" });
    if (v !== undefined) e.LATEST_VERSION = v;
    const res = await worker.fetch(latestReq(), e, ctx);
    assert.equal(res.status, 200, String(v));
    assert.deepEqual(await res.json(), { ok: true, latest: null }, String(v));
  }
  const e = makeEnv({ LATEST_VERSION: " 1.2 " });
  assert.equal((await (await worker.fetch(latestReq(), e, ctx)).json()).latest, "1.2");
});

test("latest: no storage, no limiter, no GitHub, nothing stored about the caller", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7" });
  for (let i = 0; i < 20; i++) assert.equal((await worker.fetch(latestReq(), env, ctx)).status, 200);
  await ctx.drain();
  assert.equal(env.REPORTS.map.size, 0, "nothing in R2");
  assert.equal(env.GATE.storageOf(), undefined, "limiter never touched");
  assert.equal(gh.calls.length, 0, "no GitHub call");
});

test("latest: other methods 405; HEAD works; works while reports are switched off", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7", ACCEPT_REPORTS: "off" });
  assert.equal((await worker.fetch(latestReq("POST"), env, ctx)).status, 405);
  assert.equal((await worker.fetch(latestReq("HEAD"), env, ctx)).status, 200);
  assert.equal((await (await worker.fetch(latestReq(), env, ctx)).json()).latest, "1.1.7");
});

test("latest: a trailing path or query is not the route", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7" });
  assert.equal((await worker.fetch(new Request("https://relay.example.test/v1/latest/x"), env, ctx)).status, 404);
  const q = await worker.fetch(new Request("https://relay.example.test/v1/latest?v=1.1.6"), env, ctx);
  assert.equal(q.status, 200, "a query string is ignored");
});

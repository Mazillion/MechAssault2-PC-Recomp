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
  // 1.1.7: the player's text is not in the issue (only "yes"); the summary is, in its own fence.
  assert.ok(!is.body.includes("alert(1)") && !is.body.includes("evil"), "player's text kept out of the issue");
  assert.ok(is.body.includes("**Player's text**: yes"));
  assert.equal((is.body.match(/```/g) || []).length, 2);
  const rec = await (await env.REPORTS.get(`issues/${r.body.id}.json`)).json();
  assert.equal(rec.number, 1);
  assert.equal(rec.node_id, "I_node1");
  assert.ok(is.body.includes(zkey), "R2 key in the body");
  assert.ok(is.body.includes(`Download: https://relay.example.test/v1/open/${r.body.id} (opens in your browser; asks for your owner key the first time)`),
    "one-click owner link in the body");
  assert.ok(!is.body.includes(env.OWNER_KEY), "no key in the issue");
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

// ---------------------------------------------------------------- announce + installer hash (1.1.7 self-update)

const SHA = "ab".repeat(32);
const SIZE = 23107422;
function announce(body, key = env.OWNER_KEY, method = "POST") {
  return owner("/v1/admin/latest", { method, headers: { "content-type": "application/json" },
                                     body: method === "POST" ? JSON.stringify(body) : undefined }, key);
}
async function live(e = env) {
  return (await worker.fetch(latestReq(), e, ctx)).json();
}

test("latest env: LATEST_SHA256 + LATEST_SIZE add sha256, size and the fixed asset name", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.8", LATEST_SHA256: SHA.toUpperCase(), LATEST_SIZE: String(SIZE) });
  assert.deepEqual(await live(), { ok: true, latest: "1.1.8", sha256: SHA, size: SIZE, asset: "MechAssault 2 Recomp Installer.exe" });
});

test("latest env: a bad hash or size leaves the installer fields out (the game still sees the version)", async () => {
  for (const [sha, size] of [["xyz", String(SIZE)], [SHA.slice(2), String(SIZE)], [SHA, "12"], [SHA, "999999999999"], [SHA, "2e7x"], [SHA, ""]]) {
    const b = await live(makeEnv({ LATEST_VERSION: "1.1.8", LATEST_SHA256: sha, LATEST_SIZE: size }));
    assert.deepEqual(b, { ok: true, latest: "1.1.8" }, `${sha} ${size}`);
  }
  assert.deepEqual(await live(makeEnv({ LATEST_SHA256: SHA, LATEST_SIZE: String(SIZE) })), { ok: true, latest: null });
});

test("announce: validation refuses bad input and stores nothing", async () => {
  const good = { version: "1.1.8", sha256: SHA, size: SIZE, asset: "MechAssault 2 Recomp Installer.exe", notice: "Please update." };
  const bad = [
    { ...good, version: "1.1.8-preview.1" }, { ...good, version: "1.2" }, { ...good, version: "v1.1.8" }, { ...good, version: 118 },
    { ...good, sha256: "ab".repeat(31) }, { ...good, sha256: "zz".repeat(32) }, { ...good, sha256: undefined },
    { ...good, size: 12 }, { ...good, size: 300 * 1024 * 1024 }, { ...good, size: "23107422" }, { ...good, size: 1.5e7 + 0.5 },
    { ...good, asset: "evil.exe" }, { ...good, notice: "x".repeat(201) }, { ...good, notice: "line\nbreak" },
    { ...good, notice: "bidi‮text" }, { ...good, notice: 5 }, [good], null,
  ];
  for (const b of bad) {
    const r = await send(announce(b));
    assert.equal(r.status, 400, JSON.stringify(b));
    assert.equal(r.body.ok, false);
  }
  const raw = await send(owner("/v1/admin/latest", { method: "POST", body: "not json" }));
  assert.equal(raw.status, 400);
  assert.equal(env.REPORTS.map.size, 0, "nothing stored");
});

test("announce: a wrong or missing key is 401; the browser cookie never reaches the route", async () => {
  const good = { version: "1.1.8", sha256: SHA, size: SIZE };
  assert.equal((await send(announce(good, "wrong"))).status, 401);
  assert.equal((await send(announce(good, null))).status, 401);
  assert.equal((await send(announce(good, "wrong", "GET"))).status, 401);
  // the right key through the form gives the cookie; the cookie alone is refused here
  const login = await worker.fetch(new Request("https://relay.example.test/v1/open/MA2-AAAAAA", {
    method: "POST", headers: { "content-type": "application/x-www-form-urlencoded", origin: "https://relay.example.test", "cf-connecting-ip": "203.0.113.9" },
    body: `key=${encodeURIComponent(env.OWNER_KEY)}&remember=1` }), env, ctx);
  const cookie = (login.headers.get("set-cookie") || "").split(";")[0];
  assert.match(cookie, /^ma2_owner=/);
  const withCookie = new Request("https://relay.example.test/v1/admin/latest", {
    method: "POST", headers: { cookie, "content-type": "application/json" }, body: JSON.stringify(good) });
  assert.equal((await send(withCookie)).status, 401);
  assert.equal(await env.REPORTS.get("config/latest.json"), null);
  // trimmed key works
  assert.equal((await send(announce(good, ` ${env.OWNER_KEY}\r\n`))).status, 200);
});

test("announce: the file wins over the vars, the answer changes at once, prev is kept, undo goes back", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7", UPDATE_NOTICE: "env notice", LATEST_SHA256: "cd".repeat(32), LATEST_SIZE: String(SIZE) });
  assert.deepEqual(await live(), { ok: true, latest: "1.1.7", notice: "env notice", sha256: "cd".repeat(32), size: SIZE,
                                   asset: "MechAssault 2 Recomp Installer.exe" });
  let r = await send(announce({ version: "1.1.8", sha256: SHA, size: SIZE, notice: "Please update to 1.1.8." }));
  assert.equal(r.status, 200);
  assert.equal(r.body.announced, "1.1.8");
  assert.equal(r.body.source, "file");
  assert.deepEqual(r.body.live, { ok: true, latest: "1.1.8", notice: "Please update to 1.1.8.", sha256: SHA, size: SIZE,
                                  asset: "MechAssault 2 Recomp Installer.exe" });
  assert.equal(r.body.prev, null, "no earlier announce");
  assert.deepEqual(await live(), r.body.live, "public answer = what the owner route says is live (cache refreshed)");
  r = await send(announce({ version: "1.1.9", sha256: "ef".repeat(32), size: SIZE + 1 }));
  assert.equal(r.body.live.latest, "1.1.9");
  assert.equal(r.body.live.notice, undefined, "a notice is per announce");
  assert.equal(r.body.prev.version, "1.1.8");
  const g = await send(owner("/v1/admin/latest"));
  assert.equal(g.status, 200);
  assert.equal(g.body.file.version, "1.1.9");
  r = await send(announce({ undo: true }));
  assert.equal(r.body.undone, true);
  assert.equal(r.body.live.latest, "1.1.8");
  assert.equal(r.body.prev.version, "1.1.9", "undo twice = redo");
  assert.equal((await live()).latest, "1.1.8");
  // with no prev, undo removes the file and the vars apply again
  env.REPORTS.map.delete("config/latest.prev.json");
  r = await send(announce({ undo: true }));
  assert.equal(r.body.source, "env");
  assert.equal((await live()).latest, "1.1.7");
});

test("announce: a damaged stored file falls back to the vars", async () => {
  env = makeEnv({ LATEST_VERSION: "1.1.7" });
  await env.REPORTS.put("config/latest.json", "{ not json");
  assert.equal((await live()).latest, "1.1.7");
  const e2 = makeEnv({ LATEST_VERSION: "1.1.7" });
  await e2.REPORTS.put("config/latest.json", JSON.stringify({ version: "1.1.8", sha256: "nothex", size: SIZE }));
  assert.equal((await live(e2)).latest, "1.1.7");
});

test("announce: report senders on an older version get the announced notice", async () => {
  await send(announce({ version: "1.1.8", sha256: SHA, size: SIZE, notice: "Please update to 1.1.8." }));
  const r = await send(await reportRequest({ headers: { "x-ma2-version": "1.1.7" } }));
  assert.equal(r.status, 200);
  assert.equal(r.body.message, "Please update to 1.1.8.");
  const same = await send(await reportRequest({ ip: "203.0.113.8", headers: { "x-ma2-version": "1.1.8" } }));
  assert.equal(same.body.message, "");
});

test("announce: the public route stays anonymous (no limiter, no GitHub, one stored file only)", async () => {
  await send(announce({ version: "1.1.8", sha256: SHA, size: SIZE }));
  for (let i = 0; i < 10; i++) assert.equal((await worker.fetch(latestReq(), env, ctx)).status, 200);
  assert.deepEqual([...env.REPORTS.map.keys()], ["config/latest.json"]);
  assert.equal(gh.calls.length, 0);
});

// ---------------------------------------------------------------- issue body with the player's text (opt-in)

test("ISSUE_PLAYER_TEXT=on puts the neutralised text back into the issue (4 fences)", async () => {
  env = makeEnv({ ISSUE_PLAYER_TEXT: "on" });
  const r = await send(await reportRequest({ text: "Hi @Mazillion ```js\nalert(1)```" }), env);
  assert.equal(r.status, 200);
  const is = gh.issues[0];
  assert.ok(is.body.includes("alert(1)"));
  assert.ok(!is.body.includes("@Mazillion"));
  assert.equal((is.body.match(/```/g) || []).length, 4);
});

// ---------------------------------------------------------------- retention (1.1.7)

function age(envX, id, days) {
  // Makes a report look `days` old: R2 upload time, sidecar and issue record.
  const t = new Date(Date.now() - days * 86400e3);
  for (const [k, v] of envX.REPORTS.map) if (k.includes(id)) v.uploaded = t;
  const rec = envX.REPORTS.map.get(`issues/${id}.json`);
  if (rec) {
    const j = JSON.parse(new TextDecoder().decode(rec.bytes));
    j.received = t.toISOString();
    rec.bytes = new TextEncoder().encode(JSON.stringify(j));
  }
}

test("retention: after 60 days the zip, the sidecar and the issue go (token without delete rights: emptied, closed, locked)", async () => {
  const old = await send(await reportRequest({ text: "old one" }));
  const fresh = await send(await reportRequest({ text: "new one" }));
  age(env, old.body.id, 61);
  age(env, fresh.body.id, 59);
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  const keys = [...env.REPORTS.map.keys()];
  assert.ok(!keys.some((k) => k.includes(old.body.id)), "nothing of the old report is left in R2: " + keys.join(","));
  assert.ok(keys.some((k) => k.endsWith(`${fresh.body.id}.zip`)), "the 59-day-old report stays");
  const oldIssue = gh.issues.find((x) => x.number === 1);
  assert.equal(oldIssue.title, `[${old.body.id}] removed`);
  assert.equal(oldIssue.body, `Report ${old.body.id} was removed (after 60 days).`);
  assert.equal(oldIssue.state, "closed");
  assert.deepEqual(gh.locked, [1]);
  const freshIssue = gh.issues.find((x) => x.number === 2);
  assert.ok(freshIssue.body.includes("System summary"), "the fresh issue is untouched");
});

test("retention: a token with admin rights deletes the old issue outright", async () => {
  gh.canDelete = true;
  const old = await send(await reportRequest());
  age(env, old.body.id, 70);
  const res = await (await worker.fetch(owner("/v1/admin/expire", { method: "POST" }), env, ctx)).json();
  assert.equal(res.deleted, 1);
  assert.equal(res.objects, 2);
  assert.deepEqual(gh.deleted, [1]);
  assert.equal(gh.issues.length, 0);
  assert.equal(env.REPORTS.map.size, 0, "R2 is empty");
});

test("retention: an issue opened before the record existed is found by title", async () => {
  const old = await send(await reportRequest());
  await env.REPORTS.delete(`issues/${old.body.id}.json`);
  // A 1.1.6-era report: no issues/ record. The R2 objects expire; the issue is retired through the queue.
  await env.REPORTS.put(`retire/${old.body.id}.json`, JSON.stringify({ id: old.body.id, received: new Date().toISOString() }));
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  assert.equal(gh.issues[0].state, "closed");
  assert.equal(await env.REPORTS.get(`retire/${old.body.id}.json`), null);
});

test("retention: GitHub down keeps the work for the next run; an expired token stops the run", async () => {
  const a = await send(await reportRequest({ ip: "192.0.2.1" }));
  const b = await send(await reportRequest({ ip: "192.0.2.2" }));
  age(env, a.body.id, 61);
  age(env, b.body.id, 61);
  gh.mode = "down";
  let res = await (await worker.fetch(owner("/v1/admin/expire", { method: "POST" }), env, ctx)).json();
  assert.equal(res.failed, 2);
  assert.equal(res.objects, 4, "R2 objects go even when GitHub is down");
  assert.ok(await env.REPORTS.get(`issues/${a.body.id}.json`), "issue record kept for the next run");
  gh.mode = "expired";
  gh.calls.length = 0;
  res = await (await worker.fetch(owner("/v1/admin/expire", { method: "POST" }), env, ctx)).json();
  assert.equal(res.stopped, true);
  assert.equal(res.failed, 1);
  gh.mode = "up";
  res = await (await worker.fetch(owner("/v1/admin/expire", { method: "POST" }), env, ctx)).json();
  assert.equal(res.issues, 2);
  assert.ok(gh.issues.every((x) => x.state === "closed"));
});

test("retention: nothing personal survives in the emptied issue (title, body, labels)", async () => {
  const old = await send(await reportRequest({ text: "my name is Jane", headers: { "x-ma2-signature": "c0000005 0019236B" } }));
  age(env, old.body.id, 61);
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  const is = gh.issues[0];
  assert.ok(!JSON.stringify([is.title, is.body, is.labels]).match(/Jane|c0000005|Version|1\.1\.6/));
});

// ---------------------------------------------------------------- owner delete (1.1.7)

test("owner delete: needs the key; deletes zip, sidecar, record and empties the issue", async () => {
  const r = await send(await reportRequest({ text: "please delete" }));
  const path = `/v1/report/${r.body.id}`;
  assert.equal((await worker.fetch(owner(path, { method: "DELETE" }, null), env, ctx)).status, 401);
  assert.equal((await worker.fetch(owner(path, { method: "DELETE" }, "wrong"), env, ctx)).status, 401);
  const res = await worker.fetch(owner(path, { method: "DELETE" }), env, ctx);
  assert.equal(res.status, 200);
  assert.deepEqual(await res.json(), { ok: true, id: r.body.id, zip: true, info: true, pending: false, issue: "emptied" });
  assert.equal(env.REPORTS.map.size, 0, "nothing left in R2");
  assert.equal(gh.issues[0].body, `Report ${r.body.id} was removed (deleted on request).`);
  assert.equal(gh.issues[0].state, "closed");
  assert.equal((await worker.fetch(owner(path), env, ctx)).status, 404, "download now 404");
});

test("owner delete with admin rights deletes the issue; unknown id reports nothing found", async () => {
  gh.canDelete = true;
  const r = await send(await reportRequest());
  const b = await (await worker.fetch(owner(`/v1/report/${r.body.id}`, { method: "DELETE" }), env, ctx)).json();
  assert.equal(b.issue, "deleted");
  assert.equal(gh.issues.length, 0);
  const n = await (await worker.fetch(owner("/v1/report/MA2-AAAAAA", { method: "DELETE" }), env, ctx)).json();
  assert.deepEqual(n, { ok: true, id: "MA2-AAAAAA", zip: false, info: false, pending: false, issue: "none" });
});

test("owner delete while GitHub is down: R2 cleared now, the issue queued and retired by the next cron", async () => {
  const r = await send(await reportRequest());
  gh.mode = "down";
  const b = await (await worker.fetch(owner(`/v1/report/${r.body.id}`, { method: "DELETE" }), env, ctx)).json();
  assert.equal(b.issue, "queued");
  assert.equal(b.zip, true);
  assert.ok(await env.REPORTS.get(`retire/${r.body.id}.json`));
  gh.mode = "up";
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  assert.equal(gh.issues[0].state, "closed");
  assert.deepEqual([...env.REPORTS.map.keys()], [], "queue and record gone");
});

test("owner delete of a report still pending (issue never opened): marker removed, no issue made later", async () => {
  gh.mode = "down";
  const r = await send(await reportRequest());
  gh.mode = "up";
  const b = await (await worker.fetch(owner(`/v1/report/${r.body.id}`, { method: "DELETE" }), env, ctx)).json();
  assert.equal(b.pending, true);
  assert.equal(b.issue, "none");
  await worker.scheduled({}, env, ctx);
  await ctx.drain();
  assert.equal(gh.issues.length, 0, "the retry did not open an issue for a deleted report");
});

test("owner routes stay absent without OWNER_KEY (DELETE too)", async () => {
  env = makeEnv({ OWNER_KEY: "" });
  const res = await worker.fetch(owner("/v1/report/MA2-AAAAAA", { method: "DELETE" }, "x"), env, ctx);
  assert.equal(res.status, 404);
});

// ---------------------------------------------------------------- one-click owner download (11-22)

import { ownerCookieValue, OPEN_TEXT } from "../src/worker.js";

const BASE = "https://relay.example.test";
function openReq(id, init = {}) {
  return new Request(`${BASE}/v1/open/${id}`, init);
}
function formReq(id, fields, headers = {}) {
  return openReq(id, {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded", origin: BASE, "cf-connecting-ip": "198.51.100.9", ...headers },
    body: new URLSearchParams(fields).toString(),
    redirect: "manual",
  });
}
async function cookieFor(e = env) {
  return `ma2_owner=${await ownerCookieValue(e)}`;
}

test("open: no cookie gives the key page (no-store, no framing, no referrer), never the zip", async () => {
  const r = await send(await reportRequest({ zip: tinyZip(100) }));
  const res = await worker.fetch(openReq(r.body.id), env, ctx);
  assert.equal(res.status, 200);
  assert.match(res.headers.get("content-type"), /^text\/html/);
  assert.equal(res.headers.get("cache-control"), "no-store");
  assert.equal(res.headers.get("x-frame-options"), "DENY");
  assert.match(res.headers.get("content-security-policy"), /frame-ancestors 'none'/);
  assert.equal(res.headers.get("referrer-policy"), "no-referrer");
  const html = await res.text();
  assert.ok(html.includes(OPEN_TEXT.ask) && html.includes('type="password"') && html.includes(OPEN_TEXT.remember));
  assert.ok(!html.includes("PK"), "no zip bytes");
  assert.equal(res.headers.get("set-cookie"), null);
});

test("open: a wrong key sets no cookie and gives the generic error on the same page", async () => {
  const r = await send(await reportRequest());
  const res = await worker.fetch(formReq(r.body.id, { key: "wrong", remember: "1" }), env, ctx);
  assert.equal(res.status, 401);
  assert.equal(res.headers.get("set-cookie"), null);
  const html = await res.text();
  assert.ok(html.includes(OPEN_TEXT.wrong) && html.includes('type="password"'));
  assert.ok(!html.includes("wrong\""), "the typed key is not echoed");
});

test("open: the right key sets an HttpOnly Secure Lax cookie for a year, then the zip downloads", async () => {
  const zip = tinyZip(321);
  const r = await send(await reportRequest({ zip }));
  const res = await worker.fetch(formReq(r.body.id, { key: env.OWNER_KEY, remember: "1" }), env, ctx);
  assert.equal(res.status, 303);
  assert.equal(res.headers.get("location"), `/v1/open/${r.body.id}`);
  const sc = res.headers.get("set-cookie");
  for (const flag of ["HttpOnly", "Secure", "SameSite=Lax", "Path=/v1/", "Max-Age=31536000"]) assert.ok(sc.includes(flag), flag);
  assert.ok(!sc.includes(env.OWNER_KEY), "the cookie is not the key");
  assert.ok(!res.headers.get("location").includes(env.OWNER_KEY));
  const cookie = sc.split(";")[0];
  const dl = await worker.fetch(openReq(r.body.id, { headers: { cookie } }), env, ctx);
  assert.equal(dl.status, 200);
  assert.equal(dl.headers.get("content-disposition"), `attachment; filename="${r.body.id}.zip"`);
  assert.equal(dl.headers.get("cache-control"), "no-store");
  assert.deepEqual(new Uint8Array(await dl.arrayBuffer()), zip);
});

test("open: without Remember the cookie lasts for the browser session only", async () => {
  const r = await send(await reportRequest());
  const res = await worker.fetch(formReq(r.body.id, { key: env.OWNER_KEY }), env, ctx);
  assert.equal(res.status, 303);
  assert.ok(!res.headers.get("set-cookie").includes("Max-Age"));
});

test("open: a cookie made before the key was rotated is refused (the key page again)", async () => {
  const r = await send(await reportRequest());
  const old = await cookieFor(env);
  const rotated = makeEnv({ OWNER_KEY: "a-new-owner-key" });
  rotated.REPORTS = env.REPORTS;
  const res = await worker.fetch(openReq(r.body.id, { headers: { cookie: old } }), rotated, ctx);
  assert.equal(res.status, 200);
  assert.match(res.headers.get("content-type"), /^text\/html/);
  assert.ok((await res.text()).includes('type="password"'));
});

test("open: the cookie reaches no other owner route (admin, info, delete, header-key download)", async () => {
  const r = await send(await reportRequest());
  const cookie = await cookieFor();
  const routes = [
    ["GET", "/v1/admin/reports"], ["GET", "/v1/admin/pending"], ["POST", "/v1/admin/retry"], ["POST", "/v1/admin/expire"],
    ["GET", "/v1/admin/switch"], ["POST", "/v1/admin/switch"], ["DELETE", `/v1/report/${r.body.id}`],
    ["GET", `/v1/report/${r.body.id}`], ["GET", `/v1/report/${r.body.id}/info`],
  ];
  for (const [method, path] of routes) {
    const res = await worker.fetch(new Request(BASE + path, { method, headers: { cookie } }), env, ctx);
    assert.equal(res.status, 401, `${method} ${path}`);
  }
  assert.ok(await env.REPORTS.get(`reports/${new Date().toISOString().slice(0, 7)}/${r.body.id}.zip`), "nothing deleted");
});

test("open: a malformed id is a plain 404; an unknown id without the cookie looks exactly like a real one", async () => {
  for (const bad of ["MA2-ABC", "MA2-000000", "..%2Fadmin", "MA2-ABCDEF.zip", "ma2-abcdef"]) {
    const res = await worker.fetch(openReq(bad), env, ctx);
    assert.equal(res.status, 404, bad);
    assert.ok(!(await res.text()).includes("password"), bad);
  }
  const real = await send(await reportRequest());
  const a = await (await worker.fetch(openReq(real.body.id), env, ctx)).text();
  const b = await (await worker.fetch(openReq("MA2-ZZZZZZ"), env, ctx)).text();
  assert.equal(a.replaceAll(real.body.id, "ID"), b.replaceAll("MA2-ZZZZZZ", "ID"), "existence is not revealed");
  const withCookie = await worker.fetch(openReq("MA2-ZZZZZZ", { headers: { cookie: await cookieFor() } }), env, ctx);
  assert.equal(withCookie.status, 404);
  assert.ok((await withCookie.text()).includes(OPEN_TEXT.missing));
});

test("open: a POST from another origin (or a cross-site fetch) is refused before the key is looked at", async () => {
  const r = await send(await reportRequest());
  let res = await worker.fetch(formReq(r.body.id, { key: env.OWNER_KEY }, { origin: "https://evil.example" }), env, ctx);
  assert.equal(res.status, 403);
  assert.equal(res.headers.get("set-cookie"), null);
  res = await worker.fetch(formReq(r.body.id, { key: env.OWNER_KEY }, { origin: "null" }), env, ctx);
  assert.equal(res.status, 403);
  const noOrigin = formReq(r.body.id, { key: env.OWNER_KEY }, { "sec-fetch-site": "cross-site" });
  noOrigin.headers.delete("origin");
  res = await worker.fetch(noOrigin, env, ctx);
  assert.equal(res.status, 403);
  res = await worker.fetch(openReq(r.body.id, { method: "POST", headers: { origin: BASE, "content-type": "application/json" }, body: "{}" }), env, ctx);
  assert.equal(res.status, 415);
});

test("open: key tries are limited per connection (10 an hour), then the page says so; a right key is also refused then", async () => {
  const r = await send(await reportRequest());
  for (let i = 0; i < 10; i++) assert.equal((await worker.fetch(formReq(r.body.id, { key: "nope" + i }), env, ctx)).status, 401);
  const res = await worker.fetch(formReq(r.body.id, { key: env.OWNER_KEY }), env, ctx);
  assert.equal(res.status, 429);
  assert.equal(res.headers.get("set-cookie"), null);
  assert.ok((await res.text()).includes(OPEN_TEXT.busy));
  const other = await worker.fetch(formReq(r.body.id, { key: env.OWNER_KEY }, { "cf-connecting-ip": "203.0.113.200" }), env, ctx);
  assert.equal(other.status, 303, "another connection is not affected");
  assert.ok(!env.GATE.storageOf().dump().includes("198.51.100.9"), "the IP is not stored");
});

test("open: Forget this browser clears the cookie", async () => {
  const res = await worker.fetch(openReq("forget"), env, ctx);
  assert.equal(res.status, 200);
  const sc = res.headers.get("set-cookie");
  assert.ok(sc.startsWith("ma2_owner=;") && sc.includes("Max-Age=0") && sc.includes("Path=/v1/"));
  assert.ok((await res.text()).includes(OPEN_TEXT.forgotten));
});

test("open: absent when OWNER_KEY is not set", async () => {
  env = makeEnv({ OWNER_KEY: "" });
  assert.equal((await worker.fetch(openReq("MA2-AAAAAA"), env, ctx)).status, 404);
});

// ---------------------------------------------------------------- owner key whitespace (11-22)

test("owner key: a trailing space or CRLF in the header, the form or the stored secret still matches", async () => {
  const zip = tinyZip(10);
  const r = await send(await reportRequest({ zip }));
  const path = `/v1/report/${r.body.id}`;
  for (const k of [env.OWNER_KEY + " ", " " + env.OWNER_KEY, env.OWNER_KEY + "\t"]) {
    assert.equal((await worker.fetch(owner(path, {}, k), env, ctx)).status, 200, JSON.stringify(k));
  }
  for (const k of [env.OWNER_KEY + " ", env.OWNER_KEY + "\r\n", env.OWNER_KEY + "\n"]) {
    assert.equal((await worker.fetch(formReq(r.body.id, { key: k }), env, ctx)).status, 303, JSON.stringify(k));
  }
  // The secret itself saved with a trailing newline: the plain key, header and form, still matches; the cookie too.
  const nl = makeEnv({ OWNER_KEY: "owner-secret-for-tests\r\n" });
  nl.REPORTS = env.REPORTS;
  assert.equal((await worker.fetch(owner(path, {}, "owner-secret-for-tests"), nl, ctx)).status, 200);
  assert.equal((await worker.fetch(formReq(r.body.id, { key: "owner-secret-for-tests" }, { "cf-connecting-ip": "192.0.2.77" }), nl, ctx)).status, 303);
  assert.equal(await ownerCookieValue(nl), await ownerCookieValue(env), "same cookie with or without the newline");
  // An empty or whitespace-only key never matches.
  assert.equal((await worker.fetch(owner(path, {}, "   "), env, ctx)).status, 401);
  const ws = makeEnv({ OWNER_KEY: "  \n" });
  assert.equal((await worker.fetch(owner(path, {}, "x"), ws, ctx)).status, 404, "a blank secret = owner routes off");
});

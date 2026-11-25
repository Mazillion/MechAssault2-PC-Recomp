// Report intake filters (REPORTS-1124 G1 and G2). Run: node --test test/intake.test.mjs (from the relay folder).
//  G1: an empty MA2 Report.exe report from a sandbox (helper, manual, no words, no install, no logs, a zip with only
//      system.txt) is stored in R2 as before but opens no GitHub issue and no conversation.
//  G2: build "test" or "smoke" is refused (4xx) and nothing is stored.
import { test, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import worker, { quietReport, zipNames } from "../src/worker.js";
import { makeEnv, MockCtx, MockGitHub, reportRequest, tinyZip, zipOf } from "./mocks.mjs";

const ID_RE = /^MA2-[A-Z2-7]{6}$/;
const SECRET = "c".repeat(64);
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

function owner(path) {
  return new Request("https://relay.example.test" + path, { headers: { "x-ma2-owner-key": env.OWNER_KEY } });
}

// The 8 sandbox reports of 2026-11-23/24 (REPORTS-1124 section 1, note 4): MA2 Report.exe run on its own in a VM with
// 2 GB, "4 threads" and the Basic Render Driver; no text, no install, no logs; the zip holds a ~666-byte system.txt.
const SANDBOX = [
  ["2LVTMU", "1.1.7", 19045], ["BKGYWQ", "1.1.7", 19045], ["GN7LJM", "1.1.7", 19045], ["RLE7K3", "1.1.7", 18362],
  ["B3ZAHU", "1.2.0", 19045], ["FQU2FK", "1.2.0", 18362], ["NUSSJW", "1.2.0", 19045], ["O6Q7OB", "1.2.0", 19045],
];

function sandboxSummary(version, winBuild) {
  return [
    `MechAssault 2 Recomp ${version} (build -) | report: manual | sent by MA2 Report ${version}`,
    "Installed: no recomp-install.json in the game folder",
    `Windows build ${winBuild} (x64)`,
    "CPU: Intel(R) Core(TM) i7-12700K CPU @ 4.20GHz, 4 threads",
    "Memory: 2.0 GB, 1.0 GB free; commit limit 4.0 GB, 3.1 GB free",
    "Power: mains on, battery none",
    "GPU: Microsoft Basic Render Driver 0 GB (vendor 1414 device 008C)",
    "Settings: none yet (the game has not been started)",
    "Game log: none",
    "",
  ].join("\n");
}

function systemTxt(summary) {
  const s = "Report problems at: https://github.com/Mazillion/MechAssault2-PC-Recomp/issues\n" + summary +
    "Included: system.txt\nNote: no game logs found in %LOCALAPPDATA%\\MechAssault2\\Logs\n";
  return s.padEnd(666, " ");
}

// A helper report as MA2 Report.exe sends it (build empty, kind manual); `over` changes one thing at a time.
async function helperReport({ version = "1.2.0", winBuild = 19045, text = "", summary, files, headers = {}, secret } = {}) {
  const sum = summary ?? sandboxSummary(version, winBuild);
  const zip = zipOf(files ?? { "system.txt": systemTxt(sum) });
  const fields = {};
  if (secret ?? (version === "1.2.0" ? SECRET : null)) fields.reply_secret = secret ?? SECRET;
  return reportRequest({
    text, summary: sum, zip, fields,
    headers: { "x-ma2-client": "helper", "x-ma2-version": version, "x-ma2-build": "", "x-ma2-kind": "manual", "x-ma2-signature": "", ...headers },
  });
}

const month = () => new Date().toISOString().slice(0, 7);

// ---------------------------------------------------------------- G1: the 8 sandbox reports

for (const [orig, version, winBuild] of SANDBOX) {
  test(`G1 sandbox report like ${orig} (${version}, build ${winBuild}): stored, no issue, no conversation`, async () => {
    const r = await send(await helperReport({ version, winBuild }));
    assert.equal(r.status, 200);
    assert.equal(r.body.ok, true);
    assert.match(r.body.id, ID_RE, "the player still gets a report number");
    const zkey = `reports/${month()}/${r.body.id}.zip`;
    const zip = await env.REPORTS.get(zkey);
    assert.ok(zip, "zip stored in R2");
    assert.equal(zip.customMetadata.client, "helper");
    assert.equal(zip.customMetadata.issue, "none");
    const side = await (await env.REPORTS.get(`reports/${month()}/${r.body.id}.json`)).json();
    assert.equal(side.text, "");
    assert.equal(side.issue, "none");
    assert.equal(side.replies, false);
    assert.equal(gh.issues.length, 0, "no GitHub issue");
    assert.equal(gh.calls.length, 0, "GitHub not called at all");
    assert.equal(await env.REPORTS.get(`pending/${r.body.id}.json`), null, "no pending marker (the cron would open it)");
    assert.equal(await env.REPORTS.get(`conv/${r.body.id}.json`), null, "no conversation");
    await worker.scheduled({}, env, ctx);
    await ctx.drain();
    assert.equal(gh.issues.length, 0, "the hourly retry opens nothing either");
  });
}

test("G1 the owner list shows a quiet report with issue 'none'", async () => {
  const r = await send(await helperReport());
  const list = await (await worker.fetch(owner("/v1/admin/reports?days=7"), env, ctx)).json();
  assert.equal(list.reports.find((x) => x.id === r.body.id).issue, "none");
});

// ---------------------------------------------------------------- G1: anything more still opens an issue

async function opensIssue(req) {
  const r = await send(await req);
  assert.equal(r.status, 200);
  assert.match(r.body.id, ID_RE);
  assert.equal(gh.issues.length, 1, "issue opened");
  assert.ok(gh.issues[0].title.startsWith(`[${r.body.id}]`));
  return r;
}

test("G1 a helper report with the player's words opens an issue (and its conversation)", async () => {
  const r = await opensIssue(helperReport({ text: "The game froze after a jump." }));
  assert.ok(await env.REPORTS.get(`conv/${r.body.id}.json`), "conversation kept for a 1.2.0 report with text");
});

test("G1 whitespace only counts as no text", async () => {
  const r = await send(await helperReport({ text: "  \n\t " }));
  assert.equal(r.status, 200);
  assert.equal(gh.issues.length, 0);
});

test("G1 a helper report with logs opens an issue", async () => {
  const summary = sandboxSummary("1.2.0", 19045).replace("Game log: none", "Game log: 20261124-100000-1.log");
  await opensIssue(helperReport({
    summary,
    files: { "system.txt": systemTxt(summary), "logs/20261124-100000-1.log": "[LAUNCH] build version=1.2.0\n" },
  }));
});

test("G1 a log in the zip opens an issue even when the summary says none", async () => {
  const summary = sandboxSummary("1.2.0", 19045);
  await opensIssue(helperReport({ files: { "system.txt": systemTxt(summary), "logs/install-20261124-100000.log": "x" } }));
});

test("G1 a helper report from an installed game opens an issue", async () => {
  const summary = sandboxSummary("1.2.0", 19045)
    .replace("Installed: no recomp-install.json in the game folder", "Installed: 1.2.0 (kit c5267209)\nLow-memory build: no");
  await opensIssue(helperReport({ summary, files: { "system.txt": systemTxt(summary), "recomp-install.json": "{}" } }));
});

test("G1 the same empty report from the installer opens an issue", async () => {
  await opensIssue(helperReport({ headers: { "x-ma2-client": "installer" } }));
});

test("G1 the same empty report from the game opens an issue", async () => {
  await opensIssue(helperReport({ headers: { "x-ma2-client": "game" } }));
});

test("G1 an empty helper crash or hang report opens an issue", async () => {
  await opensIssue(helperReport({ headers: { "x-ma2-kind": "crash", "x-ma2-signature": "0xc0000005:rva1234" } }));
});

test("G1 an unreadable zip directory opens an issue (when in doubt, file it)", async () => {
  const r = await send(await reportRequest({
    text: "", summary: sandboxSummary("1.2.0", 19045), zip: tinyZip(600),
    headers: { "x-ma2-client": "helper", "x-ma2-version": "1.2.0", "x-ma2-build": "", "x-ma2-kind": "manual", "x-ma2-signature": "" },
  }));
  assert.equal(r.status, 200);
  assert.equal(gh.issues.length, 1);
});

test("G1 quietReport and zipNames on their own", async () => {
  const meta = { client: "helper", kind: "manual" };
  const sum = sandboxSummary("1.1.7", 18362);
  const blob = (files) => new Blob([zipOf(files)]);
  assert.deepEqual(zipNames(zipOf({ "system.txt": "a", "logs/x.log": "b" })), ["system.txt", "logs/x.log"]);
  assert.deepEqual(zipNames(tinyZip()), []);
  assert.equal(zipNames(new Uint8Array(40)), null);
  assert.equal(await quietReport(meta, "", sum, blob({ "system.txt": systemTxt(sum) })), true);
  assert.equal(await quietReport(meta, "", sum.replace("\r", ""), blob({ "System.txt": "x" })), true);
  assert.equal(await quietReport(meta, "hi", sum, blob({ "system.txt": "x" })), false);
  assert.equal(await quietReport(meta, "", sum, blob({ "system.txt": "x", "pc_settings.ini": "y" })), false);
  assert.equal(await quietReport(meta, "", sum, blob({})), false);
  assert.equal(await quietReport(meta, "", sum.replace("Game log: none", "Game log: a.log"), blob({ "system.txt": "x" })), false);
  assert.equal(await quietReport({ client: "helper", kind: "hang" }, "", sum, blob({ "system.txt": "x" })), false);
  assert.equal(await quietReport(meta, "", sum, new Blob([new Uint8Array(70 * 1024)])), false, "large zips are never quiet");
});

// ---------------------------------------------------------------- G2: test builds are refused

for (const build of ["test", "smoke", "TEST", "Smoke"]) {
  test(`G2 build '${build}' in the header is refused with 403; nothing stored, no issue, no rate budget`, async () => {
    const r = await send(await reportRequest({ headers: { "x-ma2-build": build } }));
    assert.equal(r.status, 403);
    assert.equal(r.body.ok, false);
    assert.equal(r.body.id, null);
    assert.match(r.body.message, /test reports/);
    assert.equal(env.REPORTS.map.size, 0, "nothing in R2");
    assert.equal(gh.issues.length, 0);
    assert.equal(env.GATE.storageOf(), undefined, "limiter not touched");
  });
}

test("G2 build 'test' as a form field (no X-MA2 headers) is refused; nothing stored", async () => {
  const r = await send(await reportRequest({
    omit: ["x-ma2-client", "x-ma2-version", "x-ma2-build", "x-ma2-kind", "x-ma2-signature"],
    fields: { client: "installer", version: "1.2.0", build: "test", kind: "install-fail", signature: "build:link" },
  }));
  assert.equal(r.status, 403);
  assert.equal(env.REPORTS.map.size, 0);
  assert.equal(gh.issues.length, 0);
});

test("G2 the leaked test report (installer 1.2.0 build test, 'build:link') is refused", async () => {
  const r = await send(await reportRequest({
    text: "The build stopped at the link step.",
    headers: { "x-ma2-client": "installer", "x-ma2-version": "1.2.0", "x-ma2-build": "test", "x-ma2-kind": "install-fail", "x-ma2-signature": "build:link" },
    fields: { reply_secret: SECRET },
  }));
  assert.equal(r.status, 403);
  assert.equal(env.REPORTS.map.size, 0);
  assert.equal(gh.issues.length, 0);
});

test("G2 only the local test harness (ACCEPT_TEST_BUILDS=on) takes a test build", async () => {
  const local = makeEnv({ ACCEPT_TEST_BUILDS: "on" });
  const r = await send(await reportRequest({ headers: { "x-ma2-build": "test" } }), local);
  assert.equal(r.status, 200);
  assert.equal(gh.issues.length, 1);
  for (const v of ["", "off", "yes"]) {
    const e = makeEnv({ ACCEPT_TEST_BUILDS: v });
    assert.equal((await send(await reportRequest({ headers: { "x-ma2-build": "smoke" } }), e)).status, 403, v);
  }
});

test("G2 builds that only contain the words are accepted", async () => {
  for (const build of ["testing1", "smoke2", "c5267209", "1.2.0-test"]) {
    const r = await send(await reportRequest({ headers: { "x-ma2-build": build } }));
    assert.equal(r.status, 200, build);
  }
  assert.equal(gh.issues.length, 4);
});

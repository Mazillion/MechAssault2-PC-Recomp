// Local mock relay: the real worker.js behind a plain Node HTTP server, with in-memory R2, limiter and GitHub.
// For client tests (installer ui_flow, MA2 Report.exe) and for triage.py. No network, no packages.
//
//   node test/local-relay.mjs [port]            (default 8787; prints "LISTENING <port>" when ready)
// Env:  MOCK_GITHUB = up | down | expired | malformed | offline   (default up)
//       MOCK_FORCE_STATUS = 429 | 500 | ...          answer every upload with this status and {ok:false}
//       MOCK_DELAY_MS = n                            wait before answering uploads (timeout tests)
//       OWNER_KEY, DAILY_CAP, PER_IP_PER_HOUR, MAX_UPLOAD_BYTES, ACCEPT_REPORTS, NOTICE, LATEST_VERSION,
//       LATEST_SHA256, LATEST_SIZE, UPDATE_NOTICE, MIN_VERSION, ISSUE_PLAYER_TEXT, DELETE_ISSUES  (as on the Worker)
// Extra test-only routes: GET /__mock/issues  -> the issues the mock GitHub received; GET /__mock/comments -> its comments.
//       REPLY_POLLS_PER_HOUR (1.2.0 reply checks)
import http from "node:http";
import worker from "../src/worker.js";
import { makeEnv, MockCtx, MockGitHub } from "./mocks.mjs";

const port = parseInt(process.argv[2] || "8787", 10);
const over = {};
for (const k of ["OWNER_KEY", "DAILY_CAP", "PER_IP_PER_HOUR", "MAX_UPLOAD_BYTES", "ACCEPT_REPORTS", "NOTICE", "OFF_MESSAGE",
  "LATEST_VERSION", "LATEST_SHA256", "LATEST_SIZE", "UPDATE_NOTICE", "MIN_VERSION", "ISSUE_PLAYER_TEXT", "DELETE_ISSUES", "REPLY_POLLS_PER_HOUR"])
  if (process.env[k] !== undefined) over[k] = process.env[k];
over.PUBLIC_URL = `http://127.0.0.1:${port}`;
const env = makeEnv({ OWNER_KEY: "local-owner-key", ...over });
const gh = new MockGitHub();
gh.mode = process.env.MOCK_GITHUB || "up";
gh.install();
const forced = parseInt(process.env.MOCK_FORCE_STATUS || "0", 10);
const delay = parseInt(process.env.MOCK_DELAY_MS || "0", 10);

const server = http.createServer(async (req, res) => {
  try {
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const body = Buffer.concat(chunks);
    const url = `http://127.0.0.1:${port}${req.url}`;
    if (req.url === "/__mock/issues") {
      res.writeHead(200, { "content-type": "application/json" });
      return res.end(JSON.stringify(gh.issues));
    }
    if (req.url === "/__mock/comments") {
      res.writeHead(200, { "content-type": "application/json" });
      return res.end(JSON.stringify(gh.comments));
    }
    if (req.url === "/v1/report" && delay) await new Promise((r) => setTimeout(r, delay));
    if (req.url === "/v1/report" && forced) {
      res.writeHead(forced, { "content-type": "application/json" });
      return res.end(JSON.stringify({ ok: false, id: null, message: `forced ${forced} (mock)` }));
    }
    const headers = new Headers();
    for (const [k, v] of Object.entries(req.headers)) if (typeof v === "string") headers.set(k, v);
    headers.set("cf-connecting-ip", req.socket.remoteAddress || "unknown");
    const request = new Request(url, {
      method: req.method,
      headers,
      body: ["GET", "HEAD"].includes(req.method) ? undefined : body,
    });
    const ctx = new MockCtx();
    const out = await worker.fetch(request, env, ctx);
    const buf = Buffer.from(await out.arrayBuffer());
    res.writeHead(out.status, Object.fromEntries(out.headers));
    res.end(buf);
    await ctx.drain();
  } catch (e) {
    res.writeHead(500, { "content-type": "text/plain" });
    res.end("mock relay error: " + e.message);
  }
});
server.listen(port, "127.0.0.1", () => console.log(`LISTENING ${port}`));

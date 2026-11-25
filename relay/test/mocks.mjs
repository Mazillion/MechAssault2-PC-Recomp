// In-process mock of the parts of the Workers runtime the relay uses: R2 bucket, Durable Object namespace and
// storage, ExecutionContext, and the GitHub REST API (by replacing globalThis.fetch). No network, no packages.
import { Gate } from "../src/worker.js";

async function toBytes(v) {
  if (v === null || v === undefined) return new Uint8Array();
  if (typeof v === "string") return new TextEncoder().encode(v);
  if (v instanceof Uint8Array) return v;
  if (v instanceof ArrayBuffer) return new Uint8Array(v);
  if (typeof v.arrayBuffer === "function") return new Uint8Array(await v.arrayBuffer()); // Blob / File
  if (typeof v.getReader === "function") return new Uint8Array(await new Response(v).arrayBuffer());
  throw new TypeError("R2 mock: unsupported value");
}

function r2Object(key, rec) {
  return {
    key,
    size: rec.bytes.length,
    uploaded: rec.uploaded,
    httpMetadata: rec.httpMetadata,
    customMetadata: rec.customMetadata,
    get body() {
      return new Blob([rec.bytes]).stream();
    },
    arrayBuffer: async () => rec.bytes.slice().buffer,
    text: async () => new TextDecoder().decode(rec.bytes),
    json: async () => JSON.parse(new TextDecoder().decode(rec.bytes)),
  };
}

export class MockR2 {
  constructor() {
    this.map = new Map();
    this.failPut = false;
  }
  async put(key, value, opts = {}) {
    if (this.failPut) throw new Error("R2 put failed (mock)");
    const bytes = await toBytes(value);
    this.map.set(key, {
      bytes,
      uploaded: new Date(),
      httpMetadata: opts.httpMetadata || {},
      customMetadata: opts.customMetadata || {},
    });
    return r2Object(key, this.map.get(key));
  }
  async get(key) {
    const r = this.map.get(key);
    return r ? r2Object(key, r) : null;
  }
  async head(key) {
    const r = this.map.get(key);
    return r ? r2Object(key, r) : null;
  }
  async delete(keys) {
    for (const k of [].concat(keys)) this.map.delete(k);
  }
  async list({ prefix = "", limit = 1000, cursor, include = [] } = {}) {
    const keys = [...this.map.keys()].filter((k) => k.startsWith(prefix)).sort();
    const start = cursor ? parseInt(cursor, 10) : 0;
    const page = keys.slice(start, start + limit);
    const truncated = start + limit < keys.length;
    return {
      objects: page.map((k) => {
        const o = r2Object(k, this.map.get(k));
        if (!include.includes("customMetadata")) delete o.customMetadata;
        return o;
      }),
      truncated,
      cursor: truncated ? String(start + limit) : undefined,
    };
  }
  allBytes() {
    // Everything stored, as one latin1 string, for "is this value anywhere?" checks.
    let s = "";
    for (const [k, r] of this.map) {
      s += k + JSON.stringify(r.customMetadata) + Buffer.from(r.bytes).toString("latin1");
    }
    return s;
  }
}

export class MockStorage {
  constructor() {
    this.map = new Map();
    this.alarm = null;
  }
  async get(k) {
    const v = this.map.get(k);
    return v === undefined ? undefined : structuredClone(v);
  }
  async put(k, v) {
    this.map.set(k, structuredClone(v));
  }
  async delete(k) {
    return this.map.delete(k);
  }
  async list({ prefix = "" } = {}) {
    return new Map([...this.map].filter(([k]) => k.startsWith(prefix)).sort());
  }
  async getAlarm() {
    return this.alarm;
  }
  async setAlarm(t) {
    this.alarm = t;
  }
  dump() {
    return JSON.stringify([...this.map]);
  }
}

export class MockDONamespace {
  constructor(env) {
    this.env = env;
    this.instances = new Map();
  }
  idFromName(name) {
    return { name };
  }
  get(id) {
    if (!this.instances.has(id.name)) {
      const storage = new MockStorage();
      this.instances.set(id.name, { storage, obj: new Gate({ storage }, this.env) });
    }
    const inst = this.instances.get(id.name);
    return {
      fetch: (input, init) => inst.obj.fetch(new Request(input, init)),
    };
  }
  storageOf(name = "gate") {
    return this.instances.get(name)?.storage;
  }
  objOf(name = "gate") {
    return this.instances.get(name)?.obj;
  }
}

export class MockCtx {
  constructor() {
    this.promises = [];
  }
  waitUntil(p) {
    this.promises.push(p);
  }
  async drain() {
    const ps = this.promises.splice(0);
    await Promise.all(ps);
  }
}

// GitHub mock. mode: "up" | "down" (HTTP 500) | "expired" (401) | "malformed" (400, a token GitHub cannot parse)
// | "offline" (fetch throws).
export class MockGitHub {
  constructor() {
    this.mode = "up";
    this.issues = [];
    this.calls = [];
    this.next = 1;
    this.canDelete = false;   // GraphQL deleteIssue: true = the token has admin rights, false = FORBIDDEN
    this.deleted = [];
    this.locked = [];
    this.comments = [];
    this.realFetch = globalThis.fetch;
  }
  install() {
    globalThis.fetch = async (input, init = {}) => {
      const req = new Request(input, init);
      const url = new URL(req.url);
      if (url.hostname !== "api.github.com") throw new Error("test tried to reach the network: " + url.href);
      this.calls.push({ method: req.method, path: url.pathname, auth: req.headers.get("authorization") });
      if (this.mode === "offline") throw new TypeError("fetch failed (mock offline)");
      if (this.mode === "down") return new Response("{}", { status: 500 });
      if (this.mode === "expired") return new Response('{"message":"Bad credentials"}', { status: 401 });
      if (this.mode === "malformed") return new Response('{"message":"Bad request"}', { status: 400 });
      if (req.method === "POST" && /^\/repos\/[^/]+\/[^/]+\/issues$/.test(url.pathname)) {
        const b = await req.json();
        const n = this.next++;
        const issue = { number: n, node_id: `I_node${n}`, state: "open", ...b };
        this.issues.push(issue);
        return new Response(JSON.stringify(issue), { status: 201 });
      }
      if (req.method === "POST" && url.pathname === "/graphql") {
        const b = await req.json();
        const i = this.issues.findIndex((x) => x.node_id === b.variables?.id);
        if (!/deleteIssue/.test(b.query || "")) return new Response('{"errors":[{"type":"UNKNOWN"}]}', { status: 200 });
        if (i < 0) return new Response('{"data":null,"errors":[{"type":"NOT_FOUND","message":"no issue"}]}', { status: 200 });
        if (!this.canDelete)
          return new Response('{"data":{"deleteIssue":null},"errors":[{"type":"FORBIDDEN","message":"no admin rights"}]}', { status: 200 });
        this.deleted.push(this.issues[i].number);
        this.comments = this.comments.filter((c) => c.issue !== this.issues[i].number);
        this.issues.splice(i, 1);
        return new Response('{"data":{"deleteIssue":{"clientMutationId":null}}}', { status: 200 });
      }
      // 1.2.0: comments (the player's answers)
      const cm = url.pathname.match(/^\/repos\/[^/]+\/[^/]+\/issues\/(\d+)\/comments$/);
      if (cm && req.method === "POST") {
        const issue = this.issues.find((x) => x.number === Number(cm[1]));
        if (!issue) return new Response('{"message":"Not Found"}', { status: 404 });
        const c = { id: 9000 + this.comments.length + 1, issue: issue.number, ...(await req.json()) };
        this.comments.push(c);
        return new Response(JSON.stringify(c), { status: 201 });
      }
      const dm = url.pathname.match(/^\/repos\/[^/]+\/[^/]+\/issues\/comments\/(\d+)$/);
      if (dm && req.method === "DELETE") {
        const i = this.comments.findIndex((x) => x.id === Number(dm[1]));
        if (i < 0) return new Response('{"message":"Not Found"}', { status: 404 });
        this.comments.splice(i, 1);
        return new Response(null, { status: 204 });
      }
      let im = url.pathname.match(/^\/repos\/[^/]+\/[^/]+\/issues\/(\d+)(\/lock)?$/);
      if (im) {
        const issue = this.issues.find((x) => x.number === Number(im[1]));
        if (!issue) return new Response('{"message":"Not Found"}', { status: 404 });
        if (req.method === "PATCH" && !im[2]) {
          Object.assign(issue, await req.json());
          return new Response(JSON.stringify(issue), { status: 200 });
        }
        if (req.method === "PUT" && im[2]) {
          this.locked.push(issue.number);
          issue.locked = true;
          return new Response(null, { status: 204 });
        }
      }
      if (req.method === "GET" && url.pathname === "/search/issues") {
        const q = url.searchParams.get("q");
        const id = (q.match(/"(MA2-[0-9A-Z]{6})"/) || [])[1];
        const hits = this.issues.filter((i) => id && i.title.includes(id));
        return new Response(JSON.stringify({ total_count: hits.length, items: hits }), { status: 200 });
      }
      return new Response("{}", { status: 404 });
    };
  }
  uninstall() {
    globalThis.fetch = this.realFetch;
  }
}

export function makeEnv(over = {}) {
  const env = {
    GITHUB_REPO: "Mazillion/MechAssault2-Reports",
    GITHUB_TOKEN: "github_pat_TEST",
    OWNER_KEY: "owner-secret-for-tests",
    PUBLIC_URL: "https://relay.example.test",
    ACCEPT_REPORTS: "on",
    NOTICE: "",
    DAILY_CAP: "300",
    PER_IP_PER_HOUR: "5",
    MAX_UPLOAD_BYTES: "26214400",
    ...over,
  };
  env.REPORTS = new MockR2();
  env.GATE = new MockDONamespace(env);
  return env;
}

// A minimal valid zip: one stored member. Content is not checked by the relay beyond the magic bytes.
export function tinyZip(extra = 0) {
  const name = new TextEncoder().encode("system.txt");
  const data = new TextEncoder().encode("Version 1.1.6\n" + "x".repeat(extra));
  const lfh = new Uint8Array(30 + name.length);
  const dv = new DataView(lfh.buffer);
  dv.setUint32(0, 0x04034b50, true);
  dv.setUint16(26, name.length, true);
  dv.setUint32(18, data.length, true);
  dv.setUint32(22, data.length, true);
  lfh.set(name, 30);
  const eocd = new Uint8Array(22);
  new DataView(eocd.buffer).setUint32(0, 0x06054b50, true);
  const out = new Uint8Array(lfh.length + data.length + eocd.length);
  out.set(lfh, 0);
  out.set(data, lfh.length);
  out.set(eocd, lfh.length + data.length);
  return out;
}

// A real zip (stored members, local headers + central directory) of {name: text}, as the clients make them.
export function zipOf(files) {
  const enc = new TextEncoder();
  const local = [], central = [];
  let off = 0;
  for (const [n, t] of Object.entries(files)) {
    const name = enc.encode(n), data = enc.encode(t);
    const lfh = new Uint8Array(30 + name.length);
    const l = new DataView(lfh.buffer);
    l.setUint32(0, 0x04034b50, true); l.setUint16(4, 20, true);
    l.setUint32(18, data.length, true); l.setUint32(22, data.length, true); l.setUint16(26, name.length, true);
    lfh.set(name, 30);
    const cdh = new Uint8Array(46 + name.length);
    const c = new DataView(cdh.buffer);
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true);
    c.setUint32(20, data.length, true); c.setUint32(24, data.length, true); c.setUint16(28, name.length, true);
    c.setUint32(42, off, true);
    cdh.set(name, 46);
    local.push(lfh, data); central.push(cdh);
    off += lfh.length + data.length;
  }
  const cdSize = central.reduce((a, x) => a + x.length, 0);
  const eocd = new Uint8Array(22);
  const e = new DataView(eocd.buffer);
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, central.length, true); e.setUint16(10, central.length, true);
  e.setUint32(12, cdSize, true); e.setUint32(16, off, true);
  const parts = [...local, ...central, eocd];
  const out = new Uint8Array(parts.reduce((a, x) => a + x.length, 0));
  let p = 0;
  for (const x of parts) { out.set(x, p); p += x.length; }
  return out;
}

// Builds a real multipart request with an explicit Content-Length, as the clients send it.
export async function reportRequest({
  ip = "203.0.113.7",
  headers = {},
  text = "It crashed when I jumped.",
  summary = "Version: 1.1.6\nWindows: 10.0.22631",
  zip = tinyZip(),
  fields = {},
  omit = [],
  url = "https://relay.example.test/v1/report",
} = {}) {
  const form = new FormData();
  if (text !== null) form.set("text", text);
  if (summary !== null) form.set("summary", summary);
  if (zip !== null) form.set("zip", new Blob([zip], { type: "application/zip" }), "report.zip");
  for (const [k, v] of Object.entries(fields)) form.set(k, v);
  const tmp = new Request(url, { method: "POST", body: form });
  const body = new Uint8Array(await tmp.arrayBuffer());
  const h = {
    "content-type": tmp.headers.get("content-type"),
    "content-length": String(body.length),
    "cf-connecting-ip": ip,
    "x-ma2-client": "installer",
    "x-ma2-version": "1.1.6",
    "x-ma2-build": "33544eb",
    "x-ma2-kind": "crash",
    "x-ma2-signature": "c0000005 @ 0019236B",
    ...headers,
  };
  for (const k of omit) delete h[k];
  return new Request(url, { method: "POST", headers: h, body });
}

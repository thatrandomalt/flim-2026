// Sample file store: content-addressed by sha256, so a file is uploaded once per server
// lifetime no matter how many projects or channel states reference it. Plain HTTP next to
// the WebSocket (same port, same password):
//   POST /blobs/missing   body: sha256 hex per line  -> the ones not stored, one per line
//   HEAD /blob/<sha256>   200 + content-length | 404
//   GET  /blob/<sha256>   the file
//   PUT  /blob/<sha256>   store (streamed to disk; rejected if the content doesn't hash to <sha256>)
// Auth: header x-flim-key = sha256 hex of the server password (any value if no password is set).
// Files live in FLIM_BLOB_DIR (default: <tmp>/flim-blobs). On Fly that disk resets when the
// machine stops: fine, clients re-upload what is missing. Oldest files are evicted above
// FLIM_BLOB_STORE_MAX bytes.
import crypto from "crypto";
import fs from "fs";
import http from "http";
import os from "os";
import path from "path";

const DIR = process.env.FLIM_BLOB_DIR ?? path.join(os.tmpdir(), "flim-blobs");
const FILE_MAX = Number(process.env.FLIM_BLOB_FILE_MAX ?? 2000 * 1024 * 1024);
const STORE_MAX = Number(process.env.FLIM_BLOB_STORE_MAX ?? 4 * 1024 * 1024 * 1024);
const HEX = /^[0-9a-f]{64}$/;

fs.mkdirSync(DIR, { recursive: true });

function keyOk(req: http.IncomingMessage, password: string): boolean {
  if (!password) return true;
  const want = crypto.createHash("sha256").update(password, "utf8").digest("hex");
  const got = String(req.headers["x-flim-key"] ?? "");
  const h = (v: string) => crypto.createHash("sha256").update(v).digest();
  return crypto.timingSafeEqual(h(got), h(want));
}

const file = (hex: string) => path.join(DIR, hex);

function exists(hex: string): fs.Stats | null {
  try {
    return fs.statSync(file(hex));
  } catch {
    return null;
  }
}

// Keep the store under STORE_MAX: drop the least recently used files.
function evict(): void {
  let entries: { p: string; size: number; t: number }[] = [];
  for (const name of fs.readdirSync(DIR)) {
    if (!HEX.test(name)) continue;
    try {
      const st = fs.statSync(path.join(DIR, name));
      entries.push({ p: path.join(DIR, name), size: st.size, t: st.atimeMs });
    } catch {
      /* raced with a delete */
    }
  }
  let total = entries.reduce((a, e) => a + e.size, 0);
  if (total <= STORE_MAX) return;
  entries = entries.sort((a, b) => a.t - b.t);
  for (const e of entries) {
    if (total <= STORE_MAX) break;
    try {
      fs.unlinkSync(e.p);
      total -= e.size;
      console.log(`[blob] evicted ${path.basename(e.p).slice(0, 12)} (${e.size} B)`);
    } catch {
      /* ignore */
    }
  }
}

function readBody(req: http.IncomingMessage, max: number): Promise<string | null> {
  return new Promise((resolve) => {
    let n = 0;
    const parts: Buffer[] = [];
    req.on("data", (c: Buffer) => {
      n += c.length;
      if (n > max) {
        resolve(null);
        req.destroy();
      } else parts.push(c);
    });
    req.on("end", () => resolve(Buffer.concat(parts).toString("utf8")));
    req.on("error", () => resolve(null));
  });
}

// Returns true if the request was a blob request (handled).
export function handleBlob(req: http.IncomingMessage, res: http.ServerResponse, password: string): boolean {
  const url = (req.url ?? "").split("?")[0];
  const m = /^\/blob\/([0-9a-f]{64})$/.exec(url);
  const isMissing = url === "/blobs/missing";
  if (!m && !isMissing) return false;
  if (!keyOk(req, password)) {
    res.writeHead(401);
    res.end();
    req.resume();
    return true;
  }

  if (isMissing) {
    if (req.method !== "POST") {
      res.writeHead(405);
      res.end();
      return true;
    }
    readBody(req, 1024 * 1024).then((body) => {
      if (body === null) {
        res.writeHead(413);
        res.end();
        return;
      }
      const missing = body
        .split(/\s+/)
        .filter((h) => HEX.test(h) && !exists(h));
      res.writeHead(200, { "content-type": "text/plain" });
      res.end(missing.join("\n"));
    });
    return true;
  }

  const hex = m![1];
  if (req.method === "HEAD" || req.method === "GET") {
    const st = exists(hex);
    if (!st) {
      res.writeHead(404);
      res.end();
      return true;
    }
    res.writeHead(200, { "content-type": "application/octet-stream", "content-length": st.size });
    if (req.method === "HEAD") {
      res.end();
      return true;
    }
    const now = new Date();
    fs.utimes(file(hex), now, st.mtime, () => {}); // LRU: recently fetched files stay
    fs.createReadStream(file(hex)).pipe(res);
    return true;
  }

  if (req.method === "PUT") {
    const len = Number(req.headers["content-length"] ?? -1);
    if (!(len >= 0) || len > FILE_MAX) {
      res.writeHead(413);
      res.end();
      req.resume();
      return true;
    }
    if (exists(hex)) {
      res.writeHead(200);
      res.end();
      req.resume();
      return true;
    }
    const tmp = path.join(DIR, `.${hex}.${process.pid}.${Math.random().toString(36).slice(2)}`);
    const out = fs.createWriteStream(tmp);
    const hash = crypto.createHash("sha256");
    let got = 0;
    let failed = false;
    const fail = (code: number) => {
      if (failed) return;
      failed = true;
      out.destroy();
      fs.unlink(tmp, () => {});
      if (!res.headersSent) {
        res.writeHead(code);
        res.end();
      }
    };
    req.on("data", (c: Buffer) => {
      got += c.length;
      if (got > len) return fail(400);
      hash.update(c);
    });
    req.on("error", () => fail(400));
    req.pipe(out);
    out.on("error", () => fail(500));
    out.on("finish", () => {
      if (failed) return;
      if (got !== len || hash.digest("hex") !== hex) {
        console.log(`[blob] rejected ${hex.slice(0, 12)}: content does not match`);
        return fail(400);
      }
      fs.rename(tmp, file(hex), (err) => {
        if (err) return fail(500);
        console.log(`[blob] stored ${hex.slice(0, 12)} (${got} B)`);
        res.writeHead(201);
        res.end();
        try {
          evict();
        } catch {
          /* ignore */
        }
      });
    });
    return true;
  }

  res.writeHead(405);
  res.end();
  return true;
}

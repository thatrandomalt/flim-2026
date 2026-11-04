import crypto from "crypto";
import http from "http";
import { WebSocketServer, WebSocket } from "ws";
import { handleBlob } from "./blobs";
import { ClientMsg, PING_INTERVAL_MS, Role, UserInfo } from "./protocol";
import { Client, Session, send } from "./session";

const PORT = Number(process.env.PORT ?? 8080);
const MAX_PAYLOAD = 64 * 1024 * 1024; // 64 MB — an .flp snapshot in base64
// Shared server password (Fly: `fly secrets set FLIM_PASSWORD=...`). Unset = open server (local dev).
const PASSWORD = process.env.FLIM_PASSWORD ?? "";
if (!PASSWORD) console.warn("FLIM_PASSWORD is not set: anyone who knows the URL can join.");

// Limits for a server open to the internet (from upstream flim's public server):
// open WebSockets per IP, so one visitor can't fill the machine;
const MAX_CONNS_PER_IP = Number(process.env.FLIM_MAX_CONNS_PER_IP ?? 12);
// wrong passwords (WebSocket or sample store key) per IP: after FAIL_MAX in FAIL_WINDOW_MS that IP is refused
// until the window has passed, so a password can't be guessed by reconnecting;
const FAIL_MAX = Number(process.env.FLIM_FAIL_MAX ?? 8);
const FAIL_WINDOW_MS = 10 * 60_000;
// messages per connection, a token bucket (RATE_PER_S, burst RATE_BURST): beyond it the connection is closed;
// flim's own traffic (cursors every 50 ms, bursts of edits) stays far below it;
const RATE_PER_S = Number(process.env.FLIM_RATE_PER_S ?? 400);
const RATE_BURST = Number(process.env.FLIM_RATE_BURST ?? 4000);
// people per session (the leader included), and how soon a connection must host / join.
const MAX_PEOPLE = Number(process.env.FLIM_MAX_PEOPLE ?? 8);
const JOIN_TIMEOUT_MS = 20_000;

const fails = new Map<string, { n: number; since: number }>(); // ip -> wrong passwords in the current window
function failCount(ip: string): number {
  const f = fails.get(ip);
  if (f && Date.now() - f.since > FAIL_WINDOW_MS) { fails.delete(ip); return 0; }
  return f ? f.n : 0;
}
function failRecord(ip: string): void {
  const f = fails.get(ip);
  if (!f || Date.now() - f.since > FAIL_WINDOW_MS) fails.set(ip, { n: 1, since: Date.now() });
  else f.n++;
  if (failCount(ip) === FAIL_MAX) console.log(`[auth] ${ip}: ${FAIL_MAX} wrong passwords, refused for ${FAIL_WINDOW_MS / 60_000} min`);
}
const connsPerIp = new Map<string, number>();

function passwordOk(given: unknown): boolean {
  if (!PASSWORD) return true;
  const h = (v: string) => crypto.createHash("sha256").update(v, "utf8").digest();
  return crypto.timingSafeEqual(h(typeof given === "string" ? given : ""), h(PASSWORD));
}

const sessions = new Map<string, Session>();

const sha256 = (v: string) => crypto.createHash("sha256").update(v, "utf8").digest();
function roomPasswordOk(session: Session, given: unknown): boolean {
  return !session.passwordHash || crypto.timingSafeEqual(sha256(typeof given === "string" ? given : ""), session.passwordHash);
}
function badRoomPassword(ws: WebSocket, ip: string, code: string, user: UserInfo): void {
  failRecord(ip);
  console.log(`[room ${code}] wrong room password from ${user.name} (${ip})`);
  fail(ws, "bad_room_password", "Wrong room password.");
  setTimeout(() => ws.close(4001, "bad_room_password"), 1000);
}

// The room browser: one line per room, as plain text (flim.exe's window reads it). The password is the
// server's, percent-encoded (headers aren't UTF-8).
function roomList(): string {
  const rows = [...sessions.values()].sort((a, b) => a.code.localeCompare(b.code))
    .map((s) => `${s.code}\t${s.othersThan("")}\t${MAX_PEOPLE}\t${s.passwordHash ? 1 : 0}\n`);
  return "flim-rooms 1\n" + rows.join("");
}
function roomListPasswordOk(req: http.IncomingMessage): boolean {
  try { return passwordOk(decodeURIComponent(String(req.headers["x-flim-password"] ?? ""))); }
  catch { return false; }
}

function makeCode(): string {
  let code: string;
  do {
    code = Math.random().toString(36).slice(2, 8).toUpperCase();
  } while (sessions.has(code));
  return code;
}

// Health check for the Fly proxy (TCP connect here also wakes a stopped machine), and the
// sample file store (blobs.ts).
const httpServer = http.createServer((req, res) => {
  const ip = clientIp(req);
  if ((req.url?.startsWith("/blob") || req.url === "/rooms") && failCount(ip) >= FAIL_MAX) {   // (locked out: the store's key is the password too)
    res.writeHead(429);
    res.end();
    req.resume();
    return;
  }
  if (handleBlob(req, res, PASSWORD, () => failRecord(ip))) return;
  if (req.url === "/rooms" && req.method === "GET") {
    if (!roomListPasswordOk(req)) {
      failRecord(ip);
      res.writeHead(401);
      res.end();
      return;
    }
    res.writeHead(200, { "content-type": "text/plain", "cache-control": "no-store" });
    res.end(roomList());
  } else if (req.url === "/health" || req.url === "/") {
    res.writeHead(200, { "content-type": "text/plain" });
    res.end("flim-server ok\n");
  } else {
    res.writeHead(404);
    res.end();
  }
});

const wss = new WebSocketServer({ server: httpServer, maxPayload: MAX_PAYLOAD });

// Per-connection bookkeeping until the client joins a session.
interface Conn {
  client?: Client;
  session?: Session;
  tokens: number;      // message rate (RATE_PER_S / RATE_BURST)
  tokensAt: number;
}
function rateOk(conn: Conn): boolean {
  const now = Date.now();
  conn.tokens = Math.min(RATE_BURST, conn.tokens + ((now - conn.tokensAt) / 1000) * RATE_PER_S);
  conn.tokensAt = now;
  if (conn.tokens < 1) return false;
  conn.tokens -= 1;
  return true;
}
const conns = new WeakMap<WebSocket, Conn>();

function fail(ws: WebSocket, code: string, msg: string): void {
  send(ws, { t: "error", code, msg });
}
function full(ws: WebSocket, code: string): void {
  console.log(`[${code}] full (${MAX_PEOPLE} people): refused`);
  fail(ws, "full", `The session is full (${MAX_PEOPLE} people).`);
  setTimeout(() => ws.close(4004, "full"), 200);
}

// The address a connection comes from (IP bans). Behind fly.io's proxy, the one it reports (FLY_APP_NAME is set
// there); elsewhere the header could be made up, so the socket's own.
function clientIp(req: http.IncomingMessage): string {
  const fly = process.env.FLY_APP_NAME ? req.headers["fly-client-ip"] : undefined;
  return String((Array.isArray(fly) ? fly[0] : fly) ?? req.socket.remoteAddress ?? "").replace(/^::ffff:/, "");
}

wss.on("connection", (ws, req) => {
  const ip = clientIp(req);
  const n = (connsPerIp.get(ip) ?? 0) + 1;
  connsPerIp.set(ip, n);
  conns.set(ws, { tokens: RATE_BURST, tokensAt: Date.now() });
  (ws as any).isAlive = true;
  ws.on("pong", () => ((ws as any).isAlive = true));
  if (n > MAX_CONNS_PER_IP) {
    console.log(`[limit] ${ip}: more than ${MAX_CONNS_PER_IP} connections, refused`);
    fail(ws, "too_many", "Too many connections from your address.");
    ws.close(4008, "too_many");
  } else if (failCount(ip) >= FAIL_MAX) {
    fail(ws, "too_many_failures", "Too many wrong passwords from your address: try again in a few minutes.");
    ws.close(4001, "too_many_failures");
  }
  const joinTimer = setTimeout(() => { if (!conns.get(ws)?.session) ws.close(4002, "join_timeout"); }, JOIN_TIMEOUT_MS);

  ws.on("message", (raw) => {
    const conn = conns.get(ws);
    if (!conn) return;
    if (!rateOk(conn)) {
      console.log(`[limit] ${conn.client?.user.name ?? ip}: more than ${RATE_PER_S} messages a second, disconnected`);
      fail(ws, "rate", "Too many messages: disconnected.");
      ws.close(4008, "rate");
      return;
    }
    let msg: ClientMsg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return fail(ws, "bad_json", "Malformed message.");
    }

    // ---- pre-session messages ----
    if (!conn.session) {
      if ((msg.t === "host" || msg.t === "join" || msg.t === "join_room") && failCount(ip) >= FAIL_MAX) {
        fail(ws, "too_many_failures", "Too many wrong passwords from your address: try again in a few minutes.");
        ws.close(4001, "too_many_failures");
        return;
      }
      if ((msg.t === "host" || msg.t === "join" || msg.t === "join_room") && !passwordOk(msg.password)) {
        failRecord(ip);
        console.log(`[auth] wrong password from ${sanitizeUser(msg.user).name} (${ip})`);
        fail(ws, "bad_password", "Wrong server password.");
        // a little delay makes guessing slow; then hang up
        setTimeout(() => ws.close(4001, "bad_password"), 1000);
        return;
      }
      if (msg.t === "host") {
        const client: Client = { ws, user: sanitizeUser(msg.user), role: "leader", alive: true, ip };
        const code = makeCode();
        const session = new Session(code, client, () => sessions.delete(code));
        sessions.set(code, session);
        conn.client = client;
        conn.session = session;
        return;
      }
      if (msg.t === "join") {
        const session = sessions.get(msg.session?.toUpperCase?.() ?? "");
        if (!session) return fail(ws, "no_session", "Session not found.");
        const client: Client = { ws, user: sanitizeUser(msg.user), role: "member", alive: true, ip };
        if (!roomPasswordOk(session, msg.room_password)) return badRoomPassword(ws, ip, session.code, client.user);
        if (session.isBanned(client.user, ip)) return fail(ws, "kicked", "You were removed from this session. The host can let you back in");
        if (session.othersThan(client.user.id) >= MAX_PEOPLE) return full(ws, session.code);
        conn.client = client;
        conn.session = session;
        session.add(client);
        return;
      }
      if (msg.t === "join_room") {
        // join-or-create a named room; first in becomes leader. Lets two
        // clients meet without copying a session code.
        const code = String(msg.room ?? "").toUpperCase().replace(/[^A-Z0-9_-]/g, "").slice(0, 32) || "FLIM";
        let session = sessions.get(code);
        if (!session) {
          const client: Client = { ws, user: sanitizeUser(msg.user), role: "leader", alive: true, ip };
          session = new Session(code, client, () => sessions.delete(code));
          if (typeof msg.room_password === "string" && msg.room_password) session.passwordHash = sha256(msg.room_password);
          sessions.set(code, session);
          conn.client = client;
          conn.session = session;
          console.log(`[room ${code}] created by ${client.user.name} (leader)`);
        } else {
          const client: Client = { ws, user: sanitizeUser(msg.user), role: "member", alive: true, ip };
          if (!roomPasswordOk(session, msg.room_password)) return badRoomPassword(ws, ip, code, client.user);
          if (session.isBanned(client.user, ip)) {
            console.log(`[room ${code}] ${client.user.name} was removed: not let in`);
            fail(ws, "kicked", "You were removed from this session. The host can let you back in");
            setTimeout(() => ws.close(4003, "kicked"), 200);
            return;
          }
          if (session.othersThan(client.user.id) >= MAX_PEOPLE) return full(ws, code);
          conn.client = client;
          conn.session = session;
          session.add(client);
          console.log(`[room ${code}] ${client.user.name} joined (member); peers=${session.size}`);
        }
        return;
      }
      return fail(ws, "not_joined", "Send host/join/join_room first.");
    }

    // ---- in-session messages ----
    const { client, session } = conn as { client: Client; session: Session };
    if (!session.has(client)) return; // removed (kick): nothing of theirs counts any more
    switch (msg.t) {
      case "snapshot":
        session.setSnapshot(client, msg.flp);
        break;
      case "project_swap":
        session.projectSwap(client, msg.flp);
        break;
      case "op":
        if (msg.op.kind === "cur" || msg.op.kind === "alive") { session.relayOp(client, msg.op); break; } // frequent, not edits
        session.touchLocksOf(client.user.id); // editing keeps the user's locks fresh
        session.relayOp(client, msg.op);
        console.log(`[op] ${client.user.name}: ${JSON.stringify(msg.op).slice(0, 120)}`);
        break;
      case "lock_acquire":
        session.acquire(client, msg.key);
        break;
      case "lock_release":
        session.release(client, msg.key);
        break;
      case "admin": {
        const why = session.admin(client, msg.action, String(msg.id ?? ""), msg.on === true);
        if (why) fail(ws, session.leaderId === client.user.id ? "admin" : "not_allowed", why);
        break;
      }
      case "bye":
        ws.close(1000, "bye");
        break;
      default:
        fail(ws, "unknown", `Unknown message type.`);
    }
  });

  ws.on("close", () => {
    const conn = conns.get(ws);
    const left = (connsPerIp.get(ip) ?? 1) - 1;
    if (left > 0) connsPerIp.set(ip, left);
    else connsPerIp.delete(ip);
    clearTimeout(joinTimer);
    if (conn?.client) console.log(`[disconnect] ${conn.client.user.name}`);
    conn?.session?.remove(ws);
    conns.delete(ws);
  });

  ws.on("error", () => {
    /* close handler does cleanup */
  });
});

// Names are shown to peers (and in FL's hint bar): the client already restricts them to
// letters/digits/space/-_. ; the server only strips control and quote-ish characters.
function sanitizeUser(u: UserInfo | undefined): UserInfo {
  const clean = (v: unknown, max: number) =>
    String(v ?? "").replace(/[\u0000-\u001f\u007f"\\<>]/g, "").trim().slice(0, max);
  const id = clean(u?.id, 64) || `anon-${Math.random().toString(36).slice(2, 8)}`;
  const name = clean(u?.name, 24) || "Anonymous";
  return { id, name };
}

// Liveness: ping clients; terminate dead ones (triggers close → lock release / session end).
const ping = setInterval(() => {
  for (const ws of wss.clients) {
    if ((ws as any).isAlive === false) {
      ws.terminate();
      continue;
    }
    (ws as any).isAlive = false;
    try {
      ws.ping();
    } catch {
      /* ignore */
    }
  }
}, PING_INTERVAL_MS);

// Lock TTL safety sweep.
const sweep = setInterval(() => {
  for (const s of sessions.values()) s.sweepLocks();
  for (const ip of [...fails.keys()]) failCount(ip);   // (drops windows that have passed)
}, 5_000);

wss.on("close", () => {
  clearInterval(ping);
  clearInterval(sweep);
});

httpServer.listen(PORT, () => {
  console.log(`flim-server listening on :${PORT}`);
});

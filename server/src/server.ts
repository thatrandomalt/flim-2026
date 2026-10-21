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

function passwordOk(given: unknown): boolean {
  if (!PASSWORD) return true;
  const h = (v: string) => crypto.createHash("sha256").update(v, "utf8").digest();
  return crypto.timingSafeEqual(h(typeof given === "string" ? given : ""), h(PASSWORD));
}

const sessions = new Map<string, Session>();

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
  if (handleBlob(req, res, PASSWORD)) return;
  if (req.url === "/health" || req.url === "/") {
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
}
const conns = new WeakMap<WebSocket, Conn>();

function fail(ws: WebSocket, code: string, msg: string): void {
  send(ws, { t: "error", code, msg });
}

wss.on("connection", (ws) => {
  conns.set(ws, {});
  (ws as any).isAlive = true;
  ws.on("pong", () => ((ws as any).isAlive = true));

  ws.on("message", (raw) => {
    let msg: ClientMsg;
    try {
      msg = JSON.parse(raw.toString());
    } catch {
      return fail(ws, "bad_json", "Malformed message.");
    }
    const conn = conns.get(ws)!;

    // ---- pre-session messages ----
    if (!conn.session) {
      if ((msg.t === "host" || msg.t === "join" || msg.t === "join_room") && !passwordOk(msg.password)) {
        console.log(`[auth] wrong password from ${sanitizeUser(msg.user).name}`);
        fail(ws, "bad_password", "Wrong server password.");
        // a little delay makes guessing slow; then hang up
        setTimeout(() => ws.close(4001, "bad_password"), 1000);
        return;
      }
      if (msg.t === "host") {
        const client: Client = { ws, user: sanitizeUser(msg.user), role: "leader", alive: true };
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
        const client: Client = { ws, user: sanitizeUser(msg.user), role: "member", alive: true };
        conn.client = client;
        conn.session = session;
        session.add(client);
        return;
      }
      if (msg.t === "join_room") {
        // join-or-create a named room; first in becomes leader. Lets two
        // clients meet without copying a session code.
        const code = (msg.room || "FLIM").toUpperCase().slice(0, 32);
        let session = sessions.get(code);
        if (!session) {
          const client: Client = { ws, user: sanitizeUser(msg.user), role: "leader", alive: true };
          session = new Session(code, client, () => sessions.delete(code));
          sessions.set(code, session);
          conn.client = client;
          conn.session = session;
          console.log(`[room ${code}] created by ${client.user.name} (leader)`);
        } else {
          const client: Client = { ws, user: sanitizeUser(msg.user), role: "member", alive: true };
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
      case "bye":
        ws.close(1000, "bye");
        break;
      default:
        fail(ws, "unknown", `Unknown message type.`);
    }
  });

  ws.on("close", () => {
    const conn = conns.get(ws);
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
}, 5_000);

wss.on("close", () => {
  clearInterval(ping);
  clearInterval(sweep);
});

httpServer.listen(PORT, () => {
  console.log(`flim-server listening on :${PORT}`);
});

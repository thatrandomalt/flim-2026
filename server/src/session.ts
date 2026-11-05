import { WebSocket } from "ws";
import { AdminAction, LOCK_TTL_MS, NotesMode, Op, PeerInfo, Role, ServerMsg, UnitKey, UserInfo } from "./protocol";

export interface Client {
  ws: WebSocket;
  user: UserInfo;
  role: Role;
  alive: boolean; // for ping/pong liveness
  sync?: boolean; // sync-only: gets everything, nothing of theirs is relayed
  ip: string; // the address it connects from (IP bans)
}

// IP bans: every session on this server, until let back in (or the server restarts). Also by id and name.
const ipBans = new Map<string, UserInfo & { ip: string }>();
const matches = (b: UserInfo, u: UserInfo) => b.id === u.id || b.name.toLowerCase() === u.name.toLowerCase();

// What a sync-only member may still send: asking for things.
const SYNC_ONLY_OPS = new Set(["state.req", "hello", "shared.need", "notes.mergereq"]);

interface Lock {
  owner: string; // user id
  at: number;
}

export function send(ws: WebSocket, msg: ServerMsg): void {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

export class Session {
  readonly code: string;
  leaderId: string;
  passwordHash: Buffer | null = null; // the room's own password, if it has one (server.ts)
  private clients = new Set<Client>();
  private locks = new Map<UnitKey, Lock>();
  private bans = new Map<string, UserInfo>(); // removed (kick) until let back in; by id, and the name matches too
  private syncs = new Map<string, UserInfo>(); // sync-only, kept for a reconnect
  private seq = 0;
  private flp: string | null = null;
  private epoch = 0;
  private onEmpty: () => void;

  readonly notes: NotesMode;

  constructor(code: string, leader: Client, onEmpty: () => void, notes: NotesMode = "lock") {
    this.code = code;
    this.notes = notes;
    this.leaderId = leader.user.id;
    this.onEmpty = onEmpty;
    this.add(leader);
  }

  has(client: Client): boolean {
    return this.clients.has(client);
  }

  get size(): number {
    return this.clients.size;
  }
  // People other than `id` (distinct ids: a reconnect's old connection may still be here until it times out).
  othersThan(id: string): number {
    const ids = new Set<string>();
    for (const c of this.clients) if (c.user.id !== id) ids.add(c.user.id);
    return ids.size;
  }

  private peers(): PeerInfo[] {
    return [...this.clients].map((c) => ({ id: c.user.id, name: c.user.name, role: c.role, ...(c.sync ? { sync: true as const } : {}) }));
  }

  // The leader may use the host controls (give host, kick, sync-only): this server has no host window.
  private presence(): ServerMsg {
    const banned: Array<UserInfo & { ip?: true }> = [...this.bans.values()];
    for (const b of ipBans.values()) banned.push({ id: b.id, name: b.name, ip: true });
    return { t: "presence", peers: this.peers(), leaderControls: true, banned };
  }

  isBanned(u: UserInfo, ip: string): boolean {
    for (const b of this.bans.values()) if (matches(b, u)) return true;
    for (const b of ipBans.values()) if (matches(b, u) || b.ip === ip) return true;
    return false;
  }

  private broadcast(msg: ServerMsg, except?: Client): void {
    for (const c of this.clients) if (c !== except) send(c.ws, msg);
  }

  private clientOf(ws: WebSocket): Client | undefined {
    for (const c of this.clients) if (c.ws === ws) return c;
    return undefined;
  }

  add(client: Client): void {
    for (const b of this.syncs.values()) if (matches(b, client.user)) client.sync = true; // sync-only sticks
    this.clients.add(client);
    send(client.ws, { t: "welcome", session: this.code, role: client.role, you: client.user, peers: this.peers(), notes: this.notes });
    send(client.ws, { t: "snapshot", flp: this.flp, epoch: this.epoch });
    send(client.ws, { t: "lock_state", locks: [...this.locks].map(([key, l]) => ({ key, owner: l.owner })) });
    this.broadcast(this.presence()); // (the newcomer too: leaderControls, who was removed)
  }

  // Leader seeds the initial project blob (host upload).
  setSnapshot(from: Client, flp: string): void {
    if (from.user.id !== this.leaderId) {
      send(from.ws, { t: "error", code: "not_leader", msg: "Only the party leader can set the project." });
      return;
    }
    this.flp = flp;
    // epoch unchanged for the initial seed; members already got it via welcome/snapshot
    this.broadcast({ t: "snapshot", flp: this.flp, epoch: this.epoch }, from);
  }

  // Leader swaps the whole project; members must resync. Clears all locks.
  projectSwap(from: Client, flp: string): void {
    if (from.user.id !== this.leaderId) {
      send(from.ws, { t: "error", code: "not_leader", msg: "Can't change FLP as non party leader" });
      return;
    }
    this.flp = flp;
    this.epoch += 1;
    this.locks.clear();
    this.broadcast({ t: "project_swap", flp, epoch: this.epoch }, from);
  }

  relayOp(from: Client, op: Op): void {
    if (from.sync && !SYNC_ONLY_OPS.has(op.kind)) return; // sync-only: only asking for things goes through
    // Backstop: if the op targets a unit locked by someone else, reject it.
    if (op.key) {
      const l = this.locks.get(op.key);
      if (l && l.owner !== from.user.id) {
        send(from.ws, { t: "error", code: "locked", msg: `Unit ${op.key} is locked by ${l.owner}` });
        return;
      }
    }
    this.seq += 1;
    this.broadcast({ t: "op", seq: this.seq, from: from.user.id, op }, from);
  }

  acquire(from: Client, key: UnitKey): void {
    if (from.sync) return; // sync-only: edits nothing, locks nothing
    const now = Date.now();
    const existing = this.locks.get(key);
    if (existing && existing.owner !== from.user.id && now - existing.at < LOCK_TTL_MS) {
      send(from.ws, { t: "lock", key, granted: false, owner: existing.owner });
      return;
    }
    this.locks.set(key, { owner: from.user.id, at: now });
    send(from.ws, { t: "lock", key, granted: true, owner: from.user.id });
    this.broadcast({ t: "lock", key, granted: true, owner: from.user.id }, from);
  }

  release(from: Client, key: UnitKey): void {
    const l = this.locks.get(key);
    if (l && l.owner === from.user.id) {
      this.locks.delete(key);
      this.broadcast({ t: "lock_released", key });
    }
  }

  // Safety sweep: drop locks whose owner went silent past the TTL.
  sweepLocks(): void {
    const now = Date.now();
    for (const [key, l] of this.locks) {
      if (now - l.at >= LOCK_TTL_MS) {
        this.locks.delete(key);
        this.broadcast({ t: "lock_released", key });
      }
    }
  }

  // Refresh a lock's timestamp while its owner keeps editing (op heartbeat).
  touchLocksOf(userId: string): void {
    const now = Date.now();
    for (const l of this.locks.values()) if (l.owner === userId) l.at = now;
  }

  private dropLocks(userId: string): void {
    for (const [key, l] of this.locks) {
      if (l.owner === userId) {
        this.locks.delete(key);
        this.broadcast({ t: "lock_released", key });
      }
    }
  }

  // Host controls from the session leader (see protocol.ts AdminAction). Returns "" when done, else why not.
  admin(by: Client, action: AdminAction, id: string, on: boolean): string {
    if (by.user.id !== this.leaderId) return "Only the session leader can do that.";
    if (action === "unban") {
      const b = this.bans.get(id) ?? ipBans.get(id);
      if (!b) return "Nobody by that id was removed.";
      this.bans.delete(id);
      ipBans.delete(id);
      console.log(`[room ${this.code}] ${by.user.name} let ${b.name} back in`);
    } else {
      const t = [...this.clients].find((c) => c.user.id === id);
      if (!t) return "That person isn't in the session (any more).";
      if (action === "give_host") {
        if (t.role === "leader") return "";
        by.role = "member";
        t.role = "leader";
        t.sync = false;
        this.syncs.delete(t.user.id);
        this.leaderId = t.user.id;
        console.log(`[room ${this.code}] ${by.user.name} made ${t.user.name} the session leader`);
      } else if (action === "kick" || action === "ban_ip") {
        if (t === by) return "You can't remove yourself.";
        if (action === "kick") this.bans.set(t.user.id, t.user);
        else ipBans.set(t.user.id, { ...t.user, ip: t.ip });
        console.log(`[room ${this.code}] ${by.user.name} ${action === "kick" ? "removed" : "banned the IP address of"} ${t.user.name}`);
        const msg = action === "kick" ? `${by.user.name} removed you from the session` : `${by.user.name} banned you from this server`;
        send(t.ws, { t: "error", code: "kicked", msg });
        this.remove(t.ws); // (presence goes out from there)
        t.ws.close(4003, "kicked");
        return "";
      } else if (action === "sync_only") {
        if (t.role === "leader" && on) return "The session leader can't be sync-only: make someone else the leader first.";
        if (on) this.syncs.set(t.user.id, t.user);
        else this.syncs.delete(t.user.id);
        if (!!t.sync === on) return "";
        t.sync = on;
        if (on) this.dropLocks(t.user.id);
        console.log(`[room ${this.code}] ${by.user.name} set ${t.user.name} to ${on ? "sync-only" : "normal"}`);
      } else return "Unknown action.";
    }
    this.broadcast(this.presence());
    return "";
  }

  remove(ws: WebSocket): void {
    const client = this.clientOf(ws);
    if (!client) return;
    this.clients.delete(client);
    this.dropLocks(client.user.id); // release this user's locks

    if (this.clients.size === 0) return this.onEmpty();
    // the leader left: the longest-connected member leads now (presence carries the roles)
    if (client.user.id === this.leaderId) {
      const heir = this.clients.values().next().value as Client;
      heir.role = "leader";
      heir.sync = false;
      this.leaderId = heir.user.id;
    }
    this.broadcast(this.presence());
  }

  allClients(): Client[] {
    return [...this.clients];
  }
}

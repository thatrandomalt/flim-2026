// flim wire protocol. JSON messages over WebSocket; the `.flp` snapshot is
// carried as base64 in `flp`. Keep this file in sync with the FL client.

export type Role = "leader" | "member";
export type NotesMode = "lock" | "merge";
export const CAP_NOTES_MERGE = "notes.merge";

export interface UserInfo {
  id: string;
  name: string;
}

export interface PeerInfo {
  id: string;
  name: string;
  role: Role;
  sync?: true; // sync-only: gets everything, nothing of theirs is relayed
}

// Host controls (the session leader's; this server has no host window): give host, remove (banned from the
// session until let back in), ban their IP address (every session on this server, until let back in or the
// server restarts), sync-only on / off (sticks across a reconnect), let a removed / IP banned person back in.
export type AdminAction = "give_host" | "kick" | "ban_ip" | "sync_only" | "unban";

// A unit key identifies the lockable edit atom. Opaque to the server; the FL client uses:
//  - parameter:        `param:<recEventId>`     (channel / mixer knobs, EQ, slot on/mix, tempo...)
//  - plugin:           `plug:ch:<channel>` / `plug:fx:<track>:<slot>`
//  - notes:            `pat:<pattern>:<channel>` (one channel's notes in one pattern)
//  - playlist clips:   `clips:<arrangement>:<track>` (one playlist track, FL's reversed index)
export type UnitKey = string;

// An op is opaque to the server: it only orders and relays it. The FL client
// defines/interprets the payload. `key` (optional) ties the op to a lock unit.
// `samples` (optional, project.load / plug.state / chan.add) lists the sample files the op's
// data uses: [{p: base64 UTF-16 path, h: sha256 hex, s: bytes, ms?: WAV length}], fetched from
// /blob/<h> (blobs.ts). The sender uploads them after the op; receivers retry on 404.
export interface Op {
  kind: string;
  key?: UnitKey;
  [k: string]: unknown;
}

// ---- client -> server ----
export type ClientMsg =
  // `password`: the server's FLIM_PASSWORD (ignored when the server has none)
  // `room_password`: the room's own (optional); whoever creates a room sets it
  | { t: "host"; user: UserInfo; password?: string }
  | { t: "join"; user: UserInfo; session: string; password?: string; room_password?: string; caps?: string[] }
  // `notes`: when this creates the room, what happens when two people edit the same notes: "lock" (default) or
  // "merge" (both kept; engine's notes.merge ops). `caps`: "notes.merge" = this client can merge
  | { t: "join_room"; user: UserInfo; room: string; password?: string; room_password?: string; notes?: NotesMode; caps?: string[] } // join-or-create a named room
  | { t: "snapshot"; flp: string } // leader seeds/updates the project blob
  | { t: "project_swap"; flp: string } // leader only; replaces the project
  | { t: "op"; op: Op }
  | { t: "lock_acquire"; key: UnitKey }
  | { t: "lock_release"; key: UnitKey }
  | { t: "admin"; action: AdminAction; id: string; on?: boolean } // session leader only
  | { t: "bye" };

// ---- server -> client ----
export type ServerMsg =
  | { t: "welcome"; session: string; role: Role; you: UserInfo; peers: PeerInfo[]; notes: NotesMode }
  | { t: "snapshot"; flp: string | null; epoch: number }
  | { t: "op"; seq: number; from: string; op: Op }
  | { t: "lock"; key: UnitKey; granted: boolean; owner: string | null }
  | { t: "lock_state"; locks: Array<{ key: UnitKey; owner: string }> }
  | { t: "lock_released"; key: UnitKey }
  | { t: "project_swap"; flp: string; epoch: number } // members: show resync notice, reload
  | { t: "presence"; peers: PeerInfo[]; leaderControls: boolean; banned: Array<UserInfo & { ip?: true }> } // (never the address)
  | { t: "error"; code: string; msg: string }; // code "kicked": removed by the host (the connection closes)

export const LOCK_TTL_MS = 30_000; // safety auto-release if a client never releases
export const PING_INTERVAL_MS = 15_000;

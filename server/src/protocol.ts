// flim wire protocol. JSON messages over WebSocket; the `.flp` snapshot is
// carried as base64 in `flp`. Keep this file in sync with the FL client.

export type Role = "leader" | "member";

export interface UserInfo {
  id: string;
  name: string;
}

export interface PeerInfo {
  id: string;
  name: string;
  role: Role;
}

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
  | { t: "host"; user: UserInfo; password?: string }
  | { t: "join"; user: UserInfo; session: string; password?: string }
  | { t: "join_room"; user: UserInfo; room: string; password?: string } // join-or-create a named room
  | { t: "snapshot"; flp: string } // leader seeds/updates the project blob
  | { t: "project_swap"; flp: string } // leader only; replaces the project
  | { t: "op"; op: Op }
  | { t: "lock_acquire"; key: UnitKey }
  | { t: "lock_release"; key: UnitKey }
  | { t: "bye" };

// ---- server -> client ----
export type ServerMsg =
  | { t: "welcome"; session: string; role: Role; you: UserInfo; peers: PeerInfo[] }
  | { t: "snapshot"; flp: string | null; epoch: number }
  | { t: "op"; seq: number; from: string; op: Op }
  | { t: "lock"; key: UnitKey; granted: boolean; owner: string | null }
  | { t: "lock_state"; locks: Array<{ key: UnitKey; owner: string }> }
  | { t: "lock_released"; key: UnitKey }
  | { t: "project_swap"; flp: string; epoch: number } // members: show resync notice, reload
  | { t: "presence"; peers: PeerInfo[] }
  | { t: "error"; code: string; msg: string };

export const LOCK_TTL_MS = 30_000; // safety auto-release if a client never releases
export const PING_INTERVAL_MS = 15_000;

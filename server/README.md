# flim-server

WebSocket relay for [flim](../README.md). `flim.exe` has the same server built in (**Host a session**,
`client/launcher/host.c`); this Node.js version is for a dedicated machine that runs without anyone's
FL. It doesn't understand FL Studio. It:
- puts every edit (`op`) in one order and relays it to the other people in the session,
- gives out per-unit **locks** (a knob, a plugin, one channel's notes in a pattern, one playlist track) so two people can't edit the
  same thing at once; locks expire after 30 s if a client stops releasing them,
- tracks who's in the session (the first to join is the leader; if the leader leaves, whoever has been
  in it longest leads, and a `presence` with the new roles tells everyone),
- checks the shared **password** (`FLIM_PASSWORD`),
- stores **sample files** so each one is uploaded once and the others fetch only what they're
  missing (`src/blobs.ts`, plain HTTP on the same port).

Sessions are kept in memory only. When the machine stops, the session is gone; the next person to
connect starts a new one from their project. Sample files are a disk cache: anything missing is uploaded
again by the next session that needs it.

## Sample store
Content-addressed by SHA-256. Auth: header `x-flim-key` = SHA-256 hex of the password.
| Request | Does |
|---|---|
| `POST /blobs/missing` | body: one hash per line → the ones not stored, one per line |
| `HEAD /blob/<sha256>` | 200 + `content-length`, or 404 |
| `GET /blob/<sha256>` | the file |
| `PUT /blob/<sha256>` | store it (streamed to disk; rejected unless the content hashes to `<sha256>`) |

| Variable | Default |
|---|---|
| `FLIM_BLOB_DIR` | `<system temp>/flim-blobs` |
| `FLIM_BLOB_STORE_MAX` | 4 GB in total; least recently used files are deleted above it |
| `FLIM_BLOB_FILE_MAX` | 2000 MB per file |

On fly.io the disk is reset when the machine stops, which is fine for a cache. Sample downloads
count towards fly.io's outbound data transfer.

## Run locally
Requires Node.js 20+.
```bash
npm install
npm run build
npm start                 # :8080, or set PORT
npm run dev               # auto-restarting dev mode (tsx watch)
curl http://localhost:8080/health    # -> flim-server ok
```
Set `FLIM_PASSWORD` to require a password. If it's not set, the server lets in anyone who
knows its address (a warning is logged).

Limits (the same in flim's built-in host, `client/launcher/host.c`):

| Variable | Default | |
|---|---|---|
| `FLIM_MAX_PEOPLE` | 8 | people per session; someone already in may always reconnect (error `full`) |
| `FLIM_MAX_CONNS_PER_IP` | 12 | open connections per address (error `too_many`) |
| `FLIM_FAIL_MAX` | 8 | wrong passwords / sample-store keys per address in 10 minutes, then refused until they're over (error `too_many_failures`; HTTP 429) |
| `FLIM_RATE_PER_S`, `FLIM_RATE_BURST` | 400, 4000 | messages per connection (token bucket); beyond it the connection is closed (error `rate`) |

A connection that hasn't joined a session after 20 s is closed. `client/dev/server_limits.py
<host:port> <password> <max people>` checks them all against a fresh server.

## Deploy to fly.io
```bash
fly auth login
fly launch --no-deploy --copy-config --name <your-unique-app-name>
fly secrets set FLIM_PASSWORD=<something-long>
fly deploy
```
Clients connect to `wss://<your-unique-app-name>.fly.dev`.

- `fly.toml` scales to zero (`auto_stop_machines = "stop"`, `min_machines_running = 0`): no cost
  while idle, and it wakes in a second or two when someone connects. Set `min_machines_running = 1`
  for an always-warm instance.
- Change `primary_region` to the region closest to you and your friends (`fly platform regions`).
- `MAX_PAYLOAD` in `src/server.ts` (64 MB) limits the size of a project sent as one message.

## Protocol
JSON over WebSocket; see `src/protocol.ts`. A client sends
`{"t":"join_room","room":"FLIM","password":"…","room_password":"…","user":{"id":"…","name":"…"}}` and gets `welcome`
(its role plus the peer list), then exchanges `op`, `lock_acquire` / `lock_release` (→ `lock`,
`lock_released`), and receives `presence` updates. Ops that carry a project or a plugin state may
list the samples it uses (`"samples":[{"p":…,"h":<sha256>,"s":<bytes>,"ms":<WAV length, optional>}]`), fetched from `/blob/<h>`.
The op is sent before its samples are uploaded: receivers retry `GET /blob/<h>` (404 = still uploading) and use a
silent placeholder file meanwhile. Two ops are signals rather than edits: the leader sends `alive` every
3 s (members warn after 30 s without one) and `saving` `on:1` / `on:0` around FL saving the project
(members hold their input meanwhile); like `cur` (mouse positions) they don't keep locks alive. A wrong password gets
`{"t":"error","code":"bad_password"}` and the connection is closed with code 4001.

**Rooms.** `join_room` joins the room or creates it; the creator's `room_password` (optional) then guards it, and
goes when the room ends. Room names keep only `A-Z 0-9 - _` after upper-casing, up to 32 characters; empty means
`FLIM`. A wrong room password gets `{"t":"error","code":"bad_room_password"}` and close code 4001, and counts
toward the wrong-password lockout. `GET /rooms` lists them as plain text for flim's room browser, with the
server password percent-encoded in an `x-flim-password` header (401 if wrong, 429 when locked out):
```
flim-rooms 1
<name>\t<people>\t<max people>\t<1 if it has a password>
```
flim's built-in host has no `/rooms` and ignores `room_password`. `npm run build && npm test` runs the rooms test.

**Merging notes.** When `join_room` creates a room, `"notes":"merge"` makes it merge notes two people edit at once
(default `"lock"`); `welcome` tells everyone with its `notes` field. Clients that can merge send
`"caps":["notes.merge"]`, and a merging room refuses others with `{"t":"error","code":"notes_merge"}`. On the built-in
host, the host's checkbox decides instead. The server only relays the ops: `notes.merge` (a change), `notes.mergefull`
(someone's whole state for a pattern) and `notes.mergereq` (asks the leader for it; `"pat":0` = all). Sync-only members
can still send `notes.mergereq`.

**Host controls.** `presence` also carries `leaderControls` (whether the leader may use them: always here,
since this server has no host window; flim's built-in host only when its host allows it) and `banned`
(`[{"id","name","ip"?:true}]`, the people removed or IP banned; never the address). A peer that is
sync-only has `"sync":true`. The leader sends `{"t":"admin","action":…,"id":<peer id>,"on":<bool>}`:
- `give_host`: that peer leads now (a `presence` with the new roles; nobody reloads).
- `sync_only` (`on`): the server drops every op of theirs except `state.req`, `hello` and `shared.need`, and
  ignores their lock requests; their locks are released. Kept by id and name for a reconnect.
- `kick`: they get `{"t":"error","code":"kicked","msg":…}` and are disconnected (close code 4003); the same
  id or name can't join the session again. `ban_ip`: the same, and their address (or id or name) can't join
  any session on this server.
- `unban`: lifts a kick or an IP ban.
Anyone else gets `error` `not_allowed`; a refused action (e.g. making the leader sync-only) `admin`.

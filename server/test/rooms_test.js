// Rooms: the list, the server password, room passwords. Needs a build: npm run build && npm test
const assert = require("assert");
const { spawn } = require("child_process");
const path = require("path");
const WebSocket = require("ws");

const PORT = 18477;
const srv = spawn(process.execPath, [path.join(__dirname, "../dist/server.js")], {
  env: { ...process.env, PORT: String(PORT), FLIM_PASSWORD: "sërver", FLIM_FAIL_MAX: "100" },
  stdio: "ignore",
});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const list = async (pw = "sërver") => {
  const r = await fetch(`http://localhost:${PORT}/rooms`, { headers: { "x-flim-password": encodeURIComponent(pw) } });
  return { status: r.status, text: await r.text() };
};
// Joins a room; resolves with the first answer that says how it went, and the socket.
const join = (room, room_password, name) => new Promise((resolve, reject) => {
  const ws = new WebSocket(`ws://localhost:${PORT}`);
  ws.on("open", () => ws.send(JSON.stringify({ t: "join_room", room, password: "sërver", room_password, user: { id: name, name } })));
  ws.on("message", (m) => {
    const msg = JSON.parse(m.toString());
    if (msg.t === "welcome" || msg.t === "error") resolve({ msg, ws });
  });
  ws.on("error", reject);
});

(async () => {
  for (let i = 0; ; i++) {
    try { await list(); break; } catch (e) { if (i > 50) throw e; await sleep(100); }
  }
  assert.strictEqual((await list("wrong")).status, 401, "browsing needs the server password");
  assert.strictEqual((await list()).text, "flim-rooms 1\n", "no rooms yet");

  const a = await join("beats", "secret", "A");
  assert.strictEqual(a.msg.role, "leader", "the creator leads");
  const b = await join("open room!", undefined, "B");
  assert.strictEqual(b.msg.session, "OPENROOM", "names are cleaned up");
  assert.strictEqual((await list()).text, "flim-rooms 1\nBEATS\t1\t8\t1\nOPENROOM\t1\t8\t0\n");

  assert.strictEqual((await join("BEATS", "nope", "C")).msg.code, "bad_room_password");
  assert.strictEqual((await join("BEATS", undefined, "C")).msg.code, "bad_room_password");
  const c = await join("BEATS", "secret", "C");
  assert.strictEqual(c.msg.role, "member", "the right room password gets in");
  assert.ok((await list()).text.includes("BEATS\t2\t8\t1"));

  // The session-code way in needs the room password too.
  const viaCode = await new Promise((resolve) => {
    const ws = new WebSocket(`ws://localhost:${PORT}`);
    ws.on("open", () => ws.send(JSON.stringify({ t: "join", session: "BEATS", password: "sërver", user: { id: "D", name: "D" } })));
    ws.on("message", (m) => { const msg = JSON.parse(m.toString()); if (msg.t === "welcome" || msg.t === "error") resolve(msg); });
  });
  assert.strictEqual(viaCode.code, "bad_room_password");

  const d = await join("OPENROOM", "ignored", "D");
  assert.strictEqual(d.msg.role, "member", "a room without a password lets anyone in");

  // A room goes when its last person leaves, and its password with it.
  a.ws.close(); c.ws.close();
  await sleep(300);
  assert.ok(!(await list()).text.includes("BEATS"), "an empty room is gone");
  const e = await join("BEATS", undefined, "E");
  assert.strictEqual(e.msg.role, "leader", "a new room of that name is a new room");

  console.log("rooms_test ok");
})().then(() => { srv.kill(); process.exit(0); }, (e) => { console.error(e); srv.kill(); process.exit(1); });

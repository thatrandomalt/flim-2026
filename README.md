# flim

**Real-time multiplayer for FL Studio 2026.** Two or more people each run their own FL Studio, with their
own windows, zoom and selection, while the song itself (knobs, plugins, channels, notes, playlist, mixer,
samples) is kept in sync over the internet. It works a bit like [Drawpile](https://drawpile.net/), but for FL.

> 🚧 **Still missing features.** A few things don't sync yet, such as samples inside Kontakt and
> other people's selection. See [What's synced](#whats-synced) for the full list.

> ⚠️ **Unofficial and experimental.** flim is not made by, affiliated with or endorsed by Image-Line.
> It works by running extra code inside FL Studio's process, and it only supports **one exact FL build**
> (see [Requirements](#requirements)). It can crash FL, so save often. flim never overwrites your
> original `.flp` files (see [Sessions](#sessions)).

---

## How it works

```
 you                                  server                         friend
┌────────────────────────┐        ┌──────────────┐        ┌────────────────────────┐
│ flim.exe starts FL and │   ws   │ flim-server  │   ws   │ flim.exe starts FL and │
│ loads flim_engine.dll  │◄──────►│ orders edits │◄──────►│ loads flim_engine.dll  │
│ into it                │        │ hands out    │        │ into it                │
│  - notices your edits  │        │ edit locks   │        │  - applies your edits  │
│  - applies theirs      │        │ (Node.js)    │        │  - sends theirs        │
└────────────────────────┘        └──────────────┘        └────────────────────────┘
```

- **`flim.exe`** (the launcher) starts your normal FL Studio and loads the flim engine into it. Nothing in
  your FL Studio installation is modified on disk.
- **`flim_engine.dll`** (the engine) runs inside FL. It watches for local edits, sends them to the server,
  and applies everyone else's edits through FL's own internal functions, so FL's UI stays consistent.
  It draws nothing into FL itself (except other people's mouse arrows): the connection status, who is
  where, sample transfers and a log are in flim's own window, which also connects and disconnects.
- **`flim-server`** is a small WebSocket relay. It puts edits in one order for everyone and gives out locks,
  so two people can't edit the same pattern or knob at the same moment. One person runs it and everyone
  connects to it: it's built into `flim.exe` (**Host a session**), or runs on its own with Node.js on an
  always-on machine.

### What's synced
| Synced | Not yet |
|---|---|
| Channel volume / pan / on-off / mixer routing, filter, pitch, portamento, gate, swing, time offset, delay and arp settings | Samples inside plugins that store them compressed or encrypted (Kontakt, ...) |
| Every plugin parameter (generators and effects, FL's own and third-party VSTs) | Showing other people's selection inside an editor |
| Full plugin state (presets, wavetables, samples loaded inside a VST, ...), checked as soon as you stop working in the plugin's window | Placeholders for MP3 / FLAC / OGG samples (those wait for their download, see [Samples](#samples)) |
| Tempo, swing and main pitch (the main volume is each person's own) | |
| Mixer volume / pan / stereo separation / mute / built-in EQ; each effect slot's on/off and mix level | |
| Mixer track names, colors and routing (sends on/off and send levels) | |
| Inserting, deleting, trimming and moving mixer tracks (routing and automation follow the track); moving an effect up or down in a track (its effects reload on the other side) | |
| Adding (anywhere in the list, cloning too), deleting and reordering channels; channel names and colors | |
| Adding, replacing and removing mixer effects | |
| Notes in every pattern; pattern names, colors and length (so new patterns too) | |
| Inserting, cloning, deleting and splitting patterns (later patterns move along) | |
| Playlist clips; playlist track names and colors | |
| Playlist markers (including time signature markers); the project's time signature | |
| Time signatures of single patterns (pattern menu) and piano roll markers | |
| Automation clips, with their links to what they automate (made, cloned, added or removed in the session too), and the links of internal controllers (Peak, Formula...) | |
| Sample files used by channels, audio clips and plugins like FPC, and other files plugins load, such as SF2 Player's soundfont (see [Samples](#samples)) | |
| Opening or loading a project (the leader's project is sent to everyone) | |
| Which window everyone is working in, and where their mouse is in the piano roll and playlist (see [Who's where](#whos-where)) | |

**Working at the same time:** everyone can edit at once. Notes and playlist clips sync about 0.25 s
after you stop moving them, one channel / one playlist track at a time, so two people can write in the
same pattern (say, drums and bass) or arrange on different playlist tracks simultaneously without
getting in each other's way.

**Locks:** while you tweak a mixer knob, change a plugin, edit one channel's notes in a pattern or edit
a playlist track, that part is yours. If someone else touches the same part at the same time, their
change is undone and FL's hint bar shows *"Channel 'Kick' in pattern 3 is being edited by Alex"*,
*"Playlist track 4 is being edited by Alex"* or *"Channel 'Serum 2' is being edited by Alex"*. A plugin
is locked as a whole: any knob or preset change on it takes the lock. Locks are released shortly after
you stop: about 0.6 s for a channel or mixer knob, 2 s for notes or a playlist track, and for a plugin
**10 s** by default. Each person sets their own plugin time (1–10 s, *Keep a plugin I change locked to
me for … s* in flim's window): it is how long a plugin you're working on stays yours after your last change.
Names, colors, routing, markers and the time signature have no locks; if two people change the same
one at the same moment, the leader's change wins. If the channel lists ever end up different anyway (say,
two people add or delete channels at the very same moment), flim notices and reloads the leader's
project for everyone instead of letting edits land on the wrong channels. As a safety net, whenever
nobody has changed anything for a few seconds, each FL compares a fingerprint of the song (names,
routing, notes, clips, automation links, mixer levels) with the leader's. One that differs twice in a
row reloads the leader's project, at most every 2 minutes.

**What isn't an edit:** values that move by themselves (automation playing back, a Patcher preset's
own modulation, controllers linked inside FL) happen the same way on every PC, so flim only syncs a
change when someone is using FL (a click or key press in the last 1.5 s). A knob an automation clip
or controller drives is never synced: each FL plays its own automation, so one person playing the
song doesn't move everyone's knob. Changes made with a MIDI controller alone aren't synced for now.

**Solo is yours:** soloing a mixer track only changes what you hear. The others keep their mutes, and
when you unsolo, your tracks get back the mutes everyone shares, including any someone changed while
you were soloing. A song saved while soloed opens soloed only for whoever opens it; the others get it
with every track on, which is what the solo leaves when it ends. The main volume knob is yours too.

**Staying out of your way:** someone else's plugin changes load without opening the plugin's window,
and they wait while you're in the middle of something (holding a mouse button, a menu or dialog of a
plugin open) or while FL itself is loading. Other people's pattern operations (Clone, Insert...) don't
change which pattern you're on, and nothing they do changes which channels you have selected.

### Who's where
The status in flim's window shows the window each other person last clicked or typed in,
e.g. `Connected to flim.example.com · leader · 2 people · Alex: Piano roll - Kick · Pattern 3`.

When someone's mouse is over the piano roll or the playlist, you see a small arrow with their name at
the same spot in yours (the same note and bar, whatever your zoom or scroll), as long as you are looking
at the same pattern or arrangement. The arrows can't be clicked and never get in your way. To hide them,
untick **Show cursors** in flim's window.

---

## Requirements

- **Client:** Windows 10 or 11 (64-bit) and **FL Studio 2026, build 26.1.7.5654 exactly** (Help → About).
  The engine calls into FL at fixed internal addresses, and those change with every FL build. With any
  other build, flim warns you before starting and FL will most likely crash.
- **Server:** any machine with Node.js 20 or newer (Windows, Linux or macOS) that everyone can reach.

---

## Quick start

### Client
1. Download `flim-client-win64.zip` from [Releases](../../releases) and unzip it anywhere.
   Keep `flim.exe` and `flim_engine.dll` in the same folder.
2. Double-click **`flim.exe`**. The first time, it asks for:
   - your **FL Studio 2026 folder** (the one that contains `FL64.exe`, usually
     `C:\Program Files\Image-Line\FL Studio 2026`). It is checked right away.
   - a **name** that the others will see (letters, digits, spaces, `- _ .`, up to 24 characters).
3. flim's window opens. (It can be resized: made smaller than its layout, e.g. by a tiling window manager
   under Wine, it gets scrollbars, and the mouse wheel scrolls it.) Either:
   - **Join a session:** enter the address and password you got from whoever hosts and click **Join**.
     FL opens and joins by itself.
   - **Host a session on this PC:** pick a port (8080 is fine) and a password and click **Start hosting**.
     Send the others the address under *Address to give the others* (**Copy**) and the password. With
     *Also open FL on this PC and join* ticked, FL opens here too. Keep flim's window open while you
     host: closing it ends the session. See [Hosting](#hosting) for people outside your network.
4. flim's window stays open next to FL. Its **Session** panel shows the status, green once connected
   (`Connected to 203.0.113.7:8080 · member · 2 people · Alex: Mixer`), and a log of what happened:
   people joining, the project arriving and being zipped, samples uploading and downloading. You can
   select and copy from the log. **Detailed log** adds every change sent and received, and every 30 s a
   summary of the song (two people's summaries differ where their songs do). **Export log...** saves
   what a bug report needs as one zip. **Resync from host...** (after a warning) loads the session
   leader's project again if your song seems out of step; changes of yours that haven't reached the
   others yet may be lost. Clicked by the leader, it sends everyone the leader's project.
5. Make music together. While FL runs, the **Join** button is **Disconnect** / **Connect**. You can close
   flim's window (FL keeps going) and open it again later: it finds your FL. *Open FL without
   connecting* starts FL offline. With **Attach to FL automatically** unticked (top of the window),
   flim doesn't take a running FL by itself: **Join** lists the FL Studio processes that are running
   (and whether flim is in them already) plus *Open a new FL Studio*, and uses the one you pick.
6. When you're done using flim, restart FL Studio before working on your own: flim stays loaded in it
   until then, and some strange behavior can be left over. flim reminds you when you close its window
   while FL is still open.

### Hosting
The host is built into `flim.exe`, nothing else to install. What to give the others:
- **Same network:** the address flim shows, e.g. `ws://192.168.1.20:8080`.
- **Over the internet:** tick *Open the port on my router automatically (UPnP)* (works if your router
  allows UPnP), or forward the TCP port to this PC in your router, and give them
  `ws://<your-public-IP>:8080`. If the address flim shows is already public (not `192.168.…`, `10.…`
  or `172.16–31.…`), nothing needs forwarding.
- **Tailscale (or another VPN) on both machines:** give them `ws://<this PC's Tailscale IP>:8080`, e.g.
  `ws://100.64.1.2:8080`. Nothing to forward, and it works from anywhere.
- The first time you host, Windows Firewall asks whether flim may use the network: allow it, on public
  networks too if your connection is marked public.
- The hosting PC's window can manage the people in the session (see [Host controls](#host-controls)).
  If this PC only hosts (*Also open FL on this PC and join* unticked), tick *Let the session leader use
  the people controls too* to let the leader do it from their own flim window as well.

### Dedicated server
For a server that runs without anyone's FL (an always-on PC or a cloud machine), use the Node.js
version. Only one person needs to run this.
```bash
git clone https://github.com/fckwader/flim.git
cd flim/server
npm install
npm run build
FLIM_PASSWORD=pick-a-password npm start      # PowerShell: $env:FLIM_PASSWORD="pick-a-password"; npm start
```
The server listens on port **8080**. Give your friends your address and the password:
- **Same network:** `ws://<your-LAN-IP>:8080`, e.g. `ws://192.168.1.20:8080`.
- **Over the internet:** forward TCP port 8080 on your router to this PC and give them
  `ws://<your-public-IP>:8080`. Or put the server behind a reverse proxy with HTTPS and use `wss://`
  (see [Server details](#server-details)).
- **Just testing on one PC:** `localhost:8080`.

---

## Using flim

### Sessions
- A server holds **one session**. The **first person to connect is the leader**: their FL holds the
  project that people who join get. (The leader doesn't have to be the PC that hosts the server.)
- Each song has **its own folder** in your projects folder (`Documents\flim\Projects\` unless you pick
  another with **Change...** in flim's window; **Projects folder** opens it): `Projects\MySong\` holds
  the session copies (`MySong_flim_1012_2130.flp`), the samples you received for it, a zip of each copy
  and `Backups\`.
- Anyone who joins later is a **member** and gets the leader's current project with every sample it uses,
  stored in that song's folder; FL opens it by itself. Once the samples have arrived, it's also zipped
  to `MySong_flim_1012_2130.zip` next to it: a portable copy of the project as it was when you joined.
  Samples added later in the session go into the same folder, and **Ctrl+S** saves your copy there.
- **Song name:** if you lead with a new, untitled project, type a name in *Song name* in flim's window and
  flim saves the project as `Projects\<name>\<name>.flp` (FL's title and Ctrl+S use it), so the folder
  and everyone's copies get that name instead of "untitled".
- When the leader opens another project, it is sent to everyone. Your original `.flp` files are never
  saved over: flim always works on `*_flim_MMDD_hhmm.flp` copies. A **zipped project** (FL unpacks it to
  a temporary folder it deletes later) is moved into its song folder first, samples included, and
  reopened from there, so nothing goes missing after a restart.
- **If the leader leaves, the session goes on:** whoever has been in it longest becomes the leader, and
  everyone gets a popup saying who leads now. If the PC hosting the server stops (or the connection
  drops), a popup tells you you're no longer in the session; your FL keeps the project as it is.
- **While the leader saves** (Ctrl+S, Save as, autosave), nobody else can change the project: clicks and
  key presses wait, and a small *"Alex is saving the project"* popup shows until the save is done.
- **While your FL loads the leader's project** (when you join, or when two people changed the same
  channels or tracks in the same instant and your FL catches up), clicks and key presses wait the same
  way for the second or two it takes, so nothing you do then gets lost. FL's own dialogs still work.
- **If the leader's FL stops responding** for 30 s (some plugins take a while to load, so not sooner),
  everyone else gets a popup suggesting they save their own copy.

### Host controls
*People in the session* in flim's window lists everyone (the leader, who is sync-only, who was removed).
The PC hosting with flim's built-in server can use its buttons on anyone. The session leader can use them
too when the host allows it (*Let the session leader use the people controls too*, only when the hosting
PC doesn't join itself), and always on a [dedicated server](#dedicated-server), which has no host window.
- **Make leader...** (asks first): that person's FL holds the project from now on, and people who join get
  it from them. Nobody reloads: everyone already has the same project. Both get a popup.
- **Sync-only** / **Back to normal**: a sync-only person still gets everything the others do (and can load
  the leader's project with *Resync from host*), but nothing they do in FL reaches anyone: the server drops
  it, and their flim doesn't send it either. Their status says *sync-only*. Changes they make meanwhile
  stay in their FL; switched back, their FL loads the leader's project once so it matches again. It sticks
  if they reconnect. The leader can't be sync-only (make someone else the leader first).
- **Remove...** (asks first): their FL is disconnected with a popup saying who removed them, and they
  can't join this session again (same id or same name) until **Let back in**, or until the session ends.
- **Ban IP...** (asks first): like Remove, and nobody from their internet address (or with their name) can
  join any session on that server until **Let back in**, or until the server stops. People sharing that
  connection are shut out too. The address itself is never shown to the leader.

### Samples
The samples your channels, audio clips and sample-based plugins use are sent along automatically,
both when the leader's project is sent and when someone loads a new sample during the session.
- Each file is uploaded to the server **once**; the others download only the files they don't already
  have, and their copy of the project points at the result. Received samples go into the song's folder
  (one you already have is hard-linked there, or copied from another drive), the leader's too.
- Nobody waits for a transfer: the change that uses a sample (a new audio clip, a sample dropped on a
  channel, the leader's project) arrives right away, and the upload and download happen in the
  background. Until a WAV or AIFF sample has arrived, the others get a **silent placeholder** with the
  same name and about the same length, so the audio clip or channel is already there and the arrangement
  keeps its shape; when the download finishes, the real sound replaces it by itself. flim's window
  shows *"Uploading samples: 12.3 / 40.0 MB"* / *"Downloading samples: … (placeholders until done)"*.
- MP3, FLAC, OGG and other formats can't have a placeholder: a change that uses one waits until that
  file has downloaded (your other edits still go out right away).
- **If more than 50 MB would have to be uploaded at once, flim asks first** (before anything is sent) and shows the exact amount,
  e.g. *"Sending them uploads 60.6 MB (63,504,132 bytes). Send them now?"*. **No** leaves those samples
  out (they will be missing for the others) and flim won't ask about them again until FL restarts.
- Samples in FL's factory content are never sent: every FL installation has them.
- Files inside plugin states are found too: in FPC (and other newer Image-Line plugins that store
  them the same way), and as file paths in any other plugin's state (SF2 Player's soundfont, Slicer,
  Serum's own samples...), whatever the file type. Programs and plugin files (`.dll`, `.vst3`, ...) are
  never sent. For the latter, the receiving copy points at a link to the downloaded file whose path has
  exactly the length of the original (so the plugin's data stays valid): in a subfolder of the song
  folder when that fits, else under the shorter `Documents\flim\samples\`. If no such path fits and the
  plugin stores the path's length (SF2 Player), it points at the downloaded file itself. When the real file arrives, the plugin is loaded
  again with the state it was sent (not the one it made of the silent placeholder, so Slicer's slices
  stay right). Plugins that compress or encrypt their state (Serum 2, Kontakt, ...) don't show their
  files to flim: load those plugins' samples from the shared folder (below).

### The shared folder
**Shared folder** in flim's window opens `C:\Users\Public\Documents\flim\Shared`: the same path on every
PC. Whatever is in it, everyone in the session gets, at that same path. So a sample any plugin loads
from there (Serum 2, Kontakt, a sampler, a wavetable, an impulse response) is found on everyone's PC,
because the plugin's own saved path is right everywhere; flim doesn't need to understand the plugin.
- Put files in (copy, drag, unzip, subfolders are fine), then load them in the plugin from there. Tip:
  add the folder to FL's browser (Options > File settings > Browser extra search folders).
- Changes reach the others within a few seconds: a new file, or a new version of one (yours replaces
  theirs, whatever the file's date says). If two people change the same file at once, one version wins
  for everyone, and a version that only you had is kept in `C:\Users\Public\Documents\flim\Shared
  backups\` (30 days, then the Recycle Bin).
- Deleting a file only deletes your copy, and it stays deleted for you (a newer version from someone
  brings it back). Renaming one sends the new name; the old one stays with the others.
- Files that are still being copied or downloaded wait until they're finished. Programs, scripts,
  shortcuts and plugin files are never shared; files over 2000 MB and links (junctions) in the folder
  aren't either (flim's window says so). flim downloads only while there's at least 2 GB free.
- A plugin loaded before its file arrived: plugins that store plain paths (FL's samplers, SF2 Player,
  many VSTs) are loaded again by themselves; Serum 2, Kontakt and others need the preset reloaded or the
  sample picked again.

### Backups and crash reports
- **Backups:** *Back up the project every [10] min* (on by default) saves a zip of the project and every
  sample it uses into the song folder's `Backups\` whenever it changed; the newest 20 per song are kept.
- **Crash reports:** if FL crashes, flim's window logs it and saves
  `Documents\flim\Crash reports\FL crash <date time>.zip`: FL's crash log and startup log from that run,
  flim's log for that FL, and a memory dump when FL jumped into a plugin that was already unloaded.
- **Hang reports:** if FL doesn't respond for 8 s, flim saves `FL hang <date time>.zip` there: a small
  dump with what every thread was doing, which is what's needed to find a freeze. FL may still recover.
  If FL is then closed while not responding, `FL not responding <date time>.zip` has the dump too, and
  flim's log names the step FL was stuck in and where (the `STUCK` lines).
- **Crash reports** in flim's window opens the folder. Send the zip to whoever helps you with flim.

### Server addresses
| You type | Means |
|---|---|
| `localhost:8080` | a server on your own PC (plain `ws://`) |
| `ws://192.168.1.20:8080` | a server elsewhere on your LAN |
| `ws://203.0.113.7:8080` | a server over the internet, unencrypted |
| `wss://flim.example.com` | a server behind HTTPS (encrypted, port 443) |
| `flim.example.com` | same thing: a bare host name means `wss://`, port 443 |

### Changing your settings
Change your name in flim's window (it's used from the next connect), and your FL folder with
**FL folder...** there (or run `flim.exe --setup`). The song name, plugin lock time, backups and the
projects folder are set there too and apply right away.

### Where flim keeps things
| What | Where |
|---|---|
| Settings (FL folder, name, last address, host port, passwords if "Remember" is ticked or you host) | `%APPDATA%\flim\flim.ini`. Passwords are encrypted for your Windows user (DPAPI). |
| Log | `%TEMP%\flim_engine.log` |
| Exported logs (**Export log...**: the log for this FL, flim's window and host logs, versions and settings, no passwords) | `Documents\flim\Logs\` |
| Songs: session copies, received samples, zips, backups | `Documents\flim\Projects\<song>\` (or the projects folder you picked) |
| Same-length links for plugins that store sample paths, when the song folder's path is too long (safe to delete when no session is running) | `Documents\flim\samples\` |
| Crash and hang reports | `Documents\flim\Crash reports\` |
| The shared folder, its backups, and what flim knows about its files (versions, deletions) | `C:\Users\Public\Documents\flim\Shared\`, `...\flim\Shared backups\`, `%APPDATA%\flim\shared-*.tsv` |
| The built-in host's sample store (a cache, safe to delete when not hosting) | `%LOCALAPPDATA%\flim\host-samples\` |
| flim's scratch files (plugin states on their way) | `%TEMP%\flim\` |

**Clear cache...** (bottom of flim's window) deletes the three caches above (`samples`, `host-samples`,
`%TEMP%\flim`) after showing their size and asking. It won't run while FL Studio is open or you're
hosting, and never touches songs, backups, the shared folder or settings.

---

## Server details

See [`server/README.md`](server/README.md) for the protocol and options.

- **Password:** set `FLIM_PASSWORD`. If it isn't set, the server lets in anyone who knows its address
  (fine for testing on your own PC, not for a public server). A wrong password is rejected right away.
- **Port:** 8080 by default; set `PORT` to change it.
- **Limits** (both servers, for one open to the internet):
  - **People per session:** 8 by default. Set `FLIM_MAX_PEOPLE` on the Node server; on the built-in host
    it's **Max people** in the Host section of flim's window (2–64, applies at once, also while hosting).
    Someone already in the session can always reconnect.
  - **Connections per address:** at most 12 open at once (`FLIM_MAX_CONNS_PER_IP`).
  - **Wrong passwords:** after 8 from one address within 10 minutes, that address is refused until the
    10 minutes are over, even with the right password (`FLIM_FAIL_MAX`). Wrong sample-store keys count
    too.
  - **Message rate:** a connection sending more than 400 messages a second for long (bursts up to 4000)
    is disconnected (`FLIM_RATE_PER_S`, `FLIM_RATE_BURST`). flim's own traffic stays far below that.
  - **Joining:** a connection must join a session within 20 seconds.
  - Whoever hits a limit gets a message saying which one in flim's window.
- **Encryption:** the server speaks plain `ws://`. For `wss://`, put it behind any reverse proxy that
  handles HTTPS and WebSockets (Caddy, nginx, a cloud load balancer, ...) and forward to port 8080.
  The proxy must pass plain HTTP requests too (sample files use `/blob/...` on the same port) and allow
  large request bodies.
- **Sample store:** sample files are kept on the server's disk under their SHA-256, in `FLIM_BLOB_DIR`
  (default: the system temp folder). `FLIM_BLOB_STORE_MAX` caps the total (default 4 GB; the least
  recently used files go first) and `FLIM_BLOB_FILE_MAX` a single file (default 2000 MB). It's only a
  cache: anything missing is uploaded again by the next session that needs it.
- **Docker:** a `Dockerfile` is included:
  ```bash
  docker build -t flim-server server
  docker run -p 8080:8080 -e FLIM_PASSWORD=pick-a-password flim-server
  ```
- **Health check:** `GET /health` answers `flim-server ok`.
- Sessions live in memory only. Restarting the server ends the session (and lifts IP bans); run a single
  instance.
- **Host controls:** the session leader can use them (there is no host window). On fly.io, IP bans use the
  `Fly-Client-IP` header; elsewhere the address the connection comes from (behind another proxy, that is
  the proxy's).

---

## Building the client from source

Requires **Visual Studio 2022 Build Tools** (the "Desktop development with C++" workload).
```bat
cd client
build.bat
```
`build.bat` writes `flim.exe` and `flim_engine.dll` to `client\out\`. If your Build Tools are not in the
default location, change `VCVARS` at the top of `build.bat`. Both files are plain C, linked statically,
and use only DLLs that ship with Windows.

### Developer shortcuts
These environment variables are read by the engine (for tests and two FLs on one PC):

| Variable | Effect |
|---|---|
| `FLIM_AUTOCONNECT=<url>` | connect at startup (with `FLIM_PASSWORD=<pw>`) |
| `FLIM_NAME=<name>` | override the name from `flim.ini` (e.g. to run two FLs on one PC) |
| `FLIM_ROOM=<room>` | use another session name on the same server (default `FLIM`) |
| `FLIM_SAMPLE_NOLOCAL=1` | never use a local copy of a received sample, always download it (to test sample transfer on one PC) |

With `FLIM_AUTOCONNECT` set, `flim.exe` skips its window and starts FL connected right away.
`client\restart_test.ps1 [-Url localhost:8080] [-Password ...]` uses that: it closes all FL instances,
rebuilds, and starts two of them ("A" as leader, "B" as member) connected to a server, which is handy
for testing sync on one machine.

---

## Repository layout
```
client/                 the FL side (C, Win32)
  launcher/flim.c       flim.exe: setup and join/host windows, starts FL, loads the engine into it, shows
                        FL's status and log
  launcher/host.c       the built-in server behind "Host a session" (relay, locks, sample store, UPnP)
  engine/flim_engine.c  the engine: edit detection, applying edits, locks, pattern ops, sample transfer,
                        networking, status / log / Connect for flim.exe's window
  common/               settings (flim.ini) and resource IDs shared by both
  build.bat             build script (MSVC)
  restart_test.ps1      dev loop: rebuild and start two connected FLs
  dev/                  reverse-engineering and test tools (not built or shipped)
server/                 flim-server (Node.js + TypeScript, WebSocket + HTTP)
  src/                  server.ts (connections, password), session.ts (ordering, locks), protocol.ts,
                        blobs.ts (sample file store)
  Dockerfile            container image
```

## Troubleshooting
- **"This FL Studio is version X, but flim was built against 26.1.7.5654"**: your FL build is different.
  See [Requirements](#requirements).
- **"Can't connect: server not found"** or **"nothing answers at that address"**: check the address, that
  the server is running, and (over the internet) that the port is forwarded. If you host, check that
  Windows Firewall lets flim in (see [Hosting](#hosting)).
- **"Can't connect: secure connection failed"**: you typed a bare host name or `wss://`, but the server
  speaks plain WebSocket. Use `ws://host:8080`.
- **"Can't connect: not a flim server (HTTP …)"**: something answered at that address, but it isn't
  flim-server.
- **"Wrong password"**: ask whoever runs the server. The password is the server's `FLIM_PASSWORD`.
- **"Alex is on flim X (built …) and you're on Y (built …)"**: everyone in a session needs the same flim
  (the same download); another build may miss or misread some changes. Everyone updates to the newest.
  (Builds from before this check don't say which they are, so it only shows between newer ones.)
- **"The plugin X is missing"**: someone uses a plugin (VST) that isn't installed on your PC (or isn't
  in FL's plugin database: rescan in FL's plugin manager). Click OK. That channel or effect slot keeps
  a placeholder that holds the plugin and its settings, so nothing is lost: it plays for everyone who
  has the plugin and comes back if you install it. It's silent for you. FL warns once per plugin;
  after that, changes to it still arrive (the placeholder keeps the latest settings) and are only
  noted in flim's log.
- **Something didn't sync**: tick **Detailed log** in flim's window (everyone in the session) and carry
  on. When it happens again, note the time, click **Export log...** and attach the zip to an issue,
  one from each person if you can.
- **FL crashed**: attach the crash report zip (see [Backups and crash reports](#backups-and-crash-reports)).

---

*FL Studio is a trademark of Image-Line. This project contains no FL Studio code or files. It only talks
to an FL Studio installation that you already have.*

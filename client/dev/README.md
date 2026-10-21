# client/dev: reverse-engineering and test tools

Not part of the flim build (`build.bat` ignores this folder) and never shipped. These are what the
engine's `*_VA` addresses were found and checked with, for FL Studio 26.1.7.5654 (earlier: 25.2.3.5171).

Python 3 with `python -m pip install --user capstone pefile numpy`.

| File | What |
|---|---|
| `flre.py` | static analysis of `FLEngine_x64.dll` (the FL 2026 install by default, `FLRE_DLL=<path>` for another build): disassembly (`dis`, `near`), resolving the protector's call stubs (`resolve`), callers of a function (`callers`, `show_callers`), rip-relative refs to a global (`xrefs`, `show_xrefs`), Delphi published method names -> addresses (`meth('EEPatInsMenuClick')`, `find_name`), strings (`strs`). Run as `python flre.py "dis(0x11180c0)"`. |
| `flsigs.py` | rewrites `../engine/fl_addrs.h`: a byte signature for each FL address the engine uses, made from that table's VAs, so the engine finds them in other FL builds (`FL2026-internals.md` section 8). New FL build: `python flsigs.py --rebase` (moves the rows that still match, lists the rest), fix those by hand, then `python flsigs.py`. `--json` prints a DLL's addresses (`FLRE_DLL=<path>`). |
| `flrun.py` | starts an FL from any folder with the engine in it, as flim.exe does, without flim.exe / flim.ini: `python flrun.py "<FL folder>" B ws://localhost:8080` (e.g. a second, older FL build next to the installed one). |
| `slowlink.py` | a slow internet connection on one PC: a TCP relay that limits bandwidth and adds latency per connection (`--kbit 1000 --ms 60`, listens on 8081, forwards to 8080). `restart_test.ps1 -Url localhost:8081` puts both FLs behind it. |
| `probe/probe.c` | **dev-only** DLL injected into a running FL. Answers commands on `\\.\pipe\flimprobe_<pid>`: read/write memory, read a Delphi string, make a Delphi string, call a function on FL's main thread. Any local program can use that pipe while FL runs, so only inject it into an FL you're testing with. Build: `probe\build.bat`. |
| `fl.py` | client for the probe (`inject(pid)`, `Probe(pid)`: `r`, `q`, `d`, `g` (deref a global VA), `call(va, ...)`, `mkstr`, `ustr`), plus window helpers that never move the real mouse or keyboard (`windows`, `close_dialogs`, `confirm`, `poke`) and screenshots (`shot_fl`). |
| `FL2026-internals.md` | **start here for FL internals**: the address map with how each address was found, the FLP event format (FL 2026's escaped ids), mixer track records and FL's insert / delete / move / permute functions (decompiled), plugin wrapper state, thread rules. |
| `mk.py` | FL 2026 helpers on top of `fl.py` for the two-FL test session (`A`, `B` = pids of "A" and "B" from the engine log): layout constants, REC get/set, mixer (`mix_ids`, `mix_insert`, `mix_delete`, `mix_move`), channels, patterns, notes, clips, markers (`marks`, `pmarks`), pattern ops (`patop`). Each probe knows its FL build's addresses (`V(p, 'MIXARR')`, from the signatures), so B may run another build. |
| `live_test.py` | live regression suite on the two FLs: makes a change in one through FL's own functions and checks the other (39 checks: params, mixer, notes, patterns, markers, channels, clips, VST state, mixer insert / delete / move). Run after every port. |
| `flp.py`, `tlv.py`, `paths.py` | parse FLP / .fst events (including FL 2026's escaped ids, see FL2026-internals.md), Image-Line's tag/length/value plugin data, and list sample paths inside plugin states. |
| `survey.py` | which factory presets store sample paths in their plugin data, and in what layout. |
| `harness/t_samples.c` | offline test of the plugin-data sample path code (includes the engine source). `harness\build.bat`, then `t_samples.exe` -> `OK`. |
| `harness/t_groups.c` | offline test of per-channel / per-track sync (diff, merge, lock commit with two people in one pattern), sample placeholders, FL 2026's event escape, a joined project's song folder + zip, the mixer track insert / delete diff, the FL address signatures (on the installed DLL), big messages, state hashes across FL builds and finding moved samples. `t_groups.exe` -> `OK`. |

## Typical session
1. Start a server (`flim.exe` -> Start hosting, or `cd server && npm start`), then
   `client\restart_test.ps1 -Minimized` (add `-NoLocalB` to make B download samples even though both
   FLs are on one PC).
2. `python live_test.py` (or a few: `python live_test.py mix_insert mix_delete`).
3. By hand: `python -i -c "from mk import *; a, b = probe(A), probe(B)"`, change something in A
   through FL's own function (e.g. `mix_insert(a, 3, 2)`), then check B and `%TEMP%\flim_engine.log`.

The full notes on FL's internals (addresses, decompiled functions, data layouts) are in
[FL2026-internals.md](FL2026-internals.md). Shorter notes on things that took a while to find:
- Calls in `FLEngine_x64.dll` go through stubs (`E9 rel32` + junk byte) outside `.text`; `resolve()`
  follows them. The real function table is the PE exception directory, not the `.pdata` section.
- Menu handlers: the binary's DFM text maps a caption to its `OnClick` name, and the Delphi method
  table maps the name to the address (`meth()`).
- FL's message box is its own form (`TMsgForm`, `0x9635b0`); dialogs inside a call block the probe's
  main-thread call until they are closed (`fl.confirm` / `fl.close_dialogs`).
- Never destroy a plugin from inside a `WH_GETMESSAGE` hook: with a GPU-drawn plugin (OTT, ...) FL later
  dies in unloaded `dxgi.dll`. The engine and the probe both run main-thread work from a message-only
  window instead.
- Editor grids are painted by the form, not child windows: piano roll grid = `TEventPanel_PR` at form+0xc68,
  playlist = `TEventPanel_AU` at form+0xd2c; bounds at +0x90, ticks/pixel = form+0xb04 (double), scroll
  form+0xb14 / grid+0x3fc (FL 2026; tick = (scroll + x) * ticks/pixel). The engine's "where everyone is pointing" section has the full mapping.
  `FLIM_CURSOR_NOCOVER=1` shows other people's arrows even when FL is behind other windows (tests).
- Screenshots: capture FL's window with `PrintWindow`, never the screen (other apps may be on top).

Porting to a new FL build (how the 26.1.7 addresses were found):
- FL's Python scripting API is a set of `PyMethodDef` tables in `.data` (name, handler, flags, doc). The
  handlers marshal to FL's main thread through a callback that calls the real setter, so e.g.
  `mixer.setTrackName` leads to SetMixerTrackName, `ui.setHintMsg` to ShowHint and `general.processRECEvent`
  to ProcessRECEvent, and their bodies show the globals and struct offsets (mixer array and stride,
  channel lists, pattern table...). Hex-Rays (IDA) on those callbacks does most of the work.
- Published Delphi handlers (`flre.find_name`) cover the rest: `EEPat*MenuClick` (pattern ops = project
  vmt slots), `FileOpenActionExecute` (message 0x566, the open handler with busy begin/end and the
  project's LoadFile slot), `SaveChanStateMenuClick` (the `.fst` writer `SavePresetFile` and its masks).
- Check everything live with the probe before trusting it: Hex-Rays can show the wrong global (the marker
  lock looked like `off_15E3D30[0]`; the instruction reads `0x15E3D40`).
- `FLIM_CRASHDUMP=1` makes the engine write `%TEMP%\flim_crash_<pid>.dmp` on the first access violation in
  unloaded code; `cdbX64 -z <dmp> -c ".ecxr; dps @rsp L90; q"` (WinDbg) shows who called it.

# flim client

`flim.exe` (launcher) + `flim_engine.dll` (engine loaded into FL Studio). See the
[main README](../README.md) for what it does and how to use it.

## Build
Requires Visual Studio 2022 Build Tools with the C++ workload.
```bat
build.bat
```
Output: `out\flim.exe`, `out\flim_engine.dll`. Keep the two files in the same folder.
If the Build Tools are installed somewhere else, change `VCVARS` at the top of `build.bat`.

## Files
| File | What |
|---|---|
| `launcher/flim.c`, `flim.rc`, `flim.ico` | the launcher: first-run setup window (FL folder + name, validated), main window (join a session / host one; stays open with FL's status and log, Connect / Disconnect, popups for the engine's alerts, song name, plugin lock time, backups, projects folder), crash and hang reports for its FL, starts FL suspended, loads the engine, resumes. `FLIM_AUTOCONNECT` in its environment skips the window (dev loop) |
| `launcher/host.c`, `host.h` | the built-in server behind "Host a session": the same protocol as `server/` (WebSocket relay, locks, sample store over HTTP) in C, dual-stack IPv6/IPv4, UPnP port mapping (NATUPnP COM) |
| `engine/flim_engine.c`, `flim_engine.rc` | the engine: polls FL's state, sends and applies edits on FL's main thread, locks, sample transfer (WinHTTP, SHA-256 via BCrypt), WinHTTP WebSocket client, status / log / Connect for flim.exe's window (WM_COPYDATA, "flim window link"); draws nothing into FL but other people's mouse arrows |
| `common/flim_config.h` | `%APPDATA%\flim\flim.ini` read/write (settings listed at its top), name sanitizing, DPAPI-encrypted password, the flim window link's message kinds |
| `common/flim_zip.h` | plain .zip writer (stored files): song zips, backups, crash reports |
| `common/flim_res.h` | dialog resource IDs |
| `restart_test.ps1` | dev loop: close FL, rebuild, start leader "A" and member "B" connected to a server (`-Minimized`, `-NoLocalB`) |
| `gen_icon.py` | regenerates `launcher/flim.ico` (needs Pillow) |
| `dev/` | reverse-engineering notes and test tools: kept local, not in this repo (`.gitignore`) |

## FL build dependency
The engine calls FL's internal functions and reads its data at addresses found for
**FL Studio 26.1.7.5654** (`FLIM_FL_BUILD` in `common/flim_config.h`, addresses as `*_VA` defines
at the top of `engine/flim_engine.c`). Supporting another build means finding those addresses again.

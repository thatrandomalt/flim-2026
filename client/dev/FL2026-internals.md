# FL Studio 2026 internals used by flim

What flim relies on inside `FLEngine_x64.dll` of **FL Studio 26.1.7.5654**, found with IDA (Hex-Rays)
and checked live with the probe. Addresses are VAs with image base `0x400000` (runtime address =
module base + VA - 0x400000); `sub_XXXXXX` is IDA's name for the function at that VA. The names in
the "what" column are ours: the binary has almost none. Every FL update moves all of this; the "how
found" notes are the way back. The engine's `*_VA` defines (top of `engine/flim_engine.c`) name the
same addresses with one-line comments. The engine finds them by signature in whatever build runs
(section 8); the VAs here are that table's fallback for 26.1.7.5654.

1. [Finding things](#1-finding-things)
2. [Address map](#2-address-map)
3. [FLP / .fst event stream](#3-flp--fst-event-stream)
4. [Mixer tracks](#4-mixer-tracks)
5. [Plugins: window state, loading, missing plugins](#5-plugins-window-state-loading-missing-plugins)
6. [Threads and locks](#6-threads-and-locks)
7. [Checking things live](#7-checking-things-live)
8. [Signatures](#8-signatures)
9. [Networks and mixed FL builds](#9-networks-and-mixed-fl-builds)

## 1. Finding things

- **IDA from Python (idalib):** the Hex-Rays venv has it, system Python doesn't:
  ```python
  # %APPDATA%\Hex-Rays\IDA Pro\venv\Scripts\python.exe
  import idapro; idapro.open_database(r'...\FLEngine_x64.dll', True)   # first time: ~2 min, 59,572 functions
  import ida_hexrays, ida_funcs, idautils, idc, ida_bytes
  print(ida_hexrays.decompile(0x12a46f0))                             # pseudocode
  [hex(r) for r in idautils.CodeRefsTo(0x12a4030, 0)]                 # callers
  ```
  Work on a copy of the DLL and keep the `.i64` next to it; later opens take seconds.
- **Strings** are Delphi `UnicodeString` literals (UTF-16LE) referenced with `lea`. Find them with
  `ida_bytes.find_bytes('insert mixer track(s)'.encode('utf-16-le'), lo, hi)`, then
  `idautils.DataRefsTo(ea)`. The best anchors are undo step names (`"insert mixer track(s)"`) and dialog
  texts (`"Delete tracks|This will |delete| %d selected mixer track(s). Are you sure?"`).
- **Published Delphi handlers** (menu `OnClick` methods) map to addresses through the method table:
  `python flre.py "print(meth('DeleteMixerTrackMenuClick'))"` (prints decimal).
- **FL's Python API** (`mixer.setTrackName`, `general.processRECEvent`, ...) is a set of `PyMethodDef`
  tables in `.data`. Each handler marshals to the main thread through a callback that calls the real
  setter, so the callback's body shows the setter and the globals it touches.
- **Traps:** Hex-Rays can show the wrong global for `off_X[0]` (the marker lock looked like `0x15E3D30`;
  the instruction reads `0x15E3D40`), so check the disassembly. Data "refs" that land in `.pdata` (the
  exception directory, `(no func)`) are not real references.
- **Check live** before trusting anything: `probe/` + `mk.py` (section 7).

## 2. Address map

Offsets that changed from FL 25.2.3.5171 are marked with the old value in brackets.

### Core
| Define | VA / offset | What | How found |
|---|---|---|---|
| `REC_RVA` | `0x1046a70` | `ProcessRECEvent(id, value, flags)`: every channel / mixer / plugin / global param. Flags 2 = get, 0x11 = set | `general.processRECEvent` callback `0xd846e0` |
| `MAINFORM_VA` | `0x15e13b8` | global -> main form; `+0xf30` non-NULL once FL's UI exists (REC crashes before) | REC's first instructions read it |
| `MAINOBJ_VA` | `0x15e0ff8` | global -> var -> main form object (what FL's open/save code passes) | `ui.setHintMsg` passes it |
| `HINT_VA` | `0x11f16f0` | `ShowHint(form, text, 0)`: the hint bar | `ui.setHintMsg` |
| `UIUPDATE_VA` | `0xbd68e0` | `UIUpdate(what, 0)`; 0x18 mixer, 7 after mixer structure changes | mixer color setter |
| `MSGBOX_VA` / `MSGBOX_CORE_VA` | `0x9635b0` / `0x963520` | FL's message box `(text, flags, callback)` -> button; the engine detours it | identical prologue bytes in 25.2.3 |
| `MSGBOX2_VA` / `MSGBOX2_CORE_VA` | `0x9636a0` / `0x9635d0` | the same with extra buttons `(text, flags, buttons, x, callback)`: error dialogs, "made with a newer FL" (section 9); the engine detours it | the loader's version check `0xf4a6ce` |
| `MODIFIED_VA` | `0x11d5530` | `SetModified(1)` | |
| main form `+0x353C` | | int: while > 0, FL's progress bar (and the forced repaint it does) is skipped; see section 5 | `sub_1187300`; FL's `sub_12A4C30` brackets work with it |

### Plugins (units = a channel or a mixer effect slot)
| Define | VA / offset | What | How found |
|---|---|---|---|
| `SAVEPRESET_VA` | `0xf54570` | `SavePresetFile(file, mask, obj, flags)` -> 0: writes an `.fst`. Mask 32 channel, 48 effect slot, 64 mixer track | `SaveChanStateMenuClick` |
| `LOADPRESET_VA` | `0xf54760` | `LoadPresetFile(file, mask, obj, flags)` -> 0: replaces the plugin. `<FL>\Data\System\(delete) effect.fst` empties a slot. **flags bit 0 = don't open the plugin's editor** (masks 32/48 open it otherwise, even for a state of the same plugin; section 5) | same |
| `FLLOADS_VA` | `0x15e3908` | global -> int: `LoadPresetFile` loads running (`sub_F51FD0` `++` at `0xf520e1`, `--` at `0xf53f4b`). flim does nothing while it is > 0 | xrefs |
| `SAVEPROJ_VA` | `0x11dbe10` | `SaveProjectTo(path, flags)` -> bool: FL's project writer (Save, Save as, autosave all end here; it calls `SavePresetFile(path, 0, 0, flags)`). Prologue `55 53 48 81 EC 98 00 00 00 48 8B EC`: flim's save detour moves those 12 bytes to a trampoline | callers `sub_11DC560`, `sub_11DCB80`... |
| open handler | `sub_11D14E0` | WM `0x566` to the main form: opens the file in the global UnicodeString at `0x16bcca8` (`.flp` or `.zip`; lParam > 0 re-posts with lParam - 1), wrapped in `BUSYBEGIN`/`BUSYEND`, via the project's `LoadFile(path, 1)` | `mov edx, 566h` |
| `HASPLUGIN_VA` | `0x132f7c0` | `HasPlugin(obj)` | `plugins.isValid` |
| `GETPLUGIN_VA` | `0x1331380` | `GetPlugin(obj, &intf)`: the interface is AddRef'd, release it with `INTFCLEAR_VA` `0x41a770` (Delphi `_IntfClear`) or the plugin can never be destroyed | `plugins.getPluginName` |

### Channels
| Define | VA / offset | What | How found |
|---|---|---|---|
| `CHANLIST_VA` | `0x15e2218` | global -> channel list: items `+8`, count `+0x10` | `channels.channelCount` (global) |
| `CHANFILT_VA` | `0x15e0128` | same layout, the rack's current filter group | `channels.channelCount` (group) |
| `ADDCHAN_VA` | `0x1004230` | `AddChannel(-1, 0, 1)` -> new channel (appended) | tutorial `addChannel` |
| `DELCHAN_VA` | `0x1004a60` | `DeleteChannel(index, 0, flags)`; flags 1\|2 = no dialog | tutorial `removeChannel` |
| `RESOLVER_VA` | `0x1005770` | `ResolveChannel(index, global, &obj)` | channel wrappers |
| `MOVECHAN_VA` | `0x1042800` | `ReorderChannels(**MOVEOBJ 0x15e14a0, mode, **MOVEARG 0x15e3a60, flags)`: Alt+Up/Down and sorts; moves the selected channels | |
| channel `+0x50` / `+0x78` | | name (UnicodeString) / color; vmt `+0x70` SetName, `+0x78` SetColor | `setChannelName/Color` |
| `CH_SELECTED` | `0x32c` [`0x35c`] | selected byte | `isChannelSelected` |
| `CHSELECT_VA` | `0xff8ad0` | `SetChannelSelected(channel, on)`: the rack's selection light (and the mixer shows the selected channel's track) | `sub_11E84E0` |
| `sub_11E84E0` | | `SelectOnlyChannel(index)`: what FL's own loads and pattern jumps call | |
| `PATSELECT_VA` | `0x12d8b30` | `SelectPattern(pattern)`: the current pattern, as MIDI scripts' `patterns.jumpToPattern` (`sub_D659C0`) | Python API table at `0x1519a70` |
| `CH_GRID` | `0x7ac` [`0x7bc`] | step grid object | `setGridBit` |
| channel `+0x258` | | mixer track the channel goes to (FL renumbers it when tracks move) | `sub_12A4030` |
| channel `+0x794` | | channel index | `getChannelIndex` |

### Patterns and notes
| Define | VA / offset | What | How found |
|---|---|---|---|
| `PATARR_VA` | `0x15e2aa0` | global -> static pattern table, stride `0xC0`: name `+0`, color `+8`, notes `+0x28`, markers `+0x30`, length `+0x44` [`+0x10`] | `getPatternColor`, `setPatternLength` |
| `CURPAT_VA` | `0x15e4018` | global -> int: current pattern | `patterns.patternNumber` |
| `PATNAME_VA` / `PATCOLOR_VA` | `0x12d0fc0` / `0x12d14b0` | `SetPatternName/Color(pattern, v)` | |
| `PATLENSET_VA` | `0x12d2350` | `PatternLengthChanged(pattern)`: call after writing `+0x44` | `patterns.setPatternLength` |
| `PATREFRESH_VA` | `0xea7ae0` | after a rename, on (playlist form `+0xd34` [`0xd00`]) | |
| `NOTEARR_VA` / `NOTEDEL_VA` | `0x12d16e0` / `0x10603b0` | `NoteArray(pattern, create)`, `DeleteNote(arr, i, 0)`; note record 24 bytes, channel `+6` | |
| `NOTELOCK_VA` | `0x15e2008` | CRITICAL_SECTION* held while editing notes (the audio thread reads them) | |
| `GRIDREFRESH_VA` | `0x12d6900` | `RefreshGrid(channel->grid)` | |
| pattern ops | project vmt (`*(*FLPROJ+0x10)`) | clone `+0xe8`, delete `+0xf8`, insert `+0x118`, split `+0x120` | `EEPat*MenuClick` |

### Playlist, arrangements, markers, time signature
| Define | VA / offset | What | How found |
|---|---|---|---|
| `ARRS_VA` / `ARRCUR_VA` | `0x196eb60` / `0x15d65f0` | arrangement pointer array (count int64 at `-8`) / current index | `0x12e1a60` current arrangement |
| arrangement `+0x14` | | clip array object; clip record `0x58` [`0x50`] bytes: `+0xc` track (499 = track 1), `+0x20` id | arrangement ctor `0x12e3230` |
| arrangement `+t*0x114` | | playlist track `t` (1..500): name `+0x24`, color `+0x2c`; `+0x24+180` link type (1 = mixer-linked audio track, 3 = channel-linked instrument track), `+0x24+184` mixer track | `PLTRACK_VA` `0x12e60f0`, `sub_12A4030` |
| `PLFORM_VA` / `PLREFRESH_VA` | `0x15e35c8` / `0xe39560` | playlist form / `RefreshPlaylist(form)` | |
| `ARR_MARKERS` | `0x21c48` | arrangement: marker dynarray, record `0x34`: pos `+0`, action `+4` (8 = time signature), name `+8`, num/den `+0x10/+0x11` | `0x12d88c0` |
| `MARKADD_VA` | `0x10637c0` | `MarkerListAdd(&list, pos, name, action, num, den)` | |
| `MARKREFRESH_VA` | `0xe53810` | `MarkersChanged(form)` (`0xe3feb0` does the same plus an overlap dialog: avoid) | |
| `MARKTYPE_VA` | `0x1054280` | -> RTTI of the marker dynarray (`0x1054288`). The engine dereferences the slot, as FL's own code does (`MarkerListAdd` `0x10637e3: mov rdx, [rip -> 1054280]`): no code addresses the RTTI itself, so only the slot can be signed | `MarkerListAdd` |
| `SONGLOCK_VA` | `0x15e3d40` | CRITICAL_SECTION* (markers, time signature, mixer structure) | disassembly of `0xe543b0` |
| `TSNUM_VA` / `TSDEN_VA` | `0x15e39c0` / `0x15e1c70` | -> int: project time signature | |
| `TSNUMSET_VA` / `TSDENSET_VA` | `0x1366990` / `0x1366a10` | `SetNumerator/Denominator(main form, v)` | `general.setNumerator` |
| `PATLENUPD_VA`, `PATMARKS_VA`, `MARKSORT_VA` | `0x12d17a0`, `0x12d0b60`, `0x1063130` | pattern time signature: bar length, redraw, table build | |

### Project files
| Define | VA / offset | What | How found |
|---|---|---|---|
| `FLPROJ_VA` | `0x15e4790` | holder; `*(holder+0x10)` = project object; vmt `+0x40` `LoadFile(self, path, flags)` (flags 0: no "save changes?") | `FileOpenActionExecute` |
| `BUSYBEGIN_VA` / `BUSYEND_VA` | `0x11cf9f0` / `0x11cfa40` | FL's open handler wraps a load in these | same |
| `SAVEPROJ_VA` | `0x11dbe10` | `SaveProjectTo(path, 0)`: writes an `.flp`, current name unchanged | |
| `SETPROJ_VA` | `0x11d9380` | `SetProjectFile(form, path, path, addRecent)`: title, Ctrl+S target | |
| `PROJNAME_VA` | `0x16bcc98` | UnicodeString: current project file (an untitled project is `<projects folder>\untitled.flp`) | |
| `FORM_BUSY` | main form `+0x34d4` | non-NULL while an `.flp` load runs | |

### Editors (who points where)
| Define | VA / offset | What |
|---|---|---|
| `PRFORM_VA` | `0x15e2478` | -> var -> piano roll form (`0x15e4558` is the *current* editor form) |
| `ED_TICKSPERPX` / `ED_HSCROLL` | form `+0xb04` / `+0xb14` | double ticks per pixel / int scroll; tick = (scroll + x) * ticks/px |
| `PR_GRID` / `PL_GRID` | form `+0xc68` / `+0xd2c` | `TEventPanel_PR` / `TEventPanel_AU`, bounds at `+0x90` |

### Mixer (section 4 has the functions)
| Define | VA / offset | What | How found |
|---|---|---|---|
| `MIXARR_VA` | `0x15e06a0` | -> the track array: 502 records always allocated (a static array), stride `MIX_STRIDE` `0x1478` [`0x1474`] | mixer wrappers |
| `MIXCNT_VA` | `0x15e2198` | -> int: track count (3..502; 18 in the default template, 502 at boot) | `mixer.trackCount` |
| `MUTE_RVA` | `0x127b9b0` | `SetTrackEnabled(track, 1 / 0 / -1)` | `mixer.enableTrack` |
| `MIXNAME_VA` / `MIXCOLOR_VA` | `0x12be6f0` / `0x12c17e0` | `SetMixerTrackName(t, name, 1)` (renames linked playlist tracks too) / `SetMixerTrackColor(record, c)` | getters `0x12be330` / `0x12c1930` |
| `MIXOBJ_VA`, `SETROUTE_VA`, `ROUTEUPD_VA` | `0x15e22d8`, `0x12a33c0`, `0x12a28f0` | mixer object; `SetRoute(mixer, src, dst, on, 1)`; `RoutingChanged(mixer)` | `setRouteTo` |
| `MIXINS_VA` | `0x12a46f0` | `InsertMixerTracks(pos, count, flags)` | undo name "insert mixer track(s)" |
| `MIXDEL_VA` | `0x12a5280` | `DeleteMixerTracks(int dynarray, last, flags)` | "Delete tracks" dialog's caller |
| (not used) | `0x12a4030`, `0x12a4490`, `0x127cee0` | permute / move selected / set count | section 4 |

## 3. FLP / .fst event stream

`.flp` and `.fst` files are `FLhd` (header chunk) + `FLdt` (one chunk of events). An event is an id byte
and a payload sized by the id: `< 64` 1 byte, `< 128` 2, `< 192` 4, else a 7-bit varint length and
that many bytes.

**FL 2026 adds an escape** (reader `sub_A9E7A0`): id byte **172** (`0xAC`) is followed by a 32-bit
little-endian id. Ids below `0x100` keep the ranges above; ids `>= 0x100` take their size class from
the top bits: `0x00000000` byte, `0x40000000` word, `0x80000000` dword, `0xC0000000` varint data. Any
other top nibble is invalid. FL's known-id check `sub_A9ED80` accepts 0..0x34, 0x40..0x68, 0x80..0xAC,
0xC0..0xFF, `0x100`, `0x40000100`, `0x80000100` and `0xC0000100..0xC0000102`. Every FL 2026 file starts
with an escaped `0xC0000101` (a string such as `FL Studio 26.1.7.5654.5654`) right after the version
events (199, 159), and has an escaped `0x100` (byte) before the project-time event 237. A walker that
treats 172 as a plain dword event loses alignment there and misses everything after it. flim's
`flp_walk` (engine), `flp.py` and `live_test.py` handle it.

Events flim uses: 196 sample path (UTF-16), 199 version, 201 / 203 plugin name, 212 new plugin, 213
plugin data (Image-Line TLV records, or a VST wrapper blob), 224 note channel, 233 playlist items, 236
mixer track flags (`u32, flags, u32`, one per track in track order: 8 enabled, `0x1000` soloed), 237
project time (creation date + work time as two doubles).

## 4. Mixer tracks

### Records
`*MIXARR_VA` points at 502 track records of `0x1478` bytes; `**MIXCNT_VA` says how many are in use.
Track 0 is Master, 1..n-2 are inserts, n-1 is "Current".

| Offset | Field |
|---|---|
| `+0x04` | color |
| `+0x0c` | name (UnicodeString, `''` = "Insert N") |
| `+0x18` | enabled (byte) |
| `+0x19` | selected (byte); `sub_12C19C0(record)` = selected, or it is the focused track (int at `0x193FA10`) |
| `+0x1a` | soloed (byte); `mixer.isTrackSolo` reads it |
| `+0x1b` | solo leaves this track alone (byte; see Solo) |
| `+0x1c` | enabled before the solo (byte; see Solo) |
| `+0x28` | the track's own index (int) |
| `+0x2d` | dock side byte (insert copies the neighbour's with flag 4) |
| `+0x118`, `+0x158`, `+0x1c0` | per-track objects; each holds the track index (at `+8`, `+8`, `+0x1c`) |
| `+0x370`, `+0x378` | ints set to -1 when a track is deleted |
| `+0x394` | sends: 502 x {int level (12800 = 100%), byte on} |
| `+0x134c` | 10 effect slot objects (each knows its slot at `+0x134`) |

### Functions
- **`sub_127CEE0(n)` set count.** Runs under `SONGLOCK`, clamps n to 3..502. Tracks that come into use
  are reset (`sub_128AE20(t,1,0)`, `sub_127B780(t,7)`, `sub_12C18F0(rec,0)`), so the old Current record
  becomes an ordinary track. Channel targets and playlist links that point past the end are clamped
  to 0.
- **`sub_12A4030(int perm[502], undo)` permute.** New track `i` gets old record `perm[i]`. It copies all
  502 records through a temporary array (RTTI "TFXTracks", managed fields handled), builds the inverse
  map, then renumbers everything that refers to tracks: playlist track links (type 1, `+184`), every
  track's send array (`+0x394` reordered by `perm`), slot back-references (`sub_12BE0C0(slot, t,
  slot+0x134)`), the per-track objects' index fields, `+0x28`, channel targets (`+0x258`), automation
  (`sub_12CB6C0` / `sub_D2BED0` callbacks), and finally tells the project (`*(FLPROJ+0x5c)` vmt
  `+0x30` / `+0x28`). `undo` records `TUndoEvent_MovedFXTracks` (`sub_10199E0`). It takes `SONGLOCK`.
- **`sub_12A46F0(pos, count, flags)` insert** (`MIXINS_VA`). Flags: 1 undo step
  (`TUndoEvent_FXInsertTracks`, `sub_1019AA0`), 2 select the new tracks and scroll to them, 4 give them
  the dock side (`+0x2d`) of track `pos-1` (or `pos` with flag 8). Refuses if `count > 502 - n`. It
  grows the count, builds a permutation that moves the new tracks from the end to `pos`, permutes,
  then refreshes. FL's Insert menu (`InsertMixerTrackMenuItemClick` = `sub_129C990`) calls
  `sub_12A46F0(selected track, n, 7)`; "Insert multiple" asks "Number of tracks to add" first.
- **`sub_12A5280(int *idx, last, flags)` delete** (`MIXDEL_VA`). `idx` is a Delphi dynamic array of
  ascending track numbers (it only reads `idx[0..last]`, but flim passes a real header: int padding,
  int refcount, int64 length). Flags: 2 delete (a send A -> D -> C through a deleted track D becomes
  A -> C, via `SetRoute`), 1 trim unused, 0x10 undo step, 4 unlink linked playlist instrument/audio
  tracks, 8 delete those tracks and their channels too. It resets the deleted tracks, permutes them to
  the end, shrinks the count and renumbers with -1 for the removed ones. FL's Delete menu
  (`DeleteMixerTrackMenuClick` `0x129C980` -> `sub_12A5B60`) collects the selected tracks, refuses
  during recording, confirms, asks about linked playlist tracks ("Delete tracks and channels" = 8,
  "Unlink" = 4), then calls `sub_12A5280(sel, n-1, 0x12 | 4/8)`. Trim unused is `sub_12A6920`.
- **`sub_12A4490(dir, undo, scroll)` move selected.** For each selected (or focused) track it swaps its
  permutation entry with the neighbour in direction `dir` (`sub_12A43D0`, wrapping), then permutes
  and refreshes. Undo `sub_1018CC0`.
- **Refresh** after any of these, in this order: `sub_1046790(1)` (begin), the change,
  `sub_1182D70()` (each channel: `sub_1330D90`), `UIUpdate(7)` when the byte at `0x15E0BD0` isn't 1
  (then set it), `sub_11D5560()` (project work time), `sub_FDEED0()` (dirty flags `|= 7`), for each
  object in the list at `*0x15E37B0` its vmt `+0x70` and `sub_F5CE70(obj[23]+8)`, `sub_1046790(0)`, then
  `sub_127FE50(mixer form 0x16BD450)`.
- Other entry points: `AddOneMixerTrackMenuClick` `0x12A8BB0` ("Enter the number of mixer tracks",
  "increase nr of mixer tracks"), `AddTrackAfterSelectedBtnClick` `0x12A9290` (-> `sub_12A85E0`).

### Records move, so slot objects name tracks
Insert, delete and move all go through `sub_12A4030`, which copies whole records, so each track
keeps its 10 slot object pointers wherever it goes. flim uses **slot 0's object as the track's
identity**. Live, inserting 2 tracks at 3 in an 18-track mixer gave: the old Current record (17)
became the first new track (3), a fresh record became the second (4), old 3..16 became 5..18, and a
fresh record became Current (19). Deleting those two put the first deleted record into the unused
part, made the second the new Current, and moved everything back. So, comparing only tracks 1..n-2
(Master and Current never move), insert and delete each show up as exactly their new or missing
identities.

### How flim syncs it (engine "mixer track structure" section)
- `do_struct` (main thread, every 500 ms) diffs the identities against the last known list
  (`mix_diff`). New or missing identities go out as `{"kind":"mix.struct","n0","n","del":[old
  numbers],"ins":[new numbers]}`. Only moved means it isn't structural: the moved tracks' state goes
  out as ordinary edits (heavier, their plugins reload on the other side).
- Receivers check `n0`, then call `sub_12A5280(del, ..., 2)` and `sub_12A46F0(run, len, 4)` per
  contiguous run, lowest first. That's the menus' resulting state without undo steps or scrolling;
  linked channels the sender deleted arrive as `chan.del`.
- Both sides then carry each slot's bookkeeping to the new numbers, re-take the mixer name/color/send
  baselines and re-read every value without sending (`mix_rebaseline`).
- The poll thread sends nothing while the count differs from the last known one. It also undoes a
  tick's pending changes if the identities changed during that tick, so a delete caught halfway (FL
  permutes before it shrinks the count) never leaks shifted values.

### Solo
FL solos by muting the other tracks, so flim used to send those mutes: a solo soloed for everyone.
- `sub_127C2A0(track, mode, flags)` is the entry (`mixer.soloTrack`'s callback `sub_D5F580`). Mode 0
  unsolo, 1 solo; 2 and 3 toggle (they decide from the current state, and add flag 4 when another
  track is soloed already). The Python default for flags is 2.
- `sub_127BB10(track, on, flags)` does the work, record by record (all 501). Solo sets `+0x1a` on the
  soloed track only and saves `+0x18` into `+0x1c` (not with flag 4: the first solo's save stays).
  Then it mutes, through `sub_12C1810(record, on)`, every track that neither feeds nor is fed by the
  soloed one (`sub_127BAB0`), except tracks with `+0x1b`.
- Unsolo clears `+0x1a` and enables the tracks again: from `+0x1c` only if a main-form option is on
  (`*(*MAINFORM + 0xd98) + 0x49e`), otherwise all of them. In the test FLs, unsolo unmuted everything.
- `SetTrackEnabled(t, 0)` (`MUTE_RVA`) clears `+0x1a` on every track unless `t` has `+0x1b`: a mute
  click ends the solo and leaves the other tracks muted.
- Saved projects keep it: event 236 has `0x1000` on the soloed track and the solo's mutes in bit 8.
- FL 26.1.5 has the same offsets (its `SetTrackEnabled` reads `+0x1b` and clears `+0x1a`).

How flim keeps it local (engine `solo_*`, `mute_*`):
- While anything is soloed here, a track's synced mute is its `+0x1c`, so soloing changes nothing
  the others see.
- A mute that arrives meanwhile is written to `+0x1c` and heard after the unsolo. `SetTrackEnabled`
  would end the solo and leave the other tracks muted, and those mutes would then go out.
- When the solo ends, every track gets the mute state everyone shares back, whatever FL did (all
  enabled, or what a mute click left).
- FL changes the tracks one at a time around flagging the soloed one, so a poll pass can land
  half-way. A mute change counts only once two passes in a row read it, and not for 1 s after the
  solo state changed.
- A project snapshot (join, re-sync, backup) has none of the leader's solo: `project_snapshot` puts
  each track's `+0x1c` into bit 8 of event 236 and clears `0x1000`. Checked by restarting B while A
  had a track soloed: B came in with the shared mutes and no solo.
- Tests: `live_test.py solo`, harness `t_solo`.

## 5. Plugins: window state, loading, missing plugins

**Window state inside saved states.** Every saved plugin state carries its editor window, which is
each person's own view:
- in the plugin record (event 212, 52 bytes): flags at `+16`, bit 0 = editor open; editor x / y at
  `+36` / `+40` (verified by opening, closing and moving a Sampler and an OTT editor and diffing the
  saves);
- for a VST (event 201 = `Fruity Wrapper`): the wrapper data (event 213) is a 4-byte header and
  `[u32 id][u64 len][data]` records. Record 2's bytes 1..8 are the editor size (two int32: 567x805
  while open, 160x25 after closing). Everything from the `XTSV` marker on is the plugin's own state.

flim's `state_hash` ignores those bytes (opening or moving an editor is not an edit), and `do_load`
copies our own window bytes into a received state before loading it (someone else's load never opens,
closes or moves our windows). Before this, opening an editor sent the whole state and took the plugin
lock.

**Loads and FL's progress bar** (the "access violation reading FFFFFFFFFFFFFFFF" crash a member hit
when a sample finished downloading). `LoadPresetFile` reads the `.fst` through a stream whose refill
calls a progress callback: `sub_A9E2B0` -> `[stream+0x100044]` -> `sub_F3C210` -> `sub_661B70`
(the application's progress handler) -> `sub_1187300(mainform, ...)` -> `sub_11F3030` (a text bar) ->
`sub_11F19F0` = `ShowHint` + the hint bar's `Update` -> `UpdateWindow`. That synchronous repaint
happens while the plugin is half replaced. If a visible editor paints it (a Sampler's waveform:
`sub_F5AA10` = the plugin form's WndProc -> `sub_F6B100` -> `sub_CC1980` -> `sub_773990` ->
`sub_69B640` reads `*(sample+0x44)` = -1), FL raises, shows its error dialog (`sub_86A870`
`HandleException` -> `sub_11F1290` -> `sub_9636A0`), and the dialog's message loop repaints and faults
again. **`sub_1187300` skips all of this while the int at main form `+0x353C` is above 0.** FL's own long
operations bump it (`sub_12A4C30`, `sub_1190A90`: `++` before, `--` after), and flim does the same around
its `LoadPresetFile` calls (`progress_quiet`, main form = `**MAINOBJ_VA`).

**A plugin this FL doesn't have.** `LoadPresetFile` with a state of an uninstalled VST (tested with
OTT's state, its VST3 class id / name / file name changed) shows a modal `TMsgForm` titled "Warning",
"The plugin XYZ is missing. Please make sure...", with one button, and returns only after it's closed.
The slot then holds a `Fruity Wrapper` placeholder (`HasPlugin` = 1) whose saved state still contains
the original plugin's id, file name and data, so passing it on loses nothing.
The text comes from FL's plugin-error reporter `sub_12B88D0(name, type, code)`: code 60 = "The plugin
|%s| is missing. Please make sure it was installed correctly.", -2 crashed when opened, 30 couldn't be
found or opened, 40 not supported, 70 unknown problem, 'I' DirectX plugin not found, and others. It
shows `"Warning|" + text` through the message box `sub_9635B0` (`MSGBOX_VA`) unless the byte at
`0x15E2310` is set, which collects names into FL's missing-plugins list instead; project loads use
that and show one summary at the end. flim's detour of `MSGBOX_VA` lets such a warning through the
first time per plugin name while flim is loading a state (`g_flimLoad`), then answers it OK and logs a
`PLUGIN ...` line to flim's window. The load continues either way.

**How many params a plugin has.** The plugin holder (channel object or effect slot object) has FL's
param count at `+0x68` (`PLUG_NPARAMS`); `plugins.getParamCount` (`0xd7f330`) returns just that.
- Native plugins report their real count (Fruity Limiter 36, Soundgoodizer 6...).
- FL's VST wrapper always reports **4240**: 4096 VST param slots, then the wrapper's MIDI CC params
  ("MIDI CC #104" at 4200).
- `ProcessRECEvent` reads every unused VST slot as 0. So flim used to watch 4096 params per VST and,
  on a project with a dozen VSTs, filled its whole 65,536-slot table. Plugins added later then got no
  slots, and their knobs never synced.
- The unused slots have **empty names**. `plugins.getParamName` (`0xd7f490`) gets a name like this:
  `GetPlugin(holder, &intf)`, `fp = intf->vmt[0x20](intf)` (the plugin's FL SDK `TFruityPlug`), then
  `fp->vmt[0x20](fp, 0, index, 0, char name[4096])` (`TFruityPlug::GetName`, section 0 = param).
- So flim counts each plugin's params up to the last named one: Serum 2 541, OTT 20. This happens on
  FL's main thread, since GetName goes into the plugin: `plug_param_count`, task `T_PCOUNT`, once per
  plugin instance.

**Opening an editor from code.** `channels.showEditor` (handler `sub_D5C470`, main-thread callback
`sub_D5C3D0`) = `ResolveChannel(index, global, &obj)`, `HasPlugin(obj)`, then `obj->vmt[0xE0](obj, show)`
(1 show, 0 hide).

**Why loads opened editors (the "window flash").** In the loader's event processor `sub_F46490`
(`sub_F51FD0` -> `sub_F46490(frame)`; the frame holds the loader's args: mask at `+17544`, obj at
`+17552`, flags at `+17560`), after a plugin is (re)created for mask 32 or 48 it calls
`obj->vmt[0xE0](obj, 1)` unless `flags & 1` or the global `dword_16BAC70 > 0`. That happens even for
a state of the same plugin. Project loads (mask 0) instead reopen the editors whose plugin record has
the open bit (event 212 `+16`). flim passes flags 1; its window guard stays as a backstop.
FL's browser drop into a channel (`sub_FEDD90`) sets the byte `*off_15E2308` around its load: the loader
then keeps the channel's mixer target (FLP_TargetFXTrack). Not needed by flim.

**Zipped projects.** Opening a `.zip` extracts it to `%TEMP%\Image-Line\<pid>-{guid}\Zip\`; the project's
sample paths then point there (the project name is the `.zip`'s path) and FL deletes the folder later,
so the samples are gone after a crash or restart. flim's leader copies them into the song folder,
rewrites the paths and reopens the project from there (`temp_project_rescue`).

**Files plugins load (soundfonts, wavetables, samples).** Besides FL's sample paths (event 196) and
Image-Line TLV records, flim looks for file paths anywhere in plugin data (event 213), because each
plugin stores them its own way:
- SF2 Player keeps its soundfont as a Delphi short string: a length byte, the path with no end mark,
  then `0xFF` filler. Factory soundfonts are `%FLStudioFactoryData%\...`. The old scan read the
  filler as part of the path and dropped it, so a soundfont loaded during a session never went out.
- FL's VST wrapper names the plugin's binary in a record `[u32 id][u64 len][path]`, followed by the
  next record's id (a digit: `...\OTT.vst3` then `8`).
- Fruity Slicer 2: `[u32 len incl. NUL]` + ANSI path.
- VSTs write UTF-16 or UTF-8, with `\` or `/`.

The sender (`raw_collect`) takes a path that starts at a drive (`X:\`, `X:/`), a UNC `\\` or
`%FLStudioUserData%`, in UTF-16LE or 8-bit (UTF-8, else ANSI). Its end comes from a length right
before it (u64, u32 or byte as above, if the next character isn't a letter), else from a terminator,
else from the longest prefix that is an existing file. It must name a file (`\name.ext`) that isn't a
program or system file (`.exe`, `.dll`, `.vst3`, ..., anything under `Windows\`): those are installed,
not sent. The receiver (`raw_rewrite`) replaces each path it has a download for as an exact string in
all three encodings: with a same-length alias of the local file, or, in a short string, with the
local path and a new length byte. Tests: harness `t_samples`, `live_test.py sf2` (B downloads a
soundfont A's SF2 Player loads).

**Sample plugins and placeholders (Slicer 2).** Loaded with flim's silent 8 kHz placeholder, Fruity
Slicer 2 fits its slices to that; its own save afterwards has lost them. So after the download flim
reloads the state it *received* (`phload_*`), not the plugin's save of itself.

**Someone else's edits are not ours to send back (the Fruity Delay 3 lock).** Applying another
person's param changes moves the plugin's saved state. flim's state checks (the sweep, one unit every
`SWEEP_MS` while someone uses this FL, and the check after working in a plugin window) compared it
with the baseline from before and took the difference for a local change. A tester busy elsewhere in
FL then sent the whole state back under the plugin lock, and the editor's next moves snapped back;
while the editor held the lock, the tester's FL reloaded the old agreed state every couple of seconds
instead, undoing the edits there. Now each param applied from someone else delays that unit's checks
until a new baseline (`REBASE_MS` after the last one), and params a plugin moves along with it
(`SUPPRESS_MS`) aren't local edits. `live_test.py fd3_lock` failed both ways before.

**Reading plugin params during a load deadlocks.** With flim's poll thread calling `ProcessRECEvent`
(get) on Fruity Slicer 2's params while the main thread was inside `LoadPresetFile` on it, the *next*
load of Slicer 2 hung for good: main thread in `sub_F46490` (`0xf4c58f`) -> Slicer 2 -> its
`engine_x64.dll` waiting, no other thread in its code (plain FL, and flim without a session, never hung).
The poll thread now skips while `FLLOADS_VA` > 0 or a project load runs.

## 6. Threads and locks

- Call FL only from its main thread. flim runs that work from a **message-only window's WndProc**
  (`FlimTasks`), and the probe does the same. That window also takes flim.exe's commands
  (`WM_COPYDATA`) and runs the arrows' timer; flim draws nothing else into FL's UI. **Never destroy a plugin inside a `WH_GETMESSAGE`
  hook:** with a GPU-drawn editor (Xfer OTT, ...) FL later dies in unloaded `dxgi.dll` (event log:
  faulting module `dxgi.dll_unloaded`, offset `0x52b9`) because a DXGI hook is left dangling in the
  hook chain.
- `SONGLOCK` (`0x15e3d40` -> the `CRITICAL_SECTION` at `0x16b6aa8`) guards markers, the time signature
  and mixer structure, and is also the lock the **audio render** holds: `sub_D0D4C0` (render a buffer,
  thread proc `sub_5251B0`) -> `sub_CF4250` runs the mixing jobs (`sub_666A30`: job start `vmt+16`,
  wait `vmt+24` -> `sub_1276CE0`) while holding it. A tester's hang (Task Manager dump): the main thread in
  `LoadPresetFile` (`sub_F46490` -> channel reset `sub_FFE2D0`/`sub_FE90D0`) waiting for it, the render
  thread holding it and waiting for its jobs, the jobs stuck inside plugins (Fruity Phaser via
  Distructor's `module_x64.dll`, SDRR2). The load had been started by flim. `NOTELOCK` (`0x15e2008`)
  guards notes. flim takes them only around its own edits of those.
- Never replace or delete a plugin while that plugin's code is on the main thread's stack (its menu,
  knob drag or dialog runs a message loop that can dispatch flim's task window): the call returns into
  freed code, FL's "Execution of address ..." crash. flim's queue holds such tasks back (`main_busy`:
  stack walk with `RtlCaptureStackBackTrace`, anything not FL / Windows / flim; also while a mouse
  button is down).
- Reading a dump without a debugger: the `minidump` Python package (`pip install minidump`) gives
  threads, contexts, modules and memory; scan the stack from `Rsp` for values inside modules (the
  scratchpad scripts did that). A `CRITICAL_SECTION`'s `OwningThread` names the thread that holds it.
- The poll thread reads FL memory unlocked under SEH. Reads can tear, so flim discards ticks during
  which indices moved (channels: `g_structGen`; mixer: count + identity hash).

## 7. Checking things live

- `client\restart_test.ps1 -Minimized -NoLocalB` starts leader "A" and member "B" (needs a server on
  `localhost:8080`: flim.exe -> Start hosting, or `server/`).
- `mk.py` has the FL 2026 helpers: pids from the log, probe, layout constants, mixer
  (`mix_ids`, `mix_insert`, `mix_delete`, `mix_move`), channels, patterns, notes, clips, markers,
  pattern ops.
- `live_test.py` makes a change in one FL through FL's own functions and checks the other. It covers
  params, mixer names/mute/routes/colors, notes, patterns and pattern ops, markers, time signatures,
  channels (add, rename, reorder, delete, with a sample), clips, VST state, an SF2 Player soundfont,
  mixer insert, delete, append and move, solo staying local, and a plugin someone else is editing
  (51 checks). Run it after every port.
- The engine writes `%TEMP%\flim_crash_<pid>.dmp` on the first jump into unloaded code (on by default,
  `FLIM_CRASHDUMP=0` turns it off); flim.exe puts it into the crash report, and saves a hang report
  (a small dump of FL) after 20 s without an answer (`hang_watch`; `IsHungAppWindow` doesn't notice a
  stuck FL, a 3 s `SendMessageTimeout` does). `cdbX64 -z <dmp> -c ".ecxr; dps @rsp L90; q"` shows who called.
- flim only syncs changes made while someone uses FL (input in the last 1.5 s). The probe makes no
  input, so `live_test.py` runs a background thread that posts F24 key-ups to both FLs.

## 8. Signatures

The engine finds every address in section 2 that has a define (70 of them) at start, so an FL update
that only moves code needs no re-port.

**Tried on another build:** FL 26.1.5.5613 (installed next to 26.1.7 in `FL Studio 2026 OLD`).
- All 70 signatures were found exactly once. 68 of the 70 addresses differ from 26.1.7; only two
  Delphi RTL helpers stayed put. The old fixed table would have called into the wrong code
  everywhere.
- The sanity checks passed and the detours installed.
- A live session with A on 26.1.7 and B on 26.1.5 (`flrun.py`, `mk.py` with per-build addresses)
  passed `live_test.py` 39/39, once flim handled the "made with a newer FL" question and FL's version
  stamps (section 9).

**The table.** `engine/fl_addrs.h` has one row per address:
`FLA(name, va, off, nx, "pattern")`. The `X_VA` defines are `FLVA(X)`, which is `g_va[FA_X]`.
- `pattern` is bytes of `FLEngine_x64.dll`'s `.text`, `??` = any byte. It must match exactly once.
  The wildcards are the bytes that move when FL is rebuilt: rip-relative displacements, branch
  targets (rel8 and rel32), and every byte a base relocation touches (64-bit absolute addresses:
  their values differ when the DLL is loaded at another address).
- `off < 0`: a function's first bytes; the match is the address. 47 rows.
- `off >= 0`: an instruction that addresses a global rip-relative (`mov rax, [rip+X]`, `lea`):
  address = match + `nx` + the rel32 at match + `off`. 23 rows: the globals, plus the marker RTTI
  slot.
- `va` is the address in FL `FLIM_FL_BUILD` (`common/flim_config.h`), image base `0x400000`.

**At start** (`fl_addrs_resolve`, engine section "FL addresses"), before anything else touches FL:
- Each pattern is looked up in `.text`: `memchr` on its first concrete byte, then a compare. All 70
  take about 0.3 s.
- The FL build is the file version of the loaded `FLEngine_x64.dll`.
- Outcomes, all logged to `%TEMP%\flim_engine.log`:
  - **All found once:** the engine uses the scanned addresses. Log: "FL addresses: FL 26.1.7.5654,
    all 70 signatures found once in 282 ms, using them (equal to this build's table)".
  - **Found, but different from the table on FLIM_FL_BUILD itself:** each difference is logged and
    the table is used.
  - **Any not found, found twice, or decoding outside the image:** the failing names are logged
    ("1 of 70 signatures failed: REC (not found)"). On FLIM_FL_BUILD the table's VAs are used.
    On any other build, sync is off.
- **Sync off** (`sync_off_idle`) means FL is left alone: no hooks, no polling, no detours. Only the
  engine's task window runs, on the engine thread, so flim.exe still gets the reason. Its status
  says "Sync is off: flim doesn't know this FL Studio (version) yet", an alert lists the missing
  names, and Connect is refused.
- flim.exe no longer asks "start anyway?" for another build.

**Sanity checks** (`fl_sanity`, once FL has settled, before the vmt hooks and detours). These cover
what a signature can't find. A failure turns sync off on another build; on FLIM_FL_BUILD it is only
logged.
- Mixer: track count 3..502; each record's `+0x28` == its index (`MIX_STRIDE`, `MIXARR`, `MIXCNT`);
  `MIX_NAME` holds a UnicodeString.
- Channels: `+0x794` == index; `CH_SELECTED` is 0/1; `CH_NAME` holds a UnicodeString; vmt slots
  `CH_SETNAME_VSLOT` / `CH_SETCOLOR_VSLOT` point into `.text`.
- Patterns (1..999): note array element size (`+0x10`) == `NOTE_SIZE` where one exists
  (`PAT_STRIDE`, `PAT_NOTES`); `PAT_NAME` holds a UnicodeString.
- Playlist: arrangement 0's clip array element size == `CLIP_SIZE` (`ARR_CLIPS`).
- Project object: vmt `FLPROJ_LOAD_SLOT` and the six `PATOP_SLOT`s point into `.text`.
- Already checked elsewhere: the save detour and both message box detours compare their 12 prologue
  bytes before patching.
- "Holds a UnicodeString" means NULL, or codepage 1200 and element size 2 at -12 / -10.
- **Not checked** (wrong values would show up as wrong sync, not crashes, or are UI-only):
  - `MIX_ENABLED`, `MIX_COLOR`, `MIX_ROUTE`, `MIX_SLOTS_OFF`;
  - `CH_COLOR`, `CH_GRID`;
  - `PAT_COLOR`, `PAT_LENGTH`, `PAT_MARKERS`, the note record fields;
  - the clip record fields (`CLIP_*_OFS`), `ARR_MARKERS`, `MARK_SIZE`, `PL_TRK_*`;
  - the form offsets (`FORM_BUSY`, `FORM_NOPROGRESS`, `PLFORM_PATS`, `PL_MARKSEL`, `ED_*`, `PR_*`,
    `PL_GRID`, `GRID_VSCROLL`, `TRK_*`);
  - the note / clip vmt slots (`NOTEADD_VSLOT`, `CLIPADD_VSLOT`, `CLIPPURGE_VSLOT`);
  - the plugin param count path (`PLUG_NPARAMS`, `INTF_FRUITY_VSLOT`, `FRUITY_GETNAME_VSLOT`; section 5).
    It runs under SEH and falls back to 4096 params, so a wrong value costs slots, not a crash;
  - the REC id layout.

  After an FL update that passes everything, run `live_test.py` anyway.

**When a signature fails in a new FL build:**
1. In `client/dev/`, run `python flsigs.py --rebase` (system Python, `flre.py`'s capstone;
   `FLRE_DLL=<path>` selects a DLL other than the installed one). Every row whose pattern still
   matches once gets its new VA. The rest are listed, the same names the engine log gives.
2. Find those by hand as in section 1, put their VAs into `fl_addrs.h`, and set `FLIM_FL_BUILD` to
   the new build.
3. Run `python flsigs.py` (about 20 s). It rewrites every pattern from the VAs, checks each one the
   way the engine will (one match, decodes to its VA), and writes nothing if any fails.
4. Build. The engine log should say "all 70 signatures found once ... (equal to this build's
   table)". Run the harness (`t_groups` maps the installed DLL and resolves every signature) and
   `live_test.py`.

## 9. Networks and mixed FL builds

### Big messages go through the blob store
Projects and plugin states used to travel inside one WebSocket message: 3 MB as base64 for the 2.4 MB
test project, 2.8 MB for a big Serum 2 state. Measured over `slowlink.py` (1 Mbit/s each way,
+60 ms) with the 2.4 MB project:
- **Before:** the leader's FL froze for 15.5 s. Every main-thread send, including the 3 s "alive"
  timer, waited for the big send. 20 s in, the send failed, the connection dropped (the server's 15 s
  ping could not get through either) and the member never got the project.
- **After:** the upload took 31 s and the download 33 s. FL never stopped answering for more than
  0.46 s on either side, nothing disconnected, and `live_test.py` passed 39/39 over the same link.

How it works now (engine "big messages"):
- Any op over 32 KB (`BIG_MSG`) is put into the server's blob store by the outbox thread (HTTP PUT,
  streamed in 256 KB pieces, skipped when a `HEAD` shows the same content is already there).
- The WebSocket only carries `{"kind":"big","h":<sha256>,"n":<bytes>,"key":<its lock key>}`. The key
  stays visible so the server's lock check still applies.
- The receiver's inbox thread downloads it (verified by hash, retried), puts the server's `seq` /
  `from` back (`op_splice`), and handles it as if it had arrived inline. Later ops wait behind it.
- The leader's `alive` and the mouse arrows (`cur`) skip the outbox queue, so a long upload can't
  look like a frozen leader.
- This covers `project.load`, `plug.state`, `chan.add`, and the join answers `plug.full` / `state.full`.

### Moved samples
A project opened from someone else's PC or zip keeps their sample paths, here
`C:\Users\<them>\AppData\Local\Temp\Image-Line\{...}\x.wav`. FL finds such a file by name in the
project's folder. The sender now does the same (`sample_locate`): it uploads that file under the
path text the project uses, and receivers map that text to their download as before. Before this,
such projects went out with no samples at all.

### Mixed FL builds in one session
- **"Made with a newer FL" question.** Loading a project or `.fst` saved by a newer FL makes FL ask
  "This project/preset has been created using a more recent version of FL Studio (|x|) than yours
  (|y|)... continue loading?" (Yes / No / Get latest version, through `MSGBOX2_VA` = `sub_9636A0`;
  the loader `sub_F46490` aborts on No / Cancel). During flim's own loads the detour answers Yes and
  tells flim's window once. It recognises the question by this FL's build number in the text.
- **Version stamps.** Every saved state starts with events 199 (version text), 159 (build) and the
  escaped `0xC0000101` ("FL Studio 26.1.7.5654.5654"). `state_hash` now leaves them out. Otherwise
  the older FL saw every state it received as changed, sent it back, and the leader's reload of it
  swallowed edits made at that moment (`live_test.py` "param ch0 vol").

### What a very big project showed
The stress project (`Test FLP/trinity.../titty_3 (no VClip).flp`): 306 channels, 101 mixer tracks,
about 1,300 plugins (a dozen Serum 2), 3,681 playlist clips, 161 samples / 676 MB. It needs about
9 GB of RAM per FL and takes FL about 30 s to open. After the fixes below, `live_test.py` passes 39/39
on top of it. `VClip2.vst3` crashes FL by itself (an access violation in its window procedure), so
the test copy has it removed.
- **Channels past 256 weren't synced** (`MAX_CH` was 256). Now 1024; REC ids allow about 28,000.
- **A mixer move looked like a project close.** Swapping two tracks that carry effects emptied
  three or more slots in one scan (`PROJ_FX_MASS`). Sync paused and the move was never sent. Now
  an effect only counts as gone if its plugin is in no slot at all; FL's move copies whole track
  records, so it is one track over.
- **No baselines yet.** Plugin baselines (`Unit.hash`) are taken by the sweep, one unit per
  `SWEEP_MS`, which is about 15 minutes for this project. A preset load, or a VST swapped inside
  the same wrapper (FL keeps the wrapper, so the identity doesn't change), on a unit without one
  was silently taken as the baseline. A burst of local edits on such a unit now sends its state.
- **Snapshots during a reorder.** A burst or sweep snapshot that ran after FL reordered the
  channels, but before the structure scan sent `chan.order`, went out under the old index and
  overwrote the wrong channel on the other side. It waits now (`chan_list_changed`). This was the
  flaky "channel reorder" check.
- **False "leader froze" alerts.** Opening a big project keeps the leader's main thread (and its
  `alive` timer) busy for 30 s or more. The leader now sends `alive` with `"loading":1` when FL
  starts opening a project, and members allow it `LEADER_LOAD_MS` (5 min).
- **Plugin parameter counts.** FL's VST wrapper reports 4240 params for every VST. See section 5;
  this project now watches 27,586 params instead of filling the 65,536-slot table.

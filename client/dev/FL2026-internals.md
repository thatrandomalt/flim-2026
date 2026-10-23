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
| `ADDCHAN_VA` | `0x1004230` | `AddChannel(pos, select, group)` -> new channel: appended, then moved to `pos` (`sub_FFEEE0`) unless `pos` is -1. FL's Clone (`ChanCloneMenuClick` `0x11F5F20`) calls `AddChannel(source + 1, 0, 0)` and copies the source in (`sub_1005590(source, new, 0)`: its state saved and loaded like a preset) | tutorial `addChannel` |
| `DELCHAN_VA` | `0x1004a60` | `DeleteChannel(index, 0, flags)`; flags 1\|2 = no dialog. flim detours it (prologue `55 57 56 53 48 81 EC 88 00 00 00 48 8B EC`, the same in 26.1.5): see "The channel list" in section 5 | tutorial `removeChannel` |
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
| `NOTEARR_VA` / `NOTEDEL_VA` | `0x12d16e0` / `0x10603b0` | `NoteArray(pattern, update)`: makes the array if the pattern has none, and then, with `update` while FL plays, calls `sub_11E7470(song pos + 1, 0, 0)`, which takes `SONGLOCK` and then `NOTELOCK` (section 6); `DeleteNote(arr, i, 0)`; note record 24 bytes, channel `+6` | |
| `NOTELOCK_VA` | `0x15e2008` | CRITICAL_SECTION* held while editing notes (the audio thread reads them); -> the `CRITICAL_SECTION` at `0x16b6b20` | |
| playing | `0x193fb0c` (slot `0x15e09f8`) | int: 1 while FL plays (a space key posted to the main form starts / stops it) | `NoteArray` |
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
| `MIXMOVE_VA` / `MIXFOCUS_VA` | `0x12a4490` / `0x193fa10` | `MoveSelectedMixerTracks(dir, undo, scroll)` / int: the focused track | section 4 |
| (not used) | `0x12a4030`, `0x127cee0`, `0x1233b30` | permute / set count / move an effect to another slot of its track | section 4 |

### Automation links (section 5 has the details)
| Define | VA / offset | What | How found |
|---|---|---|---|
| `LINKS_VA` | `0x15e0a00` | -> { MIDI link list, internal link list } | the project writer's link loop `0xF43513` |
| `LINKDEL_VA` | `0xf832b0` | `DeleteLinksFrom(lo, hi)`: internal links whose source is in `[lo, hi]` (takes `SONGLOCK`) | channel type change `sub_FF7770` |
| `FORMTEXT_VA` | `0xbaffd0` | `FormulaText(formula, UnicodeString *out, 0)`: the text the project writer saves as event 230 | `0xF435EB` |
| (not used) | `0xf82530`, `0xf825b0`, `0x1191a50` | new link record (flags) / sort a list by source / "Create automation clip" | section 5 |

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
keeps its 10 slot object pointers wherever it goes. flim uses **the lowest of them as the track's
identity** (`mix_tid`). Not slot 0's alone: an effect's Move up / down (`sub_1233B30(new slot, undo)`
on the slot in `*0x15E2C38`, from `MoveUpMenuClick` `0x1233DE0` / `MoveDownMenuClick` `0x1235190` and
the mouse wheel) swaps two slot objects inside the track under `SONGLOCK` and remaps automation
(`sub_1233A70`). With slot 0's object as the identity, moving the first effect looked like the track
deleted and a new one inserted: the others deleted the track with its plugins and inserted an empty
one. Now the move shows up as two slots whose plugin changed, whose states go out (and the links that
FL remapped, with their clips). Live, inserting 2 tracks at 3 in an 18-track mixer gave: the old Current record (17)
became the first new track (3), a fresh record became the second (4), old 3..16 became 5..18, and a
fresh record became Current (19). Deleting those two put the first deleted record into the unused
part, made the second the new Current, and moved everything back. So, comparing only tracks 1..n-2
(Master and Current never move), insert and delete each show up as exactly their new or missing
identities.

### How flim syncs it (engine "mixer track structure" section)
- `do_struct` (main thread, every 500 ms, and at once when the poll thread sees the layout change)
  diffs the identities against the last known list (`mix_diff`) and sends `{"kind":"mix.struct","n0",
  "n","del":[old numbers],"ins":[new numbers][,"perm":[...]]}`. `perm` is there when tracks moved:
  replaying del and ins keeps the old tracks in their order and puts the new ones at their places, and
  position j then gets the track at `perm[j-1]` (j = 1..n-2).
- Receivers check `n0`, then call `sub_12A5280(del, ..., 2)` and `sub_12A46F0(run, len, 4)` per
  contiguous run, lowest first. That's the menus' resulting state without undo steps or scrolling;
  linked channels the sender deleted arrive as `chan.del`. Then `mix_reorder` replays `perm` with
  FL's own move, `sub_12A4490(-1, 0, 0)` (`MIXMOVE`), one step left at a time: only that track
  selected (`+0x19`) and focused (`MIXFOCUS`, int `0x193FA10`, whose pointer slot is `0x15E26A0`),
  which moves it and everything that refers to it exactly as the sender's move did. The selection and
  the focused track are put back by identity afterwards.
- Moves used to travel as per-track edits: names, sends, params and plugin states by number. The
  receiver applied each track's send row against its half-moved matrix, and FL's `SetRoute`
  (`0x12A33C0`) refuses a route that would close a loop (`0x127C640` walks the routes; it returns -1 and
  writes nothing). flim ignored that and re-baselined, so a new insert feeding its left neighbour and
  moved past it ended up routed nowhere on the others. A bus with ~20 senders swapped in one pass also
  went over `META_BULK` (24) and nothing was sent at all ("the routing of all the inserts got messed
  up"). `live_test.py mix_routes` covers both.
- Both sides then carry each slot's bookkeeping to the new numbers, re-take the mixer name/color/send
  baselines (and forget when they last sent them: the leader's conflict rule is by number) and
  re-read every value without sending (`mix_rebaseline`).
- The poll thread sends nothing while the count or the order differs from the last known one
  (`mix_layout_changed`): values read mid-move would go out by number and be moved a second time by the
  others' replay. It also undoes a tick's pending changes if the identities changed during that tick,
  so a delete caught halfway (FL permutes before it shrinks the count) never leaks shifted values.
- A received send row (`do_meta`, M_ROUTE) drops routes first, then adds, and retries refused ones once;
  `SetRoute`'s 5th argument is 0 (1 adds an undo step), and FL's "Disable routing? Some plugins use this
  track route" box is answered Yes (`g_mbAuto`), as the sender already chose.

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
- Nor does a song file the leader opens: one saved while soloed has the solo and the solo's mutes,
  and opening it sets every track's `+0x1c` to 1 (nothing of the mutes from before the solo is in the
  file), so `leader_project_loaded` strips the file it sends the same way: the others get every track
  enabled, what the leader gets on unsolo. It used to send the file as saved, and everyone opened it
  soloed (a tester's "soloing for one person does it for everyone").
- Tests: `live_test.py solo solo_song`, harness `t_solo`.

### Mute fade
- While audio runs, `SetTrackEnabled(t, 0)` doesn't clear `+0x18`: `sub_12C1810(record, 0)` starts a
  fade-out (`+0x151` = 2, samples left at `+0x154`) and the audio thread clears `+0x18` when it ends.
  An unmute meanwhile compares with `+0x18` (still 1), sees no change and does nothing, so the fade
  finishes muted. The test FLs mute at once (no fade), so this is from the code, not seen live.
- So a peer's mute and unmute applied back to back (queued behind other work, or arriving together)
  left the track muted here, and the poll took that for a local edit: sent to everyone (undoing the
  unmute) when someone here was at the keyboard, otherwise this FL alone stayed muted.
- Separately, a mute click back to the value flim had just applied counted as flim's own echo for
  `ECHO_MS` (2 s, meant for plugins rounding a value) and was never sent: on/off values go back to
  the applied one all the time.
- flim now: a peer's newer mute replaces one still queued for that track (`enqueue`); a mute that ends
  up against flim's last apply within `MUTE_FADE_MS` (500 ms) gets the applied value once more (task
  mode 2, dropped if another value was applied since); after those 500 ms any mute change is a click.
- Tests: `live_test.py mute_fast` (B's main thread held in `Sleep` while A mutes and unmutes; B's
  `+0x18` cleared right after an unmute lands, as a fade would; B's own clicks right after).

## 5. Plugins: window state, loading, missing plugins

**Window state inside saved states.** Every saved plugin state carries its editor window, which is
each person's own view:
- in the plugin record (event 212, 52 bytes): flags at `+16`, bit 0 = editor open; editor x / y at
  `+36` / `+40` (verified by opening, closing and moving a Sampler and an OTT editor and diffing the
  saves); its width / height at `+44` / `+48` (Fruity Parametric EQ 2 700x350, Fruity Compressor 500x105,
  a Sampler 0x0). FL corrects the size when the editor first opens after a load: kHs Gate's 236x458
  became 235x457 and stayed so after closing;
- for a VST (event 201 = `Fruity Wrapper`): the wrapper data (event 213) is a 4-byte header and
  `[u32 id][u64 len][data]` records. Record 2's bytes 1..8 are the editor size (two int32: 567x805
  while open, 160x25 after closing). Everything from the `XTSV` marker on is the plugin's own state.

flim's `state_hash` ignores those bytes (opening, moving or resizing an editor is not an edit), and
`do_load` copies our own window bytes into a received state before loading it (someone else's load
never opens, closes, moves or resizes our windows). Before this, opening an editor sent the whole state
and took the plugin lock.

**Zip entry times inside saved states.** Some plugins keep their state as a zip archive: Kilohearts'
(kHs Gate, kHs Compactor) is one `state.json` entry, deflated, with a data descriptor, inside FL's VST
wrapper record 53. Every entry carries its save time, DOS time + date in 2 s steps, in its local header
(`PK\3\4` `+10`) and in its central directory record (`PK\1\2` `+12`), so two saves differ as soon as the
clock moves on. `state_hash` clears those 4 bytes (`zip_times_clear`) in each archive in plugin data
(event 213) whose end record (`PK\5\6`) checks out: the directory ends right at the end record and holds
exactly the entries it counts, and each entry's offset (counted from the archive's own start, wherever
it sits in the data) lands on a local header with the same name. Bytes that only look like an archive
are hashed as they are. Only the hash's copy is cleared: loads and sends keep the real times. Not
covered: times in zip extra fields (`0x5455`, NTFS `0x000a`), which the kHs archives don't have. Tests:
harness `t_groups`, `live_test.py drift`.

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
local path and a new length byte (only outside TLV data, which covers FL's VST wrapper records,
whose lengths would otherwise be wrong). Tests: harness `t_samples`, `live_test.py sf2` (B downloads
a soundfont A's SF2 Player loads).

**Plugins that compress their state** hide their paths: Serum 2 keeps its samples in zstd-compressed
CBOR (`"XferJson\0"` blocks inside FL's VST wrapper record 53, key `samplePath`), Kontakt has its own
format. A Serum 2 reader that rewrote those blocks was built and removed again (2026-10-21): code for one
plugin's private format breaks when the plugin changes it. The shared folder (below) covers every such
plugin without reading its state. Raw scanning may still pick up a scrap of a path from compressed
bytes; it names no file, so it is never hashed or sent.

**Shared folder** (engine section "shared folder"; `flimcfg_shared_dir`). One folder with the same path
on every PC, `CSIDL_COMMON_DOCUMENTS\flim\Shared` = `C:\Users\Public\Documents\flim\Shared` (the same
physical path in every Windows language; OneDrive doesn't move Public folders; Wine maps it too; any
local user can write there). A plugin's saved path into it is right everywhere, so nothing is rewritten:
`samples_resolve` maps such a path to itself (`map_set(p, NULL)`) and the folder brings the file.
Senders still list those files as samples: someone whose Windows isn't on C: has another shared path,
which then travels like any sample. `FLIM_SHARED_DIR` moves the folder (tests: two FLs on one PC).
- Sync by version, not by file time (a copy keeps its old time, clocks differ). Per file: its content
  hash, its version, the highest version anyone sent, deleted here, made here. A change here gets
  max(version, highest seen) + 1; a higher version replaces ours; the same version with other content
  (two changes at once) goes to the higher hash. A version that only this PC had is first hard-linked
  (else copied) to `<root>\..\Shared backups\<time>\<path>`; no backup, no replace. A file deleted here
  is a tombstone: only a newer version brings it back (deletes don't sync). The table is
  `%APPDATA%\flim\shared-<hash of the root>.tsv`, so restarts know versions and deletions.
- Ops: `shared.files` with `full` 0 (what changed, after `/blobs/missing` + uploads), 1 (everything,
  on our welcome; announced only) or 2 (everyone's answer to a 1). A download that finds no blob asks
  its sender after 5 s: `shared.need` (`h`, `to`), at most every 30 s; the sender checks the store
  first. Dispatched before `hold_if_loading` (nothing of the song in them). Paths are base64 UTF-16.
- `shThread` owns the table: scans every 2 s (a changed file is hashed only once two scans agree on its
  size and time and a read-only open succeeds: nobody is writing it), decides, puts files in place.
  `shDlThread` only downloads verified `~flim-<hash>.part` files next to the target. A file with the
  same content elsewhere in the folder is copied instead of downloaded. Finished files go on
  `g_reload` in batches (`SH_RELOAD_MS`), so plugins naming them reload once per batch.
- Limits: 10000 files, paths up to 200 characters, files up to `SAMPLE_MAX_BYTES`; links (junctions)
  in the folder are skipped (and written through never: `sh_no_links`), the root must not be one;
  downloads keep 2 GB free. A peer's path must pass `sh_rel_ok`: no drive, `..`, stream, device name,
  trailing dot or space; not a program, script or shell file (`content_file` + a few more); not
  `desktop.ini`, `~$*` or an unfinished download (`.part`, `.crdownload`, `.tmp`).
- Tests: harness `t_samples` (path rules), `live_test.py shared` (both ways, newer version wins with a
  backup, an old-dated copy still wins, unfinished files and programs stay put, a delete survives B
  reconnecting, a channel keeps a shared path).

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

**Only a plugin someone worked in here goes out (the drift ping-pong).** Some plugins never save the
same bytes twice: Kilohearts' (kHs Gate, kHs Compactor) state is a zip whose entries carry the save
time (DOS time and date, 2 s steps: local header `+10`, central directory `+12`), and automation moves
others. flim's state checks (the sweep, the check after working in a plugin window, the one after a
preset-sized burst) used to send any difference if there had been input anywhere in FL since the
unit's baseline. Two people busy elsewhere, or `live_test.py`'s fake user, then sent such a plugin back
and forth on every sweep pass (about every 16 s), and each apply reloads it: an audio glitch each time.
Now:
- A state goes out only if someone here worked in that plugin since its baseline or the last state
  loaded into it (`unit_touched`: `Unit.userAt` later than `baseAt`): its params moved with local input
  (the poll thread's activity pass), input went into its plugin window (`plugin_input` ->
  `plugfocus_poll` -> `do_plugfocus`), a preset was loaded into it (`loadpreset_detour`; FL's browser drop
  onto a channel calls `LoadPresetFile` too), or its automation links changed
  here (`links_scan`). Any other difference is drift and isn't sent. Not seen: a file dropped on Patcher with
  Shift held (`sub_FEDD90` hands it to the plugin with `FPD_LoadFile`, no `LoadPresetFile`).
- The sweep skips plugins nobody worked in: a look would take the drift as the baseline, and with it an
  edit whose plugin-window check hasn't run yet. A check that sends nothing leaves `baseAt` alone, so a
  change the plugin makes a moment later (a file it loads in the background) still goes out with the
  sweep. A sent state, a silent baseline (plain knob tweaks, someone else's applies, moves) or a loaded
  state ends the work.
- Work in a plugin window means input whose own window is inside that `TPluginForm` (the message
  hook's `plugin_input`). It used to be the last clicked window (`g_focusHwnd`), so after one click in
  an editor every key-up or wheel turn anywhere counted as work in it: kHs Gate went out 20 times in a
  minute with the fake user typing. A click is checked once, `PLUGFOCUS_IDLE_MS` after the work stops
  (or every `PLUGFOCUS_LONG_MS` while it goes on); it used to be checked at once and again after.
- Working in a kHs plugin's window used to send it at each check (and the other FL reloaded it: a
  glitch): the hash couldn't tell its zip times from an edit, and the first open of its editor after a
  load moved its size in the plugin record by a pixel. Both are left out now ("Window state" and "Zip
  entry times" above): a click in kHs Gate's window gets it checked (the `PLUGFOCUS` log line) and sends
  nothing. Its saves after a load, 10 s and 40 s later, with the editor first opened, clicked and closed
  all hash the same (all six differed before).
- Still sent: working in a plugin that drifts some other way sends it at each check. Input into a
  bridged plugin's editor (another process) never reaches the hook: its param changes count
  (`input_recent`), but a change that moves no param only goes out with the next work in that plugin here.
- Live, light test project, 2 minutes with the fake user and nobody in a plugin: 8 states sent and 3
  reloads before (lock reverts; the rest arrived in the same 2 s and matched), none now. Test:
  `live_test.py drift`.

**Reading plugin params during a load deadlocks.** With flim's poll thread calling `ProcessRECEvent`
(get) on Fruity Slicer 2's params while the main thread was inside `LoadPresetFile` on it, the *next*
load of Slicer 2 hung for good: main thread in `sub_F46490` (`0xf4c58f`) -> Slicer 2 -> its
`engine_x64.dll` waiting, no other thread in its code (plain FL, and flim without a session, never hung).
The poll thread now skips while `FLLOADS_VA` > 0 or a project load runs.
- FL lowers that count only on the loader's normal way out (`sub_F51FD0`: `++` at `0xf520e1`, `--` at
  `0xf53f4b`), and not every way out passes the decrement. Seen live: `LoadPresetFile` given a `.wav`
  (probe call, mask 32, a channel) left it raised, one per call, and flim then held everything on that FL
  ("FL is loading something") for good. FL's browser drop onto a channel (`sub_FEDD90`) calls
  `LoadPresetFile(file, 32, channel, flags)` with the dropped file first (`0xfedf99`), so a sample drop may
  hit the same exit; not reproduced with a real drag. `loadpreset_call` (around flim's own calls and inside
  `loadpreset_detour`) puts back whatever a call leaves above where it started: the call is synchronous, so
  that is a leak. `live_test.py sample_drop` checks the count after loading another sample into a Sampler.

**The channel list.** flim knows channels only by their object's address: a channel has no ID (the
only per-channel counter-like field is its index at `+0x794`). It used to sync only channels added at
the end, plus pure reorders. A Clone puts the copy right after its source, undo puts deleted channels
back where they were, and a drop lands where it is dropped. Such an insert was logged as "changed in an
unsupported way" and dropped, and every channel after it was off by one from then on: later adds were
skipped ("chan.add: expected index 8 but have 15 channels"), and states, notes and edits landed on the
wrong channels (a tester's session lost a channel's plugin that way). How it works now (engine
`chan_struct_local`, `do_addch`, `do_delch`):
- A change goes out as steps the others replay with FL's own functions. First the deleted channels
  (highest index first), then the rest's new order (`chan.order`), then each new channel at its index,
  lowest first (`chan.add` with its state, then its notes, then its settled state 1.5 s later). The
  receiver inserts with `AddChannel(pos, 0, 1)`, as Clone does.
- Each op carries `n0`, the channel count it applies to. A receiver whose count differs is out of step:
  a member asks for the leader's project again, the leader sends its project to everyone
  (`resync`, at most once every 10 s).
- FL hands a deleted channel's memory to the next channel it makes, so a delete and an add between two
  checks looked like one channel moving. The `DeleteChannel` detour notes each deleted channel, and the
  check (now within one poll pass, about 30 ms, after FL's list changes) counts one of those found
  again as new.
- A peer's channel index means a place in the list everyone has (`g_chKnown`), which this FL keeps up
  to date op by op, even while its own change isn't sent yet. Peer loads bind to a channel when they
  run, not when they arrive behind other queued channel ops. This FL's own structural change waits
  until the peers' ops that came before it are applied (up to 2 s).
- A state check queued before channels or tracks moved is dropped (a moved unit has no baseline yet,
  and its index-dependent state looked like an edit) and taken again later.
- Tests: `live_test.py chan_insert` (an insert, a burst of three, a delete and an insert together, a
  Clone) and `chan_race` (both insert at the same moment). The old engine failed all but the first.

### Edits made in the same moment as a structure change
Everything flim compares is keyed by number: a value by REC id, a name / color / route by track or channel
number, notes by their channel field, clips by the channel they play. A structure change renumbers it
all, so flim used to re-take every baseline from FL afterwards (and re-read every value while one was
pending). Any edit made in that moment was taken for known and never went out: a burst of everyday edits
from one person left the other FL with different names, mutes, routes, volumes, effects, notes and clips
(`live_test.py stress`: 2 of 18 checks matched). Now the main thread carries the baselines through the
change with `from[]` (new number -> old, -1 = new), so only real edits differ afterwards and go out after
the structure op:
- `chan_rebase` (`do_addch`, `do_delch`, `do_order`, `chan_struct_local`, which builds one map through
  its deletes, reorder and inserts) and `mix_rebaseline` (`mix_struct`, `do_mixstruct`):
  - `slots_rebase` renames each watched value to its entity's new REC id, keeping its last value
    (channel `ch << 16`, mixer `t << 22` and send destinations, mute `0xF0000000 | t`);
  - `meta_remap` moves names, colors and routes with their entity (the route hash is by destination
    track identity, so renumbering isn't a route edit);
  - `notes_remap` rewrites the channel of every stored note (`+6`) and channel clip (`+4` =
    `ch << 16 | 0x5000`; a pattern clip is `(0x5000 + pattern) << 16 | 0x5000`) in the last / pending /
    base states.
- Pending sends move with their unit: a plugin swapped (`identAt`) or edited (`lastAct`) just before its
  track or channel moved still goes out, and a state check queued before a move is tried again.
- The poll thread doesn't re-read values while a structure change is pending or after it; what bumps
  `g_structGen` without a map (a project load, an order FL didn't follow) re-reads itself (`slots_reread`).
- An edit read just as a structure change landed could still be lost in two places, both fixed:
  - the poll thread used to take a changed value as seen (`Slot.last`) before checking whether the tick
    was clean; when a rebase came in between, the rollback couldn't find the renamed slot and the value
    was never sent. Now a change becomes seen only once the tick is clean (`slot_take`, and the commit
    after the `g_structGen` check);
  - a `T_METASNAP` (name / color / route send) is queued by number; run after a renumbering it looked at
    the wrong entity and sent nothing. Now, if `g_structGen` moved since it was queued, `do_metasnap`
    hands every unsent edit of that kind back to `scan_meta` (`g_mAt = 1`), which queues them again
    under their new numbers.
  - a new channel's / track's values aren't watched until `rescan` adds them (`RESCAN_MS`), and `rescan`
    takes what they are then as their start: an edit made on a just-inserted track was never sent.
    `chan_rebase` / `mix_rebaseline` now ask for a rescan on the next tick (`g_rescanNow`).
- A plain param edit whose lock the server refused (someone else held that knob) was dropped by the
  server but stayed in the editor's FL. The first edit of a lock now keeps its old value (`Slot.undo`,
  `hasUndo`, as plugin params already did), and a refusal applies it back ("lock denied ...: local edit
  put back").
- Pattern ops carry note and pattern-name / marker baselines through their map too (`pat_rebase`, from
  `sc_done_patop`): a note edit made in the same moment still goes out. Clips are still re-taken: their
  baseline is one hash for the whole arrangement. The moves (`0x108`, `0x110`) and split (`0x120`) have
  no known map and still re-take everything.

### Structure ops that cross (the structure count)
Two people changing channels or mixer tracks in the same instant each change their own list first, and
the other's op lands on a list it wasn't made for. Example from a test run: A inserts a mixer track
while B moves one. The track count is the same either way, so the `n0` checks pass, and the lists
differ from then on: every later edit by number lands on different tracks. Separately, an edit someone
made before a structure change reached them still names channels / tracks by their old numbers.
- `g_sc[SC_CH | SC_MIX | SC_PAT]` count the structure ops of each kind this FL has applied, its own
  included: channels (`chan.add`, `chan.del`, `chan.order`), mixer tracks (`mix.struct`), patterns
  (`pat.op`). `sc_done` counts on send (`chan_struct_local`, `mix_struct`, `patop_common`) and on
  apply (`do_addch`, `do_delch`, `do_order`, `do_mixstruct`, `do_patop`); `run_task` counts one that
  failed here too. The server orders all ops, so FLs in step count alike. `ws_send` /
  `ws_send_samples` stamp every op with the sender's counts:
  `{"t":"op","op":{"sc":[ch,mix,pat],...}}` (`sc_stamp`). No server change was needed.
- One count per kind, not one for all: kinds don't conflict, so two FLs may apply a channel op and a
  mixer op in either order. With a single count, both then held the same number for different
  histories. A's next channel op reached B at B's count, and B missed that it crossed B's own channel
  op (the leader saw the crossing and skipped it; B drifted until the safety net). Found by
  `stress_both`.
- A structure op whose sender's count of its kind isn't ours crossed another one (`sc_rebase`). The
  leader skips it. A member re-syncs (loads the leader's project). Everyone who already applied it sees
  a crossing too (they apply the leader's op at a count it wasn't made for), so all end up with the
  leader's lists. In a test run the member had the leader's project 60 ms after the crossing, instead
  of 30 s+ through the safety net.
- One cross-kind case: a new channel (`chan.add`) made before a mixer change it hadn't seen. An
  automation channel's link names a mixer track by number. Only the FL receiving the channel can tell
  (the mixer op crosses nothing on the other side), so it re-syncs, and if it is the leader, it sends
  everyone its project.
- A change made in this FL counts only once flim sends it (`do_struct`, a poll tick or more after FL
  did it). Channel ops from peers are applied through the known list (`g_chKnown`), so a local channel
  change not yet sent doesn't matter to them. Mixer ops and edits name FL's current tracks, though.
  A peer's op that names mixer tracks (`names_mix_tracks`) is therefore preceded by any track change
  still waiting here (`run_task` calls `mix_struct`), then moved through it or found crossed.
- Each applied structure op is logged with its map (`g_scLog[kind]`, `SC_LOG` = 64 each, new number ->
  old). A peer's edit with an older count of a kind it names is moved through the ones of that kind it
  hadn't seen, before it runs:
  - `T_PARAM` through `rebase_slot_id`;
  - `T_LOAD` / `T_FXCLEAR` by channel or track;
  - `T_NOTES`: its pattern; the channel filter and each note's channel;
  - `T_CLIPS`: each clip's channel (`ch << 16 | 0x5000` at `+4`) or pattern (`(0x5000 + p) << 16 |
    0x5000`), clips of gone ones left out (`recs_remap`);
  - `T_META`: `"i"` (channel, mixer track or pattern, by its tag) and a route's targets
    (`meta_path_remap`).

  An edit for something a structure op removed is dropped. A log entry with no map of a kind the edit
  names (FL didn't do it as sent, a pattern move), or a newer `sc` (a crossing this FL skipped), leaves
  the edit as it is.
- Pattern op maps (`patop_map`, measured on FL 2026 with uniquely named patterns): `Insert(p)` puts an
  empty pattern at `p`, later ones one up; `Clone(pats)` puts each copy right after its source (sources
  in ascending order); `Delete(pats)` closes the gap. The probe couldn't run Delete itself (its
  confirmation is modal); removing and closing the gap is the only thing it can do to the numbers.
- Members start at -1 (unknown) and take the counts from the leader's `project.load` when it runs
  (`T_PROJLOAD`, in queue order). Without counts (an older flim on either side) all of this is skipped
  and things work as before.
- Locks name units by number too: `plug:ch:<i>`, `plug:fx:<t>:<s>`, `pat:<p>:<ch>`. Kept across a
  renumbering, a lock held for one channel then sat on whichever channel moved into that number. Found
  in the full suite: B held `plug:ch:13`, channels shifted, the leader's new automation channel landed
  at 13, its settled state was refused (lock) and reverted, so B never got it. Now when numbers
  actually move (`chan_rebase`, `mix_rebaseline`, `pat_rebase` / a pattern op with no map), each FL
  gives back its own locks of that kind (`ulock_release_moved`). The next edit takes them again.
- A channel rename made within 2 s around a remote state loading into that channel was taken as
  coming from the load (`absorbUntil` in `scan_meta`) and never sent. An automation channel's state
  doesn't carry its name, so the load doesn't touch it. Found in the full suite: A renamed B's new
  automation channel just as B's state for it loaded. Now `do_load` hashes the channel's name and
  color before the load, and takes them as known after it only if the load changed them
  (`meta_take_channel`); the window is gone.
- After a remote name / color apply, `meta_absorb` took every entity's current value as known (FL's
  setters touch others: a mixer rename renames linked playlist tracks), which swallowed a rename made
  since the scanner's last look. Now only what changed during the apply (`meta_absorb_begin` hashes
  before it; user edits run on the same main thread, so none land inside it).
- `notes_apply` leaves out notes for channels the FL doesn't have. FL calls them "invalid notes" and
  stops the next load of the project with a modal about them.
- Tests:
  - `client/dev/harness/t_groups.c`: moving and dropping edits; a crossing skipped, ops of other kinds
    not; the pattern op maps; notes, clips and pattern names through a pattern insert.
  - `live_test.py stress_both`: A and B on a thread each, at the same moment (channels, mixer
    tracks, pattern insert / clone, notes, names, volumes, mutes); `FLIM_STRESS_SEED` sets the seed.

  Before these changes, seeds 11–15 all ended with volumes, names, mutes and routes on different
  tracks, found only by the safety net or not at all. After them, seeds 11–15 converged, and crossings
  were re-synced at once. Note order within the same position can differ between FLs, so
  `stress_both` compares notes as sets.
- Not covered:
  - edits made before a pattern move / split go as they are (no map);
  - edits a member makes while it loads the leader's project (a crossing's re-sync takes 1–3 s) are
    absorbed by the load. `stress_both` keeps editing through a re-sync and sometimes needs the safety
    net afterwards for that.
- **A pattern's length is cached** in its record: `+0x48` / `+0x4c` / `+0x50` (length in ticks), `+0x54`,
  `+0x5c` (60 when empty). `UpdatePatternLength(p, flag)` (`PATLENUPD`, `sub_12D17A0`) works them out from
  the notes and markers. Its last step, `sub_E4B5F0(playlist, p, old, new)`, resizes the pattern's
  playlist clips whose length was the old one; `flag` only redraws the editors when `p` is the current
  pattern. FL runs it after its own note edits (`PatternLengthChanged` passes 1), but not after notes
  added through the array's vmt, which is what flim's apply does. The receiver's cache stayed stale and
  later cut the writer's longer clips back: the two fought over them (`LOCK clips:... local change
  reverted`). `notes_apply` now ends with it (`pat_len_recalc`). Test: `live_test.py
  pattern_length_notes` (2 checks; the length check failed on the old engine). `stress`'s note op now
  runs it too, as FL's editing would.
- Still open: in a long run (the whole suite, then `stress`), A's clips can end up longer than the
  pattern after a channel delete took the pattern's notes. FL on A shrinks the pattern but not its
  clips, while B's FL and any project load size them to the pattern. No op carries that, and a reload
  doesn't fix it (the safety net stops reloading for it; see below).

### The desync safety net
Whatever still slips through mustn't stay. Examples: two people changing structure in the same instant, an
op lost to a crash or a dropped connection, a value FL moved by itself on one side only. So the leader
fingerprints the song (`digest_take`) and sends it (`digest` op) every `DIGEST_NET_MS` (15 s), but only
while nothing is in flight: no song op sent or received for `QUIET_MS` (5 s), its queue empty, no project
loading. A member that is quiet too compares the fingerprint with its own, part by part. If a part differs
on two checks in a row, it loads the leader's project (`resync`), at most once per `DESYNC_MIN_MS` (2 min).
- **The parts:**
  - names and colors of channels, mixer tracks, patterns (with lengths) and playlist tracks;
  - markers, time signature, pattern markers;
  - routes by track number;
  - notes and the current arrangement's clips (their saved first 0x20 bytes), in any order, selection
    masked;
  - automation links;
  - mixer track volume and pan, unless automated.
- **Left out:** each person's own state: solo and mutes, selections, windows, main volume, plugin states
  (they drift).
- **The valve:** a part that still differs on the first check after such a reload is one FL itself makes
  different (it normalizes some things on load). The member stops reloading for it until it matches
  again, and logs `DESYNC <part> still differ after loading the leader's project: a reload doesn't fix
  that`. Without the valve the clip case above reloaded every 2 minutes.
- Input doesn't block a check: an edit still settling makes one mismatch, and a reload needs two.
- **Live:**
  - B's mixer volume moved with no input there (taken for automation, so not sent): two mismatches,
    then a reload, then equal again, 40 s in all.
  - In the full suite it fixed mixer volumes and automation links after `stress`'s "both" phase; its
    clips stayed different (the valve).
  - No false reloads when idle.
- `t_clip` writes clip records FL doesn't make itself (item `0x5001`); after a reload B has FL's form of
  them. That's the valve's case too.
- Notes and the bulk guard: a channel insert / delete renumbers the notes of every later channel in every
  pattern, and the note scanner (which keeps plain hashes, not records) sees all of those patterns change
  at once. `NOTES_BULK` (8 patterns = a project load) used to re-baseline them all, and a note edited in
  the same scan was never sent (3 of 3 lost with a delete and a note at once, HEAD included; `stress`
  phase "A alone" notes whenever `chan_race` ended in a real race, which changes the seeded op sequence).
  Now nothing settles while FL's channel list differs from flim's (`chan_list_changed`, as for clips),
  a pattern that equals its renumbered base as sets (`pat_matches_base`: `group_diff` against
  `unit_base`) only changed number and is taken as known, and only the rest count toward `NOTES_BULK`.
  `do_notesnap` waits too if a structure change is pending: a check queued just before one would send
  renumbered notes ahead of the chan op.
- `main_busy` holds plugin-touching tasks while a mouse button is down only when FL is the foreground
  app: the global button state held flim's queue for as long as someone dragged anything in any program.

### Automation clips and controller links
An automation clip is an ordinary channel (type 5, points in event 234). What it drives is a **link**,
kept apart from every channel: `*LINKS` (`0x15E0A00` -> `0x16BACB8`) = { MIDI list (event 226: each
person's hardware), internal list (event 227) }, TLists (items `+8`, count `+0x10`, capacity `+0x14`)
of 28-byte records from `sub_F82530(flags)`: `+0` formula object (event 230, text via `FORMTEXT`
`sub_BAFFD0(formula, &str, 0)`), `+8` source (the clip channel's REC base `index << 16`, or a mixer
slot's for a controller there; plugin controller outputs add `0x8000 + k`), `+0xC` runtime, `+0x10`
target REC id, `+0x14` flags (8 internal, 4 MIDI, 2 smoothing), `+0x18` smoothing (469).
- Saved only with whole projects (`SavePresetFile` mask 0 writes 226/227/230 at `0xF435C5`; a channel
  or slot state never has them). The loader appends a 226/227 it finds in any state, mask 32/48 too,
  with no check for duplicates, but sorts the lists only after a project (`sub_F825B0`, a stable sort
  by `+8`; FL's own adders sort right after appending). Playback (`sub_D0B7B0(source, value)` from
  `0xD0C8E0`) finds a source's links by binary search (`sub_F82660`), so an unsorted list silently
  drives nothing for some clips while the UI still shows them linked.
- FL makes them in `sub_1191A50(target, 1, 0, 1, undo)` ("Create automation clip": `AddChannel(-2)`
  from `AutoTrack.fst`, then the link, under `SONGLOCK`); Clone and Make unique copy a clip's links
  (`sub_13030A0`); `sub_F697D0` adds a target, `RemTargetLinkBtnClick` `0xF66920` removes one,
  `LINKDEL` `sub_F832B0(lo, hi)` deletes those whose source is in `[lo, hi]` (under `SONGLOCK`).
  Channel and mixer structure changes renumber both ends (`sub_12CB6C0`), freeing a plugin deletes the
  links into its params (`sub_12BA900` -> `sub_12CA340`), a channel changing type deletes its own.
- **How flim syncs them** (engine section "automation links"): `unit_save` appends the unit's own
  links to its state as 227 (+230), so they travel with the clip (chan.add, plug.state, plug.full)
  but in a field of their own (`"lk"`): an older flim, loading them into its list with every state,
  would pile up copies. A load of a state with links first drops the unit's (`LINKDEL`), rebases
  their source to the unit it loads into, and sorts the list after (`links_sort`, under `SONGLOCK`).
  The poll thread hashes the list per source every 250 ms (`links_scan`): a change made here counts
  as an edit of that clip; one made by FL during a load or structure op (`LINK_QUIET_MS`) doesn't. When
  a slot's or channel's plugin changes, the clips with links into it go again after it
  (`links_resend_into`): the clip may have arrived first and lost its link to FL's plugin swap there.
- A param that is a link's target is never sent or applied (`auto_target`): each FL plays its own
  automation. Before, one person playing the song sent the automated knob to everyone.
- Tests: `live_test.py auto_clip auto_add auto_remove slot_move`.

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
  Distructor's `module_x64.dll`, SDRR2). The load had been started by flim. `NOTELOCK` (`0x15e2008`
  -> `0x16b6b20`) guards notes. flim takes them only around its own edits of those.
- **Lock order: `SONGLOCK`, then `NOTELOCK`.** The audio thread takes them in that order
  (`sub_D0C220` takes the note lock first thing, inside the render's song lock), and so does FL's
  `sub_11E7470`, which `NoteArray(p, 1)` calls when it makes a pattern's note array while FL plays.
  flim's `notes_apply` used to call `NoteArray(p, 1)` holding the note lock: the first notes of a
  pattern someone else wrote, arriving while this FL played, deadlocked it (hang report 2026-10-21
  175345: main thread in `notes_apply` -> `NoteArray` -> `sub_11E7470` waiting for the song lock,
  render thread holding it and waiting for the note lock). Now the array is made before the note
  lock is taken. `live_test.py notes_playing` writes notes into 20 new patterns while B plays: the old
  engine froze B at the sixth.
- Never replace or delete a plugin while that plugin's code is on the main thread's stack (its menu,
  knob drag or dialog runs a message loop that can dispatch flim's task window): the call returns into
  freed code, FL's "Execution of address ..." crash. flim's queue holds such tasks back (`main_busy`:
  stack walk with `RtlCaptureStackBackTrace`, anything not FL / Windows / flim; also while a mouse
  button is down).
- A task still queued can be replaced in place (a newer state for the same unit, a peer's newer mute),
  so the main thread copies a task out of the queue as it takes it, under the queue lock. It used to
  copy it before its busy checks: a state replaced meanwhile was freed under it and the newer one lost.
- Two testers' "crashes" (2026-10-21) were hangs: FL's main thread stayed inside one of flim's tasks
  (`BEAT ... inHook=1 hookCalls=0`, the queue growing), and Windows closed FL when they chose "Close
  the program" (exit code `0xCFFFFFFF`). The likely calls are a state save (`SavePresetFile` ->
  getChunk) or load during playback. Since then: tasks that save or replace plugins wait while a
  plugin's menu/drag/dialog is on the stack or a mouse button is down (`calls_plugins` + `main_busy`);
  the poll thread doesn't read plugin params or rescan while the main thread is in a task (`g_inHook`);
  a task running over 5 s is logged with FL's main-thread stack (`main_stuck`: suspend, unwind with
  `RtlVirtualUnwind`, resume, then name the modules: `STUCK main thread: ntdll.dll+... Serum2.vst3+...`).
  flim.exe makes the hang dump after 8 s (was 20) and puts it into the report when FL is closed while
  hung ("FL not responding <time>.zip").
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
  mixer insert, delete, append and move, routes across moves, solo staying local, quick mutes, a plugin
  someone else is editing, channels inserted mid-list, quickly and by both at once, the main volume
  staying local, the shared folder, notes into new patterns while the other FL plays, automation
  clips and their links, an effect moved inside its track, a drifting plugin staying put, another sample loaded into a Sampler, a pattern made longer by someone else's notes, and a soloed song (123 checks, 18 of them the quick-change stress test; the last
  test opens a song). Run it after every port.
- The engine writes `%TEMP%\flim_crash_<pid>.dmp` on the first jump into unloaded code (on by default,
  `FLIM_CRASHDUMP=0` turns it off); flim.exe puts it into the crash report, and saves a hang report
  (a small dump of FL) after 8 s without an answer (`hang_watch`; `IsHungAppWindow` doesn't notice a
  stuck FL, a 3 s `SendMessageTimeout` does). `cdbX64 -z <dmp> -c ".ecxr; dps @rsp L90; q"` shows who called.
- flim only syncs changes made while someone uses FL (input in the last 1.5 s). The probe makes no
  input, so `live_test.py` runs a background thread that posts F24 key-ups to both FLs.

## 8. Signatures

The engine finds every address in section 2 that has a define (75 of them) at start, so an FL update
that only moves code needs no re-port.

**Tried on another build:** FL 26.1.5.5613 (installed next to 26.1.7 in `FL Studio 2026 OLD`).
- All 70 signatures were found exactly once (75 since MIXMOVE, MIXFOCUS, LINKS, LINKDEL and FORMTEXT:
  all found there too). 68 of the 70 addresses differ from 26.1.7; only two
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
- `off < 0`: a function's first bytes; the match is the address. 50 rows.
- `off >= 0`: an instruction that addresses a global rip-relative (`mov rax, [rip+X]`, `lea`):
  address = match + `nx` + the rel32 at match + `off`. 25 rows: the globals, plus the marker RTTI
  slot.
- `va` is the address in FL `FLIM_FL_BUILD` (`common/flim_config.h`), image base `0x400000`.

**At start** (`fl_addrs_resolve`, engine section "FL addresses"), before anything else touches FL:
- Each pattern is looked up in `.text`: `memchr` on its first concrete byte, then a compare. All 75
  take about 0.3 s.
- The FL build is the file version of the loaded `FLEngine_x64.dll`.
- Outcomes, all logged to `%TEMP%\flim_engine.log`:
  - **All found once:** the engine uses the scanned addresses. Log: "FL addresses: FL 26.1.7.5654,
    all 75 signatures found once in 343 ms, using them (equal to this build's table)".
  - **Found, but different from the table on FLIM_FL_BUILD itself:** each difference is logged and
    the table is used.
  - **Any not found, found twice, or decoding outside the image:** the failing names are logged
    ("1 of 75 signatures failed: REC (not found)"). On FLIM_FL_BUILD the table's VAs are used.
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
4. Build. The engine log should say "all 75 signatures found once ... (equal to this build's
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

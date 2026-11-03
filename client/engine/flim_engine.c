// flim engine (native, injected into FL). v3: params + plugin state + channels.
//  - joins a named room on flim-server (first in = leader)
//  - DETECT: polls params through FL's ProcessRECEvent (f4e910, REC_GetValue)
//            and sends param.set ops keyed by REC event id. Watched: channel settings
//            (vol/pan/filter/pitch/route/arp...), channel plugin params, mixer vol/pan/stereo/mute/EQ,
//            mixer effect params + slot on/mix, tempo/swing/main pitch; rescanned every few
//            seconds for new channels/plugins
//  - PLUGINS: each channel / mixer FX slot is a "unit"; its full state (FL .fst via
//            FL's own preset saver) is sent on plugin add/replace, preset-sized param
//            bursts, or hash changes found by a slow sweep (e.g. Serum wavetables), and
//            applied with FL's preset loader. Channel add/delete are replayed too.
//  - JOIN: a member sends state.req; the leader answers with plug.full + state.full
//  - APPLY: inbound param.set is queued and executed on FL's main thread
//           (WH_GETMESSAGE hook) via ProcessRECEvent(REC_UpdateValue|REC_UpdateControl)
//  - LOCKS: a param is locked while we tweak it, released after a short idle;
//           params locked by a peer are snapped back if tweaked locally
//  - PATTERNS: insert / clone / delete / split are hooked (TFLProject vmt) and replayed as calls
//  - FOCUS: which FL window each person last used, shown in the status
//  - FL ADDRESSES: found by signature in FLEngine_x64.dll at start (fl_addrs.h, "FL addresses"); sync
//         stays off in a build where they aren't found
//  - BIG: ops over 32 KB (projects, plugin states, join answers) go through the server's blob store
//         over HTTP; the WebSocket carries a reference ("big messages")
//  - NET: WinHTTP WebSocket (ws:// or wss://). Not connected at start: flim.exe's window connects
//         and disconnects, and shows the status, transfers and a log (WM_COPYDATA, "flim window
//         link"); nothing is drawn into FL's own UI except other people's mouse arrows.
// Config: user name from flim.ini (set by flim.exe). Env overrides for dev/tests:
//         FLIM_NAME, FLIM_ROOM (default FLIM), FLIM_AUTOCONNECT=<url> (+ FLIM_PASSWORD) connects
//         at startup; legacy FLIM_SERVER=host:port means FLIM_AUTOCONNECT=ws://host:port.
//         Tests only: FLIM_SAMPLE_NOLOCAL=1 (always download samples), FLIM_CURSOR_NOCOVER=1 (show
//         other people's arrows even where another window covers FL).
// Logs to %TEMP%\flim_engine.log.

#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <shlobj.h>
#include "../common/flim_config.h"
#include "../common/flim_zip.h"
#include "../common/flim_res.h"
#include "../common/flim_version.h"
#include <bcrypt.h>
#include <dbghelp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

#define FL_MODULE    L"FLEngine_x64.dll"
#define IMG_BASE     0x400000      // image base of the VAs below; runtime addr = module + (VA - IMG_BASE)
// FL's functions and globals: found by signature in FLEngine_x64.dll at start (fl_addrs.h, "FL addresses"
// below), or FL FLIM_FL_BUILD's own VAs. The *_VA defines name them.
enum {
#define FLA(n, va, off, nx, pat) FA_##n,
#include "fl_addrs.h"
#undef FLA
    FA_N };
static uint32_t g_va[FA_N] = {
#define FLA(n, va, off, nx, pat) va,
#include "fl_addrs.h"
#undef FLA
};
#define FLVA(n)      g_va[FA_##n]
static char    g_syncOff[1024];    // "" = sync on; else why it's off (FL addresses not found: "FL addresses" below)
static wchar_t g_flBuild[32];      // FLEngine_x64.dll's file version
#define REC_RVA      (FLVA(REC) - IMG_BASE) // ProcessRECEvent(eventId, value, flags)
#define MUTE_RVA     (FLVA(MUTE) - IMG_BASE) // setTrackEnabled(track, enabled: 1/0, -1 toggle)
#define MIXARR_VA    FLVA(MIXARR)  // global: pointer to mixer track array (502 records, stride MIX_STRIDE)
#define MIXCNT_VA    FLVA(MIXCNT)  // global: pointer to mixer track count (int)
#define MAINFORM_VA  FLVA(MAINFORM) // global: pointer to FL main form pointer
#define MIX_STRIDE   0x1478
#define MIX_ENABLED  0x18          // byte: 1 = enabled (unmuted)
#define MIX_SOLO     0x1a          // byte: 1 = this track is soloed
#define MIX_PRESOLO  0x1c          // byte: its MIX_ENABLED from before a solo, put back on unsolo

#define REC_UpdateValue   0x01
#define REC_GetValue      0x02
#define REC_UpdateControl 0x10
#define REC_NONE          0xffffffffu  // returned for a nonexistent target

#define MAX_CH       1024          // channels synced (a big project had 306; REC ids allow ~28000)
#define NSUB         (int)(sizeof(SUBS) / sizeof(SUBS[0]))
#define MAX_SLOTS    65536
#define HASH_SIZE    (MAX_SLOTS * 2)
#define MAX_PLUG_PARAMS 4096
#define MAX_MIX      512
#define MAX_FX_SLOTS 10
#define RESCAN_MS    2000

// REC id layout (FL ProcessRECEvent; names from FL's own Shared\Python\Lib\midi.py):
//   channel param        (ch << 16) | sub              sub: see SUBS (vol, pan, filter, pitch, route, arp...)
//   channel plugin param (ch << 16) | (0x8000 + p)
//   mixer track param    0x70000000 + (t << 22) + sub  sub: see MIX_SUBS (vol, pan, stereo, EQ)
//   mixer effect param   0x70000000 + (((t << 6) | slot) << 16) + 0x8000 + p
//   mixer slot on/mix    0x70000000 + (((t << 6) | slot) << 16) + 0x1f00 / 0x1f01  (REC_Plug_Mute / MixLevel)
//   global               0x40000000 + sub              sub: 0 main volume, 1 swing, 2 main pitch, 5 tempo
// Mixer mute is not a REC param; it gets a pseudo id MUTE_ID(t) handled separately.
#define REC_PLUG_FIRST 0x8000
#define MIX_ID(t, sub)        (0x70000000u + ((uint32_t)(t) << 22) + (sub))
#define FX_ID(t, slot, p)     (0x70000000u + ((((uint32_t)(t) << 6) | (slot)) << 16) + REC_PLUG_FIRST + (p))
#define SLOTGEN_ID(t, slot, k) (0x70000000u + ((((uint32_t)(t) << 6) | (slot)) << 16) + 0x1f00 + (k))
#define GLOBAL_ID(sub)        (0x40000000u + (sub))
#define MUTE_ID(t)            (0xF0000000u | (uint32_t)(t))
#define IS_MUTE_ID(id)        (((id) & 0xFFF00000u) == 0xF0000000u)
// A channel's mixer target (FL's channels.setTargetFxTrack writes it): a param whose VALUE is a mixer track
// number. Mixer track changes renumber it (FL does, on every FL); a value sent before one is mapped through it.
#define CH_TARGET_SUB         8
#define IS_CH_TARGET(id)      ((id) < 0x70000000u && ((id) >> 16) < MAX_CH && ((id) & 0xFFFFu) == CH_TARGET_SUB)
#define POLL_MS      30
#define LOCK_IDLE_MS 600           // release a param lock after this much idle

typedef uint32_t (*rec_t)(uint32_t id, uint64_t value, uint32_t flags);

// Channel: vol, pan, filter cut/res, pitch, filter type, porta, on/off, mixer route, gate, crossfade,
// time offset, swing mix, sample offset, stretch time, the five "offset" knobs, delay (0x200+), arp (0x300+).
// Ids a channel doesn't have read back as REC_NONE and are skipped.
static const uint32_t SUBS[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19, 20,
                                 0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207,
                                 0x300, 0x301, 0x302, 0x303, 0x304, 0x305, 0x306, 0x307 };
// Mixer track: vol, pan, stereo separation, then the built-in EQ: 3 bands of gain, freq, Q.
static const uint32_t MIX_SUBS[] = { 0x1fc0, 0x1fc1, 0x1fc2,
                                     0x1fd0, 0x1fd1, 0x1fd2, 0x1fd8, 0x1fd9, 0x1fda, 0x1fe0, 0x1fe1, 0x1fe2 };
// Project-wide: swing, main pitch, tempo. The main volume (sub 0) is each person's own listening
// level, like solo: not synced.
static const uint32_t GLOBAL_SUBS[] = { 1, 2, 5 };
#define MAIN_VOL_ID GLOBAL_ID(0)

typedef void (*setenabled_t)(int track, int enabled);
static void retry_pending(void);
static void send_full_state(void);
static void send_notes_full(void);
static void notes_rebaseline(void);
static void proj_begin(const char *why);
static void clips_rebaseline(void);
static void clips_hold_retry(void);
static int  unit_before_send(const char *key, int isPat, int idx, const unsigned char *d, unsigned n, int g);
static void unit_agreed(int isPat, int idx, const unsigned char *d, unsigned n, int g);
static int  unit_base(int isPat, int idx, unsigned char **d, unsigned *n);
static int  group_diff(int isPat, const unsigned char *a, unsigned na, const unsigned char *b, unsigned nb, int *out, int cap);
static unsigned char *recs_group(int isPat, const unsigned char *d, unsigned n, int g, unsigned *on);
static unsigned rec_group(int isPat, const unsigned char *r);
static void clips_capture(int arr);
static void pat_capture(int p);
static int  pat_matches_base(int p);
static void unit_revert(const char *key);
static void unit_worked(int unit, int a, int b);
static void view_save(void);
static void view_restore(void);
static void arr_known_take(void);
static int  arr_count(void);
static char *arr_obj(int arr);
static int g_haveSong;
static int  input_recent(DWORD now);
static void units_capture_all(void);
static int  closing_now(void);
static int b64val(char c);
static int  plug_lock_send(int unit, int a, int b, char *key, int cap);
static void plug_agreed_set(int unit, int a, int b, const unsigned char *d, size_t n);
static void plug_agreed_reset(int channels, int fx);
static void plug_revert_state(int unit, int a, int b);
static void plug_undo(const char *key, int apply);
static void meta_rebaseline(void);
static void meta_rebase_type(int type);
static void meta_rebase_patterns(void);
static void meta_rebase_channels(void);
static uint64_t meta_chan_hash(int ch);
static void meta_take_channel(int ch, uint64_t before);
static void digest_tick(DWORD now);
static void pat_len_recalc(int p);
static void meta_remap(int type, const int *from, int n);
static void slots_rebase(const int *chFrom, int nch, const int *mixFrom, int nmix);
static void notes_remap(const int *from, int n);
static void chan_rebase(const int *from, int n);
static void mix_known_take(void);
static void mix_struct(void);
static int  unit_params(int unit, int a, int b);
static int  chan_list_changed(void);
static void resync(const char *why);
static void resync_asked(void);
static void notes_send_channel(int c);
static wchar_t *dstr_keep(const wchar_t *s);
static int int_after(const char *p, const char *end, const char *key, int *out);
static void ws_send(const char *msg);
static void ws_send_samples(char *msg, const unsigned char *blob, size_t n);
static void samples_rewrite(unsigned char **d, size_t *n);

typedef struct {
    uint32_t id;
    uint32_t last;          // last value seen/applied (REC_NONE = unknown/absent)
    volatile LONG gen;      // bumped on every apply; poller discards racing reads
    int  lockedByMe;
    DWORD lastTouch;        // tick of our last local change
    int  lockedByPeer;
    int  hasUndo;           // plugin param changed while its plugin lock was still requested:
    uint32_t undo;          //   value to restore if the lock is denied
    DWORD selfAt;           // last change nobody made here (automation, modulation, Patcher...)
    uint32_t echo;          // value read back right after our last apply, and when (ECHO_MS): the
    DWORD echoAt;           //   plugin's own rounding of it, not a local edit
    uint32_t applied;       // the value of that apply: a target that takes it late (mute) reaches it then
    DWORD lastSent;         // our last param.set for it (poll thread: PARAM_SEND_MS)
    int  sendPending;       // changed again sooner: its newest value (last) is still to go out
} Slot;
#define ECHO_MS 2000

static Slot   g_slots[MAX_SLOTS];
static volatile LONG g_nslot = 0;
static int    g_hash[HASH_SIZE];   // slot index + 1; 0 = empty
static setenabled_t g_setEnabled = NULL;
static char  *g_flBase = NULL;
static int    g_chanPlugN[MAX_CH];
static int    g_fxN[MAX_MIX][MAX_FX_SLOTS];
static volatile LONG g_isLeader = 0;
static volatile LONG g_syncOnly = 0;   // the host made us sync-only: we get everything, send nothing (host controls)
static volatile LONG g_syncKept = 0;   //   edits of ours it kept here meanwhile (sync_only_keeps)
static volatile LONG g_patDepth = 0;  // a pattern op is running (main thread; its dialogs pump messages): scans wait
static rec_t  g_rec = NULL;
static HINTERNET g_ws = NULL;    // the open WebSocket (NULL = not connected); guarded by g_sendLock
static CRITICAL_SECTION g_sendLock, g_slotLock, g_qLock;
static char   g_myId[64];
static HWND   g_mainWnd = NULL;
static HHOOK  g_hook = NULL;
static HINSTANCE g_hinst = NULL;

// ---- plugin / structure sync (FL functions, VAs) ----
#define SAVEPRESET_VA FLVA(SAVEPRESET) // SavePresetFile(UnicodeString file, mask, obj, flags) -> 0 ok
#define LOADPRESET_VA FLVA(LOADPRESET) // LoadPresetFile(UnicodeString file, mask, obj, flags) -> 0 ok (replaces plugin)
#define LOADPRESET_NOEDITOR 1      //   flags bit 0: don't open the plugin's editor (masks 32 / 48 do otherwise)
#define HASPLUGIN_VA  FLVA(HASPLUGIN) // HasPlugin(obj) -> bool
#define GETPLUGIN_VA  FLVA(GETPLUGIN) // GetPlugin(obj, &intf) -> plugin type; intf is AddRef'd (must release!)
#define INTFCLEAR_VA  FLVA(INTFCLEAR) // Delphi _IntfClear(&intf): nil it and Release
#define ADDCHAN_VA    FLVA(ADDCHAN) // AddChannel(-1, 0, 1) -> new channel obj (appended)
#define DELCHAN_VA    FLVA(DELCHAN) // DeleteChannel(index, 0, flags); flags 1|2 = no confirmation dialog
#define DELCHAN_FLAGS 3            // bit 2 = "already confirmed" (FL's menu sets it after its own dialog)
#define RESOLVER_VA   FLVA(RESOLVER) // ResolveChannel(index, global, &obj): global 0 = index in the rack's filter group
#define CHANLIST_VA   FLVA(CHANLIST) // global: pointer to channel list (items +8, count +0x10)
#define MIX_SLOTS_OFF 0x134c       // mixer track: 10 effect slot object pointers
#define MOVECHAN_VA   FLVA(MOVECHAN) // ReorderChannels(**MOVEOBJ, mode, **MOVEARG, flags): FL's Alt+Up/Down + sorts
#define MOVEOBJ_VA    FLVA(MOVEOBJ) //   first arg (pointer global, deref twice)
#define MOVEARG_VA    FLVA(MOVEARG) //   third arg (int, deref twice; -1 seen)
#define MOVE_UP       4            //   mode 4 = move selected up, 5 = down, 1..3 = sorts
#define MOVE_FLAGS    1            //   bit 1 = close undo group; bit 2 = take selection from the rack UI (not wanted)
#define CHANFILT_VA   FLVA(CHANFILT) // global: channel list of the rack's current filter group (same layout)
#define CH_SELECTED   0x32c        // channel object: selected byte (what ReorderChannels moves)
// ---- notes (per pattern: a flat array of 24-byte records, same layout as FLP event 224) ----
#define PATARR_VA     FLVA(PATARR) // global: pointer to the static pattern table (stride 0xC0)
#define PAT_STRIDE    0xC0
#define PAT_NOTES     0x28         // pattern record: note array object (NULL until the pattern has had notes)
#define MAX_PAT       1000         // FL caps pattern numbers at 999
#define NOTES_DATA    0x08         // note array: record pointer
#define NOTES_COUNT   0x14         //             record count (u32); element size (0x18) at +0x10
#define NOTE_SIZE     24
#define NOTE_CH_OFS   0x06         // note record: u16 channel (FLP event 224 "rack channel")
#define NOTE_SEL_OFS  0x13         // byte: low bits = note color, bit 0x80 = SELECTED (client-only, never synced)
#define NOTE_SEL_BIT  0x80
#define NOTEARR_VA    FLVA(NOTEARR) // NoteArray(pattern, create) -> note array object
#define NOTEDEL_VA    FLVA(NOTEDEL) // DeleteNote(arr, index, 0)
#define NOTEADD_VSLOT 1            // arr->vmt[1](arr, &rec): add a note (vmt[2] fills a default record)
#define NOTELOCK_VA   FLVA(NOTELOCK) // global: CRITICAL_SECTION* FL holds while editing notes (audio thread reads them)
#define GRIDREFRESH_VA FLVA(GRIDREFRESH) // RefreshGrid(channel->grid at CH_GRID)
#define CH_GRID       0x7ac
#define MASK_CHAN     32           // preset mask for channels (what FL's undo uses)
#define MASK_FX       48           // preset mask for mixer slots (32 faults on a slot)

// ---- project files (FL functions / globals, VAs) ----
#define FLPROJ_VA     FLVA(FLPROJ) // global -> holder; *(holder + 0x10) = TFLProject object
#define FLPROJ_LOAD_SLOT (0x40 / 8) // TFLProject vmt +0x40: LoadFile(self, path, flags) -> bool; flags bit 0 = "save changes?" prompt
#define MAINOBJ_VA    FLVA(MAINOBJ) // global -> var -> main form object (what FL's own open/save code passes)
#define FORM_BUSY     0x34d4       // main form: busy object, non-NULL while an .flp load runs
#define SETPROJ_VA    FLVA(SETPROJ) // SetProjectFile(form, path, path, addRecent): title, Ctrl+S target, recent list
#define SAVEPROJ_VA   FLVA(SAVEPROJ) // SaveProjectTo(path, 0) -> bool: FL's project writer; current name unchanged
#define BUSYBEGIN_VA  FLVA(BUSYBEGIN) // FL's open handler wraps an .flp load in these two
#define BUSYEND_VA    FLVA(BUSYEND)
#define PROJNAME_VA   FLVA(PROJNAME) // UnicodeString global: current project file ('' = untitled)
#define WM_FL_OPENPROJ 0x566       // FL posts this to its main form to open a project (dialog, browser, drop)
// ---- playlist clips (current arrangement) ----
#define ARRS_VA       FLVA(ARRS)   // global -> arrangement pointer array (count as int64 at -8)
#define ARRCUR_VA     FLVA(ARRCUR) // int global: current arrangement index
#define ARR_CLIPS     0x14         // arrangement: clip array object (same array family as notes)
#define CLIPS_DATA    0x08
#define CLIPS_COUNT   0x14
#define CLIP_SIZE     0x58         // first 32 bytes = FLP event 233 record
#define CLIP_DEL_OFS  0x04         // -1 = deleted (then purge)
#define CLIP_SEL_OFS  0x13         // bit 0x80 = SELECTED (client-only)
#define CLIP_SEL_BIT  0x80
#define CLIP_ID_OFS   0x20         // per-instance clip id (FL renumbers on add)
#define CLIPADD_VSLOT   1          // arr->vmt[1](arr, &rec) -> index (kept sorted by position)
#define CLIPPURGE_VSLOT 3          // arr->vmt[3](arr): drop records marked deleted, free their resources
#define PLFORM_VA     FLVA(PLFORM) // global -> var -> playlist form
#define PLREFRESH_VA  FLVA(PLREFRESH) // RefreshPlaylist(form): what FL's clip-properties dialog calls after an edit
#define HINT_VA       FLVA(HINT)   // ShowHint(form, UnicodeString text, 0): FL's hint bar (top left)
#define PROJ_FX_MASS  3            // this many effects gone in one scan = project close, not a delete
#define PROJ_CH_MASS  4            // this many channels / mixer tracks gone in one scan, and most of them: the project

typedef int   (*presetfile_t)(const wchar_t *file, int mask, void *obj, int flags);
typedef char  (*hasplugin_t)(void *obj);
typedef void *(*getplugin_t)(void *obj, void **inst);
typedef void  (*intfclear_t)(void **intf);
typedef void *(*addchan_t)(int a, int b, int c);
typedef void  (*delchan_t)(unsigned idx, char force, uint64_t flags);
typedef int   (*resolver_t)(int idx, char useGlobal, void **out);
typedef void  (*movechan_t)(void *obj, uint8_t mode, int arg, uint64_t flags);
typedef void *(*notearr_t)(int pattern, char create);
typedef void  (*notedel_t)(void *arr, int index, char flag);
typedef void  (*noteadd_t)(void *arr, const void *rec);
typedef void  (*gridrefresh_t)(void *grid);
typedef char  (*projload_t)(void *self, const wchar_t *path, uint8_t flags);
typedef void  (*setproj_t)(void *form, const wchar_t *p1, const wchar_t *p2, uint8_t addRecent);
typedef char  (*saveproj_t)(const wchar_t *path, uint8_t flag);
typedef void  (*formfn_t)(void *form);
typedef int   (*clipadd_t)(void *arr, const void *rec);
typedef void  (*clippurge_t)(void *arr);
typedef void  (*hint_t)(void *form, const wchar_t *text, uint8_t flag);

static presetfile_t g_savePreset, g_loadPreset;
static int loadpreset_call(presetfile_t fn, const wchar_t *file, int mask, void *obj, int flags);
static hasplugin_t  g_hasPlugin;
static getplugin_t  g_getPlugin;
static intfclear_t g_intfClear;
static addchan_t    g_addChannel;
static delchan_t    g_delChannel;
static resolver_t   g_resolve;
static movechan_t   g_moveChans;
static notearr_t    g_noteArr;
static notedel_t    g_noteDel;
static gridrefresh_t g_gridRefresh;

// ---- main-thread task queue ----
// Param writes are cheap and run back to back; anything that calls into FL's
// preset/channel code is "heavy" and runs one per message-loop tick (a plugin
// often needs the message loop to run between load and the next call).
enum { T_PARAM, T_SNAP, T_LOAD, T_ADDCH, T_DELCH, T_STRUCT, T_FULL, T_ORDER, T_FXCLEAR, T_FXSYNC,
       T_NOTESNAP, T_NOTES, T_NOTESYNC, T_PROJLOAD, T_PROJDONE, T_PROJSNAP,
       T_CLIPSNAP, T_CLIPS, T_UNITREVERT, T_HINT, T_METASNAP, T_META, T_PATOP, T_RELOADU, T_PLUGFOCUS,
       T_MIXSTRUCT, T_NAMEPROJ, T_PCOUNT, T_ARRSTRUCT, T_ARROP };
enum { U_CH, U_FX };
enum { SNAP_BASELINE, SNAP_IF_DIFF, SNAP_FORCE, SNAP_COLLECT, SNAP_RESEND };   // RESEND: FORCE, for links_resend_into
#define QCAP 16384
typedef struct {
    int type;
    uint32_t id, v;          // T_PARAM
    int unit, a, b;          // unit kind + (channel) or (track, slot)
    int mode;                // T_SNAP mode / T_LOAD: 1 = skip if identical / T_PARAM: 1 = someone else's edit,
                             // 2 = a mute put back after FL's fade, unless another value was applied since (MUTE_FADE_MS) /
                             // T_ADDCH, T_DELCH: the sender's channel count before it (-1: not sent)
    char *path;              // T_LOAD / T_ADDCH: inbound .fst file (owned; deleted after use)
    int *order;              // T_ORDER: [n, from0..from(n-1)]; T_FXSYNC/T_NOTESYNC: bitmap;
                             // T_NOTES: note records, it->mode = count (owned)
    HWND wnd;                // T_PLUGFOCUS: the plugin window the user worked in
    void *chobj;             // channel tasks: the channel object at index a when queued (see run_task)
    int peer;                // someone else's op: channel index a is in the list as everyone has it (g_chKnown)
                             // when it runs, after the ops before it
    LONG gen;                // g_structGen when queued: a state check queued before channels moved is stale
    int sc[3];               // someone else's op: the sender's structure counts when it sent it (-1: none; sc_rebase)
} QItem;
static void run_task_inner(const QItem *it);
static QItem g_q[QCAP];
static int g_qh = 0, g_qt = 0;
static __declspec(thread) int t_peerMsg;   // inside handle_msg: what gets queued is someone else's op
enum { SC_CH, SC_MIX, SC_PAT, SC_N };       // what a structure op renumbers: channels, mixer tracks, patterns
static __declspec(thread) int t_msgSc[SC_N] = { -1, -1, -1 };   // ... and the "sc" that op carried
// Structure counts, one per kind: how many channel (chan.add / chan.del / chan.order), mixer track (mix.struct)
// and pattern (pat.op) structure ops this FL has applied, its own included. Ops of one kind are applied in the
// same order everywhere, so in-step FLs count alike; each op carries its sender's counts ("sc":[ch,mix,pat]).
// (One count for all kinds matched on two FLs whose histories differed once kinds applied in either order.)
// Main thread changes them (sc_done). -1: not known (a member until the leader's project comes; an older
// leader's project has none).
static volatile LONG g_sc[SC_N] = { 0, 0, 0 };
// Project number: the leader counts the projects it sends (send_project), a member takes the number of the one
// it got (on_project_load); every op carries its sender's ("ep"). An op made before the project an FL has (or is
// loading) is dropped there: the leader applied a member's edit after its snapshot while the member's load
// wiped it, and the two differed until the safety net. Now both lose that one edit.
static volatile LONG g_projEpoch = 0;
static volatile LONG g_showCursors = 1;    // flim's window: "Show cursors" (flim.ini show_cursors)
static __declspec(thread) int t_msgEp = -1;
static void sc_done(int kind, const int *from, int n);
static void sc_done_patop(int k, const int *pats, int n, int delta, int target);
static void pat_sigs_take(int k, const int *pats, int n);
static unsigned char g_patPreCh[MAX_PAT];   // split: channels with notes in each source (pat_sigs_take)
static uint32_t g_patPreH[MAX_PAT];         // split: each source's notes, in any order (selection ignored)
static void ulock_release_moved(const char *pre1, const char *pre2, const int *from, int n);
static void send_lock(const char *t, uint32_t id);
static void sc_done_del(int k, int n0);
static uint32_t sc_mix_map_value(uint32_t v, int from);
static void sc_done_add(int k, int n0);
static volatile LONG g_peerQueued = 0;     // such tasks still waiting in the queue
static volatile LONG g_structGen;          // (with the units below)
// The detailed log (flim.ini log_detailed, "Detailed log" in flim's window): every op sent and received,
// and a digest of the project's structure every DIGEST_MS. Two people's logs side by side then show
// where they stopped matching (ops in carry the server's sequence number: same order for everyone).
static volatile LONG g_logDetailed = 0;

static void gui_log(const char *line);
static void logmsg(const char *fmt, ...) {
    char path[MAX_PATH], line[8192];
    va_list ap; va_start(ap, fmt); _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap); va_end(ap);
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, MAX_PATH, "flim_engine.log");
    {   // one append per line, shared: other threads (and other FLs) write the same file at the same time
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char out[8300];
            SYSTEMTIME t; GetLocalTime(&t);
            int n = _snprintf_s(out, sizeof(out), _TRUNCATE, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), line);
            DWORD w;
            if (n < 0) n = (int)strlen(out);
            WriteFile(f, out, (DWORD)n, &w, NULL);
            CloseHandle(f);
        }
    }
    gui_log(line);
}

// Detailed log: an op sent or received, its data cut down to its size. The arrows, the leader's
// "alive" and who-is-where would drown everything else out.
static void log_op(const char *dir, const char *m) {
    static const char *const QUIET[] = { "\"kind\":\"cur\"", "\"kind\":\"alive\"", "\"kind\":\"focus\"", "\"kind\":\"sel\"" };
    char s[480];
    size_t o = 0;
    const char *p = m;
    int i;
    if (!g_logDetailed) return;
    for (i = 0; i < 3; i++) if (strstr(m, QUIET[i])) return;
    while (*p && o + 32 < sizeof(s)) {
        if (strncmp(p, "\"data\":\"", 8) == 0) {
            const char *e = strchr(p + 8, '"');
            if (!e) break;
            o += sprintf_s(s + o, sizeof(s) - o, "\"data\":<%zu>", (size_t)(e - p - 8));
            p = e + 1;
        } else s[o++] = *p++;
    }
    s[o] = 0;
    logmsg("OP %s %s%s", dir, s, *p ? " ..." : "");
}

static unsigned hash_id(uint32_t id) { return (id * 2654435761u) % HASH_SIZE; }

static Slot *slot_by_id(uint32_t id) {
    Slot *r = NULL;
    unsigned h = hash_id(id);
    EnterCriticalSection(&g_slotLock);
    while (g_hash[h]) {
        if (g_slots[g_hash[h] - 1].id == id) { r = &g_slots[g_hash[h] - 1]; break; }
        h = (h + 1) % HASH_SIZE;
    }
    LeaveCriticalSection(&g_slotLock);
    return r;
}

static int mix_count(void) {
    int n;
    __try { n = **(int **)(g_flBase + (MIXCNT_VA - IMG_BASE)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n < 0 ? 0 : (n > MAX_MIX ? MAX_MIX : n);
}

// ProcessRECEvent dereferences FL's main form (*(*MAINFORM_VA) + 0xf30) first thing;
// it is null until FL has finished creating its UI. Calling REC before that crashes FL.
static int fl_ready(void) {
    __try {
        char **pl = *(char ***)(g_flBase + (MAINFORM_VA - IMG_BASE));
        if (!pl || !*pl) return 0;
        return *(void **)(*pl + 0xf30) != NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Solo is each person's own listening. FL solos by muting the other tracks (MIX_SOLO set on the
// soloed one, every track's own state kept in MIX_PRESOLO until the unsolo), so while anything is
// soloed here, a track's mute as the others know it is its MIX_PRESOLO.
static int solo_scan(void) {
    int t, n = mix_count();
    __try {
        char *arr = *(char **)(g_flBase + (MIXARR_VA - IMG_BASE));
        for (t = 0; t < n; t++) if (arr[(size_t)t * MIX_STRIDE + MIX_SOLO]) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}
static int solo_on(void) {                 // solo_scan at most once per clock tick: the poll reads every track's mute
    static volatile DWORD at;
    static volatile int on;
    DWORD now = GetTickCount();
    if (now != at) { on = solo_scan(); at = now; }
    return on;
}
static uint32_t mute_get(int t, int solo) {
    __try { return (*(char **)(g_flBase + (MIXARR_VA - IMG_BASE)))[(size_t)t * MIX_STRIDE + (solo ? MIX_PRESOLO : MIX_ENABLED)] ? 1u : 0u; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return REC_NONE; }
}

// The channel rack's solo is each person's own too. FL's toggle (sub_FFAAA0, chsolo_detour) turns the other
// channels off, keeping each one's on/off from before in CH_PRESOLO (2 on, 1 off) for the unsolo; a solo is FL's
// shape (one channel on, the rest off, CH_SOLOSKIP aside), and CH_PRESOLO stays behind after it. So flim takes a
// solo as on from the toggle until that shape is gone, and meanwhile a channel's on/off as the others know it is
// its CH_PRESOLO.
#define CH_ON        0x32e         // byte: the channel is on (FL's isChannelSolo reads it)
#define CH_PRESOLO   0x891         // byte: its on/off from before a solo, put back on unsolo: 2 on, 1 off, 0 none
#define CH_SOLOSKIP  0x892         // byte: solo leaves this channel alone
#define IS_CH_MUTE(id) ((id) < 0x70000000u && ((id) >> 16) < MAX_CH && ((id) & 0xFFFFu) == 7)   // REC_Chan_Mute: on/off
static volatile LONG g_chSolo;             // a channel solo is on here
static volatile DWORD g_chSoloAt;          // when it last went on or off (FL's loop over the channels: reads wait)
static int chan_list(void ***items);
static char *chan_at(int i) {
    void **items;
    int n = chan_list(&items);
    return items && i >= 0 && i < n ? (char *)items[i] : NULL;
}
static int chsolo_shape(void) {            // FL's sub_FFAA40: some channel is soloed
    void **items;
    int n = chan_list(&items), i, on = 0;
    if (!items) return 0;
    __try {
        for (i = 0; i < n; i++) if (!((char *)items[i])[CH_SOLOSKIP] && ((char *)items[i])[CH_ON] && ++on > 1) return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return on == 1;
}
static uint32_t chmute_presolo(int ch, uint32_t cur) {   // its shared on/off while soloing here
    char *c = chan_at(ch), p;
    if (!c) return REC_NONE;
    __try { p = c[CH_PRESOLO]; } __except (EXCEPTION_EXECUTE_HANDLER) { return REC_NONE; }
    return p ? (p == 2 ? 1u : 0u) : cur;   // (0: one added since the solo began)
}

// Current value of any synced id (REC param or pseudo id). REC_NONE if absent.
// SEH-guarded so a bad moment inside FL (e.g. mid project load) can't take FL down.
static uint32_t read_value(uint32_t id) {
    __try {
        if (IS_MUTE_ID(id)) {
            int t = (int)(id & 0xFFFFF);
            if (t >= mix_count()) return REC_NONE;
            return mute_get(t, solo_on());
        }
        if (g_chSolo && IS_CH_MUTE(id)) {
            uint32_t v = g_rec(id, 0, REC_GetValue);
            return v == REC_NONE ? v : chmute_presolo((int)(id >> 16), v);
        }
        return g_rec(id, 0, REC_GetValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return REC_NONE;
    }
}

// Must run on FL's main thread.
static uint32_t write_value(uint32_t id, uint32_t v) {
    __try {
        if (IS_MUTE_ID(id)) {
            int t = (int)(id & 0xFFFFF), solo = solo_scan();
            if (t >= mix_count()) return REC_NONE;
            // soloing here: heard after the unsolo (FL's mute would also end the solo, leaving the rest muted)
            if (solo) (*(char **)(g_flBase + (MIXARR_VA - IMG_BASE)))[(size_t)t * MIX_STRIDE + MIX_PRESOLO] = v ? 1 : 0;
            else g_setEnabled(t, v ? 1 : 0);
            return mute_get(t, solo);
        }
        if (g_chSolo && IS_CH_MUTE(id)) {          // soloing here: heard after the unsolo
            char *c = chan_at((int)(id >> 16));
            if (!c) return REC_NONE;
            c[CH_PRESOLO] = v ? 2 : 1;
            return v ? 1u : 0u;
        }
        return g_rec(id, v, REC_UpdateValue | REC_UpdateControl);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logmsg("APPLY faulted id=0x%08x", id);
        return REC_NONE;
    }
}

// Register an id for syncing; returns 1 if it exists in FL (value != REC_NONE).
static int add_slot(uint32_t id) {
    if (slot_by_id(id)) return 1;
    uint32_t v = read_value(id);
    if (v == REC_NONE) return 0;
    EnterCriticalSection(&g_slotLock);
    if (slot_by_id(id)) { LeaveCriticalSection(&g_slotLock); return 1; }   // (the main thread's watch_* and rescan)
    if (g_nslot < MAX_SLOTS) {
        Slot *s = &g_slots[g_nslot];
        memset(s, 0, sizeof(*s));
        s->id = id;
        s->last = v;
        unsigned h = hash_id(id);
        while (g_hash[h]) h = (h + 1) % HASH_SIZE;
        g_hash[h] = g_nslot + 1;
        InterlockedIncrement(&g_nslot);   // publish after the slot is filled
    } else {
        static volatile LONG told = 0;
        if (!InterlockedExchange(&told, 1)) logmsg("PARAMS all %d watch slots in use: params found from now on aren't synced (id 0x%08x)", MAX_SLOTS, id);
    }
    LeaveCriticalSection(&g_slotLock);
    return 1;
}

// A track / channel just inserted (here or by a peer): its values watched at once, from FL's defaults. Left to the
// poll thread's rescan (which waits while the main thread is in a flim task), a value set meanwhile became its start
// and was never sent (live_test stress: a volume on a new track stayed on one FL). Its plugins come with rescan.
static void watch_track(int t) {
    int k, sl;
    for (k = 0; k < (int)(sizeof(MIX_SUBS) / sizeof(MIX_SUBS[0])); k++) add_slot(MIX_ID(t, MIX_SUBS[k]));
    add_slot(MUTE_ID(t));
    for (sl = 0; sl < MAX_FX_SLOTS; sl++) { add_slot(SLOTGEN_ID(t, sl, 0)); add_slot(SLOTGEN_ID(t, sl, 1)); }
}
static void watch_channel(int c) {
    int k;
    for (k = 0; k < NSUB; k++) add_slot(((uint32_t)c << 16) | SUBS[k]);
}
// Discover channels, plugins and mixer slots; extend param ranges that grew.
// Cheap enough to run every few seconds (only probes past known ranges).
static void rescan(void) {
    LONG before = g_nslot;
    int c, k, t, sl;
    for (c = 0; c < MAX_CH; c++) {
        if (read_value((uint32_t)c << 16) == REC_NONE) continue;   // no such channel
        int np = unit_params(U_CH, c, 0);
        for (k = 0; k < NSUB; k++) add_slot(((uint32_t)c << 16) | SUBS[k]);
        while (g_chanPlugN[c] < np &&
               add_slot(((uint32_t)c << 16) | (REC_PLUG_FIRST + g_chanPlugN[c])))
            g_chanPlugN[c]++;
    }
    int nm = mix_count();
    for (t = 0; t < nm; t++) {
        for (k = 0; k < (int)(sizeof(MIX_SUBS) / sizeof(MIX_SUBS[0])); k++) add_slot(MIX_ID(t, MIX_SUBS[k]));
        add_slot(MUTE_ID(t));
        for (sl = 0; sl < MAX_FX_SLOTS; sl++) {
            add_slot(SLOTGEN_ID(t, sl, 0));                  // effect slot on/off
            add_slot(SLOTGEN_ID(t, sl, 1));                  //             mix level
            int np = unit_params(U_FX, t, sl);
            while (g_fxN[t][sl] < np && add_slot(FX_ID(t, sl, g_fxN[t][sl])))
                g_fxN[t][sl]++;
        }
    }
    for (k = 0; k < (int)(sizeof(GLOBAL_SUBS) / sizeof(GLOBAL_SUBS[0])); k++) add_slot(GLOBAL_ID(GLOBAL_SUBS[k]));
    if (g_nslot != before)
        logmsg("rescan: watching %ld params (+%ld), mixer tracks=%d", g_nslot, g_nslot - before, nm);
}

static int rebase_slot_id(uint32_t id, int chMap, const int *chToNew, int mixMap, const int *mixToNew, uint32_t *nid) {
    *nid = id;
    if (IS_MUTE_ID(id)) {
        int t = (int)(id & 0xFFFFFu), nt;
        if (!mixMap || t >= MAX_MIX) return 1;
        nt = mixToNew[t];
        if (nt < 0) return 0;
        *nid = 0xF0000000u | (uint32_t)nt;
        return 1;
    }
    if (id >= 0x70000000u && id < 0xF0000000u) {
        uint32_t rel = id - 0x70000000u, sub = rel & 0xFFFFu;
        int t = (int)(rel >> 22), slot = (int)((rel >> 16) & 63u), nt;
        if (!mixMap || t >= MAX_MIX) return 1;
        nt = mixToNew[t];
        if (nt < 0) return 0;
        if (slot == 0 && sub >= 0x2000u && sub < 0x2000u + MAX_MIX) {
            int dst = (int)(sub - 0x2000u), ndst = mixToNew[dst];
            if (ndst < 0) return 0;
            sub = 0x2000u + (uint32_t)ndst;
        }
        *nid = 0x70000000u + (((((uint32_t)nt << 6) | (uint32_t)slot) << 16) + sub);
        return 1;
    }
    if (id < 0x70000000u && (id >> 16) < MAX_CH) {
        int ch = (int)(id >> 16), nch;
        if (!chMap) return 1;
        nch = chToNew[ch];
        if (nch < 0) return 0;
        *nid = ((uint32_t)nch << 16) | (id & 0xFFFFu);
        return 1;
    }
    return 1;
}

static void slots_rebase(const int *chFrom, int nch, const int *mixFrom, int nmix) {
    int chToNew[MAX_CH], mixToNew[MAX_MIX], oldN, w = 0, i, j;
    Slot ns;
    uint32_t nid;
    for (j = 0; j < MAX_CH; j++) chToNew[j] = -1;
    for (j = 0; j < MAX_MIX; j++) mixToNew[j] = -1;
    if (chFrom) for (j = 0; j < nch; j++) if (chFrom[j] >= 0 && chFrom[j] < MAX_CH) chToNew[chFrom[j]] = j;
    if (mixFrom) for (j = 0; j < nmix; j++) if (mixFrom[j] >= 0 && mixFrom[j] < MAX_MIX) mixToNew[mixFrom[j]] = j;
    EnterCriticalSection(&g_slotLock);
    oldN = (int)g_nslot;
    memset(g_hash, 0, sizeof(g_hash));
    for (i = 0; i < oldN; i++) {
        Slot *s = &g_slots[i];
        if (!rebase_slot_id(s->id, chFrom != NULL, chToNew, mixFrom != NULL, mixToNew, &nid)) continue;
        ns = *s;
        ns.id = nid;
        if (mixFrom && IS_CH_TARGET(nid) && ns.last != REC_NONE)   // FL renumbered the target too: not an edit
            ns.last = ns.last < MAX_MIX && mixToNew[ns.last] >= 0 ? (uint32_t)mixToNew[ns.last] : 0;
        InterlockedIncrement(&ns.gen);
        ns.echoAt = 0;
        g_slots[w] = ns;
        unsigned h = hash_id(nid);
        while (g_hash[h] && g_slots[g_hash[h] - 1].id != nid) h = (h + 1) % HASH_SIZE;
        if (g_hash[h]) continue;
        g_hash[h] = w + 1;
        w++;
    }
    for (i = w; i < oldN; i++) memset(&g_slots[i], 0, sizeof(g_slots[i]));
    g_nslot = w;
    LeaveCriticalSection(&g_slotLock);
}
// Poll thread: v (read under generation g0) is what this slot is now, unless an apply or a rebase came between.
static void slot_take(Slot *s, LONG g0, uint32_t v) {
    EnterCriticalSection(&g_slotLock);
    if (s->gen == g0) s->last = v;
    LeaveCriticalSection(&g_slotLock);
}
// No map to carry them through (a project load, an order FL didn't follow): every value is taken as it is.
static void slots_reread(void) {
    int k;
    for (k = 0; k < g_nslot; k++) {
        EnterCriticalSection(&g_slotLock);
        g_slots[k].last = read_value(g_slots[k].id);
        g_slots[k].lockedByMe = 0;
        InterlockedIncrement(&g_slots[k].gen);
        LeaveCriticalSection(&g_slotLock);
    }
}

// ---------------- websocket ----------------
static void b64enc(const unsigned char *in, int n, char *out) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, o = 0;
    for (i = 0; i < n; i += 3) {
        int a = in[i], b = (i + 1 < n) ? in[i + 1] : 0, c = (i + 2 < n) ? in[i + 2] : 0;
        out[o++] = T[a >> 2];
        out[o++] = T[((a & 3) << 4) | (b >> 4)];
        out[o++] = (i + 1 < n) ? T[((b & 15) << 2) | (c >> 6)] : '=';
        out[o++] = (i + 2 < n) ? T[c & 63] : '=';
    }
    out[o] = 0;
}

// One text message, now. WinHTTP allows one send at a time per socket: g_sendLock serializes.
// Everything else calls ws_send (samples section), which keeps ops behind ones waiting for samples.
static void ws_send_now(const char *msg) {
    DWORD e = 0;
    EnterCriticalSection(&g_sendLock);
    if (g_ws) e = WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)msg, (DWORD)strlen(msg));
    LeaveCriticalSection(&g_sendLock);
    if (e) logmsg("send failed (%lu)", e);
}

// Server address as typed in the Connect dialog:
//   wss://host[:port][/path], ws://..., https:// / http:// (same as wss / ws), or host[:port].
// A bare host means wss on 443; localhost, 127.x or an explicit non-443 port mean plain ws.
typedef struct { int secure; wchar_t host[256]; INTERNET_PORT port; wchar_t path[512]; char shown[300]; } NetUrl;

static int parse_url(const char *in, NetUrl *u) {
    char buf[800], pathA[512] = "/", *p, *h, *slash, *colon;
    int scheme = -1;                                    // 1 = TLS, 0 = plain, -1 = not given
    size_t n;
    memset(u, 0, sizeof(*u));
    while (*in == ' ' || *in == '\t') in++;
    strncpy_s(buf, sizeof(buf), in, _TRUNCATE);
    n = strlen(buf);
    while (n && (unsigned char)buf[n - 1] <= ' ') buf[--n] = 0;
    p = buf;
    if (_strnicmp(p, "wss://", 6) == 0) { scheme = 1; p += 6; }
    else if (_strnicmp(p, "https://", 8) == 0) { scheme = 1; p += 8; }
    else if (_strnicmp(p, "ws://", 5) == 0) { scheme = 0; p += 5; }
    else if (_strnicmp(p, "http://", 7) == 0) { scheme = 0; p += 7; }
    else if (strstr(p, "://")) return 0;
    h = p;
    slash = strchr(h, '/');
    if (slash) { strncpy_s(pathA, sizeof(pathA), slash, _TRUNCATE); *slash = 0; }
    colon = strrchr(h, ':');
    if (colon) {
        char *e; long port = strtol(colon + 1, &e, 10);
        if (*e || port <= 0 || port > 65535) return 0;
        u->port = (INTERNET_PORT)port; *colon = 0;
    }
    if (!*h) return 0;
    for (p = h; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_')) return 0;
    if (scheme < 0) {
        int local = _stricmp(h, "localhost") == 0 || strncmp(h, "127.", 4) == 0;
        scheme = ((u->port && u->port != 443) || local) ? 0 : 1;
        if (local && !u->port) u->port = 8080;
    }
    u->secure = scheme;
    if (!u->port) u->port = scheme ? 443 : 80;
    MultiByteToWideChar(CP_UTF8, 0, h, -1, u->host, 256);
    MultiByteToWideChar(CP_UTF8, 0, pathA, -1, u->path, 512);
    strcpy_s(u->shown, sizeof(u->shown), h);              // status text: host[:port][/path]
    if (u->port != (scheme ? 443 : 80)) sprintf_s(u->shown + strlen(u->shown), sizeof(u->shown) - strlen(u->shown), ":%u", u->port);
    if (pathA[1]) strncat_s(u->shown, sizeof(u->shown), pathA, _TRUNCATE);
    return 1;
}

typedef struct { HINTERNET ses, con, ws; } NetConn;

static void net_close_handles(NetConn *c) {
    if (c->ws) WinHttpCloseHandle(c->ws);
    if (c->con) WinHttpCloseHandle(c->con);
    if (c->ses) WinHttpCloseHandle(c->ses);
    memset(c, 0, sizeof(*c));
}

static void net_error_text(DWORD e, const NetUrl *u, char *out, int cap) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: strcpy_s(out, cap, "server not found (check the URL)"); break;
    case ERROR_WINHTTP_CANNOT_CONNECT:    strcpy_s(out, cap, "nothing answers at that address"); break;
    case ERROR_WINHTTP_TIMEOUT:           strcpy_s(out, cap, "timed out"); break;
    case ERROR_WINHTTP_SECURE_FAILURE:
        strcpy_s(out, cap, u->secure ? "secure connection failed (a local server needs ws://)" : "secure connection failed");
        break;
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        strcpy_s(out, cap, u->secure ? "bad server response" : "bad server response (try wss://)");
        break;
    default: sprintf_s(out, cap, "network error %lu", e);
    }
}

// Open a WebSocket to the server (blocking). Returns 1 with c->ws set, or 0 + err.
static int ws_open(const NetUrl *u, NetConn *c, char *err, int errcap) {
    HINTERNET req = NULL;
    DWORD status = 0, sl = sizeof(status);
    memset(c, 0, sizeof(*c));
    c->ses = WinHttpOpen(L"flim/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->ses)   // before Windows 8.1
        c->ses = WinHttpOpen(L"flim/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->ses) goto fail;
    WinHttpSetTimeouts(c->ses, 0, 15000, 30000, 30000);          // resolve, connect, send, handshake reply
    c->con = WinHttpConnect(c->ses, u->host, u->port, 0);
    if (!c->con) goto fail;
    req = WinHttpOpenRequest(c->con, L"GET", u->path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             u->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto fail;
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) goto fail;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0)) goto fail;
    if (!WinHttpReceiveResponse(req, NULL)) goto fail;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) { sprintf_s(err, errcap, "not a flim server (HTTP %lu)", status); goto out; }
    c->ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    if (!c->ws) goto fail;
    WinHttpCloseHandle(req);      // idle connections are fine: the server pings every 15 s
    return 1;
fail:
    net_error_text(GetLastError(), u, err, errcap);
out:
    if (req) WinHttpCloseHandle(req);
    net_close_handles(c);
    return 0;
}

// ---------------- main-thread task queue ----------------
static HWND g_taskWnd = NULL;     // hidden window whose window procedure runs the queue (see main-thread apply)
static void wake_main(void) {
    if (g_taskWnd) PostMessageW(g_taskWnd, WM_APP + 0x32 /* WM_FLIM_TASKS */, 0, 0);
    else if (g_mainWnd) PostMessageW(g_mainWnd, WM_NULL, 0, 0);
}

static void enqueue(const QItem *it) {
    int i;
    EnterCriticalSection(&g_qLock);
    if (it->type == T_PARAM && it->mode == 1 && IS_MUTE_ID(it->id)) {
        // someone's newer mute for a track that has one queued takes its place: held up behind other work,
        // a mute and its unmute would run back to back, and FL's mute fade swallows the second (MUTE_FADE_MS)
        for (i = g_qh; i != g_qt; i = (i + 1) % QCAP)
            if (g_q[i].type == T_PARAM && g_q[i].id == it->id) {
                g_q[i].v = it->v; g_q[i].mode = 1;
                LeaveCriticalSection(&g_qLock);
                return;
            }
    }
    if (it->type == T_LOAD) {
        // a newer state for a unit that still has a load queued replaces it (latest wins)
        for (i = g_qh; i != g_qt; i = (i + 1) % QCAP) {
            QItem *q = &g_q[i];
            if (q->type == T_LOAD && q->unit == it->unit && q->a == it->a && q->b == it->b &&
                q->peer == (it->peer || t_peerMsg) && q->chobj == it->chobj) {
                DeleteFileA(q->path); free(q->path);
                q->path = it->path; q->mode = it->mode;
                LeaveCriticalSection(&g_qLock);
                logmsg("LOAD coalesced unit=%d %d/%d", it->unit, it->a, it->b);
                return;
            }
        }
    }
    int nt = (g_qt + 1) % QCAP;
    if (nt != g_qh) {
        QItem *q = &g_q[g_qt];
        *q = *it;
        q->peer = q->peer || t_peerMsg;
        for (i = 0; i < SC_N; i++) q->sc[i] = t_peerMsg ? t_msgSc[i] : -1;
        q->gen = g_structGen;
        if (q->peer) InterlockedIncrement(&g_peerQueued);
        g_qt = nt;
    } else logmsg("queue full, dropped task type %d", it->type);
    LeaveCriticalSection(&g_qLock);
    wake_main();
}

// A task that must run next, ahead of what is already queued (main thread, from a task).
static void enqueue_front(const QItem *it) {
    int i, nh;
    EnterCriticalSection(&g_qLock);
    nh = (g_qh - 1 + QCAP) % QCAP;
    if (nh != g_qt) {
        QItem *q = &g_q[nh];
        *q = *it;
        for (i = 0; i < SC_N; i++) q->sc[i] = -1;
        q->gen = g_structGen;
        if (q->peer) InterlockedIncrement(&g_peerQueued);
        g_qh = nh;
    } else logmsg("queue full, dropped task type %d", it->type);
    LeaveCriticalSection(&g_qLock);
    wake_main();
}

static void enqueue_apply(uint32_t id, uint32_t v, int mode) {
    QItem it; memset(&it, 0, sizeof(it));
    it.type = T_PARAM; it.id = id; it.v = v; it.mode = mode;
    enqueue(&it);
}

static int chan_list(void ***items);
static void enqueue_unit(int type, int unit, int a, int b, int mode, char *path) {
    QItem it; memset(&it, 0, sizeof(it));
    it.type = type; it.unit = unit; it.a = a; it.b = b; it.mode = mode; it.path = path;
    if (unit == U_CH && (type == T_LOAD || type == T_SNAP || type == T_RELOADU)) {
        void **items;
        int n = chan_list(&items);
        if (items && a >= 0 && a < n) it.chobj = items[a];   // which channel this is for, whatever its index later
    }
    enqueue(&it);
}
// A peer's op for a channel: its index can't be bound now, while channel ops queued before it (a burst of
// inserts) haven't run yet; task_rebind binds it when it runs.
static void enqueue_peer(int type, int unit, int a, int b, int mode, char *path) {
    QItem it; memset(&it, 0, sizeof(it));
    it.type = type; it.unit = unit; it.a = a; it.b = b; it.mode = mode; it.path = path; it.peer = 1;
    enqueue(&it);
}

// ---------------- plugin / structure sync ----------------
// A "unit" is one channel or one mixer effect slot. Its full plugin state
// (identity + preset/chunk) travels as an .fst produced by FL's own preset
// saver and is applied with FL's preset loader on the other side.
typedef struct {
    void *inst;              // plugin instance seen last (identity; NULL = none)
    uint64_t hash;           // baseline state hash (0 = unknown)
    DWORD lastAct;           // last local param activity (0 = idle)
    int burst;               // a preset-sized param burst happened in this activity
    DWORD identAt;           // identity changed locally at (send after settle)
    DWORD suppressUntil;     // remote state being applied: ignore local param changes
    DWORD absorbUntil;       // identity changes until then come from our own apply
    DWORD baselineAt;        // take a silent baseline at this tick
    DWORD baseAt;            // when the current hash baseline was taken (or a peer's state loaded)
    int editGen;             // bumped by local edits (params, sent states)
    DWORD userAt;            // someone here last worked in this plugin: its params, its window, a preset (unit_touched)
    void *pcInst;            // plugin instance pcount was counted for (plug_param_count)
    int pcount;              //   its params worth watching
    void *pcAsked;           // count requested from the main thread for this instance, at:
    DWORD pcAskedAt;
} Unit;

#define BURST_PARAMS     8       // params of one unit changing in one poll tick = preset load
#define SETTLE_ACT_MS    1000    // quiet time after param activity before re-hashing
#define SETTLE_IDENT_MS  1500    // let a new/replaced plugin finish loading before saving it
#define SUPPRESS_MS      600     // after applying remote state: late async param updates from the plugin
#define ABSORB_MS        2000    // identity changes this soon after our own apply are ours
#define REBASE_MS        1500    // silent re-baseline of a unit this long after a remote apply
#define INPUT_RECENT_MS  5000    // "someone is using FL": gate for sweeps (state sends: unit_touched)
#define SWEEP_MS         700     // one round-robin state check per this interval
#define STRUCT_MS        500     // channel list / plugin identity scan interval

static Unit g_chU[MAX_CH];
static Unit g_fxU[MAX_MIX][MAX_FX_SLOTS];
static void *g_chKnown[MAX_CH];
static int   g_nChKnown = -1;
static volatile LONG g_structGen = 0;     // bumped when channel indices shift
static volatile DWORD g_lastInput = 0;    // last local mouse/keyboard input (main thread queue)
static char  g_tmpDir[MAX_PATH];
static char  g_fxDeletePreset[MAX_PATH];  // <FL>\Data\System\(delete) effect.fst: what FL's own Delete loads into a slot
static volatile LONG g_tmpSeq = 0;

static Unit *unit_ptr(int unit, int a, int b) {
    if (unit == U_CH) return (a >= 0 && a < MAX_CH) ? &g_chU[a] : NULL;
    return (a >= 0 && a < MAX_MIX && b >= 0 && b < MAX_FX_SLOTS) ? &g_fxU[a][b] : NULL;
}

// Someone here worked in this plugin since its baseline. A saved state that differs without that is
// drift: plugins whose save is never byte-identical (and states automation moves) would otherwise go
// out whenever anyone touched anything in FL, and each one reloads the plugin on the other side.
static int unit_touched(const Unit *u) { return u->userAt && (!u->baseAt || (LONG)(u->userAt - u->baseAt) > 0); }

// Unit a REC id belongs to (for activity tracking); returns 0 if none.
static int unit_of_id(uint32_t id, int *unit, int *a, int *b) {
    if (IS_MUTE_ID(id)) return 0;
    if (id < 0x70000000u) { *unit = U_CH; *a = (int)(id >> 16); *b = 0; return *a < MAX_CH; }
    if ((id & 0xFFFF) < REC_PLUG_FIRST) return 0;            // mixer track vol/pan/stereo
    uint32_t ts = (id - 0x70000000u) >> 16;
    *unit = U_FX; *a = (int)(ts >> 6); *b = (int)(ts & 63);
    return *a < MAX_MIX && *b < MAX_FX_SLOTS;
}

// Delphi x64 UnicodeString with refcount -1 (a "literal"): FL copies rather than
// references it, so we can free it after the call.
static wchar_t *dstr_new(const char *s) {
    int n = (int)strlen(s), i;
    unsigned char *m = (unsigned char *)calloc(1, 16 + (n + 1) * 2);
    *(uint16_t *)(m + 4) = 1200; *(uint16_t *)(m + 6) = 2;
    *(int32_t *)(m + 8) = -1;    *(int32_t *)(m + 12) = n;
    wchar_t *w = (wchar_t *)(m + 16);
    for (i = 0; i < n; i++) w[i] = (unsigned char)s[i];
    return w;
}
static void dstr_free(wchar_t *w) { if (w) free((unsigned char *)w - 16); }

static void *chan_obj(int i) {
    void *o = NULL;
    __try { if (!g_resolve(i, 1, &o)) o = NULL; } __except (EXCEPTION_EXECUTE_HANDLER) { o = NULL; }   // i: full channel list
    return o;
}
static void *fx_obj(int t, int s) {
    __try {
        char *arr = *(char **)(g_flBase + (MIXARR_VA - IMG_BASE));
        return *(void **)(arr + (size_t)t * MIX_STRIDE + MIX_SLOTS_OFF + (size_t)s * 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
// A mixer track's identity: the lowest of its effect slot objects. FL's track moves carry them all along;
// moving an effect up / down swaps two of them inside the track (sub_1233B30), so slot 0's alone won't do.
static void *mix_tid(int t) {
    void *m = NULL;
    int s;
    __try {
        char *r = *(char **)(g_flBase + (MIXARR_VA - IMG_BASE)) + (size_t)t * MIX_STRIDE + MIX_SLOTS_OFF;
        for (s = 0; s < MAX_FX_SLOTS; s++) { void *o = *(void **)(r + (size_t)s * 8); if (o && (!m || o < m)) m = o; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return m;
}
static void *unit_obj(int unit, int a, int b) { return unit == U_CH ? chan_obj(a) : fx_obj(a, b); }
static int   unit_mask(int unit) { return unit == U_CH ? MASK_CHAN : MASK_FX; }

static int ptr_cmp(const void *a, const void *b) {
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

// Identity token of the plugin in obj (NULL = none). GetPlugin hands out an
// AddRef'd interface; it must be released or the plugin can never be destroyed
// (deleting its channel then hangs inside the plugin's teardown).
static void *plugin_inst(void *obj) {
    void *intf = NULL, *id = NULL;
    if (!obj) return NULL;
    __try {
        if (g_hasPlugin(obj)) g_getPlugin(obj, &intf);
        id = intf;
        if (intf) g_intfClear(&intf);
    } __except (EXCEPTION_EXECUTE_HANDLER) { id = NULL; }
    return id;
}

// How many of a plugin's params to watch. FL's VST wrapper reports a fixed 4240 (4096 VST params, then
// MIDI CCs) at PLUG_NPARAMS, and REC reads the unused ones as 0, so a project with a few VSTs filled the
// whole slot table with params that don't exist. The unused ones have no name: count up to the last
// named one, asking the plugin through its FL SDK interface (TFruityPlug::GetName, what FL's
// plugins.getParamName calls). Main thread: GetName goes into the plugin.
#define PLUG_NPARAMS       0x68       // plugin holder (channel / effect slot object): int, FL's param count
#define INTF_FRUITY_VSLOT  (0x20 / 8) // GetPlugin's interface vmt: -> the TFruityPlug
#define FRUITY_GETNAME_VSLOT (0x20 / 8) // TFruityPlug vmt: GetName(section 0 = param, index, value, char name[4096])
typedef void *(*intffruity_t)(void *intf);
typedef void (*getname_t)(void *fp, int section, int index, intptr_t value, char *name);
static int plug_param_count(void *obj) {
    static char name[4096];
    void *intf = NULL;
    int n = 0;
    __try {
        if (obj && g_hasPlugin(obj)) {
            void *fp;
            n = *(int *)((char *)obj + PLUG_NPARAMS);
            if (n > MAX_PLUG_PARAMS) n = MAX_PLUG_PARAMS;
            g_getPlugin(obj, &intf);
            fp = intf ? ((intffruity_t)(*(void ***)intf)[INTF_FRUITY_VSLOT])(intf) : NULL;
            for (; fp && n > 0; n--) {
                name[0] = 0;
                ((getname_t)(*(void ***)fp)[FRUITY_GETNAME_VSLOT])(fp, 0, n - 1, 0, name);
                if (name[0]) break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = MAX_PLUG_PARAMS; }
    __try { if (intf) g_intfClear(&intf); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
// Poll thread (rescan): the unit's plugin params to watch; 0 until the main thread has counted them.
static int unit_params(int unit, int a, int b) {
    Unit *u = unit_ptr(unit, a, b);
    void *inst = plugin_inst(unit_obj(unit, a, b));
    DWORD now = GetTickCount();
    if (!u || !inst) return MAX_PLUG_PARAMS;            // no plugin: REC says where they end
    if (u->pcInst == inst) return u->pcount;
    if (u->pcAsked != inst || now - u->pcAskedAt > 10000) {
        u->pcAsked = inst; u->pcAskedAt = now;
        enqueue_unit(T_PCOUNT, unit, a, b, 0, NULL);
    }
    return 0;
}
static volatile LONG g_rescanNow = 0;    // a count came in: the poll thread rescans on its next tick
// Main thread.
static void do_pcount(const QItem *it) {
    Unit *u = unit_ptr(it->unit, it->a, it->b);
    void *obj = unit_obj(it->unit, it->a, it->b), *inst = plugin_inst(obj);
    if (!u || !inst) return;
    u->pcount = plug_param_count(obj);
    u->pcInst = inst;
    InterlockedExchange(&g_rescanNow, 1);
    logmsg("PLUG params: %s %d/%d watches %d (FL says %d)", it->unit == U_CH ? "ch" : "fx", it->a, it->b, u->pcount,
           *(int *)((char *)obj + PLUG_NPARAMS));
}

static int chan_list(void ***items) {
    __try {
        char *list = **(char ***)(g_flBase + (CHANLIST_VA - IMG_BASE));
        *items = *(void ***)(list + 8);
        int n = *(int *)(list + 0x10);
        return n < 0 ? 0 : (n > MAX_CH ? MAX_CH : n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { *items = NULL; return 0; }
}

static int chan_list_filtered(void ***items) {
    __try {
        char *list = **(char ***)(g_flBase + (CHANFILT_VA - IMG_BASE));
        *items = *(void ***)(list + 8);
        int n = *(int *)(list + 0x10);
        return n < 0 ? 0 : (n > MAX_CH ? MAX_CH : n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { *items = NULL; return 0; }
}

static uint64_t fnv64(const unsigned char *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

#define EV_NEWCHAN       64        // one per channel (word: its index)
#define EV_PLUGNAME      201
#define EV_NEWPLUGIN     212
#define EV_SAMPLE_PATH   196
#define EV_PLUGIN_DATA   213
#define EV_INSFLAGS      236       // a mixer track's flags (u32, flags, u32), one per track in order
#define INS_ENABLED      0x8
#define INS_SOLO         0x1000
typedef int (*flpev_t)(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end);
static int flp_walk(const unsigned char *d, size_t n, flpev_t cb, void *ctx);

// ---- each person's plugin windows inside saved states ----
// FL saves a plugin's window with the plugin: in the plugin record (event 212) flags +16 bit 0 =
// editor open, +36 / +40 = editor x / y, +44 / +48 = its width / height (FL corrects them by a pixel
// when the editor first opens after a load); a VST wrapper (event 201 "Fruity Wrapper") also keeps its
// editor size in record 2 of its data (bytes 1..8). That is one person's view, so it must not count
// as a change, and loading someone's state must not open, close, move or resize our windows.
typedef struct { unsigned char *rec, *wrap; int wrapper; } WinBytes;
static int winbytes_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    WinBytes *w = (WinBytes *)ctx;
    (void)at; (void)end;
    if (id == EV_PLUGNAME) w->wrapper = n >= 28 && memcmp(p, L"Fruity Wrapper", 28) == 0;
    else if (id == EV_NEWPLUGIN && n >= 52 && !w->rec) w->rec = (unsigned char *)p;
    else if (id == EV_PLUGIN_DATA && w->wrapper && !w->wrap) {
        size_t o = 4;                                         // 4-byte header, then [u32 id][u64 length][data]
        while (o + 12 <= n) {
            uint32_t rid = *(const uint32_t *)(p + o);
            uint64_t ln = *(const uint64_t *)(p + o + 4);
            if (ln > n - o - 12) break;
            if (rid == 2 && ln >= 9) { w->wrap = (unsigned char *)p + o + 13; break; }
            o += 12 + (size_t)ln;
        }
    }
    return 1;
}
static void winbytes_find(unsigned char *d, size_t n, WinBytes *w) { memset(w, 0, sizeof(*w)); flp_walk(d, n, winbytes_cb, w); }
// Before loading someone's state: our own window bytes (from own, our current save) go in; with
// nothing of ours to keep, the editor stays closed.
static void winbytes_keep(unsigned char *d, size_t n, unsigned char *own, size_t on) {
    WinBytes a, b;
    winbytes_find(d, n, &a);
    if (own) winbytes_find(own, on, &b); else memset(&b, 0, sizeof(b));
    if (a.rec) a.rec[16] = (unsigned char)((a.rec[16] & ~1) | (b.rec ? b.rec[16] & 1 : 0));
    if (a.rec && b.rec) memcpy(a.rec + 36, b.rec + 36, 16);
    if (a.wrap && b.wrap) memcpy(a.wrap, b.wrap, 8);
}

// Zip archives inside plugin data (Kilohearts keeps its whole state as one) stamp each entry with
// its save time: DOS time + date, 2 s steps, in the local header ("PK\3\4" +10) and the central
// directory record ("PK\1\2" +12), so no two saves match. This clears them in each archive whose
// end record ("PK\5\6") checks out: its directory ends right at it, holds exactly the entries it
// counts, and each entry points at a local header with the same name. Other bytes stay as they are.
static void zip_times_clear(unsigned char *p, size_t n) {
    size_t e, cd, z, o, lh, nl, len;
    uint32_t cds, cdo;
    int cnt, k, pass;
    for (e = 0; e + 22 <= n; e++) {
        if (p[e] != 'P' || memcmp(p + e, "PK\5\6", 4) != 0) continue;
        cnt = *(const uint16_t *)(p + e + 10);
        cds = *(const uint32_t *)(p + e + 12);
        cdo = *(const uint32_t *)(p + e + 16);
        if (cds > e || cdo > e - cds) continue;
        cd = e - cds; z = cd - cdo;                         // its offsets count from its own start
        for (pass = 0; pass < 2; pass++) {                  // check every entry, then clear
            for (k = 0, o = cd; k < cnt; k++, o += len) {
                if (o + 46 > e || memcmp(p + o, "PK\1\2", 4) != 0) break;
                nl = *(const uint16_t *)(p + o + 28);
                len = 46 + nl + *(const uint16_t *)(p + o + 30) + *(const uint16_t *)(p + o + 32);
                lh = z + *(const uint32_t *)(p + o + 42);
                if (o + len > e || lh + 30 + nl > cd || memcmp(p + lh, "PK\3\4", 4) != 0 ||
                    *(const uint16_t *)(p + lh + 26) != nl || memcmp(p + lh + 30, p + o + 46, nl) != 0) break;
                if (pass) { memset(p + lh + 10, 0, 4); memset(p + o + 12, 0, 4); }
            }
            if (k < cnt || o != e) break;
        }
    }
}

// Hash of a saved .fst without its window bytes and zip entry times, skipping the 14-byte "FLhd"
// header: it embeds the project's channel count, which would make every unit look changed on add/delete.
// Events that only say which FL build saved it (199 version, 159 build, FL 2026's escaped "FL Studio
// <build>"): people may run different builds, so they are never a change.
#define EV_VERSION 199
#define EV_BUILD   159
#define EV_FLNAME  0xC0000101u
static int hash_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    uint64_t *h = (uint64_t *)ctx;
    size_t i;
    (void)at; (void)end;
    if (id == EV_VERSION || id == EV_BUILD || (uint32_t)id == EV_FLNAME) return 1;
    if (id == EV_PLUGIN_DATA) zip_times_clear((unsigned char *)p, n);   // state_hash's own copy
    *h = (*h ^ (uint32_t)id) * 1099511628211ull;
    for (i = 0; i < n; i++) *h = (*h ^ p[i]) * 1099511628211ull;
    return 1;
}
static uint64_t state_hash(const unsigned char *d, size_t n) {
    unsigned char *c = (unsigned char *)malloc(n ? n : 1);
    WinBytes w;
    uint64_t h = 1469598103934665603ull;
    memcpy(c, d, n);
    winbytes_find(c, n, &w);
    if (w.rec) { w.rec[16] &= (unsigned char)~1; memset(w.rec + 36, 0, 16); }
    if (w.wrap) memset(w.wrap, 0, 8);
    if (!flp_walk(c, n, hash_cb, &h)) h = fnv64(c, n);
    free(c);
    return h ? h : 1;
}

static unsigned char *read_file(const char *path, size_t *n) {
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(sz > 0 ? sz : 1);
    *n = fread(buf, 1, sz > 0 ? sz : 0, f);
    fclose(f);
    return buf;
}

static char *tmp_path(const char *tag) {
    char *p = (char *)malloc(MAX_PATH);
    sprintf_s(p, MAX_PATH, "%s\\%s_%ld.fst", g_tmpDir, tag, InterlockedIncrement(&g_tmpSeq));
    return p;
}

// ---- automation and controller links ----
// FL keeps the links of automation clips and of internal controllers (Peak, Formula, LFO...) in one list
// and saves it only with whole projects (FLP event 227, the mapping formula in 230 after it): *LINKS ->
// { MIDI list (226: each person's own hardware, never synced), internal list }, each a TList (items +8,
// count +0x10) of 28-byte records: +0 formula object, +8 source (the clip's or controller's REC base:
// channel index << 16, or its mixer slot's), +0xC runtime, +0x10 target REC id, +0x14 flags (8 internal),
// +0x18 smoothing. FL renumbers both ends itself on every structural change flim replays.
// - A target's value comes from its clip or controller on each FL as it plays: it is never sent or applied
//   as a param (one person playing moved everyone's automated knob).
// - Links travel with their source's state: unit_save appends the unit's own links as 227 (+230), sent
//   in a field of their own ("lk": an older flim never loads them). A load of a state with links first
//   drops the unit's links (LINKDEL), as FL's loader appends the ones in a state whatever its mask, and
//   then sorts the list (links_sort), as FL's loader doesn't for a channel or slot. Link edits made here
//   send the unit's state (links_scan). Before, a clip made, cloned or merged in a session reached the
//   others unlinked, link edits never went out, and a re-sync put everyone on the leader's links.
#define LINKS_VA     FLVA(LINKS)     // global -> { MIDI list, internal list }
#define LINKDEL_VA   FLVA(LINKDEL)   // DeleteLinksFrom(lo, hi): internal links with source in [lo, hi] (takes SONGLOCK)
#define FORMTEXT_VA  FLVA(FORMTEXT)  // FormulaText(formula, string *out, 0): what the project writer saves as 230
#define EV_LINK      227
#define EV_LINKFORM  230
typedef void (*linkdel_t)(uint32_t lo, uint32_t hi);
typedef void (*formtext_t)(void *formula, void **out, uint64_t zero);
static CRITICAL_SECTION g_autoLock;
static uint32_t *g_autoT; static int g_nautoT;  // targets of internal links, sorted (g_autoLock)
#define LINK_QUIET_MS 1500
static volatile DWORD g_linkQuiet;               // last load / structure op: FL renumbers or deletes links then itself

static char *links_list(void) {
    __try { char **l = *(char ***)(g_flBase + (LINKS_VA - IMG_BASE)); return l ? l[1] : NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static uint32_t unit_rec_base(int unit, int a, int b) {
    return unit == U_CH ? (uint32_t)a << 16 : 0x70000000u + ((uint32_t)((a << 6) | b) << 16);
}
static int u32_cmp(const void *x, const void *y) {
    uint32_t p = *(const uint32_t *)x, q = *(const uint32_t *)y;
    return p < q ? -1 : p > q;
}
static Unit *link_src_unit(uint32_t src) {
    uint32_t ts = (src - 0x70000000u) >> 16;
    return src < 0x70000000u ? unit_ptr(U_CH, (int)(src >> 16), 0) : unit_ptr(U_FX, (int)(ts >> 6), (int)(ts & 63));
}
// Poll thread, every 250 ms: the params a clip or controller drives here, and whose links changed. A
// change made here (add / remove a target, a formula) counts as an edit of the source unit: its state,
// which carries its links, goes out as after a preset-sized burst. steady = no channels / tracks moving:
// FL renumbers links itself then (on everyone's FL), and a load's links are someone else's.
typedef struct { uint32_t src; uint64_t h; } LinkSum;
static void links_scan(DWORD now, int steady) {
    static DWORD at;
    static LinkSum *prev;
    static int nprev = -1;
    static LONG gen;
    uint32_t *t = NULL;
    LinkSum *s = NULL;
    int n = 0, ns = 0, i, j, k, cnt;
    char *L, **items;
    if (now - at < 250) return;
    if (now - at > 1000 || now - g_linkQuiet < LINK_QUIET_MS) steady = 0;   // skipped meanwhile (a load, channels
                                                        // moving) or a load / structure op just ran: re-baseline
    at = now;
    L = links_list();
    __try {
        if (L && (cnt = *(int *)(L + 0x10)) > 0 && cnt <= 65536 && (items = *(char ***)(L + 8)) != NULL) {
            t = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)cnt);
            s = (LinkSum *)malloc(sizeof(LinkSum) * (size_t)cnt);
            for (i = 0; i < cnt; i++) {
                char *r = items[i];
                uint32_t rec[6];
                if (!r) continue;
                memcpy(rec, r + 8, 20); rec[1] = 0;                 // (runtime state)
                memcpy(rec + 5, r, 4);                              // low half of the formula object: a new formula
                t[n++] = rec[2];
                s[ns].src = rec[0] & 0xFFFF0000u;
                s[ns++].h = fnv64((const unsigned char *)rec, sizeof(rec));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = ns = 0; }
    if (n > 1) qsort(t, (size_t)n, sizeof(uint32_t), u32_cmp);
    EnterCriticalSection(&g_autoLock);
    free(g_autoT); g_autoT = t; g_nautoT = n;
    LeaveCriticalSection(&g_autoLock);
    // one sum per source (records in any order), sorted by source
    if (ns > 1) qsort(s, (size_t)ns, sizeof(LinkSum), u32_cmp);
    for (i = k = 0; i < ns; i++) {
        if (k && s[k - 1].src == s[i].src) s[k - 1].h += s[i].h;
        else s[k++] = s[i];
    }
    ns = k;
    if (steady && nprev >= 0 && gen == g_structGen)
        for (i = j = 0; i < ns || j < nprev; ) {
            uint32_t src;
            Unit *u;
            if (j >= nprev || (i < ns && s[i].src < prev[j].src)) src = s[i++].src;
            else if (i >= ns || prev[j].src < s[i].src) src = prev[j++].src;
            else { src = s[i].src; if (s[i++].h == prev[j++].h) continue; }
            if ((u = link_src_unit(src)) != NULL && now >= u->suppressUntil && !u->identAt) {
                if (!u->lastAct) logmsg("LINKS of %s 0x%08x changed here: its state goes out", src < 0x70000000u ? "channel" : "mixer slot", src);
                u->lastAct = u->userAt = now; u->burst = 1;
            }
        }
    free(prev); prev = s; nprev = ns; gen = g_structGen;
}
// FL is playing the song (its automation moves the params a link targets).
static int fl_playing(void) {
    __try { return **(int **)(g_flBase + (FLVA(PLAYING) - IMG_BASE)) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static int auto_target(uint32_t id) {
    int lo = 0, hi, r = 0;
    EnterCriticalSection(&g_autoLock);
    for (hi = g_nautoT - 1; lo <= hi && !r; ) {
        int m = (lo + hi) / 2;
        if (g_autoT[m] == id) r = 1; else if (g_autoT[m] < id) lo = m + 1; else hi = m - 1;
    }
    LeaveCriticalSection(&g_autoLock);
    return r;
}
// A Delphi string (Ansi or Unicode: its header says which) -> UTF-16; chars, 0 = none.
static int dstr_wide(const void *s, wchar_t *out, int cap) {
    const unsigned char *p = (const unsigned char *)s;
    int len = *(const int32_t *)(p - 4), es = *(const uint16_t *)(p - 10), cp = *(const uint16_t *)(p - 12), k;
    if (len <= 0 || len >= cap) return 0;
    if (es == 2) { memcpy(out, s, (size_t)len * 2); out[len] = 0; return len; }
    k = MultiByteToWideChar(cp ? cp : CP_ACP, 0, (const char *)s, len, out, cap - 1);
    out[k > 0 ? k : 0] = 0;
    return k > 0 ? k : 0;
}
static void ev_put(unsigned char **o, size_t *on, size_t *cap, int id, const void *p, size_t n) {
    size_t v = n;
    if (*on + n + 8 > *cap) { *cap = (*on + n + 8) * 2; *o = (unsigned char *)realloc(*o, *cap); }
    (*o)[(*on)++] = (unsigned char)id;
    do { unsigned char c = (unsigned char)(v & 0x7f); v >>= 7; (*o)[(*on)++] = (unsigned char)(c | (v ? 0x80 : 0)); } while (v);
    memcpy(*o + *on, p, n);
    *on += n;
}
// Main thread (unit_save): the unit's own links into its saved state, as FL's project writer writes them.
static void links_append(int unit, int a, int b, unsigned char **d, size_t *n) {
    uint32_t base = unit_rec_base(unit, a, b);
    char *L = links_list(), **items;
    unsigned char *o = NULL;
    size_t on = 0, cap = 0, dt = *n >= 8 ? 8 + (size_t)*(uint32_t *)(*d + 4) : 0;
    int i, cnt;
    if (!L || !dt || dt + 8 > *n || memcmp(*d + dt, "FLdt", 4) != 0) return;
    __try {
        cnt = *(int *)(L + 0x10);
        items = *(char ***)(L + 8);
        for (i = 0; items && i < cnt && cnt <= 65536; i++) {
            char *r = items[i];
            uint32_t ev[5];
            void *txt = NULL;
            wchar_t w[512];
            int k;
            if (!r || (*(uint32_t *)(r + 8) & 0xFFFF0000u) != base) continue;
            memcpy(ev, r + 8, 20);
            ev[1] = 0;                                          // (runtime state)
            ev_put(&o, &on, &cap, EV_LINK, ev, 20);
            if (*(void **)r) {
                // ponytail: the string FL makes here is never released, a few bytes per formula and save
                ((formtext_t)(g_flBase + (FORMTEXT_VA - IMG_BASE)))(*(void **)r, &txt, 0);
                if (txt && (k = dstr_wide(txt, w, 512)) > 0) ev_put(&o, &on, &cap, EV_LINKFORM, w, (size_t)(k + 1) * 2);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("LINKS reading this unit's links faulted"); free(o); return; }
    if (!on) return;
    *d = (unsigned char *)realloc(*d, *n + on);
    memcpy(*d + *n, o, on);
    *n += on;
    *(uint32_t *)(*d + dt + 4) += (uint32_t)on;                 // the FLdt chunk's length
    free(o);
}
// A state's links, before it loads: their source becomes this unit (whatever index it had where it was
// saved), and has = it brings any.
typedef struct { uint32_t base; int has; } LinkFix;
static int linkfix_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    LinkFix *f = (LinkFix *)ctx;
    (void)at; (void)end;
    if (id == EV_LINK && n == 20) {
        uint32_t *src = (uint32_t *)p;
        *src = f->base | (*src & 0xFFFF);
        f->has = 1;
    }
    return 1;
}
// Main thread, before loading a state into a unit: its own links go (the state brings the ones it has).
static void links_drop(int unit, int a, int b) {
    uint32_t base = unit_rec_base(unit, a, b);
    __try { ((linkdel_t)(g_flBase + (LINKDEL_VA - IMG_BASE)))(base, base | 0xFFFF); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("LINKS dropping a unit's links faulted"); }
}
static CRITICAL_SECTION *song_lock(void);
// Main thread, after a load appended links: FL's loader sorts the list only after a whole project, and
// playback finds a source's links by binary search (sub_F82660), so unsorted some would drive nothing.
// Sorted by source, stable, as FL's own sub_F825B0 does after adding one.
static void links_sort(void) {
    char *L = links_list();
    if (!L) return;
    EnterCriticalSection(song_lock());
    __try {
        char **it = *(char ***)(L + 8);
        int n = *(int *)(L + 0x10), i, j;
        for (i = 1; it && i < n && n <= 65536; i++) {
            char *r = it[i];
            for (j = i; j > 0 && *(uint32_t *)(it[j - 1] + 8) > *(uint32_t *)(r + 8); j--) it[j] = it[j - 1];
            it[j] = r;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("LINKS sorting faulted"); }
    LeaveCriticalSection(song_lock());
}
// Where a saved state's links (links_append) start. They travel apart ("lk", unit_json_fields): an older
// flim would load them on top of the ones it has, again with every state.
static int linkat_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    (void)p; (void)n; (void)end;
    if (id == EV_LINK || id == EV_LINKFORM) { *(size_t *)ctx = at; return 0; }
    return 1;
}
static size_t links_at(const unsigned char *d, size_t n) { size_t at = n; flp_walk(d, n, linkat_cb, &at); return at; }
// Main thread, after a unit's new plugin went out: FL deletes the links into a plugin it replaces, and a
// clip or controller pointing into the new one may have reached the others before it (a chan.add goes
// at once, a new plugin's state SETTLE_IDENT_MS later). Their states go again, after it.
static void links_resend_into(int unit, int a, int b) {
    uint32_t lo = unit_rec_base(unit, a, b) + 0x8000, hi = lo + 0x7FFF, last = 1;
    char *L = links_list(), **items;
    int i, cnt;
    if (!L) return;
    __try {
        cnt = *(int *)(L + 0x10);
        items = *(char ***)(L + 8);
        for (i = 0; items && i < cnt && cnt <= 65536; i++) {
            uint32_t t = *(uint32_t *)(items[i] + 0x10), src = *(uint32_t *)(items[i] + 8) & 0xFFFF0000u, ts = (src - 0x70000000u) >> 16;
            if (t < lo || t > hi || src == last) continue;
            last = src;
            if (src < 0x70000000u) enqueue_unit(T_SNAP, U_CH, (int)(src >> 16), 0, SNAP_RESEND, NULL);
            else enqueue_unit(T_SNAP, U_FX, (int)(ts >> 6), (int)(ts & 63), SNAP_RESEND, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("LINKS reading the links into a unit faulted"); }
}

// Save a unit's full state with FL's preset saver, with its links (above). Main thread only.
// Returns malloc'd bytes (caller frees) or NULL (empty slot / failure).
static unsigned char *unit_save(int unit, int a, int b, size_t *n) {
    void *obj = unit_obj(unit, a, b);
    if (!obj) return NULL;
    if (unit == U_FX && !plugin_inst(obj)) return NULL;     // never save an empty slot
    char path[MAX_PATH];
    sprintf_s(path, sizeof(path), "%s\\snap.fst", g_tmpDir);
    DeleteFileA(path);
    wchar_t *w = dstr_new(path);
    int r = -1;
    __try { r = g_savePreset(w, unit_mask(unit), obj, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("SAVE faulted unit=%d %d/%d", unit, a, b); r = -1; }
    dstr_free(w);
    if (r != 0) return NULL;
    {
        unsigned char *d = read_file(path, n);
        if (d) links_append(unit, a, b, &d, n);
        return d;
    }
}

static void b64enc(const unsigned char *in, int n, char *out);

// Serialize {"unit":"ch","ch":i | "unit":"fx","trk":t,"slot":s, "data":"<b64>","lk":"<b64>"} fields: the
// state without its links, and its links (links_at; "" = it has none, LOAD_LINKS), which parse_unit puts back.
static char *unit_json_fields(int unit, int a, int b, const unsigned char *data, size_t n) {
    size_t cap = 112 + (n + 2) / 3 * 4 + 1, k = links_at(data, n), dt = n >= 8 ? 8 + (size_t)*(const uint32_t *)(data + 4) : 0;
    char *j = (char *)malloc(cap);
    unsigned char *c = (unsigned char *)malloc(k ? k : 1);
    int o = unit == U_CH ? sprintf_s(j, cap, "\"unit\":\"ch\",\"ch\":%d,\"data\":\"", a)
                         : sprintf_s(j, cap, "\"unit\":\"fx\",\"trk\":%d,\"slot\":%d,\"data\":\"", a, b);
    memcpy(c, data, k);
    if (k < n && dt + 8 <= k) *(uint32_t *)(c + dt + 4) -= (uint32_t)(n - k);   // the FLdt chunk's length without them
    b64enc(c, (int)k, j + o);
    o += (int)strlen(j + o);
    o += sprintf_s(j + o, cap - o, "\",\"lk\":\"");
    b64enc(data + k, (int)(n - k), j + o);
    strcat_s(j, cap, "\"");
    free(c);
    return j;
}

// " 'name'" of channel a for log lines ("" for effect slots). Main thread.
static const char *unit_name_note(int unit, int a, char *out, int cap) {
    void **items;
    int n = chan_list(&items);
    out[0] = 0;
    if (unit != U_CH || !items || a < 0 || a >= n) return out;
    __try {
        const wchar_t *w = *(const wchar_t **)((char *)items[a] + 0x50);   // CH_NAME
        if (w) _snprintf_s(out, cap, _TRUNCATE, " '%S'", w);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
    return out;
}

// key: the plugin lock the op is sent under (NULL = none); the server drops it if a peer holds it.
static void send_unit(const char *kind, int unit, int a, int b, const unsigned char *data, size_t n, const char *key) {
    char *f = unit_json_fields(unit, a, b, data, n);
    size_t cap = strlen(f) + 128;
    char *m = (char *)malloc(cap);
    if (key) sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"%s\",\"key\":\"%s\",%s}}", kind, key, f);
    else sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"%s\",%s}}", kind, f);
    ws_send_samples(m, data, n);         // lists (and uploads) the samples the state uses
    free(f);
    {
        char nm[80];
        logmsg("PLUG sent %s unit=%s %d/%d%s (%zu bytes)", kind, unit == U_CH ? "ch" : "fx", a, b, unit_name_note(unit, a, nm, sizeof(nm)), n);
    }
}

static void do_snap(const QItem *it) {
    Unit *u = unit_ptr(it->unit, it->a, it->b);
    size_t n = 0;
    if (!u) return;
    if ((it->mode == SNAP_IF_DIFF || it->mode == SNAP_BASELINE) && it->gen != g_structGen) {
        // queued before channels or tracks moved: moved units have no baseline (hash 0), and a check
        // would take their index-dependent state for an edit to send. A silent baseline comes later.
        static LONG said = -1;
        if (said != g_structGen) { said = g_structGen; logmsg("PLUG state checks queued before channels / tracks moved: retaken later"); }
        if (it->mode == SNAP_IF_DIFF) { u->lastAct = GetTickCount(); u->burst = 1; }   // an edit's check: again, not dropped
        else u->baselineAt = GetTickCount() + REBASE_MS;
        return;
    }
    // States go out by channel index: while FL's channel list differs from the last structure scan (a
    // reorder the scan hasn't sent yet), index a may be another channel already. A burst / sweep snapshot
    // waits for the scan, which re-baselines the moved channels anyway.
    if (it->unit == U_CH && (it->mode == SNAP_IF_DIFF || it->mode == SNAP_BASELINE) && chan_list_changed()) {
        logmsg("PLUG ch %d: channels are moving, state not taken", it->a);
        return;
    }
    unsigned char *d = unit_save(it->unit, it->a, it->b, &n);
    if (!d) return;
    uint64_t h = state_hash(d, n);
    DWORD now = GetTickCount();
    // Provenance: a state goes out only if someone here worked in this plugin since its baseline
    // (unit_touched). Any input anywhere in FL used to do: two people busy elsewhere then sent a
    // drifting VST's state back and forth, a reload (an audio glitch) on each side every sweep.
    int userSince = unit_touched(u);
    int send = 0;
    char key[32];
    switch (it->mode) {
    case SNAP_IF_DIFF:
        // No baseline yet (a big project: the sweep takes one unit every SWEEP_MS) after a preset-sized burst
        // of local edits (a preset, or a VST swapped in the same wrapper): can't tell, so it goes out
        // (a receiver that has it already doesn't reload).
        if (!u->hash || h != u->hash) {
            if (userSince) send = 1;
            else logmsg("PLUG drift absorbed unit=%s %d/%d (nobody worked in it here)", it->unit == U_CH ? "ch" : "fx", it->a, it->b);
        }
        break;
    case SNAP_FORCE:   send = 1; break;   // identity changed: always
    case SNAP_RESEND:  send = 1; break;
    default: break;
    }
    if (send) {
        if (!plug_lock_send(it->unit, it->a, it->b, key, sizeof(key))) {
            // someone else is editing this plugin: put back the state everyone agreed on
            free(d);
            plug_revert_state(it->unit, it->a, it->b);
            return;
        }
        send_unit("plug.state", it->unit, it->a, it->b, d, n, key);
        u->editGen++;
        if (it->mode == SNAP_FORCE) links_resend_into(it->unit, it->a, it->b);
    }
    plug_agreed_set(it->unit, it->a, it->b, d, n);
    u->hash = h;
    // a check that sent nothing leaves the work in it open: a change the plugin makes a moment later
    // (a file it loads in the background) still goes out with the sweep
    if (send || it->mode != SNAP_IF_DIFF) u->baseAt = now;
    free(d);
}

// FL's preset reader reports progress in the hint bar and forces a synchronous repaint for it
// (sub_F3C210 -> sub_1187300 -> ShowHint -> UpdateWindow). If that repaint reaches a window that
// shows the plugin being replaced (an open sampler editor drawing its waveform), FL faults on the
// half-replaced sample and shows error dialogs. FL's own long operations bump a counter on the main
// form that keeps the progress bar quiet (sub_1187300 checks it); so do we around our loads.
#define FORM_NOPROGRESS 0x353c
static void progress_quiet(int on) {
    __try { *(int *)(**(char ***)(g_flBase + (MAINOBJ_VA - IMG_BASE)) + FORM_NOPROGRESS) += on ? 1 : -1; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int g_flimLoad = 0;                // main thread: inside one of our LoadPresetFile calls (msgbox_detour)
static int g_flimProjLoad = 0;            // main thread: inside our load of the leader's project (msgbox_detour)

// A state loaded while a sample in it was still a silent placeholder: once the sample is in, this
// state is loaded again, not the plugin's own save of itself, which may have adapted to the
// placeholder (Slicer fits its slices to the sample it has). Kept per channel / slot object, and
// only used if nothing was edited here since (editGen). Main thread.
#define PHLOAD_MAX 64
#define LOAD_FORCE 2                      // T_LOAD mode bits: load even if we have exactly this state already
#define LOAD_LINKS 4                      //   the sender puts the unit's links in its states: none in it = none
typedef struct { void *obj; unsigned char *d; size_t n; int editGen; } PhLoad;
static PhLoad g_phLoads[PHLOAD_MAX];
static int g_nPhLoads = 0;
static int state_has_placeholder(const unsigned char *d, size_t n);
// The kept state for obj (removed from the list), if nothing was edited here since. NULL = none.
static unsigned char *phload_take(void *obj, int editGen, size_t *n) {
    int i;
    for (i = 0; i < g_nPhLoads; i++) {
        PhLoad p = g_phLoads[i];
        if (p.obj != obj) continue;
        g_phLoads[i] = g_phLoads[--g_nPhLoads];
        if (p.editGen == editGen) { *n = p.n; return p.d; }
        free(p.d);
        return NULL;
    }
    return NULL;
}
static void phload_note(void *obj, const unsigned char *d, size_t n, int editGen) {
    size_t k;
    free(phload_take(obj, editGen, &k));                  // a newer state replaces a kept one
    if (g_nPhLoads == PHLOAD_MAX || !state_has_placeholder(d, n)) return;
    g_phLoads[g_nPhLoads].obj = obj;
    g_phLoads[g_nPhLoads].d = (unsigned char *)malloc(n);
    memcpy(g_phLoads[g_nPhLoads].d, d, n);
    g_phLoads[g_nPhLoads].n = n;
    g_phLoads[g_nPhLoads].editGen = editGen;
    g_nPhLoads++;
}

static void do_load(const QItem *it) {
    Unit *u = unit_ptr(it->unit, it->a, it->b);
    void *obj = unit_obj(it->unit, it->a, it->b);
    if (!u || !obj) { logmsg("LOAD: no target unit=%d %d/%d", it->unit, it->a, it->b); return; }
    size_t n = 0, m = 0;
    unsigned char *in = read_file(it->path, &n), *own;
    LinkFix lf;
    uint64_t mh0;                                // the channel's name / color before the load (meta_take_channel)
    lf.base = unit_rec_base(it->unit, it->a, it->b); lf.has = 0;
    if (in) flp_walk(in, n, linkfix_cb, &lf);
    own = unit_save(it->unit, it->a, it->b, &m);
    uint64_t hin = in ? state_hash(in, n) : 0, hown = own ? state_hash(own, m) : 0;
    if (in) {
        FILE *f = NULL;
        plug_agreed_set(it->unit, it->a, it->b, in, n);       // what a plugin-lock revert restores
        winbytes_keep(in, n, own, m);                         // our windows stay as they are
        if (fopen_s(&f, it->path, "wb") == 0 && f) { fwrite(in, 1, n, f); fclose(f); }
    }
    free(own);
    if (!(it->mode & LOAD_FORCE) && hown && hown == hin) {   // we have exactly this already: no reload, no glitch
        u->hash = hown;
        u->baseAt = GetTickCount();
        free(in);
        logmsg("PLUG unit=%s %d/%d already identical: not reloaded", it->unit == U_CH ? "ch" : "fx", it->a, it->b);
        return;
    }
    u->identAt = 0; u->lastAct = 0; u->burst = 0;
    u->suppressUntil = GetTickCount() + 60000;  // the params this load moves aren't local edits (set properly below)
    wchar_t *w = dstr_new(it->path);
    int r = -1;
    if ((it->mode & LOAD_LINKS) || lf.has) links_drop(it->unit, it->a, it->b);   // the state's links replace this unit's
    progress_quiet(1);
    mh0 = it->unit == U_CH ? meta_chan_hash(it->a) : 0;
    g_flimLoad = 1;
    __try { r = loadpreset_call(g_loadPreset, w, unit_mask(it->unit), obj, LOADPRESET_NOEDITOR); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("LOAD faulted unit=%d %d/%d", it->unit, it->a, it->b); }
    g_flimLoad = 0;
    if (lf.has) links_sort();
    progress_quiet(0);
    dstr_free(w);
    if (in) phload_note(obj, in, n, u->editGen);
    free(in);
    u->inst = plugin_inst(obj);                 // ours: the structure scan must not report it back
    if (it->unit == U_CH) meta_take_channel(it->a, mh0);   // the name / color it brought: known now, a rename after it is an edit
    if (it->unit == U_CH && it->peer && it->sc[SC_MIX] >= 0 && g_sc[SC_MIX] > it->sc[SC_MIX]) {
        // FL's loader set the channel's mixer target from the state: a track number as its sender had them,
        // before mixer track changes this FL has done since. Through them (like a target param, sc_rebase).
        uint32_t id = ((uint32_t)it->a << 16) | CH_TARGET_SUB, t = read_value(id), nt;
        if (t != REC_NONE && (nt = sc_mix_map_value(t, it->sc[SC_MIX])) != t) {
            write_value(id, nt);
            logmsg("LOAD channel %d: its mixer target %u is track %u here (moved since the state was made)", it->a, t, nt);
        }
    }
    // The load moved this unit's params; take them as the new truth right away
    // (instead of ignoring local edits for seconds), plus a short window for
    // plugins that update params asynchronously after a state load.
    {
        int i, k = 0, un, ua, ub;
        for (i = 0; i < g_nslot; i++) {
            Slot *s = &g_slots[i];
            if (!unit_of_id(s->id, &un, &ua, &ub) || un != it->unit || ua != it->a || ub != it->b) continue;
            EnterCriticalSection(&g_slotLock);
            s->last = read_value(s->id);
            InterlockedIncrement(&s->gen);
            LeaveCriticalSection(&g_slotLock);
            k++;
        }
        DWORD now = GetTickCount();
        u->suppressUntil = now + SUPPRESS_MS;
        u->absorbUntil = now + ABSORB_MS;
        u->baseAt = now;
        u->baselineAt = now + REBASE_MS;
        (void)k;
    }
    {
        char nm[80];
        logmsg("PLUG applied unit=%s %d/%d%s ret=%d", it->unit == U_CH ? "ch" : "fx", it->a, it->b, unit_name_note(it->unit, it->a, nm, sizeof(nm)), r);
    }
}

static void units_shift_after_delete(int idx, int n) {
    int i;
    for (i = idx; i + 1 < n && i + 1 < MAX_CH; i++) g_chU[i] = g_chU[i + 1];
    if (n - 1 >= 0 && n - 1 < MAX_CH) memset(&g_chU[n - 1], 0, sizeof(Unit));
    for (i = idx; i < MAX_CH; i++) {
        g_chanPlugN[i] = 0;          // param ranges re-probed by rescan
        g_chU[i].hash = 0;           // saved state is index-dependent: re-baseline, don't send
        g_chU[i].baselineAt = GetTickCount() + REBASE_MS;   // (without one, the sweep sent every moved channel's state)
    }
    plug_agreed_reset(1, 0);
    InterlockedIncrement(&g_structGen);
}
// A channel came in at idx (n before): the later channels' bookkeeping moves up one.
static void units_insert(int idx, int n) {
    int i;
    for (i = n < MAX_CH - 1 ? n : MAX_CH - 1; i > idx; i--) g_chU[i] = g_chU[i - 1];
    if (idx < MAX_CH) memset(&g_chU[idx], 0, sizeof(Unit));
    for (i = idx; i < MAX_CH; i++) {
        g_chanPlugN[i] = 0;
        g_chU[i].hash = 0;
        if (i > idx) g_chU[i].baselineAt = GetTickCount() + REBASE_MS;
    }
    plug_agreed_reset(1, 0);
    InterlockedIncrement(&g_structGen);
}

// Channels FL deleted since the last structure check (delchan_detour; main thread): one of these
// in FL's list again is a new channel in its memory.
#define CH_DEAD_MAX 64
static void *g_chDead[CH_DEAD_MAX];
static int   g_nChDead = 0;

static void known_from_list(void) {
    void **items; int n = chan_list(&items), i;
    if (n > MAX_CH) n = MAX_CH;
    for (i = 0; i < n; i++) g_chKnown[i] = items[i];
    g_nChKnown = n;
    g_nChDead = 0;
}

static int index_of(void **list, int n, void *o) {
    int i;
    for (i = 0; i < n; i++) if (list[i] == o) return i;
    return -1;
}

// A peer's new channel at index a of the channel list everyone has (g_chKnown; it->mode = that list's
// length then). FL's AddChannel(pos) puts it there, as FL's Clone does. A channel added here that isn't
// sent yet stays put: the peer's goes next to the channel it follows in the shared list.
static void do_addch(const QItem *it) {
    void **items; int n = chan_list(&items), nk = g_nChKnown, a = it->a, pos;
    void *obj = NULL;
    if (nk >= MAX_CH) { logmsg("chan.add: more than %d channels aren't synced", MAX_CH); DeleteFileA(it->path); free(it->path); return; }
    if (it->mode >= 0 ? (nk != it->mode || a < 0 || a > nk) : a != nk) {
        logmsg("chan.add: index %d of %d channels, but this FL has %d", a, it->mode >= 0 ? it->mode : a, nk);
        DeleteFileA(it->path); free(it->path);
        resync("a channel added to another channel list");
        return;
    }
    pos = a < nk ? index_of(items, n, g_chKnown[a]) : (nk ? index_of(items, n, g_chKnown[nk - 1]) + 1 : 0);
    if (!items || pos < 0) {                                // its neighbour was deleted here and that isn't sent yet
        DeleteFileA(it->path); free(it->path);
        resync("a channel added next to one deleted here");
        return;
    }
    __try { obj = g_addChannel(pos < n ? pos : -1, 0, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { obj = NULL; }
    if (!obj) { logmsg("chan.add: AddChannel failed"); DeleteFileA(it->path); free(it->path); resync("a channel FL didn't add"); return; }
    memmove(g_chKnown + a + 1, g_chKnown + a, sizeof(void *) * (size_t)(nk - a));
    g_chKnown[a] = obj;
    g_nChKnown = nk + 1;
    units_insert(a, nk);
    g_chU[a].inst = plugin_inst(obj);
    g_chU[a].absorbUntil = GetTickCount() + SUPPRESS_MS + SETTLE_IDENT_MS;
    {   // the channels after it moved up (FL renumbered their notes and clips): baselines follow them
        static int from[MAX_CH];
        int i;
        for (i = 0; i <= nk; i++) from[i] = i < a ? i : i == a ? -1 : i - 1;
        chan_rebase(from, nk + 1);
        sc_done(SC_CH, from, nk + 1);
    }
    logmsg("CHAN added index %d (remote)", a);
    clips_hold_retry();
    {   // its state loads on a later tick, into this channel wherever it is by then
        QItem q; memset(&q, 0, sizeof(q));
        q.type = T_LOAD; q.unit = U_CH; q.a = pos < n ? pos : n; q.path = it->path; q.chobj = obj;
        enqueue_front(&q);                                // next: its name comes with it, and a rename queued
    }                                                     // after the add (a replay) must not be overwritten by it
}

// A peer deleted channel a of the list everyone has (it->mode = that list's length then).
static void do_delch(const QItem *it) {
    void **items; int n = chan_list(&items), nk = g_nChKnown, a = it->a, j;
    if ((it->mode >= 0 && nk != it->mode) || a < 0 || a >= nk) {
        logmsg("chan.del: index %d of %d channels, but this FL has %d", a, it->mode, nk);
        resync("a channel deleted from another channel list");
        return;
    }
    j = index_of(items, n, g_chKnown[a]);                  // -1: deleted here too, not sent yet
    if (j >= 0) {
        __try { g_delChannel((unsigned)j, 0, DELCHAN_FLAGS); } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("chan.del faulted"); }
    }
    units_shift_after_delete(a, nk);
    memmove(g_chKnown + a, g_chKnown + a + 1, sizeof(void *) * (size_t)(nk - a - 1));
    g_nChKnown = nk - 1;
    {
        static int from[MAX_CH];
        int i;
        for (i = 0; i < nk - 1; i++) from[i] = i < a ? i : i + 1;
        chan_rebase(from, nk - 1);
        sc_done(SC_CH, from, nk - 1);
    }
    logmsg("CHAN deleted index %d (remote)", a);
}

// Channels changed places: carry each channel's unit bookkeeping (plugin identity etc.)
// along to its new index. Saved channel state embeds the index, so moved channels
// re-baseline their hash silently; their param ranges are re-probed and every param
// is re-read without being sent (g_structGen) -- the values moved with the channels.
static void units_permute(const int *from, int n) {
    static Unit old[MAX_CH];
    int i;
    memcpy(old, g_chU, sizeof(Unit) * (size_t)n);
    for (i = 0; i < n; i++) {
        g_chU[i] = old[from[i]];
        if (from[i] != i) {                           // (pending sends go with the channel)
            g_chU[i].hash = 0;
            if (!g_chU[i].identAt && !g_chU[i].lastAct) g_chU[i].baselineAt = GetTickCount() + REBASE_MS;
            g_chanPlugN[i] = 0;
        }
    }
    plug_agreed_reset(1, 0);
    InterlockedIncrement(&g_structGen);
}

// Remove the effect from a mixer slot the way FL's slot menu "Delete" does: load the
// "(delete) effect" preset into it. Main thread.
static void do_fxclear(const QItem *it) {
    Unit *u = unit_ptr(U_FX, it->a, it->b);
    void *obj = fx_obj(it->a, it->b);
    if (!u || !obj || !plugin_inst(obj)) return;              // already empty
    wchar_t *w = dstr_new(g_fxDeletePreset);
    int r = -1;
    progress_quiet(1);
    __try { r = loadpreset_call(g_loadPreset, w, MASK_FX, obj, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("FX clear faulted %d/%d", it->a, it->b); }
    progress_quiet(0);
    dstr_free(w);
    memset(u, 0, sizeof(Unit));
    plug_agreed_set(U_FX, it->a, it->b, NULL, 0);
    u->inst = plugin_inst(obj);
    u->absorbUntil = GetTickCount() + ABSORB_MS;
    logmsg("PLUG cleared fx %d/%d (remote) ret=%d%s", it->a, it->b, r, u->inst ? " BUT SLOT STILL OCCUPIED" : "");
}

// Join: slots the leader has empty (absent from plug.full) must be empty here too.
static void do_fxsync(const QItem *it) {
    const unsigned char *keep = (const unsigned char *)it->order;
    int t, s, n = 0, nm = mix_count();
    for (t = 0; t < nm; t++)
        for (s = 0; s < MAX_FX_SLOTS; s++) {
            if (keep[t * MAX_FX_SLOTS + s] || !plugin_inst(fx_obj(t, s))) continue;
            enqueue_unit(T_FXCLEAR, U_FX, t, s, 0, NULL);
            n++;
        }
    if (n) logmsg("PLUG join: clearing %d effect slots the leader has empty", n);
}

// Remote reorder: rebuild the peer's order with FL's own "move selected up", one
// channel at a time (select only it, move, repeat), then restore our selection.
// Works on the rack's filter-group list like FL does; channels outside the group
// must stay where they are.
static void do_order(const QItem *it) {
    int n = it->order[0], i, j, steps = 0;
    const int *from = it->order + 1;
    void **items, **filt;
    static void *M[MAX_CH], *T[MAX_CH], *D[MAX_CH];
    static unsigned char sel[MAX_CH];
    int nm = chan_list(&items);
    if (!items || nm != n || g_nChKnown != n || chan_list_changed()) {    // (changed: a reorder here crossed it)
        logmsg("chan.order: have %d channels (known %d), peer %d", nm, g_nChKnown, n);
        resync("channels reordered in another channel list");
        return;
    }
    {
        unsigned char seen[MAX_CH] = { 0 };
        for (i = 0; i < n; i++) {
            if (from[i] < 0 || from[i] >= n || seen[from[i]]) { logmsg("chan.order: not a permutation; skipped"); return; }
            seen[from[i]] = 1;
        }
    }
    for (i = 0; i < n; i++) { M[i] = items[i]; T[i] = items[from[i]]; }
    int nf = chan_list_filtered(&filt), nd = 0;
    if (!filt) return;
    for (i = 0; i < n; i++) {
        int inF = index_of(filt, nf, M[i]) >= 0;
        if (!inF && T[i] != M[i]) { logmsg("chan.order: channel %d is outside the rack's filter group; skipped", i); return; }
        if (index_of(filt, nf, T[i]) >= 0) D[nd++] = T[i];
    }
    if (nd != nf) { logmsg("chan.order: filter list mismatch (%d vs %d); skipped", nd, nf); return; }
    void *obj = NULL; int arg = -1;
    __try {
        obj = **(void ***)(g_flBase + (MOVEOBJ_VA - IMG_BASE));
        arg = **(int **)(g_flBase + (MOVEARG_VA - IMG_BASE));
        for (i = 0; i < n; i++) sel[i] = *((unsigned char *)M[i] + CH_SELECTED);
        for (i = 0; i < nd; i++) {
            nf = chan_list_filtered(&filt);
            j = index_of(filt, nf, D[i]);
            while (j > i) {
                int k, j2;
                for (k = 0; k < nf; k++) *((unsigned char *)filt[k] + CH_SELECTED) = (unsigned char)(k == j);
                g_moveChans(obj, MOVE_UP, arg, MOVE_FLAGS);
                steps++;
                nf = chan_list_filtered(&filt);
                j2 = index_of(filt, nf, D[i]);
                if (j2 != j - 1) { logmsg("chan.order: move did not take (%d -> %d); stopping", j, j2); i = nd; break; }
                j = j2;
            }
        }
        for (i = 0; i < n; i++) *((unsigned char *)M[i] + CH_SELECTED) = sel[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("chan.order faulted after %d moves", steps); }
    nm = chan_list(&items);
    int ok = nm == n;
    for (i = 0; ok && i < n; i++) ok = items[i] == T[i];
    if (ok) units_permute(from, n);
    else {
        // out of step with the peer's order: keep our bookkeeping consistent with what FL has
        for (i = 0; i < nm; i++) { g_chU[i].hash = 0; g_chU[i].inst = plugin_inst(items[i]); g_chanPlugN[i] = 0; }
        plug_agreed_reset(1, 0);
    }
    known_from_list();
    if (ok) { chan_rebase(from, n); sc_done(SC_CH, from, n); }
    else {                                            // no map to carry baselines through: take them as they are
        slots_reread();
        notes_rebaseline();
        meta_rebase_channels();
        InterlockedIncrement(&g_structGen);
    }
    logmsg("CHAN reordered (remote): %d moves, %s", steps, ok ? "order matches" : "ORDER MISMATCH");
    if (!ok) resync("FL didn't reorder the channels as asked");
}

// A new channel goes out at once, at its index i (n0: the others' channel count before it), with
// the state it has now; its settled state follows (identAt), as FL may still be loading its plugin.
static void chan_add_send(int i, int n0) {
    size_t n = 0, cap;
    unsigned char *d = unit_save(U_CH, i, 0, &n);
    char *f, *m, nm[80];
    if (!d) { resync("a new channel's state couldn't be saved"); return; }
    f = unit_json_fields(U_CH, i, 0, d, n);
    cap = strlen(f) + 128;
    m = (char *)malloc(cap);
    sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"chan.add\",\"n0\":%d,%s}}", n0, f);
    ws_send_samples(m, d, n);                       // lists (and uploads) the samples the state uses
    logmsg("CHAN added locally: index %d of %d%s (sent, %zu bytes)", i, n0 + 1, unit_name_note(U_CH, i, nm, sizeof(nm)), n);
    free(f); free(d);
}

// FL's channel list changed here: tell the others in steps they replay with FL's own functions.
// Deleted channels first (highest index first), then the rest's new order, then each new channel at
// its index, lowest first: FL's Clone puts the copy right after its source, undo puts deleted channels
// back where they were, a drop lands where it is dropped, Add appends. Each op carries the channel
// count it applies to, so a peer whose list differs notices (resync) instead of editing the wrong channels.
static void chan_struct_local(void **items, int n, DWORD now) {
    static void *K[MAX_CH];
    static int from[MAX_CH], added[MAX_CH], O[MAX_CH], all[MAX_CH];   // O: each K's index before this change
    int nk = g_nChKnown, k, i, j, nc = 0, na = 0;
    char m[64 + MAX_CH * 5];
    if (n > MAX_CH) n = MAX_CH;
    memcpy(K, g_chKnown, sizeof(void *) * (size_t)nk);
    for (k = 0; k < nk; k++) O[k] = k;
    for (k = nk - 1; k >= 0; k--) {
        if (index_of(items, n, K[k]) >= 0 && index_of(g_chDead, g_nChDead, K[k]) < 0) continue;   // (dead: its memory is a new channel's)
        sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"chan.del\",\"ch\":%d,\"n0\":%d}}", k, nk);
        ws_send(m);
        sc_done_del(k, nk);
        logmsg("CHAN deleted locally: index %d (sent)", k);
        units_shift_after_delete(k, nk);
        memmove(K + k, K + k + 1, sizeof(void *) * (size_t)(nk - k - 1));
        memmove(O + k, O + k + 1, sizeof(int) * (size_t)(nk - k - 1));
        nk--;
    }
    for (i = 0; i < n; i++) all[i] = (j = index_of(K, nk, items[i])) >= 0 ? O[j] : -1;   // new index -> old, the whole change
    for (i = 0; i < n; i++) if ((j = index_of(K, nk, items[i])) >= 0) from[nc++] = j;
    for (i = 0; i < nc && from[i] == i; i++) {}
    if (i < nc) {
        // moved (up/down, sort): the peers replay it with FL's own move, so channels keep their state
        int len = sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"chan.order\",\"n\":%d,\"from\":[", nc);
        for (i = 0; i < nc; i++) len += sprintf_s(m + len, sizeof(m) - len, "%s%d", i ? "," : "", from[i]);
        strcat_s(m, sizeof(m), "]}}");
        ws_send(m);
        sc_done(SC_CH, from, nc);
        units_permute(from, nc);
        logmsg("CHAN list reordered locally (sent chan.order)");
    }
    for (i = 0, k = nc; i < n; i++) {                 // k: how many channels the others have
        if (index_of(K, nk, items[i]) >= 0) continue;
        units_insert(i, k);
        g_chU[i].inst = plugin_inst(items[i]);
        g_chU[i].identAt = now;
        chan_add_send(i, k);
        sc_done_add(i, k);
        added[na++] = i;
        k++;
    }
    known_from_list();
    chan_rebase(all, n);                              // the peers replay the same change; edits made meanwhile still go out
    for (i = 0; i < na; i++) notes_send_channel(added[i]);
}

// Compare FL's channel list and plugin instances with what we knew.
#define STRUCT_DEFER_MS 2000
static volatile LONG g_structQueued = 0;  // the poll thread queued a structure check for a channel change
static void do_struct(void) {
    void **items; int n = chan_list(&items), i, t, s;
    DWORD now = GetTickCount();
    static DWORD deferSince = 0;
    InterlockedExchange(&g_structQueued, 0);
    if (!items) return;
    if (g_nChKnown < 0) {
        known_from_list();
        mix_known_take();
        for (i = 0; i < n; i++) g_chU[i].inst = plugin_inst(items[i]);
        for (t = 0; t < mix_count(); t++)
            for (s = 0; s < MAX_FX_SLOTS; s++) g_fxU[t][s].inst = plugin_inst(fx_obj(t, s));
        return;
    }
    if (g_nChKnown > 0) {
        // Every known channel gone (project load/new/close), or most of them at once: not a series of deletes.
        // A cancelled project load leaves FL with one channel (a tester's FL kept channel 0 of 44): sent as 43
        // deletes, it emptied the leader's song too. Most of the song gone in one tick is the project, not edits:
        // a member then loads the leader's project again, the leader sends its own.
        int k, j, common = 0, gone;
        for (k = 0; k < g_nChKnown; k++)
            for (j = 0; j < n; j++) if (items[j] == g_chKnown[k]) { common++; break; }
        gone = g_nChKnown - common;
        if (!common) { proj_begin("channel list replaced"); return; }
        if (gone >= PROJ_CH_MASS && gone * 2 > g_nChKnown) {
            logmsg("CHAN %d of %d channels gone at once: FL replaced or emptied the project (a load cancelled?), not sent as deletes", gone, g_nChKnown);
            proj_begin("most channels gone at once");
            return;
        }
    }
    if (closing_now()) return;
    if (!chan_list_changed()) deferSince = 0;
    else if (g_peerQueued > 0 && (!deferSince || now - deferSince < STRUCT_DEFER_MS)) {
        // the others' ops that came before this change still wait in the queue: they go first, as their
        // channel numbers don't count it yet (unless they keep coming for STRUCT_DEFER_MS)
        if (!deferSince) deferSince = now;
        return;
    } else { deferSince = 0; chan_struct_local(items, n, now); }
    mix_struct();                    // before the slot checks: inserted / deleted tracks move plugins along
    // plugin identity (add/replace) per channel and mixer slot
    for (i = 0; i < n; i++) {
        void *inst = plugin_inst(items[i]);
        Unit *u = &g_chU[i];
        if (inst != u->inst) {
            u->inst = inst;
            if (now < u->absorbUntil) continue;
            if (inst) { u->identAt = now; logmsg("PLUG identity changed: ch %d", i); }
        }
    }
    // Effects gone from every slot (not moved along with their track: FL's move copies whole track
    // records, so a moved track's effects are still there, one track over).
    int nm = mix_count(), gone = 0, nin = 0;
    static void *ins[MAX_MIX * MAX_FX_SLOTS];
    for (t = 0; t < nm; t++)
        for (s = 0; s < MAX_FX_SLOTS; s++)
            if ((ins[nin] = plugin_inst(fx_obj(t, s))) != NULL) nin++;
    qsort(ins, nin, sizeof(void *), ptr_cmp);
    for (t = 0; t < nm; t++)
        for (s = 0; s < MAX_FX_SLOTS; s++)
            if (g_fxU[t][s].inst && now >= g_fxU[t][s].absorbUntil && !bsearch(&g_fxU[t][s].inst, ins, nin, sizeof(void *), ptr_cmp)) gone++;
    if (gone >= PROJ_FX_MASS) { proj_begin("many effects removed at once"); return; }
    for (t = 0; t < nm; t++)
        for (s = 0; s < MAX_FX_SLOTS; s++) {
            Unit *u = &g_fxU[t][s];
            void *inst = plugin_inst(fx_obj(t, s));
            if (inst == u->inst) continue;
            void *was = u->inst;
            u->inst = inst;
            if (now < u->absorbUntil) continue;
            if (inst) { u->identAt = now; logmsg("PLUG identity changed: fx %d/%d", t, s); }
            else if (was) {
                char m[112];
                sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"fx.clear\",\"trk\":%d,\"slot\":%d}}", t, s);
                ws_send(m);
                u->identAt = 0; u->lastAct = 0; u->hash = 0;
                plug_agreed_set(U_FX, t, s, NULL, 0);
                logmsg("PLUG removed from fx %d/%d (sent fx.clear)", t, s);
            }
        }
}

// Leader: every unit's state, then the param values (join / project load).
static void do_full(void) {
    void **items; int n = chan_list(&items), i, t, s, cnt = 0;
    size_t cap = 256, len;
    char *m = (char *)malloc(cap);
    len = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"plug.full\",\"nch\":%d,\"items\":[", n);
    for (i = 0; i < n + mix_count() * MAX_FX_SLOTS; i++) {
        int unit = i < n ? U_CH : U_FX;
        int a = i < n ? i : (i - n) / MAX_FX_SLOTS, b = i < n ? 0 : (i - n) % MAX_FX_SLOTS;
        size_t dn = 0;
        unsigned char *d = unit_save(unit, a, b, &dn);
        if (!d) continue;
        Unit *u = unit_ptr(unit, a, b);
        if (u) u->hash = state_hash(d, dn);
        char *f = unit_json_fields(unit, a, b, d, dn);
        free(d);
        size_t fl = strlen(f);
        if (len + fl + 8 > cap) { cap = (len + fl + 8) * 2; m = (char *)realloc(m, cap); }
        len += sprintf_s(m + len, cap - len, "%s{%s}", cnt ? "," : "", f);
        free(f);
        cnt++;
    }
    if (len + 8 > cap) { cap = len + 8; m = (char *)realloc(m, cap); }
    strcat_s(m, cap, "]}}");
    ws_send(m);
    free(m);
    logmsg("PLUG full state sent: %d channels, %d units", n, cnt);
    (void)t; (void)s;
    send_notes_full();
    send_full_state();
}

// ---------------- notes ----------------
// Detection works per pattern: the poll thread hashes every pattern's note array (SEH-guarded,
// no lock: a torn read just shows up as a change that settles). What is sent, locked and
// applied is one CHANNEL's notes in one pattern ("pat:<p>:<ch>"): a settled pattern is diffed
// against what was last sent/applied (the unit's "base") and each channel that differs goes
// out on its own, so two people can edit different channels of the same pattern at once.
// Snapshots and applies run on the main thread under FL's own note lock, so the audio thread
// never sees a half-replaced pattern; an apply only touches the notes of its channel.
static uint64_t g_patHash[MAX_PAT];       // state last checked by do_notesnap (0 = no notes)
static uint64_t g_patSeen[MAX_PAT];       // last state the scanner saw
static DWORD    g_patChangedAt[MAX_PAT];  // when g_patSeen last moved (0 = settled/handled)
static volatile LONG g_notesReady = 0;    // baseline taken

#define NOTES_SCAN_MS   100
#define NOTES_SETTLE_MS 250     // a drag rewrites notes every frame: send once it stops
#define NOTES_BULK      8       // this many patterns changing at once: check for renumbered ones first

// Hash of note records with the selection bit masked out: selecting is not an edit.
static uint64_t notes_fnv(const unsigned char *p, unsigned n) {
    uint64_t h = 1469598103934665603ull;
    size_t i, len = (size_t)n * NOTE_SIZE;
    for (i = 0; i < len; i++) {
        unsigned char b = p[i];
        if (i % NOTE_SIZE == NOTE_SEL_OFS) b &= (unsigned char)~NOTE_SEL_BIT;
        h ^= b; h *= 1099511628211ull;
    }
    return h ? h : 1;
}

static void notes_clear_sel(unsigned char *d, unsigned n) {
    unsigned i;
    for (i = 0; i < n; i++) d[(size_t)i * NOTE_SIZE + NOTE_SEL_OFS] &= (unsigned char)~NOTE_SEL_BIT;
}

static char *pat_notes_arr(int p) {
    char *tbl = *(char **)(g_flBase + (PATARR_VA - IMG_BASE));
    return *(char **)(tbl + (size_t)p * PAT_STRIDE + PAT_NOTES);
}

static uint64_t notes_hash(int p) {
    __try {
        char *a = pat_notes_arr(p);
        if (!a) return 0;
        unsigned n = *(unsigned *)(a + NOTES_COUNT);
        if (!n) return 0;
        if (n > 1000000) return 1;
        return notes_fnv(*(unsigned char **)(a + NOTES_DATA), n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}

static CRITICAL_SECTION *note_lock(void) { return *(CRITICAL_SECTION **)(g_flBase + (NOTELOCK_VA - IMG_BASE)); }

// Copy of pattern p's note records (main thread, under FL's note lock). *n = count.
static unsigned char *notes_copy(int p, unsigned *n) {
    unsigned char *buf = NULL;
    *n = 0;
    EnterCriticalSection(note_lock());
    __try {
        char *a = pat_notes_arr(p);
        unsigned c = a ? *(unsigned *)(a + NOTES_COUNT) : 0;
        if (c && c <= 1000000) {
            buf = (unsigned char *)malloc((size_t)c * NOTE_SIZE);
            memcpy(buf, *(unsigned char **)(a + NOTES_DATA), (size_t)c * NOTE_SIZE);
            *n = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(buf); buf = NULL; *n = 0; }
    LeaveCriticalSection(note_lock());
    return buf;
}

// Take the current note state of every pattern as already known (no sends).
static void notes_rebaseline(void) {
    int p;
    for (p = 1; p < MAX_PAT; p++) { g_patSeen[p] = g_patHash[p] = notes_hash(p); g_patChangedAt[p] = 0; }
    clips_rebaseline();          // audio clips reference channels: same structural events
    meta_rebase_patterns();      // pattern names / markers move with pattern ops (the rest: chan_rebase, mix_rebaseline)
    units_capture_all();         // and it is the state a lock conflict reverts to
}

static void redraw_editors(void);

// Replace channel g's notes in pattern p with recs[0..n) (g < 0: the whole pattern). Main
// thread. Other channels' notes are left alone, so a peer's edit never disturbs what this user
// is doing elsewhere in the pattern. Selection is local: incoming selection bits are dropped, and
// notes this user had selected stay selected when an identical note is still there afterwards.
// What FL ends up with is the new agreed state of that channel.
static void notes_apply(int p, const unsigned char *recs, unsigned n, int g) {
    unsigned i, j, nold = 0, nnow = 0;
    int k;
    unsigned char *in = recs_group(1, recs, n, g, &n);
    unsigned char *old = NULL, *now;
    if (in) notes_clear_sel(in, n);
    // Creating a pattern's note array takes FL's song lock and then the note lock, the order FL's
    // audio thread uses too: create it before taking the note lock, or a pattern's first notes
    // arriving during playback deadlock FL (hang report 2026-10-21 175345).
    __try { if (n) g_noteArr(p, 1); } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("NOTES create faulted pat %d", p); }
    EnterCriticalSection(note_lock());
    __try {
        char *a = pat_notes_arr(p);
        if (a) {
            noteadd_t add = (noteadd_t)(*(void ***)a)[NOTEADD_VSLOT];
            unsigned c = *(unsigned *)(a + NOTES_COUNT);
            if (c && c <= 1000000) {
                old = (unsigned char *)malloc((size_t)c * NOTE_SIZE);
                memcpy(old, *(unsigned char **)(a + NOTES_DATA), (size_t)c * NOTE_SIZE);
                nold = c;
            }
            for (j = 0; j < nold && in; j++) {
                unsigned char o[NOTE_SIZE];
                memcpy(o, old + (size_t)j * NOTE_SIZE, NOTE_SIZE);
                if (!(o[NOTE_SEL_OFS] & NOTE_SEL_BIT) || (g >= 0 && (int)rec_group(1, o) != g)) continue;
                o[NOTE_SEL_OFS] &= (unsigned char)~NOTE_SEL_BIT;
                for (i = 0; i < n; i++) {
                    unsigned char *r = in + (size_t)i * NOTE_SIZE;
                    if (!(r[NOTE_SEL_OFS] & NOTE_SEL_BIT) && memcmp(r, o, NOTE_SIZE) == 0) { r[NOTE_SEL_OFS] |= NOTE_SEL_BIT; break; }
                }
            }
            // top down, so the indices still to delete stay valid
            for (k = (int)nold - 1; k >= 0; k--)
                if (g < 0 || (int)rec_group(1, old + (size_t)k * NOTE_SIZE) == g) g_noteDel(a, k, 0);
            {   // never a note on a channel this FL doesn't have (an edit made before channels changed that
                // couldn't be moved along): FL calls those invalid and asks about them in a modal on load
                void **items; int nc = chan_list(&items), bad = 0;
                for (i = 0; i < n; i++) {
                    if (*(uint16_t *)(in + (size_t)i * NOTE_SIZE + NOTE_CH_OFS) >= nc) { bad++; continue; }
                    add(a, in + (size_t)i * NOTE_SIZE);
                }
                if (bad) logmsg("NOTES pattern %d: %d notes for channels this FL doesn't have, left out", p, bad);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("NOTES apply faulted pat %d", p); }
    LeaveCriticalSection(note_lock());
    free(old); free(in);
    now = notes_copy(p, &nnow);
    if (now) notes_clear_sel(now, nnow);
    unit_agreed(1, p, now, nnow, g);
    free(now);
    {
        void **items; int nc = chan_list(&items);
        __try {
            for (k = 0; k < nc; k++) {
                void *grid = *(void **)((char *)items[k] + CH_GRID);
                if (grid) g_gridRefresh(grid);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("NOTES grid refresh faulted"); }
    }
    pat_len_recalc(p);
    redraw_editors();
    // g_patSeen / g_patHash are left as they are: the scanner sees the change, and do_notesnap
    // finds this channel equal to its base (nothing echoed) but still sends a local edit that
    // was waiting to settle in another channel.
}

// g >= 0: one channel ("ch" field); g < 0: a whole pattern (join / project).
static char *notes_json_item(int p, int g, const unsigned char *d, unsigned n) {
    size_t cap = 64 + ((size_t)n * NOTE_SIZE + 2) / 3 * 4 + 1;
    char *j = (char *)malloc(cap);
    int o = g >= 0 ? sprintf_s(j, cap, "\"pat\":%d,\"ch\":%d,\"data\":\"", p, g) : sprintf_s(j, cap, "\"pat\":%d,\"data\":\"", p);
    b64enc(d ? d : (const unsigned char *)"", (int)(n * NOTE_SIZE), j + o);
    strcat_s(j, cap, "\"");
    return j;
}

#define GROUP_MAX 1024

// Channel g's notes in pattern p (d: the pattern's records) go out under that channel's lock.
static void notes_send_group(int p, int g, const unsigned char *d, unsigned n) {
    char key[32], *f, *m;
    unsigned gn;
    unsigned char *gd;
    size_t cap;
    sprintf_s(key, sizeof(key), "pat:%d:%d", p, g);
    if (!unit_before_send(key, 1, p, d, n, g)) { unit_revert(key); return; }
    gd = recs_group(1, d, n, g, &gn);
    f = notes_json_item(p, g, gd, gn);
    cap = strlen(f) + 96;
    m = (char *)malloc(cap);
    sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"notes.set\",\"key\":\"%s\",%s}}", key, f);
    ws_send(m);
    free(m); free(f); free(gd);
    logmsg("NOTES sent pattern %d channel %d (%u notes)", p, g, gn);
}

// A pattern's notes settled: send each channel whose notes differ from what was last
// sent or applied, under that channel's lock.
static void do_notesnap(const QItem *it) {
    int p = it->a, gs[GROUP_MAX], ng, i;
    unsigned n, bn;
    unsigned char *d, *b;
    // FL renumbered notes that chan_rebase hasn't yet (queued just before a channel insert / delete):
    // they'd go out under numbers the others don't have yet. The scanner tries again after it.
    if (chan_list_changed()) { g_patChangedAt[p] = GetTickCount(); return; }
    d = notes_copy(p, &n);
    if (d) notes_clear_sel(d, n);
    if (!unit_base(1, p, &b, &bn)) {              // no base yet: this is it
        unit_agreed(1, p, d, n, -1);
        g_patHash[p] = n ? notes_fnv(d, n) : 0;
        free(d);
        return;
    }
    ng = group_diff(1, d, n, b, bn, gs, GROUP_MAX);
    free(b);
    for (i = 0; i < ng; i++) notes_send_group(p, gs[i], d, n);
    g_patHash[p] = n ? notes_fnv(d, n) : 0;
    free(d);
}

// A channel FL just inserted may come with notes (a clone of one that has some): after its chan.add,
// they go out like an edit (the rebaseline took them as known).
static void notes_send_channel(int c) {
    int p;
    for (p = 1; p < MAX_PAT; p++) {
        unsigned n, gn = 0;
        unsigned char *d, *gd;
        if (!notes_hash(p) || !(d = notes_copy(p, &n))) continue;
        notes_clear_sel(d, n);
        gd = recs_group(1, d, n, c, &gn);
        if (gn) notes_send_group(p, c, d, n);
        free(gd); free(d);
    }
}

// it->b: the channel (-1 = the whole pattern)
static void do_notes(const QItem *it) {
    notes_apply(it->a, (const unsigned char *)it->order, (unsigned)it->mode, it->b);
    logmsg("NOTES applied pattern %d channel %d (%d notes)", it->a, it->b, it->mode);
}

// Join: patterns the leader has no notes in must be empty here too.
static void do_notesync(const QItem *it) {
    const unsigned char *keep = (const unsigned char *)it->order;
    int p, n = 0;
    for (p = 1; p < MAX_PAT; p++) {
        if (keep[p] || !notes_hash(p)) continue;
        notes_apply(p, NULL, 0, -1);
        n++;
    }
    if (n) logmsg("NOTES join: cleared %d patterns the leader has empty", n);
}

// Leader: every pattern that has notes (join / project load).
static void send_notes_full(void) {
    size_t cap = 256, len;
    char *m = (char *)malloc(cap);
    int p, cnt = 0;
    len = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"notes.full\",\"items\":[");
    for (p = 1; p < MAX_PAT; p++) {
        unsigned n;
        if (!notes_hash(p)) continue;
        unsigned char *d = notes_copy(p, &n);
        g_patSeen[p] = g_patHash[p] = n ? notes_fnv(d, n) : 0;
        if (!n) { free(d); continue; }
        notes_clear_sel(d, n);
        char *f = notes_json_item(p, -1, d, n);
        free(d);
        size_t fl = strlen(f);
        if (len + fl + 8 > cap) { cap = (len + fl + 8) * 2; m = (char *)realloc(m, cap); }
        len += sprintf_s(m + len, cap - len, "%s{%s}", cnt ? "," : "", f);
        free(f);
        cnt++;
    }
    if (len + 8 > cap) { cap = len + 8; m = (char *)realloc(m, cap); }
    strcat_s(m, cap, "]}}");
    ws_send(m);
    free(m);
    logmsg("NOTES full state sent: %d patterns", cnt);
}

// Poll thread: find patterns whose notes changed locally and have settled.
static void scan_notes(DWORD now) {
    static DWORD last = 0;
    static int pend[MAX_PAT];
    int p, np = 0, i, moving;
    if (now - last < NOTES_SCAN_MS || g_patDepth) return;
    last = now;
    if (!g_notesReady) { notes_rebaseline(); g_notesReady = 1; return; }
    // a channel inserted / deleted renumbers the notes of every later channel: nothing settles before
    // chan_rebase has renumbered the bases too (and its chan op went out)
    moving = chan_list_changed();
    for (p = 1; p < MAX_PAT; p++) {
        uint64_t h = notes_hash(p);
        if (h != g_patSeen[p]) { g_patSeen[p] = h; g_patChangedAt[p] = now; }
        if (!moving && g_patChangedAt[p] && now - g_patChangedAt[p] >= NOTES_SETTLE_MS) {
            g_patChangedAt[p] = 0;
            if (g_patSeen[p] != g_patHash[p]) pend[np++] = p;
        }
    }
    if (np >= NOTES_BULK) {
        // renumbered patterns still match their base: only the others are changes (an edit made in
        // the same moment went into the bulk below with them, and was never sent)
        int k = 0;
        for (i = 0; i < np; i++)
            if (pat_matches_base(pend[i])) g_patHash[pend[i]] = g_patSeen[pend[i]];
            else pend[k++] = pend[i];
        np = k;
    }
    // The rest are edits, however many patterns (a transpose across many of them). A project load / new
    // never gets here: this scan pauses while FL loads and in project mode (proj_begin: the param bulk
    // guard, the channel list replaced), and notes are re-baselined when that ends.
    for (i = 0; i < np; i++) enqueue_unit(T_NOTESNAP, 0, pend[i], 0, 0, NULL);
}

static unsigned char *b64dec(const char *s, size_t n, size_t *outn) {
    unsigned char *out = (unsigned char *)malloc(n / 4 * 3 + 3);
    size_t o = 0, i;
    int acc = 0, bits = 0;
    for (i = 0; i < n; i++) {
        int v = b64val(s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)((acc >> bits) & 0xff); }
    }
    *outn = o;
    return out;
}

// Parse one {"pat":P[,"ch":C],"data":"..."} starting at p; queue its apply. Returns the end or NULL.
static const char *parse_notes_item(const char *p, unsigned char *keep) {
    int pat, ch = -1;
    const char *q = strstr(p, "\"pat\":");
    if (!q) return NULL;
    pat = atoi(q + 6);
    const char *d = strstr(q, "\"data\":\"");
    if (!d) return NULL;
    int_after(q, d, "\"ch\":", &ch);
    d += 8;
    const char *e = strchr(d, '"');
    if (!e) return NULL;
    if (pat >= 1 && pat < MAX_PAT) {
        size_t n;
        unsigned char *recs = b64dec(d, (size_t)(e - d), &n);
        QItem it; memset(&it, 0, sizeof(it));
        it.type = T_NOTES; it.a = pat; it.b = ch < 0 || ch > 0xffff ? -1 : ch; it.mode = (int)(n / NOTE_SIZE); it.order = (int *)recs;
        enqueue(&it);
        if (keep) keep[pat] = 1;
    }
    return e + 1;
}

// ---------------- playlist clips ----------------
// Clips of one arrangement (records = FLP event 233 layout + runtime tail). Same scheme as
// notes: the whole arrangement is hashed from the poll thread, but what is sent, locked and
// applied is one playlist TRACK's clips ("clips:<arr>:<track>"), so several people can arrange
// on different tracks at once. Copy/replace run on the main thread under FL's note lock, then
// FL's own playlist refresh (what its clip-properties dialog calls). Comparisons ignore the
// selection bit and the per-instance clip id, and are order-independent (FL orders clips at
// the same position by insertion, which differs between peers).
static uint64_t g_clipHash = 0, g_clipSeen = 0;
static DWORD    g_clipChangedAt = 0;
static int      g_clipArr = -1;                  // arrangement the baseline belongs to
static void    *g_clipArrObj;                   //   that arrangement (its number is another's after an add / delete)
static DWORD    g_clipFlushAt;                   // an unsent edit in it went out on a switch (scan_clips), when

static int clip_cmp(const void *a, const void *b) { return memcmp(a, b, CLIP_SIZE); }

static char *arr_clips(int arr) {
    char *arrs = *(char **)(g_flBase + (ARRS_VA - IMG_BASE));
    if (!arrs || arr < 0 || arr >= (int)*(int64_t *)(arrs - 8)) return NULL;
    char *a = *(char **)(arrs + (size_t)arr * 8);
    return a ? *(char **)(a + ARR_CLIPS) : NULL;
}
static int cur_arr(void) {
    __try { return *(int *)(g_flBase + (ARRCUR_VA - IMG_BASE)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Masked, sorted copy -> hash. d may be modified (it is a private copy).
static uint64_t clips_fnv(unsigned char *d, unsigned n) {
    unsigned i;
    if (!n) return 0;
    for (i = 0; i < n; i++) {
        unsigned char *r = d + (size_t)i * CLIP_SIZE;
        r[CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
        memset(r + CLIP_ID_OFS, 0, 4);
    }
    qsort(d, n, CLIP_SIZE, clip_cmp);
    return fnv64(d, (size_t)n * CLIP_SIZE);
}

// Called from the poll thread and the main thread: private buffer per call.
static uint64_t clips_hash(int arr) {
    unsigned char *buf = NULL;
    uint64_t h = 0;
    __try {
        char *ca = arr_clips(arr);
        unsigned n = ca ? *(unsigned *)(ca + CLIPS_COUNT) : 0;
        if (n > 200000) h = 1;
        else if (n) {
            buf = (unsigned char *)malloc((size_t)n * CLIP_SIZE);
            memcpy(buf, *(unsigned char **)(ca + CLIPS_DATA), (size_t)n * CLIP_SIZE);
            h = clips_fnv(buf, n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { h = 1; }
    free(buf);
    return h;
}

static void clips_rebaseline(void) {
    g_clipArr = cur_arr();
    g_clipArrObj = arr_obj(g_clipArr);
    g_clipFlushAt = 0;
    g_clipSeen = g_clipHash = clips_hash(g_clipArr);
    g_clipChangedAt = 0;
    clips_capture(g_clipArr);
}

// Copy of an arrangement's clips (main thread, under FL's note lock).
static unsigned char *clips_copy(int arr, unsigned *n) {
    unsigned char *buf = NULL;
    *n = 0;
    EnterCriticalSection(note_lock());
    __try {
        char *ca = arr_clips(arr);
        unsigned c = ca ? *(unsigned *)(ca + CLIPS_COUNT) : 0;
        if (c && c <= 200000) {
            buf = (unsigned char *)malloc((size_t)c * CLIP_SIZE);
            memcpy(buf, *(unsigned char **)(ca + CLIPS_DATA), (size_t)c * CLIP_SIZE);
            *n = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(buf); buf = NULL; *n = 0; }
    LeaveCriticalSection(note_lock());
    return buf;
}

static void playlist_refresh(void) {
    __try {
        void *form = **(void ***)(g_flBase + (PLFORM_VA - IMG_BASE));
        if (form) ((formfn_t)(g_flBase + (PLREFRESH_VA - IMG_BASE)))(form);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("CLIPS refresh faulted"); }
}

// Replace playlist track g's clips in an arrangement with recs[0..n) (g < 0: every clip; main
// thread). Clips on other tracks are left alone. Selection stays local: clips this user had
// selected stay selected if an identical clip is still there. The result is the agreed state.
static void clips_apply(int arr, const unsigned char *recs, unsigned n, int g) {
    unsigned i, j, nold = 0, nnow = 0;
    unsigned char *in = recs_group(0, recs, n, g, &n);
    unsigned char *old = NULL, *now;
    for (i = 0; i < n; i++) in[(size_t)i * CLIP_SIZE + CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
    EnterCriticalSection(note_lock());
    __try {
        char *ca = arr_clips(arr);
        if (ca) {
            void **vmt = *(void ***)ca;
            clipadd_t add = (clipadd_t)vmt[CLIPADD_VSLOT];
            clippurge_t purge = (clippurge_t)vmt[CLIPPURGE_VSLOT];
            unsigned c = *(unsigned *)(ca + CLIPS_COUNT);
            unsigned char *d = *(unsigned char **)(ca + CLIPS_DATA);
            if (c && c <= 200000) {
                old = (unsigned char *)malloc((size_t)c * CLIP_SIZE);
                memcpy(old, d, (size_t)c * CLIP_SIZE);
                nold = c;
            }
            for (j = 0; j < nold; j++) {
                unsigned char *o = old + (size_t)j * CLIP_SIZE;
                if (!(o[CLIP_SEL_OFS] & CLIP_SEL_BIT) || (g >= 0 && (int)rec_group(0, o) != g)) continue;
                o[CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
                for (i = 0; i < n; i++) {
                    unsigned char *r = in + (size_t)i * CLIP_SIZE;
                    if (!(r[CLIP_SEL_OFS] & CLIP_SEL_BIT) && memcmp(r, o, CLIP_ID_OFS) == 0 &&
                        memcmp(r + CLIP_ID_OFS + 4, o + CLIP_ID_OFS + 4, CLIP_SIZE - CLIP_ID_OFS - 4) == 0) {
                        r[CLIP_SEL_OFS] |= CLIP_SEL_BIT; break;
                    }
                }
            }
            // FL's own delete: mark the clips deleted, then purge (also frees their resources)
            for (j = 0; j < nold; j++)
                if (g < 0 || (int)rec_group(0, old + (size_t)j * CLIP_SIZE) == g)
                    *(int32_t *)(d + (size_t)j * CLIP_SIZE + CLIP_DEL_OFS) = -1;
            if (nold) purge(ca);
            for (i = 0; i < n; i++) add(ca, in + (size_t)i * CLIP_SIZE);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("CLIPS apply faulted arr %d", arr); }
    LeaveCriticalSection(note_lock());
    free(old); free(in);
    playlist_refresh();
    now = clips_copy(arr, &nnow);
    for (i = 0; i < nnow; i++) now[(size_t)i * CLIP_SIZE + CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
    unit_agreed(0, arr, now, nnow, g);
    free(now);
    // like notes: the scanner sees the change and do_clipsnap sends only real local edits
}

// The current arrangement's clips settled: send each playlist track whose clips differ from
// what was last sent or applied, under that track's lock.
static void do_clipsnap(const QItem *it) {
    int gs[GROUP_MAX], ng, k;
    unsigned n, bn, i;
    unsigned char *d = clips_copy(it->a, &n), *b, *h = n ? (unsigned char *)malloc((size_t)n * CLIP_SIZE) : NULL;
    uint64_t hv;
    if (h) memcpy(h, d, (size_t)n * CLIP_SIZE);
    hv = n ? clips_fnv(h, n) : 0;
    free(h);
    if (it->a != g_clipArr) { free(d); return; }
    for (i = 0; i < n; i++) d[(size_t)i * CLIP_SIZE + CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
    if (!unit_base(0, it->a, &b, &bn)) { unit_agreed(0, it->a, d, n, -1); g_clipHash = hv; free(d); return; }
    if (it->mode == 1) {                         // FL's own change (scan_clips): what it shows now is the agreed state
        if (group_diff(0, d, n, b, bn, gs, GROUP_MAX) > 0) logmsg("CLIPS changed with nobody at the keyboard here (FL's own): taken as known, not sent");
        unit_agreed(0, it->a, d, n, -1); g_clipHash = hv; free(b); free(d); return;
    }
    ng = group_diff(0, d, n, b, bn, gs, GROUP_MAX);
    for (k = 0; k < ng; k++) {
        char key[32];
        unsigned gn;
        unsigned char *gd;
        sprintf_s(key, sizeof(key), "clips:%d:%d", it->a, gs[k]);
        if (!unit_before_send(key, 0, it->a, d, n, gs[k])) { unit_revert(key); continue; }
        gd = recs_group(0, d, n, gs[k], &gn);
        size_t cap = 128 + ((size_t)gn * CLIP_SIZE + 2) / 3 * 4;
        char *m = (char *)malloc(cap);
        int o = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"clips.set\",\"key\":\"%s\",\"arr\":%d,\"trk\":%d,\"data\":\"",
                          key, it->a, gs[k]);
        b64enc(gd ? gd : (const unsigned char *)"", (int)(gn * CLIP_SIZE), m + o);
        strcat_s(m, cap, "\"}}");
        ws_send(m);
        free(m);
        logmsg("CLIPS sent arrangement %d track %d (%u clips)", it->a, gs[k], gn);
        {   // a clip edit can change its channel too (an audio clip stretched in the playlist sets the sample's stretch,
            // a state-only setting): each channel whose clips changed here counts as worked in, so its state is checked
            unsigned bg = 0, x, y;
            unsigned char *bd = recs_group(0, b, bn, gs[k], &bg);
            for (x = 0; x < gn; x++) {
                const unsigned char *r = gd + (size_t)x * CLIP_SIZE;
                uint32_t item = *(const uint32_t *)(r + 4);
                for (y = 0; y < bg; y++) {
                    const unsigned char *o = bd + (size_t)y * CLIP_SIZE;
                    if (memcmp(r, o, CLIP_ID_OFS) == 0 && memcmp(r + CLIP_ID_OFS + 4, o + CLIP_ID_OFS + 4, CLIP_SIZE - CLIP_ID_OFS - 4) == 0) break;
                }
                if (y == bg && (item & 0xFFFFu) == 0x5000u && (item >> 16) < 0x5000u) unit_worked(U_CH, (int)(item >> 16), 0);
            }
            free(bd);
        }
        free(gd);
    }
    free(b);
    g_clipHash = hv;
    free(d);
}

// A peer's clips naming a channel this FL doesn't have yet (its chan.add is still being made ready here, e.g.
// its sample): added, FL's playlist read a channel that isn't there as it drew them and FL crashed (a tester,
// 2026-10-27: an audio clip dropped in with its new channel). Held until the channel is here (clips_hold_retry
// after a remote chan.add); a newer op for the same playlist track replaces it, the leader's project drops them.
// ponytail: channel moves while one is held aren't followed (the next chan.add or project load sorts it out).
#define CLIP_HOLD_MAX 16
typedef struct { int arr, trk; unsigned n; unsigned char *d; } ClipHold;
static ClipHold g_clipHold[CLIP_HOLD_MAX];
static int g_nClipHold;
static int clips_missing_ch(const unsigned char *d, unsigned n) {
    void **items;
    int nch = chan_list(&items);
    unsigned i;
    for (i = 0; i < n; i++) {
        uint32_t item = *(const uint32_t *)(d + (size_t)i * CLIP_SIZE + 4);
        if ((item & 0xFFFFu) == 0x5000u && (item >> 16) < 0x5000u && (int)(item >> 16) >= nch) return 1;
    }
    return 0;
}
static void clips_hold_drop(int arr, int trk) {   // trk -2: all
    int i, k = 0;
    for (i = 0; i < g_nClipHold; i++)
        if (trk == -2 || (g_clipHold[i].arr == arr && (g_clipHold[i].trk == trk || trk == -1 || g_clipHold[i].trk == -1))) free(g_clipHold[i].d);
        else g_clipHold[k++] = g_clipHold[i];
    g_nClipHold = k;
}
static void clips_hold_retry(void) {
    int i, k = 0;
    for (i = 0; i < g_nClipHold; i++) {
        ClipHold *h = &g_clipHold[i];
        if (clips_missing_ch(h->d, h->n)) { g_clipHold[k++] = *h; continue; }
        clips_apply(h->arr, h->d, h->n, h->trk);
        logmsg("CLIPS applied arrangement %d track %d (%u clips): their channel is here now", h->arr, h->trk, h->n);
        free(h->d);
    }
    g_nClipHold = k;
}
// it->b: the playlist track (-1 = every clip)
static void do_clips(const QItem *it) {
    clips_hold_drop(it->a, it->b);
    if (clips_missing_ch((const unsigned char *)it->order, (unsigned)it->mode)) {
        ClipHold *h;
        if (g_nClipHold == CLIP_HOLD_MAX) { free(g_clipHold[0].d); memmove(g_clipHold, g_clipHold + 1, sizeof(ClipHold) * --g_nClipHold); }
        h = &g_clipHold[g_nClipHold++];
        h->arr = it->a; h->trk = it->b; h->n = (unsigned)it->mode;
        h->d = (unsigned char *)malloc((size_t)h->n * CLIP_SIZE + 1);
        memcpy(h->d, it->order, (size_t)h->n * CLIP_SIZE);
        logmsg("CLIPS for arrangement %d track %d name a channel this FL doesn't have yet: held until it's here", it->a, it->b);
        return;
    }
    clips_apply(it->a, (const unsigned char *)it->order, (unsigned)it->mode, it->b);
    logmsg("CLIPS applied arrangement %d track %d (%d clips)", it->a, it->b, it->mode);
}


// Poll thread: the current arrangement's clips changed locally and settled?
static void scan_clips(DWORD now) {
    static DWORD last = 0;
    static int byUser = 0;                       // local input while the current change came in
    int arr;
    uint64_t h;
    if (now - last < NOTES_SCAN_MS || g_patDepth) return;
    last = now;
    arr = cur_arr();
    if (arr != g_clipArr) {                                    // switched arrangement: new baseline,
        // after an edit in the old one still unsent goes out (taking the new baseline at once dropped it: an edit, then
        // a switch). Only while the old one is the same arrangement: after an add / delete its number is another's
        // (do_arrstruct takes the baselines then).
        if (g_clipArr >= 0 && g_clipArr < arr_count() && arr_obj(g_clipArr) == g_clipArrObj && clips_hash(g_clipArr) != g_clipHash) {
            if (!g_clipFlushAt) {
                g_clipFlushAt = now | 1;
                enqueue_unit(T_CLIPSNAP, 0, g_clipArr, 0, byUser || input_recent(now) ? 0 : 1, NULL);
                return;
            }
            if (now - g_clipFlushAt < 3000) return;
        }
        clips_rebaseline();
        return;
    }
    h = clips_hash(arr);
    if (h != g_clipSeen) {
        if (!g_clipChangedAt) byUser = 0;
        g_clipSeen = h; g_clipChangedAt = now;
        if (input_recent(now)) byUser = 1;
    }
    if (g_clipChangedAt && now - g_clipChangedAt >= NOTES_SETTLE_MS) {
        // a new automation clip / audio clip channel: its chan.add must go out first
        if (chan_list_changed()) return;
        g_clipChangedAt = 0;
        // nobody here at the keyboard: FL changed clips by itself (it recomputes a stretched audio clip from its
        // channel after a peer's clips are applied). Sent, the other FL recomputed its own and sent that back: the
        // two ping-ponged. Taken as known instead (mode 1), like a param that moves by itself.
        if (g_clipSeen != g_clipHash) enqueue_unit(T_CLIPSNAP, 0, arr, 0, byUser ? 0 : 1, NULL);
    }
}

// Inbound clips.set (recv thread).
static void on_clips_set(const char *m) {
    int arr = -1, trk = -1;
    const char *q = strstr(m, "\"arr\":"), *d = strstr(m, "\"data\":\"");
    if (!q || !d) return;
    arr = atoi(q + 6);
    int_after(m, d, "\"trk\":", &trk);
    d += 8;
    const char *e = strchr(d, '"');
    if (!e) return;
    size_t n;
    unsigned char *recs = b64dec(d, (size_t)(e - d), &n);
    QItem it; memset(&it, 0, sizeof(it));
    it.type = T_CLIPS; it.a = arr; it.b = trk < 0 || trk > 0xffff ? -1 : trk; it.mode = (int)(n / CLIP_SIZE); it.order = (int *)recs;
    enqueue(&it);
}

// ---------------- names, colors, mixer routing ("meta") ----------------
// Small per-entity state: mixer track name + color, mixer track routing (sends on/off +
// level), channel name + color, pattern name + color + length, playlist track name + color
// (every arrangement). Same scheme as notes: hashes from the poll thread, sent once settled,
// applied on the main thread with FL's own setters (the ones its Python API calls). Joins and
// project loads carry all of this in the .flp, so only live edits travel. No locks (renames
// are rare); if both sides change the same thing at once, the leader's value wins.
#define MIXNAME_VA    FLVA(MIXNAME) // SetMixerTrackName(track, name, 1): also renames linked playlist tracks
#define MIXCOLOR_VA   FLVA(MIXCOLOR) // SetMixerTrackColor(track record, color)
#define UIUPDATE_VA   FLVA(UIUPDATE) // UIUpdate(what, 0); 0x18 = mixer (what FL calls after a mixer color)
#define MIXOBJ_VA     FLVA(MIXOBJ) // global -> var -> mixer object (routing calls take it)
#define SETROUTE_VA   FLVA(SETROUTE) // SetRoute(mixer, src, dst, on, 1) -> int
#define ROUTEUPD_VA   FLVA(ROUTEUPD) // RoutingChanged(mixer): after route / send level edits
#define MIX_NAME      0x0c         // mixer track: UnicodeString name ('' = "Insert N")
#define MIX_COLOR     0x04
#define MIX_ROUTE     0x394        // + dst*8: int send level (12800 = 100%); +4: byte, 1 = routed
#define MIX_ROUTE_MAX 502
#define CH_NAME       0x50         // channel object: UnicodeString name
#define CH_COLOR      0x78
#define CH_SETNAME_VSLOT  (0x70 / 8)   // channel vmt: SetName(self, name)
#define CH_SETCOLOR_VSLOT (0x78 / 8)   //              SetColor(self, color)
#define PAT_NAME      0x00         // pattern record: UnicodeString name ('' = "Pattern N")
#define PAT_COLOR     0x08
#define PAT_LENGTH    0x44         // int, <= 0 = automatic (ticks: PPQ * beats)
#define PATNAME_VA    FLVA(PATNAME) // SetPatternName(pattern, name)
#define PATCOLOR_VA   FLVA(PATCOLOR) // SetPatternColor(pattern, color): also sets the "custom color" flag
#define PATREFRESH_VA FLVA(PATREFRESH) // what FL calls on (playlist form +0xd34) after a pattern rename
#define PLFORM_PATS   0xd34
#define PL_TRK_STRIDE 0x114        // arrangement object: playlist track t (1..500) at + t*0x114
#define PL_TRK_NAME   0x24
#define PL_TRK_COLOR  0x2c
#define PL_TRK_MAX    500
#define PLTRACK_VA    FLVA(PLTRACK) // SetPlaylistTrack(arrangement, track, name, color)
// playlist markers (per arrangement) and the project time signature
#define ARR_MARKERS   0x21c48      // arrangement object: dynamic array of marker records (stride 0x34)
#define MARK_SIZE     0x34         // +0 position, +4 action (8 = time signature), +8 UnicodeString name,
#define MARK_TAIL     0x10         //   +0x10.. plain data (+0x10 numerator, +0x11 denominator)
#define MARK_MAX      4096
#define MARKADD_VA    FLVA(MARKADD) // MarkerListAdd(&list, pos, name, action, num, den): appends a record
#define MARKTYPE_VA   FLVA(MARKTYPE) // -> RTTI of the marker record array (TTimeMarker dynarray, for DynArraySetLength): FL loads it from here too
#define DYNSETLEN_VA  FLVA(DYNSETLEN) // Delphi DynArraySetLength(&arr, typeinfo, 1, newLength)
#define MARKREFRESH_VA FLVA(MARKREFRESH) // MarkersChanged(editor form): sort, number, redraw, time signature from markers
#define PL_MARKSEL    0xd8c        // playlist form: index of the marker being edited (-1 = none)
// pattern markers (piano roll): same records, one list per pattern; a pattern's own time signature
// (pattern menu > Time signature) is an action-8 marker at 0 in this list
#define PAT_MARKERS   0x30         // pattern record: dynamic array of marker records
#define MARKSORT_VA   FLVA(MARKSORT) // MarkerListTables(&list): builds the time signature table at list+8 (the pattern's +0x38)
#define PATLENUPD_VA  FLVA(PATLENUPD) // UpdatePatternLength(pattern, 0): bar length from the pattern's time signature
#define PATMARKS_VA   FLVA(PATMARKS) // PatternMarkersChanged(pattern record): redraws the editors showing them
#define SONGLOCK_VA   FLVA(SONGLOCK) // global: CRITICAL_SECTION* FL holds while editing markers / time signature
#define MODIFIED_VA   FLVA(MODIFIED) // SetModified(1): project changed
#define TSNUM_VA      FLVA(TSNUM)  // global -> int: project time signature numerator
#define TSDEN_VA      FLVA(TSDEN)  //                                       denominator
#define TSNUMSET_VA   FLVA(TSNUMSET) // SetNumerator(main form, n): what general.setNumerator calls (controls + globals)
#define TSDENSET_VA   FLVA(TSDENSET) // SetDenominator(main form, d): what general.setDenominator calls
#define PATLENSET_VA  FLVA(PATLENSET) // PatternLengthChanged(pattern): what patterns.setPatternLength calls after the write
#define META_ARR_MAX  32          // arrangements whose playlist tracks and markers sync (was 8)
#define META_MAX      (META_ARR_MAX * 512)
#define META_SCAN_MS  150
#define META_SETTLE_MS 300         // some dialogs rename per keystroke: send once it stops
#define META_BULK     24           // this many entities changing at once = project load / pattern insert
#define META_CONFLICT_MS 1500      // leader: a remote change this soon after our own send of the same thing loses

typedef void (*mixname_t)(int track, const wchar_t *name, uint8_t update);
typedef void (*mixcolor_t)(void *rec, uint32_t color);
typedef void (*uiupdate_t)(int what, uint64_t flag);
typedef int  (*setroute_t)(void *mixer, int src, int dst, int on, int flag);
typedef void (*mixerfn_t)(void *mixer);
typedef void (*chsetname_t)(void *ch, const wchar_t *name);
typedef void (*chsetcolor_t)(void *ch, uint32_t color);
typedef void (*patname_t)(int pat, const wchar_t *name);
typedef void (*patcolor_t)(int pat, uint32_t color);
typedef void (*patrefresh_t)(void *obj, uint8_t a, uint8_t b);
typedef void (*pltrack_t)(void *arr, int track, const wchar_t *name, uint32_t color);
typedef void (*markadd_t)(void *list, int pos, const wchar_t *name, int action, uint8_t num, uint8_t den);
typedef void (*dynsetlen_t)(void *arr, void *typeinfo, int64_t dims, int64_t len);
typedef void (*setmod_t)(int on);
typedef void (*patlenupd_t)(int pat, int flag);
// FL caches a pattern's length in its record (+0x48 / +0x4c / +0x50 / +0x54 / +0x5c), works it out again
// after its own note edits (PATLENUPD(p, 1), as PatternLengthChanged does) and sizes the pattern's playlist
// clips from it. After notes applied from someone else it stayed stale: that FL cut the clips back to its
// old length and fought the editor over them. Main thread.
static void pat_len_recalc(int p) {
    __try { ((patlenupd_t)(g_flBase + (PATLENUPD_VA - IMG_BASE)))(p, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("NOTES pattern %d length update faulted", p); }
}

enum { M_MIX, M_ROUTE, M_CHAN, M_PAT, M_PLT, M_MARK, M_TSIG, M_PMARK, M_NTYPES };
static const char *const M_TAG[M_NTYPES] = { "mx", "rt", "ch", "pt", "pl", "mk", "ts", "pm" };
static uint64_t g_mHash[M_NTYPES][META_MAX];   // last state sent or applied
static uint64_t g_mSeen[M_NTYPES][META_MAX];   // last state the scanner saw
static DWORD    g_mAt[M_NTYPES][META_MAX];     // when g_mSeen last moved (0 = settled/handled)
static DWORD    g_mSentAt[M_NTYPES][META_MAX];
static volatile LONG g_metaReady = 0;

static uint64_t fnv_add(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
// Length of a Delphi UnicodeString (NULL = '').
static int ustr_len(const wchar_t *s) {
    int n = s ? *(const int32_t *)((const char *)s - 4) : 0;
    return n < 0 || n > 4096 ? 0 : n;
}
static uint64_t fnv_ustr(uint64_t h, const wchar_t *s) {
    int n = ustr_len(s);
    h = fnv_add(h, &n, 4);
    return n ? fnv_add(h, s, (size_t)n * 2) : h;
}
static int ustr_eq(const wchar_t *cur, const wchar_t *w) {
    int n = ustr_len(cur);
    return n == (int)wcslen(w) && (n == 0 || memcmp(cur, w, (size_t)n * 2) == 0);
}

static int meta_cap(int type) {
    switch (type) {
    case M_MIX: case M_ROUTE: return MAX_MIX;
    case M_CHAN: return MAX_CH;
    case M_PAT: case M_PMARK: return MAX_PAT;
    case M_MARK: return META_ARR_MAX;
    case M_TSIG: return 1;
    default: return META_MAX;
    }
}
static char *mix_rec(int t) { return *(char **)(g_flBase + (MIXARR_VA - IMG_BASE)) + (size_t)t * MIX_STRIDE; }
static char *pat_rec(int p) { return *(char **)(g_flBase + (PATARR_VA - IMG_BASE)) + (size_t)p * PAT_STRIDE; }
static int arr_count(void) {
    __try {
        char *arrs = *(char **)(g_flBase + (ARRS_VA - IMG_BASE));
        int n = arrs ? (int)*(int64_t *)(arrs - 8) : 0;
        return n < 0 ? 0 : (n > META_ARR_MAX ? META_ARR_MAX : n);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static char *arr_obj(int arr) {
    char *arrs = *(char **)(g_flBase + (ARRS_VA - IMG_BASE));
    return *(char **)(arrs + (size_t)arr * 8);
}
// Marker array of an arrangement (Delphi dynamic array: length as int64 at -8).
static char **mark_list(int arr) { return (char **)(arr_obj(arr) + ARR_MARKERS); }
// M_MARK: arrangement i's markers; M_PMARK: pattern i's
static char **marks_of(int type, int i) { return type == M_MARK ? mark_list(i) : (char **)(pat_rec(i) + PAT_MARKERS); }
static int mark_count(const char *a) {
    int64_t n = a ? *(const int64_t *)(a - 8) : 0;
    return n < 0 || n > MARK_MAX ? 0 : (int)n;
}
static int *ts_num(void) { return *(int **)(g_flBase + (TSNUM_VA - IMG_BASE)); }
static int *ts_den(void) { return *(int **)(g_flBase + (TSDEN_VA - IMG_BASE)); }
static CRITICAL_SECTION *song_lock(void) { return *(CRITICAL_SECTION **)(g_flBase + (SONGLOCK_VA - IMG_BASE)); }

// i = arrangement * 512 + track (1..500)
static char *pl_track(int i) {
    char *a = arr_obj(i / 512);
    return a ? a + (size_t)(i % 512) * PL_TRK_STRIDE : NULL;
}
// A playlist track's link (FL: sub_12F38F0(arrangement, track, mixer track) for an audio track; instrument tracks
// write it in sub_12F1280): +0xd8 type (0 none, 1 audio track: mixer track at +0xdc; 3 instrument track: the
// channel's own id, channel +0x794, at +0xe0; unlinked -1). Sent as [type, mixer track / channel INDEX] (ids are
// each FL's own); hashed by what it points to, so renumbering it isn't an edit.
#define PL_TRK_LTYPE  0xd8
#define PL_TRK_LMIX   0xdc
#define PL_TRK_LCHAN  0xe0
#define CH_ID         0x794
static int chan_index_of_id(int id) {
    void **items; int n = chan_list(&items), k;
    for (k = 0; items && k < n; k++) if (*(int32_t *)((char *)items[k] + CH_ID) == id) return k;
    return -1;
}
static void pl_link_get(const char *r, int *type, int *x) {   // x: mixer track / channel index, -1 if none
    *type = *(int32_t *)(r + PL_TRK_LTYPE);
    *x = *type == 1 ? *(int32_t *)(r + PL_TRK_LMIX) : *type == 3 ? chan_index_of_id(*(int32_t *)(r + PL_TRK_LCHAN)) : -1;
}
// (By number, not by what it points to: the safety net compares these hashes between FLs, where object
// addresses differ. A renumbering then reads as an edit and is sent again, which changes nothing there.)
static uint64_t pl_link_hash(uint64_t h, const char *r) {
    int lt, lx;
    pl_link_get(r, &lt, &lx);
    h = fnv_add(h, &lt, 4);
    return fnv_add(h, &lx, 4);
}
static int meta_valid(int type, int i) {
    switch (type) {
    case M_MIX: case M_ROUTE: return i >= 0 && i < mix_count();
    case M_CHAN: return i >= 0 && i < g_nChKnown;
    case M_PAT: case M_PMARK: return i >= 1 && i < MAX_PAT;
    case M_MARK: return i >= 0 && i < arr_count();
    case M_TSIG: return i == 0;
    default: return i % 512 >= 1 && i % 512 <= PL_TRK_MAX && i / 512 < arr_count();
    }
}

// Hash of one entity's synced fields (0 = absent). Any thread: SEH-guarded unlocked reads
// (a torn read is just a change that settles).
static uint64_t meta_hash(int type, int i) {
    uint64_t h = 1469598103934665603ull;
    if (!meta_valid(type, i)) return 0;
    __try {
        switch (type) {
        case M_MIX: {
            char *r = mix_rec(i);
            h = fnv_add(h, r + MIX_COLOR, 4);
            h = fnv_ustr(h, *(wchar_t **)(r + MIX_NAME));
            break; }
        case M_ROUTE: {
            char *r = mix_rec(i);
            int d, nm = mix_count();
            uint64_t sum = 0;
            for (d = 0; d < nm && d < MIX_ROUTE_MAX; d++) {
                unsigned char on = (unsigned char)r[MIX_ROUTE + d * 8 + 4];
                if (on) {
                    uint64_t e = 1469598103934665603ull;
                    void *tid = mix_tid(d);
                    e = fnv_add(e, &tid, sizeof(tid));
                    e = fnv_add(e, r + MIX_ROUTE + d * 8, 4);
                    sum += e;
                }
            }
            h = fnv_add(h, &sum, sizeof(sum));
            break; }
        case M_CHAN: {
            void **items;
            int n = chan_list(&items);
            if (!items || i >= n) return 0;
            h = fnv_add(h, (char *)items[i] + CH_COLOR, 4);
            h = fnv_ustr(h, *(wchar_t **)((char *)items[i] + CH_NAME));
            break; }
        case M_PAT: {
            char *r = pat_rec(i);
            h = fnv_add(h, r + PAT_COLOR, 4);
            h = fnv_add(h, r + PAT_LENGTH, 4);
            h = fnv_ustr(h, *(wchar_t **)(r + PAT_NAME));
            break; }
        case M_MARK: case M_PMARK: {
            const char *a = *marks_of(type, i);
            int k, n = mark_count(a);
            h = fnv_add(h, &n, 4);
            for (k = 0; k < n; k++) {
                const char *r = a + (size_t)k * MARK_SIZE;
                h = fnv_add(h, r, 8);
                h = fnv_add(h, r + MARK_TAIL, MARK_SIZE - MARK_TAIL);
                h = fnv_ustr(h, *(wchar_t **)(r + 8));
            }
            break; }
        case M_TSIG:
            h = fnv_add(h, ts_num(), 4);
            h = fnv_add(h, ts_den(), 4);
            break;
        default: {
            char *r = pl_track(i);
            if (!r) return 0;
            h = fnv_add(h, r + PL_TRK_COLOR, 4);
            h = fnv_ustr(h, *(wchar_t **)(r + PL_TRK_NAME));
            h = pl_link_hash(h, r);
            break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
    return h > 2 ? h : 3;
}

static void meta_rebase_type(int type) {
    int i;
    for (i = 0; i < meta_cap(type); i++) { g_mSeen[type][i] = g_mHash[type][i] = meta_hash(type, i); g_mAt[type][i] = 0; }
}
static void meta_rebase_patterns(void) { meta_rebase_type(M_PAT); meta_rebase_type(M_PMARK); }
static void meta_rebase_channels(void) { meta_rebase_type(M_CHAN); }
// A remote state loaded into channel ch: the name / color it brought are known, if it brought any (before:
// meta_chan_hash before the load). A rename made here before it, that the load didn't touch, is still an edit.
static uint64_t meta_chan_hash(int ch) { return ch >= 0 && ch < MAX_CH ? meta_hash(M_CHAN, ch) : 0; }
static void meta_take_channel(int ch, uint64_t before) {
    uint64_t h = meta_chan_hash(ch);
    if (!h || h == before) return;
    g_mSeen[M_CHAN][ch] = g_mHash[M_CHAN][ch] = h;
    g_mAt[M_CHAN][ch] = 0;
}

// Channels / tracks renumbered: each entity keeps the hashes (and a pending local edit) it had under its
// old number, so an edit made in the same moment still goes out. A new entity is taken as it is.
static void meta_remap(int type, const int *from, int n) {
    int cap = meta_cap(type), i;
    static uint64_t oldH[1024], oldSeen[1024];
    static DWORD oldAt[1024], oldSent[1024];
    if (!from || cap > 1024) { meta_rebase_type(type); return; }
    for (i = 0; i < cap; i++) {
        oldH[i] = g_mHash[type][i]; oldSeen[i] = g_mSeen[type][i];
        oldAt[i] = g_mAt[type][i]; oldSent[i] = g_mSentAt[type][i];
    }
    for (i = 0; i < cap; i++) {
        int f = i < n ? from[i] : -1;
        if (f >= 0 && f < cap) {
            g_mHash[type][i] = oldH[f]; g_mSeen[type][i] = oldSeen[f];
            g_mAt[type][i] = oldAt[f]; g_mSentAt[type][i] = oldSent[f];
        } else {
            g_mHash[type][i] = g_mSeen[type][i] = meta_hash(type, i);
            g_mAt[type][i] = g_mSentAt[type][i] = 0;
        }
    }
}

static void meta_rebaseline(void) {
    int t;
    for (t = 0; t < M_NTYPES; t++) meta_rebase_type(t);
}

// After applying a remote change: FL's setters touch other entities too (a mixer rename
// renames its linked playlist tracks). Take those as known, except where a local edit is pending.
// Only what changed during the apply (meta_absorb_begin before it): the user's edits run on the same
// main thread, so none lands inside it, while one made since the scanner's last look (META_SCAN_MS)
// was taken as known too and never sent.
static uint64_t g_mPre[M_NTYPES][META_MAX];
static void meta_absorb_begin(void) {
    int t, i;
    for (t = 0; t < M_NTYPES; t++)
        for (i = 0; i < meta_cap(t); i++) g_mPre[t][i] = meta_hash(t, i);
}
static void meta_absorb(void) {
    int t, i;
    uint64_t h;
    for (t = 0; t < M_NTYPES; t++)
        for (i = 0; i < meta_cap(t); i++) {
            if (g_mAt[t][i] || g_mSeen[t][i] != g_mHash[t][i] || (h = meta_hash(t, i)) == g_mPre[t][i]) continue;
            if (h != g_mHash[t][i]) logmsg("META %s %d changed along with the apply: taken as known", M_TAG[t], i);
            g_mSeen[t][i] = g_mHash[t][i] = h;
        }
}

// ---------------- mixer track structure ----------------
// Inserting or deleting mixer tracks renumbers every later track, and everything flim syncs per
// track (params, effect slots, names, routing) is keyed by number. So the change itself travels as
// one op, {"kind":"mix.struct","n0":<count before>,"n":<after>,"del":[old numbers],"ins":[new
// numbers]}, which the others replay with FL's own functions (they also renumber sends, channel
// targets and linked playlist tracks); then both sides re-take their per-track baselines silently.
// Track identity: FL moves whole track records (sub_12A4030 copies all 502 through a permutation),
// so a track's effect slot objects go with it, and they name the track (mix_tid). Master (0) and
// Current (last) never move; FL recycles the Current record as the first inserted track and the
// first deleted track's record as the new Current. Moved tracks travel in the same op as the order
// after the inserts and deletes ("perm"), which the others replay with FL's own "move selected": sent
// track by track instead, each track's sends were applied against a half-moved matrix, FL refused
// the ones that made a loop, and a bus with many senders went over META_BULK and wasn't sent at all.
// The poll thread sends nothing while the order differs from the last known one either (values read
// mid-move would go out by number and be moved a second time by the others' replay).
// Ops someone else sent before this change reached them carry the old numbers: sc_rebase moves them along.
#define MIXINS_VA     FLVA(MIXINS) // InsertMixerTracks(pos, count, flags): 1 undo step, 2 select + scroll, 4 dock
                                   //   side from the neighbour (the menu passes 7); count <= 502 - tracks
#define MIXDEL_VA     FLVA(MIXDEL) // DeleteMixerTracks(int dynarray, last index, flags): 2 delete (sends through a
                                   //   deleted track re-routed past it), 1 trim unused, 0x10 undo step, 4 / 8 unlink /
                                   //   also delete linked playlist instrument tracks + channels (menu: 0x12 | 4 or 8)
#define MIXINS_FLAGS  4            // the menu's resulting state, without the undo step and scrolling
#define MIXDEL_FLAGS  2            // channels the sender's FL deleted along arrive as chan.del
#define MIXMOVE_VA    FLVA(MIXMOVE) // MoveSelectedMixerTracks(dir, undo, scroll): the selected tracks and the focused one,
                                   //   one step (permute + FL's whole refresh)
#define MIXFOCUS_VA   FLVA(MIXFOCUS) // int global: the focused mixer track
#define MIX_SELECTED  0x19         // byte: the track is selected (in the record, so it moves with it)
typedef void (*mixins_t)(unsigned pos, unsigned count, unsigned char flags);
typedef void (*mixdel_t)(const int *idx, int64_t last, unsigned char flags);
typedef void (*mixmove_t)(int dir, unsigned char undo, unsigned char scroll);

static void *g_mixKnown[MAX_MIX];               // track identities the main thread last saw
static volatile LONG g_nMixKnown = -1;

static void mix_known_take(void) {
    int n = mix_count(), t;
    for (t = 0; t < n; t++) g_mixKnown[t] = mix_tid(t);
    InterlockedExchange(&g_nMixKnown, n);
}
// Poll thread: tracks inserted / deleted that the main thread hasn't handled yet.
static int mix_count_changed(void) { return g_nMixKnown >= 0 && mix_count() != g_nMixKnown; }
// ... or moved.
static int mix_layout_changed(void) {
    int n = mix_count(), t;
    if (g_nMixKnown < 0) return 0;
    if (n != g_nMixKnown) return 1;
    for (t = 1; t < n - 1; t++) if (mix_tid(t) != g_mixKnown[t]) return 1;
    return 0;
}
// Order-sensitive hash of the track identities: changes when tracks are inserted, deleted or moved.
static uint64_t mix_ids_hash(void) {
    int n = mix_count(), t;
    uint64_t h = fnv_add(1469598103934665603ull, &n, 4);
    for (t = 0; t < n; t++) { void *id = mix_tid(t); h = fnv_add(h, &id, sizeof(id)); }
    return h;
}

// Track identities old[0..n0) vs FL's tracks now: deleted old numbers, inserted new numbers, and for
// each new number the old one it came from (-1 = new). Returns the new count.
static int mix_diff(void *const *old, int n0, int *del, int *nd, int *ins, int *ni, int *from) {
    int n = mix_count(), i, j;
    *nd = *ni = 0;
    for (j = 0; j < n; j++) from[j] = -1;
    if (n > 0) from[0] = 0;
    if (n > 1 && n0 > 1) from[n - 1] = n0 - 1;
    for (j = 1; j < n - 1; j++) {
        void *id = mix_tid(j);
        for (i = 1; i < n0 - 1 && old[i] != id; i++) {}
        if (i < n0 - 1) from[j] = i; else ins[(*ni)++] = j;
    }
    for (i = 1; i < n0 - 1; i++) {
        for (j = 1; j < n - 1 && from[j] != i; j++) {}
        if (j == n - 1) del[(*nd)++] = i;
    }
    return n;
}

// Tracks renumbered: each effect slot's bookkeeping, each value flim last saw, and each name / color /
// route baseline follow their track (from[]: new number -> old, -1 = new). Re-taking them from FL instead
// swallowed edits made in the same moment (they never went out).
static void mix_rebaseline(const int *from, int n) {
    static Unit old[MAX_MIX][MAX_FX_SLOTS];
    static int oldN[MAX_MIX][MAX_FX_SLOTS];
    int t, s, i;
    memcpy(old, g_fxU, sizeof(old));
    memcpy(oldN, g_fxN, sizeof(oldN));
    for (t = 0; t < MAX_MIX; t++)
        for (s = 0; s < MAX_FX_SLOTS; s++) {
            Unit *u = &g_fxU[t][s];
            int f = t < n ? from[t] : -1;
            if (f >= 0) { *u = old[f][s]; g_fxN[t][s] = oldN[f][s]; }
            else { memset(u, 0, sizeof(*u)); g_fxN[t][s] = 0; }
            // a moved track's pending sends (a plugin swapped or edited just before) go with it
            if (f != t && f >= 0) { u->hash = 0; if (!u->identAt && !u->lastAct) u->baselineAt = GetTickCount() + REBASE_MS; }
            // a moved track keeps the plugins flim knew in it: one swapped meanwhile is still found by do_struct
            if (t < n && f < 0) u->inst = plugin_inst(fx_obj(t, s));
        }
    (void)i;
    plug_agreed_reset(0, 1);
    slots_rebase(NULL, 0, from, n);                          // values, names, colors and sends go with their tracks,
    for (t = 0; t < n; t++) if (from[t] < 0) watch_track(t);
    meta_remap(M_MIX, from, n);                              // so edits made in the same moment still go out
    meta_remap(M_ROUTE, from, n);
    ulock_release_moved("plug:fx:", NULL, from, n);
    mix_known_take();
    InterlockedIncrement(&g_structGen);
    InterlockedExchange(&g_rescanNow, 1);                    // a new track's values watched now: an edit made on it
                                                             // before the next periodic rescan was taken as its start
}

static int int_in(const int *a, int n, int v) { int i; for (i = 0; i < n; i++) if (a[i] == v) return 1; return 0; }

// Main thread (do_struct): tracks inserted, deleted or moved here go out as one mix.struct. "perm": the
// order to put the tracks in after replaying del and ins (which keeps the old tracks in their order and
// puts the new ones at their places): position j gets the track that is then at perm[j - 1].
static void mix_struct(void) {
    static int del[MAX_MIX], ins[MAX_MIX], from[MAX_MIX], after[MAX_MIX], perm[MAX_MIX];
    int n0 = g_nMixKnown, n = mix_count(), nd, ni, i, j, k, moved = 0;
    if (n0 < 0) { mix_known_take(); return; }
    if (n == n0) {
        for (i = 0; i < n && mix_tid(i) == g_mixKnown[i]; i++) {}
        if (i == n) return;
    }
    n = mix_diff(g_mixKnown, n0, del, &nd, ins, &ni, from);
    if (nd >= PROJ_CH_MASS && nd * 2 > n0 - 2) {             // (as do_struct's channels: the project, not deletes)
        logmsg("MIX %d of %d tracks gone at once: FL replaced or emptied the project, not sent as deletes", nd, n0 - 2);
        proj_begin("most mixer tracks gone at once");
        return;
    }
    for (j = 1, i = 1, k = 0; j < n - 1; j++) {             // what del + ins alone gives: old index or -1 (new)
        if (k < ni && ins[k] == j) { after[j] = -1; k++; continue; }
        while (i < n0 - 1 && int_in(del, nd, i)) i++;
        after[j] = i++;
    }
    for (j = 1; j < n - 1; j++) {
        if (from[j] < 0) perm[j] = j;                        // a new track: where the insert puts it
        else for (perm[j] = 1; perm[j] < n - 1 && after[perm[j]] != from[j]; perm[j]++) {}
        moved |= perm[j] != j;
    }
    {
        size_t cap = 128 + (size_t)(nd + ni) * 5 + (moved ? (size_t)n * 5 : 0);
        char *m = (char *)malloc(cap);
        int o = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"mix.struct\",\"n0\":%d,\"n\":%d,\"del\":[", n0, n);
        for (i = 0; i < nd; i++) o += sprintf_s(m + o, cap - o, "%s%d", i ? "," : "", del[i]);
        o += sprintf_s(m + o, cap - o, "],\"ins\":[");
        for (i = 0; i < ni; i++) o += sprintf_s(m + o, cap - o, "%s%d", i ? "," : "", ins[i]);
        o += sprintf_s(m + o, cap - o, "]");
        if (moved) {
            o += sprintf_s(m + o, cap - o, ",\"perm\":[");
            for (j = 1; j < n - 1; j++) o += sprintf_s(m + o, cap - o, "%s%d", j > 1 ? "," : "", perm[j]);
            o += sprintf_s(m + o, cap - o, "]");
        }
        sprintf_s(m + o, cap - o, "}}");
        ws_send(m);
        free(m);
    }
    logmsg("MIX tracks changed locally: %d deleted, %d inserted%s, %d -> %d tracks (sent)", nd, ni, moved ? ", moved" : "", n0, n);
    sc_done(SC_MIX, from, n);
    mix_rebaseline(from, n);
    {   // a track inserted here may have a name already (routing a channel to a new track names it after the channel):
        // taken as known, it never went out and the others kept FL's default ("mixer names differ", reloads). Its name
        // and color go out after the insert (the others' FL made the track with its defaults)
        int t;
        for (t = 0; t < n && t < META_MAX; t++)
            if (from[t] < 0) {
                uint64_t h = meta_hash(M_MIX, t);
                g_mSeen[M_MIX][t] = h; g_mHash[M_MIX][t] = h != 1 ? h ^ 1 : 2; g_mAt[M_MIX][t] = 1;
            }
    }
}

// Remote: the tracks into the sender's order, position j getting the track now at perm[j] (j = 1..n-2),
// with FL's own "move selected", one step left at a time: it renumbers sends, channel targets,
// automation and linked playlist tracks and refreshes all of it, as the sender's move did. Each person's
// selection and focused track stay on their tracks. 0: FL didn't move a track as expected.
static int mix_reorder(const int *perm, int n) {
    static void *want[MAX_MIX], *sel[MAX_MIX];
    int *focus = (int *)(g_flBase + (MIXFOCUS_VA - IMG_BASE));
    void *focusId;
    int j, c, t, ns = 0, steps = 0, ok = 1;
    for (j = 1; j < n - 1; j++) want[j] = mix_tid(perm[j]);
    for (t = 1; t < n - 1; t++) if (mix_rec(t)[MIX_SELECTED]) sel[ns++] = mix_tid(t);
    focusId = *focus >= 0 && *focus < n ? mix_tid(*focus) : NULL;
    for (j = 1; j < n - 1 && ok; j++) {
        for (c = j; c < n - 1 && mix_tid(c) != want[j]; c++) {}
        if (c == n - 1) { ok = 0; break; }
        for (; c > j; c--, steps++) {
            for (t = 0; t < n; t++) mix_rec(t)[MIX_SELECTED] = (char)(t == c);
            *focus = c;
            ((mixmove_t)(g_flBase + (MIXMOVE_VA - IMG_BASE)))(-1, 0, 0);
            if (mix_tid(c - 1) != want[j]) { ok = 0; break; }
        }
    }
    for (t = 0; t < n; t++) {                                // their selection and focus back, wherever the tracks went
        void *id = mix_tid(t);
        int k, on = 0;
        for (k = 0; k < ns && !on; k++) on = sel[k] == id;
        mix_rec(t)[MIX_SELECTED] = (char)on;
        if (id == focusId) *focus = t;
    }
    logmsg("MIX tracks moved (remote): %d step%s%s", steps, steps == 1 ? "" : "s", ok ? "" : ", FL DIDN'T MOVE THEM AS SENT");
    return ok;
}

// "key":[1,2,3] -> out; returns how many.
static int json_ints(const char *m, const char *key, int *out, int cap) {
    const char *p = strstr(m, key);
    int n = 0;
    if (!p) return 0;
    for (p += strlen(key); n < cap && (isdigit((unsigned char)*p) || *p == '-'); ) {
        out[n++] = (int)strtol(p, (char **)&p, 10);
        if (*p == ',') p++;
    }
    return n;
}

// Remote mix.struct: replay it with FL's own insert / delete, then re-take baselines as for a local
// one. A member whose track list no longer matches the sender's re-syncs to the leader. Main thread.
static void do_mixstruct(const QItem *it) {
    static int del[MAX_MIX], ins[MAX_MIX], from[MAX_MIX], d2[MAX_MIX], i2[MAX_MIX], perm[MAX_MIX], seen[MAX_MIX];
    static void *before[MAX_MIX];
    int nd = json_ints(it->path, "\"del\":[", del, MAX_MIX), ni = json_ints(it->path, "\"ins\":[", ins, MAX_MIX);
    int np = json_ints(it->path, "\"perm\":[", perm + 1, MAX_MIX - 2);
    int n0 = -1, want = -1, have = mix_count(), bad = 0, i, k, nd2, ni2;
    int_after(it->path, NULL, "\"n0\":", &n0);
    int_after(it->path, NULL, "\"n\":", &want);
    if (have != n0) {
        logmsg("MIX struct: have %d tracks but the sender had %d; %s", have, n0, g_isLeader ? "skipped" : "re-syncing");
        if (!g_isLeader) ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"state.req\"}}");
        return;
    }
    for (i = 0; i < nd; i++) bad |= del[i] < 1 || del[i] > have - 2 || (i && del[i] <= del[i - 1]);
    for (i = 0; i < ni; i++) bad |= ins[i] < 1 || ins[i] > want - 2 || (i && ins[i] <= ins[i - 1]);
    if (np) {                                                // a permutation of 1..want-2
        memset(seen, 0, sizeof(seen));
        bad |= np != want - 2;
        for (i = 1; i <= np && !bad; i++) { bad |= perm[i] < 1 || perm[i] > want - 2 || seen[perm[i]]; if (!bad) seen[perm[i]] = 1; }
    }
    if (bad || want != have - nd + ni || want > MIX_ROUTE_MAX) { logmsg("MIX struct: malformed op; skipped"); return; }
    for (i = 0; i < have; i++) before[i] = mix_tid(i);
    __try {
        if (nd) {
            // FL takes a Delphi dynamic array: refcount and length in front of the ints (a huge
            // refcount so FL's own code never frees ours)
            int64_t *blk = (int64_t *)calloc(2 + ((size_t)nd + 1) / 2, 8);
            ((int32_t *)blk)[1] = 0x3fffffff;
            blk[1] = nd;
            memcpy(blk + 2, del, sizeof(int) * (size_t)nd);
            ((mixdel_t)(g_flBase + (MIXDEL_VA - IMG_BASE)))((const int *)(blk + 2), nd - 1, MIXDEL_FLAGS);
            free(blk);
        }
        for (i = 0; i < ni; i = k) {                          // runs of new tracks, lowest first
            for (k = i + 1; k < ni && ins[k] == ins[k - 1] + 1; k++) {}
            ((mixins_t)(g_flBase + (MIXINS_VA - IMG_BASE)))((unsigned)ins[i], (unsigned)(k - i), MIXINS_FLAGS);
        }
        if (np && mix_count() == want && !mix_reorder(perm, want) && !g_isLeader)
            ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"state.req\"}}");   // not the sender's order: the leader's project again
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("MIX struct faulted"); }
    mix_diff(before, have, d2, &nd2, i2, &ni2, from);
    logmsg("MIX tracks changed (remote): %d deleted, %d inserted, %d -> %d tracks%s", nd, ni, have, mix_count(),
           mix_count() == want ? "" : " (NOT THE SENDER'S COUNT)");
    mix_rebaseline(from, mix_count());
    if (mix_count() == want) sc_done(SC_MIX, from, want);
}

// {"t":"op","op":{"kind":"meta.set","e":"<tag>","i":N, ...fields}}; name = base64 UTF-16LE.
// Main thread. NULL if the entity is gone.
static char *meta_json(int t, int i) {
    size_t cap = 32768;
    char *m;
    if (t == M_MARK || t == M_PMARK) {
        __try { cap += (size_t)mark_count(*marks_of(t, i)) * (64 + 2 * ((MARK_SIZE - MARK_TAIL + 2) / 3 * 4) + 4 * 256); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    }
    m = (char *)malloc(cap);
    int o = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"meta.set\",\"e\":\"%s\",\"i\":%d", M_TAG[t], i);
    __try {
        const wchar_t *name = NULL;
        uint32_t color = 0;
        char *r = NULL;
        switch (t) {
        case M_MIX: r = mix_rec(i); color = *(uint32_t *)(r + MIX_COLOR); name = *(wchar_t **)(r + MIX_NAME); break;
        case M_CHAN: {
            void **items;
            int n = chan_list(&items);
            if (!items || i >= n) { free(m); return NULL; }
            r = (char *)items[i]; color = *(uint32_t *)(r + CH_COLOR); name = *(wchar_t **)(r + CH_NAME);
            break; }
        case M_PAT:
            r = pat_rec(i); color = *(uint32_t *)(r + PAT_COLOR); name = *(wchar_t **)(r + PAT_NAME);
            o += sprintf_s(m + o, cap - o, ",\"l\":%d", *(int32_t *)(r + PAT_LENGTH));
            break;
        case M_PLT: {
            int lt, lx;
            r = pl_track(i);
            if (!r) { free(m); return NULL; }
            color = *(uint32_t *)(r + PL_TRK_COLOR); name = *(wchar_t **)(r + PL_TRK_NAME);
            pl_link_get(r, &lt, &lx);
            o += sprintf_s(m + o, cap - o, ",\"lk\":[%d,%d]", lt, lx);
            break; }
        case M_ROUTE: {
            int d, k = 0, nm = mix_count();
            r = mix_rec(i);
            o += sprintf_s(m + o, cap - o, ",\"r\":[");
            for (d = 0; d < nm && d < MIX_ROUTE_MAX; d++)
                if (r[MIX_ROUTE + d * 8 + 4])
                    o += sprintf_s(m + o, cap - o, "%s[%d,%d]", k++ ? "," : "", d, *(int32_t *)(r + MIX_ROUTE + d * 8));
            o += sprintf_s(m + o, cap - o, "]");
            break; }
        case M_MARK: case M_PMARK: {
            // "mk":[[pos,action,"<tail b64>","<name b64>"],...]
            const char *a = *marks_of(t, i);
            int k, n = mark_count(a);
            o += sprintf_s(m + o, cap - o, ",\"mk\":[");
            for (k = 0; k < n; k++) {
                const char *r = a + (size_t)k * MARK_SIZE;
                const wchar_t *nm = *(wchar_t **)(r + 8);
                int nl = ustr_len(nm);
                if (nl > 255) nl = 255;
                o += sprintf_s(m + o, cap - o, "%s[%d,%d,\"", k ? "," : "", *(int32_t *)r, *(int32_t *)(r + 4));
                b64enc((const unsigned char *)r + MARK_TAIL, MARK_SIZE - MARK_TAIL, m + o);
                o += (int)strlen(m + o);
                o += sprintf_s(m + o, cap - o, "\",\"");
                b64enc(nl ? (const unsigned char *)nm : (const unsigned char *)"", nl * 2, m + o);
                o += (int)strlen(m + o);
                o += sprintf_s(m + o, cap - o, "\"]");
            }
            o += sprintf_s(m + o, cap - o, "]");
            break; }
        case M_TSIG:
            o += sprintf_s(m + o, cap - o, ",\"nu\":%d,\"de\":%d", *ts_num(), *ts_den());
            break;
        }
        if (t != M_ROUTE && t != M_MARK && t != M_TSIG && t != M_PMARK) {
            int nl = ustr_len(name);
            o += sprintf_s(m + o, cap - o, ",\"c\":%u,\"n\":\"", color);
            b64enc(nl ? (const unsigned char *)name : (const unsigned char *)"", nl * 2, m + o);
            o += (int)strlen(m + o);
            m[o++] = '"'; m[o] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(m); return NULL; }
    sprintf_s(m + o, cap - o, "}}");
    return m;
}

static void do_metasnap(const QItem *it) {
    int t = it->unit, i = it->a, j;
    uint64_t h;
    if (it->gen != g_structGen) {
        // renumbered since it was queued (meta_remap moved the edit's baseline along): the index is stale, so
        // every unsent edit of this kind goes back to scan_meta, which queues it again under its new number
        for (j = 0; j < meta_cap(t); j++) if (g_mSeen[t][j] != g_mHash[t][j] && !g_mAt[t][j]) g_mAt[t][j] = 1;
        return;
    }
    if (chan_list_changed() || mix_layout_changed()) {
        // a channel / track insert or delete not sent yet: the index is already in its new numbering, which
        // the others (and the op's "sc") don't have until it goes out. Back to scan_meta until it has.
        if (!g_mAt[t][i]) g_mAt[t][i] = 1;
        return;
    }
    h = meta_hash(t, i);
    if (!h || h == g_mHash[t][i]) return;
    char *m = meta_json(t, i);
    if (!m) return;
    ws_send(m);
    free(m);
    g_mHash[t][i] = h;
    g_mSentAt[t][i] = GetTickCount();
    logmsg("META sent %s %d", M_TAG[t], i);
}

// Poll thread: find entities that changed locally and settled.
static void scan_meta(DWORD now) {
    static DWORD last = 0;
    static int pend[M_NTYPES * META_MAX];
    int t, i, np = 0;
    if (now - last < META_SCAN_MS || g_patDepth) return;
    last = now;
    if (!g_metaReady) { meta_rebaseline(); g_metaReady = 1; return; }
    for (t = 0; t < M_NTYPES; t++)
        for (i = 0; i < meta_cap(t); i++) {
            uint64_t h = meta_hash(t, i);
            if (h != g_mSeen[t][i]) { g_mSeen[t][i] = h; g_mAt[t][i] = now; }
            if (!g_mAt[t][i] || now - g_mAt[t][i] < META_SETTLE_MS) continue;
            g_mAt[t][i] = 0;
            if (g_mSeen[t][i] == g_mHash[t][i]) continue;
            // gone / just appeared: structural sync handles it. (A remotely loaded channel's name is taken as
            // known when the load ends, meta_take_channel: a time window after it swallowed real renames.)
            if (!g_mSeen[t][i] || !g_mHash[t][i]) { g_mHash[t][i] = g_mSeen[t][i]; continue; }
            pend[np++] = t * META_MAX + i;
        }
    if (np >= META_BULK) {
        for (i = 0; i < np; i++) g_mHash[pend[i] / META_MAX][pend[i] % META_MAX] = g_mSeen[pend[i] / META_MAX][pend[i] % META_MAX];
        logmsg("META bulk change in %d entities: not sent", np);
        return;
    }
    for (i = 0; i < np; i++) enqueue_unit(T_METASNAP, pend[i] / META_MAX, pend[i] % META_MAX, 0, 0, NULL);
}

// Detailed log: what the project looks like here, as hashes of what the scanners last saw (names and
// colors of mixer tracks, routes, channels, patterns, playlist tracks, markers, time signatures, notes,
// the current arrangement's clips), every DIGEST_MS. Two people's lines from the same moment should
// match; the first one that doesn't shows when and in what they went out of step.
#define DIGEST_MS 30000
static void state_digest(DWORD now) {
    static DWORD last = 0;
    char s[600];
    int t, i, o;
    uint64_t h;
    if (!g_logDetailed || !g_metaReady || now - last < DIGEST_MS) return;
    last = now;
    o = sprintf_s(s, sizeof(s), "STATE channels %d, mixer tracks %d |", g_nChKnown, mix_count());
    for (t = 0; t < M_NTYPES; t++) {
        h = 1469598103934665603ull;
        for (i = 0; i < meta_cap(t); i++) h = fnv_add(h, &g_mSeen[t][i], 8);
        o += sprintf_s(s + o, sizeof(s) - o, " %s %08x", M_TAG[t], (unsigned)(h ^ (h >> 32)));
    }
    h = 1469598103934665603ull;
    for (i = 1; i < MAX_PAT; i++) h = fnv_add(h, &g_patSeen[i], 8);
    sprintf_s(s + o, sizeof(s) - o, " notes %08x clips %08x", (unsigned)(h ^ (h >> 32)), (unsigned)(g_clipSeen ^ (g_clipSeen >> 32)));
    logmsg("%s", s);
}

// Inbound meta.set: the whole message goes to the main thread.
static void on_meta_set(const char *m) {
    QItem q; memset(&q, 0, sizeof(q));
    q.type = T_META; q.path = _strdup(m);
    enqueue(&q);
}

static wchar_t *meta_name_arg(const wchar_t *w) { return w[0] ? dstr_keep(w) : NULL; }   // '' = nil string

// Replace a marker list (arrangement or pattern) with the ones in "mk":[...] (main thread, inside
// do_meta's SEH): empty the array the way FL's own delete shrinks it, re-add each with FL's adder,
// then FL's refresh for that kind of list.
static void marks_apply(int t, int i, const char *m) {
    const char *q = strstr(m, "\"mk\":["), *e;
    char **list = marks_of(t, i);
    void *ti = *(void **)(g_flBase + (MARKTYPE_VA - IMG_BASE));
    markadd_t add = (markadd_t)(g_flBase + (MARKADD_VA - IMG_BASE));
    int n = 0;
    if (!q) return;
    q += 6;
    EnterCriticalSection(song_lock());
    __try {
    ((dynsetlen_t)(g_flBase + (DYNSETLEN_VA - IMG_BASE)))(list, ti, 1, 0);
    while (*q == '[' && n < MARK_MAX) {
        char *x;
        int pos = (int)strtol(q + 1, &x, 10), action;
        size_t tn = 0, nn = 0;
        unsigned char *tail = NULL, *nb = NULL;
        wchar_t name[256];
        if (*x != ',') break;
        action = (int)strtol(x + 1, &x, 10);
        if (strncmp(x, ",\"", 2) != 0 || !(e = strchr(x + 2, '"'))) break;
        tail = b64dec(x + 2, (size_t)(e - x - 2), &tn);
        x = (char *)e + 1;
        if (strncmp(x, ",\"", 2) != 0 || !(e = strchr(x + 2, '"'))) { free(tail); break; }
        nb = b64dec(x + 2, (size_t)(e - x - 2), &nn);
        nn = nn / 2 > 255 ? 255 : nn / 2;
        memcpy(name, nb, nn * 2); name[nn] = 0;
        add(list, pos, nn ? dstr_keep(name) : NULL, action,
            tn > 0 ? tail[0] : 4, tn > 1 ? tail[1] : 4);
        if (tn == MARK_SIZE - MARK_TAIL && mark_count(*list) == n + 1)
            memcpy(*list + (size_t)n * MARK_SIZE + MARK_TAIL, tail, MARK_SIZE - MARK_TAIL);
        free(tail); free(nb);
        n++;
        q = e + 1;
        if (*q == ']') q++;
        if (*q == ',') q++;
    }
    } __finally { LeaveCriticalSection(song_lock()); }
    if (t == M_PMARK) {
        // what the piano roll's MarkersChanged does for its pattern, for any pattern
        ((formfn_t)(g_flBase + (MARKSORT_VA - IMG_BASE)))(list);
        ((patlenupd_t)(g_flBase + (PATLENUPD_VA - IMG_BASE)))(i, 0);
        ((formfn_t)(g_flBase + (PATMARKS_VA - IMG_BASE)))(pat_rec(i));
        playlist_refresh();                                          // pattern clips change length
    } else if (i == cur_arr()) {
        char *form = **(char ***)(g_flBase + (PLFORM_VA - IMG_BASE));
        if (form) {
            *(int32_t *)(form + PL_MARKSEL) = -1;
            ((formfn_t)(g_flBase + (MARKREFRESH_VA - IMG_BASE)))(form);
        }
    }
    ((setmod_t)(g_flBase + (MODIFIED_VA - IMG_BASE)))(1);
}

// Project time signature: FL's own setters (the ones general.setNumerator / setDenominator call)
// set the toolbar's time controls and then the globals from them. Same ranges as those.
typedef void (*tsset_t)(void *form, int v);
static void tsig_apply(int nu, int de) {
    char *form = **(char ***)(g_flBase + (MAINFORM_VA - IMG_BASE));
    int *pn = ts_num(), *pd = ts_den();
    if (!form || nu < 1 || nu > 16 || de < 2 || de > 16 || (nu == *pn && de == *pd)) return;
    if (nu != *pn) ((tsset_t)(g_flBase + (TSNUMSET_VA - IMG_BASE)))(form, nu);
    if (de != *pd) ((tsset_t)(g_flBase + (TSDENSET_VA - IMG_BASE)))(form, de);
    if (*pn != nu || *pd != de) logmsg("META time signature %d/%d came out as %d/%d", nu, de, *pn, *pd);
}

static volatile LONG g_mbAuto;           // (the pattern ops section)
static void do_meta(const QItem *it) {
    const char *m = it->path, *e = strstr(m, "\"e\":\""), *q;
    int t, i = -1, len = 0, hasL, hasC = 0;
    uint32_t color = 0;
    wchar_t *name = NULL;
    DWORD now = GetTickCount();
    if (!e) return;
    for (t = 0; t < M_NTYPES && strncmp(e + 5, M_TAG[t], 2) != 0; t++) {}
    if (t == M_NTYPES || !int_after(m, NULL, "\"i\":", &i) || !meta_valid(t, i)) { logmsg("META %.2s %d: no such target here", e + 5, i); return; }
    if (g_isLeader && g_mSentAt[t][i] && now - g_mSentAt[t][i] < META_CONFLICT_MS) {
        // both changed it at once: the leader's value wins; send ours again so the peer converges
        g_mHash[t][i] = 0;
        enqueue_unit(T_METASNAP, t, i, 0, 0, NULL);
        logmsg("META %s %d: changed here too, keeping ours", M_TAG[t], i);
        return;
    }
    if ((q = strstr(m, "\"n\":\"")) != NULL) {
        const char *qe = strchr(q + 5, '"');
        if (qe) {
            size_t bn;
            unsigned char *b = b64dec(q + 5, (size_t)(qe - q - 5), &bn);
            name = (wchar_t *)calloc(bn / 2 + 1, 2);
            memcpy(name, b, bn / 2 * 2);
            free(b);
        }
    }
    if ((q = strstr(m, "\"c\":")) != NULL) { color = (uint32_t)strtoul(q + 4, NULL, 10); hasC = 1; }
    hasL = int_after(m, NULL, "\"l\":", &len);
    meta_absorb_begin();
    __try {
        switch (t) {
        case M_MIX: {
            char *r = mix_rec(i);
            if (name && !ustr_eq(*(wchar_t **)(r + MIX_NAME), name))
                ((mixname_t)(g_flBase + (MIXNAME_VA - IMG_BASE)))(i, meta_name_arg(name), 1);
            if (hasC && *(uint32_t *)(r + MIX_COLOR) != color) {
                ((mixcolor_t)(g_flBase + (MIXCOLOR_VA - IMG_BASE)))(r, color);
                ((uiupdate_t)(g_flBase + (UIUPDATE_VA - IMG_BASE)))(0x18, 0);
            }
            break; }
        case M_ROUTE: {
            static unsigned char on[MIX_ROUTE_MAX];
            static int lvl[MIX_ROUTE_MAX];
            char *r = mix_rec(i), *end;
            void *mixer = **(void ***)(g_flBase + (MIXOBJ_VA - IMG_BASE));
            int d, nm = mix_count(), changed = 0, pass;
            memset(on, 0, sizeof(on));
            q = strstr(m, "\"r\":[");
            if (!q || !mixer) break;
            q += 5;
            while (*q == '[') {
                int l;
                d = (int)strtol(q + 1, &end, 10);
                if (*end != ',') break;
                l = (int)strtol(end + 1, &end, 10);
                if (*end != ']') break;
                if (d >= 0 && d < MIX_ROUTE_MAX) { on[d] = 1; lvl[d] = l; }
                q = end + 1;
                if (*q == ',') q++;
            }
            // FL refuses a route that would close a loop with what is routed here now: the routes this row drops
            // go first, then the new ones, and the ones refused once more after that. No undo step here; FL's
            // "Disable routing?" (a plugin uses the route) is answered yes, as the sender did.
            InterlockedExchange(&g_mbAuto, 1);
            for (pass = 0; pass < 3; pass++)
                for (d = 0; d < nm && d < MIX_ROUTE_MAX; d++) {
                    int cur = r[MIX_ROUTE + d * 8 + 4] != 0;
                    if (cur == on[d] || (pass == 0) != !on[d]) continue;
                    if (((setroute_t)(g_flBase + (SETROUTE_VA - IMG_BASE)))(mixer, i, d, on[d], 0) >= 0) changed = 1;
                    else if (pass == 2) logmsg("META route %d -> %d: FL refused it here (it would make a loop)", i, d);
                }
            InterlockedExchange(&g_mbAuto, 0);
            for (d = 0; d < nm && d < MIX_ROUTE_MAX; d++)
                if (on[d] && r[MIX_ROUTE + d * 8 + 4] && *(int32_t *)(r + MIX_ROUTE + d * 8) != lvl[d]) { *(int32_t *)(r + MIX_ROUTE + d * 8) = lvl[d]; changed = 1; }
            if (changed) {
                ((mixerfn_t)(g_flBase + (ROUTEUPD_VA - IMG_BASE)))(mixer);
                ((uiupdate_t)(g_flBase + (UIUPDATE_VA - IMG_BASE)))(0x18, 0);
            }
            break; }
        case M_CHAN: {
            char *o = (char *)chan_obj(i);
            void **vmt;
            int changed = 0;
            Unit *u = unit_ptr(U_CH, i, 0);
            if (!o) break;
            vmt = *(void ***)o;
            if (name && !ustr_eq(*(wchar_t **)(o + CH_NAME), name)) { ((chsetname_t)vmt[CH_SETNAME_VSLOT])(o, meta_name_arg(name)); changed = 1; }
            if (hasC && *(uint32_t *)(o + CH_COLOR) != color) { ((chsetcolor_t)vmt[CH_SETCOLOR_VSLOT])(o, color); changed = 1; }
            // the channel's saved state holds its name and color: not an edit of ours to send back (it went
            // out under the plugin lock and replaced the state the renaming FL had just loaded)
            if (changed && u) u->baselineAt = now + REBASE_MS;
            break; }
        case M_PAT: {
            char *r = pat_rec(i);
            int changed = 0;
            if (name && !ustr_eq(*(wchar_t **)(r + PAT_NAME), name)) { ((patname_t)(g_flBase + (PATNAME_VA - IMG_BASE)))(i, meta_name_arg(name)); changed = 1; }
            if (hasC && *(uint32_t *)(r + PAT_COLOR) != color) { ((patcolor_t)(g_flBase + (PATCOLOR_VA - IMG_BASE)))(i, color); changed = 1; }
            if (hasL && *(int32_t *)(r + PAT_LENGTH) != len) {
                *(int32_t *)(r + PAT_LENGTH) = len;
                ((patlenupd_t)(g_flBase + (PATLENSET_VA - IMG_BASE)))(i, 0);   // (pattern): flag unused
                changed = 1;
            }
            if (changed) {
                char *form = **(char ***)(g_flBase + (PLFORM_VA - IMG_BASE));
                void *pats = form ? *(void **)(form + PLFORM_PATS) : NULL;
                if (pats) ((patrefresh_t)(g_flBase + (PATREFRESH_VA - IMG_BASE)))(pats, 1, 0);
                playlist_refresh();
            }
            break; }
        case M_PLT: {
            char *r = pl_track(i);
            const char *lk = strstr(m, "\"lk\":[");
            if (!r) break;
            // FL's setter also recolors the track's linked channel. Not from here: the link may be one this op
            // replaces (tracks moved), and a channel recolored at the sender comes as its own edit anyway. So
            // the track is unlinked around it, and its link written after.
            if ((name && !ustr_eq(*(wchar_t **)(r + PL_TRK_NAME), name)) || (hasC && *(uint32_t *)(r + PL_TRK_COLOR) != color)) {
                int32_t lt0 = *(int32_t *)(r + PL_TRK_LTYPE);
                *(int32_t *)(r + PL_TRK_LTYPE) = 0;
                ((pltrack_t)(g_flBase + (PLTRACK_VA - IMG_BASE)))(arr_obj(i / 512), i % 512,
                    name ? meta_name_arg(name) : *(wchar_t **)(r + PL_TRK_NAME),
                    hasC ? color : *(uint32_t *)(r + PL_TRK_COLOR));
                *(int32_t *)(r + PL_TRK_LTYPE) = lt0;
            }
            if (lk) {                                   // its link: written as FL's own setters leave it (they also
                int lt = 0, lx = -1, cur, cx;           // rename the track; the name comes with this op anyway)
                char *e;
                lt = (int)strtol(lk + 6, &e, 10);
                if (*e == ',') lx = (int)strtol(e + 1, NULL, 10);
                pl_link_get(r, &cur, &cx);
                if (lt != cur || lx != cx) {
                    void **items; int n = chan_list(&items);
                    if (lt == 1 && lx >= 0 && lx < mix_count()) {
                        *(int32_t *)(r + PL_TRK_LTYPE) = 1; *(int32_t *)(r + PL_TRK_LMIX) = lx; *(int32_t *)(r + PL_TRK_LCHAN) = -1;
                    } else if (lt == 3 && items && lx >= 0 && lx < n) {
                        *(int32_t *)(r + PL_TRK_LTYPE) = 3; *(int32_t *)(r + PL_TRK_LMIX) = -1;
                        *(int32_t *)(r + PL_TRK_LCHAN) = *(int32_t *)((char *)items[lx] + CH_ID);
                    } else {
                        *(int32_t *)(r + PL_TRK_LTYPE) = 0; *(int32_t *)(r + PL_TRK_LMIX) = -1; *(int32_t *)(r + PL_TRK_LCHAN) = -1;
                    }
                    playlist_refresh();
                }
            }
            break; }
        case M_MARK: case M_PMARK:
            marks_apply(t, i, m);
            break;
        case M_TSIG: {
            int nu = 0, de = 0;
            if (int_after(m, NULL, "\"nu\":", &nu) && int_after(m, NULL, "\"de\":", &de)) tsig_apply(nu, de);
            break; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("META apply faulted %s %d", M_TAG[t], i); }
    free(name);
    g_mSeen[t][i] = g_mHash[t][i] = meta_hash(t, i);
    g_mAt[t][i] = 0;
    meta_absorb();
    logmsg("META applied %s %d", M_TAG[t], i);
}

// ---------------- window guard ----------------
// Window state is client-only: what is open, where, and in which order is never
// synced. FL's preset loader opens/raises the plugin editor, so around every
// remote-originated heavy op we snapshot FL's floating forms and put things back
// afterwards (and keep doing so for a moment, as FL shows some windows late).
// Local input during that time ends the guard: the user's own actions win.
#define WG_MAX       128
#define WG_GUARD_MS  2000
typedef struct { HWND h; RECT r; int plugin; } WinRec;
typedef struct { int n; WinRec w[WG_MAX]; HWND fg, focus; DWORD at; } WinSnap;
static WinSnap g_wsnap;
static DWORD   g_wguardUntil = 0, g_wguardLast = 0;
static HWND    g_mainForm = NULL;

static int wnd_class_is(HWND h, const char *cls) {
    char c[64]; GetClassNameA(h, c, sizeof(c)); return strcmp(c, cls) == 0;
}
// FL's floating forms (plugin wrappers, channel rack, mixer, playlist, browser...)
static int is_fl_form(HWND h) {
    char c[64]; size_t n;
    GetClassNameA(h, c, sizeof(c)); n = strlen(c);
    return n > 5 && c[0] == 'T' && strcmp(c + n - 4, "Form") == 0 && strcmp(c, "TFruityLoopsMainForm") != 0;
}
static void rect_in_parent(HWND h, RECT *r) {
    GetWindowRect(h, r);
    MapWindowPoints(NULL, GetParent(h), (POINT *)r, 2);
}

typedef struct { HWND list[WG_MAX]; int n; } HwndList;
static BOOL CALLBACK collect_forms(HWND h, LPARAM lp) {
    HwndList *L = (HwndList *)lp;
    if (L->n < WG_MAX && IsWindowVisible(h) && is_fl_form(h)) L->list[L->n++] = h;
    return TRUE;
}
// visible FL forms, topmost first (EnumChildWindows walks siblings in z-order)
static void visible_forms(HwndList *L) {
    L->n = 0;
    if (g_mainForm) EnumChildWindows(g_mainForm, collect_forms, (LPARAM)L);
}

static BOOL CALLBACK redraw_cb(HWND h, LPARAM lp) {
    (void)lp;
    if (wnd_class_is(h, "TEventEditForm")) RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return TRUE;
}
static void redraw_editors(void) {
    if (g_mainForm) EnumChildWindows(g_mainForm, redraw_cb, 0);
}

static void wguard_begin(void) {
    DWORD now = GetTickCount();
    if (now < g_wguardUntil) { g_wguardUntil = now + WG_GUARD_MS; return; }   // extend, keep first snapshot
    HwndList L; int i;
    visible_forms(&L);
    g_wsnap.n = L.n;
    for (i = 0; i < L.n; i++) {
        g_wsnap.w[i].h = L.list[i];
        g_wsnap.w[i].plugin = wnd_class_is(L.list[i], "TPluginForm");
        rect_in_parent(L.list[i], &g_wsnap.w[i].r);
    }
    g_wsnap.fg = GetForegroundWindow();
    g_wsnap.focus = GetFocus();
    g_wsnap.at = now;
    g_wguardUntil = now + WG_GUARD_MS;
}

static int snap_index(HWND h) {
    int i;
    for (i = 0; i < g_wsnap.n; i++) if (g_wsnap.w[i].h == h) return i;
    return -1;
}

static void wguard_pass(void) {
    DWORD now = GetTickCount();
    HwndList L; int i, j, changedOrder = 0;
    if (!g_wguardUntil || now >= g_wguardUntil) { g_wguardUntil = 0; return; }
    if (g_lastInput && (LONG)(g_lastInput - g_wsnap.at) > 0) { g_wguardUntil = 0; return; }  // user took over
    visible_forms(&L);
    // windows that newly appeared: a replaced plugin's editor takes over its old
    // window's place; anything else the remote op opened gets closed again
    for (i = 0; i < L.n; i++) {
        HWND h = L.list[i];
        if (snap_index(h) >= 0 || !wnd_class_is(h, "TPluginForm")) continue;
        int heir = -1;
        for (j = 0; j < g_wsnap.n; j++)
            if (g_wsnap.w[j].plugin && (!IsWindow(g_wsnap.w[j].h) || !IsWindowVisible(g_wsnap.w[j].h))) { heir = j; break; }
        if (heir >= 0) {
            RECT r = g_wsnap.w[heir].r;
            SetWindowPos(h, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            g_wsnap.w[heir].h = h;            // it now stands in for the old one
            logmsg("WINDOW kept replaced plugin editor in place");
        } else {
            SendMessageW(h, WM_CLOSE, 0, 0);  // same as the user closing it
            logmsg("WINDOW closed editor opened by a remote op");
        }
    }
    // positions
    for (i = 0; i < g_wsnap.n; i++) {
        HWND h = g_wsnap.w[i].h; RECT r;
        if (!IsWindow(h) || !IsWindowVisible(h)) continue;
        rect_in_parent(h, &r);
        if (r.left != g_wsnap.w[i].r.left || r.top != g_wsnap.w[i].r.top)
            SetWindowPos(h, NULL, g_wsnap.w[i].r.left, g_wsnap.w[i].r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // stacking order: compare current order of the snapshot windows with the snapshot
    visible_forms(&L);
    {
        int k = 0;
        for (i = 0; i < L.n; i++) {
            int si = snap_index(L.list[i]);
            if (si < 0) continue;
            while (k < g_wsnap.n && !IsWindowVisible(g_wsnap.w[k].h)) k++;
            if (k >= g_wsnap.n || g_wsnap.w[k].h != L.list[i]) { changedOrder = 1; break; }
            k++;
        }
    }
    if (changedOrder) {
        for (i = g_wsnap.n - 1; i >= 0; i--)          // bottom-most first, so the old top ends on top
            if (IsWindow(g_wsnap.w[i].h) && IsWindowVisible(g_wsnap.w[i].h))
                SetWindowPos(g_wsnap.w[i].h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        logmsg("WINDOW restored stacking order");
    }
    if (g_wsnap.fg && GetForegroundWindow() != g_wsnap.fg && IsWindow(g_wsnap.fg)) SetForegroundWindow(g_wsnap.fg);
    if (g_wsnap.focus && GetFocus() != g_wsnap.focus && IsWindow(g_wsnap.focus)) SetFocus(g_wsnap.focus);
    g_wguardLast = now;
}

// ---------------- flim window link: status, log, Connect / Disconnect ----------------
// flim draws nothing into FL's own UI: the connection status, who is where, sample transfers and a
// log of what happened live in flim.exe's window, which also connects and disconnects. flim.exe
// hands its window over in FLIM_GUI when it starts FL, or introduces itself later (GUI_HELLO).
// WM_COPYDATA, UTF-8 text:
//   engine -> window (wParam = our task window): GUI_STATUS "<state>\t<text>", GUI_LOG "<line>",
//                                                 GUI_ALERT "<text>" (a popup: the session leader left...),
//                                                 GUI_PEOPLE "<leaderControls 0/1>\t<my id>\n" then a line per person,
//                                                   "P\t<id>\t<name>\t<leader 0/1>\t<sync-only 0/1>\n" or, removed
//                                                   from the session, "B\t<id>\t<name>\t<IP banned 0/1>\n" (on_peers)
//   window -> engine (to our task window):       GUI_CONNECT "<url>\n<password>\n<name>",
//                                                 GUI_DISCONNECT, GUI_HELLO,
//                                                 GUI_SETTINGS (flim.ini changed: plugin_lock_s),
//                                                 GUI_RESYNC ("Resync from host": the leader's project now),
//                                                 GUI_ADMIN "<action>\t<id>\t<on 0/1>" (host controls, as the leader)
// Ours go out from a thread of our own with a timeout, so a busy or closed window never holds FL up.
// (GUI_* kinds and NET_* states: common/flim_config.h)
static volatile LONG g_netState = NET_OFF;
static volatile LONG g_uiWanted = 0;      // FL has settled: the main thread runs ui_start
static CRITICAL_SECTION g_netTextLock;    // who is where (focus) and the arrows (cursors), below
static char     g_myName[96];             // UTF-8, sanitized
static HWND     g_gui = NULL;             // flim.exe's window
static volatile LONG g_plugLockMs = FLIM_LOCK_S_MAX * 1000;   // a plugin lock is held this long after our last change
typedef struct GuiMsg { struct GuiMsg *next; int kind; char text[1]; } GuiMsg;
static CRITICAL_SECTION g_guiLock;        // the queue and g_netLine
static GuiMsg  *g_guiHead, *g_guiTail;
static HANDLE   g_guiEvt;
static char     g_netLine[320] = "0\tNot connected";   // the last status: for a window that says hello

static void net_connect(const char *url, const char *pw);
static void net_disconnect(void);
static void on_peers(const char *m);
static char g_peersJson[4096];
static void focus_summary(char *out, int cap, const char *sep);
static void json_esc(const char *in, char *out, int cap);
static void fl_hint(const char *msg);
static void cursor_update(void);
#define CUR_TIMER     2            // task window timer: other people's mouse arrows
#define CUR_TIMER_MS  40
#define BACKUP_TIMER  4            // task window timer: is a project backup due? (project backups)
// Is the leader's FL still alive? Its main thread says so every ALIVE_MS (a frozen FL stops saying
// it, while its connection, kept by another thread, stays up). Members that hear nothing for
// LEADER_STUCK_MS (generous: some plugins take long to load) are told to save their own copy.
#define ALIVE_TIMER   5
#define ALIVE_MS      3000
#define LEADER_STUCK_MS 30000
#define LEADER_LOAD_MS  300000      // ...unless the leader's FL said it is opening a project (a big one takes minutes)
static volatile DWORD g_leaderAliveAt = 0;  // member: the leader's last "alive" (0 = its flim doesn't send them)
static volatile LONG  g_leaderStuck = 0;    // member: warned about it already

static void gui_push(int kind, const char *text) {
    size_t n = strlen(text);
    GuiMsg *g;
    if (!g_guiEvt) return;
    g = (GuiMsg *)malloc(sizeof(GuiMsg) + n);
    g->next = NULL; g->kind = kind; memcpy(g->text, text, n + 1);
    EnterCriticalSection(&g_guiLock);
    if (g_guiTail) g_guiTail->next = g; else g_guiHead = g;
    g_guiTail = g;
    LeaveCriticalSection(&g_guiLock);
    SetEvent(g_guiEvt);
}
static DWORD WINAPI guiThread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_guiEvt, INFINITE);
        for (;;) {
            GuiMsg *g;
            HWND gui = g_gui;
            EnterCriticalSection(&g_guiLock);
            g = g_guiHead;
            if (g && !(g_guiHead = g->next)) g_guiTail = NULL;
            LeaveCriticalSection(&g_guiLock);
            if (!g) break;
            if (gui && IsWindow(gui)) {
                COPYDATASTRUCT cd;
                DWORD_PTR r;
                cd.dwData = (ULONG_PTR)g->kind; cd.cbData = (DWORD)strlen(g->text) + 1; cd.lpData = g->text;
                SendMessageTimeoutW(gui, WM_COPYDATA, (WPARAM)g_taskWnd, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &r);
            }
            free(g);
        }
    }
}
static void gui_status_again(void) {
    char line[320];
    EnterCriticalSection(&g_guiLock);
    strcpy_s(line, sizeof(line), g_netLine);
    LeaveCriticalSection(&g_guiLock);
    gui_push(GUI_STATUS, line);
}

// The window's log: our own log lines, for the events people care about (logmsg calls this). With the
// detailed log, nearly all of them: the per-knob, per-op and mouse lines stay in the file (and its export).
static void gui_log(const char *line) {
    static const char *const KEEP[] = { "PROJECT ", "SAMPLES uploaded", "SAMPLES downloaded", "SAMPLES gave up",
                                        "SAMPLES could not", "SAMPLES upload", "SAMPLES the server", "MIX ", "LOCK ",
                                        "PLUGIN ", "SYNC ", "SHARED " };
    static const char *const NOISE[] = { "DETECT ", "APPLY ", "OP ", "CURSOR ", "PLUGFOCUS", "BEAT " };
    int i;
    if (g_logDetailed) {
        for (i = 0; i < (int)(sizeof(NOISE) / sizeof(NOISE[0])); i++) if (strncmp(line, NOISE[i], strlen(NOISE[i])) == 0) return;
        gui_push(GUI_LOG, line);
        return;
    }
    for (i = 0; i < (int)(sizeof(KEEP) / sizeof(KEEP[0])); i++)
        if (strncmp(line, KEEP[i], strlen(KEEP[i])) == 0) { gui_push(GUI_LOG, line); return; }
}

// Any thread: the status the flim window shows (UTF-8); a change of state goes into its log too.
static void net_status(int state, const char *fmt, ...) {
    char a[256], line[320];
    LONG was;
    va_list ap; va_start(ap, fmt); vsprintf_s(a, sizeof(a), fmt, ap); va_end(ap);
    was = InterlockedExchange(&g_netState, state);
    sprintf_s(line, sizeof(line), "%d\t%s", state, a);
    EnterCriticalSection(&g_guiLock);
    strcpy_s(g_netLine, sizeof(g_netLine), line);
    LeaveCriticalSection(&g_guiLock);
    logmsg("NET %s", a);
    gui_push(GUI_STATUS, line);
    if (state != was || state == NET_ERR) gui_push(GUI_LOG, a);
}

static char g_netShown[300];       // the server as shown in the status ("host[:port]")
static char g_netErr[160];         // why the server ended the current connection, if it said so
static volatile LONG g_kicked;     // ...because the host removed us (host controls)

// welcome / presence: "Connected to <server> · leader · 3 people"
static void net_show_connected(const char *m) {
    const char *p = strstr(m, "\"peers\":[");
    int n = 0;
    while (p && (p = strstr(p + 1, "\"role\":\"")) != NULL) n++;
    char where[600];
    focus_summary(where, sizeof(where), " \xC2\xB7 ");
    net_status(NET_ON, "Connected to %s \xC2\xB7 %s \xC2\xB7 %d %s%s%s", g_netShown, g_isLeader ? "leader" : g_syncOnly ? "sync-only (your edits stay here)" : "member",
               n, n == 1 ? "person" : "people", where[0] ? " \xC2\xB7 " : "", where);
}

// Main thread, once FL has settled: the arrows' timer, and our status for the flim window.
static void ui_start(void) {
    if (g_taskWnd) SetTimer(g_taskWnd, CUR_TIMER, CUR_TIMER_MS, NULL);
    if (g_taskWnd) SetTimer(g_taskWnd, BACKUP_TIMER, 60000, NULL);
    if (g_taskWnd) SetTimer(g_taskWnd, ALIVE_TIMER, ALIVE_MS, NULL);
    gui_status_again();
}

static volatile LONG g_backupMin;
static void leader_name_project(void);
static void backup_setting(void);
static void backup_tick(void);
// Main thread (task window): a command from the flim window, which from now on gets our news.
static void gui_command(HWND from, const COPYDATASTRUCT *cd) {
    char t[1400], *pw, *name;
    size_t n = cd->lpData ? cd->cbData : 0;
    if (n >= sizeof(t)) n = sizeof(t) - 1;
    memcpy(t, cd->lpData, n);
    t[n] = 0;
    if (from && IsWindow(from)) g_gui = from;
    switch ((int)cd->dwData) {
    case GUI_HELLO: gui_status_again(); if (g_peersJson[0]) on_peers(g_peersJson); break;
    case GUI_CONNECT:
        pw = strchr(t, '\n');
        if (pw) *pw++ = 0;
        name = pw ? strchr(pw, '\n') : NULL;
        if (name) *name++ = 0;
        if (name && name[0]) {
            wchar_t w[128], clean[FLIM_NAME_MAX + 1];
            MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 128);
            if (flim_sanitize_name(w, clean, FLIM_NAME_MAX + 1))
                WideCharToMultiByte(CP_UTF8, 0, clean, -1, g_myName, sizeof(g_myName), NULL, NULL);
        }
        if (g_netState == NET_ON || g_netState == NET_CONNECTING) net_disconnect();
        net_connect(t, pw ? pw : "");
        break;
    case GUI_DISCONNECT: net_disconnect(); break;
    case GUI_SETTINGS:
        g_plugLockMs = flimcfg_lock_s() * 1000;
        backup_setting();
        g_logDetailed = flimcfg_log_detailed();
        InterlockedExchange(&g_showCursors, flimcfg_show_cursors());
        logmsg("settings: others' cursors %s; plugin lock %ld s, project backup every %ld min (0 = off), detailed log %s", g_showCursors ? "shown" : "hidden", g_plugLockMs / 1000, g_backupMin,
               g_logDetailed ? "on" : "off");
        if (g_isLeader && g_netState == NET_ON) leader_name_project();   // a song name typed in mid-session
        break;
    case GUI_RESYNC: resync_asked(); break;
    case GUI_ADMIN: {                  // a host control: "<action>\t<id>\t<on 0/1>" (the server decides if we may)
        char *id = strchr(t, '\t'), *on = id ? strchr(id + 1, '\t') : NULL, m[400], ea[40], ei[160];
        if (!on) break;
        *id++ = 0; *on++ = 0;
        json_esc(t, ea, sizeof(ea)); json_esc(id, ei, sizeof(ei));
        sprintf_s(m, sizeof(m), "{\"t\":\"admin\",\"action\":\"%s\",\"id\":\"%s\",\"on\":%s}", ea, ei, *on == '1' ? "true" : "false");
        logmsg("NET host control: %s %s%s", t, id, strcmp(t, "sync_only") ? "" : *on == '1' ? " on" : " off");
        if (g_netState == NET_ON) ws_send(m);
        break; }
    }
    SecureZeroMemory(t, sizeof(t));
}

// ---------------- project sync ----------------
// A project load/new/close is never replayed piece by piece. The engine enters
// "project mode": nothing is sent, every param change is absorbed, and only project
// tasks run on FL's thread. When FL's load is over and things are quiet, all baselines
// are re-taken. The leader then sends the project FILE (members open the same .flp);
// a member that loaded something on its own asks the leader for its project instead.
// Both sides work on "<name>_flim_MMDD_hhmm.flp" so Ctrl+S never touches the original file
// and reopening the original never overwrites an earlier session's copy.
static volatile LONG  g_inHook = 0;
static volatile LONG  g_projBusy = 0;          // project mode
static volatile LONG  g_projRemote = 0;        // member: loading the leader's project
static volatile LONG  g_projExpectCh = 0;      //   how many channels its file has (do_projload; 0 = not known)
static int            g_projShortLoads = 0;    //   loads of it in a row that FL ended with most channels missing
static volatile DWORD g_resyncAt;              // (resync)
static volatile DWORD g_projAskedAt = 0;       // member: asked for it (state.req: join, re-sync), until it comes
#define PROJ_ASK_MAX_MS 15000                  // (no answer by then: let the user work again)
// Member: the leader's project is on its way or loading. What the user does meanwhile would be lost to the
// load (it replaces the song), so input is held off then, as while the host saves (input_held).
// It lasts until the ops held during the load have run too (g_replaying): replayed, they only wait in the queue,
// and a change made here before they ran went out ahead of ops the leader had made earlier (they all crossed).
static volatile LONG g_replaying = 0;
static volatile DWORD g_replayAt = 0;
#define REPLAY_MAX_MS 5000                     // (a busy session keeps the queue from ever being empty: not longer)
static int member_reloading(void) {
    DWORD a = g_projAskedAt;
    if (g_isLeader) return 0;
    if (g_replaying && !g_projBusy && (g_peerQueued <= 0 || GetTickCount() - g_replayAt > REPLAY_MAX_MS) &&
        InterlockedExchange(&g_replaying, 0))
        logmsg("PROJECT caught up with the changes made during the load");
    return (g_projBusy && g_projRemote) || g_replaying || (a && (LONG)(GetTickCount() - a) < PROJ_ASK_MAX_MS);   // (signed: a is now | 1)
}
static volatile LONG  g_projDoneQueued = 0;
static volatile DWORD g_projSince = 0;
static volatile DWORD g_closingUntil = 0;      // FL is closing: never broadcast what teardown does
static volatile DWORD g_lastHookTick = 0;
static CRITICAL_SECTION g_heldLock;            // member: ops that arrived during a project load
static char **g_held = NULL;
static int    g_nheld = 0, g_capheld = 0;

#define PROJ_SETTLE_MS  1500    // quiet time after FL's load before baselines are re-taken
#define PROJ_MAX_MS     180000  // give up waiting for quiet after this long

static void handle_msg(const char *m);

static int closing_now(void) { return g_closingUntil && (LONG)(GetTickCount() - g_closingUntil) < 0; }

static void fl_ustr(uint32_t va, wchar_t *out, int cap);
static wchar_t g_projNameAtBegin[MAX_PATH];     // FL's project file when the project change began

static void proj_begin(const char *why) {
    if (InterlockedExchange(&g_projBusy, 1) == 0) {
        fl_ustr(PROJNAME_VA, g_projNameAtBegin, MAX_PATH);
        g_projSince = GetTickCount();
        InterlockedExchange(&g_projRemote, 0);
        InterlockedExchange(&g_projDoneQueued, 0);
        logmsg("PROJECT change (%s): sync paused until FL settles", why);
    }
}

static void *fl_form(void) {
    __try { return **(void ***)(g_flBase + (MAINOBJ_VA - IMG_BASE)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
static void *fl_project(void) {
    __try { char *h = **(char ***)(g_flBase + (FLPROJ_VA - IMG_BASE)); return *(void **)(h + 0x10); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
}
// FL is inside an .flp load (its open handler parks a busy object on the form meanwhile).
static int fl_loading(void) {
    char *f = (char *)fl_form();
    if (!f) return 0;
    __try { return *(void **)(f + FORM_BUSY) != NULL; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// Copy of one of FL's UnicodeString globals ('' if empty).
static void fl_ustr(uint32_t va, wchar_t *out, int cap) {
    out[0] = 0;
    __try {
        const wchar_t *s = *(const wchar_t **)(g_flBase + (va - IMG_BASE));
        if (s) wcsncpy_s(out, cap, s, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
}
// A Delphi literal string FL may keep a pointer to: never freed (a few hundred bytes per load).
static wchar_t *dstr_keep(const wchar_t *s) {
    int n = (int)wcslen(s);
    unsigned char *m = (unsigned char *)calloc(1, 16 + (n + 1) * 2);
    *(uint16_t *)(m + 4) = 1200; *(uint16_t *)(m + 6) = 2;
    *(int32_t *)(m + 8) = -1;    *(int32_t *)(m + 12) = n;
    memcpy(m + 16, s, (size_t)n * 2);
    return (wchar_t *)(m + 16);
}

static unsigned char *read_file_w(const wchar_t *path, size_t *n) {
    FILE *f = NULL;
    *n = 0;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(sz > 0 ? sz : 1);
    *n = fread(buf, 1, sz > 0 ? sz : 0, f);
    fclose(f);
    return buf;
}
static int write_file_w(const wchar_t *path, const unsigned char *d, size_t n) {
    FILE *f = NULL;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return 0;
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    return w == n;
}
static int file_exists_w(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int ends_with_ci(const wchar_t *s, const wchar_t *suf) {
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && _wcsicmp(s + a - b, suf) == 0;
}
static const wchar_t *base_name(const wchar_t *p) {
    const wchar_t *b = wcsrchr(p, L'\\'), *c = wcsrchr(p, L'/');
    if (c && (!b || c > b)) b = c;
    return b ? b + 1 : p;
}
// "<stem>_flim.flp" or "<stem>_flim_MMDD_hhmm.flp": already a session copy, work on it directly.
static int is_flim_name(const wchar_t *p) {
    size_t n = wcslen(p);
    int i;
    if (ends_with_ci(p, L"_flim.flp")) return 1;
    if (n < 19 || _wcsicmp(p + n - 4, L".flp") != 0) return 0;
    const wchar_t *t = p + n - 19;                     // "_flim_MMDD_hhmm.flp"
    if (_wcsnicmp(t, L"_flim_", 6) != 0 || t[10] != L'_') return 0;
    for (i = 6; i < 15; i++) if (i != 10 && !iswdigit(t[i])) return 0;
    return 1;
}
// path with its extension replaced by "_flim_MMDD_hhmm.flp" (local time).
static void flim_name(const wchar_t *path, wchar_t *out, int cap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wcscpy_s(out, cap, path);
    wchar_t *dot = wcsrchr(out, L'.'), *sl = wcsrchr(out, L'\\');
    if (dot && (!sl || dot > sl)) *dot = 0;
    size_t n = wcslen(out);
    swprintf_s(out + n, cap - n, L"_flim_%02d%02d_%02d%02d.flp", st.wMonth, st.wDay, st.wHour, st.wMinute);
}

// An existing file at p is kept as p.bak (replacing an older .bak) before p is rewritten.
static void backup_existing(const wchar_t *p) {
    wchar_t bak[MAX_PATH + 8];
    if (!file_exists_w(p)) return;
    swprintf_s(bak, MAX_PATH + 8, L"%s.bak", p);
    if (MoveFileExW(p, bak, MOVEFILE_REPLACE_EXISTING)) logmsg("PROJECT kept the previous copy as %S", bak);
}

// Make path FL's current project (title, Ctrl+S target, recent files). Main thread.
static void set_project_file(const wchar_t *path) {
    void *form = fl_form();
    wchar_t *w = dstr_keep(path);
    if (!form) return;
    __try { ((setproj_t)(g_flBase + (SETPROJ_VA - IMG_BASE)))(form, w, w, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("PROJECT set name faulted"); }
}

// Re-take every baseline after a project change (main thread). Nothing is sent.
static void proj_rebaseline(void) {
    void **items; int n = chan_list(&items), i, t, s;
    arr_known_take();
    known_from_list();
    mix_known_take();
    memset(g_chU, 0, sizeof(g_chU));
    memset(g_fxU, 0, sizeof(g_fxU));
    plug_agreed_reset(1, 1);
    memset(g_chanPlugN, 0, sizeof(g_chanPlugN));
    memset(g_fxN, 0, sizeof(g_fxN));
    for (i = 0; i < n; i++) g_chU[i].inst = plugin_inst(items[i]);
    for (t = 0; t < mix_count() && t < MAX_MIX; t++)
        for (s = 0; s < MAX_FX_SLOTS; s++) g_fxU[t][s].inst = plugin_inst(fx_obj(t, s));
    notes_rebaseline();
    meta_rebaseline();
    rescan();
    slots_reread();
    InterlockedIncrement(&g_structGen);
}

// {"t":"op","op":{"kind":"project.load","name":"<utf8>","data":"<b64>"}}
static void send_project(const unsigned char *d, size_t n, const wchar_t *name) {
    char nm[MAX_PATH * 3], esc[MAX_PATH * 6];
    int i, o = 0;
    nm[0] = 0;
    if (name && *name) WideCharToMultiByte(CP_UTF8, 0, name, -1, nm, sizeof(nm), NULL, NULL);
    for (i = 0; nm[i] && o < (int)sizeof(esc) - 7; i++) {
        unsigned char c = (unsigned char)nm[i];
        if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = (char)c; }
        else if (c < 0x20) o += sprintf_s(esc + o, sizeof(esc) - o, "\\u%04x", c);
        else esc[o++] = (char)c;
    }
    esc[o] = 0;
    size_t cap = strlen(esc) + (n + 2) / 3 * 4 + 128;
    char *m = (char *)malloc(cap);
    int len = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"project.load\",\"name\":\"%s\",\"data\":\"", esc);
    b64enc(d, (int)n, m + len);
    strcat_s(m, cap, "\"}}");
    InterlockedIncrement(&g_projEpoch);                       // (stamped on it: the members take it)
    ws_send_samples(m, d, n);
    logmsg("PROJECT sent %S (%zu bytes, project %ld)", (name && *name) ? name : L"(untitled)", n, g_projEpoch);
}

// A snapshot carries no solo of ours: each track gets its own mute state back and loses the solo.
// Nor does a song saved while soloed: its file has the solo and the solo's mutes, and FL opens it with
// every track's +0x1c set (what the tracks get back on unsolo), so the others would open it soloed.
static int solostrip_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    int *t = (int *)ctx;
    (void)at; (void)end;
    if (id != EV_INSFLAGS || n < 8) return 1;
    if (*t < mix_count()) {
        uint32_t *f = (uint32_t *)(p + 4);
        *f = (*f & ~(uint32_t)(INS_SOLO | INS_ENABLED)) | (mute_get(*t, 1) == 1 ? INS_ENABLED : 0);
    }
    (*t)++;
    return 1;
}
// Nor a channel rack solo of ours: each channel gets its on/off from before it (CH_PRESOLO), event 0 after the
// channel's EV_NEWCHAN (its index), as everyone else has it.
#define EV_CHON 0                          // byte: the channel is on
static int chsolostrip_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    int *ch = (int *)ctx;
    (void)at; (void)end;
    if (id == EV_NEWCHAN && n >= 2) *ch = *(const uint16_t *)p;
    else if (id == EV_CHON && n == 1 && *ch >= 0) {
        uint32_t v = chmute_presolo(*ch, *p ? 1u : 0u);
        if (v != REC_NONE) *(unsigned char *)p = (unsigned char)v;
    }
    return 1;
}

// Leader: snapshot of the current project (FL's own project writer; the current file
// name stays as it is). Returns malloc'd bytes or NULL. Main thread.
static int g_ownSave = 0;                  // inside it: not a save to announce (save_detour)
static unsigned char *project_snapshot(size_t *n) {
    wchar_t path[MAX_PATH];
    char r = 0;
    *n = 0;
    swprintf_s(path, MAX_PATH, L"%S\\snapshot_%ld.flp", g_tmpDir, InterlockedIncrement(&g_tmpSeq));
    g_ownSave = 1;
    __try { r = ((saveproj_t)(g_flBase + (SAVEPROJ_VA - IMG_BASE)))(dstr_keep(path), 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("PROJECT snapshot faulted"); r = 0; }
    g_ownSave = 0;
    if (!r) { logmsg("PROJECT snapshot failed"); return NULL; }
    unsigned char *d = read_file_w(path, n);
    DeleteFileW(path);
    if (d && solo_scan()) { int t = 0; flp_walk(d, *n, solostrip_cb, &t); }
    if (d && g_chSolo) { int ch = -1; flp_walk(d, *n, chsolostrip_cb, &ch); }
    return d;
}

// Leader, after a local load: work on <name>_flim_MMDD_hhmm.flp (a copy next to the original) and
// send that file to the members. Untitled projects (new from template) go as a snapshot.
static void song_dir_set(const wchar_t *dir);
static int temp_project_rescue(const wchar_t *cur);
static void leader_name_project(void);
static void leader_project_loaded(void) {
    wchar_t cur[MAX_PATH], flim[MAX_PATH + 16];
    size_t n = 0;
    unsigned char *d = NULL;
    song_dir_set(L"");                                   // another song: samples from now on go to its folder
    fl_ustr(PROJNAME_VA, cur, MAX_PATH);
    if (temp_project_rescue(cur)) return;                // opened from a zip: reopened from its song folder first
    if (!cur[0] || !file_exists_w(cur)) { leader_name_project(); fl_ustr(PROJNAME_VA, cur, MAX_PATH); }   // new: the song name, if set
    if (cur[0] && file_exists_w(cur)) {
        if (!is_flim_name(cur)) {
            flim_name(cur, flim, MAX_PATH + 16);
            backup_existing(flim);                       // only if reopened within the same minute
            if (CopyFileW(cur, flim, FALSE)) {
                set_project_file(flim);
                logmsg("PROJECT working copy %S (original %S is never saved by flim sessions)", flim, cur);
                wcscpy_s(cur, MAX_PATH, flim);
            } else logmsg("PROJECT could not create %S (error %lu); staying on the original", flim, GetLastError());
        }
        d = read_file_w(cur, &n);
        if (d && solo_scan()) { int t = 0; flp_walk(d, n, solostrip_cb, &t); }
        if (d) { send_project(d, n, base_name(cur)); free(d); return; }
    }
    d = project_snapshot(&n);
    if (d) { send_project(d, n, NULL); free(d); }
}

static void proj_end(void) {
    InterlockedExchange(&g_projDoneQueued, 0);
    InterlockedExchange(&g_projRemote, 0);
    InterlockedExchange(&g_projBusy, 0);
}

static void replay_held(void) {
    char **h;
    int n, i;
    EnterCriticalSection(&g_heldLock);
    h = g_held; n = g_nheld;
    g_held = NULL; g_nheld = g_capheld = 0;
    LeaveCriticalSection(&g_heldLock);
    if (n) logmsg("PROJECT replaying %d ops that arrived during the load", n);
    for (i = 0; i < n; i++) { handle_msg(h[i]); free(h[i]); }
    free(h);
}

static void do_projdone(void) {
    int remote = g_projRemote, same = 0;
    {
        // a real load replaces every channel object; same objects = cancelled open (or a big
        // preset change that tripped the bulk guard): nothing to send. FL can reallocate the
        // new channels at the old addresses, so a changed project file also counts as a load.
        void **items; int n = chan_list(&items), i;
        wchar_t now[MAX_PATH];
        same = items && g_nChKnown > 0 && n == g_nChKnown;
        for (i = 0; same && i < n; i++) if (items[i] != g_chKnown[i]) same = 0;
        fl_ustr(PROJNAME_VA, now, MAX_PATH);
        if (_wcsicmp(now, g_projNameAtBegin) != 0) same = 0;
    }
    proj_rebaseline();
    if (same && !remote && !closing_now()) {
        logmsg("PROJECT unchanged after %lu ms (open cancelled?): nothing sent", GetTickCount() - g_projSince);
        proj_end();
        return;
    }
    if (closing_now()) { logmsg("PROJECT settled while FL is closing: nothing sent"); proj_end(); return; }
    logmsg("PROJECT settled after %lu ms (%d channels, %ld params)", GetTickCount() - g_projSince, g_nChKnown, g_nslot);
    if (remote && g_projExpectCh >= PROJ_CH_MASS && g_nChKnown * 2 < g_projExpectCh) {
        // FL ended the load with most of the song missing: cancelled half-way (a tester's slow load stopped at 1 of 44
        // channels). Replaying the others' ops onto it only got it further out of step. Loaded again once; a second
        // time, the user is told (they may be cancelling on purpose) and "Resync from host" is theirs to press.
        int expect = g_projExpectCh;
        proj_end();
        if (++g_projShortLoads <= 1) {
            logmsg("PROJECT the leader's project has %d channels, FL ended its load with %d (cancelled?): loading it again", expect, g_nChKnown);
            g_resyncAt = 0;
            resync("the leader's project didn't load completely");
        } else {
            char a[400];
            g_projShortLoads = 0;
            logmsg("PROJECT the leader's project has %d channels, FL ended its load with %d again: not loading it a third time", expect, g_nChKnown);
            sprintf_s(a, sizeof(a), "The session's project didn't finish loading in FL (it has %d channels, FL ended with %d). "
                      "If the load was stopped, press \"Resync from host...\" in flim's window when you're ready to load it.", expect, g_nChKnown);
            gui_push(GUI_ALERT, a);
        }
        return;
    }
    if (remote) { g_projShortLoads = 0; view_restore(); g_haveSong = 1; g_replayAt = GetTickCount(); InterlockedExchange(&g_replaying, 1); proj_end(); replay_held(); member_reloading(); return; }
    proj_end();
    if (g_isLeader) leader_project_loaded();
    else {
        logmsg("PROJECT loaded locally on a member: re-syncing to the leader's project");
        ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"state.req\"}}");
    }
}

static void song_of(const wchar_t *name, wchar_t *out, int cap);
static void song_dir_of(const wchar_t *name, wchar_t *dir, int cap);
// Leader (main thread): FL's project is untitled and flim's window has a song name (flim.ini
// song_name): save it as <projects>\<song>\<song>.flp and make that FL's file (title, Ctrl+S), so
// the song, its folder and everyone's copies are named after it.
static void leader_name_project(void) {
    wchar_t cur[MAX_PATH], name[MAX_PATH], song[MAX_PATH], dir[MAX_PATH], path[MAX_PATH * 2];
    unsigned char *d;
    size_t n = 0;
    if (!g_isLeader || g_projBusy) return;
    fl_ustr(PROJNAME_VA, cur, MAX_PATH);
    flimcfg_get(L"song_name", name, MAX_PATH);
    if ((cur[0] && file_exists_w(cur)) || !name[0]) return;   // saved already (new = "untitled.flp", no file) / no name
    song_of(name, song, MAX_PATH);
    song_dir_of(name, dir, MAX_PATH);
    swprintf_s(path, MAX_PATH * 2, L"%s\\%s.flp", dir, song);
    if (file_exists_w(path)) {                            // never over an earlier song of that name
        wchar_t t[MAX_PATH * 2];
        flim_name(path, t, MAX_PATH * 2);
        wcscpy_s(path, MAX_PATH * 2, t);
    }
    if (!(d = project_snapshot(&n))) return;
    SHCreateDirectoryExW(NULL, dir, NULL);
    if (write_file_w(path, d, n)) {
        set_project_file(path);
        song_dir_set(dir);
        logmsg("PROJECT named \"%S\": saved as %S", song, path);
    } else logmsg("PROJECT could not write %S", path);
    free(d);
}

// Member: open the leader's project. it->path = UTF-16 path of the received file.
static int newchan_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    (void)p; (void)n; (void)at; (void)end;
    if (id == EV_NEWCHAN) (*(int *)ctx)++;
    return 1;
}
static void do_projload(const QItem *it) {
    const wchar_t *path = (const wchar_t *)it->path;
    void *form = fl_form(), *proj = fl_project();
    char r = 0;
    DWORD t0 = GetTickCount();
    if (!form || !proj) { logmsg("PROJECT load: FL objects not found"); return; }
    view_save();                                              // (do_projdone puts this FL's own view back)
    {   // how many channels it has: a load cancelled half-way ends with fewer (do_projdone)
        size_t n = 0;
        unsigned char *d = read_file_w(path, &n);
        int c = 0;
        if (d) { flp_walk(d, n, newchan_cb, &c); free(d); }
        InterlockedExchange(&g_projExpectCh, c);
    }
    wchar_t *w = dstr_keep(path);
    __try {
        ((formfn_t)(g_flBase + (BUSYBEGIN_VA - IMG_BASE)))(form);
        g_flimProjLoad = 1;
        r = ((projload_t)(*(void ***)proj)[FLPROJ_LOAD_SLOT])(proj, w, 0);   // flags 0: no "save changes?" prompt
        g_flimProjLoad = 0;
        ((formfn_t)(g_flBase + (BUSYEND_VA - IMG_BASE)))(form);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("PROJECT load faulted"); }
    if (it->mode) set_project_file(path);                     // named: Ctrl+S saves the member's own copy
    logmsg("PROJECT opened %S ret=%d in %lu ms", path, r, GetTickCount() - t0);
}

// This FL's channel list no longer matches the others' (an op made for another list): carrying on,
// every later channel op would land on the wrong channel. The leader's project is the reference: a
// member asks for it again, the leader sends it to everyone. At most once every RESYNC_MS.
#define RESYNC_MS 10000
// One re-sync per RESYNC_MS. One asked for sooner isn't dropped (it stayed out of step until the safety
// net, minutes later): it goes out when its turn comes (resync_tick), unless the leader's project arrives
// first (on_project_load), which covers it.
static volatile LONG g_resyncPending = 0;
static volatile DWORD g_resyncAt = 0;   // (declared with g_projRemote)
static volatile LONG g_userResync = 0;  // a re-sync the user asked for: sync-only (host controls) doesn't hold it back
static void resync(const char *why) {
    DWORD now = GetTickCount();
    if (g_syncOnly && !g_userResync) {      // its own edits stay here: a reload would make everyone reload
        logmsg("SYNC out of step with the others (%s): sync-only, so not reloading (\"Resync from host\" does)", why);
        return;
    }
    if ((g_resyncAt && now - g_resyncAt < RESYNC_MS) || (g_isLeader && g_projBusy)) {
        if (!InterlockedExchange(&g_resyncPending, 1)) logmsg("SYNC out of step with the others (%s): re-syncing when the last re-sync's turn is over", why);
        return;
    }
    g_resyncAt = now;
    InterlockedExchange(&g_resyncPending, 0);
    logmsg("SYNC out of step with the others (%s): %s", why,
           g_isLeader ? "sending everyone the leader's project" : "loading the leader's project again");
    if (!g_isLeader) ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"state.req\"}}");
    else enqueue_unit(T_PROJSNAP, 0, 0, 0, 0, NULL);
}
// "Resync from host" in the flim window (it asked first): now, not held to RESYNC_MS.
static void resync_asked(void) {
    if (g_netState != NET_ON) logmsg("SYNC resync asked for, but not connected");
    else if (member_reloading() || (g_isLeader && g_projBusy)) logmsg("SYNC resync asked for: one is already under way");
    else {
        g_resyncAt = 0;
        InterlockedExchange(&g_userResync, 1);
        resync("asked for in the flim window");
        InterlockedExchange(&g_userResync, 0);
    }
}
// Poll thread.
static void resync_tick(DWORD now) {
    if (g_resyncPending && now - g_resyncAt >= RESYNC_MS && !g_projBusy) resync("a re-sync that waited for its turn");
}

// Leader: a member asked for the project (join or re-sync).
static void do_projsnap(void) {
    wchar_t cur[MAX_PATH];
    size_t n = 0;
    unsigned char *d;
    // A structure change of ours not sent yet goes out first, as ops ahead of the project: sent after it, the
    // members (who have it from the project) did it a second time.
    // (directly: do_struct may put it off while peer ops wait)
    if (chan_list_changed()) { void **items; int nc = chan_list(&items); if (items) chan_struct_local(items, nc, GetTickCount()); }
    if (mix_layout_changed()) mix_struct();
    d = project_snapshot(&n);
    if (!d) return;
    fl_ustr(PROJNAME_VA, cur, MAX_PATH);
    send_project(d, n, cur[0] ? base_name(cur) : NULL);
    free(d);
}

// ---- song folders ----
// One folder per song in the projects folder (flim.ini projects_dir, set in flim's window): its
// session copies (<song>_flim_MMDD_hhmm.flp, a portable .zip of each), the samples received from
// the others (flat, under their own names) and Backups\. Song = the project's file name without
// ".flp" and flim's "_flim_MMDD_hhmm".
static void song_of(const wchar_t *name, wchar_t *out, int cap) {
    size_t n;
    int i;
    wcscpy_s(out, cap, name && name[0] ? base_name(name) : L"untitled");
    for (i = 0; out[i]; i++) if (wcschr(L"\\/:*?\"<>|", out[i]) || out[i] < 32) out[i] = L'_';
    if (is_flim_name(out)) out[wcslen(out) - (ends_with_ci(out, L"_flim.flp") ? 9 : 19)] = 0;
    else if (ends_with_ci(out, L".flp") || ends_with_ci(out, L".zip")) out[wcslen(out) - 4] = 0;
    n = wcslen(out);
    while (n && (out[n - 1] == L'.' || out[n - 1] == L' ')) out[--n] = 0;       // Windows drops these
    if (!n) wcscpy_s(out, cap, L"untitled");
}
static void song_dir_of(const wchar_t *name, wchar_t *dir, int cap) {
    wchar_t s[MAX_PATH];
    song_of(name, s, MAX_PATH);
    flimcfg_projects_dir(dir, cap);
    wcscat_s(dir, cap, L"\\");
    wcscat_s(dir, cap, s);
}

// Member: the song folder for the leader's project in op m and the session copy in it
// (<song>_flim_MMDD_hhmm.flp; a name that already is a session copy stays as is).
static void join_dir_of(const char *m, wchar_t *dir, int cap, wchar_t *flp, int fcap) {
    const char *nk = strstr(m, "\"name\":\"");
    wchar_t name[MAX_PATH];
    char nm[MAX_PATH * 3];
    int o = 0, i;
    if (nk)
        for (nk += 8; *nk && *nk != '"' && o < (int)sizeof(nm) - 1; nk++) {
            if (*nk == '\\' && nk[1]) nk++;
            nm[o++] = *nk;
        }
    nm[o] = 0;
    name[0] = 0;
    if (o) MultiByteToWideChar(CP_UTF8, 0, nm, -1, name, MAX_PATH);
    for (i = 0; name[i]; i++) if (wcschr(L"\\/:*?\"<>|", name[i]) || name[i] < 32) name[i] = L'_';
    if (!name[0]) wcscpy_s(name, MAX_PATH, L"untitled.flp");
    if (!is_flim_name(name)) {                                 // members also work on a session copy
        wchar_t tmp[MAX_PATH];
        flim_name(name, tmp, MAX_PATH);
        wcscpy_s(name, MAX_PATH, tmp);
    }
    song_dir_of(name, dir, cap);
    if (flp) swprintf_s(flp, fcap, L"%s\\%s", dir, name);
}
static const char *g_joinMsg;                // the project.load samples_resolve put into g_joinDir (inbox thread)
static wchar_t g_joinDir[MAX_PATH], g_joinFlp[MAX_PATH * 2];
static void song_dir_set(const wchar_t *dir);
static void zip_song_async(const wchar_t *dir);

// Member: store the leader's project as the session copy in its song folder (samples_resolve put
// its samples there), zip it with its samples as a portable copy once the downloads are in, and
// queue the load. Samples that come with later ops go into the same folder.
static void on_project_load(const char *m) {
    const char *dk = strstr(m, "\"data\":\"");
    wchar_t dir[MAX_PATH], path[MAX_PATH * 2];
    int i;
    size_t n = 0;
    if (!dk) return;
    if (g_joinMsg == m && g_joinDir[0]) { wcscpy_s(dir, MAX_PATH, g_joinDir); wcscpy_s(path, MAX_PATH * 2, g_joinFlp); }   // same minute stamp
    else join_dir_of(m, dir, MAX_PATH, path, MAX_PATH * 2);
    g_joinMsg = NULL;
    dk += 8;
    const char *e = strchr(dk, '"');
    if (!e) return;
    unsigned char *d = b64dec(dk, (size_t)(e - dk), &n);
    samples_rewrite(&d, &n);
    SHCreateDirectoryExW(NULL, dir, NULL);
    backup_existing(path);
    if (!write_file_w(path, d, n)) { logmsg("PROJECT could not write %S", path); free(d); return; }
    free(d);
    song_dir_set(dir);
    zip_song_async(path);
    // supersede anything queued or held from the previous project
    EnterCriticalSection(&g_heldLock);
    for (i = 0; i < g_nheld; i++) free(g_held[i]);
    g_nheld = 0;
    LeaveCriticalSection(&g_heldLock);
    InterlockedExchange(&g_projBusy, 1);
    InterlockedExchange(&g_projRemote, 1);
    g_projAskedAt = 0;                                         // (it came: member_reloading goes by g_projBusy now)
    InterlockedExchange(&g_resyncPending, 0);                  // (a re-sync asked for before it: this is the one)
    InterlockedExchange(&g_projDoneQueued, 0);
    g_projSince = GetTickCount();
    logmsg("PROJECT received %S (%zu bytes): loading", path, n);
    QItem q; memset(&q, 0, sizeof(q));
    q.type = T_PROJLOAD;
    q.mode = 1;                                                // named: Ctrl+S saves the member's own copy
    q.path = (char *)_wcsdup(path);
    enqueue(&q);
}

// Member (recv thread): keep ops for after the load. Returns 1 if held.
static int hold_if_loading(const char *m) {
    if (!g_projBusy || g_isLeader || !strstr(m, "\"t\":\"op\"")) return 0;
    EnterCriticalSection(&g_heldLock);
    if (g_nheld == g_capheld) { g_capheld = g_capheld ? g_capheld * 2 : 64; g_held = (char **)realloc(g_held, sizeof(char *) * g_capheld); }
    g_held[g_nheld++] = _strdup(m);
    LeaveCriticalSection(&g_heldLock);
    return 1;
}

// Poll thread, project mode: absorb everything, wait for FL's load to end and go quiet.
static void proj_poll(DWORD now) {
    static uint64_t sig = 0;
    static DWORD sigSince = 0, lastRescan = 0;
    int k, n, t, s, p;
    if (fl_loading() || g_inHook) { sigSince = now; return; }          // don't touch FL mid-load
    if (now - lastRescan > 500) { rescan(); lastRescan = now; }
    for (k = 0; k < g_nslot; k++) g_slots[k].last = read_value(g_slots[k].id);
    {
        void **items; uint64_t h = 1469598103934665603ull;
        n = chan_list(&items);
        for (k = 0; k < n && items; k++) { h ^= (uint64_t)(uintptr_t)items[k]; h *= 1099511628211ull; }
        for (t = 0; t < mix_count() && t < MAX_MIX; t++)
            for (s = 0; s < MAX_FX_SLOTS; s++) { h ^= (uint64_t)(uintptr_t)fx_obj(t, s); h *= 1099511628211ull; }
        for (p = 1; p < MAX_PAT; p += 1) { h ^= notes_hash(p); h *= 1099511628211ull; }
        h ^= (uint64_t)g_nslot * 0x9E3779B97F4A7C15ull;
        if (h != sig) { sig = h; sigSince = now; }
    }
    if (g_projDoneQueued) return;
    if ((now - g_projSince >= 1000 && now - sigSince >= PROJ_SETTLE_MS) || now - g_projSince > PROJ_MAX_MS) {
        InterlockedExchange(&g_projDoneQueued, 1);
        enqueue_unit(T_PROJDONE, 0, 0, 0, 0, NULL);
    }
}

// ---------------- pattern structure (insert, clone, delete, split...) ----------------
// These renumber patterns, so they can't travel as per-pattern edits (every later pattern would
// look changed). FL runs them as TFLProject virtual methods; we hook those vmt slots, send the
// call itself once FL has done it here (nothing if it was cancelled), and the peers make the same
// call. Afterwards both sides re-take their baselines: the renumbered patterns aren't edits.
// Open-array arguments arrive as (pointer, high index).
#define PATOP_N       6
static const int PATOP_SLOT[PATOP_N] = {   // FL 2026 vmt offsets (25.2.3 had d8 e0 e8 f0 f8 100)
    0xe8,       // Clone(pats[])                       EEPatCloneMenuClick
    0xf8,       // Delete(pats[]): asks for confirmation EEPatDelMenuClick
    0x108,      // (pats[], delta, target): move / drag-rearrange (patterns.movePattern)
    0x110,      // (pats[], a, b, c)
    0x118,      // Insert(pattern): "Insert one"       EEPatInsMenuClick
    0x120,      // SplitByChannel(pats[])              EEPatChanSplitMenuClick
};
#define PATOP_LO      0xe8         // vmt range the hooks write to
#define PATOP_HI      0x120
#define PATOP_INSERT  4            // the one slot that takes a single pattern number
#define MSGBOX_VA     FLVA(MSGBOX) // FL's message box (text, flags, callback) -> button id (12 bytes, detoured)
#define MSGBOX_CORE_VA FLVA(MSGBOX_CORE) //   what it calls: (text, flags, 3, callback)
#define MSGBOX2_VA    FLVA(MSGBOX2) // FL's message box with extra buttons (text, flags, buttons, x, callback) -> button
                                    //   id (12 bytes, detoured): "made with a newer FL" when loading, error dialogs
#define MSGBOX2_CORE_VA FLVA(MSGBOX2_CORE) // what it calls: (text, flags, buttons, x, 3, callback)
typedef uint64_t (*patop_t)(void *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
typedef int (*msgcore_t)(void *text, uint32_t flags, uint8_t kind, void *cb);
typedef int (*msgcore2_t)(void *text, uint32_t flags, void *buttons, void *x, uint8_t kind, void *cb);
static patop_t g_patOrig[PATOP_N];
static volatile LONG g_patRemote = 0;     // replaying a peer's op: don't send it back
static volatile LONG g_mbAuto = 0;        // main thread: FL message boxes are answered without showing

// A plugin FL can't open while we load someone's state (not installed here, ...): FL's reporter
// (sub_12B88D0) shows "Warning|The plugin |NAME| is missing. ..." through this message box. It shows
// once per plugin; later ones are answered here and noted in flim's window. The state still loads:
// FL keeps a silent placeholder that holds the plugin and its settings.
#define WARNED_MAX 64
static wchar_t *g_warned[WARNED_MAX];
static DWORD    g_warnedLog[WARNED_MAX];
static int      g_nwarned = 0;
// "Warning|The plugin |NAME| ..." -> NAME (between FL's highlight bars). 0 = not a plugin warning.
static int plugin_warning(const wchar_t *t, wchar_t *name, int cap) {
    const wchar_t *a, *b;
    if (wcsncmp(t, L"Warning|", 8) != 0 || !wcsstr(t, L"plugin")) return 0;
    a = wcschr(t + 8, L'|');
    b = a ? wcschr(a + 1, L'|') : NULL;
    if (!b || b == a + 1 || b - a - 1 >= cap) return 0;
    wcsncpy_s(name, cap, a + 1, (size_t)(b - a - 1));
    return 1;
}
// Main thread, a plugin warning during our load: 1 = answer it here (already shown once).
static int plugin_warning_seen(const wchar_t *name) {
    DWORD now = GetTickCount();
    int i;
    for (i = 0; i < g_nwarned && wcscmp(g_warned[i], name) != 0; i++) {}
    if (i < g_nwarned) {
        if (now - g_warnedLog[i] > 30000) {
            g_warnedLog[i] = now;
            logmsg("PLUGIN %S (not installed here) was updated: kept in its placeholder", name);
        }
        return 1;
    }
    if (g_nwarned < WARNED_MAX) { g_warned[g_nwarned] = _wcsdup(name); g_warnedLog[g_nwarned++] = 0; }
    logmsg("PLUGIN %S isn't installed here: its channel or effect keeps a silent placeholder with the plugin's "
           "settings. FL warns once; later updates are noted here.", name);
    return 0;
}

// Detour target for FL's message box. A peer's op must not wait for a dialog here: answer
// OK / Yes (what the peer already chose) while g_mbAuto is set; otherwise FL's own code.
static int msgbox_detour(void *text, uint32_t flags, void *cb) {
    wchar_t name[128];
    if (g_mbAuto) {
        uint32_t t = flags & 0xf;
        logmsg("PATTERN op: answered FL's dialog (flags 0x%x)", flags);
        return (t == MB_YESNO || t == MB_YESNOCANCEL) ? IDYES : IDOK;
    }
    if (g_flimLoad && text && plugin_warning((const wchar_t *)text, name, 128) && plugin_warning_seen(name)) return IDOK;
    return ((msgcore_t)(g_flBase + (MSGBOX_CORE_VA - IMG_BASE)))(text, flags, 3, cb);
}
// Someone's FL is newer than ours: loading their project or plugin state asks "This project / preset has
// been created using a more recent version of FL Studio (|x|) than yours (|this build|)... continue?"
// (Yes / No / Get latest version) every time. While flim loads: Yes, and say so once in flim's window.
// Recognized by our own build number in the text, whatever FL's UI language.
static int msgbox2_detour(void *text, uint32_t flags, void *buttons, void *x, void *cb) {
    static volatile LONG told = 0;
    if ((g_flimLoad || g_flimProjLoad) && text && g_flBuild[0] && wcsstr((const wchar_t *)text, g_flBuild)) {
        if (!InterlockedExchange(&told, 1))
            logmsg("PROJECT someone here uses a newer FL Studio than this one (%S): their changes are loaded anyway. "
                   "If something doesn't sound or open right, update FL Studio.", g_flBuild);
        return (flags & 0xf) == MB_YESNO || (flags & 0xf) == MB_YESNOCANCEL ? IDYES : IDOK;
    }
    return ((msgcore2_t)(g_flBase + (MSGBOX2_CORE_VA - IMG_BASE)))(text, flags, buttons, x, 3, cb);
}
// Replace FL's function at `at` (whose first 12 bytes must be `pro`) with a jump to target.
static int detour12(unsigned char *at, const char *pro, void *target) {
    unsigned char jmp[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };   // mov rax, imm64; jmp rax
    DWORD old;
    if (memcmp(at, pro, 12) != 0 || !VirtualProtect(at, sizeof(jmp), PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(jmp + 2, &target, 8);
    memcpy(at, jmp, sizeof(jmp));
    VirtualProtect(at, sizeof(jmp), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(jmp));
    return 1;
}

// Everything a pattern op can renumber: notes, names, colors, lengths, markers.
static uint64_t pat_sig(void) {
    uint64_t h = 1469598103934665603ull, x;
    int p;
    for (p = 1; p < MAX_PAT; p++) {
        x = notes_hash(p); h = fnv_add(h, &x, 8);
        x = meta_hash(M_PAT, p); h = fnv_add(h, &x, 8);
        x = meta_hash(M_PMARK, p); h = fnv_add(h, &x, 8);
    }
    return h;
}

static uint64_t patop_common(int k, void *self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    int send = !g_patRemote && !g_patDepth && g_ws && !g_projBusy && !closing_now();
    uint64_t sig = send ? pat_sig() : 0, r;
    static int pl[MAX_PAT];                      // the pattern list, as it was before the op
    int np = 1, ok = 1, i;
    if (send) {
        if (k == PATOP_INSERT) pl[0] = (int)a;
        else {
            np = (int)b + 1 < MAX_PAT ? (int)b + 1 : MAX_PAT;
            __try { memcpy(pl, (const int *)a, sizeof(int) * (size_t)(np > 0 ? np : 0)); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        }
        if (ok && np > 0) pat_sigs_take(k, pl, np);
    }
    InterlockedIncrement(&g_patDepth);
    r = g_patOrig[k](self, a, b, c, d, e);
    InterlockedDecrement(&g_patDepth);
    if (!send) return r;
    if (pat_sig() == sig) { logmsg("PATTERN op 0x%x: nothing changed (cancelled?), not sent", PATOP_SLOT[k]); return r; }
    if (!ok || np < 1) { logmsg("PATTERN op 0x%x: bad pattern list, not sent", PATOP_SLOT[k]); notes_rebaseline(); return r; }
    {
        char m[8192];
        int o = sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"pat.op\",\"s\":%d,\"p\":[", PATOP_SLOT[k]);
        for (i = 0; i < np; i++) o += sprintf_s(m + o, sizeof(m) - o, "%s%d", i ? "," : "", pl[i]);
        o += sprintf_s(m + o, sizeof(m) - o, "],\"a\":%d,\"b\":%d,\"c\":%u", (int)c, (int)d, (unsigned)(uint16_t)e);
        if (k == 5) {                                // split: each source's notes, as a hash (do_patop)
            o += sprintf_s(m + o, sizeof(m) - o, ",\"chh\":[");
            for (i = 0; i < np && o < (int)sizeof(m) - 16; i++)
                o += sprintf_s(m + o, sizeof(m) - o, "%s%u", i ? "," : "", pl[i] >= 1 && pl[i] < MAX_PAT ? g_patPreH[pl[i]] : 0);
            o += sprintf_s(m + o, sizeof(m) - o, "]");
        }
        sprintf_s(m + o, sizeof(m) - o, "}}");
        ws_send(m);
        logmsg("PATTERN op sent: %.200s", m);
    }
    sc_done_patop(k, pl, np, (int)c, (int)d);    // counted, and the baselines carried through it
    return r;
}
#define PATOP_HOOK(k) static uint64_t patop_hook##k(void *s, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) \
    { return patop_common(k, s, a, b, c, d, e); }
PATOP_HOOK(0) PATOP_HOOK(1) PATOP_HOOK(2) PATOP_HOOK(3) PATOP_HOOK(4) PATOP_HOOK(5)
static const patop_t PATOP_HOOKS[PATOP_N] = { patop_hook0, patop_hook1, patop_hook2, patop_hook3, patop_hook4, patop_hook5 };

// Once, when FL's project object exists: hook the vmt slots and detour the message boxes.
static void patops_install(void) {
    void **vmt;
    unsigned char *mb = (unsigned char *)(g_flBase + (MSGBOX_VA - IMG_BASE));
    DWORD old;
    int k;
    char *proj = (char *)fl_project();
    if (!proj) { logmsg("PATTERN ops: no project object, not hooked"); return; }
    vmt = *(void ***)proj;
    // the detour replaces the whole function: check it is the 12-byte prologue we know
    if (memcmp(mb, "\x48\x83\xec\x28\xb0\x03\x4d\x89\xc1\x41\x89\xc0", 12) != 0) {
        logmsg("PATTERN ops: FL's message box looks different, not hooked");
        return;
    }
    if (!VirtualProtect(vmt + PATOP_LO / 8, PATOP_HI - PATOP_LO + 8, PAGE_EXECUTE_READWRITE, &old)) { logmsg("PATTERN ops: vmt not writable"); return; }
    for (k = 0; k < PATOP_N; k++) {
        g_patOrig[k] = (patop_t)vmt[PATOP_SLOT[k] / 8];
        vmt[PATOP_SLOT[k] / 8] = (void *)PATOP_HOOKS[k];
    }
    VirtualProtect(vmt + PATOP_LO / 8, PATOP_HI - PATOP_LO + 8, old, &old);
    detour12(mb, "\x48\x83\xec\x28\xb0\x03\x4d\x89\xc1\x41\x89\xc0", (void *)msgbox_detour);
    if (!detour12((unsigned char *)(g_flBase + (MSGBOX2_VA - IMG_BASE)), "\x48\x83\xec\x38\xc6\x44\x24\x20\x03\x48\x8b\x44",
                  (void *)msgbox2_detour))
        logmsg("PATTERN ops: FL's other message box looks different: a newer FL's projects will ask before loading");
    logmsg("PATTERN ops hooked (vmt %p)", (void *)vmt);
}

// Main thread, from the message hook (also inside modal loops): while a peer's op is replayed,
// FL's follow-up dialogs (Insert opens a rename box) are closed as if cancelled. The peer's
// own choice (e.g. the name) arrives as its own edit.
static BOOL CALLBACK close_name_dlg(HWND h, LPARAM lp) {
    DWORD pid = 0;
    (void)lp;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && wnd_class_is(h, "TNameEditForm")) {
        PostMessageW(h, WM_CLOSE, 0, 0);
        logmsg("PATTERN op: closed FL's rename box");
    }
    return TRUE;
}
static void patop_dialogs(void) {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (!g_mbAuto || now - last < 50) return;
    last = now;
    EnumWindows(close_name_dlg, 0);
}

static void on_pat_op(const char *m) {
    QItem q; memset(&q, 0, sizeof(q));
    q.type = T_PATOP; q.path = _strdup(m);
    enqueue(&q);
}


// A peer's pattern op (main thread): the same call here, dialogs answered as the peer did.
static void do_patop(const QItem *it) {
    const char *m = it->path, *q = strstr(m, "\"p\":[");
    int slot = 0, k, n = 0, a = 0, b = 0, c = 0;
    static int pats[MAX_PAT];
    void *proj = fl_project();
    if (!q || !proj || !int_after(m, NULL, "\"s\":", &slot)) return;
    for (k = 0; k < PATOP_N && PATOP_SLOT[k] != slot; k++) {}
    if (k == PATOP_N || !g_patOrig[k]) { logmsg("PATTERN op 0x%x: not supported here", slot); return; }
    for (q += 5; *q && *q != ']' && n < MAX_PAT; ) {
        char *e;
        pats[n++] = (int)strtol(q, &e, 10);
        if (e == q) break;
        q = e;
        if (*q == ',') q++;
    }
    if (!n) return;
    int_after(m, NULL, "\"a\":", &a);
    int_after(m, NULL, "\"b\":", &b);
    int_after(m, NULL, "\"c\":", &c);
    pat_sigs_take(k, pats, n);                               // (sc_done_patop checks the map against what FL did)
    if (k == 5 && (q = strstr(m, "\"chh\":[")) != NULL) {
        // a split moves a source's notes into new patterns: if a source here has other notes than the sender's
        // had (a note edit still on its way), the same split would come out different. Not that: re-sync.
        int x = 0;
        for (q += 7; x < n && *q && *q != ']'; x++) {
            char *e;
            uint32_t want = (uint32_t)strtoul(q, &e, 10);
            if (e == q) break;
            q = *e == ',' ? e + 1 : e;
            if (pats[x] >= 1 && pats[x] < MAX_PAT && want != g_patPreH[pats[x]]) {
                logmsg("PATTERN split of %d: its notes here aren't the sender's; re-syncing instead", pats[x]);
                resync("a pattern split crossed a note edit");
                return;
            }
        }
    }
    InterlockedExchange(&g_patRemote, 1);
    InterlockedExchange(&g_mbAuto, 1);
    InterlockedIncrement(&g_patDepth);
    __try {
        if (k == PATOP_INSERT) g_patOrig[k](proj, (uint64_t)(int64_t)pats[0], 0, 0, 0, 0);
        else g_patOrig[k](proj, (uint64_t)pats, (uint64_t)(int64_t)(n - 1), (uint64_t)(int64_t)a, (uint64_t)(int64_t)b, (uint64_t)(uint16_t)c);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("PATTERN op 0x%x faulted", slot); slot = -1; }
    InterlockedDecrement(&g_patDepth);
    InterlockedExchange(&g_mbAuto, 0);
    InterlockedExchange(&g_patRemote, 0);
    if (slot >= 0) sc_done_patop(k, pats, n, a, b);         // (faulted: run_task counts it; baselines re-taken)
    else notes_rebaseline();
    logmsg("PATTERN op applied: slot 0x%x, %d patterns (first %d)", slot, n, pats[0]);
}

// ---------------- unit locks (mutexing) ----------------
// One channel's notes in a pattern ("pat:<p>:<ch>") and one playlist track's clips in an
// arrangement ("clips:<a>:<track>") sync as whole groups, so two people editing the same group
// at once would overwrite each other; different channels / tracks never conflict. Whoever
// changes a group first takes its lock (server-arbitrated; ops carry the key and the server
// drops ops on keys someone else holds). While someone else holds it, a local change is
// reverted to the last agreed state and FL's hint bar says who is editing. A lock is released
// ULOCK_IDLE_MS after its owner's last change (plugin locks: the owner's own setting, g_plugLockMs,
// 1-10 s in flim's window). Params keep their own per-param locks.
#define ULOCK_MAX      4096
#define ULOCK_IDLE_MS  2000
#define HINT_EVERY_MS  1500
typedef struct {
    char  key[32];
    int   mine;              // we hold it (granted or optimistically requested)
    int   granted;           // the server confirmed it
    int   peer;              // someone else holds it
    DWORD lastLocal;         // our last change under it
    DWORD lastHint;
    int   stateSent;         // plugin key: a plug.state went out under it before the grant
    char  owner[64];
} ULock;
static ULock g_ulocks[ULOCK_MAX];
static int   g_nulocks = 0;
static CRITICAL_SECTION g_ulockLock;   // also guards the last/pending state buffers below
static char  g_peersJson[4096];        // last welcome/presence message (id -> name for hints)

// Per pattern / for the current arrangement's clips, whole-unit record arrays:
//   last  the agreed state (what a revert restores), per group
//   pend  what we sent and the server hasn't confirmed yet (only its sent groups matter)
//   base  what was last sent or applied: the next local change is diffed against it
typedef struct { unsigned char *d; unsigned n; int known; } UState;
static UState g_patLast[MAX_PAT], g_patPend[MAX_PAT], g_patBase[MAX_PAT];
static UState g_clipLast, g_clipPend, g_clipBase;
static int    g_clipLastArr = -1;                // arrangement g_clipLast / g_clipBase belong to

static void ustate_set(UState *s, const unsigned char *d, unsigned n, size_t rec) {
    free(s->d);
    s->d = n ? (unsigned char *)malloc((size_t)n * rec) : NULL;
    if (n) memcpy(s->d, d, (size_t)n * rec);
    s->n = n; s->known = 1;
}

// ---- groups inside a unit: a note's channel (NOTE_CH_OFS), a clip's playlist track ----
#define CLIP_TRK_OFS  0x0c         // clip record: u16 playlist track, reversed: 499 = track 1 (FLP event 233)
static size_t rec_size(int isPat) { return isPat ? NOTE_SIZE : CLIP_SIZE; }
static unsigned rec_group(int isPat, const unsigned char *r) { return *(const uint16_t *)(r + (isPat ? NOTE_CH_OFS : CLIP_TRK_OFS)); }

// group first, then the (normalized) bytes
static int note_gcmp(const void *a, const void *b) {
    int d = (int)rec_group(1, (const unsigned char *)a) - (int)rec_group(1, (const unsigned char *)b);
    return d ? d : memcmp(a, b, NOTE_SIZE);
}
static int clip_gcmp(const void *a, const void *b) {
    int d = (int)rec_group(0, (const unsigned char *)a) - (int)rec_group(0, (const unsigned char *)b);
    return d ? d : memcmp(a, b, CLIP_SIZE);
}
// Sorted copy without what isn't content: the selection bit, and a clip's per-instance id.
static unsigned char *recs_sorted(int isPat, const unsigned char *d, unsigned n) {
    size_t rs = rec_size(isPat);
    unsigned i;
    unsigned char *o = (unsigned char *)malloc((size_t)n * rs + 1);
    if (n) memcpy(o, d, (size_t)n * rs);
    for (i = 0; i < n; i++) {
        unsigned char *r = o + (size_t)i * rs;
        if (isPat) r[NOTE_SEL_OFS] &= (unsigned char)~NOTE_SEL_BIT;
        else { r[CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT; memset(r + CLIP_ID_OFS, 0, 4); }
    }
    qsort(o, n, rs, isPat ? note_gcmp : clip_gcmp);
    return o;
}
// The groups whose records differ between a and b (as sets). Writes up to cap of them to out.
static int group_diff(int isPat, const unsigned char *a, unsigned na, const unsigned char *b, unsigned nb, int *out, int cap) {
    size_t rs = rec_size(isPat);
    unsigned char *x = recs_sorted(isPat, a, na), *y = recs_sorted(isPat, b, nb);
    unsigned i = 0, j = 0;
    int k = 0;
    while ((i < na || j < nb) && k < cap) {
        unsigned ga = i < na ? rec_group(isPat, x + (size_t)i * rs) : 0x10000;
        unsigned gb = j < nb ? rec_group(isPat, y + (size_t)j * rs) : 0x10000;
        unsigned m = ga < gb ? ga : gb, i0 = i, j0 = j;
        while (i < na && rec_group(isPat, x + (size_t)i * rs) == m) i++;
        while (j < nb && rec_group(isPat, y + (size_t)j * rs) == m) j++;
        if (i - i0 != j - j0 || memcmp(x + (size_t)i0 * rs, y + (size_t)j0 * rs, (size_t)(i - i0) * rs) != 0) out[k++] = (int)m;
    }
    free(x); free(y);
    return k;
}
// Copy of the records of group g (g < 0: all of them). NULL if none.
static unsigned char *recs_group(int isPat, const unsigned char *d, unsigned n, int g, unsigned *on) {
    size_t rs = rec_size(isPat);
    unsigned i, k = 0;
    unsigned char *o = (d && n) ? (unsigned char *)malloc((size_t)n * rs) : NULL;
    for (i = 0; o && i < n; i++)
        if (g < 0 || (int)rec_group(isPat, d + (size_t)i * rs) == g) memcpy(o + (size_t)k++ * rs, d + (size_t)i * rs, rs);
    if (o && !k) { free(o); o = NULL; }
    *on = k;
    return o;
}
// dst := dst without group g + src's group-g records (g < 0: dst := src).
static void ustate_merge(UState *dst, int isPat, const unsigned char *src, unsigned sn, int g) {
    size_t rs = rec_size(isPat);
    unsigned i, k = 0;
    unsigned char *o;
    if (g < 0) { ustate_set(dst, src, sn, rs); return; }
    o = (unsigned char *)malloc(((size_t)dst->n + sn) * rs + 1);
    for (i = 0; i < dst->n; i++)
        if ((int)rec_group(isPat, dst->d + (size_t)i * rs) != g) memcpy(o + (size_t)k++ * rs, dst->d + (size_t)i * rs, rs);
    for (i = 0; i < sn; i++)
        if ((int)rec_group(isPat, src + (size_t)i * rs) == g) memcpy(o + (size_t)k++ * rs, src + (size_t)i * rs, rs);
    free(dst->d);
    dst->d = o; dst->n = k; dst->known = 1;
}
static void ustate_remap_ch(UState *s, int isPat, const int *toNew) {
    size_t rs;
    unsigned i, k;
    if (!s->known || !s->d) return;
    rs = rec_size(isPat);
    for (i = 0, k = 0; i < s->n; i++) {
        unsigned char *r = s->d + (size_t)i * rs;
        int old, neu;
        if (isPat) {
            old = (int)*(const uint16_t *)(r + NOTE_CH_OFS);
        } else {
            uint32_t item = *(const uint32_t *)(r + 0x04);
            if ((item & 0xFFFFu) != 0x5000u) {
                if (k != i) memmove(s->d + (size_t)k * rs, r, rs);
                k++;
                continue;
            }
            old = (int)(item >> 16);
        }
        if (old < 0 || old >= MAX_CH) {                 // not a channel (a pattern clip: (0x5000 + pattern) << 16 | 0x5000)
            if (k != i) memmove(s->d + (size_t)k * rs, r, rs);
            k++;
            continue;
        }
        neu = toNew[old];
        if (neu < 0) continue;
        if (isPat) *(uint16_t *)(r + NOTE_CH_OFS) = (uint16_t)neu;
        else *(uint32_t *)(r + 0x04) = ((uint32_t)neu << 16) | 0x5000u;
        if (k != i) memmove(s->d + (size_t)k * rs, r, rs);
        k++;
    }
    s->n = k;
}

static void notes_remap(const int *from, int n) {
    int toNew[MAX_CH];
    int p, i;
    for (i = 0; i < MAX_CH; i++) toNew[i] = -1;
    for (i = 0; i < n && i < MAX_CH; i++) if (from[i] >= 0 && from[i] < MAX_CH) toNew[from[i]] = i;
    EnterCriticalSection(&g_ulockLock);
    for (p = 1; p < MAX_PAT; p++) {
        ustate_remap_ch(&g_patLast[p], 1, toNew);
        ustate_remap_ch(&g_patPend[p], 1, toNew);
        ustate_remap_ch(&g_patBase[p], 1, toNew);
    }
    ustate_remap_ch(&g_clipLast, 0, toNew);
    ustate_remap_ch(&g_clipPend, 0, toNew);
    ustate_remap_ch(&g_clipBase, 0, toNew);
    LeaveCriticalSection(&g_ulockLock);
}

static void chan_rebase(const int *from, int n) {
    int i;
    slots_rebase(from, n, NULL, 0);
    for (i = 0; i < n; i++) if (from[i] < 0) watch_channel(i);
    meta_remap(M_CHAN, from, n);
    notes_remap(from, n);
    ulock_release_moved("plug:ch:", "pat:", from, n);
    InterlockedIncrement(&g_structGen);
    InterlockedExchange(&g_rescanNow, 1);           // (as mix_rebaseline: a new channel's values watched now)
}

// "pat:<p>[:<ch>]" / "clips:<a>[:<track>]" -> unit + group (-1 = whole unit). 0 = not such a key.
static int unit_key_parse(const char *key, int *isPat, int *idx, int *g) {
    *g = -1;
    if (sscanf_s(key, "pat:%d:%d", idx, g) >= 1) { *isPat = 1; return *idx >= 1 && *idx < MAX_PAT; }
    if (sscanf_s(key, "clips:%d:%d", idx, g) >= 1) { *isPat = 0; return 1; }
    return 0;
}

static ULock *ulock_get(const char *key, int create) {
    int i;
    for (i = 0; i < g_nulocks; i++) if (strcmp(g_ulocks[i].key, key) == 0) return &g_ulocks[i];
    if (!create) return NULL;
    if (g_nulocks >= ULOCK_MAX) {                 // full: reuse an entry nobody holds
        for (i = 0; i < g_nulocks && (g_ulocks[i].mine || g_ulocks[i].peer); i++) {}
        if (i == g_nulocks) return NULL;
    } else i = g_nulocks++;
    memset(&g_ulocks[i], 0, sizeof(ULock));
    strcpy_s(g_ulocks[i].key, sizeof(g_ulocks[0].key), key);
    return &g_ulocks[i];
}

static void peer_name(const char *id, char *out, int cap) {
    char pat[96];
    const char *p, *q;
    sprintf_s(pat, sizeof(pat), "\"id\":\"%s\",\"name\":\"", id);
    p = strstr(g_peersJson, pat);
    if (p && (p += strlen(pat), q = strchr(p, '"')) && q - p < cap) { memcpy(out, p, q - p); out[q - p] = 0; }
    else strcpy_s(out, cap, "another user");
}

// FL's hint bar (top left). Main thread. The string is kept: FL may hold on to it.
static void fl_hint(const char *msg) {
    wchar_t w[256];
    void *form = fl_form();
    if (!form) return;
    MultiByteToWideChar(CP_UTF8, 0, msg, -1, w, 256);
    __try { ((hint_t)(g_flBase + (HINT_VA - IMG_BASE)))(form, dstr_keep(w), 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("hint faulted"); }
}

// "plug:ch:<i>" / "plug:fx:<track>:<slot>" -> unit. Returns 0 if key is no plugin key.
static int plug_parse(const char *key, int *unit, int *a, int *b) {
    *b = 0;
    if (sscanf_s(key, "plug:ch:%d", a) == 1) { *unit = U_CH; return *a >= 0 && *a < MAX_CH; }
    if (sscanf_s(key, "plug:fx:%d:%d", a, b) == 2) { *unit = U_FX; return *a >= 0 && *a < MAX_MIX && *b >= 0 && *b < MAX_FX_SLOTS; }
    return 0;
}

static void chan_label(int a, char *out, int cap) {
    char name[48] = "";
    __try {
        const wchar_t *w = *(const wchar_t **)((char *)chan_obj(a) + 0x50);   // CH_NAME
        if (w && !WideCharToMultiByte(CP_UTF8, 0, w, -1, name, sizeof(name), NULL, NULL)) name[0] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { name[0] = 0; }
    if (name[0]) _snprintf_s(out, cap, _TRUNCATE, "Channel '%s'", name);
    else _snprintf_s(out, cap, _TRUNCATE, "Channel %d", a + 1);
}

static void unit_label(const char *key, char *out, int cap) {
    int unit, a, b, isPat, g;
    if (unit_key_parse(key, &isPat, &a, &g)) {
        char ch[64];
        if (isPat && g >= 0) { chan_label(g, ch, sizeof(ch)); _snprintf_s(out, cap, _TRUNCATE, "%s in pattern %d", ch, a); }
        else if (isPat) sprintf_s(out, cap, "Pattern %d", a);
        else if (g >= 0 && g <= 499) sprintf_s(out, cap, "Playlist track %d", 500 - g);
        else strcpy_s(out, cap, "The playlist");
    }
    else if (plug_parse(key, &unit, &a, &b)) {
        if (unit == U_FX) _snprintf_s(out, cap, _TRUNCATE, "Mixer insert %d slot %d", a, b + 1);
        else chan_label(a, out, cap);
    }
    else strcpy_s(out, cap, key);
}

static void hint_locked(ULock *l) {
    char who[64], what[96], msg[200];
    DWORD now = GetTickCount();
    if (now - l->lastHint < HINT_EVERY_MS) return;
    l->lastHint = now;
    peer_name(l->owner, who, sizeof(who));
    unit_label(l->key, what, sizeof(what));
    _snprintf_s(msg, sizeof(msg), _TRUNCATE, "%s is being edited by %s", what, who);
    fl_hint(msg);
}

// Before sending a local change of unit `key` (main thread). Returns 1 = go ahead (lock
// requested/held, op must carry the key), 0 = someone else holds it: caller reverts.
static int ulock_local(const char *key, int *already) {
    int r = 1;
    char m[96];
    EnterCriticalSection(&g_ulockLock);
    ULock *l = ulock_get(key, 1);
    *already = 0;
    if (!l) { LeaveCriticalSection(&g_ulockLock); return 1; }
    if (l->peer) { hint_locked(l); r = 0; }
    else {
        *already = l->granted;
        if (!l->mine) {
            l->mine = 1;
            sprintf_s(m, sizeof(m), "{\"t\":\"lock_acquire\",\"key\":\"%s\"}", key);
            ws_send(m);
        }
        l->lastLocal = GetTickCount();
    }
    LeaveCriticalSection(&g_ulockLock);
    return r;
}

// Poll thread: give back locks we stopped using.
static void ulock_release_idle(DWORD now) {
    int i;
    char m[96];
    EnterCriticalSection(&g_ulockLock);
    for (i = 0; i < g_nulocks; i++) {
        ULock *l = &g_ulocks[i];
        if (l->mine && now - l->lastLocal > (strncmp(l->key, "plug:", 5) == 0 ? (DWORD)g_plugLockMs : ULOCK_IDLE_MS)) {
            sprintf_s(m, sizeof(m), "{\"t\":\"lock_release\",\"key\":\"%s\"}", l->key);
            ws_send(m);
            l->mine = 0; l->granted = 0;
        }
    }
    LeaveCriticalSection(&g_ulockLock);
}

// Channels / tracks / patterns renumbered (from: new -> old) and our locks name them by number: a lock kept
// would hold someone else's channel at that number (who then couldn't send it, and reverted). Ours under
// these prefixes go back; the next edit takes them again under the new numbers.
static void ulock_release_moved(const char *pre1, const char *pre2, const int *from, int n) {
    int i;
    char m[96];
    for (i = 0; from && i < n && (from[i] < 0 || from[i] == i); i++) {}
    if (from && i == n) return;                             // nothing moved (added or removed at the end); NULL: not known
    EnterCriticalSection(&g_ulockLock);
    for (i = 0; i < g_nulocks; i++) {
        ULock *l = &g_ulocks[i];
        if (!l->mine || (strncmp(l->key, pre1, strlen(pre1)) != 0 && (!pre2 || strncmp(l->key, pre2, strlen(pre2)) != 0))) continue;
        sprintf_s(m, sizeof(m), "{\"t\":\"lock_release\",\"key\":\"%s\"}", l->key);
        ws_send(m);
        l->mine = 0; l->granted = 0;
    }
    LeaveCriticalSection(&g_ulockLock);
}

// State bookkeeping for a unit key (caller holds g_ulockLock). commit: the group we sent
// under this key becomes agreed.
static void unit_commit(const char *key) {
    int isPat, idx, g;
    if (!unit_key_parse(key, &isPat, &idx, &g)) return;
    if (isPat) { if (g_patPend[idx].known) ustate_merge(&g_patLast[idx], 1, g_patPend[idx].d, g_patPend[idx].n, g); }
    else if (g_clipPend.known && idx == g_clipLastArr) ustate_merge(&g_clipLast, 0, g_clipPend.d, g_clipPend.n, g);
}

// Main thread: restore the last agreed state of a group (our change lost the lock race or
// was made while someone else held the lock).
static void unit_revert(const char *key) {
    unsigned char *d = NULL;
    unsigned n = 0;
    int known = 0, idx = -1, isPat = 0, g = -1;
    int unit, a, b;
    if (plug_parse(key, &unit, &a, &b)) {
        // params were snapped back already (plug_undo); a state we sent was dropped by the server
        int sent = 0;
        EnterCriticalSection(&g_ulockLock);
        ULock *l = ulock_get(key, 0);
        if (l) { sent = l->stateSent; l->stateSent = 0; }
        LeaveCriticalSection(&g_ulockLock);
        if (sent) plug_revert_state(unit, a, b);
        logmsg("LOCK %s held by another user: local change reverted%s", key, sent ? " (state)" : "");
        return;
    }
    if (!unit_key_parse(key, &isPat, &idx, &g)) return;
    EnterCriticalSection(&g_ulockLock);
    {
        UState *s = isPat ? &g_patLast[idx] : (g_clipLastArr == idx ? &g_clipLast : NULL);
        if (s && s->known) { known = 1; d = recs_group(isPat, s->d, s->n, g, &n); }
    }
    LeaveCriticalSection(&g_ulockLock);
    if (!known) { logmsg("LOCK %s: no agreed state to revert to", key); return; }
    if (isPat) notes_apply(idx, d, n, g); else clips_apply(idx, d, n, g);
    free(d);
    logmsg("LOCK %s held by another user: local change reverted", key);
}

static void do_unitrevert(const QItem *it) {
    unit_revert(it->path);
    EnterCriticalSection(&g_ulockLock);
    ULock *l = ulock_get(it->path, 0);
    if (l) hint_locked(l);
    LeaveCriticalSection(&g_ulockLock);
}

// Inbound lock messages for unit keys (recv thread). Returns 1 if handled.
static int on_unit_lock(const char *m) {
    const char *k = strstr(m, "\"key\":\"");
    char key[32], owner[64] = "";
    int i = 0, granted, released = strstr(m, "\"t\":\"lock_released\"") != NULL;
    if (!k) return 0;
    k += 7;
    if (strncmp(k, "pat:", 4) != 0 && strncmp(k, "clips:", 6) != 0 && strncmp(k, "plug:", 5) != 0) return 0;
    while (k[i] && k[i] != '"' && i < (int)sizeof(key) - 1) { key[i] = k[i]; i++; }
    key[i] = 0;
    granted = strstr(m, "\"granted\":true") != NULL;
    {
        const char *o = strstr(m, "\"owner\":\"");
        if (o) { o += 9; for (i = 0; o[i] && o[i] != '"' && i < (int)sizeof(owner) - 1; i++) owner[i] = o[i]; owner[i] = 0; }
    }
    EnterCriticalSection(&g_ulockLock);
    ULock *l = ulock_get(key, 1);
    if (l) {
        if (released) l->peer = 0;
        else if (strcmp(owner, g_myId) == 0) {
            if (granted) { l->mine = 1; l->granted = 1; l->peer = 0; l->stateSent = 0; unit_commit(key); plug_undo(key, 0); }
        } else {
            int lost = !granted;                     // denials go only to the side that asked: our change lost
            l->peer = 1; l->mine = 0; l->granted = 0;
            strcpy_s(l->owner, sizeof(l->owner), owner);
            if (lost) plug_undo(key, 1);             // plugin params we changed meanwhile: snap back
            if (lost) {
                QItem q; memset(&q, 0, sizeof(q));
                q.type = T_UNITREVERT; q.path = _strdup(key);
                enqueue(&q);
                logmsg("LOCK %s denied (held by %s): reverting our change", key, owner);
            }
        }
    }
    LeaveCriticalSection(&g_ulockLock);
    return 1;
}


// ---- plugin lock ----
// One lock per plugin (channel generator or mixer effect slot): "plug:ch:<i>" / "plug:fx:<t>:<s>".
// Any local change of the plugin takes it (a param tweak or a state change); param.set and
// plug.state ops carry it, so the server drops them while a peer holds it. While a peer holds
// it, local param tweaks snap back and a local state change is undone by reloading the last
// agreed state. A denied request (race) snaps back the params changed while it was pending.
static UState g_plugLastCh[MAX_CH];
static UState g_plugLastFx[MAX_MIX][MAX_FX_SLOTS];
static volatile DWORD g_hintQueuedAt = 0;

static void plug_key(int unit, int a, int b, char *key, int cap) {
    if (unit == U_CH) sprintf_s(key, cap, "plug:ch:%d", a);
    else sprintf_s(key, cap, "plug:fx:%d:%d", a, b);
}
static UState *plug_last(int unit, int a, int b) {
    if (unit == U_CH) return (a >= 0 && a < MAX_CH) ? &g_plugLastCh[a] : NULL;
    return (a >= 0 && a < MAX_MIX && b >= 0 && b < MAX_FX_SLOTS) ? &g_plugLastFx[a][b] : NULL;
}
// Plugin param id -> its unit. 0 for channel/mixer basics (vol, pan, route...): those keep per-param locks.
static int plug_param_unit(uint32_t id, int *unit, int *a, int *b) {
    return !IS_MUTE_ID(id) && (id & 0xFFFF) >= REC_PLUG_FIRST && unit_of_id(id, unit, a, b);
}

// Last agreed state of a plugin (.fst bytes): set on baseline, send and remote apply. NULL clears.
static void plug_agreed_set(int unit, int a, int b, const unsigned char *d, size_t n) {
    EnterCriticalSection(&g_ulockLock);
    UState *s = plug_last(unit, a, b);
    if (s) {
        if (d && n) ustate_set(s, d, (unsigned)n, 1);
        else { free(s->d); memset(s, 0, sizeof(*s)); }
    }
    LeaveCriticalSection(&g_ulockLock);
}
// Channel indices moved / project replaced: stored states no longer belong to their index.
static void plug_agreed_reset(int channels, int fx) {
    int i, j;
    EnterCriticalSection(&g_ulockLock);
    if (channels) for (i = 0; i < MAX_CH; i++) { free(g_plugLastCh[i].d); memset(&g_plugLastCh[i], 0, sizeof(UState)); }
    if (fx)
        for (i = 0; i < MAX_MIX; i++)
            for (j = 0; j < MAX_FX_SLOTS; j++) { free(g_plugLastFx[i][j].d); memset(&g_plugLastFx[i][j], 0, sizeof(UState)); }
    LeaveCriticalSection(&g_ulockLock);
}

static int load_queued(int unit, int a, int b) {
    int i, r = 0;
    EnterCriticalSection(&g_qLock);
    for (i = g_qh; i != g_qt && !r; i = (i + 1) % QCAP)
        r = g_q[i].type == T_LOAD && g_q[i].unit == unit && g_q[i].a == a && g_q[i].b == b;
    LeaveCriticalSection(&g_qLock);
    return r;
}

// Main thread: reload the plugin's last agreed state (our state change lost to the lock holder).
static void plug_revert_state(int unit, int a, int b) {
    unsigned char *d = NULL;
    unsigned n = 0;
    if (load_queued(unit, a, b)) return;         // the holder's newer state is about to replace ours anyway
    EnterCriticalSection(&g_ulockLock);
    UState *s = plug_last(unit, a, b);
    if (s && s->known && s->n) { d = (unsigned char *)malloc(s->n); memcpy(d, s->d, s->n); n = s->n; }
    LeaveCriticalSection(&g_ulockLock);
    if (!d) { logmsg("PLUG %s %d/%d: no agreed state to revert to", unit == U_CH ? "ch" : "fx", a, b); return; }
    char *path = tmp_path("rv");
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") == 0 && f) {
        fwrite(d, 1, n, f); fclose(f);
        enqueue_unit(T_LOAD, unit, a, b, 0, path);
        logmsg("PLUG %s %d/%d: reverting to the agreed state (%u bytes)", unit == U_CH ? "ch" : "fx", a, b, n);
    } else free(path);
    free(d);
}

// Main thread, before sending a plug.state: 1 = go ahead under *key, 0 = a peer holds the plugin.
static int plug_lock_send(int unit, int a, int b, char *key, int cap) {
    int already;
    plug_key(unit, a, b, key, cap);
    if (!ulock_local(key, &already)) return 0;
    if (!already) {
        EnterCriticalSection(&g_ulockLock);
        ULock *l = ulock_get(key, 0);
        if (l) l->stateSent = 1;
        LeaveCriticalSection(&g_ulockLock);
    }
    return 1;
}

// Poll thread, a local plugin param change: 1 = send it under *key, 0 = a peer holds the
// plugin (the caller snaps the param back). Changes made before the grant remember their
// previous value in case the request is denied.
static int plug_lock_param(int unit, int a, int b, Slot *s, uint32_t prev, char *key, int cap) {
    int r = 1;
    char m[96];
    plug_key(unit, a, b, key, cap);
    EnterCriticalSection(&g_ulockLock);
    ULock *l = ulock_get(key, 1);
    if (l && l->peer) r = 0;
    else if (l) {
        if (!l->mine) {
            l->mine = 1;
            sprintf_s(m, sizeof(m), "{\"t\":\"lock_acquire\",\"key\":\"%s\"}", key);
            ws_send(m);
        }
        if (!l->granted && !s->hasUndo) { s->undo = prev; s->hasUndo = 1; }
        l->lastLocal = GetTickCount();
    }
    LeaveCriticalSection(&g_ulockLock);
    return r;
}

// Lock answer for a plugin key: granted -> forget the undo values; denied -> apply them.
static void plug_undo(const char *key, int apply) {
    int unit, a, b, k, n = 0;
    if (!plug_parse(key, &unit, &a, &b)) return;
    for (k = 0; k < g_nslot; k++) {
        Slot *s = &g_slots[k];
        int u2, a2, b2;
        if (!s->hasUndo || !plug_param_unit(s->id, &u2, &a2, &b2) || u2 != unit || a2 != a || b2 != b) continue;
        if (apply) { enqueue_apply(s->id, s->undo, 0); n++; }
        s->hasUndo = 0;
    }
    if (n) logmsg("LOCK %s denied: %d params snapped back", key, n);
}

// Poll thread: show the hint for a held plugin (throttled; FL calls happen on the main thread).
static void enqueue_hint(const char *key) {
    DWORD now = GetTickCount();
    if (now - g_hintQueuedAt < HINT_EVERY_MS) return;
    g_hintQueuedAt = now;
    QItem q; memset(&q, 0, sizeof(q));
    q.type = T_HINT; q.path = _strdup(key);
    enqueue(&q);
}

static void do_hint(const QItem *it) {
    EnterCriticalSection(&g_ulockLock);
    ULock *l = ulock_get(it->path, 0);
    if (l && l->peer) hint_locked(l);
    LeaveCriticalSection(&g_ulockLock);
}

// ---- where everyone is working ----
// Each client tells the others which FL window it last clicked or typed in (its caption, e.g.
// "Piano roll - Kick · Pattern 3", "Mixer - Insert 2", a plugin's name). Shown after the status
// text, and in full in FL's hint bar when the mouse is over the flim status.
#define FOCUS_MAX     32
#define FOCUS_POLL_MS 300
typedef struct { char id[64]; char where[200]; } PeerFocus;
static PeerFocus g_focus[FOCUS_MAX];
static int g_nfocus = 0;                   // guarded by g_netTextLock
static volatile HWND g_focusHwnd = NULL;   // last window that got a click / key (main thread sets it)
static volatile LONG g_focusResend = 0;    // someone joined: tell them where we are

// Caption of the FL form h is in, without FL's icon glyphs (private-use characters).
static int focus_label(char *out, int cap) {
    HWND h = g_focusHwnd;
    wchar_t t[200], c[200];
    int i, o = 0;
    while (h && h != g_mainForm && !is_fl_form(h)) h = GetParent(h);
    if (!h || h == g_mainForm) return 0;                // toolbar, menus...: keep the last place
    InternalGetWindowText(h, t, 200);
    for (i = 0; t[i] && o < 190; i++) if (t[i] < 0xE000 || t[i] > 0xF8FF) c[o++] = t[i];
    while (o && (c[o - 1] == L' ' || c[o - 1] == L'-')) o--;
    c[o] = 0;
    if (!o) return 0;
    if (wnd_class_is(h, "TEventEditForm") && wcsncmp(c, L"Piano roll", 10) == 0) {
        int pat = 0;
        wchar_t nm[64] = L"";
        __try {
            pat = **(int **)(g_flBase + (FLVA(CURPAT) - IMG_BASE));       // current pattern
            if (pat >= 1 && pat < MAX_PAT) {
                const wchar_t *n = *(const wchar_t **)(pat_rec(pat) + PAT_NAME);
                int k = ustr_len(n);
                if (k > 60) k = 60;
                if (k) { memcpy(nm, n, (size_t)k * 2); nm[k] = 0; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { pat = 0; }
        if (pat >= 1 && pat < MAX_PAT) {
            if (nm[0]) swprintf_s(c + o, 200 - o, L" \x00B7 %s", nm);
            else swprintf_s(c + o, 200 - o, L" \x00B7 Pattern %d", pat);
        }
    }
    return WideCharToMultiByte(CP_UTF8, 0, c, -1, out, cap, NULL, NULL) > 1;
}

// Poll thread: send our place when it changed (or someone new is here).
static void focus_poll(DWORD now) {
    static DWORD last = 0;
    static char sent[200] = "";
    char cur[200], esc[420], m[512];
    if (now - last < FOCUS_POLL_MS || !g_ws) return;
    last = now;
    if (!focus_label(cur, sizeof(cur))) {
        if (!g_focusResend || !sent[0]) return;
        strcpy_s(cur, sizeof(cur), sent);
    }
    if (strcmp(cur, sent) == 0 && !InterlockedExchange(&g_focusResend, 0)) return;
    strcpy_s(sent, sizeof(sent), cur);
    json_esc(cur, esc, sizeof(esc));
    sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"focus\",\"w\":\"%s\"}}", esc);
    ws_send(m);
}

// Inbound focus op (recv thread): {"t":"op","seq":..,"from":"<id>","op":{"kind":"focus","w":"..."}}
static void on_focus(const char *m) {
    const char *f = strstr(m, "\"from\":\""), *w = strstr(m, "\"w\":\"");
    char id[64], where[200];
    int i, o = 0;
    if (!f || !w) return;
    for (f += 8, i = 0; f[i] && f[i] != '"' && i < (int)sizeof(id) - 1; i++) id[i] = f[i];
    id[i] = 0;
    for (w += 5; *w && *w != '"' && o < (int)sizeof(where) - 1; w++) {
        if (*w == '\\' && w[1]) {
            w++;
            if (*w == 'u') { w += 4; continue; }             // json_esc only escapes control characters this way
        }
        where[o++] = *w;
    }
    where[o] = 0;
    EnterCriticalSection(&g_netTextLock);
    for (i = 0; i < g_nfocus && strcmp(g_focus[i].id, id) != 0; i++) {}
    if (i == g_nfocus && g_nfocus < FOCUS_MAX) strcpy_s(g_focus[g_nfocus++].id, sizeof(g_focus[0].id), id);
    if (i < g_nfocus) strcpy_s(g_focus[i].where, sizeof(g_focus[0].where), where);
    LeaveCriticalSection(&g_netTextLock);
    if (g_peersJson[0]) net_show_connected(g_peersJson);
}

// "Alex: Piano roll - Kick<sep>Bo: Mixer" for the people in the session now (any thread).
static void focus_summary(char *out, int cap, const char *sep) {
    int i, o = 0;
    out[0] = 0;
    EnterCriticalSection(&g_netTextLock);
    for (i = 0; i < g_nfocus; i++) {
        char pat[96], who[64];
        const char *p, *q;
        sprintf_s(pat, sizeof(pat), "\"id\":\"%s\",\"name\":\"", g_focus[i].id);
        if (!g_focus[i].where[0] || !(p = strstr(g_peersJson, pat))) continue;   // left the session
        p += strlen(pat);
        q = strchr(p, '"');
        if (!q || q - p >= (int)sizeof(who)) continue;
        memcpy(who, p, q - p); who[q - p] = 0;
        o += _snprintf_s(out + o, cap - o, _TRUNCATE, "%s%s: %s", o ? sep : "", who, g_focus[i].where);
        if (o < 0 || o >= cap - 1) break;
    }
    LeaveCriticalSection(&g_netTextLock);
}

// ---- where everyone is pointing ----
// While the mouse is over the piano roll's note grid or the playlist, its position goes out in
// song terms (tick + key, or tick + track), so it lands on the same note / bar for everyone
// whatever their zoom, scroll or window size. Each peer is shown as a small arrow with their name
// on top of our editor when it shows the same pattern / arrangement: click-through windows owned
// by FL's main form, hidden when something else covers that point of the editor.
#define PRFORM_VA     FLVA(PRFORM) // global -> var -> piano roll form (NULL until first opened)
#define CURPAT_VA     FLVA(CURPAT) // global -> int: current pattern (what the piano roll edits)
#define ED_HWND       0x45c        // editor form: window handle
#define ED_TICKSPERPX 0xb04        //   double: ticks per pixel (horizontal zoom)
#define ED_HSCROLL    0xb14        //   int: horizontal scroll, pixels
#define PR_GRID       0xc68        // piano roll form: TEventPanel_PR (note grid)
#define PL_GRID       0xd2c        // playlist form: TEventPanel_AU (clip grid)
#define CTL_BOUNDS    0x90         // control: Left, Top, Width, Height in its form's client area
#define GRID_VSCROLL  0x3fc        // grid panel: vertical scroll, pixels
#define PR_TOPKEY     0x418        // piano roll grid: y = (topkey - key) * keyh - vscroll
#define PR_KEYH       0x520
#define TRK_TOP       0xb4         // playlist track (arrangement + t * 0x114): top y and height, pixels
#define TRK_H         0xb8
#define CUR_SEND_MS   50
#define CUR_LEAVE_MS  350          // the mouse off the grid this long before the others hear it left
#define CUR_HIDE_MS   350          // an arrow stays put this long before a passing reason hides it
#define CUR_STALE_MS  30000        // nothing from them for this long: hide (they walked away)
#define CUR_KEY       RGB(255, 0, 254)   // transparent colour of the arrow windows

typedef struct { HWND h; int L, T, W, H, hs, vs, topkey, keyh; double tpp; char *arr; } EdGeom;
typedef struct { char id[64]; char w[4]; int p, x, y, why; DWORD at; HWND ov; char name[64]; int sx, sy; DWORD hideSince; } PeerCursor;
static PeerCursor g_cur[FOCUS_MAX];
static int g_curNoCover = 0;               // FLIM_CURSOR_NOCOVER=1 (tests): show arrows even where FL is covered
static int g_ncur = 0;                     // guarded by g_netTextLock

// Where an editor's grid is and how it maps (any thread; FL's own fields, SEH-guarded).
// pl: 0 = piano roll, 1 = playlist. 0 = not there / not visible.
static int ed_geom(int pl, EdGeom *g) {
    __try {
        char *form = **(char ***)(g_flBase + ((pl ? PLFORM_VA : PRFORM_VA) - IMG_BASE)), *grid;
        if (!form) return 0;
        grid = *(char **)(form + (pl ? PL_GRID : PR_GRID));
        if (!grid) return 0;
        g->h = (HWND)(uintptr_t)*(uint32_t *)(form + ED_HWND);
        g->L = *(int *)(grid + CTL_BOUNDS); g->T = *(int *)(grid + CTL_BOUNDS + 4);
        g->W = *(int *)(grid + CTL_BOUNDS + 8); g->H = *(int *)(grid + CTL_BOUNDS + 12);
        g->tpp = *(double *)(form + ED_TICKSPERPX);
        g->hs = *(int *)(form + ED_HSCROLL);
        g->vs = *(int *)(grid + GRID_VSCROLL);
        g->topkey = *(int *)(grid + PR_TOPKEY);
        g->keyh = *(int *)(grid + PR_KEYH);
        g->arr = pl ? arr_obj(cur_arr()) : NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!g->h || !IsWindowVisible(g->h) || g->W <= 0 || g->H <= 0 || !(g->tpp > 0) || (!pl && g->keyh <= 0) || (pl && !g->arr)) return 0;
    return 1;
}
static int cur_pat(void) {
    __try { return **(int **)(g_flBase + (CURPAT_VA - IMG_BASE)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// Playlist track t: top y and height in the grid (before scrolling).
static int trk_rows(char *arr, int t, int *top, int *h) {
    __try {
        *top = *(int *)(arr + (size_t)t * PL_TRK_STRIDE + TRK_TOP);
        *h = *(int *)(arr + (size_t)t * PL_TRK_STRIDE + TRK_H);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

// A window at a grid point that doesn't really cover it: another arrow, or one of FL's small pop-ups (its hints,
// tooltips and drag images open right under the mouse). Taken as covering, they flipped the sender between "over
// the grid" and "not", and the receiver's arrow between shown and hidden: it flickered.
#define CUR_POPUP_W   520
#define CUR_POPUP_H   240
static int cur_passthrough(HWND hw) {
    HWND root = hw ? GetAncestor(hw, GA_ROOT) : NULL;
    DWORD pid = 0;
    RECT r;
    if (!root) return 0;
    if (wnd_class_is(root, "FlimCursor") || wnd_class_is(root, "FlimSel")) return 1;
    GetWindowThreadProcessId(root, &pid);
    if (pid != GetCurrentProcessId() || root == g_mainForm || !(GetWindowLongW(root, GWL_STYLE) & WS_POPUP)) return 0;
    return GetWindowRect(root, &r) && r.right - r.left < CUR_POPUP_W && r.bottom - r.top < CUR_POPUP_H;
}

// Screen point -> song position on editor pl: x = tick, y = key * 100 (piano roll) or
// track * 100 + percent into the track (playlist). 0 = not over that grid.
static int cur_from_screen(int pl, POINT s, int *ctx, int *x, int *y) {
    EdGeom g;
    HWND hw = WindowFromPoint(s);
    int t, top, h, yy;
    if (!ed_geom(pl, &g) || (hw != g.h && !IsChild(g.h, hw) && !cur_passthrough(hw))) return 0;
    ScreenToClient(g.h, &s);
    s.x -= g.L; s.y -= g.T;
    if (s.x < 0 || s.y < 0 || s.x >= g.W || s.y >= g.H) return 0;
    *x = (int)((s.x + g.hs) * g.tpp);
    if (!pl) {
        *y = (int)(((double)g.topkey - (double)(s.y + g.vs) / g.keyh) * 100.0);
        *ctx = cur_pat();
        return *ctx >= 1;
    }
    yy = s.y + g.vs;
    for (t = 1; t <= PL_TRK_MAX && trk_rows(g.arr, t, &top, &h); t++)
        if (h > 0 && yy >= top && yy < top + h) { *y = t * 100 + (yy - top) * 100 / h; *ctx = cur_arr(); return 1; }
    return 0;
}

// Song position -> screen point on our editor pl. 1 = there, 0 = editor closed / scrolled away,
// -1 = covered by another window.
static int cur_to_screen(int pl, int x, int y, POINT *s) {
    EdGeom g;
    HWND hw;
    if (!ed_geom(pl, &g)) return 0;
    s->x = g.L + (int)(x / g.tpp) - g.hs;
    if (!pl) s->y = g.T + (int)(((double)g.topkey - y / 100.0) * g.keyh) - g.vs;
    else {
        int t = y / 100, top, h;
        if (t < 1 || t > PL_TRK_MAX || !trk_rows(g.arr, t, &top, &h)) return 0;
        s->y = g.T + top - g.vs + h * (y % 100) / 100;
    }
    if (s->x < g.L || s->y < g.T || s->x >= g.L + g.W || s->y >= g.T + g.H) return 0;
    ClientToScreen(g.h, s);
    hw = WindowFromPoint(*s);          // a plugin or another app on top? (our arrows let the point through)
    return g_curNoCover || hw == g.h || IsChild(g.h, hw) || cur_passthrough(hw) ? 1 : -1;
}

// Poll thread: tell the others where our mouse is (only while it is over a grid and moving).
static void cursor_poll(DWORD now) {
    static DWORD last = 0;
    static char sent[96] = "";
    char cur[96], m[192];
    static DWORD onAt = 0;
    POINT s;
    int pl, ctx = 0, x = 0, y = 0, on = 0;
    if (now - last < CUR_SEND_MS || !g_ws) return;
    last = now;
    if (GetCursorPos(&s))
        for (pl = 0; pl < 2 && !on; pl++)
            if (cur_from_screen(pl, s, &ctx, &x, &y)) {
                on = 1;
                sprintf_s(cur, sizeof(cur), "\"w\":\"%s\",\"p\":%d,\"x\":%d,\"y\":%d", pl ? "pl" : "pr", ctx, x, y);
            }
    if (on) onAt = now;
    else if (onAt && now - onAt < CUR_LEAVE_MS) return;      // (passing over something: not "left" yet)
    if (!on) strcpy_s(cur, sizeof(cur), "\"w\":\"\"");
    if (strcmp(cur, sent) == 0) return;
    strcpy_s(sent, sizeof(sent), cur);
    sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"cur\",%s}}", cur);
    ws_send(m);
}

// Inbound cur op (recv thread): {"t":"op",..,"from":"<id>","op":{"kind":"cur","w":"pr","p":3,"x":..,"y":..}}
static void on_cursor(const char *m) {
    const char *f = strstr(m, "\"from\":\""), *w = strstr(m, "\"w\":\"");
    char id[64];
    int i, p = 0, x = 0, y = 0;
    if (!f || !w) return;
    for (f += 8, i = 0; f[i] && f[i] != '"' && i < (int)sizeof(id) - 1; i++) id[i] = f[i];
    id[i] = 0;
    int_after(m, NULL, "\"p\":", &p);
    int_after(m, NULL, "\"x\":", &x);
    int_after(m, NULL, "\"y\":", &y);
    EnterCriticalSection(&g_netTextLock);
    for (i = 0; i < g_ncur && strcmp(g_cur[i].id, id) != 0; i++) {}
    if (i == g_ncur && g_ncur < FOCUS_MAX) { memset(&g_cur[i], 0, sizeof(g_cur[i])); g_cur[i].why = -1; strcpy_s(g_cur[g_ncur++].id, sizeof(g_cur[0].id), id); }
    if (i < g_ncur) {
        PeerCursor *c = &g_cur[i];
        int on = w[5] == 'p' && (w[6] == 'r' || w[6] == 'l');
        c->w[0] = on ? 'p' : 0; c->w[1] = on ? w[6] : 0; c->w[2] = 0;
        c->p = p; c->x = x; c->y = y; c->at = GetTickCount();
    }
    LeaveCriticalSection(&g_netTextLock);
}

static COLORREF cursor_color(const char *id) {
    static const COLORREF pal[] = { RGB(0xff, 0x8a, 0x3d), RGB(0x3d, 0xc8, 0xff), RGB(0xb4, 0x6c, 0xff), RGB(0x5c, 0xe0, 0x7a),
                                    RGB(0xff, 0x5c, 0x8a), RGB(0xff, 0xd2, 0x3d), RGB(0x3d, 0xff, 0xd8), RGB(0xff, 0x6c, 0xff) };
    uint64_t h = fnv_add(1469598103934665603ull, id, strlen(id));
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}
static HFONT cursor_font(int sc) {
    return CreateFontW(-13 * sc / 100, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Segoe UI");
}

// Arrow + name label; everything in CUR_KEY is see-through. GWLP_USERDATA = colour, "sc" = scale %.
static LRESULT CALLBACK cursor_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, lb;
        SIZE ts;
        wchar_t name[64];
        int sc = (int)(intptr_t)GetPropW(h, L"sc"), n, k;
        HBRUSH bk = CreateSolidBrush(CUR_KEY), br = CreateSolidBrush((COLORREF)GetWindowLongPtrW(h, GWLP_USERDATA));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x20, 0x20, 0x20));
        HFONT f = cursor_font(sc);
        POINT arrow[7] = { {0, 0}, {0, 16}, {4, 12}, {7, 19}, {10, 18}, {7, 11}, {12, 11} };
        HGDIOBJ of, op, ob;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bk);
        for (k = 0; k < 7; k++) { arrow[k].x = arrow[k].x * sc / 100; arrow[k].y = arrow[k].y * sc / 100; }
        op = SelectObject(dc, pen); ob = SelectObject(dc, br); of = SelectObject(dc, f);
        Polygon(dc, arrow, 7);
        n = GetWindowTextW(h, name, 64);
        GetTextExtentPoint32W(dc, name, n, &ts);
        lb.left = 12 * sc / 100; lb.top = 16 * sc / 100;
        lb.right = lb.left + ts.cx + 8 * sc / 100; lb.bottom = lb.top + ts.cy + 2 * sc / 100;
        RoundRect(dc, lb.left, lb.top, lb.right, lb.bottom, 6 * sc / 100, 6 * sc / 100);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x10, 0x10, 0x10));
        TextOutW(dc, lb.left + 4 * sc / 100, lb.top + 1 * sc / 100, name, n);
        SelectObject(dc, of); SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(f); DeleteObject(pen); DeleteObject(br); DeleteObject(bk);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) RemovePropW(h, L"sc");
    return DefWindowProcW(h, m, wp, lp);
}

static HWND cursor_window(const char *id, const char *name) {
    static int reg = 0;
    wchar_t wn[64];
    HDC dc = GetDC(g_mainForm);
    int sc = MulDiv(GetDeviceCaps(dc, LOGPIXELSY), 100, 96);
    HFONT f = cursor_font(sc);
    HGDIOBJ of = SelectObject(dc, f);
    SIZE ts;
    HWND h;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = cursor_proc; wc.hInstance = g_hinst; wc.lpszClassName = L"FlimCursor";
        RegisterClassW(&wc);
        reg = 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wn, 64);
    GetTextExtentPoint32W(dc, wn, (int)wcslen(wn), &ts);
    SelectObject(dc, of); DeleteObject(f);
    ReleaseDC(g_mainForm, dc);
    h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"FlimCursor", wn, WS_POPUP,
                        0, 0, (12 + 8 + 2) * sc / 100 + ts.cx, (16 + 4) * sc / 100 + ts.cy, g_mainForm, NULL, g_hinst, NULL);
    if (!h) return NULL;
    SetLayeredWindowAttributes(h, CUR_KEY, 0, LWA_COLORKEY);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cursor_color(id));
    SetPropW(h, L"sc", (HANDLE)(intptr_t)sc);
    return h;
}

// Main thread (net bar timer): place, show or hide every peer's arrow. A passing reason to hide it (the mouse
// briefly off the grid, a pop-up over it, a scroll) keeps it where it was for CUR_HIDE_MS first; and the window
// is only moved when its spot changes (moving it every tick re-raised it and could repaint it each time).
static void cursor_update(void) {
    DWORD now = GetTickCount();
    int i, live = g_netState == NET_ON && g_mainForm && !IsIconic(g_mainForm);
    EnterCriticalSection(&g_netTextLock);
    for (i = 0; i < g_ncur; i++) {
        PeerCursor *c = &g_cur[i];
        POINT s;
        char pat[96], name[64];
        const char *p = NULL, *q;
        int pl = c->w[1] == 'l', why, shown = c->ov && IsWindowVisible(c->ov);
        static const char *const WHY[] = { "shown", "not connected / FL minimized", "not over a grid", "idle",
                                           "left", "other pattern / arrangement", "not in view", "covered", "cursors turned off" };
        sprintf_s(pat, sizeof(pat), "\"id\":\"%s\",\"name\":\"", c->id);
        why = !g_showCursors ? 8 : !live ? 1 : !c->w[0] ? 2 : now - c->at >= CUR_STALE_MS ? 3 : !(p = strstr(g_peersJson, pat)) ? 4 :
              c->p != (pl ? cur_arr() : cur_pat()) ? 5 : 0;
        if (!why) { int r = cur_to_screen(pl, c->x, c->y, &s); why = r > 0 ? 0 : r == 0 ? 6 : 7; }
        if (why != c->why) { c->why = why; logmsg("CURSOR %s: %s", c->id, WHY[why]); }
        if (why) {
            int passing = why == 2 || why == 6 || why == 7;
            if (!c->hideSince) c->hideSince = now | 1;
            if (shown && passing && (LONG)(now - c->hideSince) < CUR_HIDE_MS) continue;   // (stays where it was, for now;
            if (shown) ShowWindow(c->ov, SW_HIDE);                                          //  signed: hideSince is now | 1)
            continue;
        }
        c->hideSince = 0;
        p += strlen(pat);
        q = strchr(p, '"');
        if (!q || q - p >= (int)sizeof(name)) continue;
        memcpy(name, p, q - p); name[q - p] = 0;
        if (c->ov && strcmp(name, c->name) != 0) { DestroyWindow(c->ov); c->ov = NULL; shown = 0; }   // renamed: new label size
        if (!c->ov) {
            strcpy_s(c->name, sizeof(c->name), name);
            if (!(c->ov = cursor_window(c->id, name))) continue;
        }
        if (shown && s.x == c->sx && s.y == c->sy) continue;
        c->sx = s.x; c->sy = s.y;
        SetWindowPos(c->ov, HWND_TOP, s.x, s.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | (shown ? SWP_NOZORDER : SWP_SHOWWINDOW));
    }
    LeaveCriticalSection(&g_netTextLock);
}

// ---- what everyone has selected ----
// The notes selected in someone's current pattern and the clips selected in their current arrangement go out
// (only when they change, at most every SEL_SEND_MS) and show as outlines in their colour on our piano roll /
// playlist when it shows the same pattern / arrangement: one click-through window per person and editor over
// the grid, with "Show cursors". An outline whose middle is covered by another window isn't drawn.
// ponytail: the servers treat "sel" as an edit (it keeps that person's locks fresh; the Node one logs it).
#define SEL_SEND_MS 200
#define SEL_MAX     256
typedef struct { int pos, len, row; } SelBox;          // ticks; row: key (piano roll) / playlist track 1..500
typedef struct { COLORREF col; int n; RECT r[SEL_MAX]; } SelDraw;   // an overlay's outlines, in its client area
typedef struct { char id[64]; int pat, np, arr, nc; SelBox n[SEL_MAX], c[SEL_MAX]; HWND ov[2]; uint64_t drawn[2]; SelDraw d[2]; } PeerSel;
static PeerSel g_psel[FOCUS_MAX];
static int g_npsel = 0;                     // guarded by g_netTextLock

static int sel_peek(int pl, int ctx, SelBox *out) {   // our selection (poll thread; SEH-guarded reads)
    int k = 0;
    unsigned i, n;
    __try {
        char *a = pl ? arr_clips(ctx) : pat_notes_arr(ctx);
        unsigned char *d = a ? *(unsigned char **)(a + (pl ? CLIPS_DATA : NOTES_DATA)) : NULL;
        n = a ? *(unsigned *)(a + (pl ? CLIPS_COUNT : NOTES_COUNT)) : 0;
        if (n > 1000000) n = 0;
        for (i = 0; i < n && k < SEL_MAX; i++) {
            const unsigned char *r = d + (size_t)i * (pl ? CLIP_SIZE : NOTE_SIZE);
            if (!(r[pl ? CLIP_SEL_OFS : NOTE_SEL_OFS] & (pl ? CLIP_SEL_BIT : NOTE_SEL_BIT))) continue;
            out[k].pos = *(const int *)r; out[k].len = *(const int *)(r + 8);
            out[k].row = pl ? 500 - *(const uint16_t *)(r + CLIP_TRK_OFS) : *(const uint16_t *)(r + 12);   // track 1..500 / key
            k++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return k;
}
static int sel_json(char *o, int cap, const SelBox *b, int n) {
    int i, len = 0;
    for (i = 0; i < n && len < cap - 40; i++) len += sprintf_s(o + len, cap - len, "%s%d,%d,%d", i ? "," : "", b[i].pos, b[i].len, b[i].row);
    return len;
}
// Poll thread: tell the others what we have selected, when it changes.
static void sel_poll(DWORD now) {
    static DWORD last = 0;
    static uint64_t sent = 0;
    static SelBox nb[SEL_MAX], cb[SEL_MAX];
    int pat, arr, nn, nc, o;
    uint64_t h;
    char *m;
    if (now - last < SEL_SEND_MS || !g_ws) return;
    last = now;
    pat = cur_pat(); arr = cur_arr();
    nn = pat >= 1 ? sel_peek(0, pat, nb) : 0;
    nc = arr >= 0 ? sel_peek(1, arr, cb) : 0;
    h = fnv_add(fnv_add(fnv_add(1469598103934665603ull, nb, sizeof(SelBox) * (size_t)nn), cb, sizeof(SelBox) * (size_t)nc), &pat, sizeof(pat));
    h = fnv_add(fnv_add(h, &arr, sizeof(arr)), &nn, sizeof(nn));
    if (h == sent) return;
    sent = h;
    m = (char *)malloc(128 + (size_t)SEL_MAX * 2 * 36);
    o = sprintf_s(m, 128, "{\"t\":\"op\",\"op\":{\"kind\":\"sel\",\"p\":%d,\"a\":%d,\"n\":[", pat, arr);
    o += sel_json(m + o, SEL_MAX * 36, nb, nn);
    o += sprintf_s(m + o, 16, "],\"c\":[");
    o += sel_json(m + o, SEL_MAX * 36, cb, nc);
    sprintf_s(m + o, 8, "]}}");
    ws_send(m);
    free(m);
}
static int sel_parse(const char *m, const char *key, SelBox *b) {
    const char *p = strstr(m, key);
    int k = 0;
    char *e;
    if (!p) return 0;
    for (p += strlen(key); *p && *p != ']' && k < SEL_MAX; k++) {
        b[k].pos = (int)strtol(p, &e, 10); if (*e != ',') break;
        b[k].len = (int)strtol(e + 1, &e, 10); if (*e != ',') break;
        b[k].row = (int)strtol(e + 1, &e, 10);
        p = *e == ',' ? e + 1 : e;
    }
    return k;
}
// Inbound sel op (recv thread).
static void on_sel(const char *m) {
    const char *f = strstr(m, "\"from\":\"");
    char id[64];
    int i, p = 0, a = -1;
    if (!f) return;
    for (f += 8, i = 0; f[i] && f[i] != '"' && i < (int)sizeof(id) - 1; i++) id[i] = f[i];
    id[i] = 0;
    int_after(m, NULL, "\"p\":", &p);
    int_after(m, NULL, "\"a\":", &a);
    EnterCriticalSection(&g_netTextLock);
    for (i = 0; i < g_npsel && strcmp(g_psel[i].id, id) != 0; i++) {}
    if (i == g_npsel && g_npsel < FOCUS_MAX) { memset(&g_psel[i], 0, sizeof(g_psel[i])); strcpy_s(g_psel[g_npsel++].id, sizeof(g_psel[0].id), id); }
    if (i < g_npsel) {
        PeerSel *s = &g_psel[i];
        int np = s->np, nc = s->nc;
        s->pat = p; s->arr = a;
        s->np = sel_parse(m, "\"n\":[", s->n);
        s->nc = sel_parse(m, "\"c\":[", s->c);
        if (s->np != np || s->nc != nc) logmsg("SEL %s: %d notes selected in pattern %d, %d clips in arrangement %d", id, s->np, p, s->nc, a);
    }
    LeaveCriticalSection(&g_netTextLock);
}

static LRESULT CALLBACK sel_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_PAINT || m == WM_PRINTCLIENT) {          // (PRINTCLIENT: PrintWindow, for checking it)
        PAINTSTRUCT ps;
        HDC dc = m == WM_PAINT ? BeginPaint(h, &ps) : (HDC)wp;
        const SelDraw *d = (const SelDraw *)GetPropW(h, L"sel");
        RECT rc;
        HBRUSH bk = CreateSolidBrush(CUR_KEY);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bk);
        if (d) {
            HBRUSH br = CreateSolidBrush(d->col);
            int i;
            for (i = 0; i < d->n; i++) {
                RECT r = d->r[i], r2 = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
                FrameRect(dc, &r, br);
                if (r2.right > r2.left && r2.bottom > r2.top) FrameRect(dc, &r2, br);   // 2 px
            }
            DeleteObject(br);
        }
        DeleteObject(bk);
        if (m == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) RemovePropW(h, L"sel");
    return DefWindowProcW(h, m, wp, lp);
}
static HWND sel_window(SelDraw *d) {
    static int reg = 0;
    HWND h;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = sel_proc; wc.hInstance = g_hinst; wc.lpszClassName = L"FlimSel";
        RegisterClassW(&wc);
        reg = 1;
    }
    h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"FlimSel", L"", WS_POPUP,
                        0, 0, 1, 1, g_mainForm, NULL, g_hinst, NULL);
    if (!h) return NULL;
    SetLayeredWindowAttributes(h, CUR_KEY, 0, LWA_COLORKEY);
    SetPropW(h, L"sel", d);
    return h;
}
// A selection box -> its rectangle in the grid's client area (before clipping). 0 = not in this arrangement.
static int sel_rect(int pl, const EdGeom *g, const SelBox *b, RECT *r) {
    r->left = (int)(b->pos / g->tpp) - g->hs;
    r->right = (int)((b->pos + (b->len > 0 ? b->len : 1)) / g->tpp) - g->hs + 1;
    if (!pl) { r->top = (g->topkey - b->row) * g->keyh - g->vs; r->bottom = r->top + g->keyh + 1; }
    else {
        int top, h;
        if (b->row < 1 || b->row > PL_TRK_MAX || !trk_rows(g->arr, b->row, &top, &h) || h <= 0) return 0;
        r->top = top - g->vs; r->bottom = r->top + h + 1;
    }
    return 1;
}
// Main thread (with cursor_update): place, redraw or hide every person's selection outlines.
static void sel_update(void) {
    int i, pl, live = g_netState == NET_ON && g_mainForm && !IsIconic(g_mainForm) && g_showCursors;
    EnterCriticalSection(&g_netTextLock);
    for (i = 0; i < g_npsel; i++) {
        PeerSel *s = &g_psel[i];
        char pat[96];
        sprintf_s(pat, sizeof(pat), "\"id\":\"%s\",", s->id);
        for (pl = 0; pl < 2; pl++) {
            EdGeom g;
            SelDraw *d = &s->d[pl];
            const SelBox *b = pl ? s->c : s->n;
            int nb = pl ? s->nc : s->np, k;
            POINT o = { 0, 0 };
            uint64_t h;
            d->n = 0;
            if (live && nb && strstr(g_peersJson, pat) && (pl ? s->arr == cur_arr() : s->pat == cur_pat()) && ed_geom(pl, &g)) {
                o.x = g.L; o.y = g.T;
                ClientToScreen(g.h, &o);
                for (k = 0; k < nb && d->n < SEL_MAX; k++) {
                    RECT r;
                    POINT c;
                    HWND hw;
                    if (!sel_rect(pl, &g, &b[k], &r) || r.right <= 0 || r.bottom <= 0 || r.left >= g.W || r.top >= g.H) continue;
                    if (r.left < 0) r.left = 0;
                    if (r.top < 0) r.top = 0;
                    if (r.right > g.W) r.right = g.W;
                    if (r.bottom > g.H) r.bottom = g.H;
                    c.x = o.x + (r.left + r.right) / 2; c.y = o.y + (r.top + r.bottom) / 2;
                    hw = WindowFromPoint(c);
                    if (!g_curNoCover && hw != g.h && !IsChild(g.h, hw) && !cur_passthrough(hw)) continue;   // covered there
                    d->r[d->n++] = r;
                }
            }
            if (!d->n) {
                if (s->ov[pl] && IsWindowVisible(s->ov[pl])) ShowWindow(s->ov[pl], SW_HIDE);
                s->drawn[pl] = 0;
                continue;
            }
            d->col = cursor_color(s->id);
            h = fnv_add(fnv_add(fnv_add(1469598103934665603ull, d->r, sizeof(RECT) * (size_t)d->n), &o, sizeof(o)), &g.W, sizeof(int) * 2);
            if (h == s->drawn[pl] && s->ov[pl] && IsWindowVisible(s->ov[pl])) continue;
            s->drawn[pl] = h;
            if (!s->ov[pl] && !(s->ov[pl] = sel_window(d))) continue;
            SetWindowPos(s->ov[pl], HWND_TOP, o.x, o.y, g.W, g.H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(s->ov[pl], NULL, FALSE);
        }
    }
    LeaveCriticalSection(&g_netTextLock);
}

// Hooks for the notes / clips code ------------------------------------------------------
// Local change of group g about to be sent (main thread). d = the unit's whole current records.
// Returns 0 if someone else holds the group: the caller reverts instead of sending.
static int unit_before_send(const char *key, int isPat, int idx, const unsigned char *d, unsigned n, int g) {
    int already;
    if (!ulock_local(key, &already)) return 0;
    EnterCriticalSection(&g_ulockLock);
    ustate_merge(isPat ? &g_patPend[idx] : &g_clipPend, isPat, d, n, g);
    ustate_merge(isPat ? &g_patBase[idx] : &g_clipBase, isPat, d, n, g);
    if (already) unit_commit(key);               // lock already ours: this state is agreed now
    LeaveCriticalSection(&g_ulockLock);
    return 1;
}
// Group g (-1 = all) of a unit was replaced from outside (remote op / revert / baseline):
// that state is the agreed one, and what the next local change is compared against.
static void unit_agreed(int isPat, int idx, const unsigned char *d, unsigned n, int g) {
    EnterCriticalSection(&g_ulockLock);
    if (isPat) {
        if (idx >= 1 && idx < MAX_PAT) { ustate_merge(&g_patLast[idx], 1, d, n, g); ustate_merge(&g_patBase[idx], 1, d, n, g); }
    } else if (g < 0 || idx == g_clipLastArr) {
        ustate_merge(&g_clipLast, 0, d, n, g);
        ustate_merge(&g_clipBase, 0, d, n, g);
        g_clipLastArr = idx;
    }
    LeaveCriticalSection(&g_ulockLock);
}
// Copy of a unit's base. 0 if there is none yet.
static int unit_base(int isPat, int idx, unsigned char **d, unsigned *n) {
    UState *s;
    int ok;
    EnterCriticalSection(&g_ulockLock);
    s = isPat ? &g_patBase[idx] : (idx == g_clipLastArr ? &g_clipBase : NULL);
    ok = s && s->known;
    *d = ok ? recs_group(isPat, s->d, s->n, -1, n) : NULL;
    if (!ok) *n = 0;
    LeaveCriticalSection(&g_ulockLock);
    return ok;
}
// Pattern p's notes now: unlocked, SEH-guarded reads (as the hash scan does), selection bits
// cleared. 0 if the read faulted.
static int pat_peek(int p, unsigned char **out, unsigned *on) {
    unsigned i, n = 0;
    unsigned char *d = NULL;
    __try {
        char *a = pat_notes_arr(p);
        n = a ? *(unsigned *)(a + NOTES_COUNT) : 0;
        if (n > 1000000) n = 0;
        if (n) { d = (unsigned char *)malloc((size_t)n * NOTE_SIZE); memcpy(d, *(unsigned char **)(a + NOTES_DATA), (size_t)n * NOTE_SIZE); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(d); return 0; }
    for (i = 0; i < n; i++) d[(size_t)i * NOTE_SIZE + NOTE_SEL_OFS] &= (unsigned char)~NOTE_SEL_BIT;
    *out = d; *on = n;
    return 1;
}
// Baseline of one pattern / the clips of one arrangement: the current state is agreed.
static void pat_capture(int p) {
    unsigned char *d;
    unsigned n;
    if (!pat_peek(p, &d, &n)) return;
    unit_agreed(1, p, d, n, -1);
    free(d);
}
// Pattern p's notes equal its base (as sets, like do_notesnap compares): nothing to send. After a
// channel insert / delete, chan_rebase renumbered the base along with FL's notes.
static int pat_matches_base(int p) {
    unsigned char *d, *b;
    unsigned n, bn;
    int g, k;
    if (!unit_base(1, p, &b, &bn)) return 0;
    if (!pat_peek(p, &d, &n)) { free(b); return 0; }
    k = group_diff(1, d, n, b, bn, &g, 1);
    free(d); free(b);
    return !k;
}
static void clips_capture(int arr) {
    unsigned i, n = 0;
    unsigned char *d = NULL;
    __try {
        char *ca = arr_clips(arr);
        n = ca ? *(unsigned *)(ca + CLIPS_COUNT) : 0;
        if (n > 200000) n = 0;
        if (n) { d = (unsigned char *)malloc((size_t)n * CLIP_SIZE); memcpy(d, *(unsigned char **)(ca + CLIPS_DATA), (size_t)n * CLIP_SIZE); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(d); return; }
    for (i = 0; i < n; i++) d[(size_t)i * CLIP_SIZE + CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
    unit_agreed(0, arr, d, n, -1);
    free(d);
}
// Baseline (start, project load, structural ops): the current state of every unit is agreed.
static void units_capture_all(void) {
    int p;
    for (p = 1; p < MAX_PAT; p++) pat_capture(p);
    clips_capture(cur_arr());
}

// ---- samples that finished downloading: reload what uses them ----
static int mem_find(const unsigned char *d, size_t n, const void *pat, size_t pn) {
    size_t i;
    if (!pn || pn > n) return 0;
    for (i = 0; i + pn <= n; i++) if (d[i] == *(const unsigned char *)pat && memcmp(d + i, pat, pn) == 0) return 1;
    return 0;
}
// Main thread: if this unit's state mentions one of the file names in it->path (a list of
// NUL-terminated names, ending in an empty one), load its own state back, so FL reads the
// sample (no longer a placeholder) from disk again. Nothing is sent: everyone else had it.
static int names_in(const unsigned char *d, size_t n, const wchar_t *names) {   // any of names in d (UTF-16 or ANSI)
    const wchar_t *nm;
    for (nm = names; *nm; nm += wcslen(nm) + 1) {
        char an[MAX_PATH * 2];
        int al = WideCharToMultiByte(CP_ACP, 0, nm, -1, an, sizeof(an), NULL, NULL) - 1;
        if (mem_find(d, n, nm, wcslen(nm) * 2) || (al > 0 && mem_find(d, n, an, (size_t)al))) return 1;
    }
    return 0;
}
static void do_reloadu(const QItem *it) {
    size_t n = 0, kn = 0;
    unsigned char *d, *k;
    Unit *u = unit_ptr(it->unit, it->a, it->b);
    void *obj = unit_obj(it->unit, it->a, it->b);
    if (load_queued(it->unit, it->a, it->b)) return;     // a peer's newer state is about to load anyway
    d = unit_save(it->unit, it->a, it->b, &n);
    if (!d) return;
    // the state as it came, not as the plugin made it over the placeholder (or without the missing file:
    // its own save may not even name it any more)
    k = u ? phload_take(obj, u->editGen, &kn) : NULL;
    if (k && !names_in(k, kn, (const wchar_t *)it->path) && !names_in(d, n, (const wchar_t *)it->path)) {
        phload_note(obj, k, kn, u->editGen);              // not this file's: kept for its own
        free(k);
        k = NULL;
    }
    if (k || names_in(d, n, (const wchar_t *)it->path)) {
        char *path = tmp_path("rl");
        FILE *f = NULL;
        if (k) { free(d); d = k; n = kn; }
        if (fopen_s(&f, path, "wb") == 0 && f) {
            fwrite(d, 1, n, f); fclose(f);
            enqueue_unit(T_LOAD, it->unit, it->a, it->b, LOAD_FORCE, path);
            logmsg("SAMPLES reloading %s %d/%d: its sample arrived", it->unit == U_CH ? "ch" : "fx", it->a, it->b);
        } else free(path);
    }
    free(d);
}
// Poll thread (not during a project load): one reload check per unit for everything that
// finished downloading since the last call.
static wchar_t *reload_take(int *n, size_t *len);
static void reload_poll(void) {
    wchar_t *list;
    int n, i, t, s;
    size_t len;
    if (!(list = reload_take(&n, &len))) return;
    for (i = 0; i < g_nChKnown && i < MAX_CH; i++) {
        wchar_t *c = (wchar_t *)malloc(len * sizeof(wchar_t));
        memcpy(c, list, len * sizeof(wchar_t));
        enqueue_unit(T_RELOADU, U_CH, i, 0, 0, (char *)c);
    }
    for (t = 0; t < mix_count(); t++)
        for (s = 0; s < MAX_FX_SLOTS; s++) {
            wchar_t *c;
            if (!g_fxU[t][s].inst) continue;
            c = (wchar_t *)malloc(len * sizeof(wchar_t));
            memcpy(c, list, len * sizeof(wchar_t));
            enqueue_unit(T_RELOADU, U_FX, t, s, 0, (char *)c);
        }
    logmsg("SAMPLES %d finished downloading: checking which channels / effects use them", n);
    free(list);
}

// ---- plugin windows: state changes that move no parameter ----
// Many plugins (third-party VSTs especially) change their state without touching an automatable
// parameter: loading a sample or wavetable in their own browser, editing a mod matrix, drawing an
// envelope. Once the user stops working in a plugin window, that plugin's state is compared with its
// baseline (and sent if it changed); the plugin counts as worked in (unit_touched), so the round-robin
// sweep also sends a change that lands later. The window -> plugin mapping comes from FL's form object
// (VCL keeps it in a window property) referencing the channel / mixer slot or its plugin instance.
#define PLUGFOCUS_IDLE_MS  700
#define PLUGFOCUS_LONG_MS  3000         // long sessions in one window: check this often meanwhile
typedef struct { void *obj; int unit, a, b; } PlugRef;
typedef struct { void *obj; } VclFind;

static BOOL CALLBACK vcl_prop(HWND h, LPWSTR name, HANDLE data, ULONG_PTR lp) {
    wchar_t buf[64];
    (void)h;
    if (IS_INTRESOURCE(name)) { if (!GlobalGetAtomNameW((ATOM)(ULONG_PTR)name, buf, 64)) return TRUE; }
    else wcsncpy_s(buf, 64, name, _TRUNCATE);
    if (wcsncmp(buf, L"ControlOfs", 10) == 0) { ((VclFind *)lp)->obj = (void *)data; return FALSE; }
    return TRUE;
}
static int mem_readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!p || (uintptr_t)p < 0x10000 || !VirtualQuery(p, &mi, sizeof(mi))) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(mi.Protect & 0xEE)) return 0;
    return (const char *)p + n <= (const char *)mi.BaseAddress + mi.RegionSize;
}
static int plugref_cmp(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const PlugRef *)a)->obj, y = (uintptr_t)((const PlugRef *)b)->obj;
    return x < y ? -1 : x > y;
}
// Votes per known unit for the pointers in the first 4 KB of o (weight w each). Readability is
// checked first (never touch a guard page).
typedef struct { int unit, a, b, votes; } Vote;
#define VOTE_MAX 64
static void vote_refs(const char *o, const PlugRef *R, int nr, Vote *V, int *nv, int w) {
    int k, j;
    if (!mem_readable(o, 0x1000)) return;
    for (k = 0; k < 0x1000 / 8; k++) {
        PlugRef key, *r;
        key.obj = ((void *const *)o)[k];
        if (!key.obj || !(r = (PlugRef *)bsearch(&key, R, (size_t)nr, sizeof(PlugRef), plugref_cmp))) continue;
        for (j = 0; j < *nv && !(V[j].unit == r->unit && V[j].a == r->a && V[j].b == r->b); j++) {}
        if (j == *nv) {
            if (*nv >= VOTE_MAX) continue;
            V[j].unit = r->unit; V[j].a = r->a; V[j].b = r->b; V[j].votes = 0;
            (*nv)++;
        }
        V[j].votes += w;
    }
}
// Which unit a plugin window belongs to. The form and the objects it holds point at their own
// channel / slot through several fields (FL 2026: 4-6 of them); a field naming some other
// "current" channel adds a vote or two. Direct references count 4, one object further in 1;
// the clear winner is taken, a tie or a weak match is skipped.
static void do_plugfocus(const QItem *it) {
    static PlugRef R[MAX_CH * 2 + MAX_MIX * MAX_FX_SLOTS * 2];
    VclFind vf = { NULL };
    PlugRef hit;
    Vote V[VOTE_MAX];
    void **items;
    int nr = 0, nv = 0, i, t, s, nc = chan_list(&items), r, best = -1, second = 0;
    if (!IsWindow(it->wnd)) return;
    EnumPropsExW(it->wnd, vcl_prop, (LPARAM)&vf);
    if (!vf.obj) { logmsg("PLUGFOCUS: no form object behind window %p", (void *)it->wnd); return; }
    for (i = 0; i < nc && items; i++) {
        PlugRef a = { items[i], U_CH, i, 0 };
        R[nr++] = a;
        if (g_chU[i].inst) { a.obj = g_chU[i].inst; R[nr++] = a; }
    }
    for (t = 0; t < mix_count(); t++)
        for (s = 0; s < MAX_FX_SLOTS; s++) {
            PlugRef a = { fx_obj(t, s), U_FX, t, s };
            if (!g_fxU[t][s].inst || !a.obj) continue;
            R[nr++] = a;
            a.obj = g_fxU[t][s].inst; R[nr++] = a;
        }
    qsort(R, (size_t)nr, sizeof(PlugRef), plugref_cmp);
    if (!mem_readable(vf.obj, 0x1000)) return;
    __try {
        int k;
        vote_refs((const char *)vf.obj, R, nr, V, &nv, 4);
        for (k = 0; k < 0x1000 / 8; k++) {
            const char *o = ((const char *const *)vf.obj)[k];
            if (o != (const char *)vf.obj) vote_refs(o, R, nr, V, &nv, 1);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { nv = 0; }
    for (i = 0; i < nv; i++) {
        if (best < 0 || V[i].votes > V[best].votes) { if (best >= 0) second = V[best].votes; best = i; }
        else if (V[i].votes > second) second = V[i].votes;
    }
    r = best >= 0 && V[best].votes >= 2 && V[best].votes > second;
    if (!r) {
        logmsg("PLUGFOCUS: window %p: %s", (void *)it->wnd, nv ? "no clear plugin, skipped" : "plugin not found");
        return;
    }
    hit.unit = V[best].unit; hit.a = V[best].a; hit.b = V[best].b;
    logmsg("PLUGFOCUS: window %p is %s %d/%d (%d votes vs %d): checking its state", (void *)it->wnd,
           hit.unit == U_CH ? "ch" : "fx", hit.a, hit.b, V[best].votes, second);
    unit_worked(hit.unit, hit.a, hit.b);
}
// Someone here worked in this unit (its window, its clips): what its state shows now or at the sweep's next look goes out.
static void unit_worked(int unit, int a, int b) {
    Unit *u = unit_ptr(unit, a, b);
    if (u) u->userAt = GetTickCount();
    if (!u || u->lastAct || u->identAt || u->baselineAt || GetTickCount() < u->suppressUntil) return;   // handled elsewhere
    enqueue_unit(T_SNAP, unit, a, b, u->hash ? SNAP_IF_DIFF : SNAP_BASELINE, NULL);
}
static volatile HWND g_plugForm = NULL;    // plugin window the last input into one went to
static volatile DWORD g_plugInputAt = 0;   //   when (stored after it)
// Message hook: input that went into a plugin window. Only that is work in the plugin: keys, the wheel
// and drags elsewhere after a click in one used to count too, and kept a drifting plugin going out
// every PLUGFOCUS_LONG_MS while its user worked anywhere in FL.
static void plugin_input(HWND h, int press) {
    static int fromPlug = 0;                   // the last press outside FL's small dialogs / menus went to g_plugForm
    HWND root = h ? GetAncestor(h, GA_ROOT) : NULL;
    if (root && (wnd_class_is(root, "TNameEditForm") || wnd_class_is(root, "#32768") || wnd_class_is(root, "TMsgForm") ||
                 wnd_class_is(root, "#32770"))) {
        // a knob's "Type in value..." box / context menu opened from a plugin window: its own top-level window, but
        // the value it sets is that plugin's (a state-only one, like the sampler's time stretch, went out late)
        if (fromPlug && g_plugForm) g_plugInputAt = g_lastInput;
        return;
    }
    while (h && h != g_mainForm && !wnd_class_is(h, "TPluginForm")) h = GetParent(h);
    if (h && h != g_mainForm) { g_plugForm = h; g_plugInputAt = g_lastInput; fromPlug = 1; }
    else if (press) fromPlug = 0;              // (a key-up lands wherever the focus is: not work elsewhere)
}
// Poll thread: the user worked in a plugin window and paused (or has been at it for a while).
static void plugfocus_poll(DWORD now) {
    static HWND form = NULL;
    static DWORD inputAt = 0, checkedInput = 0, checkedAt = 0;
    DWORD t = g_plugInputAt;
    if (t != inputAt) {                        // (t's window, or a later one's)
        if (inputAt == checkedInput) checkedAt = now;   // work starts: a click is checked once, after it
        form = g_plugForm; inputAt = t;
    }
    if (!form || inputAt == checkedInput) return;
    if (now - g_lastInput >= PLUGFOCUS_IDLE_MS || now - checkedAt >= PLUGFOCUS_LONG_MS) {
        QItem q; memset(&q, 0, sizeof(q));
        q.type = T_PLUGFOCUS; q.wnd = form;
        enqueue(&q);
        checkedInput = inputAt; checkedAt = now;
    }
}

// ---- what we have selected stays selected ----
// FL's own operations select what they touch: Clone / Insert pattern make the new pattern current,
// adding or loading a channel can select it (and the mixer then shows its track). Replaying a
// peer's op must not move us, so around each one we note which channels are selected and which
// pattern is current, and put them back with FL's own setters. Patterns are found again by their
// record (its bytes move with it when patterns shift).
#define PATSELECT_VA FLVA(PATSELECT) // SelectPattern(pattern): what "jump to pattern" (MIDI scripts) calls
#define CHSELECT_VA  FLVA(CHSELECT) // SetChannelSelected(channel, on): the channel rack's selection light
#define CH_SELECTED  0x32c         // channel object: byte, selected
typedef struct { void *ch[MAX_CH]; unsigned char on[MAX_CH]; int n, pat; unsigned char patRec[PAT_STRIDE]; } SelSnap;
static SelSnap g_sel;
static int cur_pat(void);
static void sel_save(void) {
    void **items;
    int i;
    g_sel.n = 0;
    __try {
        int n = chan_list(&items);
        for (i = 0; i < n && items && i < MAX_CH; i++) { g_sel.ch[i] = items[i]; g_sel.on[i] = *((unsigned char *)items[i] + CH_SELECTED); }
        g_sel.n = i;
        g_sel.pat = cur_pat();
        if (g_sel.pat >= 1 && g_sel.pat < MAX_PAT) memcpy(g_sel.patRec, pat_rec(g_sel.pat), PAT_STRIDE); else g_sel.pat = -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_sel.n = 0; g_sel.pat = -1; }
}
static void sel_restore(void) {
    void **items;
    int i, j, p, best = -1, now = cur_pat(), moved = 0;
    __try {
        int n = chan_list(&items);
        for (i = 0; i < n && items; i++) {
            unsigned char was = 0, is = *((unsigned char *)items[i] + CH_SELECTED);
            for (j = 0; j < g_sel.n && g_sel.ch[j] != items[i]; j++) {}
            if (j < g_sel.n) was = g_sel.on[j];                  // a channel that is new here: not selected
            if (is != was) { ((void (*)(void *, int))(g_flBase + (CHSELECT_VA - IMG_BASE)))(items[i], was); moved++; }
        }
        if (g_sel.pat >= 1 && !(now == g_sel.pat && memcmp(pat_rec(now), g_sel.patRec, PAT_STRIDE) == 0)) {
            for (p = 1; p < MAX_PAT; p++)
                if (memcmp(pat_rec(p), g_sel.patRec, PAT_STRIDE) == 0 && (best < 0 || abs(p - g_sel.pat) < abs(best - g_sel.pat))) best = p;
            if (best > 0 && best != now) { ((void (*)(int))(g_flBase + (PATSELECT_VA - IMG_BASE)))(best); moved++; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (moved) logmsg("SELECT a peer's change moved our selection / pattern: put back (pattern %d)", best > 0 ? best : now);
}
// Which arrangement and pattern this person is on is their own view, but a project file carries the leader's: every
// re-sync moved a member onto the leader's arrangement and pattern (and the playlist's clip source and its tool
// buttons with it). Kept by number across a load of the leader's project, once this FL has the song (not on the
// first join: what it showed before was another song).
#define SETARR_VA FLVA(SETARR)     // SetCurrentArrangement(index) (sub_12FB5F0: writes ARRCUR, redraws the playlist)
static int g_viewArr = -1, g_viewPat = -1;
static int g_haveSong = 0;
static void view_save(void) {
    g_viewArr = g_haveSong ? cur_arr() : -1;
    g_viewPat = g_haveSong ? cur_pat() : -1;
}
static void view_restore(void) {
    int n = 0;
    __try { n = (int)*(int64_t *)(*(char **)(g_flBase + (ARRS_VA - IMG_BASE)) - 8); } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    __try {
        if (g_viewArr >= 0 && g_viewArr < n && g_viewArr != cur_arr()) ((void (*)(int))(g_flBase + (SETARR_VA - IMG_BASE)))(g_viewArr);
        if (g_viewPat >= 1 && g_viewPat < MAX_PAT && g_viewPat != cur_pat()) ((void (*)(int))(g_flBase + (PATSELECT_VA - IMG_BASE)))(g_viewPat);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("SELECT putting back the arrangement / pattern faulted"); }
    if (g_viewArr >= 0) logmsg("SELECT kept this FL's own arrangement %d and pattern %d across the leader's project", g_viewArr, g_viewPat);
    g_viewArr = g_viewPat = -1;
}

// ---------------- arrangements (the playlist's Arrangements menu) ----------------
// Adding, cloning, deleting and renaming an arrangement weren't synced: a leader's new arrangement reached the members
// only with the safety net's reload, and a member's was wiped by it. Now flim keeps the list of arrangement objects
// (identity, like channels) and their names; one change at a time goes out as an op, replayed with FL's own functions:
//   {"kind":"arr.ins","at":k,"n0":count before,"n":"<name>"}  InsertArrangement(k) (sub_12F9C20; a new last one:
//                                                              AddArrangement(0, 0), sub_12F9930, without copying
//                                                              the current arrangement's track links: each FL's differs)
//   {"kind":"arr.del","at":k,"n0":count before}               DeleteArrangement(k, select a neighbour, no undo)
//   {"kind":"arr.name","i":k,"n":"<name>"}                    the name (arrangement +8, a UnicodeString)
// A new (or cloned) arrangement's content follows as the usual ops: its clips per playlist track, its playlist tracks
// that aren't FL's default and its markers. Everything keyed by arrangement number (track names / links, markers, the
// clip baseline, each person's current arrangement) follows the renumbering. Several changes in one look, or a count
// that isn't the sender's (two people at once): re-sync. ponytail: clips / meta ops already on their way during an
// arrangement insert / delete land on the old numbers; arrangements aren't in the structure counts.
#define INSARR_VA  FLVA(INSARR)
#define ADDARR_VA  FLVA(ADDARR)
#define DELARR_VA  FLVA(DELARR)
#define USTRASG_VA FLVA(USTRASG)
#define ARR_MAX    64
typedef void (*insarr_t)(int at);
typedef int  (*addarr_t)(char a, char b);
typedef void (*delarr_t)(int at, char selectNeighbour, char undo);
typedef void (*ustrasg_t)(void *dst, const wchar_t *src);
static void   *g_arrKnown[ARR_MAX];
static uint64_t g_arrNameH[ARR_MAX];
static int     g_nArrKnown = -1;
static volatile LONG g_arrQueued = 0;
static const wchar_t *arr_name(int i) { char *a = arr_obj(i); return a ? *(const wchar_t **)(a + 8) : NULL; }
// Poll thread or main thread: the list as FL has it now. 0 = unreadable.
static int arr_take(void **objs, uint64_t *names, int *n) {
    int i, c = arr_count();
    __try {
        if (c < 1 || c > ARR_MAX) return 0;
        for (i = 0; i < c; i++) { objs[i] = arr_obj(i); names[i] = fnv_ustr(1469598103934665603ull, arr_name(i)); }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    *n = c;
    return 1;
}
static void arr_known_take(void) { int n; if (arr_take(g_arrKnown, g_arrNameH, &n)) g_nArrKnown = n; }
// Poll thread: the list or a name changed here: the main thread looks (do_arrstruct).
static void arr_poll(DWORD now) {
    static DWORD last;
    static void *o[ARR_MAX];
    static uint64_t h[ARR_MAX];
    int n, i, diff;
    if (now - last < 250 || g_projBusy) return;
    if (g_nArrKnown < 0) { arr_known_take(); return; }
    last = now;
    if (!arr_take(o, h, &n)) return;
    diff = n != g_nArrKnown;
    for (i = 0; !diff && i < n; i++) diff = o[i] != g_arrKnown[i] || h[i] != g_arrNameH[i];
    if (diff && !InterlockedExchange(&g_arrQueued, 1)) enqueue_unit(T_ARRSTRUCT, 0, 0, 0, 0, NULL);
}
// Everything keyed by arrangement number, through a change (from[new] = old, -1 = new).
static void arr_renumber(const int *from, int n) {
    static int fp[META_MAX];
    int i;
    for (i = 0; i < META_MAX; i++) {
        int a = i / 512, f = a < n ? from[a] : -1;
        fp[i] = f >= 0 && f < META_ARR_MAX ? f * 512 + i % 512 : -1;
    }
    meta_remap(M_PLT, fp, META_MAX);
    meta_remap(M_MARK, from, n);
    clips_rebaseline();
}
static void arr_name_json(int i, char *out, int cap) {
    const wchar_t *s = arr_name(i);
    int nl = ustr_len(s);
    if (nl * 4 / 3 + 8 > cap) nl = 0;
    b64enc(nl ? (const unsigned char *)s : (const unsigned char *)"", nl * 2, out);
}
// A new arrangement's content goes out as the usual ops (sender, main thread): clips per track, the playlist tracks that
// aren't FL's default (the most common state among its 500), its markers.
static void arr_send_content(int k) {
    unsigned n, i, j, gn;
    unsigned char *d = clips_copy(k, &n);
    static int gs[512];
    int ng = 0, t;
    for (i = 0; i < n; i++) d[(size_t)i * CLIP_SIZE + CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
    for (i = 0; i < n; i++) {
        int g = (int)rec_group(0, d + (size_t)i * CLIP_SIZE);
        for (j = 0; j < (unsigned)ng && gs[j] != g; j++) {}
        if (j == (unsigned)ng && ng < 512) gs[ng++] = g;
    }
    for (j = 0; j < (unsigned)ng; j++) {
        unsigned char *gd = recs_group(0, d, n, gs[j], &gn);
        size_t cap = 160 + ((size_t)gn * CLIP_SIZE + 2) / 3 * 4;
        char *m = (char *)malloc(cap);
        int o = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"clips.set\",\"key\":\"clips:%d:%d\",\"arr\":%d,\"trk\":%d,\"data\":\"", k, gs[j], k, gs[j]);
        b64enc(gd ? gd : (const unsigned char *)"", (int)(gn * CLIP_SIZE), m + o);
        strcat_s(m, cap, "\"}}");
        ws_send(m);
        free(m); free(gd);
    }
    free(d);
    if (ng) logmsg("ARR sent arrangement %d's clips (%d playlist tracks)", k, ng);
    if (k < META_ARR_MAX) {
        static uint64_t hs[PL_TRK_MAX + 1];
        uint64_t def = 0;
        int best = 0, c, u, sent = 0;
        for (t = 1; t <= PL_TRK_MAX; t++) hs[t] = meta_hash(M_PLT, k * 512 + t);
        for (t = 1; t <= PL_TRK_MAX && best < PL_TRK_MAX / 2; t += 7) {   // (a sample is enough to find the default)
            for (c = 0, u = 1; u <= PL_TRK_MAX; u++) c += hs[u] == hs[t];
            if (c > best) { best = c; def = hs[t]; }
        }
        for (t = 1; t <= PL_TRK_MAX; t++)
            if (hs[t] && hs[t] != def) { i = k * 512 + t; g_mSeen[M_PLT][i] = hs[t]; g_mHash[M_PLT][i] = hs[t] ^ 1; g_mAt[M_PLT][i] = 1; sent++; }
        uint64_t mh = meta_hash(M_MARK, k);
        g_mSeen[M_MARK][k] = mh; g_mHash[M_MARK][k] = mh != 1 ? mh ^ 1 : 2; g_mAt[M_MARK][k] = 1;
        if (sent) logmsg("ARR arrangement %d: %d playlist tracks that aren't FL's default go out", k, sent);
    }
}
// Main thread: what changed in the list here.
static void do_arrstruct(void) {
    static void *o[ARR_MAX];
    static uint64_t h[ARR_MAX];
    static int from[ARR_MAX];
    int n, n0 = g_nArrKnown, i, j, k = -1;
    char m[256], nm[160];
    InterlockedExchange(&g_arrQueued, 0);
    if (n0 < 0 || g_projBusy || !arr_take(o, h, &n)) return;
    if (n == n0 + 1) {                                         // one new: the others in order around it
        for (k = 0; k < n0 && o[k] == g_arrKnown[k]; k++) {}
        for (i = k + 1; i < n && o[i] == g_arrKnown[i - 1]; i++) {}
        if (i < n) k = -2;
    } else if (n == n0 - 1) {
        for (k = 0; k < n && o[k] == g_arrKnown[k]; k++) {}
        for (i = k; i < n && o[i] == g_arrKnown[i + 1]; i++) {}
        if (i < n) k = -2;
    } else if (n == n0) {
        for (i = 0; i < n && o[i] == g_arrKnown[i]; i++) {}
        if (i < n) k = -2;
    } else k = -2;
    if (k == -2) {
        logmsg("ARR the arrangement list changed in several ways at once (%d -> %d): re-syncing", n0, n);
        arr_known_take();
        resync("arrangements changed in several ways at once");
        return;
    }
    if (n != n0) {
        for (i = 0; i < n; i++) from[i] = n > n0 ? (i < k ? i : i == k ? -1 : i - 1) : (i < k ? i : i + 1);
        if (n > n0) {
            arr_name_json(k, nm, sizeof(nm));
            sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"arr.ins\",\"at\":%d,\"n0\":%d,\"n\":\"%s\"}}", k, n0, nm);
        } else sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"arr.del\",\"at\":%d,\"n0\":%d}}", k, n0);
        ws_send(m);
        logmsg("ARR arrangement %d %s locally (sent, %d -> %d)", k, n > n0 ? "added" : "deleted", n0, n);
        arr_renumber(from, n);
        arr_known_take();
        if (n > n0) arr_send_content(k);
        return;
    }
    for (i = 0; i < n; i++)                                    // same list: renames
        if (h[i] != g_arrNameH[i]) {
            arr_name_json(i, nm, sizeof(nm));
            sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"arr.name\",\"i\":%d,\"n\":\"%s\"}}", i, nm);
            ws_send(m);
            logmsg("ARR arrangement %d renamed locally (sent)", i);
        }
    (void)j;
    arr_known_take();
}
static void arr_set_name(int i, const char *b64) {
    size_t n = 0;
    unsigned char *w = b64dec(b64, strlen(b64), &n);
    wchar_t buf[128], *s;
    int len = (int)(n / 2) < 127 ? (int)(n / 2) : 127, j;
    char *a = arr_obj(i);
    if (!a) { free(w); return; }
    if (w) memcpy(buf, w, (size_t)len * 2);
    buf[len] = 0;
    free(w);
    s = dstr_keep(buf);
    __try { ((ustrasg_t)(g_flBase + (USTRASG_VA - IMG_BASE)))(a + 8, s); } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("ARR setting a name faulted"); }
    dstr_free(s);
    (void)j;
}
// Main thread: a peer's arrangement op.
static void do_arrop(const QItem *it) {
    const char *m = it->path, *q;
    static int from[ARR_MAX];
    int at = -1, n0 = -1, i, cur = cur_arr(), n = arr_count();
    char nm[192] = "";
    if ((q = strstr(m, "\"n\":\"")) != NULL) { const char *e = strchr(q + 5, '"'); size_t l = e ? (size_t)(e - q - 5) : 0; if (l < sizeof(nm)) { memcpy(nm, q + 5, l); nm[l] = 0; } }
    if (strstr(m, "\"kind\":\"arr.name\"")) {
        if (int_after(m, NULL, "\"i\":", &at) && at >= 0 && at < n) { arr_set_name(at, nm); logmsg("ARR arrangement %d renamed (remote)", at); }
        arr_known_take();
        return;
    }
    int_after(m, NULL, "\"at\":", &at);
    int_after(m, NULL, "\"n0\":", &n0);
    if (n0 != n || at < 0 || n0 + 1 > ARR_MAX) {
        logmsg("ARR %.40s for %d arrangements, this FL has %d: re-syncing", m, n0, n);
        resync("an arrangement change for another arrangement list");
        return;
    }
    __try {
        if (strstr(m, "\"kind\":\"arr.ins\"")) {
            if (at > n0) at = n0;
            if (at < n0) ((insarr_t)(g_flBase + (INSARR_VA - IMG_BASE)))(at);
            else ((addarr_t)(g_flBase + (ADDARR_VA - IMG_BASE)))(0, 0);
            if (arr_count() != n0 + 1) { logmsg("ARR FL didn't add the arrangement: re-syncing"); resync("an arrangement FL didn't add"); return; }
            if (nm[0]) arr_set_name(at, nm);
            for (i = 0; i <= n0; i++) from[i] = i < at ? i : i == at ? -1 : i - 1;
            arr_renumber(from, n0 + 1);
            if (cur >= at) ((void (*)(int))(g_flBase + (SETARR_VA - IMG_BASE)))(cur + 1);   // (still on the same one)
            logmsg("ARR arrangement %d added (remote)", at);
        } else if (strstr(m, "\"kind\":\"arr.del\"") && at < n0 && n0 > 1) {
            ((delarr_t)(g_flBase + (DELARR_VA - IMG_BASE)))(at, 1, 0);
            if (arr_count() != n0 - 1) { logmsg("ARR FL didn't delete the arrangement: re-syncing"); resync("an arrangement FL didn't delete"); return; }
            for (i = 0; i < n0 - 1; i++) from[i] = i < at ? i : i + 1;
            arr_renumber(from, n0 - 1);
            if (cur > at) ((void (*)(int))(g_flBase + (SETARR_VA - IMG_BASE)))(cur - 1);
            logmsg("ARR arrangement %d deleted (remote)", at);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("ARR applying %.40s faulted", m); }
    arr_known_take();
}

// ---- structure ops that cross ----
// Two people changing channels or mixer tracks in the same instant each change their own list first and
// the other's op lands on a list it wasn't made for (an insert here and a move there: the same count, so the
// n0 checks pass, and the lists differ from then on). And an edit someone made before a structure change
// reached them still names channels / tracks by their old numbers. The structure counts (g_sc, one per kind:
// channels, mixer tracks, patterns) tell both: a structure op whose sender's count of its kind isn't ours
// crossed another one: the leader skips it, a member loads the leader's project (everyone who has it applied
// sees a crossing too, so all end up with the leader's lists). Kinds don't cross each other (a channel deleted
// while a mixer track moved: each FL can do them in either order), except a new channel, whose automation
// link names a mixer track. An edit with an older count is moved through the structure ops of that kind it
// hadn't seen, which the log keeps (new number -> old). Without counts (an older flim) all of it works as before.
#define SC_LOG 64
typedef struct { LONG sc; int n; int *from; } ScEnt;        // n -1: not known (FL didn't do it as sent, a move)
static ScEnt g_scLog[SC_N][SC_LOG];                         // per kind, by count
static int sc_struct(int type) { return type == T_ADDCH || type == T_DELCH || type == T_ORDER || type == T_MIXSTRUCT || type == T_PATOP; }
static int sc_kind(int type) { return type == T_MIXSTRUCT ? SC_MIX : type == T_PATOP ? SC_PAT : SC_CH; }

// Main thread: a structure op applied (or sent); from (new number -> old, -1 = new) NULL if not known.
static void sc_done(int kind, const int *from, int n) {
    ScEnt *e;
    LONG sc = g_sc[kind];
    if (sc < 0) return;
    e = &g_scLog[kind][sc % SC_LOG];
    free(e->from);
    e->from = NULL;
    e->sc = sc; e->n = from ? n : -1;
    if (from) { e->from = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1)); memcpy(e->from, from, sizeof(int) * (size_t)n); }
    InterlockedExchange(&g_sc[kind], sc + 1);
}
// A pattern op (k: PATOP_SLOT index) on pats[0..n). Insert, clone and delete have known maps (checked on FL
// 2026: Insert(p) puts an empty pattern at p; Clone puts each copy right after its source); the moves and
// split are counted without one (edits made before them go as they are).
static int int_cmp(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

// Patterns renumbered (from: new number -> old, -1 = new): each pattern's note hashes, agreed / pending /
// base states and name / marker baselines follow it, so a note edit made in the same moment still goes
// out. New patterns are taken as they are. Clips are re-taken (one hash for the whole arrangement).
static void pat_rebase(const int *from) {
    static uint64_t h[MAX_PAT], sn[MAX_PAT];
    static DWORD at[MAX_PAT];
    static UState L[MAX_PAT], P[MAX_PAT], B[MAX_PAT];
    static unsigned char used[MAX_PAT];
    int p, f;
    memcpy(h, g_patHash, sizeof(h)); memcpy(sn, g_patSeen, sizeof(sn)); memcpy(at, g_patChangedAt, sizeof(at));
    memset(used, 0, sizeof(used));
    EnterCriticalSection(&g_ulockLock);
    memcpy(L, g_patLast, sizeof(L)); memcpy(P, g_patPend, sizeof(P)); memcpy(B, g_patBase, sizeof(B));
    for (p = 1; p < MAX_PAT; p++) {
        f = from[p];
        if (f >= 1 && f < MAX_PAT && !used[f]) {
            used[f] = 1;
            g_patHash[p] = h[f]; g_patSeen[p] = sn[f]; g_patChangedAt[p] = at[f];
            g_patLast[p] = L[f]; g_patPend[p] = P[f]; g_patBase[p] = B[f];
        } else {
            g_patSeen[p] = g_patHash[p] = notes_hash(p); g_patChangedAt[p] = 0;
            memset(&g_patLast[p], 0, sizeof(UState)); memset(&g_patPend[p], 0, sizeof(UState)); memset(&g_patBase[p], 0, sizeof(UState));
        }
    }
    for (f = 1; f < MAX_PAT; f++) if (!used[f]) { free(L[f].d); free(P[f].d); free(B[f].d); }   // deleted
    LeaveCriticalSection(&g_ulockLock);
    for (p = 1; p < MAX_PAT; p++) if (from[p] < 1) pat_capture(p);
    meta_remap(M_PAT, from, MAX_PAT);
    meta_remap(M_PMARK, from, MAX_PAT);
    ulock_release_moved("pat:", NULL, from, MAX_PAT);
    clips_rebaseline();
}

// Each pattern's identity just before a pattern op (pat_sigs_take): its notes (a move swaps note contents
// between the arrays, which stay put) and its name and marker list pointers, which move with the record.
// (drain_queue_mainthread runs nothing while a pattern op is under way: FL's rename box after an Insert runs a
// modal loop, and a channel delete run in it renumbered the notes' channels.) For split: how many channels each
// source has notes in.
typedef struct { uint64_t notes; void *name, *marks; } PatSig;
static PatSig g_patPre[MAX_PAT];
static PatSig pat_sig_of(int p) {
    PatSig g;
    char *r = pat_rec(p);
    g.notes = notes_hash(p); g.name = *(void **)(r + PAT_NAME); g.marks = *(void **)(r + PAT_MARKERS);
    return g;
}
static void pat_sigs_take(int k, const int *pats, int n) {
    int p, i;
    __try {
        for (p = 1; p < MAX_PAT; p++) g_patPre[p] = pat_sig_of(p);
        for (i = 0; k == 5 && i < n; i++) {                     // split: channels with notes in each source
            static unsigned char seen[MAX_CH];
            unsigned cnt = 0, j;
            unsigned char *d;
            if (pats[i] < 1 || pats[i] >= MAX_PAT) continue;
            d = notes_copy(pats[i], &cnt);
            memset(seen, 0, sizeof(seen));
            g_patPreCh[pats[i]] = 0; g_patPreH[pats[i]] = 0;
            for (j = 0; d && j < cnt; j++) {
                uint16_t c = *(uint16_t *)(d + (size_t)j * NOTE_SIZE + NOTE_CH_OFS);
                if (c < MAX_CH && !seen[c]) { seen[c] = 1; g_patPreCh[pats[i]]++; }
                g_patPreH[pats[i]] += (uint32_t)notes_fnv(d + (size_t)j * NOTE_SIZE, 1);
            }
            free(d);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { logmsg("PATTERN op: couldn't read the patterns before it"); }
}

// What a pattern op (k: PATOP_SLOT index) on pats[0..n) does to pattern numbers: from[new] = old, -1 = new.
// Measured on FL 2026 with uniquely named patterns:
//   Insert(p)          an empty pattern at p, the rest one up
//   Clone(pats)        each copy right after its source (sources in ascending order)
//   Delete(pats)       the rest close the gap
//   0x108 (pats, delta, target)  target -1: each pattern swaps with the one delta away, in list order for a
//                      negative delta, from the end for a positive one (that keeps a moved block together);
//                      another target looks patterns up by a record field that is -1 here: nothing moves
//   0x110              renames / recolors (FL's name and color setters): nothing moves
//   SplitByChannel     each source keeps its number (its first channel's notes); one new pattern per other
//                      channel with notes goes right after it (g_patPreCh); one channel: FL refuses
// FL can skip a move by its own state, so sc_done_patop checks the result against what FL did.
static int patop_map(int k, const int *pats, int n, int delta, int target, int *from) {
    static int src[MAX_PAT];
    int i, j, m = 0;
    for (i = 0; i < MAX_PAT; i++) from[i] = i;
    if (k == 3) return 1;
    if (k == PATOP_INSERT) {
        if (n < 1 || pats[0] < 1 || pats[0] >= MAX_PAT) return 0;
        memmove(from + pats[0] + 1, from + pats[0], sizeof(int) * (size_t)(MAX_PAT - pats[0] - 1));
        from[pats[0]] = -1;
        return 1;
    }
    if (k == 2) {
        if (target != -1) return 1;
        for (j = 0; j < n; j++) {
            int p = pats[delta < 0 ? j : n - 1 - j], q = p + delta, t;
            if (p < 1 || p >= MAX_PAT || q < 1 || q >= MAX_PAT || q == p) continue;
            t = from[p]; from[p] = from[q]; from[q] = t;
        }
        return 1;
    }
    for (i = 0; i < n; i++) if (pats[i] >= 1 && pats[i] < MAX_PAT) src[m++] = pats[i];
    qsort(src, m, sizeof(int), int_cmp);
    if (k == 1) {                                               // delete: the rest close the gap
        int w = 0;
        for (i = 0; i < MAX_PAT; i++) if (!bsearch(&i, src, m, sizeof(int), int_cmp)) from[w++] = i;
        for (; w < MAX_PAT; w++) from[w] = -1;
        return 1;
    }
    for (i = 0; i < m; i++) {                                   // clone: a copy after each source; split: one per
        int add = k == 5 ? g_patPreCh[src[i]] - 1 : 1, x;       // other channel
        for (j = 0; j < MAX_PAT && from[j] != src[i]; j++) {}
        for (x = 0; x < add && j < MAX_PAT - 1; x++) {
            memmove(from + j + 2, from + j + 1, sizeof(int) * (size_t)(MAX_PAT - j - 2));
            from[j + 1] = -1;
        }
    }
    return 1;
}

// Did FL do what the map says? Every pattern that only moved is where the map puts it (a split source and new
// patterns change, so they aren't compared). Patterns that look alike (empty, unnamed) can't be told apart,
// and needn't be.
static int patop_map_ok(int k, const int *pats, int n, const int *from) {
    int j, i, x;
    __try {
        for (j = 1; j < MAX_PAT; j++) {
            PatSig now;
            if ((i = from[j]) < 1) continue;
            for (x = 0; k == 5 && x < n && pats[x] != i; x++) {}
            if (k == 5 && x < n) continue;
            now = pat_sig_of(j);
            if (now.notes != g_patPre[i].notes || now.name != g_patPre[i].name || now.marks != g_patPre[i].marks) {
                logmsg("PATTERN op: pattern %d isn't old pattern %d as expected", j, i);
                return 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

// Main thread, after a pattern op ran here (pat_sigs_take before it): counted, and the pattern baselines
// carried through its map, or re-taken when FL didn't do what the map says.
static void sc_done_patop(int k, const int *pats, int n, int delta, int target) {
    static int from[MAX_PAT];
    if (!patop_map(k, pats, n, delta, target, from)) {
        sc_done(SC_PAT, NULL, 0); notes_rebaseline(); ulock_release_moved("pat:", NULL, NULL, 0);
        return;
    }
    if (!patop_map_ok(k, pats, n, from)) {
        // FL did something else here (it skips a move by its own state, which can differ between FLs): the
        // other FLs did what the op says, or this one's version. Out of step either way: the leader's song.
        sc_done(SC_PAT, NULL, 0); notes_rebaseline(); ulock_release_moved("pat:", NULL, NULL, 0);
        resync("FL did a pattern op differently here");
        return;
    }
    sc_done(SC_PAT, from, MAX_PAT);
    pat_rebase(from);
}
// A mixer track number as it was at mixer count `from`, as it is now (a gone track: master); as it is if a
// change since has no map.
static uint32_t sc_mix_map_value(uint32_t v, int from) {
    LONG e, now = g_sc[SC_MIX];
    int j;
    if (from < 0 || now < 0 || from >= now || now - from > SC_LOG) return v;
    for (e = from; e < now; e++) if (g_scLog[SC_MIX][e % SC_LOG].sc != e || g_scLog[SC_MIX][e % SC_LOG].n < 0) return v;
    for (e = from; e < now && v; e++) {
        const ScEnt *s = &g_scLog[SC_MIX][e % SC_LOG];
        for (j = 0; j < s->n && s->from[j] != (int)v; j++) {}
        v = j < s->n ? (uint32_t)j : 0;
    }
    return v;
}
// one channel deleted at k (of n0) / added at k (n0 before it)
static void sc_done_del(int k, int n0) {
    static int from[MAX_CH];
    int i;
    for (i = 0; i < n0 - 1; i++) from[i] = i < k ? i : i + 1;
    sc_done(SC_CH, from, n0 - 1);
}
static void sc_done_add(int k, int n0) {
    static int from[MAX_CH];
    int i;
    for (i = 0; i <= n0; i++) from[i] = i < k ? i : i == k ? -1 : i - 1;
    sc_done(SC_CH, from, n0 + 1);
}

static void task_free(QItem *it) {
    switch (it->type) {
    case T_LOAD: case T_ADDCH: DeleteFileA(it->path); free(it->path); break;
    case T_META: case T_MIXSTRUCT: case T_PATOP: case T_ARROP: free(it->path); break;
    case T_ORDER: case T_NOTES: case T_CLIPS: free(it->order); break;
    }
}

// A meta.set's kind of number: channel (SC_CH), mixer track (SC_MIX: names, routes) or pattern (SC_PAT:
// names, markers); -1: none (playlist tracks, arrangement markers, time signature).
static int meta_kind(const char *m) {
    const char *e = strstr(m, "\"e\":\"");
    int t;
    if (!e) return -1;
    for (t = 0; t < M_NTYPES && strncmp(e + 5, M_TAG[t], 2) != 0; t++) {}
    if (t == M_PLT) {                           // a playlist track's link names a mixer track or a channel
        const char *lk = strstr(m, "\"lk\":[");
        return !lk ? -1 : lk[6] == '1' ? SC_MIX : lk[6] == '3' ? SC_CH : -1;
    }
    return t == M_CHAN ? SC_CH : t == M_MIX || t == M_ROUTE ? SC_MIX : t == M_PAT || t == M_PMARK ? SC_PAT : -1;
}

// Does a peer's task name mixer tracks by number? (run_task sends a track change still waiting here first)
static int names_mix_tracks(const QItem *it) {
    switch (it->type) {
    case T_MIXSTRUCT: case T_ADDCH: return 1;
    case T_PARAM:   return it->id >= 0x70000000u || IS_CH_TARGET(it->id);
    case T_LOAD: case T_FXCLEAR: return it->unit == U_FX;
    case T_META:    return meta_kind(it->path) == SC_MIX;
    }
    return 0;
}

// Does a structure op of this kind renumber what this task names?
static int sc_touches(const QItem *it, int kind) {
    switch (it->type) {
    case T_PARAM:   return kind != SC_PAT;
    case T_LOAD: case T_FXCLEAR: return kind == (it->unit == U_FX ? SC_MIX : SC_CH);
    case T_NOTES: case T_CLIPS: return kind != SC_MIX;
    case T_META:    return kind == meta_kind(it->path);
    }
    return 0;
}

// A meta.set's numbers ("i", and a route's targets) through toNew; 0: its entity is gone.
static int meta_path_remap(QItem *it, int cap, const int *toNew) {
    const char *m = it->path, *e = strstr(m, "\"e\":\""), *pi = strstr(m, "\"i\":"), *p;
    int i, route, ni, d, l;
    size_t sz = strlen(m) + 32 + 4 * MAX_MIX;                  // (renumbered targets can be a digit longer)
    char *out, *o;
    if (!e || !pi) return 1;
    if (strncmp(e + 5, M_TAG[M_PLT], 2) == 0) {                // the link's target only (its own number stays)
        const char *lk = strstr(m, "\"lk\":["), *x;
        int lt, lx;
        if (!lk || !(x = strchr(lk + 6, ','))) return 1;
        lt = atoi(lk + 6); lx = atoi(x + 1);
        lx = lx >= 0 && lx < cap && toNew[lx] >= 0 ? toNew[lx] : -1;
        out = (char *)malloc(sz);
        memcpy(out, m, (size_t)(lk - m));
        o = out + (lk - m);
        o += sprintf_s(o, sz - (size_t)(o - out), "\"lk\":[%d,%d]", lx < 0 ? 0 : lt, lx);
        strcpy_s(o, sz - (size_t)(o - out), strchr(x, ']') ? strchr(x, ']') + 1 : "");
        free(it->path);
        it->path = out;
        return 1;                                              // (a gone target: unlinked, as FL does)
    }
    route = strncmp(e + 5, M_TAG[M_ROUTE], 2) == 0;
    i = atoi(pi + 4);
    if (i < 0 || i >= cap || (ni = toNew[i]) < 0) return 0;
    out = o = (char *)malloc(sz);
    memcpy(o, m, (size_t)(pi - m)); o += pi - m;
    o += sprintf_s(o, sz - (size_t)(o - out), "\"i\":%d", ni);
    for (p = pi + 4; *p == '-' || isdigit((unsigned char)*p); p++) {}
    if (route && (e = strstr(p, "\"r\":[")) != NULL) {       // [[target,level],...]: targets renumbered, gone ones out
        int k = 0;
        memcpy(o, p, (size_t)(e + 5 - p)); o += e + 5 - p;
        for (p = e + 5; *p == '['; ) {
            char *x;
            d = (int)strtol(p + 1, &x, 10);
            if (*x != ',') break;
            l = (int)strtol(x + 1, &x, 10);
            if (*x != ']') break;
            p = x + 1;
            if (*p == ',') p++;
            if (d >= 0 && d < MAX_MIX && toNew[d] >= 0) o += sprintf_s(o, sz - (size_t)(o - out), "%s[%d,%d]", k++ ? "," : "", toNew[d], l);
        }
    }
    strcpy_s(o, sz - (size_t)(o - out), p);
    free(it->path);
    it->path = out;
    return 1;
}

// Records (notes: their channel; clips: the channel or pattern they play) through toNew; the ones whose
// channel / pattern is gone are left out. Returns how many are left.
static int recs_remap(unsigned char *r, int n, int isPat, int kind, const int *toNew, int cap) {
    int k, w = 0;
    size_t rs = isPat ? NOTE_SIZE : CLIP_SIZE;
    for (k = 0; k < n; k++) {
        unsigned char *x = r + k * rs;
        int old = -1, neu;
        if (isPat) old = *(uint16_t *)(x + NOTE_CH_OFS);
        else {
            uint32_t item = *(uint32_t *)(x + 0x04);
            if ((item & 0xFFFFu) == 0x5000u) {
                int hi = (int)(item >> 16);                     // ch << 16 | 0x5000, or (0x5000 + pattern) << 16 | 0x5000
                old = kind == SC_CH ? (hi < 0x5000 ? hi : -1) : (hi >= 0x5000 ? hi - 0x5000 : -1);
            }
        }
        if (old >= 0) {
            if (old >= cap || (neu = toNew[old]) < 0) continue;
            if (isPat) *(uint16_t *)(x + NOTE_CH_OFS) = (uint16_t)neu;
            else *(uint32_t *)(x + 0x04) = ((uint32_t)(kind == SC_CH ? neu : 0x5000 + neu) << 16) | 0x5000u;
        }
        if (w != k) memmove(r + w * rs, x, rs);
        w++;
    }
    return w;
}

// Before a peer's task runs (main thread): 0 = drop it (freed here).
static int sc_rebase(QItem *it) {
    static int chToNew[MAX_CH], mixToNew[MAX_MIX], patToNew[MAX_PAT];
    static const int CAP[SC_N] = { MAX_CH, MAX_MIX, MAX_PAT };
    int *const TONEW[SC_N] = { chToNew, mixToNew, patToNew };
    int k, j, moved = 0;
    LONG e, now;
    if (!it->peer) return 1;
    for (k = 0; k < SC_N; k++) if (it->sc[k] < 0 || g_sc[k] < 0) return 1;
    if (sc_struct(it->type)) {
        int kind = sc_kind(it->type);
        if (it->sc[kind] == g_sc[kind]) {
            if (it->type != T_ADDCH || it->sc[SC_MIX] == g_sc[SC_MIX]) return 1;
            // a new channel made before a mixer change it hadn't seen (an automation channel's link names a track by
            // number): only this FL can tell (that mixer change doesn't cross anything there), so whoever finds it
            // re-syncs; the leader sends everyone its project
            logmsg("STRUCT a new channel made at mixer count %d reached this FL at %ld: %s", it->sc[SC_MIX], g_sc[SC_MIX],
                   g_isLeader ? "skipped, sending everyone the leader's project" : "loading the leader's project");
            resync("a new channel crossed a mixer change");
            task_free(it);
            return 0;
        }
        if (g_isLeader && it->type == T_ADDCH && it->mode >= 0 && it->a == it->mode && it->sc[SC_MIX] == g_sc[SC_MIX] &&
            it->sc[SC_CH] < g_sc[SC_CH] && g_sc[SC_CH] - it->sc[SC_CH] <= SC_LOG && it->mode + (g_sc[SC_CH] - it->sc[SC_CH]) == g_nChKnown) {
            // a channel appended at the end, crossing only channels appended here (two people making automation clips
            // at once): it goes after them, so the member's re-sync brings it back (skipped, it was lost; its links
            // follow it: a load makes them this channel's)
            int ok = 1, i;
            for (e = it->sc[SC_CH]; e < g_sc[SC_CH] && ok; e++) {
                const ScEnt *s = &g_scLog[SC_CH][e % SC_LOG];
                ok = s->sc == e && s->n > 0 && s->from[s->n - 1] == -1;
                for (i = 0; ok && i < s->n - 1; i++) ok = s->from[i] == i;
            }
            if (ok) {
                logmsg("STRUCT a new channel made at count %d reached this FL at %ld: it crossed channels added here; "
                       "added after them (its sender loads the leader's project)", it->sc[SC_CH], g_sc[SC_CH]);
                it->a = it->mode = g_nChKnown;
                return 1;
            }
        }
        logmsg("STRUCT a structure op made at count %d reached this FL at %ld: it crossed another one; %s", it->sc[kind], g_sc[kind],
               g_isLeader ? "skipped (its sender loads the leader's project)" : "loading the leader's project");
        if (!g_isLeader) resync("two structure changes crossed");
        task_free(it);
        return 0;
    }
    for (k = 0; k < SC_N; k++) {                               // each kind on its own (they don't renumber each other)
        int *toNew = TONEW[k], cap = CAP[k], ok = 1;
        now = g_sc[k];
        if (!sc_touches(it, k) || it->sc[k] >= now || now - it->sc[k] > SC_LOG) continue;   // (newer: a crossing skipped here)
        for (e = it->sc[k]; e < now && ok; e++) ok = g_scLog[k][e % SC_LOG].sc == e && g_scLog[k][e % SC_LOG].n >= 0;
        if (!ok) continue;                                     // a map missing: as it is
        for (e = it->sc[k]; e < now; e++) {
            const ScEnt *s = &g_scLog[k][e % SC_LOG];
            uint32_t nid;
            int gone = 0;
            for (j = 0; j < cap; j++) toNew[j] = -1;
            for (j = 0; j < s->n; j++) if (s->from[j] >= 0 && s->from[j] < cap) toNew[s->from[j]] = j;
            moved++;
            switch (it->type) {
            case T_PARAM:
                if (!rebase_slot_id(it->id, k == SC_CH, chToNew, k == SC_MIX, mixToNew, &nid)) gone = 1;
                else it->id = nid;
                if (!gone && k == SC_MIX && IS_CH_TARGET(it->id))   // the track it routes to (a gone one: master)
                    it->v = it->v < (uint32_t)cap && toNew[it->v] >= 0 ? (uint32_t)toNew[it->v] : 0;
                break;
            case T_LOAD: case T_FXCLEAR:
                if (it->a < 0 || it->a >= cap || toNew[it->a] < 0) gone = 1;
                else it->a = toNew[it->a];
                break;
            case T_NOTES:
                if (k == SC_PAT) {                              // its pattern
                    if (it->a < 0 || it->a >= cap || toNew[it->a] < 0) gone = 1; else it->a = toNew[it->a];
                    break;
                }
                if (it->b >= 0) { if (it->b >= cap || toNew[it->b] < 0) { gone = 1; break; } it->b = toNew[it->b]; }
                it->mode = recs_remap((unsigned char *)it->order, it->mode, 1, SC_CH, toNew, cap);
                break;
            case T_CLIPS:                                       // (its group is a playlist track: not renumbered)
                it->mode = recs_remap((unsigned char *)it->order, it->mode, 0, k, toNew, cap);
                break;
            case T_META:
                gone = !meta_path_remap(it, cap, toNew);
                break;
            }
            if (gone) {
                logmsg("STRUCT an edit (task type %d) made before a structure change here is for something that change removed: dropped", it->type);
                task_free(it);
                return 0;
            }
        }
    }
    if (moved) logmsg("STRUCT an edit (task type %d) moved through %d structure change%s", it->type, moved, moved == 1 ? "" : "s");
    return 1;
}

// A channel task queued before channels moved (a reorder, a delete) is for the channel it was queued
// for, not for whatever now sits at its old index: a revert of channel 0 queued just before someone
// moved channel 1 up would otherwise load channel 0's state (and name) over the moved channel.
static int task_rebind(QItem *it) {
    void **items;
    int n, j;
    if (it->peer && it->unit == U_CH && it->type == T_LOAD) {
        // a peer's channel index: the channel at that place in the list everyone has (not FL's list,
        // which may hold a change of ours that isn't sent yet)
        if (it->a < 0 || it->a >= g_nChKnown) {
            logmsg("TASK type=%d for channel %d: there is no such channel here, dropped", it->type, it->a);
            DeleteFileA(it->path); free(it->path);
            return 0;
        }
        it->chobj = g_chKnown[it->a];
    }
    if (!it->chobj) return 1;
    n = chan_list(&items);
    for (j = 0; j < n && items && items[j] != it->chobj; j++) {}
    if (!items || j == n) {
        logmsg("TASK type=%d for channel %d: that channel is gone, dropped", it->type, it->a);
        if (it->type == T_LOAD) DeleteFileA(it->path);
        if (it->type == T_LOAD || it->type == T_RELOADU) free(it->path);
        return 0;
    }
    if (j != it->a) { logmsg("TASK type=%d for channel %d: it is channel %d now", it->type, it->a, j); it->a = j; }
    return 1;
}

static void run_task(const QItem *in) {
    QItem copy = *in, *it = &copy;
    LONG sc0;
    if (it->peer && it->sc[SC_MIX] >= 0 && g_sc[SC_MIX] >= 0 && names_mix_tracks(it) && mix_layout_changed()) {
        // tracks moved here and that isn't sent yet (do_struct hasn't run): it happened before this op came,
        // so it goes out first and the op is moved through it (or crossed it)
        logmsg("STRUCT a mixer track change made here goes out before a peer's op that names tracks");
        mix_struct();
    }
    sc0 = sc_struct(it->type) ? g_sc[sc_kind(it->type)] : -1;
    if (!sc_rebase(it)) return;
    if (!task_rebind(it)) return;
    int remoteHeavy = it->type == T_LOAD || it->type == T_ADDCH || it->type == T_DELCH || it->type == T_FXCLEAR;
    int remote = remoteHeavy || it->type == T_ORDER || it->type == T_PATOP || it->type == T_NOTES || it->type == T_NOTESYNC ||
                 it->type == T_CLIPS || it->type == T_META || it->type == T_MIXSTRUCT || it->type == T_FXSYNC;
    if (remoteHeavy) wguard_begin();
    if (remote) sel_save();
    run_task_inner(it);
    if (sc_struct(it->type) && sc0 >= 0 && g_sc[sc_kind(it->type)] == sc0) sc_done(sc_kind(it->type), NULL, 0);   // counted like everyone, even when it failed here
    if (remote) sel_restore();
    if (remoteHeavy) wguard_pass();
    if (remoteHeavy || it->type == T_ORDER || it->type == T_MIXSTRUCT || it->type == T_FXSYNC) g_linkQuiet = GetTickCount();
}

static void run_task_inner(const QItem *it) {
    switch (it->type) {
    case T_SNAP:   do_snap(it); break;
    case T_LOAD:   do_load(it); DeleteFileA(it->path); free(it->path); break;
    case T_ADDCH:  do_addch(it); break;      // hands it->path on to a T_LOAD
    case T_DELCH:  do_delch(it); break;
    case T_STRUCT: do_struct(); break;
    case T_FULL:   do_full(); break;
    case T_ORDER:  do_order(it); free(it->order); break;
    case T_FXCLEAR: do_fxclear(it); break;
    case T_FXSYNC: do_fxsync(it); free(it->order); break;
    case T_NOTESNAP: do_notesnap(it); break;
    case T_NOTES:  do_notes(it); free(it->order); break;
    case T_NOTESYNC: do_notesync(it); free(it->order); break;
    case T_PROJLOAD:                          // the leader's project: its structure count from here on
        if (it->peer) {
            int k;
            clips_hold_drop(0, -2);           // (it has them)
            // the edits our locks were for are gone with the load: the locks go back (kept, they made the
            // leader revert its own edits to them, and the two differed until the safety net)
            ulock_release_moved("", NULL, NULL, 0);
            for (k = 0; k < g_nslot; k++)
                if (g_slots[k].lockedByMe) { send_lock("lock_release", g_slots[k].id); g_slots[k].lockedByMe = 0; g_slots[k].hasUndo = 0; }
            for (k = 0; k < SC_N; k++) InterlockedExchange(&g_sc[k], it->sc[k]);
            logmsg("STRUCT counts %d / %d / %d (the leader's project)", it->sc[0], it->sc[1], it->sc[2]);
        }
        do_projload(it); free(it->path); break;
    case T_PROJDONE: do_projdone(); break;
    case T_PROJSNAP: do_projsnap(); break;
    case T_CLIPSNAP: do_clipsnap(it); break;
    case T_CLIPS:  do_clips(it); free(it->order); break;
    case T_UNITREVERT: do_unitrevert(it); free(it->path); break;
    case T_HINT:   do_hint(it); free(it->path); break;
    case T_METASNAP: do_metasnap(it); break;
    case T_META:   do_meta(it); free(it->path); break;
    case T_PATOP:  do_patop(it); free(it->path); break;
    case T_RELOADU: do_reloadu(it); free(it->path); break;
    case T_PLUGFOCUS: do_plugfocus(it); break;
    case T_MIXSTRUCT: do_mixstruct(it); free(it->path); break;
    case T_NAMEPROJ: leader_name_project(); break;
    case T_PCOUNT: do_pcount(it); break;
    case T_ARRSTRUCT: do_arrstruct(); break;
    case T_ARROP: do_arrop(it); free(it->path); break;
    }
}

// ---------------- main-thread apply ----------------
// Tasks run from the window procedure of a hidden message-only window on FL's main thread, i.e.
// from ordinary message dispatch like FL's own menu handlers, not from inside the WH_GETMESSAGE
// hook: destroying a plugin while a hook chain is being walked can leave a hook its GUI installed
// (DXGI, for Direct3D editors) pointing into an unloaded DLL, and FL crashes on its next message.
// Until that window exists (early start-up) the hook drains the queue itself.
#define WM_FLIM_TASKS (WM_APP + 0x32)
#define WM_FLIM_SAVEPOPUP (WM_APP + 0x33)   // the host is saving: show the popup (below)
static void save_popup(void);
static volatile LONG g_hookCalls = 0;

// Tasks that replace or delete plugins wait (the queue behind them too, keeping the order) while
//  - a mouse button is down: the user is in the middle of something (a knob drag, a menu), or
//  - code of some plugin is further up this stack: we were dispatched from inside its own message
//    loop (its menu, knob drag or dialog). Replacing or deleting it now returns into freed code:
//    FL's "Execution of address ..." crashes, with the plugin's frames on the stack.
// Only FL itself, Windows and flim may be on the stack.
// Nothing at all runs while FL's own preset / project loader is mid-load (its progress display
// lets messages through, ours included): saving or loading a plugin it is loading deadlocks
// (Slicer 2) or reads a half-built plugin. FL counts its running loads in an int (FLLOADS_VA).
#define BUSY_TIMER    3            // task window timer: try the waiting task again
#define BUSY_RETRY_MS 100
#define FLLOADS_VA    FLVA(FLLOADS) // global -> int: LoadPresetFile calls running (sub_F51FD0 ++ / --)
static int fl_presets_loading(void) {
    __try { return **(int **)(g_flBase + (FLLOADS_VA - IMG_BASE)) > 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
// FL's loader lowers that count only on its normal way out. A file that isn't a preset leaves it raised for
// good: a sample dropped on a channel (LoadPresetFile returns 8, then the drop loads it as a sample), and so
// does an exception in the loader. flim, waiting while FL loads, then waited forever on that FL. The call is
// synchronous, so whatever it leaves above where it started is such a leak.
static int loadpreset_call(presetfile_t fn, const wchar_t *file, int mask, void *obj, int flags) {
    int *n = NULL, at = 0, r;
    __try { n = *(int **)(g_flBase + (FLLOADS_VA - IMG_BASE)); at = *n; } __except (EXCEPTION_EXECUTE_HANDLER) { n = NULL; }
    r = fn(file, mask, obj, flags);
    __try { if (n && *n > at) *n = at; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
static int replaces_plugins(int type) {
    return type == T_LOAD || type == T_ADDCH || type == T_DELCH || type == T_FXCLEAR || type == T_MIXSTRUCT || type == T_PROJLOAD;
}
// Tasks that call into plugins (save their state, or replace them): not while a plugin's own menu, drag or
// dialog is on the stack (main_busy).
static int calls_plugins(int type) {
    return replaces_plugins(type) || type == T_SNAP || type == T_PLUGFOCUS || type == T_RELOADU;
}
// The task FL's main thread is in (pollThread reports one that doesn't come back: main_stuck).
static volatile LONG g_runType = -1, g_runUnit, g_runA, g_runB;
static volatile DWORD g_runAt;
static int module_is_plugin(HMODULE m) {
    static HMODULE seen[64];
    static char verdict[64];
    static int n = 0;
    wchar_t p[MAX_PATH], win[MAX_PATH];
    int i, r;
    if (m == GetModuleHandleW(NULL) || m == (HMODULE)g_flBase || m == (HMODULE)g_hinst) return 0;
    for (i = 0; i < n; i++) if (seen[i] == m) return verdict[i];
    GetSystemWindowsDirectoryW(win, MAX_PATH);
    r = GetModuleFileNameW(m, p, MAX_PATH) && _wcsnicmp(p, win, wcslen(win)) != 0;
    if (n < 64) { seen[n] = m; verdict[n++] = (char)r; }
    return r;
}
// A mouse button held while FL is in front: the user is dragging or in a menu in FL. Held in another program
// it says nothing about FL (it used to hold flim's queue for as long as someone dragged anything anywhere).
static int fl_mouse_down(void) {
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000));
}
static int main_busy(char *why, int cap) {
    void *f[62];
    USHORT n, i;
    if (fl_mouse_down()) { strcpy_s(why, cap, "a mouse button is down"); return 1; }
    n = RtlCaptureStackBackTrace(1, 62, f, NULL);
    for (i = 0; i < n; i++) {
        HMODULE m = NULL;
        wchar_t p[MAX_PATH];
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)f[i], &m) ||
            !module_is_plugin(m)) continue;
        GetModuleFileNameW(m, p, MAX_PATH);
        sprintf_s(why, cap, "%S is running (its menu, drag or dialog)", base_name(p));
        return 1;
    }
    return 0;
}

static void drain_queue_mainthread(void) {
    static DWORD waitSince = 0;
    if (g_patDepth) { if (g_taskWnd) SetTimer(g_taskWnd, BUSY_TIMER, BUSY_RETRY_MS, NULL); return; }   // inside FL's pattern op
    if (InterlockedExchange(&g_inHook, 1)) return;     // FL may pump messages inside a plugin load
    for (;;) {
        QItem it;
        char why[200];
        EnterCriticalSection(&g_qLock);
        if (g_qh == g_qt) { LeaveCriticalSection(&g_qLock); break; }
        it = g_q[g_qh];
        LeaveCriticalSection(&g_qLock);
        if (g_taskWnd && (fl_presets_loading() || fl_loading())) strcpy_s(why, sizeof(why), "FL is loading something");
        else if (!g_taskWnd || g_projBusy || !calls_plugins(it.type) || !main_busy(why, sizeof(why))) why[0] = 0;
        if (why[0]) {
            if (!waitSince) { waitSince = GetTickCount(); logmsg("WAIT flim's queued changes held back: %s", why); }
            SetTimer(g_taskWnd, BUSY_TIMER, BUSY_RETRY_MS, NULL);
            break;
        }
        if (waitSince) { logmsg("WAIT over after %lu ms", GetTickCount() - waitSince); waitSince = 0; }
        EnterCriticalSection(&g_qLock);
        it = g_q[g_qh];                                 // as it is now: enqueue may have put a newer value or state in it
        g_qh = (g_qh + 1) % QCAP;
        LeaveCriticalSection(&g_qLock);
        if (it.peer) InterlockedDecrement(&g_peerQueued);

        if (g_projBusy && it.type != T_PROJLOAD && it.type != T_PROJDONE && it.type != T_PROJSNAP) {
            if (it.type == T_LOAD || it.type == T_ADDCH) { if (it.path) { DeleteFileA(it.path); free(it.path); } }
            else if (it.type == T_UNITREVERT || it.type == T_PROJLOAD || it.type == T_HINT || it.type == T_META || it.type == T_PATOP ||
                     it.type == T_RELOADU || it.type == T_MIXSTRUCT) free(it.path);
            else if (it.type == T_ORDER || it.type == T_FXSYNC || it.type == T_NOTES || it.type == T_NOTESYNC || it.type == T_CLIPS) free(it.order);
            continue;
        }

        g_runType = it.type; g_runUnit = it.type == T_PARAM ? (LONG)it.id : it.unit; g_runA = it.a; g_runB = it.b;
        g_runAt = GetTickCount() | 1;
        if (it.type != T_PARAM) {
            DWORD t0 = GetTickCount();
            run_task(&it);
            g_runAt = 0;
            DWORD dt = GetTickCount() - t0;
            if (dt > 50) logmsg("SLOW main-thread task type=%d unit=%d %d/%d took %lu ms", it.type, it.unit, it.a, it.b, dt);
            // one heavy FL call per tick; come back for the rest
            if (g_qh != g_qt) wake_main();
            break;
        }
        // (params don't go through run_task: the same structure handling as there, sc_rebase included)
        if (it.peer && it.sc[SC_MIX] >= 0 && g_sc[SC_MIX] >= 0 && names_mix_tracks(&it) && mix_layout_changed()) mix_struct();
        if (!sc_rebase(&it)) { g_runAt = 0; continue; }
        Slot *s = slot_by_id(it.id);
        if (s && it.mode == 2 && s->applied != it.v) continue;
        if (s) {
            // Record the value before writing so the poller sees no change, and bump
            // gen so any poll that raced with this write is discarded.
            EnterCriticalSection(&g_slotLock);
            s->last = it.v;
            InterlockedIncrement(&s->gen);
            LeaveCriticalSection(&g_slotLock);
        }
        uint32_t r = write_value(it.id, it.v);
        if (it.mode) {
            // someone else's edit moved this channel's / effect's saved state: not ours to send back (the
            // state checks would, and take the plugin lock from them). They wait for a new baseline; params
            // a plugin links to the one it got aren't local edits either.
            int un, ua, ub;
            Unit *u = unit_of_id(it.id, &un, &ua, &ub) ? unit_ptr(un, ua, ub) : NULL;
            if (u) u->baselineAt = GetTickCount() + REBASE_MS;
            if (u && plug_param_unit(it.id, &un, &ua, &ub)) {
                // params the plugin moved along with it (it does that inside the write): taken as seen. Only
                // those: a time window would also swallow knobs the user here turns meanwhile (each knob has
                // its own lock, so two people can work one plugin)
                int k, n = 0;
                for (k = 0; k < g_nslot; k++) {
                    Slot *o = &g_slots[k];
                    int u2, a2, b2;
                    uint32_t ov;
                    // (a knob the user here is turning (its lock is ours) is theirs. ponytail: a turn that starts in
                    // the 30 ms before an apply can be taken for linked; the drag's next step goes out anyway)
                    if (o == s || o->lockedByMe || !plug_param_unit(o->id, &u2, &a2, &b2) || u2 != un || a2 != ua || b2 != ub) continue;
                    if ((ov = read_value(o->id)) == REC_NONE || ov == o->last) continue;
                    EnterCriticalSection(&g_slotLock);
                    o->last = ov;
                    InterlockedIncrement(&o->gen);
                    LeaveCriticalSection(&g_slotLock);
                    n++;
                }
                if (n) logmsg("APPLY id=0x%08x moved %d linked params of its plugin here: not edits", it.id, n);
            }
        }
        if (s) {
            // what the plugin made of it (its own rounding) isn't a local change either: sent back, it
            // would echo between the peers, or be snapped back forever while the sender holds the lock.
            // Only expected, not taken as the value: some targets (mute) update a moment later.
            uint32_t back = read_value(it.id);
            EnterCriticalSection(&g_slotLock);
            s->echo = back; s->applied = it.v; s->echoAt = GetTickCount();
            InterlockedIncrement(&s->gen);
            LeaveCriticalSection(&g_slotLock);
        }
        g_runAt = 0;
        logmsg("APPLY id=0x%08x v=%u ret=%u", it.id, it.v, r);
    }
    g_runAt = 0;
    InterlockedExchange(&g_inHook, 0);
}

// Poll thread: FL's main thread hasn't come back from one of our tasks for STUCK_MS. Which task, and where
// the thread is (each frame as module+offset, from its unwind data), so a log alone names the plugin or
// lock. The thread is stopped only while its registers and return addresses are read (no allocation,
// no loader lock meanwhile); the names are looked up after it runs again.
#define STUCK_MS 5000
static DWORD g_mainTid;
static void main_stuck(DWORD forMs) {
    DWORD64 pc[24];
    int n = 0, i;
    char line[1600];
    size_t o;
    HANDLE th = g_mainTid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainTid) : NULL;
    logmsg("STUCK FL's main thread has been in a flim task for %lu s: type %ld, unit %ld %ld/%ld (FL isn't responding)",
           forMs / 1000, g_runType, g_runUnit, g_runA, g_runB);
    if (!th) return;
    if (SuspendThread(th) != (DWORD)-1) {
        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &c)) {
            __try {
                for (n = 0; n < 24 && c.Rip; n++) {
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION f;
                    pc[n] = c.Rip;
                    f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                    if (!f) { c.Rip = *(DWORD64 *)c.Rsp; c.Rsp += 8; }   // a leaf: its return address is on top
                    else {
                        PVOID hd;
                        DWORD64 ef;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &hd, &ef, NULL);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        ResumeThread(th);
    }
    CloseHandle(th);
    o = (size_t)sprintf_s(line, sizeof(line), "STUCK main thread:");
    for (i = 0; i < n && o < sizeof(line) - 120; i++) {
        HMODULE m = NULL;
        wchar_t p[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)pc[i], &m) && m)
            GetModuleFileNameW(m, p, MAX_PATH);
        o += (size_t)sprintf_s(line + o, sizeof(line) - o, " %S+%llx", base_name(p), (unsigned long long)(pc[i] - (DWORD64)m));
    }
    logmsg("%s", line);
}

static LRESULT CALLBACK task_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_FLIM_TASKS) { drain_queue_mainthread(); return 0; }
    if (m == WM_TIMER && wp == CUR_TIMER) { cursor_update(); sel_update(); return 0; }
    if (m == WM_TIMER && wp == BUSY_TIMER) { KillTimer(h, BUSY_TIMER); drain_queue_mainthread(); return 0; }
    if (m == WM_TIMER && wp == BACKUP_TIMER) { backup_tick(); return 0; }
    if (m == WM_TIMER && wp == ALIVE_TIMER) {
        if (g_isLeader && g_netState == NET_ON) ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"alive\"}}");
        return 0;
    }
    if (m == WM_FLIM_SAVEPOPUP) { save_popup(); return 0; }
    if (m == WM_COPYDATA) { gui_command((HWND)wp, (const COPYDATASTRUCT *)lp); return TRUE; }
    return DefWindowProcW(h, m, wp, lp);
}
// Main thread.
static void task_window_create(void) {
    WNDCLASSW wc;
    if (g_taskWnd) return;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = task_proc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"FlimTasks";
    RegisterClassW(&wc);
    g_taskWnd = CreateWindowExW(0, L"FlimTasks", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hinst, NULL);
    logmsg("task window %p", (void *)g_taskWnd);
    if (g_qh != g_qt) wake_main();
}

// ---------------- the host is saving ----------------
// While the leader's FL writes the project (Save, Save as, autosave), nobody else may change it: a
// change made meanwhile would be missing from the saved file. The leader's engine wraps FL's
// project writer (SAVEPROJ_VA; flim's own snapshots excluded) and tells the others "saving" /
// "saved". Meanwhile their FLs swallow clicks and key presses and show a small popup that closes by
// itself once the save is done (or after HOSTSAVE_MAX_MS, if the leader went away mid-save).
#define HOSTSAVE_MAX_MS   30000
static volatile DWORD g_hostSavingUntil = 0;     // member: the leader is saving (0 = not)
static char  g_hostSaver[64];                    // its id
static saveproj_t g_saveprojOrig;                // FL's writer, through the trampoline
static HWND  g_savePopup;

static int host_saving(void) {
    DWORD u = g_hostSavingUntil;
    return u && (LONG)(GetTickCount() - u) < 0;
}
// Clicks and key presses don't reach FL now: the host saves, or the leader's project is loading here.
static int input_held(void) { return host_saving() || member_reloading(); }
// ... except in a dialog FL shows meanwhile (a message box, a missing plugin's warning): it needs its answer.
static int input_to_dialog(HWND h) {
    char c[32];
    HWND root = h ? GetAncestor(h, GA_ROOT) : NULL;
    if (!root || !GetClassNameA(root, c, sizeof(c))) return 0;
    return strcmp(c, "TMsgForm") == 0 || strcmp(c, "#32770") == 0 || strcmp(c, "TNameEditForm") == 0;
}
static char save_detour(const wchar_t *path, uint8_t flag) {
    int tell = !g_ownSave && g_isLeader && g_netState == NET_ON;
    char r;
    if (tell) ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"saving\",\"on\":1}}");
    r = g_saveprojOrig(path, flag);
    if (tell) { ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"saving\",\"on\":0}}"); logmsg("PROJECT saved (the others were held off meanwhile)"); }
    return r;
}
// FL's function fn starts with the n bytes pro (n >= 12, whole position-independent instructions):
// they move to a trampoline that runs the original, and a jump to target takes their place.
// Returns the trampoline (NULL: fn looks different, left alone).
static void *detour_tramp(unsigned char *fn, const unsigned char *pro, int n, void *target) {
    unsigned char jmp[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 };     // mov rax, imm64; jmp rax
    unsigned char *t, *back = fn + n;
    DWORD old;
    if (memcmp(fn, pro, (size_t)n) != 0) return NULL;
    if (!(t = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) return NULL;
    memcpy(t, fn, (size_t)n);
    memcpy(jmp + 2, &back, 8);
    memcpy(t + n, jmp, 12);
    memcpy(jmp + 2, &target, 8);
    if (!VirtualProtect(fn, 12, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    memcpy(fn, jmp, 12);
    VirtualProtect(fn, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 12);
    return t;
}
// Once FL is up: FL's project writer goes through save_detour.
static void save_detour_install(void) {
    static const unsigned char PRO[12] = { 0x55, 0x53, 0x48, 0x81, 0xEC, 0x98, 0, 0, 0, 0x48, 0x8B, 0xEC };   // push rbp/rbx; sub rsp,98h; mov rbp,rsp
    g_saveprojOrig = (saveproj_t)detour_tramp((unsigned char *)(g_flBase + (SAVEPROJ_VA - IMG_BASE)), PRO, 12, (void *)save_detour);
    if (!g_saveprojOrig) logmsg("SAVE: FL's project writer looks different: saves aren't announced");
}

// FL deletes a channel (its menu, undo...): note which. FL may give that memory to the next channel it
// makes, and a delete and an add between two structure checks would look like one channel moving.
static delchan_t g_delchanOrig;
static void delchan_detour(unsigned idx, char force, uint64_t flags) {
    void **items;
    int n = chan_list(&items);
    if (items && idx < (unsigned)n && g_nChDead < CH_DEAD_MAX) g_chDead[g_nChDead++] = items[idx];
    g_delchanOrig(idx, force, flags);
}
static void delchan_detour_install(void) {
    static const unsigned char PRO[14] = { 0x55, 0x57, 0x56, 0x53, 0x48, 0x81, 0xEC, 0x88, 0, 0, 0, 0x48, 0x8B, 0xEC };   // push rbp/rdi/rsi/rbx; sub rsp,88h; mov rbp,rsp
    g_delchanOrig = (delchan_t)detour_tramp((unsigned char *)(g_flBase + (DELCHAN_VA - IMG_BASE)), PRO, 14, (void *)delchan_detour);
    if (g_delchanOrig) g_delChannel = g_delchanOrig;      // a peer's delete (do_delch) isn't one of ours
    else logmsg("CHAN: FL's channel delete looks different: a delete and an add right after it may sync as a move");
}

// FL's channel solo toggle (the mute button's solo click, the menu, scripts): see CH_PRESOLO.
typedef uint64_t (*chsolo_t)(void *ch, char flags);
static chsolo_t g_chsoloOrig;
static uint64_t chsolo_detour(void *ch, char flags) {
    uint64_t r;
    g_chSoloAt = GetTickCount();
    r = g_chsoloOrig(ch, flags);
    InterlockedExchange(&g_chSolo, chsolo_shape());
    g_chSoloAt = GetTickCount();
    return r;
}
static void chsolo_detour_install(void) {
    static const unsigned char PRO[12] = { 0x41, 0x55, 0x57, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCB };   // push r13/rdi/rsi/rbx; sub rsp,28h; mov rbx,rcx
    g_chsoloOrig = (chsolo_t)detour_tramp((unsigned char *)(g_flBase + (FLVA(CHSOLO) - IMG_BASE)), PRO, 12, (void *)chsolo_detour);
    if (!g_chsoloOrig) logmsg("SOLO: FL's channel solo looks different: a channel rack solo syncs as mutes");
}

// FL loads a preset into a channel or effect slot (its browser, a plugin's preset menu, a drop): that
// unit's state goes out once it settles, as after a preset-sized burst of knobs. A preset that moves no
// knob (another soundfont, sample or wavetable) waited for the round-robin check, a minute in a big
// project. A channel FL is making (not in the known list yet) goes out as a new channel anyway.
static presetfile_t g_loadPresetOrig;
static int loadpreset_detour(const wchar_t *file, int mask, void *obj, int flags) {
    int r = loadpreset_call(g_loadPresetOrig, file, mask, obj, flags), i, t, s, n = mix_count();
    Unit *u = NULL;
    for (i = 0; i < g_nChKnown && !u; i++) if (g_chKnown[i] == obj) u = &g_chU[i];
    for (t = 0; t < n && t < MAX_MIX && !u; t++)
        for (s = 0; s < MAX_FX_SLOTS && !u; s++) if (fx_obj(t, s) == obj) u = &g_fxU[t][s];
    if (u && mask) { u->lastAct = u->userAt = GetTickCount(); u->burst = 1; }
    return r;
}
static void loadpreset_detour_install(void) {
    static const unsigned char PRO[16] = { 0x55, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xEC, 0x48, 0xC7, 0x45, 0x30, 0, 0, 0, 0 };   // push rbp; sub rsp,60h; mov rbp,rsp; mov [rbp+30h],0
    g_loadPresetOrig = (presetfile_t)detour_tramp((unsigned char *)(g_flBase + (LOADPRESET_VA - IMG_BASE)), PRO, 16, (void *)loadpreset_detour);
    if (g_loadPresetOrig) g_loadPreset = g_loadPresetOrig;   // flim's own loads (someone else's states) aren't edits
    else logmsg("PLUG: FL's preset loader looks different: a preset loaded here that moves no knob syncs later");
}
static LRESULT CALLBACK savepop_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_TIMER) { if (!input_held()) DestroyWindow(h); else InvalidateRect(h, NULL, TRUE); return 0; }
    if (m == WM_DESTROY) { g_savePopup = NULL; return 0; }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        RECT r;
        wchar_t t[160];
        char who[64];
        HDC dc = BeginPaint(h, &ps);
        peer_name(g_hostSaver, who, sizeof(who));
        if (host_saving()) swprintf_s(t, 160, L"%S is saving the project. One moment, then try again.", who);
        else wcscpy_s(t, 160, L"Loading the session leader's project. One moment, then try again.");
        GetClientRect(h, &r);
        FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
// Main thread: someone tried to change something while the host saves / the leader's project loads.
static void save_popup(void) {
    static ATOM cls = 0;
    RECT f;
    int w, hgt;
    if (g_savePopup || !input_held()) return;
    if (!cls) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = savepop_proc; wc.hInstance = g_hinst; wc.lpszClassName = L"FlimSaving";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_WAIT);
        cls = RegisterClassW(&wc);
    }
    GetWindowRect(g_mainForm, &f);
    {
        HDC sdc = GetDC(NULL);
        w = MulDiv(380, GetDeviceCaps(sdc, LOGPIXELSX), 96); hgt = w / 6;
        ReleaseDC(NULL, sdc);
    }
    g_savePopup = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"FlimSaving", L"flim", WS_POPUP | WS_BORDER,
                                  (f.left + f.right - w) / 2, (f.top + f.bottom - hgt) / 2, w, hgt, g_mainForm, NULL, g_hinst, NULL);
    if (!g_savePopup) return;
    ShowWindow(g_savePopup, SW_SHOWNOACTIVATE);
    SetTimer(g_savePopup, 1, 200, NULL);
    logmsg("PROJECT %s: held off a change here", host_saving() ? "the host is saving" : "the leader's project is loading");
}

static LRESULT CALLBACK getMsgHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && lp) {
        MSG *msg = (MSG *)lp;
        UINT k = msg->message;
        if ((k == WM_LBUTTONDOWN || k == WM_RBUTTONDOWN || k == WM_MBUTTONDOWN || k == WM_LBUTTONDBLCLK || k == WM_RBUTTONDBLCLK ||
             k == WM_MOUSEWHEEL || k == WM_KEYDOWN || k == WM_SYSKEYDOWN || k == WM_CHAR) && input_held() &&
            !input_to_dialog(msg->hwnd)) {
            msg->message = WM_NULL;                       // the host is saving / the leader's project loads: never reaches FL
            if (g_taskWnd) PostMessageW(g_taskWnd, WM_FLIM_SAVEPOPUP, 0, 0);
        }
        switch (msg->message) {
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_KEYDOWN:
            g_focusHwnd = msg->hwnd;
            g_lastInput = GetTickCount();
            plugin_input(msg->hwnd, 1);
            break;
        case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MOUSEWHEEL: case WM_KEYUP:
        case WM_SYSKEYDOWN: case WM_LBUTTONDBLCLK:
            g_lastInput = GetTickCount();
            plugin_input(msg->hwnd, 0);
            break;
        case WM_MOUSEMOVE:
            if (msg->wParam & (MK_LBUTTON | MK_RBUTTON)) { g_lastInput = GetTickCount(); plugin_input(msg->hwnd, 0); }   // drags
            break;
        case WM_FL_OPENPROJ:
            if (msg->hwnd != g_mainForm) break;
            // a big project keeps FL's main thread (and our "alive") busy for a while: not a frozen leader
            if (g_isLeader && g_netState == NET_ON) ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"alive\",\"loading\":1}}");
            proj_begin("FL is opening a project");
            break;
        case WM_QUIT:
            g_closingUntil = GetTickCount() + 30000;
            proj_begin("FL is quitting");
            break;
        }
    }
    g_lastHookTick = GetTickCount();
    InterlockedIncrement(&g_hookCalls);
    if (g_mbAuto) patop_dialogs();
    if (code >= 0) {
        if (g_wguardUntil && GetTickCount() - g_wguardLast >= 30) wguard_pass();
        if (g_uiWanted && InterlockedExchange(&g_uiWanted, 0)) { task_window_create(); ui_start(); }
        if (!g_taskWnd) drain_queue_mainthread();
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

// Sent messages: closing FL (or Windows logoff) must never be broadcast as edits.
static HHOOK g_hookCwp = NULL;
static LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && lp) {
        const CWPSTRUCT *c = (const CWPSTRUCT *)lp;
        if ((c->message == WM_CLOSE && (c->hwnd == g_mainForm || c->hwnd == g_mainWnd)) ||
            c->message == WM_QUERYENDSESSION || c->message == WM_ENDSESSION) {
            if (!closing_now()) logmsg("FL is closing (msg 0x%x): structural changes will not be sent", c->message);
            g_closingUntil = GetTickCount() + 30000;
            proj_begin("FL is closing");
        }
    }
    return CallNextHookEx(g_hookCwp, code, wp, lp);
}

static BOOL CALLBACK findMainWnd(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK findMainForm(HWND h, LPARAM lp) {
    DWORD pid = 0;
    char c[64];
    GetWindowThreadProcessId(h, &pid);
    GetClassNameA(h, c, sizeof(c));
    if (pid == GetCurrentProcessId() && strcmp(c, "TFruityLoopsMainForm") == 0) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static int install_mainthread_hook(void) {
    HWND h = NULL;
    int tries;
    for (tries = 0; tries < 600 && !h; tries++) { EnumWindows(findMainWnd, (LPARAM)&h); if (!h) Sleep(100); }
    if (!h) { logmsg("main window not found"); return -1; }
    g_mainWnd = h;
    DWORD tid = GetWindowThreadProcessId(h, NULL);
    g_mainTid = tid;
    g_hook = SetWindowsHookExW(WH_GETMESSAGE, getMsgHook, g_hinst, tid);
    g_hookCwp = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, g_hinst, tid);
    char cls[128] = { 0 }; GetClassNameA(h, cls, sizeof(cls));
    logmsg("main window %p class=%s tid=%lu hook=%p", (void *)h, cls, tid, (void *)g_hook);
    return g_hook ? 0 : -1;
}

// ---------------- detect ----------------
static void send_lock(const char *t, uint32_t id) {
    char m[96];
    sprintf_s(m, sizeof(m), "{\"t\":\"%s\",\"key\":\"param:%u\"}", t, id);
    ws_send(m);
}

// Which "container" an id belongs to (a channel, a mixer track, one effect slot).
// Used to tell a project load (many containers change at once) from a knob
// tweak or a preset change (one container).
static uint32_t container_of(uint32_t id) {
    if (IS_MUTE_ID(id)) return 0x20000u | (id & 0xFFFF);
    if (id >= 0x70000000u) return 0x10000u | ((id - 0x70000000u) >> 16);
    return id >> 16;
}

#define BULK_MIN_CHANGES    24
#define BULK_MIN_CONTAINERS 4
#define PARAM_SEND_MS       60      // one param.set per knob per this interval; the newest value always goes out

// Someone else holds the plugin's lock (they are loading a preset / sending its whole state).
static int plug_held_by_peer(int unit, int a, int b) {
    char key[32];
    int r;
    plug_key(unit, a, b, key, sizeof(key));
    EnterCriticalSection(&g_ulockLock);
    ULock *l = ulock_get(key, 0);
    r = l && l->peer;
    LeaveCriticalSection(&g_ulockLock);
    return r;
}
// Poll thread. A value that keeps changing (a knob dragged, the tempo scrolled) goes out at most once per
// PARAM_SEND_MS: the steps between are dropped, never the newest value.
static void param_send(Slot *s, const char *key, uint32_t v, DWORD now) {
    char op[160];
    sprintf_s(op, sizeof(op), "{\"t\":\"op\",\"op\":{\"kind\":\"param.set\",\"key\":\"%s\",\"id\":%u,\"v\":%u}}", key, s->id, v);
    ws_send(op);
    s->lastSent = now;
    s->sendPending = 0;
}

typedef struct { Slot *s; uint32_t prev, v, id; LONG gen; int self; } Change;
static Change g_changes[MAX_SLOTS];

// A param change is an edit only if someone here is using FL: values that move by themselves
// (automation playback, a Patcher preset's own modulation, controllers linked inside FL) happen
// the same way on every PC, and syncing them makes the peers fight over them under the locks.
// A param that moved by itself stays FL's until it has been still for SELF_MS.
#define EDIT_INPUT_MS 1500
#define SELF_MS       3000
// Local input lately: our hook's, or into a window of another process inside FL (a bridged
// plugin's editor: its input never passes our hook).
// A MIDI controller's knob, fader or button (control change, program change, pitch bend) is someone using FL too:
// FL's MIDI input callback (sub_BA2180, winmm's CALLBACK_FUNCTION, midiin_detour) notes when one last came. Notes,
// clock and other system messages don't count (a keyboard played or a clock running isn't an edit).
static volatile DWORD g_lastMidiIn;
typedef void (CALLBACK *midiincb_t)(void *h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2);
static midiincb_t g_midiInOrig;
static void CALLBACK midiin_detour(void *h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2) {
    unsigned st = (unsigned)(p1 & 0xF0);
    if (msg == 0x3C3 /* MIM_DATA */ && (st == 0xB0 || st == 0xC0 || st == 0xE0)) g_lastMidiIn = GetTickCount() | 1;
    g_midiInOrig(h, msg, inst, p1, p2);
}
static void midiin_detour_install(void) {
    static const unsigned char PRO[15] = { 0x55, 0x41, 0x56, 0x41, 0x55, 0x57, 0x56, 0x53, 0x48, 0x83, 0xEC, 0x48, 0x48, 0x8B, 0xEC };   // push rbp/r14/r13/rdi/rsi/rbx; sub rsp,48h; mov rbp,rsp
    g_midiInOrig = (midiincb_t)detour_tramp((unsigned char *)(g_flBase + (FLVA(MIDIINCB) - IMG_BASE)), PRO, 15, (void *)midiin_detour);
    if (!g_midiInOrig) logmsg("MIDI: FL's MIDI input looks different: changes made with a MIDI controller alone don't sync");
}
static int input_recent(DWORD now) {
    LASTINPUTINFO li;
    POINT pt;
    HWND h;
    DWORD pid = 0, root = 0;
    if (g_lastInput && now - g_lastInput < EDIT_INPUT_MS) return 1;
    if (g_lastMidiIn && (LONG)(now - g_lastMidiIn) < EDIT_INPUT_MS) return 1;   // (signed: it's now | 1, set on another thread)
    li.cbSize = sizeof(li);
    if (!GetLastInputInfo(&li) || now - li.dwTime >= EDIT_INPUT_MS || !GetCursorPos(&pt) || !(h = WindowFromPoint(pt))) return 0;
    GetWindowThreadProcessId(h, &pid);
    GetWindowThreadProcessId(GetAncestor(h, GA_ROOT), &root);
    return pid != GetCurrentProcessId() && root == GetCurrentProcessId();
}

// True if FL's channel list no longer matches what the main thread last recorded.
static int chan_list_changed(void) {
    void **items; int n = chan_list(&items), i;
    if (g_nChKnown < 0 || !items) return 0;
    if (n != g_nChKnown || g_nChDead) return 1;          // (dead: a delete whose memory a new channel may have)
    for (i = 0; i < n; i++) if (items[i] != g_chKnown[i]) return 1;
    return 0;
}

// Decide when units need a state snapshot (runs on the poll thread; the
// snapshots themselves are queued for the main thread).
static void schedule_units(DWORD now) {
    static DWORD lastSweep = 0, lastStruct = 0;
    static int sweepPos = 0;
    int i, t, s, nch = g_nChKnown, nm = mix_count();
    if (now - lastStruct >= STRUCT_MS) { enqueue_unit(T_STRUCT, 0, 0, 0, 0, NULL); lastStruct = now; }
    if (nch < 0) return;
    for (i = 0; i < nch + nm * MAX_FX_SLOTS; i++) {
        int unit = i < nch ? U_CH : U_FX;
        int a = i < nch ? i : (i - nch) / MAX_FX_SLOTS, b = i < nch ? 0 : (i - nch) % MAX_FX_SLOTS;
        Unit *u = unit_ptr(unit, a, b);
        if (!u) continue;
        if (unit == U_FX && !u->inst && !u->identAt) continue;
        if (u->identAt && now - u->identAt > SETTLE_IDENT_MS) {
            u->identAt = 0; u->lastAct = 0;
            enqueue_unit(T_SNAP, unit, a, b, SNAP_FORCE, NULL);
        } else if (!u->identAt && u->lastAct && now - u->lastAct > SETTLE_ACT_MS) {
            // a preset-sized burst may carry state params don't cover; plain knob
            // tweaks only move the baseline (a full reload on the peer would glitch audio)
            enqueue_unit(T_SNAP, unit, a, b, u->burst ? SNAP_IF_DIFF : SNAP_BASELINE, NULL);
            u->lastAct = 0; u->burst = 0;
        } else if (u->baselineAt && (LONG)(now - u->baselineAt) >= 0) {
            u->baselineAt = 0;
            enqueue_unit(T_SNAP, unit, a, b, SNAP_BASELINE, NULL);
        }
    }
    // Round-robin sweep: catches state changes that touch no params (e.g. Serum wavetables).
    if (now - lastSweep >= SWEEP_MS && g_lastInput && now - g_lastInput < INPUT_RECENT_MS) {
        int total = nch + nm * MAX_FX_SLOTS, tries;
        lastSweep = now;
        for (tries = 0; tries < total; tries++) {
            int k = (sweepPos++) % (total ? total : 1);
            int unit = k < nch ? U_CH : U_FX;
            int a = k < nch ? k : (k - nch) / MAX_FX_SLOTS, b = k < nch ? 0 : (k - nch) % MAX_FX_SLOTS;
            Unit *u = unit_ptr(unit, a, b);
            if (!u || (unit == U_FX && !u->inst)) continue;
            if (u->lastAct || u->identAt || u->baselineAt || now < u->suppressUntil) continue;
            // a plugin nobody worked in here can only have drifted: its turn passes without a look (a look
            // would take the drift as its baseline, and with it an edit whose window check hasn't run yet)
            if (!u->hash || unit_touched(u)) enqueue_unit(T_SNAP, unit, a, b, u->hash ? SNAP_IF_DIFF : SNAP_BASELINE, NULL);
            break;
        }
    }
    (void)t; (void)s;
}

// FL's solo (un)mutes the tracks one by one around flagging the soloed one, so a pass can land
// mid-way: a mute change counts once two passes in a row read it, and not while solo is changing.
// On unsolo the tracks go back to the mute states everyone shares (FL may unmute them all instead,
// or leave the rest muted when a mute click ended the solo).
#define SOLO_HOLD_MS    1500
#define SOLO_RESTORE_MS 800                // after an unsolo: FL's own fades back are over by then (MUTE_FADE_MS)
// While audio runs, FL mutes a track with a short fade: the track still reads enabled until the fade
// ends, and an unmute meanwhile changes nothing, so the fade finishes muted (FL's sub_12C1810). A
// mute that ends up against flim's last apply this soon after it is put back, not taken for an edit.
// Later, any change is someone's click: a mute click goes back to the value flim applied all the time.
#define MUTE_FADE_MS 500
// A mute change goes out once it has read the same for MUTE_SETTLE_MS: FL's solo click mutes the track on the mouse
// button going down and solos on it coming up (~0.2 s later), and that mute went out before the solo was seen (a
// tester's "mixer solos get synced", 2 of 54 solos). A solo starting meanwhile takes it as its own: the track's
// mute from before it (MIX_PRESOLO) gets the shared one back.
#define MUTE_SETTLE_MS 400
static unsigned char g_mutePend[MAX_MIX];  // a track's new mute reading + 1, until it has settled
static DWORD g_mutePendAt[MAX_MIX];        // since when it reads that
static DWORD g_soloChangedAt;              // when solo last went on or off here
// Playing is each person's own: flim never starts or stops FL. A tester saw someone's space key start others'
// FLs "sometimes" (not reproduced): FL starting or stopping is logged with whether anyone was at the keyboard
// and what came from the others just before, so the next time shows what did it.
static char g_lastInKind[24];
static volatile DWORD g_lastInAt;
static void play_poll(DWORD now) {
    static int was = -1;
    int on = fl_playing();
    if (on == was) return;
    if (was >= 0) logmsg("PLAY %s here (input here: %s; last from the others: %s %lu ms before)", on ? "started" : "stopped",
                        input_recent(now) ? "yes" : "no", g_lastInAt ? g_lastInKind : "nothing", g_lastInAt ? now - g_lastInAt : 0);
    was = on;
}
// The channel rack's solo: ended by a click (that channel back on: no longer FL's shape) as well as by the toggle.
// Afterwards the channels go back to the on/off everyone shares (FL may turn them all on instead, or a click
// that ended it left the rest off).
static void chsolo_poll(DWORD now) {
    static LONG was;
    static DWORD restoreAt;
    int i, n = g_nslot;
    if (g_chSolo && now - g_chSoloAt >= SOLO_HOLD_MS && !chsolo_shape()) { InterlockedExchange(&g_chSolo, 0); g_chSoloAt = now; }
    if (restoreAt && (LONG)(now - restoreAt) >= 0) {
        restoreAt = 0;
        if (!g_chSolo)
            for (i = 0; i < n; i++) {
                Slot *s = &g_slots[i];
                if (IS_CH_MUTE(s->id) && s->last != REC_NONE && g_rec(s->id, 0, REC_GetValue) != s->last) enqueue_apply(s->id, s->last, 0);
            }
    }
    if (g_chSolo == was) return;
    logmsg("SOLO %s in the channel rack here (each person's own: not synced)", g_chSolo ? "on" : "off");
    restoreAt = was ? (now + SOLO_RESTORE_MS) | 1 : 0;
    was = g_chSolo;
}
static void solo_poll(DWORD now) {
    static int was = -1;
    static DWORD restoreAt;                // an unsolo's put-back is due (0 = none)
    int on = solo_scan(), i, n = g_nslot;
    if (restoreAt && (LONG)(now - restoreAt) >= 0) {
        // put back once FL's fades from the unsolo are over: done at once, it read tracks mid-fade and its applies
        // landed inside FL's fades (an unmute then does nothing), leaving tracks muted; the next click sent that
        restoreAt = 0;
        if (!on)
            for (i = 0; i < n; i++) {
                Slot *s = &g_slots[i];
                int t = (int)(s->id & 0xFFFFF);
                if (IS_MUTE_ID(s->id) && t < mix_count() && s->last != REC_NONE && mute_get(t, 0) != s->last) enqueue_apply(s->id, s->last, 0);
            }
    }
    chsolo_poll(now);
    play_poll(now);
    if (on == was) return;
    if (was >= 0) logmsg("SOLO %s here (each person's own: not synced)", on ? "on" : "off");
    if (on == 1)                           // mutes not out yet: part of the solo click (applied now: into MIX_PRESOLO)
        for (i = 0; i < n; i++) {
            Slot *s = &g_slots[i];
            int t = (int)(s->id & 0xFFFFF);
            if (IS_MUTE_ID(s->id) && t < MAX_MIX && g_mutePend[t] && s->last != REC_NONE) {
                g_mutePend[t] = 0;
                enqueue_apply(s->id, s->last, 0);
                logmsg("MUTE track %d changed just before a solo here: part of it, not synced", t);
            }
        }
    restoreAt = was == 1 ? (now + SOLO_RESTORE_MS) | 1 : 0;   // (soloed again meanwhile: its own unsolo puts back)
    was = on;
    g_soloChangedAt = now;
}
static int mute_settled(Slot *s, uint32_t v, DWORD now) {
    int t = (int)(s->id & 0xFFFFF);
    if (t >= MAX_MIX) return 1;
    if (v == s->last || v == REC_NONE || s->last == REC_NONE) { g_mutePend[t] = 0; return 1; }
    if (now - g_soloChangedAt < SOLO_HOLD_MS) return 0;
    if (g_mutePend[t] != v + 1) { g_mutePend[t] = (unsigned char)(v + 1); g_mutePendAt[t] = now; return 0; }
    if (now - g_mutePendAt[t] < MUTE_SETTLE_MS) return 0;
    g_mutePend[t] = 0;
    return 1;
}

static DWORD WINAPI pollThread(LPVOID p) {
    (void)p;
    DWORD lastScan = GetTickCount(), lastBeat = GetTickCount();
    uint64_t lastMixH = 0;
    for (;;) {
        DWORD now = GetTickCount(), at = g_runAt;
        static DWORD stuckSaid;
        if (g_inHook && at && (LONG)(now - at) > STUCK_MS && at != stuckSaid) { stuckSaid = at; main_stuck(now - at); }
        if (now - lastBeat > 30000) {
            int depth = (g_qt - g_qh + QCAP) % QCAP;
            logmsg("BEAT queue=%d inHook=%ld hookCalls=%ld chanChanged=%d known=%d", depth, g_inHook,
                   InterlockedExchange(&g_hookCalls, 0), chan_list_changed(), g_nChKnown);
            lastBeat = now;
        }
        int i, n, nc = 0, active = -1;            // active: input_recent(), once a change needs it
        LONG gen0 = g_structGen;
        ulock_release_idle(now);
        if (!g_isLeader && g_netState == NET_ON && g_leaderAliveAt && (LONG)(now - g_leaderAliveAt) > LEADER_STUCK_MS &&
            !InterlockedExchange(&g_leaderStuck, 1)) {      // (signed: a ping can land after `now` was taken)
            char who[64] = "The session leader", a[400];
            const char *q = strstr(g_peersJson, "\"role\":\"leader\""), *id = q;
            while (id && id > g_peersJson && strncmp(id, "{\"id\":\"", 7) != 0) id--;
            if (id && strncmp(id, "{\"id\":\"", 7) == 0) {
                char lid[64];
                int i;
                for (i = 0; id[7 + i] && id[7 + i] != '"' && i < 63; i++) lid[i] = id[7 + i];
                lid[i] = 0;
                peer_name(lid, who, sizeof(who));
            }
            logmsg("PROJECT %s's FL (the session leader) hasn't responded for %lu s", who, (now - g_leaderAliveAt) / 1000);
            sprintf_s(a, sizeof(a), "%s's FL (the session leader) hasn't responded for %lu seconds: it may have frozen.\n\n"
                      "Save your work now (Ctrl+S saves your own copy of the song). If it comes back, carry on as usual.",
                      who, (now - g_leaderAliveAt) / 1000);
            gui_push(GUI_ALERT, a);
        }
        if (g_projBusy) { proj_poll(now); Sleep(POLL_MS); continue; }
        // Reading a plugin's params while FL's loader replaces it calls into a half-built (or freed)
        // plugin: Slicer 2 then deadlocks its next load, others crash. Not while FL loads anything.
        if (fl_presets_loading() || fl_loading()) { Sleep(POLL_MS); continue; }
        if (chan_list_changed() || mix_layout_changed()) {
            // A channel or mixer track was added, removed or moved and the main thread hasn't processed
            // it yet: REC ids of later channels / tracks have shifted, so diffs would be garbage.
            // The structure check runs now, not at its next turn (a delete and an add between two
            // checks are harder to tell apart, and the others should hear of it before anything else).
            if (!InterlockedExchange(&g_structQueued, 1)) enqueue_unit(T_STRUCT, 0, 0, 0, 0, NULL);
            schedule_units(now);
            Sleep(POLL_MS);
            continue;
        }
        // (not while FL's main thread is in one of our tasks: it may be saving or loading the very plugin the
        // rescan or the param reads below would call into; a plugin locked that way once deadlocked FL)
        if (!g_inHook && (now - lastScan > RESCAN_MS || InterlockedExchange(&g_rescanNow, 0))) { rescan(); retry_pending(); lastScan = now; }
        reload_poll();
        plugfocus_poll(now);
        scan_notes(now);
        scan_clips(now);
        arr_poll(now);
        scan_meta(now);
        state_digest(now);
        digest_tick(now);
        resync_tick(now);
        focus_poll(now);
        cursor_poll(now);
        sel_poll(now);
        solo_poll(now);
        uint64_t mixH = mix_ids_hash();
        int mixSteady = mixH == lastMixH;              // no track moved since the last pass
        lastMixH = mixH;
        links_scan(now, mixSteady);
        n = g_nslot;
        for (i = 0; i < n; i++) {
            Slot *s = &g_slots[i];
            LONG g0 = s->gen;
            uint32_t v;
            int pu, pa, pb;
            if (fl_presets_loading()) break;           // a load began meanwhile: the rest next tick
            if (g_inHook && plug_param_unit(s->id, &pu, &pa, &pb)) continue;   // a plugin's: after our task (above)
            v = read_value(s->id);
            if (s->gen != g0) continue;                 // raced with an apply; re-read next tick
            if (IS_MUTE_ID(s->id) && mixSteady && v != REC_NONE && v != s->last && v != s->applied && v != s->echo &&
                s->echoAt && now - s->echoAt < MUTE_FADE_MS) {
                // FL's fade swallowed our last apply: once more (if that fails too, what FL shows is our echo)
                EnterCriticalSection(&g_slotLock);
                s->echo = v;
                LeaveCriticalSection(&g_slotLock);
                enqueue_apply(s->id, s->applied, 2);
                logmsg("MUTE track %u ended up %s after flim's apply: applying it again", s->id & 0xFFFFF, v ? "on" : "muted");
                continue;
            }
            if (IS_MUTE_ID(s->id) && !mute_settled(s, v, now)) continue;
            if (IS_CH_MUTE(s->id) && now - g_chSoloAt < SOLO_HOLD_MS) continue;   // a channel solo going on / off
            // A change becomes "seen" (last) only once this tick turns out clean (below): a structure change
            // mid-tick renumbers the slot, and a value taken as seen there was never sent.
            uint32_t prev = s->last;
            if (v != prev && (v == REC_NONE || prev == REC_NONE)) slot_take(s, g0, v);
            if (v != prev && v != REC_NONE && prev != REC_NONE) {
                int unit, a, b;
                Unit *u;
                // a clip / controller drives it: each FL plays its own; moved by hand while stopped, it goes out
                if (auto_target(s->id) && (fl_playing() || !(active < 0 ? (active = input_recent(now)) : active))) {
                    static DWORD said = 0;
                    if (now - said > 10000) { said = now; logmsg("PARAM 0x%08x is automated and moved by FL: each FL plays its own, not synced", s->id); }
                    slot_take(s, g0, v);
                    continue;
                }
                u = unit_of_id(s->id, &unit, &a, &b) ? unit_ptr(unit, a, b) : NULL;
                if (u && now < u->suppressUntil) { slot_take(s, g0, v); continue; }   // caused by a remote state load
                // its plugin is being replaced here (the structure scan may not have seen it yet): the old
                // plugin's params "changing" aren't edits, the new one's whole state goes out (SNAP_FORCE)
                if (u && (u->identAt || plugin_inst(unit_obj(unit, a, b)) != u->inst)) { slot_take(s, g0, v); continue; }
                if ((v == s->echo || v == s->applied) && now - s->echoAt < (DWORD)(IS_MUTE_ID(s->id) ? MUTE_FADE_MS : ECHO_MS)) { slot_take(s, g0, v); continue; }   // our apply (rounded, or late)
                if (active < 0) active = input_recent(now);
                g_changes[nc].s = s; g_changes[nc].prev = prev; g_changes[nc].v = v;
                g_changes[nc].id = s->id; g_changes[nc].gen = g0;
                g_changes[nc].self = !active || now - s->selfAt < SELF_MS;
                if (g_changes[nc].self) s->selfAt = now;
                nc++;
            }
            if (s->lockedByMe && now - s->lastTouch > LOCK_IDLE_MS) {
                send_lock("lock_release", s->id);
                s->lockedByMe = 0;
            }
        }
        if (g_structGen != gen0 || chan_list_changed() || mix_count_changed() || mix_ids_hash() != mixH) {
            // indices moved mid-tick: forget this tick's changes; they're found again, under their new numbers
            Sleep(POLL_MS);
            continue;
        }
        {   // a clean tick: take the changes as seen; one whose slot an apply or a rebase touched meanwhile: next tick
            int w = 0;
            EnterCriticalSection(&g_slotLock);
            for (i = 0; i < nc; i++)
                if (g_changes[i].s->id == g_changes[i].id && g_changes[i].s->gen == g_changes[i].gen) {
                    g_changes[i].s->last = g_changes[i].v;
                    g_changes[w++] = g_changes[i];
                }
            LeaveCriticalSection(&g_slotLock);
            nc = w;
        }

        // per-unit activity: remember it, and flag preset-sized bursts
        // (a huge tick is a project load, handled by the bulk guard below)
        for (i = 0; i < nc && nc < 2000; i++) {
            int unit, a, b, j, cnt = 0;
            Unit *u;
            if (g_changes[i].self || !unit_of_id(g_changes[i].s->id, &unit, &a, &b) || !(u = unit_ptr(unit, a, b))) continue;
            u->lastAct = u->userAt = now;
            u->editGen++;
            for (j = 0; j < nc; j++) {
                int u2, a2, b2;
                if (unit_of_id(g_changes[j].s->id, &u2, &a2, &b2) && u2 == unit && a2 == a && b2 == b) cnt++;
            }
            if (cnt >= BURST_PARAMS) u->burst = 1;
        }
        schedule_units(now);

        if (nc >= BULK_MIN_CHANGES) {
            uint32_t seen[BULK_MIN_CONTAINERS];
            int ns = 0, j;
            for (i = 0; i < nc && ns < BULK_MIN_CONTAINERS; i++) {
                uint32_t c = container_of(g_changes[i].s->id);
                for (j = 0; j < ns && seen[j] != c; j++) {}
                if (j == ns) seen[ns++] = c;
            }
            if (ns >= BULK_MIN_CONTAINERS) {
                // Project load/new/revert: never broadcast as edits (see project sync).
                logmsg("BULK local change (%d values, >=%d containers)", nc, ns);
                proj_begin("bulk param change");
                Sleep(POLL_MS);
                continue;
            }
        }

        for (i = 0; i < nc; i++) {
            Slot *s = g_changes[i].s;
            char key[32];
            int unit, a, b;
            if (g_changes[i].self) {
                static DWORD said = 0;
                if (now - said > 10000) { said = now; logmsg("PARAM 0x%08x moves by itself (automation / modulation): not synced", s->id); }
                continue;
            }
            Unit *pu = plug_param_unit(s->id, &unit, &a, &b) ? unit_ptr(unit, a, b) : NULL;
            if (pu && (pu->burst || plug_held_by_peer(unit, a, b))) {
                // a preset-sized change (or someone is replacing this plugin's whole state right now): under
                // the plugin's lock, like the state that follows it
                if (!plug_lock_param(unit, a, b, s, g_changes[i].prev, key, sizeof(key))) {
                    logmsg("LOCK %s held by another user: param 0x%08x snapped back", key, s->id);
                    enqueue_apply(s->id, g_changes[i].prev, 0);
                    enqueue_hint(key);
                    continue;
                }
            } else {
                // a knob (channel, mixer or plugin): its own lock; two people can turn different knobs of one plugin
                if (s->lockedByPeer) {
                    // someone else holds this knob: snap our local edit back
                    enqueue_apply(s->id, g_changes[i].prev, 0);
                    continue;
                }
                if (!s->lockedByMe) {                       // (what to put back if someone else holds it after all)
                    send_lock("lock_acquire", s->id); s->lockedByMe = 1;
                    s->undo = g_changes[i].prev; s->hasUndo = 1;
                }
                s->lastTouch = now;
                sprintf_s(key, sizeof(key), "param:%u", s->id);
            }
            if (!IS_MUTE_ID(s->id) && now - s->lastSent < PARAM_SEND_MS) { s->sendPending = 1; continue; }   // coalesced
            param_send(s, key, g_changes[i].v, now);
            logmsg("DETECT id=0x%08x %u -> %u (sent)", s->id, g_changes[i].prev, g_changes[i].v);
        }
        for (i = 0; i < g_nslot; i++) {                     // coalesced changes that stopped: their newest value
            Slot *s = &g_slots[i];
            char key[32];
            int unit, a, b;
            Unit *pu;
            if (!s->sendPending || now - s->lastSent < PARAM_SEND_MS) continue;
            pu = plug_param_unit(s->id, &unit, &a, &b) ? unit_ptr(unit, a, b) : NULL;
            if (pu && plug_held_by_peer(unit, a, b)) { s->sendPending = 0; continue; }   // the holder's state wins
            if (pu && pu->burst) plug_key(unit, a, b, key, sizeof(key));
            else {
                if (s->lockedByPeer) { s->sendPending = 0; continue; }
                sprintf_s(key, sizeof(key), "param:%u", s->id);
            }
            param_send(s, key, s->last, now);
            logmsg("DETECT id=0x%08x -> %u (sent, coalesced)", s->id, s->last);
        }
        Sleep(POLL_MS);
    }
}

// ---------------- inbound ----------------
static int jget_u32(const char *s, const char *name, uint32_t *out) {
    char pat[32]; sprintf_s(pat, sizeof(pat), "\"%s\":", name);
    const char *p = strstr(s, pat);
    if (!p) return 0;
    *out = (uint32_t)strtoul(p + strlen(pat), NULL, 10);
    return 1;
}
static int jget_paramkey(const char *s, uint32_t *id) {
    const char *p = strstr(s, "\"key\":\"param:");
    if (!p) return 0;
    *id = (uint32_t)strtoul(p + strlen("\"key\":\"param:"), NULL, 10);
    return 1;
}

// Leader: send every known value so a joining member converges to our state.
static void send_full_state(void) {
    int i, n = 0, cap = 64 + g_nslot * 24;
    char *buf = (char *)malloc(cap);
    int o = sprintf_s(buf, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"state.full\",\"vals\":[");
    for (i = 0; i < g_nslot; i++) {
        uint32_t v = g_slots[i].last;
        if (v == REC_NONE) continue;
        o += sprintf_s(buf + o, cap - o, "%s[%u,%u]", n ? "," : "", g_slots[i].id, v);
        n++;
    }
    sprintf_s(buf + o, cap - o, "]}}");
    ws_send(buf);
    free(buf);
    logmsg("STATE sent %d values", n);
}

// Inbound values for ids this FL doesn't have yet (still loading, plugin not
// scanned yet) wait here and are retried after each rescan.
#define MAX_PEND 8192
typedef struct { uint32_t id, v; } Pend;
static Pend g_pend[MAX_PEND];
static int  g_npend = 0;

static void pend_put(uint32_t id, uint32_t v) {
    int i;
    EnterCriticalSection(&g_qLock);
    for (i = 0; i < g_npend && g_pend[i].id != id; i++) {}
    if (i < g_npend) g_pend[i].v = v;
    else if (g_npend < MAX_PEND) { g_pend[g_npend].id = id; g_pend[g_npend].v = v; g_npend++; }
    LeaveCriticalSection(&g_qLock);
}

// Apply an inbound value (queued for the main thread). onlyIfDiff skips values
// we already have (join state). Returns 1 if an apply was queued.
static int route_inbound(uint32_t id, uint32_t v, int onlyIfDiff) {
    if (id == MAIN_VOL_ID) return 0;                  // an older flim still sends its main volume
    if (auto_target(id) && fl_playing()) return 0;    // automated: while this FL plays, its own automation drives it
    if (!add_slot(id)) { pend_put(id, v); return 0; }
    Slot *s = slot_by_id(id);
    if (onlyIfDiff && s && s->last == v) return 0;
    enqueue_apply(id, v, 1);
    return 1;
}

static void retry_pending(void) {
    int i, kept = 0, applied = 0;
    EnterCriticalSection(&g_qLock);
    for (i = 0; i < g_npend; i++) {
        Pend p = g_pend[i];
        if (add_slot(p.id)) {
            Slot *s = slot_by_id(p.id);
            if (!s || s->last != p.v) { enqueue_apply(p.id, p.v, 1); applied++; }
        } else g_pend[kept++] = p;
    }
    g_npend = kept;
    LeaveCriticalSection(&g_qLock);
    if (applied) logmsg("pending: applied %d, still waiting %d", applied, kept);
}

// Member: apply a state.full payload ("vals":[[id,v],...]).
static void apply_full_state(const char *m) {
    const char *p = strstr(m, "\"vals\":[");
    int n = 0, diff = 0;
    if (!p) return;
    p += strlen("\"vals\":[");
    while ((p = strchr(p, '[')) != NULL) {
        char *e;
        uint32_t id = (uint32_t)strtoul(p + 1, &e, 10);
        if (*e != ',') break;
        uint32_t v = (uint32_t)strtoul(e + 1, &e, 10);
        if (route_inbound(id, v, 1)) diff++;
        n++;
        p = e;
    }
    logmsg("STATE received %d values, %d differ (applying)", n, diff);
}

// ---- inbound plugin state ----
static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decode base64 [s, s+n) into a new temp .fst, with the links in base64 lk[0..ln) back at the end of its
// FLdt chunk (unit_json_fields); returns its path (malloc'd) or NULL.
static char *b64_to_tmpfile(const char *s, size_t n, const char *lk, size_t ln) {
    size_t o, tn = 0, dt;
    unsigned char *out = b64dec(s, n, &o), *t = ln ? b64dec(lk, ln, &tn) : NULL;
    if (tn && o >= 8 && (dt = 8 + (size_t)*(uint32_t *)(out + 4)) + 8 <= o && memcmp(out + dt, "FLdt", 4) == 0) {
        out = (unsigned char *)realloc(out, o + tn);
        memcpy(out + o, t, tn);
        *(uint32_t *)(out + dt + 4) += (uint32_t)tn;
        o += tn;
    }
    free(t);
    samples_rewrite(&out, &o);
    char *path = tmp_path("in");
    FILE *f = NULL;
    if (fopen_s(&f, path, "wb") != 0 || !f) { free(out); free(path); return NULL; }
    fwrite(out, 1, o, f); fclose(f); free(out);
    return path;
}

static int int_after(const char *p, const char *end, const char *key, int *out) {
    const char *q = strstr(p, key);
    if (!q || (end && q > end)) return 0;
    *out = atoi(q + strlen(key));
    return 1;
}

// Parse one unit object starting at p ("unit":..., ids..., "data":"..."[,"lk":"..."]). Returns
// pointer past it, or NULL. Fields are always written in this order by unit_json_fields.
// *lk: the sender put the unit's links in (LOAD_LINKS); an older flim doesn't.
static const char *parse_unit(const char *p, int *unit, int *a, int *b, char **path, int *lk) {
    const char *u = strstr(p, "\"unit\":\"");
    const char *d = u ? strstr(u, "\"data\":\"") : NULL, *l = NULL, *le = NULL;
    if (!u || !d) return NULL;
    *unit = strncmp(u + 8, "fx", 2) == 0 ? U_FX : U_CH;
    *a = *b = 0;
    if (*unit == U_CH) int_after(u, d, "\"ch\":", a);
    else { int_after(u, d, "\"trk\":", a); int_after(u, d, "\"slot\":", b); }
    d += 8;
    const char *e = strchr(d, '"');
    if (!e) return NULL;
    if (strncmp(e + 1, ",\"lk\":\"", 7) == 0 && (le = strchr(e + 8, '"')) != NULL) l = e + 8;
    *lk = l != NULL;
    *path = b64_to_tmpfile(d, (size_t)(e - d), l, l ? (size_t)(le - l) : 0);
    return le ? le + 1 : e + 1;
}

// Member: converge channels and plugin states to the leader's.
static void apply_plug_full(const char *m) {
    int nch = 0, unit, a, b, items = 0, k, lk;
    char *path;
    void **list;
    int local = chan_list(&list);
    int_after(m, NULL, "\"nch\":", &nch);
    for (k = local - 1; k >= nch; k--) enqueue_peer(T_DELCH, U_CH, k, 0, -1, NULL);  // extra channels
    if (local > nch) local = nch;
    const char *p = strstr(m, "\"items\":[");
    unsigned char *keep = (unsigned char *)calloc(MAX_MIX * MAX_FX_SLOTS, 1);
    while (p && (p = parse_unit(p, &unit, &a, &b, &path, &lk)) != NULL) {
        if (!path) continue;
        if (unit == U_FX && a >= 0 && a < MAX_MIX && b >= 0 && b < MAX_FX_SLOTS) keep[a * MAX_FX_SLOTS + b] = 1;
        if (unit == U_CH && a >= local) { enqueue_peer(T_ADDCH, U_CH, a, 0, -1, path); local++; }
        else enqueue_peer(T_LOAD, unit, a, b, 1 | (lk ? LOAD_LINKS : 0), path);
        items++;
    }
    {
        QItem q; memset(&q, 0, sizeof(q));
        q.type = T_FXSYNC; q.order = (int *)keep;
        enqueue(&q);
    }
    logmsg("PLUG full state received: leader has %d channels, %d units queued", nch, items);
}

// The leader's "alive" / "saving" signals: handled as they come, even during a project load (when
// other ops wait), or a member mid-load would think the leader froze.
static void leader_signal(const char *m) {
    if (strstr(m, "\"kind\":\"alive\"")) {   // "loading":1 = its FL is opening a project: give it LEADER_LOAD_MS more
        g_leaderAliveAt = (GetTickCount() + (strstr(m, "\"loading\":1") ? LEADER_LOAD_MS : 0)) | 1;
        if (InterlockedExchange(&g_leaderStuck, 0)) logmsg("PROJECT the session leader's FL is responding again");
    } else {
        const char *fr = strstr(m, "\"from\":\""), *p, *r;
        char pat[96];
        int i = 0;
        if (fr) for (fr += 8; fr[i] && fr[i] != '"' && i < (int)sizeof(g_hostSaver) - 1; i++) g_hostSaver[i] = fr[i];
        g_hostSaver[i] = 0;
        sprintf_s(pat, sizeof(pat), "{\"id\":\"%s\"", g_hostSaver);   // only the leader holds everyone off
        if (!(p = strstr(g_peersJson, pat)) || !(r = strstr(p, "\"role\":\"")) || strncmp(r + 8, "leader", 6) != 0) return;
        g_hostSavingUntil = strstr(m, "\"on\":1") ? (GetTickCount() + HOSTSAVE_MAX_MS) | 1 : 0;
        logmsg("PROJECT the host %s saving", g_hostSavingUntil ? "is" : "finished");
    }
}

// ---------------- desync safety net ----------------
// Whatever still slips through (two people changing structure in the same instant, an op lost to a crash or
// a dropped connection) mustn't stay. While nothing is in flight the leader sends a fingerprint of the song
// every DIGEST_NET_MS; a member whose own fingerprint differs in the same part twice in a row loads the
// leader's project again (resync). Only what is the same for everyone goes in, compared by number, never by
// memory address: no solo, selection, windows or plugin states, no automated knobs.
#define DIGEST_NET_MS  15000
#define QUIET_MS       5000         // no song op in or out this long: nothing is in flight
#define DESYNC_MIN_MS  120000       // at most one such reload this often
enum { DG_CH, DG_MX, DG_RT, DG_PAT, DG_PLT, DG_MARK, DG_TSIG, DG_PMARK, DG_NOTES, DG_CLIPS, DG_LINKS, DG_VOL, DG_N };
static const char *const DG_TAG[DG_N] = { "channels", "mixer names", "routes", "patterns", "playlist tracks", "markers",
                                          "time signature", "pattern markers", "notes", "clips", "automation links", "mixer volumes" };
static volatile DWORD g_lastOpAt;          // last song op sent or received
static uint32_t g_leaderDg[DG_N];          // the leader's last fingerprint (g_qLock), its arrangement, when it came
static int      g_leaderArr;
static volatile DWORD g_leaderDgAt;
static int op_changes_song(const char *m) {
    static const char *const QUIET[] = { "\"kind\":\"cur\"", "\"kind\":\"alive\"", "\"kind\":\"focus\"", "\"kind\":\"saving\"", "\"kind\":\"sel\"",
                                         "\"kind\":\"hello\"", "\"kind\":\"shared.", "\"kind\":\"digest\"", "\"kind\":\"state.req\"" };
    int i;
    if (!strstr(m, "\"t\":\"op\"")) return 0;
    for (i = 0; i < (int)(sizeof(QUIET) / sizeof(QUIET[0])); i++) if (strstr(m, QUIET[i])) return 0;
    return 1;
}
static uint32_t fold64(uint64_t h) { return (uint32_t)(h ^ (h >> 32)); }
// Poll thread (SEH-guarded unlocked reads, like the scanners).
static void digest_take(uint32_t *d, int *arrOut) {
    static const int MT[DG_N] = { M_CHAN, M_MIX, -1, M_PAT, M_PLT, M_MARK, M_TSIG, M_PMARK, -1, -1, -1, -1 };
    int k, i, nm = mix_count(), t, s;
    uint64_t h;
    void **items;
    for (k = 0; k < DG_N; k++) {
        h = 1469598103934665603ull;
        if (MT[k] >= 0)
            for (i = 0; i < meta_cap(MT[k]); i++) {
                uint64_t mh = meta_hash(MT[k], i);
                if (mh) { h = fnv_add(h, &i, 4); h = fnv_add(h, &mh, 8); }
            }
        d[k] = fold64(h);
    }
    k = chan_list(&items); d[DG_CH] ^= (uint32_t)k * 2654435761u;
    d[DG_MX] ^= (uint32_t)nm * 2654435761u;
    __try {
        h = 1469598103934665603ull;                            // routes by track number (meta_hash's are by identity)
        for (t = 0; t < nm && t < MIX_ROUTE_MAX; t++) {
            char *r = mix_rec(t);
            for (i = 0; i < nm && i < MIX_ROUTE_MAX; i++) {
                if (!r[MIX_ROUTE + i * 8 + 4]) continue;
                h = fnv_add(h, &t, 4); h = fnv_add(h, &i, 4); h = fnv_add(h, r + MIX_ROUTE + i * 8, 4);
            }
        }
        d[DG_RT] = fold64(h);
        h = 1469598103934665603ull;                            // notes, in any order, selection left out
        for (t = 1; t < MAX_PAT; t++) {
            char *a = pat_notes_arr(t);
            unsigned n = a ? *(unsigned *)(a + NOTES_COUNT) : 0, j;
            const unsigned char *nd = a ? *(unsigned char **)(a + NOTES_DATA) : NULL;
            uint64_t sum = 0;
            if (!n || n > 1000000 || !nd) continue;
            for (j = 0; j < n; j++) {
                unsigned char r[NOTE_SIZE];
                memcpy(r, nd + (size_t)j * NOTE_SIZE, NOTE_SIZE);
                r[NOTE_SEL_OFS] &= (unsigned char)~NOTE_SEL_BIT;
                sum += fnv64(r, NOTE_SIZE);
            }
            h = fnv_add(h, &t, 4); h = fnv_add(h, &sum, 8);
        }
        d[DG_NOTES] = fold64(h);
        {                                                      // the current arrangement's clips: their saved part
            int arr = cur_arr();
            char *ca = arr_clips(arr);
            unsigned n = ca ? *(unsigned *)(ca + CLIPS_COUNT) : 0, j;
            const unsigned char *cd = ca ? *(unsigned char **)(ca + CLIPS_DATA) : NULL;
            uint64_t sum = 0;
            for (j = 0; cd && n <= 200000 && j < n; j++) {
                unsigned char r[0x20];
                memcpy(r, cd + (size_t)j * CLIP_SIZE, sizeof(r));
                r[CLIP_SEL_OFS] &= (unsigned char)~CLIP_SEL_BIT;
                sum += fnv64(r, sizeof(r));
            }
            d[DG_CLIPS] = fold64(sum ^ n);
            *arrOut = arr;
        }
        {                                                      // automation links (not the MIDI ones: each person's own)
            char *L = links_list(), **it = L ? *(char ***)(L + 8) : NULL;
            int n = L ? *(int *)(L + 0x10) : 0;
            uint64_t sum = 0;
            for (i = 0; it && i < n && n <= 65536; i++) {
                uint32_t rec[5];
                if (!it[i]) continue;
                memcpy(rec, it[i] + 8, 20); rec[1] = 0;
                sum += fnv64((const unsigned char *)rec, sizeof(rec));
            }
            d[DG_LINKS] = fold64(sum ^ (uint64_t)n);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { d[DG_RT] = d[DG_NOTES] = d[DG_CLIPS] = d[DG_LINKS] = 0; }
    h = 1469598103934665603ull;                                // mixer volume / pan, unless automation plays them
    for (t = 0; t < nm; t++)
        for (s = 0; s < 2; s++) {
            uint32_t id = 0x70000000u + ((uint32_t)t << 22) + 0x1fc0u + (uint32_t)s, v;
            if (auto_target(id)) continue;
            v = read_value(id);
            h = fnv_add(h, &id, 4); h = fnv_add(h, &v, 4);
        }
    d[DG_VOL] = fold64(h);
}
static int digest_quiet(DWORD now) {
    // (input alone doesn't count: an edit not out yet makes one mismatch, and a reload takes two in a row)
    return g_netState == NET_ON && !g_projBusy && g_qh == g_qt && g_metaReady && now - g_lastOpAt >= QUIET_MS;
}
// Inbound fingerprint (recv thread; the leader's only).
static void on_digest(const char *m) {
    const char *p = strstr(m, "\"d\":[");
    uint32_t d[DG_N];
    int k = 0, arr = -1;
    if (g_isLeader || !p) return;
    int_after(m, NULL, "\"arr\":", &arr);
    for (p += 5; k < DG_N && *p && *p != ']'; k++) {
        char *e;
        d[k] = (uint32_t)strtoul(p, &e, 10);
        p = *e == ',' ? e + 1 : e;
    }
    if (k != DG_N) return;                     // another build's fingerprint
    EnterCriticalSection(&g_qLock);
    memcpy(g_leaderDg, d, sizeof(d)); g_leaderArr = arr;
    LeaveCriticalSection(&g_qLock);
    g_leaderDgAt = GetTickCount() | 1;
}
// Poll thread: the leader sends, a member compares (each while nothing is in flight).
static void digest_tick(DWORD now) {
    static DWORD sentAt, resyncAt;
    static int miss[DG_N];
    static unsigned reloaded, unfixable;       // parts the last reload was for / that a reload didn't fix
    uint32_t d[DG_N], ld[DG_N];
    int arr = -1, larr, k, n = 0;
    char why[256];
    DWORD at = g_leaderDgAt;
    if (g_isLeader) {
        char m[64 + DG_N * 12];
        int o;
        if (now - sentAt < DIGEST_NET_MS || !digest_quiet(now)) return;
        sentAt = now;
        digest_take(d, &arr);
        o = sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"digest\",\"arr\":%d,\"d\":[", arr);
        for (k = 0; k < DG_N; k++) o += sprintf_s(m + o, sizeof(m) - o, "%s%u", k ? "," : "", d[k]);
        sprintf_s(m + o, sizeof(m) - o, "]}}");
        ws_send(m);
        return;
    }
    if (!at) return;
    if ((LONG)(now - at) > 4000) { g_leaderDgAt = 0; return; }   // (signed: at is now | 1)        // not quiet here while it was fresh: skipped
    // (the leader was quiet for QUIET_MS before it sent, so everything it sent before is here already)
    if (g_netState != NET_ON || g_projBusy || g_qh != g_qt || now - g_lastOpAt < 3000) return;
    g_leaderDgAt = 0;
    EnterCriticalSection(&g_qLock);
    memcpy(ld, g_leaderDg, sizeof(ld)); larr = g_leaderArr;
    LeaveCriticalSection(&g_qLock);
    digest_take(d, &arr);
    why[0] = 0;
    for (k = 0; k < DG_N; k++) {
        if (d[k] == ld[k] || (k == DG_CLIPS && arr != larr)) {
            if (unfixable & (1u << k)) logmsg("DESYNC %s match the leader's again", DG_TAG[k]);
            miss[k] = 0; reloaded &= ~(1u << k); unfixable &= ~(1u << k);
            continue;
        }
        if (reloaded & (1u << k)) {               // still different right after the leader's project was loaded:
            reloaded &= ~(1u << k);               // FL itself makes them differ (it normalizes some things on load)
            unfixable |= 1u << k;
            logmsg("DESYNC %s still differ after loading the leader's project: a reload doesn't fix that, not reloading for it again", DG_TAG[k]);
        }
        if (unfixable & (1u << k)) continue;
        if (++miss[k] < 2) { logmsg("DESYNC check: %s differ from the leader's (checking again)", DG_TAG[k]); continue; }
        sprintf_s(why + strlen(why), sizeof(why) - strlen(why), "%s%s", n++ ? ", " : "", DG_TAG[k]);
    }
    if (!n) return;
    if (resyncAt && now - resyncAt < DESYNC_MIN_MS) {
        memset(miss, 0, sizeof(miss));
        logmsg("DESYNC %s still differ from the leader's (reloaded %lu s ago: not again yet)", why, (now - resyncAt) / 1000);
        return;
    }
    for (k = 0; k < DG_N; k++) if (miss[k] >= 2) reloaded |= 1u << k;
    memset(miss, 0, sizeof(miss));
    resyncAt = now;
    logmsg("DESYNC %s differ from the leader's twice in a row: loading the leader's project", why);
    gui_push(GUI_LOG, "flim: this FL fell out of step with the session leader; reloading the leader's project");
    resync(why);
}

// Everyone in a session needs the same flim: ops change between builds, and one can misread or skip what
// another sends. Who joins says its version and build time ("ask":1), the others answer with theirs. (The
// version is the push date, so two builds of one day share it.)
// ponytail: flims older than this one say nothing, so they aren't noticed.
#define FLIM_BUILD __DATE__ " " __TIME__
#define HELLO_FMT "{\"t\":\"op\",\"op\":{\"kind\":\"hello\",\"ver\":\"" FLIM_VERSION "\",\"build\":\"" FLIM_BUILD "\",\"ask\":%d}}"
static void json_str(const char *m, const char *key, char *out, int cap) {     // "key":"value" -> out ("" if none)
    const char *p = strstr(m, key);
    int i = 0;
    if (p) for (p += strlen(key); p[i] && p[i] != '"' && i < cap - 1; i++) out[i] = p[i];
    out[i] = 0;
}
static void on_hello(const char *m) {
    static char said[16][64];
    static int nsaid;
    char ver[32], build[32], from[64], who[64], text[480];
    int i;
    json_str(m, "\"ver\":\"", ver, sizeof(ver));
    json_str(m, "\"build\":\"", build, sizeof(build));
    json_str(m, "\"from\":\"", from, sizeof(from));
    if (strstr(m, "\"ask\":1")) { char r[160]; sprintf_s(r, sizeof(r), HELLO_FMT, 0); ws_send(r); }
    if (strcmp(ver, FLIM_VERSION) == 0 && strcmp(build, FLIM_BUILD) == 0) return;
    for (i = 0; i < nsaid && strcmp(said[i], from) != 0; i++) {}
    if (i < nsaid) return;
    if (nsaid < 16) strcpy_s(said[nsaid++], sizeof(said[0]), from);
    peer_name(from, who, sizeof(who));
    logmsg("NET %s runs flim %s (built %s), this is %s (built %s): some changes may not sync", who, ver, build, FLIM_VERSION, FLIM_BUILD);
    sprintf_s(text, sizeof(text), "%s is on flim %s (built %s) and you're on %s (built %s).\n\nEveryone in a session needs "
              "the same flim, or some changes won't reach everyone. Update to the newest one.", who, ver, build, FLIM_VERSION, FLIM_BUILD);
    gui_push(GUI_ALERT, text);
}

// ---- host controls: sync-only, the people list (give host / kick: the presence / error handlers below) ----
// The host (or the session leader, if the host allows it) can make someone the leader, remove them, or make them
// sync-only. The server enforces it all; this side shows it, keeps a sync-only FL's edits here (sync_only_keeps),
// and gives the flim window the people list (GUI_PEOPLE) its host controls work on (GUI_ADMIN).
static void locks_forget(void);
static int in_peers(const char *m, const char *id) {       // id is among the message's peers (not its removed people)
    char pat[96];
    const char *p = strstr(m, "\"peers\":["), *b = strstr(m, "\"banned\":["), *q;
    sprintf_s(pat, sizeof(pat), "{\"id\":\"%s\"", id);
    return p && (q = strstr(p, pat)) != NULL && (!b || q < b);
}
// welcome / presence: are we sync-only, and who is in the session (for the flim window).
static void on_peers(const char *m) {
    char me[96], id[72], name[72];
    const char *p = strstr(m, "\"peers\":["), *b = strstr(m, "\"banned\":["), *q, *r;
    int so = 0, o, cap = 16384, ctl = strstr(m, "\"leaderControls\":true") != NULL;
    char *g;
    sprintf_s(me, sizeof(me), "{\"id\":\"%s\"", g_myId);
    if (p && (q = strstr(p, me)) != NULL && (!b || q < b) && (r = strstr(q, "\"role\":\"")) != NULL && (r = strchr(r + 8, '"')) != NULL)
        so = strncmp(r + 1, ",\"sync\":true", 12) == 0;
    if (so && !g_syncOnly) {
        InterlockedExchange(&g_syncKept, 0);
        InterlockedExchange(&g_syncOnly, 1);
        locks_forget();                                    // (the server dropped ours)
        logmsg("SYNC the host made this FL sync-only: everything from the session still comes in, but nothing done here goes out");
        gui_push(GUI_ALERT, "The host made you sync-only.\n\nYou still get everything the others do, but nothing you do in FL "
                            "reaches them until the host switches you back. Changes you make meanwhile stay in your FL only.");
    } else if (!so && g_syncOnly) {
        InterlockedExchange(&g_syncOnly, 0);
        logmsg("SYNC the host switched this FL back from sync-only: your edits reach everyone again");
        if (g_syncKept) {                                  // ours differ from the others': the leader's project again
            InterlockedExchange(&g_userResync, 1); g_resyncAt = 0;
            resync("edits made while sync-only stayed in this FL");
            InterlockedExchange(&g_userResync, 0);
        }
    }
    g = (char *)malloc(cap);
    o = sprintf_s(g, cap, "%d\t%s\n", ctl, g_myId);
    for (q = p; q && (q = strstr(q, "{\"id\":\"")) != NULL && (!b || q < b) && o < cap - 300; q++) {   // P id name leader sync
        int lead = 0, sy = 0;
        json_str(q, "{\"id\":\"", id, sizeof(id));
        json_str(q, "\"name\":\"", name, sizeof(name));
        if ((r = strstr(q, "\"role\":\"")) != NULL) {
            lead = strncmp(r + 8, "leader", 6) == 0;
            if ((r = strchr(r + 8, '"')) != NULL) sy = strncmp(r + 1, ",\"sync\":true", 12) == 0;
        }
        o += sprintf_s(g + o, cap - o, "P\t%s\t%s\t%d\t%d\n", id, name, lead, sy);
    }
    for (q = b; q && (q = strstr(q, "{\"id\":\"")) != NULL && o < cap - 300; q++) {     // B id name ipban: removed
        const char *e = strchr(q, '}');
        json_str(q, "{\"id\":\"", id, sizeof(id));
        json_str(q, "\"name\":\"", name, sizeof(name));
        r = strstr(q, "\"ip\":true");
        o += sprintf_s(g + o, cap - o, "B\t%s\t%s\t%d\n", id, name, r && e && r < e);
    }
    gui_push(GUI_PEOPLE, g);
    free(g);
}

static void handle_msg_body(const char *m);
static void sh_inbox(const char *m);
static void sh_joined(void);
// Whatever one network message queues is someone else's op (task_rebind, do_struct).
static void handle_msg(const char *m) {
    log_op("in", m);
    if (op_changes_song(m)) g_lastOpAt = GetTickCount();
    {   // what came last (play_poll; not the constant cursors / alive)
        const char *k = strstr(m, "\"kind\":\"");
        char kind[sizeof(g_lastInKind)];
        int i = 0;
        if (k) {
            for (k += 8; *k && *k != '"' && i < (int)sizeof(kind) - 1; ) kind[i++] = *k++;
            kind[i] = 0;
            if (strcmp(kind, "cur") && strcmp(kind, "alive") && strcmp(kind, "sel")) { strcpy_s(g_lastInKind, sizeof(g_lastInKind), kind); g_lastInAt = GetTickCount(); }
        }
    }
    {   // "sc":[ch,mix,pat]
        const char *q = strstr(m, "\"sc\":[");
        int k;
        for (k = 0; k < SC_N; k++) t_msgSc[k] = -1;
        if (q) for (q += 6, k = 0; k < SC_N; k++) {
            char *e;
            t_msgSc[k] = (int)strtol(q, &e, 10);
            if (e == q) { t_msgSc[0] = t_msgSc[1] = t_msgSc[2] = -1; break; }
            q = *e == ',' ? e + 1 : e;
        }
    }
    t_msgEp = -1;
    int_after(m, NULL, "\"ep\":", &t_msgEp);
    if (strstr(m, "\"kind\":\"project.load\"")) {
        if (!g_isLeader && t_msgEp >= 0) InterlockedExchange(&g_projEpoch, t_msgEp);   // (on_project_load: the one we load)
    } else if (t_msgEp >= 0 && t_msgEp < g_projEpoch && op_changes_song(m)) {
        logmsg("SYNC an edit made before project %ld (the sender's FL was on %d) dropped: that project replaced it", g_projEpoch, t_msgEp);
        return;
    }
    t_peerMsg++; handle_msg_body(m); t_peerMsg--;
    t_msgSc[0] = t_msgSc[1] = t_msgSc[2] = -1;
}
static void handle_msg_body(const char *m) {    uint32_t id, v;
    int unit, a, b;
    char *path;
    if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"project.load\"")) {
        if (!g_isLeader) on_project_load(m);
        return;
    }
    if (strstr(m, "\"t\":\"op\"") && (strstr(m, "\"kind\":\"alive\"") || strstr(m, "\"kind\":\"saving\""))) { leader_signal(m); return; }
    if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"hello\"")) { on_hello(m); return; }
    if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"digest\"")) { on_digest(m); return; }
    if (strstr(m, "\"t\":\"op\"") && (strstr(m, "\"kind\":\"shared.files\"") || strstr(m, "\"kind\":\"shared.need\""))) {
        sh_inbox(m);                                     // not held during a project load: nothing of the song in it
        return;
    }
    if (hold_if_loading(m)) return;
    if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"state.req\"")) {
        // join / member re-sync: send the project (mid-load, the load's own broadcast covers it)
        if (g_isLeader && !g_projBusy) enqueue_unit(T_PROJSNAP, 0, 0, 0, 0, NULL);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"plug.full\"")) {
        if (!g_isLeader) apply_plug_full(m);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"plug.state\"")) {
        int lk;
        if (parse_unit(m, &unit, &a, &b, &path, &lk) && path) enqueue_peer(T_LOAD, unit, a, b, lk ? LOAD_LINKS : 0, path);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"chan.add\"")) {
        int n0 = -1, lk;                                     // -1: an older flim (appends only)
        int_after(m, NULL, "\"n0\":", &n0);
        if (parse_unit(m, &unit, &a, &b, &path, &lk) && path) enqueue_peer(T_ADDCH, U_CH, a, 0, n0, path);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"mix.struct\"")) {
        QItem q; memset(&q, 0, sizeof(q));
        q.type = T_MIXSTRUCT; q.path = _strdup(m);
        enqueue(&q);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"chan.del\"")) {
        int n0 = -1;
        int_after(m, NULL, "\"n0\":", &n0);
        if (int_after(m, NULL, "\"ch\":", &a)) enqueue_peer(T_DELCH, U_CH, a, 0, n0, NULL);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"notes.set\"")) {
        parse_notes_item(strstr(m, "\"kind\":\"notes.set\""), NULL);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"clips.set\"")) {
        on_clips_set(m);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"meta.set\"")) {
        on_meta_set(m);
    } else if (strstr(m, "\"t\":\"op\"") && (strstr(m, "\"kind\":\"arr.ins\"") || strstr(m, "\"kind\":\"arr.del\"") || strstr(m, "\"kind\":\"arr.name\""))) {
        QItem q; memset(&q, 0, sizeof(q));
        q.type = T_ARROP; q.path = _strdup(m);
        enqueue(&q);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"pat.op\"")) {
        on_pat_op(m);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"focus\"")) {
        on_focus(m);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"cur\"")) {
        on_cursor(m);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"sel\"")) {
        on_sel(m);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"notes.full\"")) {
        if (!g_isLeader) {
            unsigned char *keep = (unsigned char *)calloc(MAX_PAT, 1);
            const char *q = strstr(m, "\"items\":[");
            int cnt = 0;
            while (q && (q = parse_notes_item(q, keep)) != NULL) cnt++;
            QItem it; memset(&it, 0, sizeof(it));
            it.type = T_NOTESYNC; it.order = (int *)keep;
            enqueue(&it);
            logmsg("NOTES full state received: %d patterns", cnt);
        }
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"fx.clear\"")) {
        if (int_after(m, NULL, "\"trk\":", &a) && int_after(m, NULL, "\"slot\":", &b))
            enqueue_unit(T_FXCLEAR, U_FX, a, b, 0, NULL);
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"chan.order\"")) {
        int n = 0;
        const char *f = strstr(m, "\"from\":[");
        if (int_after(m, NULL, "\"n\":", &n) && f && n > 0 && n <= MAX_CH) {
            QItem q; memset(&q, 0, sizeof(q));
            q.type = T_ORDER;
            q.order = (int *)malloc(sizeof(int) * (size_t)(n + 1));
            q.order[0] = n;
            f += 8;
            for (a = 0; a < n; a++) { q.order[a + 1] = (int)strtol(f, (char **)&f, 10); if (*f == ',') f++; }
            enqueue(&q);
        }
    } else if (strstr(m, "\"t\":\"op\"") && strstr(m, "\"kind\":\"state.full\"")) {
        if (!g_isLeader) apply_full_state(m);
    } else if (strstr(m, "\"t\":\"welcome\"")) {
        strncpy_s(g_peersJson, sizeof(g_peersJson), m, _TRUNCATE);
        logmsg("server: %.300s", m);
        // our own role is the first "role" field; later ones belong to the peers list
        const char *r = strstr(m, "\"role\":\"");
        g_isLeader = r && strncmp(r + 8, "leader", 6) == 0;
        if (!g_isLeader) { int k; for (k = 0; k < SC_N; k++) InterlockedExchange(&g_sc[k], -1); }   // until the leader's project says
        on_peers(m);
        net_show_connected(m);
        if (!g_isLeader) ws_send("{\"t\":\"op\",\"op\":{\"kind\":\"state.req\"}}");
        else enqueue_unit(T_NAMEPROJ, 0, 0, 0, 0, NULL);  // an untitled project gets the song name, if one is set
        sh_joined();                                        // the shared folder's files go out
        { char h[128]; sprintf_s(h, sizeof(h), HELLO_FMT, 1); ws_send(h); }
    } else if (strstr(m, "\"t\":\"op\"")) {
        if (strstr(m, "\"kind\":\"param.set\"") && jget_u32(m, "id", &id) && jget_u32(m, "v", &v)) {
            route_inbound(id, v, 0);
        }
    } else if ((strstr(m, "\"t\":\"lock\"") || strstr(m, "\"t\":\"lock_released\"")) && on_unit_lock(m)) {
        // pattern / playlist lock (mutexing)
    } else if (strstr(m, "\"t\":\"lock\"") && jget_paramkey(m, &id)) {
        Slot *s = slot_by_id(id);
        int granted = strstr(m, "\"granted\":true") != NULL;
        char ownerPat[96]; sprintf_s(ownerPat, sizeof(ownerPat), "\"owner\":\"%s\"", g_myId);
        int mine = strstr(m, ownerPat) != NULL;
        if (s) {
            if (granted && !mine) s->lockedByPeer = 1;
            if (granted && mine) s->hasUndo = 0;
            if (!granted && !mine) {
                // someone else holds it: the server dropped our edit, so it goes back to what everyone has
                s->lockedByMe = 0; s->lockedByPeer = 1;
                if (s->hasUndo) { enqueue_apply(id, s->undo, 0); s->hasUndo = 0; }
                logmsg("lock denied id=0x%08x: local edit put back", id);
            }
        }
    } else if (strstr(m, "\"t\":\"lock_released\"") && jget_paramkey(m, &id)) {
        Slot *s = slot_by_id(id);
        if (s) s->lockedByPeer = 0;
    } else if (strstr(m, "\"t\":\"presence\"")) {
        // someone came or went; if the leader left, the server made the longest-connected member leader
        char me[96];
        const char *p = strstr(m, "\"peers\":["), *r, *q;
        sprintf_s(me, sizeof(me), "{\"id\":\"%s\"", g_myId);
        int lead = p && (p = strstr(p, me)) != NULL && (r = strstr(p, "\"role\":\"")) != NULL && strncmp(r + 8, "leader", 6) == 0;
        char oldLeader[64] = "", oldName[64] = "", newName[64] = "", a[400];
        {   // who led until now (the previous presence / welcome)
            const char *q = strstr(g_peersJson, "\"role\":\"leader\"");
            const char *id = q ? q : NULL;
            while (id && id > g_peersJson && strncmp(id, "{\"id\":\"", 7) != 0) id--;
            if (id && strncmp(id, "{\"id\":\"", 7) == 0) {
                int i;
                for (i = 0; id[7 + i] && id[7 + i] != '"' && i < 63; i++) oldLeader[i] = id[7 + i];
                oldLeader[i] = 0;
                peer_name(oldLeader, oldName, sizeof(oldName));
            }
        }
        strncpy_s(g_peersJson, sizeof(g_peersJson), m, _TRUNCATE);
        InterlockedExchange(&g_focusResend, 1);           // a newcomer: tell them where we are
        logmsg("server: %.300s", m);
        if (oldLeader[0] && in_peers(m, oldLeader) && (q = strstr(m, "\"role\":\"leader\"")) != NULL) {
            // the leader is still here: did the host give the session to someone else? (no reload: the new leader's
            // FL has the same project; it just holds it from now on)
            char nid[64];
            const char *id = q;
            int i;
            while (id > m && strncmp(id, "{\"id\":\"", 7) != 0) id--;
            for (i = 0; id[7 + i] && id[7 + i] != '"' && i < 63; i++) nid[i] = id[7 + i];
            nid[i] = 0;
            if (strcmp(nid, oldLeader) != 0) {
                peer_name(nid, newName, sizeof(newName));
                if (lead) strcpy_s(newName, sizeof(newName), "you");
                if (strcmp(oldLeader, g_myId) == 0) strcpy_s(oldName, sizeof(oldName), "you");
                logmsg("PROJECT the host made %s the session leader (instead of %s)", newName, oldName);
                if (lead) gui_push(GUI_ALERT, "The host made you the session leader.\n\nYour FL holds the project now, and people "
                                              "who join get it from you. Nobody had to reload.");
                else if (strcmp(oldLeader, g_myId) == 0) {
                    sprintf_s(a, sizeof(a), "The host made %s the session leader.\n\nYour FL keeps going as a member: nothing to reload.", newName);
                    gui_push(GUI_ALERT, a);
                }
                if (g_leaderAliveAt) g_leaderAliveAt = GetTickCount() | 1;   // the new leader gets its own 30 s
                InterlockedExchange(&g_leaderStuck, 0);
            }
        }
        if (oldLeader[0] && strcmp(oldLeader, g_myId) != 0) {
            if (!in_peers(m, oldLeader)) {                // the leader left; the server picked the next one
                const char *q = strstr(m, "\"role\":\"leader\""), *id = q;
                while (id && id > m && strncmp(id, "{\"id\":\"", 7) != 0) id--;
                if (lead) strcpy_s(newName, sizeof(newName), "you");
                else if (id && strncmp(id, "{\"id\":\"", 7) == 0) {
                    char nid[64];
                    int i;
                    for (i = 0; id[7 + i] && id[7 + i] != '"' && i < 63; i++) nid[i] = id[7 + i];
                    nid[i] = 0;
                    peer_name(nid, newName, sizeof(newName));
                }
                logmsg("PROJECT %s (the session leader) left: %s lead%s the session now", oldName, newName[0] ? newName : "someone else", lead ? "" : "s");
                sprintf_s(a, sizeof(a), lead ? "%s (the session leader) left the session.\n\nYou lead it now: your FL holds the project, "
                                               "and people who join get it from you. The session goes on as long as you stay."
                                             : "%s (the session leader) left the session.\n\n%s leads it now. The session goes on; you don't need to do anything.",
                          oldName, newName[0] ? newName : "Someone else");
                gui_push(GUI_ALERT, a);
                if (g_leaderAliveAt) g_leaderAliveAt = GetTickCount() | 1;   // the new leader gets its own 30 s
                InterlockedExchange(&g_leaderStuck, 0);
            }
        }
        g_isLeader = lead;
        on_peers(m);
        net_show_connected(m);
    } else if (strstr(m, "\"t\":\"error\"")) {
        logmsg("server: %.300s", m);
        if (strstr(m, "\"code\":\"bad_password\"")) strcpy_s(g_netErr, sizeof(g_netErr), "Wrong password");
        if (strstr(m, "\"code\":\"full\"") || strstr(m, "\"code\":\"too_many\"") || strstr(m, "\"code\":\"too_many_failures\"") ||
            strstr(m, "\"code\":\"rate\"") || strstr(m, "\"code\":\"join_timeout\""))   // the server's limits: it hangs up next
            json_str(m, "\"msg\":\"", g_netErr, sizeof(g_netErr));
        if (strstr(m, "\"code\":\"kicked\"")) {             // removed by the host: the server hangs up next
            json_str(m, "\"msg\":\"", g_netErr, sizeof(g_netErr));
            InterlockedExchange(&g_kicked, 1);
            logmsg("PROJECT %s", g_netErr);
        } else if (strstr(m, "\"code\":\"admin\"") || strstr(m, "\"code\":\"not_allowed\"")) {   // a host control of ours refused
            char why[200];
            json_str(m, "\"msg\":\"", why, sizeof(why));
            gui_push(GUI_ALERT, why);
        }
    }
}

// ---------------- samples ----------------
// Sample files travel with the data that uses them. FLP event 196 (a channel's sample path,
// UTF-16) appears in .flp files and in saved channel states alike, so the same code covers
// project loads (project.load) and live channel changes (plug.state / chan.add).
// Sender (outbox thread): the op lists the samples its data references,
//   "samples":[{"p":<base64 UTF-16 path as written in the data>,"h":<sha256 hex>,"s":<bytes>}],
// after uploading the files the server doesn't have yet to its blob store (HTTP next to the
// WebSocket: /blobs/missing, /blob/<sha256>; see server/src/blobs.ts). More than
// SAMPLE_ASK_BYTES to upload at once -> Yes/No dialog first; "No" leaves those samples out
// (and they are not asked about again this session). Every op queued behind waits, so the
// order of ops is kept.
// Receiver (inbox thread): before an op that lists samples is handled, each sample is found
// locally (same path with the same content, or any file known to have that content) or
// downloaded into Documents\flim\samples\<hash>\; the paths in the op's data are then rewritten
// to the local files. Later ops wait behind it, again to keep the order.
// Samples inside plugin states (event 213, the plugin's own data) travel the same way: paths
// are found in Image-Line's tag/length/value layout (FPC and other newer IL plugins; rewritten
// with every enclosing length fixed), and as plain absolute UTF-16 / ANSI paths anywhere else in
// the data (any other plugin). Those get a local alias of exactly the same length (a hard link
// to the downloaded file), so length fields inside a format we don't know stay valid.
// Compressed / encrypted states (Kontakt...) show no paths and are left as they are.
#define SAMPLE_ASK_BYTES (50ull * 1024 * 1024)
#define SAMPLE_MAX_BYTES (2000ull * 1024 * 1024)
static int in_shared(const wchar_t *p);                       // the shared folder (its own section)
#define SAMPLE_PATH_MAX  1024
#define BLOB_CHUNK       (256 * 1024)

typedef struct { wchar_t *path; uint64_t size; FILETIME mt; char hex[65]; } HashEnt;
typedef struct { wchar_t *from, *to; } PathMap;
typedef struct OutItem { char *msg; unsigned char *blob; size_t blobn; struct OutItem *next; } OutItem;
typedef struct InItem { char *msg; struct InItem *next; } InItem;

static HashEnt *g_hashes;  static int g_nhash, g_caphash;    // content hash per local file (path+size+mtime)
static PathMap *g_pmap;    static int g_npmap, g_cappmap;    // receiver: path in peer data -> local file
static char (*g_declined)[65]; static int g_ndeclined, g_capdeclined;   // "No" in the upload dialog
static CRITICAL_SECTION g_sampleLock, g_outLock, g_inLock;                        // all of the above + the blob server address
static NetUrl  g_blobUrl;
static char    g_blobKey[65];                                // sha256 hex of the server password
static volatile LONG g_blobOn = 0;
static int     g_noLocal = 0;                                // FLIM_SAMPLE_NOLOCAL=1 (tests): ignore local copies
static OutItem *g_outHead, *g_outTail;
static int     g_outBusy;
static InItem  *g_inHead, *g_inTail;
static int     g_inBusy;
static HANDLE  g_outEvt, g_inEvt, g_upEvt, g_dnEvt;
static volatile uint64_t g_upDone, g_upTotal, g_dnDone, g_dnTotal;   // background transfers (status text)
static DWORD   g_xferShown;

// Background transfers. Uploads: the op goes out right away, the file follows. Downloads: the
// op is applied right away with a silent placeholder file (WAV / AIFF; MP3 / FLAC / OGG with their length) where the sample will
// be; the download retries until the sender's upload is there, then the real data is written
// INTO the placeholder file (same file, so hard-link aliases see it too) and the units that use
// it are reloaded. Other formats (and compressed ones of unknown length) wait for the download.
typedef struct XJob { char hex[65]; uint64_t size; wchar_t path[MAX_PATH * 2]; int ph; DWORD first, next; struct XJob *nx; } XJob;
static XJob   *g_upQ, *g_dnQ;                                // under g_sampleLock
typedef struct { wchar_t *path; char hex[65]; uint64_t size; } Placeholder;
static Placeholder *g_ph; static int g_nph, g_capph;          // files standing in for samples (g_sampleLock)
static wchar_t **g_reload; static int g_nreload, g_capreload; // file names to reload once downloaded (g_sampleLock)
#define DL_RETRY_MS   1500
#define DL_GIVEUP_MS  (60u * 60 * 1000)   // the sender's upload never came

typedef struct { BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; } Sha;
static int sha_begin(Sha *s) {
    memset(s, 0, sizeof(*s));
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&s->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) return 0;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(s->alg, &s->h, NULL, 0, NULL, 0, 0))) { BCryptCloseAlgorithmProvider(s->alg, 0); return 0; }
    return 1;
}
static void sha_add(Sha *s, const void *p, size_t n) { BCryptHashData(s->h, (PUCHAR)p, (ULONG)n, 0); }
static void sha_end(Sha *s, char hex[65]) {
    UCHAR d[32];
    int i;
    BCryptFinishHash(s->h, d, 32, 0);
    BCryptDestroyHash(s->h);
    BCryptCloseAlgorithmProvider(s->alg, 0);
    for (i = 0; i < 32; i++) sprintf_s(hex + i * 2, 3, "%02x", d[i]);
}
static void sha_hex(const void *p, size_t n, char hex[65]) {
    Sha s;
    hex[0] = 0;
    if (!sha_begin(&s)) return;
    sha_add(&s, p, n);
    sha_end(&s, hex);
}
static int is_hex64(const char *h) {
    int i;
    for (i = 0; i < 64; i++) if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return 0;
    return 1;
}

// Transfer progress in the toolbar status.
static void xfer_show(int force) {
    DWORD now = GetTickCount();
    char a[256] = "";
    uint64_t ut = g_upTotal, dt = g_dnTotal, ud = g_upDone, dd = g_dnDone;
    if (!force && now - g_xferShown < 1000) return;
    g_xferShown = now;
    if (ut) sprintf_s(a, sizeof(a), "Uploading samples: %.1f / %.1f MB", (ud < ut ? ud : ut) / 1048576.0, ut / 1048576.0);
    if (dt) sprintf_s(a + strlen(a), sizeof(a) - strlen(a), "%sDownloading samples: %.1f / %.1f MB (placeholders until done)",
                      a[0] ? " \xC2\xB7 " : "", (dd < dt ? dd : dt) / 1048576.0, dt / 1048576.0);
    if (a[0]) net_status(NET_ON, "%s", a);
}
static void xfer_end(void) {
    if (g_upTotal || g_dnTotal) xfer_show(1);
    else if (g_peersJson[0]) net_show_connected(g_peersJson);
}

// ---- placeholders ----
// Real content hash/size of a placeholder file (or an alias of one). 0 = not a placeholder.
static int placeholder_of(const wchar_t *p, char hex[65], uint64_t *size) {
    int i, r = 0;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_nph && !r; i++)
        if (_wcsicmp(g_ph[i].path, p) == 0) { strcpy_s(hex, 65, g_ph[i].hex); *size = g_ph[i].size; r = 1; }
    LeaveCriticalSection(&g_sampleLock);
    return r;
}
static void placeholder_add(const wchar_t *p, const char *hex, uint64_t size) {
    char h[65];
    uint64_t s;
    if (placeholder_of(p, h, &s)) return;
    EnterCriticalSection(&g_sampleLock);
    if (g_nph == g_capph) { g_capph = g_capph ? g_capph * 2 : 32; g_ph = (Placeholder *)realloc(g_ph, sizeof(Placeholder) * g_capph); }
    g_ph[g_nph].path = _wcsdup(p);
    strcpy_s(g_ph[g_nph].hex, 65, hex);
    g_ph[g_nph].size = size;
    g_nph++;
    LeaveCriticalSection(&g_sampleLock);
}
static void placeholder_done(const char *hex) {           // the real data is in: no longer placeholders
    int i;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_nph; )
        if (strcmp(g_ph[i].hex, hex) == 0) { free(g_ph[i].path); g_ph[i] = g_ph[--g_nph]; }
        else i++;
    LeaveCriticalSection(&g_sampleLock);
}
// The file names that finished downloading since the last call, as one list of NUL-terminated
// names ending in an empty one (*len wchar_ts in all). NULL if none.
static wchar_t *reload_take(int *n, size_t *len) {
    wchar_t **names, *list;
    int i;
    size_t o = 0;
    EnterCriticalSection(&g_sampleLock);
    names = g_reload; *n = g_nreload;
    g_reload = NULL; g_nreload = g_capreload = 0;
    LeaveCriticalSection(&g_sampleLock);
    if (!*n) return NULL;
    for (*len = 1, i = 0; i < *n; i++) *len += wcslen(names[i]) + 1;
    list = (wchar_t *)malloc(*len * sizeof(wchar_t));
    for (i = 0; i < *n; i++) { wcscpy_s(list + o, *len - o, names[i]); o += wcslen(names[i]) + 1; free(names[i]); }
    list[o] = 0;
    free(names);
    return list;
}
// 1 = WAV, 2 = AIFF, 3 = compressed (MP3 / FLAC / OGG: a WAV under its name, FL's loader goes by the content, so it
// needs the sender's length), 0 = no placeholder possible
static int placeholder_format(const wchar_t *p) {
    if (ends_with_ci(p, L".wav")) return 1;
    if (ends_with_ci(p, L".aif") || ends_with_ci(p, L".aiff")) return 2;
    if (ends_with_ci(p, L".mp3") || ends_with_ci(p, L".flac") || ends_with_ci(p, L".ogg")) return 3;
    return 0;
}
static void be32(unsigned char *o, uint32_t v) { o[0] = (unsigned char)(v >> 24); o[1] = (unsigned char)(v >> 16); o[2] = (unsigned char)(v >> 8); o[3] = (unsigned char)v; }
// A silent stand-in for a sample that is still on its way: 8 kHz, 8 bit, mono, about as long as
// the real one (so audio clips keep their length and look right), at the sample's final path.
static int placeholder_write(const wchar_t *path, uint64_t ms) {
    const uint32_t RATE = 8000;
    uint32_t n, w;
    unsigned char h[64], *body;
    int hn, fmt = placeholder_format(path), ok;
    HANDLE f;
    if (!fmt) return 0;
    if (ms < 50) ms = 50;
    if (ms > 20 * 60 * 1000) ms = 20 * 60 * 1000;
    n = (uint32_t)(RATE * ms / 1000);
    memset(h, 0, sizeof(h));
    if (fmt != 2) {
        memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + n; memcpy(h + 8, "WAVEfmt ", 8);
        *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 1;
        *(uint32_t *)(h + 24) = RATE; *(uint32_t *)(h + 28) = RATE; *(uint16_t *)(h + 32) = 1; *(uint16_t *)(h + 34) = 8;
        memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = n;
        hn = 44;
    } else {
        static const unsigned char rate80[10] = { 0x40, 0x0B, 0xFA, 0, 0, 0, 0, 0, 0, 0 };   // 8000.0, 80-bit extended
        memcpy(h, "FORM", 4); be32(h + 4, 4 + 26 + 16 + n + (n & 1)); memcpy(h + 8, "AIFFCOMM", 8);
        be32(h + 16, 18); h[21] = 1; be32(h + 22, n); h[27] = 8; memcpy(h + 28, rate80, 10);
        memcpy(h + 38, "SSND", 4); be32(h + 42, 8 + n);
        hn = 54;
    }
    body = (unsigned char *)malloc((size_t)n + 1);
    memset(body, fmt != 2 ? 0x80 : 0, (size_t)n + 1);     // silence: unsigned 8-bit WAV, signed AIFF
    f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS, 0, NULL);
    ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        ok = WriteFile(f, h, hn, &w, NULL) && w == (DWORD)hn && WriteFile(f, body, n + (fmt == 2 ? (n & 1) : 0), &w, NULL);
        CloseHandle(f);
    }
    free(body);
    return ok;
}
// Length of a WAV file in ms from its header (0 = unknown). Sent along so placeholders match.
static uint64_t wav_ms(const wchar_t *p) {
    unsigned char h[4096];
    DWORD got = 0;
    uint32_t rate = 0;
    size_t o = 12;
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(f, h, sizeof(h), &got, NULL)) got = 0;
    CloseHandle(f);
    if (got < 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return 0;
    while (o + 8 <= got) {
        uint32_t len = *(const uint32_t *)(h + o + 4);
        if (memcmp(h + o, "fmt ", 4) == 0 && o + 20 <= got) rate = *(const uint32_t *)(h + o + 16);   // byte rate
        if (memcmp(h + o, "data", 4) == 0) return rate ? (uint64_t)len * 1000 / rate : 0;
        o += 8 + (size_t)len + (len & 1);
    }
    return 0;
}

// Length in ms of an MP3 / FLAC / OGG (Vorbis or Opus) file from its headers (0 = unknown): for placeholders.
static unsigned char *file_part(HANDLE f, int64_t at, DWORD n, DWORD *got) {
    unsigned char *b = (unsigned char *)malloc(n);
    LARGE_INTEGER li;
    li.QuadPart = at;
    *got = 0;
    if (!SetFilePointerEx(f, li, NULL, at < 0 ? FILE_END : FILE_BEGIN) || !ReadFile(f, b, n, got, NULL)) *got = 0;
    return b;
}
static uint64_t flac_ms(const unsigned char *h, DWORD got) {
    uint32_t rate;
    uint64_t total;
    if (got < 42 || memcmp(h, "fLaC", 4) != 0 || (h[4] & 0x7F) != 0) return 0;   // STREAMINFO comes first
    rate = ((uint32_t)h[18] << 12) | ((uint32_t)h[19] << 4) | (h[20] >> 4);
    total = ((uint64_t)(h[21] & 0x0F) << 32) | ((uint64_t)h[22] << 24) | ((uint64_t)h[23] << 16) | ((uint64_t)h[24] << 8) | h[25];
    return rate ? total * 1000 / rate : 0;
}
static uint64_t ogg_ms(HANDLE f, const unsigned char *h, DWORD got) {
    uint32_t rate = 0, pre = 0;
    unsigned char *t;
    DWORD tn, i;
    uint64_t gran = 0;
    if (got < 64 || memcmp(h, "OggS", 4) != 0) return 0;
    {
        const unsigned char *pk = h + 27 + h[26];                // first packet, after the segment table
        if (pk + 20 > h + got) return 0;
        if (memcmp(pk, "\x01vorbis", 7) == 0) rate = *(const uint32_t *)(pk + 12);
        else if (memcmp(pk, "OpusHead", 8) == 0) { rate = 48000; pre = *(const uint16_t *)(pk + 10); }
    }
    if (!rate) return 0;
    t = file_part(f, -65536, 65536, &tn);
    if (!tn) { free(t); t = file_part(f, 0, 65536, &tn); }
    for (i = tn >= 14 ? tn - 14 : 0; i > 0; i--)                // the last page's granule position: the length
        if (memcmp(t + i, "OggS", 4) == 0) { gran = *(const uint64_t *)(t + i + 6); break; }
    free(t);
    if (!gran || gran == ~0ull || gran <= pre) return 0;
    return (gran - pre) * 1000 / rate;
}
static uint64_t mp3_ms(HANDLE f, const unsigned char *h, DWORD got) {
    static const int BR[2][16] = { { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },     // MPEG 1 layer III
                                   { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } };     // MPEG 2 / 2.5
    static const int SR[4] = { 44100, 48000, 32000, 0 };
    LARGE_INTEGER sz;
    DWORD o = 0;
    int v, rate, br, spf, side, mono;
    uint32_t frames = 0;
    if (got >= 10 && memcmp(h, "ID3", 3) == 0) o = 10 + (((DWORD)(h[6] & 0x7F) << 21) | ((DWORD)(h[7] & 0x7F) << 14) | ((DWORD)(h[8] & 0x7F) << 7) | (h[9] & 0x7F));
    {   // the first frame (after the tag: read it if it was longer than the head)
        unsigned char *b = NULL;
        DWORD bn = 0;
        const unsigned char *q;
        if (o + 4 > got) { b = file_part(f, o, 4096, &bn); q = b; } else { q = h + o; bn = got - o; }
        while (bn >= 4 && !(q[0] == 0xFF && (q[1] & 0xE6) == 0xE2)) { q++; bn--; o++; }   // sync, layer III
        if (bn < 4) { free(b); return 0; }
        v = (q[1] >> 3) & 3;                                     // 3 MPEG 1, 2 MPEG 2, 0 MPEG 2.5
        rate = SR[(q[2] >> 2) & 3];
        if (!rate || v == 1) { free(b); return 0; }
        rate = v == 3 ? rate : v == 2 ? rate / 2 : rate / 4;
        br = BR[v == 3 ? 0 : 1][q[2] >> 4];
        spf = v == 3 ? 1152 : 576;
        mono = (q[3] >> 6) == 3;
        side = v == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        if (bn >= (DWORD)(4 + side + 12) && (memcmp(q + 4 + side, "Xing", 4) == 0 || memcmp(q + 4 + side, "Info", 4) == 0) && (q[4 + side + 7] & 1))
            frames = ((uint32_t)q[4 + side + 8] << 24) | ((uint32_t)q[4 + side + 9] << 16) | ((uint32_t)q[4 + side + 10] << 8) | q[4 + side + 11];
        else if (bn >= 4 + 32 + 18 && memcmp(q + 4 + 32, "VBRI", 4) == 0)
            frames = ((uint32_t)q[36 + 14] << 24) | ((uint32_t)q[36 + 15] << 16) | ((uint32_t)q[36 + 16] << 8) | q[36 + 17];
        free(b);
    }
    if (frames) return (uint64_t)(frames + 1) * spf * 1000 / rate;   // (FL decodes the Xing / VBRI frame too: a silent one)
    if (!br || !GetFileSizeEx(f, &sz) || sz.QuadPart <= o) return 0;   // constant bitrate: from the size
    return (uint64_t)(sz.QuadPart - o) * 8 / br;
}
static uint64_t compressed_ms(const wchar_t *p) {
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char *h;
    DWORD got;
    uint64_t ms = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    h = file_part(f, 0, 8192, &got);
    if (ends_with_ci(p, L".flac")) ms = flac_ms(h, got);
    else if (ends_with_ci(p, L".ogg")) ms = ogg_ms(f, h, got);
    else if (ends_with_ci(p, L".mp3")) ms = mp3_ms(f, h, got);
    free(h);
    CloseHandle(f);
    return ms;
}

static int file_info(const wchar_t *p, uint64_t *size, FILETIME *mt) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (mt) *mt = a.ftLastWriteTime;
    return 1;
}
static int is_abs_path(const wchar_t *p) {
    return (iswalpha(p[0]) && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) || (p[0] == L'\\' && p[1] == L'\\');
}
// A network path (\\host\share\..., //host/...). One a peer sent is never opened here: that would make this
// PC connect to whatever host the peer names. Such samples come by content like any other.
static int is_net_path(const wchar_t *p) {
    return (p[0] == L'\\' || p[0] == L'/') && (p[1] == L'\\' || p[1] == L'/');
}

static int hash_file(const wchar_t *p, char hex[65]) {
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    unsigned char *buf;
    DWORD got;
    Sha s;
    int ok = 1;
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (!sha_begin(&s)) { CloseHandle(f); return 0; }
    buf = (unsigned char *)malloc(BLOB_CHUNK * 4);
    for (;;) {
        if (!ReadFile(f, buf, BLOB_CHUNK * 4, &got, NULL)) { ok = 0; break; }
        if (!got) break;
        sha_add(&s, buf, got);
    }
    sha_end(&s, hex);
    free(buf);
    CloseHandle(f);
    return ok;
}

static void hash_put(const wchar_t *p, uint64_t size, FILETIME mt, const char *hex) {
    int i;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_nhash && _wcsicmp(g_hashes[i].path, p) != 0; i++) {}
    if (i == g_nhash) {
        if (g_nhash == g_caphash) { g_caphash = g_caphash ? g_caphash * 2 : 256; g_hashes = (HashEnt *)realloc(g_hashes, sizeof(HashEnt) * g_caphash); }
        g_hashes[g_nhash].path = _wcsdup(p);
        g_nhash++;
    }
    g_hashes[i].size = size; g_hashes[i].mt = mt;
    strcpy_s(g_hashes[i].hex, sizeof(g_hashes[i].hex), hex);
    LeaveCriticalSection(&g_sampleLock);
}

// Content hash of a local file (cached while path, size and mtime stay the same).
static int sample_hash(const wchar_t *p, char hex[65], uint64_t *size) {
    FILETIME mt;
    uint64_t sz;
    int i, hit = 0;
    if (!file_info(p, &sz, &mt) || sz > SAMPLE_MAX_BYTES) return 0;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_nhash; i++)
        if (_wcsicmp(g_hashes[i].path, p) == 0 && g_hashes[i].size == sz && CompareFileTime(&g_hashes[i].mt, &mt) == 0) {
            strcpy_s(hex, 65, g_hashes[i].hex); hit = 1; break;
        }
    LeaveCriticalSection(&g_sampleLock);
    if (!hit) {
        if (!hash_file(p, hex)) return 0;
        hash_put(p, sz, mt, hex);
    }
    *size = sz;
    return 1;
}

// Any local file known to have this content (and unchanged since it was hashed).
static int find_by_hash(const char *hex, wchar_t *out, int cap) {
    int i, found = 0;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_nhash && !found; i++) {
        uint64_t sz;
        FILETIME mt;
        if (strcmp(g_hashes[i].hex, hex) != 0 || in_shared(g_hashes[i].path)) continue;   // (it may change under us)
        if (!file_info(g_hashes[i].path, &sz, &mt) || sz != g_hashes[i].size || CompareFileTime(&g_hashes[i].mt, &mt) != 0) continue;
        wcsncpy_s(out, cap, g_hashes[i].path, _TRUNCATE);
        found = 1;
    }
    LeaveCriticalSection(&g_sampleLock);
    return found;
}

static void map_set(const wchar_t *from, const wchar_t *to) {
    int i;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_npmap && wcscmp(g_pmap[i].from, from) != 0; i++) {}
    if (!to) {
        if (i < g_npmap) { free(g_pmap[i].from); free(g_pmap[i].to); g_pmap[i] = g_pmap[--g_npmap]; }
    } else {
        if (i == g_npmap) {
            if (g_npmap == g_cappmap) { g_cappmap = g_cappmap ? g_cappmap * 2 : 64; g_pmap = (PathMap *)realloc(g_pmap, sizeof(PathMap) * g_cappmap); }
            g_pmap[g_npmap].from = _wcsdup(from);
            g_pmap[g_npmap].to = NULL;
            g_npmap++;
        }
        free(g_pmap[i].to);
        g_pmap[i].to = _wcsdup(to);
    }
    LeaveCriticalSection(&g_sampleLock);
}

static int declined(const char *hex) {
    int i, r = 0;
    EnterCriticalSection(&g_sampleLock);
    for (i = 0; i < g_ndeclined && !r; i++) r = strcmp(g_declined[i], hex) == 0;
    LeaveCriticalSection(&g_sampleLock);
    return r;
}
static void decline(const char *hex) {
    EnterCriticalSection(&g_sampleLock);
    if (g_ndeclined == g_capdeclined) { g_capdeclined = g_capdeclined ? g_capdeclined * 2 : 64; g_declined = (char (*)[65])realloc(g_declined, 65 * (size_t)g_capdeclined); }
    strcpy_s(g_declined[g_ndeclined++], 65, hex);
    LeaveCriticalSection(&g_sampleLock);
}

// %FLStudioUserData% = <"Shared data" from FL's registry paths>\FL Studio
static void fl_user_data(wchar_t *out, int cap) {
    DWORD n = (DWORD)cap * 2;
    size_t l;
    out[0] = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Image-Line\\Shared\\Paths", L"Shared data", RRF_RT_REG_SZ, NULL, out, &n) != ERROR_SUCCESS || !out[0]) {
        DWORD k = GetEnvironmentVariableW(L"USERPROFILE", out, cap);
        if (!k || k >= (DWORD)cap) out[0] = 0;
        wcscat_s(out, cap, L"\\Documents\\Image-Line\\");
    }
    l = wcslen(out);
    if (l && out[l - 1] != L'\\') wcscat_s(out, cap, L"\\");
    wcscat_s(out, cap, L"FL Studio");
}
// A sample path as FL writes it -> a local file path. 0 = not a transferable file
// (factory data and other FL placeholders: every install has those).
static int expand_path(const wchar_t *p, wchar_t *out, int cap) {
    static const wchar_t UD[] = L"%FLStudioUserData%";
    size_t ul = wcslen(UD);
    if (p[0] != L'%') { if (wcslen(p) >= (size_t)cap) return 0; wcscpy_s(out, cap, p); return is_abs_path(out); }
    if (_wcsnicmp(p, UD, ul) != 0) return 0;
    fl_user_data(out, cap);
    if (wcslen(out) + wcslen(p + ul) >= (size_t)cap) return 0;
    wcscat_s(out, cap, p + ul);
    return 1;
}

// ---- FLP / .fst images ----
// Walk the events of an FLP image ("FLhd" header chunk, then one "FLdt" chunk). cb gets each
// event: id, payload, and the event's byte range [at, end). Returns 0 if d is no FLP image.
// Payload size by id: <64 byte, <128 word, <192 dword, else varint length + data. Byte 172 is
// an escape (FL 2026, sub_A9E7A0): a 32-bit id follows, sized by its top bits when >= 0x100.
static int flp_walk(const unsigned char *d, size_t n, flpev_t cb, void *ctx) {
    size_t o, hl;
    if (!d || n < 22 || memcmp(d, "FLhd", 4) != 0) return 0;
    hl = *(const uint32_t *)(d + 4);
    o = 8 + hl;
    if (o + 8 > n || memcmp(d + o, "FLdt", 4) != 0) return 0;
    o += 8;
    while (o < n) {
        size_t at = o, len = 0;
        uint32_t id = d[o++], cls;
        if (id == 172) {
            if (o + 4 > n) return 0;
            id = *(const uint32_t *)(d + o);
            o += 4;
        }
        if (id >= 0x100 && (id & 0x30000000)) return 0;    // FL knows no such size class
        cls = id < 0x100 ? id >> 6 : id >> 30;
        if (cls == 0) len = 1;
        else if (cls == 1) len = 2;
        else if (cls == 2) len = 4;
        else {
            int s = 0;
            for (;;) {
                unsigned char b;
                if (o >= n || s > 28) return 0;
                b = d[o++];
                len |= (size_t)(b & 0x7f) << s;
                s += 7;
                if (!(b & 0x80)) break;
            }
        }
        if (o + len > n) return 0;
        if (cb && !cb(ctx, (int)id, d + o, len, at, o + len)) break;
        o += len;
    }
    return 1;
}

static void ev_wstr(const unsigned char *p, size_t n, wchar_t *out, int cap) {
    size_t k = n / 2;
    if (k >= (size_t)cap) k = (size_t)cap - 1;
    memcpy(out, p, k * 2);
    out[k] = 0;
}

typedef struct { wchar_t **paths; int n, cap; } PathList;
static void pathlist_add(PathList *L, const wchar_t *w) {
    int i;
    if (!w[0]) return;
    for (i = 0; i < L->n; i++) if (wcscmp(L->paths[i], w) == 0) return;
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 16; L->paths = (wchar_t **)realloc(L->paths, sizeof(wchar_t *) * L->cap); }
    L->paths[L->n++] = _wcsdup(w);
}

// ---- paths inside plugin data ----
// Any file a plugin (FL's own or a VST) has loaded travels: samples, soundfonts, wavetables, impulse
// responses, multisamples... Never programs, plugin binaries (a VST wrapper's state names its .dll),
// shortcuts, keys or Windows' own files. A file name needs an extension.
static int named_file(const wchar_t *w, size_t n) {          // "...\name.ext"
    size_t e = n;
    while (e > 0 && w[e - 1] != L'.' && w[e - 1] != L'\\' && w[e - 1] != L'/') e--;
    return e >= 3 && w[e - 1] == L'.' && w[e - 2] != L'\\' && w[e - 2] != L'/' && n > e && n - e <= 10;
}
static int content_file(const wchar_t *w, size_t n) {
    static const wchar_t *const NO[] = { L".exe", L".dll", L".vst", L".vst3", L".clap", L".aaxplugin", L".component",
        L".sys", L".drv", L".ocx", L".cpl", L".bat", L".cmd", L".ps1", L".vbs", L".com", L".scr", L".msi", L".lnk",
        L".url", L".lic", L".key", L".pem", L".pfx", L".p12" };
    size_t i, k;
    if (!named_file(w, n)) return 0;
    if (n > 11 && _wcsnicmp(w + 1, L":\\Windows\\", 10) == 0) return 0;
    for (i = 0; i < sizeof(NO) / sizeof(NO[0]); i++) {
        k = wcslen(NO[i]);
        if (n > k && _wcsnicmp(w + n - k, NO[i], k) == 0) return 0;
    }
    return 1;
}
// Media that may be named even where it isn't on this PC (a moved sample): there the extension ends it.
static int audio_ext(const wchar_t *w, size_t n) {
    static const wchar_t *const EXT[] = { L".wav", L".mp3", L".ogg", L".flac", L".aif", L".aiff", L".wv", L".rx2", L".rex", L".sf2" };
    size_t i, k;
    for (i = 0; i < sizeof(EXT) / sizeof(EXT[0]); i++) {
        k = wcslen(EXT[i]);
        if (n > k && _wcsnicmp(w + n - k, EXT[i], k) == 0) return 1;
    }
    return 0;
}
// `units` of path text (wide: UTF-16LE chars; else bytes: UTF-8 when valid, else the ANSI code page) -> out.
static int path_text(const unsigned char *s, size_t units, int wide, wchar_t *out, int cap) {
    int m;
    if (units + 1 > (size_t)cap) return 0;
    if (wide) { memcpy(out, s, units * 2); out[units] = 0; return 1; }
    m = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)s, (int)units, out, cap - 1);
    if (m <= 0) m = MultiByteToWideChar(CP_ACP, 0, (const char *)s, (int)units, out, cap - 1);
    if (m <= 0) return 0;
    out[m] = 0;
    return 1;
}
// path as bytes: enc 0 UTF-16LE, 1 UTF-8, 2 the ANSI code page (0 = not representable there).
static int path_bytes(const wchar_t *w, int enc, unsigned char *out, int cap) {
    BOOL lossy = FALSE;
    int k = (int)wcslen(w), m;
    if (enc == 0) { if (k * 2 > cap) return 0; memcpy(out, w, (size_t)k * 2); return k * 2; }
    if (enc == 1) return WideCharToMultiByte(CP_UTF8, 0, w, k, (char *)out, cap, NULL, NULL);
    m = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, k, (char *)out, cap, NULL, &lossy);
    return lossy ? 0 : m;
}
// [tag u32][length u64][payload] records exactly covering v[a, b)?
static int tlv_valid(const unsigned char *v, size_t a, size_t b) {
    if (a >= b) return 0;
    while (a < b) {
        uint64_t ln;
        if (b - a < 12) return 0;
        ln = *(const uint64_t *)(v + a + 4);
        if (ln > b - a - 12) return 0;
        a += 12 + (size_t)ln;
    }
    return 1;
}
// Image-Line plugin data: a 4-byte header (FPC) or none, then TLV records. -1 = not that layout.
static int tlv_start(const unsigned char *v, size_t n) {
    if (tlv_valid(v, 4, n)) return 4;
    if (tlv_valid(v, 0, n)) return 0;
    return -1;
}
// A TLV payload that is a sample path (UTF-16, optional trailing NUL) -> out. 0 = not one.
static int tlv_path(const unsigned char *p, size_t n, wchar_t *out, int cap) {
    size_t k = n / 2, i;
    if (n < 8 || (n & 1) || k >= (size_t)cap) return 0;
    memcpy(out, p, k * 2);
    out[k] = 0;
    if (k && !out[k - 1]) k--;
    for (i = 0; i < k; i++) if (out[i] < 32) return 0;
    out[k] = 0;
    return (is_abs_path(out) || out[0] == L'%') && content_file(out, k);
}
static void tlv_collect(const unsigned char *v, size_t a, size_t b, int depth, PathList *L) {
    wchar_t w[SAMPLE_PATH_MAX];
    while (a < b) {
        size_t s = a + 12, e = s + (size_t)*(const uint64_t *)(v + a + 4);
        if (tlv_path(v + s, e - s, w, SAMPLE_PATH_MAX)) pathlist_add(L, w);
        else if (depth < 8 && tlv_valid(v, s, e)) tlv_collect(v, s, e, depth + 1, L);
        a = e;
    }
}
// Plain paths anywhere in the data ("X:\..." or "X:/...", "\\server\...", "%FLStudioUserData%\..."),
// as UTF-16LE, UTF-8 or ANSI text -> out; returns the chars (wide) or bytes it covers, 0 = none.
// Where it ends, first match wins:
//  - a length right before it: 8 or 4 bytes (a VST wrapper's records, many plugins' own formats; for
//    UTF-16 in chars or bytes) or one byte (a Delphi short string: SF2 Player), when that cut leaves
//    "name.ext" and no letter follows. That is the path, even when it's a plugin binary (then none);
//  - the end of the text;
//  - text running on into other data (SF2 Player's 0xFF filler): the longest beginning that is a file
//    here or ends in a media extension.
static size_t raw_path_at(const unsigned char *v, size_t i, size_t n, int wide, wchar_t *out, int cap) {
    size_t k = 0, st = wide ? 2 : 1, len, c[4];
    wchar_t full[SAMPLE_PATH_MAX];
    int j;
    while (i + (k + 1) * st <= n && k + 1 < (size_t)cap) {
        unsigned ch = wide ? *(const uint16_t *)(v + i + k * st) : v[i + k * st];
        if (ch < 32 || ch == '"' || ch == '*' || ch == '?' || ch == '<' || ch == '>' || ch == '|') break;
        k++;
    }
    c[0] = i >= 8 ? (size_t)*(const uint64_t *)(v + i - 8) : 0;
    c[1] = i >= 4 ? *(const uint32_t *)(v + i - 4) : 0;
    c[2] = wide ? c[0] / 2 : (i >= 1 ? v[i - 1] : 0);
    c[3] = wide ? c[1] / 2 : 0;
    for (j = 0; j < 4; j++) {
        unsigned next;
        if (c[j] < 4 || c[j] > k || !path_text(v + i, c[j], wide, out, cap) || !named_file(out, wcslen(out))) continue;
        next = c[j] == k ? 0 : (wide ? *(const uint16_t *)(v + i + c[j] * st) : v[i + c[j]]);
        if (iswalpha((wint_t)next) && next < 128) continue;
        if (content_file(out, wcslen(out))) return c[j];
        out[0] = 0;
        return 0;
    }
    if (i + k * st >= n || (wide ? *(const uint16_t *)(v + i + k * st) : v[i + k * st]) < 32)
        if (path_text(v + i, k, wide, out, cap) && content_file(out, wcslen(out))) return k;
    for (len = k; len > 4; len--)
        if (path_text(v + i, len, wide, out, cap) && content_file(out, wcslen(out)) &&
            (audio_ext(out, wcslen(out)) || (!is_net_path(out) && expand_path(out, full, SAMPLE_PATH_MAX) && file_exists_w(full)))) return len;   // (received states are walked too)
    out[0] = 0;
    return 0;
}
static int raw_starts(const unsigned char *v, size_t i, size_t n, int wide) {
    static const char UD[] = "%flstudiouserdata%";
    size_t st = wide ? 2 : 1, j;
    unsigned c[3];
    if (i + 3 * st > n) return 0;
    for (j = 0; j < 3; j++) c[j] = wide ? *(const uint16_t *)(v + i + j * st) : v[i + j * st];
    if (wide && (v[i + 1] || v[i + 3])) return 0;
    if (((c[0] | 0x20) >= 'a' && (c[0] | 0x20) <= 'z' && c[1] == ':' && (c[2] == '\\' || c[2] == '/')) || (c[0] == '\\' && c[1] == '\\'))
        return 1;
    if (c[0] != '%' || i + (sizeof(UD) - 1) * st > n) return 0;
    for (j = 0; j < sizeof(UD) - 1; j++)
        if (((wide ? *(const uint16_t *)(v + i + j * st) : v[i + j * st]) | 0x20) != (unsigned)UD[j]) return 0;
    return 1;
}
static void raw_collect(const unsigned char *v, size_t n, PathList *L) {
    wchar_t w[SAMPLE_PATH_MAX];
    size_t i, k;
    int wide;
    for (wide = 1; wide >= 0; wide--)
        for (i = 0; i + 8 < n; i++)
            if (raw_starts(v, i, n, wide) && (k = raw_path_at(v, i, n, wide, w, SAMPLE_PATH_MAX)) != 0) {
                pathlist_add(L, w);
                i += k * (wide ? 2 : 1) - 1;
            }
}

static int collect_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    PathList *L = (PathList *)ctx;
    wchar_t w[SAMPLE_PATH_MAX];
    (void)at; (void)end;
    if (id == EV_PLUGIN_DATA && n >= 12) {
        int st = tlv_start(p, n);
        if (st >= 0) tlv_collect(p, (size_t)st, n, 0, L);
        raw_collect(p, n, L);
        return 1;
    }
    if (id != EV_SAMPLE_PATH || n < 4) return 1;
    ev_wstr(p, n, w, SAMPLE_PATH_MAX);
    pathlist_add(L, w);
    return 1;
}
static void pathlist_free(PathList *L) {
    int i;
    for (i = 0; i < L->n; i++) free(L->paths[i]);
    free(L->paths);
    memset(L, 0, sizeof(*L));
}
// A saved state that uses a sample which is still a placeholder here, or a shared folder file that
// isn't here yet.
static int state_has_placeholder(const unsigned char *d, size_t n) {
    PathList L;
    char h[65];
    uint64_t s;
    int i, r = 0;
    wchar_t full[SAMPLE_PATH_MAX];
    memset(&L, 0, sizeof(L));
    flp_walk(d, n, collect_cb, &L);
    for (i = 0; i < L.n && !r; i++)
        r = placeholder_of(L.paths[i], h, &s) ||
            (expand_path(L.paths[i], full, SAMPLE_PATH_MAX) && in_shared(full) && !file_exists_w(full));
    pathlist_free(&L);
    return r;
}

typedef struct { unsigned char *o; size_t n, cap; int changed; const unsigned char *d; } RewriteCtx;
static void rw_put(RewriteCtx *c, const void *p, size_t n) {
    if (c->n + n > c->cap) { c->cap = (c->n + n) * 2 + 1024; c->o = (unsigned char *)realloc(c->o, c->cap); }
    memcpy(c->o + c->n, p, n);
    c->n += n;
}
static void rw_event(RewriteCtx *c, int id, const unsigned char *p, size_t len) {
    unsigned char b = (unsigned char)id;
    size_t v = len;
    rw_put(c, &b, 1);
    do { b = (unsigned char)(v & 0x7f); v >>= 7; if (v) b |= 0x80; rw_put(c, &b, 1); } while (v);
    rw_put(c, p, len);
}
static const wchar_t *map_find(const wchar_t *from) {   // caller holds g_sampleLock
    int i;
    for (i = 0; i < g_npmap; i++) if (wcscmp(g_pmap[i].from, from) == 0) return g_pmap[i].to;
    return NULL;
}
// TLV records of v[a, b) into c, mapped path leaves replaced (lengths follow). Caller holds g_sampleLock.
static void tlv_rewrite(const unsigned char *v, size_t a, size_t b, int depth, RewriteCtx *c) {
    wchar_t w[SAMPLE_PATH_MAX];
    while (a < b) {
        size_t s = a + 12, e = s + (size_t)*(const uint64_t *)(v + a + 4);
        const wchar_t *to = tlv_path(v + s, e - s, w, SAMPLE_PATH_MAX) ? map_find(w) : NULL;
        RewriteCtx sub;
        uint64_t ln;
        memset(&sub, 0, sizeof(sub));
        if (to) {
            int nul = e - s >= 2 && !v[e - 1] && !v[e - 2];
            rw_put(&sub, to, wcslen(to) * 2);
            if (nul) rw_put(&sub, L"", 2);
            c->changed = 1;
        } else if (depth < 8 && tlv_valid(v, s, e)) {
            sub.d = v;
            tlv_rewrite(v, s, e, depth + 1, &sub);
            if (sub.changed) c->changed = 1;
        } else rw_put(&sub, v + s, e - s);
        ln = sub.n;
        rw_put(c, v + a, 4);
        rw_put(c, &ln, 8);
        if (sub.n) rw_put(c, sub.o, sub.n);
        free(sub.o);
        a = e;
    }
}
static void samples_dir(wchar_t *out, int cap);
// A local name for `to` that is exactly `len` characters long (plain paths in plugin data
// keep their length): <dir>\<folder>\<name>, a hard link (or copy) of the file, where dir is the
// song folder `to` is in when the name fits there, else flim's samples folder (a shorter path).
static int alias_in(const wchar_t *dir, const wchar_t *to, size_t len, wchar_t *out, int cap);
static int alias_path(const wchar_t *to, size_t len, wchar_t *out, int cap) {
    wchar_t dir[MAX_PATH], proj[MAX_PATH];
    const wchar_t *sl = wcsrchr(to, L'\\');
    flimcfg_projects_dir(proj, MAX_PATH);
    if (sl && _wcsnicmp(to, proj, wcslen(proj)) == 0 && to[wcslen(proj)] == L'\\' && (size_t)(sl - to) < MAX_PATH) {
        wcsncpy_s(dir, MAX_PATH, to, (size_t)(sl - to));
        if (alias_in(dir, to, len, out, cap)) return 1;
    }
    samples_dir(dir, MAX_PATH);
    return alias_in(dir, to, len, out, cap);
}
static int alias_in(const wchar_t *dir, const wchar_t *to, size_t len, wchar_t *out, int cap) {
    wchar_t name[MAX_PATH], folder[MAX_PATH], h[17];
    const wchar_t *b = to + wcslen(to), *ext;
    size_t fixed, k, i;
    uint64_t fh = 1469598103934665603ull;
    while (b > to && b[-1] != L'\\') b--;
    ext = wcsrchr(b, L'.');
    wcscpy_s(name, MAX_PATH, b);
    fixed = wcslen(dir) + 2;                                // two backslashes
    if (fixed + wcslen(name) + 1 > len) {                   // too long: a short name keeps the extension
        swprintf_s(name, MAX_PATH, L"s%s", ext ? ext : L"");
        if (fixed + wcslen(name) + 1 > len) return 0;
    }
    k = len - fixed - wcslen(name);
    if (k >= MAX_PATH || len >= (size_t)cap) return 0;
    for (i = 0; to[i]; i++) { fh ^= to[i]; fh *= 1099511628211ull; }
    swprintf_s(h, 17, L"%016llx", (unsigned long long)fh);
    for (i = 0; i < k; i++) folder[i] = i < 16 ? h[i] : L'_';
    folder[k] = 0;
    swprintf_s(out, cap, L"%s\\%s\\%s", dir, folder, name);
    if (wcslen(out) != len) return 0;
    if (file_exists_w(out)) {                               // made earlier: must be this file (short names can collide)
        uint64_t sa, sb;
        if (!file_info(out, &sa, NULL) || !file_info(to, &sb, NULL) || sa != sb) return 0;
    } else {
        wchar_t d2[MAX_PATH * 2];
        swprintf_s(d2, MAX_PATH * 2, L"%s\\%s", dir, folder);
        SHCreateDirectoryExW(NULL, d2, NULL);
        if (!CreateHardLinkW(out, to, NULL) && !CopyFileW(to, out, TRUE)) return 0;
    }
    return 1;
}
// Plain paths in plugin data -> o: the exact strings the sender listed (g_pmap), as UTF-16LE, UTF-8 or
// ANSI text, become a local name of the same length; one after a one-byte length (a Delphi short string,
// SF2 Player) becomes the local file itself when no same-length name fits (only outside Image-Line's TLV
// layout, whose lengths that would break). Caller holds g_sampleLock. 1 = changed.
typedef struct { unsigned char *b; int n, enc; const wchar_t *from, *to; } PathPat;
static int raw_rewrite(const unsigned char *v, size_t n, int shortOk, RewriteCtx *o) {
    PathPat *P = (PathPat *)malloc(sizeof(PathPat) * (size_t)(g_npmap * 3 + 1));
    int np = 0, m, enc, changed = 0;
    size_t i = 0, done = 0;
    for (m = 0; m < g_npmap; m++) {
        unsigned char b[3][SAMPLE_PATH_MAX * 3];
        int bn[3];
        for (enc = 0; enc < 3; enc++) bn[enc] = path_bytes(g_pmap[m].from, enc, b[enc], sizeof(b[enc]));
        if (bn[2] == bn[1] && memcmp(b[2], b[1], (size_t)bn[1]) == 0) bn[2] = 0;   // ANSI = UTF-8 (plain ASCII)
        for (enc = 0; enc < 3; enc++) {
            if (bn[enc] < 8) continue;
            P[np].b = (unsigned char *)malloc((size_t)bn[enc]);
            memcpy(P[np].b, b[enc], (size_t)bn[enc]);
            P[np].n = bn[enc]; P[np].enc = enc; P[np].from = g_pmap[m].from; P[np].to = g_pmap[m].to;
            np++;
        }
    }
    while (i + 8 < n) {
        PathPat *q = NULL;
        int k;
        if (raw_starts(v, i, n, 1) || raw_starts(v, i, n, 0))
            for (k = 0; k < np && !q; k++)
                if (i + P[k].n <= n && memcmp(v + i, P[k].b, P[k].n) == 0) q = &P[k];
        if (!q) { i++; continue; }
        {
            unsigned char nb[SAMPLE_PATH_MAX * 3];
            wchar_t al[SAMPLE_PATH_MAX];
            size_t len = q->enc ? (size_t)q->n : wcslen(q->from), j;
            int ascii = 1, tn;
            if (alias_path(q->to, len, al, SAMPLE_PATH_MAX)) for (j = 0; j < len; j++) ascii &= q->enc == 0 || al[j] < 128;
            else ascii = -1;
            if (ascii == 1) {
                rw_put(o, v + done, i - done);
                if (q->enc) for (j = 0; j < len; j++) { unsigned char c = (unsigned char)al[j]; rw_put(o, &c, 1); }
                else rw_put(o, al, len * 2);
                {   // an alias of a placeholder is a placeholder too (it is a hard link: it fills in with it)
                    char ph[65];
                    uint64_t phs;
                    if (placeholder_of(q->to, ph, &phs)) placeholder_add(al, ph, phs);
                }
                logmsg("SAMPLES plugin data: %S -> %S", q->from, al);
            } else if (q->enc && shortOk && i > 0 && v[i - 1] == q->n && (tn = path_bytes(q->to, q->enc, nb, sizeof(nb))) > 0 && tn < 256) {
                unsigned char lb = (unsigned char)tn;
                rw_put(o, v + done, i - 1 - done);
                rw_put(o, &lb, 1);
                rw_put(o, nb, (size_t)tn);
                logmsg("SAMPLES plugin data: %S -> %S (its length byte follows)", q->from, q->to);
            } else {
                logmsg("SAMPLES %S: no same-length local name, it will be missing", q->from);
                i += (size_t)q->n;
                continue;
            }
            changed = 1;
            i += (size_t)q->n;
            done = i;
        }
    }
    if (changed) rw_put(o, v + done, n - done);
    for (m = 0; m < np; m++) free(P[m].b);
    free(P);
    return changed;
}

// Caller holds g_sampleLock.
static int rewrite_cb(void *ctx, int id, const unsigned char *p, size_t n, size_t at, size_t end) {
    RewriteCtx *c = (RewriteCtx *)ctx;
    wchar_t w[SAMPLE_PATH_MAX];
    int i;
    if (id == EV_PLUGIN_DATA && n >= 12) {
        RewriteCtx t, r;
        int st = tlv_start(p, n), ch = 0;
        memset(&t, 0, sizeof(t));
        memset(&r, 0, sizeof(r));
        if (st >= 0) {
            t.d = p;
            rw_put(&t, p, (size_t)st);
            tlv_rewrite(p, (size_t)st, n, 0, &t);
            ch = t.changed;
        } else rw_put(&t, p, n);
        if (raw_rewrite(t.o, t.n, st < 0, &r)) { free(t.o); t = r; ch = 1; }
        else free(r.o);
        if (ch) { rw_event(c, id, t.o, t.n); c->changed = 1; }
        else rw_put(c, c->d + at, end - at);
        free(t.o);
        return 1;
    }
    if (id == EV_SAMPLE_PATH && n >= 4) {
        ev_wstr(p, n, w, SAMPLE_PATH_MAX);
        for (i = 0; i < g_npmap; i++) {
            if (wcscmp(g_pmap[i].from, w) != 0) continue;
            rw_event(c, id, (const unsigned char *)g_pmap[i].to, (wcslen(g_pmap[i].to) + 1) * 2);   // UTF-16LE + NUL
            c->changed = 1;
            return 1;
        }
    }
    rw_put(c, c->d + at, end - at);
    return 1;
}

// Receiver: point the sample paths in an inbound .flp / .fst image at the local files.
static void samples_rewrite(unsigned char **d, size_t *n) {
    RewriteCtx c;
    size_t start;
    int ok;
    if (!g_npmap || !*d || *n < 22 || memcmp(*d, "FLhd", 4) != 0) return;
    start = 8 + *(const uint32_t *)(*d + 4) + 8;
    if (start > *n) return;
    memset(&c, 0, sizeof(c));
    c.d = *d;
    rw_put(&c, *d, start);
    EnterCriticalSection(&g_sampleLock);
    ok = flp_walk(*d, *n, rewrite_cb, &c);
    LeaveCriticalSection(&g_sampleLock);
    if (!ok || !c.changed) { free(c.o); return; }
    *(uint32_t *)(c.o + start - 4) = (uint32_t)(c.n - start);        // FLdt chunk length
    logmsg("SAMPLES paths rewritten to local files (%zu -> %zu bytes)", *n, c.n);
    free(*d);
    *d = c.o;
    *n = c.n;
}

// The shared folder (flimcfg_shared_dir; the "shared folder" section): p (expanded) is in it.
static wchar_t g_shRoot[MAX_PATH];
static int in_shared(const wchar_t *p) {
    size_t n = wcslen(g_shRoot), i;
    for (i = 0; i < n; i++)
        if (towlower(p[i] == L'/' ? L'\\' : p[i]) != towlower(g_shRoot[i])) return 0;
    return n && (p[n] == L'\\' || p[n] == L'/');
}

// ---- projects opened from a zip ----
// FL opens a zipped project by extracting it to %TEMP%\Image-Line\<pid>-{guid}\Zip\ and deletes that
// folder later: after a crash or a restart the project's samples are gone. Leader, after such a load
// (main thread): every sample the project uses from FL's temp folder is copied into the song folder,
// a session copy pointing at those copies is written there and FL opens it (that load is the one sent
// to the members). Returns 1 if it took over.
static int in_fl_temp(const wchar_t *p) {
    wchar_t t[MAX_PATH];
    size_t n = GetTempPathW(MAX_PATH, t);
    return n && _wcsnicmp(p, t, n) == 0 && _wcsnicmp(p + n, L"Image-Line\\", 11) == 0;
}
static int temp_project_rescue(const wchar_t *cur) {
    wchar_t dir[MAX_PATH], flp[MAX_PATH * 2], name[MAX_PATH], dst[MAX_PATH * 2];
    PathList L;
    size_t n = 0;
    unsigned char *d = project_snapshot(&n);
    int i, k = 0;
    if (!d) return 0;
    memset(&L, 0, sizeof(L));
    flp_walk(d, n, collect_cb, &L);
    song_dir_of(cur, dir, MAX_PATH);
    for (i = 0; i < L.n; i++) {
        const wchar_t *z;
        if (!in_fl_temp(L.paths[i]) || !file_exists_w(L.paths[i])) continue;
        z = wcsstr(L.paths[i], L"\\Zip\\");                     // keep the zip's own subfolders
        swprintf_s(dst, MAX_PATH * 2, L"%s\\%s", dir, z ? z + 5 : base_name(L.paths[i]));
        { wchar_t pd[MAX_PATH * 2], *sl; wcscpy_s(pd, MAX_PATH * 2, dst); if ((sl = wcsrchr(pd, L'\\')) != NULL) { *sl = 0; SHCreateDirectoryExW(NULL, pd, NULL); } }
        if (!CopyFileW(L.paths[i], dst, TRUE) && GetLastError() != ERROR_FILE_EXISTS) { logmsg("PROJECT could not copy %S (error %lu)", L.paths[i], GetLastError()); continue; }
        map_set(L.paths[i], dst);
        k++;
    }
    pathlist_free(&L);
    if (!k && !ends_with_ci(cur, L".zip")) { free(d); return 0; }   // a zip always: its "file" is no .flp to work on
    samples_rewrite(&d, &n);
    song_of(cur, name, MAX_PATH);
    wcscat_s(name, MAX_PATH, L".flp");
    flim_name(name, flp + 0, MAX_PATH);                       // <song>_flim_MMDD_hhmm.flp
    wcscpy_s(name, MAX_PATH, flp);
    swprintf_s(flp, MAX_PATH * 2, L"%s\\%s", dir, name);
    if (!write_file_w(flp, d, n)) { logmsg("PROJECT could not write %S", flp); free(d); return 0; }
    free(d);
    SHCreateDirectoryExW(NULL, dir, NULL);
    logmsg("PROJECT opened from a zip: its %d samples copied out of FL's temp folder into %S; reopening it from there", k, dir);
    InterlockedExchange(&g_projBusy, 1);                     // like a local open: its end sends it
    InterlockedExchange(&g_projRemote, 0);
    InterlockedExchange(&g_projDoneQueued, 0);
    g_projSince = GetTickCount();
    {
        QItem q; memset(&q, 0, sizeof(q));
        q.type = T_PROJLOAD; q.mode = 1; q.path = (char *)_wcsdup(flp);
        enqueue(&q);
    }
    return 1;
}

// ---- blob store (HTTP) ----
// One request to the server's blob store. Returns the HTTP status (0 = failed).
//   up/upSize: PUT body streamed from this file; body/bodyN: small in-memory body;
//   down: response body written to this file (downHex = its sha256); resp: small text response.
static DWORD blob_http(const wchar_t *verb, const char *what, const wchar_t *up, uint64_t upSize,
                       const char *body, DWORD bodyN, const wchar_t *down, char *downHex, char *resp, int respCap) {
    NetUrl u;
    char key[65];
    wchar_t path[700], wwhat[200], hdr[128];
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sl = sizeof(status), err = 0;
    unsigned char *buf = NULL;
    HANDLE f = INVALID_HANDLE_VALUE;
    size_t pl;
    EnterCriticalSection(&g_sampleLock);
    if (!g_blobOn) { LeaveCriticalSection(&g_sampleLock); return 0; }
    u = g_blobUrl;
    strcpy_s(key, sizeof(key), g_blobKey);
    LeaveCriticalSection(&g_sampleLock);
    wcscpy_s(path, 700, u.path);
    pl = wcslen(path);
    while (pl && path[pl - 1] == L'/') path[--pl] = 0;
    MultiByteToWideChar(CP_UTF8, 0, what, -1, wwhat, 200);
    wcscat_s(path, 700, wwhat);
    swprintf_s(hdr, 128, L"x-flim-key: %S\r\n", key);
    ses = WinHttpOpen(L"flim/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) ses = WinHttpOpen(L"flim/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto out;
    WinHttpSetTimeouts(ses, 0, 15000, 120000, 120000);
    con = WinHttpConnect(ses, u.host, u.port, 0);
    if (!con) goto out;
    req = WinHttpOpenRequest(con, verb, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) goto out;
    buf = (unsigned char *)malloc(BLOB_CHUNK);
    if (up) {
        uint64_t left = upSize;
        f = CreateFileW(up, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) goto out;
        if (!WinHttpSendRequest(req, hdr, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, (DWORD)upSize, 0)) goto out;
        while (left) {
            DWORD want = left > BLOB_CHUNK ? BLOB_CHUNK : (DWORD)left, got = 0, wr = 0;
            if (!ReadFile(f, buf, want, &got, NULL) || !got) goto out;
            if (!WinHttpWriteData(req, buf, got, &wr)) goto out;
            left -= got;
            g_upDone += got;
            xfer_show(0);
        }
        CloseHandle(f);
        f = INVALID_HANDLE_VALUE;
    } else if (!WinHttpSendRequest(req, hdr, (DWORD)-1L, (LPVOID)body, bodyN, bodyN, 0)) goto out;
    if (!WinHttpReceiveResponse(req, NULL)) goto out;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &sl, WINHTTP_NO_HEADER_INDEX);
    if (status == 200 && down) {
        Sha s;
        f = CreateFileW(down, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE || !sha_begin(&s)) { err = GetLastError(); status = 0; goto out; }
        for (;;) {
            DWORD got = 0, wr = 0;
            if (!WinHttpReadData(req, buf, BLOB_CHUNK, &got)) { err = GetLastError(); status = 0; break; }
            if (!got) break;
            sha_add(&s, buf, got);
            if (!WriteFile(f, buf, got, &wr, NULL) || wr != got) { err = GetLastError(); status = 0; break; }
            g_dnDone += got;
            xfer_show(0);
        }
        sha_end(&s, downHex);
    } else if (resp && respCap > 0) {
        int o = 0;
        for (;;) {
            DWORD got = 0;
            if (o >= respCap - 1 || !WinHttpReadData(req, resp + o, (DWORD)(respCap - 1 - o), &got) || !got) break;
            o += (int)got;
        }
        resp[o] = 0;
    }
out:
    if (!status) logmsg("SAMPLES %S %s failed (%lu)", verb, what, err ? err : GetLastError());
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    free(buf);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return status;
}

// ---- big messages ----
// An op over BIG_MSG (a project, a plugin state, the join answers plug.full / state.full) doesn't go
// through the WebSocket: on a slow link a 3 MB message there takes half a minute, and meanwhile FL's
// main thread waits to send anything, and the server, whose pings can't get through, hangs up. The
// outbox thread puts it into the blob store (streamed over HTTP, like samples) and sends
//   {"t":"op","op":{"kind":"big","h":"<sha256 of the message>","n":<bytes>[,"key":"<its lock key>"]}}
// (the key stays readable: the server drops ops under someone else's lock). The receiver's inbox
// thread fetches it and handles it as if it had come as is; later ops wait behind it.
#define BIG_MSG (32 * 1024)
static int is_big_ref(const char *m) { return strstr(m, "\"kind\":\"big\"") && strstr(m, "\"t\":\"op\""); }

static void big_path(const char *hex, wchar_t *out) { swprintf_s(out, MAX_PATH, L"%S\\big_%.16S.txt", g_tmpDir, hex); }

// Outbox thread: the reference to send instead of msg (malloc'd), or NULL to send msg itself.
static char *big_put(const char *msg) {
    static const char OP[] = "{\"t\":\"op\",\"op\":{";
    size_t n = strlen(msg);
    char hex[65], what[80], key[160] = "", *ref;
    const char *k;
    wchar_t tmp[MAX_PATH];
    DWORD st;
    Sha s;
    if (n <= BIG_MSG || !g_blobOn || strncmp(msg, OP, sizeof(OP) - 1) != 0 || !sha_begin(&s)) return NULL;
    sha_add(&s, msg, n);
    sha_end(&s, hex);
    sprintf_s(what, sizeof(what), "/blob/%s", hex);
    k = strstr(msg, "\"key\":\"");                      // the op's own lock key comes first, before its data
    if (k && k - msg < 300) {
        const char *e = strchr(k + 7, '"');
        if (e && e - k < (int)sizeof(key) - 1) { memcpy(key, k, (size_t)(e - k + 1)); key[e - k + 1] = 0; }
    }
    net_status(NET_ON, "Sending %.1f MB to the others...", n / 1048576.0);
    st = blob_http(L"HEAD", what, NULL, 0, NULL, 0, NULL, NULL, NULL, 0);    // the same state sent before
    if (st != 200) {
        big_path(hex, tmp);
        st = write_file_w(tmp, (const unsigned char *)msg, n) ? blob_http(L"PUT", what, tmp, n, NULL, 0, NULL, NULL, NULL, 0) : 0;
        DeleteFileW(tmp);
    }
    if (g_peersJson[0]) net_show_connected(g_peersJson);
    if (st != 200 && st != 201) { logmsg("NET a %zu-byte message couldn't go to the blob store (HTTP %lu): sent on the WebSocket", n, st); return NULL; }
    ref = (char *)malloc(400);
    sprintf_s(ref, 400, "{\"t\":\"op\",\"op\":{\"kind\":\"big\",\"h\":\"%s\",\"n\":%zu%s%s}}", hex, n, key[0] ? "," : "", key);
    logmsg("NET sent a %zu-byte message through the blob store (%.12s)", n, hex);
    return ref;
}

// The relayed reference ({"t":"op","seq":..,"from":..,"op":{"kind":"big"..}}) with its op replaced by
// the sender's real one ({"t":"op","op":{...}}): malloc'd, or NULL.
static char *op_splice(const char *ref, const char *real) {
    const char *o = strstr(ref, "\"op\":{"), *b = strstr(real, "\"op\":{");
    size_t pre, bn;
    char *out;
    if (!o || !b) return NULL;
    pre = (size_t)(o - ref); bn = strlen(b);
    out = (char *)malloc(pre + bn + 1);
    memcpy(out, ref, pre);
    memcpy(out + pre, b, bn + 1);
    return out;
}

// Inbox thread: the message ref stands for, with the server's "seq" / "from" kept (malloc'd), or NULL.
static char *big_get(const char *ref) {
    const char *h = strstr(ref, "\"h\":\""), *sz = strstr(ref, "\"n\":");
    char hex[65] = "", got[65] = "", what[80];
    wchar_t tmp[MAX_PATH];
    unsigned char *d = NULL;
    size_t n = 0;
    int tries;
    char *out = NULL, *b;
    if (!h || strlen(h) < 5 + 64) return NULL;
    memcpy(hex, h + 5, 64); hex[64] = 0;
    sprintf_s(what, sizeof(what), "/blob/%s", hex);
    big_path(hex, tmp);
    net_status(NET_ON, "Receiving a %.1f MB change...", (sz ? strtoull(sz + 4, NULL, 10) : 0) / 1048576.0);
    for (tries = 0; tries < 4 && !d; tries++) {                // the blob store is plain HTTP: retry a hiccup
        if (tries) Sleep(1000 * tries);
        if (blob_http(L"GET", what, NULL, 0, NULL, 0, tmp, got, NULL, 0) == 200 && strcmp(got, hex) == 0) d = read_file_w(tmp, &n);
        DeleteFileW(tmp);
    }
    if (g_peersJson[0]) net_show_connected(g_peersJson);
    if (!d) goto fail;
    b = (char *)realloc(d, n + 1);
    if (!b) { free(d); goto fail; }
    b[n] = 0;
    out = op_splice(ref, b);
    free(b);
    if (!out) goto fail;
    logmsg("NET received a %zu-byte message through the blob store (%.12s)", n, hex);
    return out;
fail:
    logmsg("NET a change from the others (%.12s) couldn't be downloaded: it's missing here", hex);
    gui_push(GUI_LOG, "A change from the others couldn't be downloaded from the server: it's missing here (Connect again to resync).");
    return NULL;
}

// ---- sender ----
typedef struct { const wchar_t *p; wchar_t *full; char hex[65]; uint64_t size, ms; int upload, skip, ph; } SEnt;

// Queue a file for the upload thread (once per content). front: ahead of what's waiting (a sample a
// change uses goes before the shared folder's bulk).
static char g_upNow[65];                                         // the content upThread is sending (g_sampleLock)
static void upload_queue(const wchar_t *full, const char *hex, uint64_t size, int front) {
    XJob *j, **t;
    EnterCriticalSection(&g_sampleLock);
    for (t = &g_upQ; *t; t = &(*t)->nx) if (strcmp((*t)->hex, hex) == 0) break;
    if (*t || strcmp(g_upNow, hex) == 0) { LeaveCriticalSection(&g_sampleLock); return; }
    j = (XJob *)calloc(1, sizeof(XJob));
    strcpy_s(j->hex, 65, hex);
    j->size = size;
    wcsncpy_s(j->path, MAX_PATH * 2, full, _TRUNCATE);
    if (front) { j->nx = g_upQ; g_upQ = j; } else *t = j;
    g_upTotal += size;
    LeaveCriticalSection(&g_sampleLock);
    SetEvent(g_upEvt);
}

static DWORD WINAPI upThread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_upEvt, INFINITE);
        for (;;) {
            XJob *j;
            char what[80];
            DWORD st;
            EnterCriticalSection(&g_sampleLock);
            j = g_upQ;
            if (j) { g_upQ = j->nx; strcpy_s(g_upNow, sizeof(g_upNow), j->hex); }
            else g_upDone = g_upTotal = 0;
            LeaveCriticalSection(&g_sampleLock);
            if (!j) { xfer_end(); break; }
            xfer_show(1);
            sprintf_s(what, sizeof(what), "/blob/%s", j->hex);
            st = blob_http(L"PUT", what, j->path, j->size, NULL, 0, NULL, NULL, NULL, 0);
            EnterCriticalSection(&g_sampleLock);
            g_upNow[0] = 0;
            LeaveCriticalSection(&g_sampleLock);
            if (st == 200 || st == 201) logmsg("SAMPLES uploaded %S (%llu bytes)", j->path, (unsigned long long)j->size);
            else logmsg("SAMPLES upload of %S failed (HTTP %lu): the others keep a placeholder", j->path, st);
            free(j);
        }
    }
}

static void group_digits(uint64_t v, wchar_t *out, int cap) {
    wchar_t t[32];
    int n, i, o = 0;
    swprintf_s(t, 32, L"%llu", (unsigned long long)v);
    n = (int)wcslen(t);
    for (i = 0; i < n && o < cap - 2; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = L',';
        out[o++] = t[i];
    }
    out[o] = 0;
}

// Outbox thread: list the samples an outgoing project / channel state uses (and queue the
// uploads the server needs; the op doesn't wait for them, receivers use placeholders meanwhile).
// Returns the message with "samples":[...] spliced in, or NULL to send it unchanged.
// The file a sample path names, or where FL itself finds a moved one: the same file name in the project's
// folder (a project from someone else's PC or zip keeps their paths). proj = the project file ('' = none).
static int sample_locate(const wchar_t *full, const wchar_t *proj, wchar_t *out, int cap) {
    const wchar_t *name = wcsrchr(full, L'\\'), *slash = wcsrchr(proj, L'\\');
    if (file_exists_w(full)) return wcscpy_s(out, cap, full) == 0;
    if (!name || !slash) return 0;
    if (_snwprintf_s(out, cap, _TRUNCATE, L"%.*s%s", (int)(slash - proj), proj, name) < 0) return 0;
    return file_exists_w(out);
}

static char *samples_attach(const char *msg, const unsigned char *blob, size_t n) {
    PathList L;
    SEnt *E = NULL;
    int i, j, ne = 0, nup = 0;
    uint64_t upBytes = 0;
    char *out = NULL;
    wchar_t proj[MAX_PATH];
    memset(&L, 0, sizeof(L));
    flp_walk(blob, n, collect_cb, &L);
    if (!L.n || !g_blobOn) goto done;
    E = (SEnt *)calloc((size_t)L.n, sizeof(SEnt));
    fl_ustr(PROJNAME_VA, proj, MAX_PATH);
    for (i = 0; i < L.n; i++) {
        wchar_t full[SAMPLE_PATH_MAX], found[SAMPLE_PATH_MAX];
        if (!expand_path(L.paths[i], full, SAMPLE_PATH_MAX)) continue;
        // still a placeholder here: list the real sample (someone else is uploading it)
        if (placeholder_of(full, E[ne].hex, &E[ne].size)) { E[ne].ph = 1; wcscpy_s(found, SAMPLE_PATH_MAX, full); }
        else if (!sample_locate(full, proj, found, SAMPLE_PATH_MAX) || !sample_hash(found, E[ne].hex, &E[ne].size)) {
            logmsg("SAMPLES %S: no such file here (or too big), not sent", full);
            continue;
        } else {
            if (wcscmp(found, full) != 0) logmsg("SAMPLES %S isn't there: sent the one in the project's folder", full);
            E[ne].ms = placeholder_format(found) == 3 ? compressed_ms(found) : wav_ms(found);
        }
        E[ne].p = L.paths[i];
        E[ne].full = _wcsdup(found);
        ne++;
    }
    if (!ne) goto done;
    {   // which ones does the server lack?
        char *body = (char *)malloc((size_t)ne * 65 + 1), *resp = (char *)malloc((size_t)ne * 65 + 16), *line, *ctx = NULL;
        int o = 0;
        DWORD st;
        for (i = 0; i < ne; i++) o += sprintf_s(body + o, (size_t)ne * 65 + 1 - o, "%s\n", E[i].hex);
        st = blob_http(L"POST", "/blobs/missing", NULL, 0, body, (DWORD)o, NULL, NULL, resp, ne * 65 + 16);
        if (st != 200) { logmsg("SAMPLES the server's sample store doesn't answer (HTTP %lu): sent without samples", st); free(body); free(resp); goto done; }
        for (line = strtok_s(resp, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx))
            for (i = 0; i < ne; i++) if (strcmp(E[i].hex, line) == 0 && !E[i].ph) E[i].upload = 1;
        free(body); free(resp);
    }
    for (i = 0; i < ne; i++) {
        if (!E[i].upload) continue;
        if (declined(E[i].hex)) { E[i].upload = 0; E[i].skip = 1; continue; }
        for (j = 0; j < i && !(E[j].upload && strcmp(E[j].hex, E[i].hex) == 0); j++) {}
        if (j < i) { E[i].upload = 0; continue; }        // same content listed twice: one upload
        upBytes += E[i].size;
        nup++;
    }
    if (upBytes > SAMPLE_ASK_BYTES) {
        wchar_t q[900], grouped[40];
        group_digits(upBytes, grouped, 40);
        swprintf_s(q, 900,
            L"The other people in this session don't have %d of the samples used here.\n\n"
            L"Sending them uploads %.1f MB (%s bytes).\n\n"
            L"Send them now?\n\n"
            L"Yes: upload them. The others hear silent placeholders until the upload is done;\n"
            L"your other changes go out right away.\n"
            L"No: don't send these samples. They will be missing for the others.",
            nup, upBytes / 1048576.0, grouped);
        logmsg("SAMPLES asking before uploading %llu bytes (%d files)", (unsigned long long)upBytes, nup);
        // Owned by FL's main window: an ownerless box from this background thread can't come to
        // the front and may open behind other windows. Flash FL's taskbar button in case FL isn't in front.
        if (g_mainForm) {
            FLASHWINFO fw = { sizeof(fw), g_mainForm, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0 };
            FlashWindowEx(&fw);
        }
        if (MessageBoxW(g_mainForm, q, L"flim: send samples?", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES) {
            for (i = 0; i < ne; i++) {
                if (!E[i].upload) continue;
                decline(E[i].hex);
                for (j = 0; j < ne; j++) if (strcmp(E[j].hex, E[i].hex) == 0) E[j].skip = 1;
                E[i].upload = 0;
            }
            logmsg("SAMPLES upload declined: %d files left out", nup);
            upBytes = 0; nup = 0;
        }
    }
    for (i = 0; i < ne; i++) if (E[i].upload) upload_queue(E[i].full, E[i].hex, E[i].size, 1);
    {   // splice "samples":[...] in front of the op's fields
        const char *op = strstr(msg, "\"op\":{");
        size_t cap = strlen(msg) + 64, len;
        int k = 0;
        if (!op) goto done;
        for (i = 0; i < ne; i++) cap += wcslen(E[i].p) * 3 + 160;
        out = (char *)malloc(cap);
        len = (size_t)(op + 6 - msg);
        memcpy(out, msg, len);
        len += sprintf_s(out + len, cap - len, "\"samples\":[");
        for (i = 0; i < ne; i++) {
            if (E[i].skip) continue;
            len += sprintf_s(out + len, cap - len, "%s{\"p\":\"", k++ ? "," : "");
            b64enc((const unsigned char *)E[i].p, (int)(wcslen(E[i].p) * 2), out + len);
            len += strlen(out + len);
            len += sprintf_s(out + len, cap - len, "\",\"h\":\"%s\",\"s\":%llu", E[i].hex, (unsigned long long)E[i].size);
            if (E[i].ms) len += sprintf_s(out + len, cap - len, ",\"ms\":%llu", (unsigned long long)E[i].ms);
            out[len++] = '}'; out[len] = 0;
        }
        len += sprintf_s(out + len, cap - len, "],");
        strcpy_s(out + len, cap - len, op + 6);
        logmsg("SAMPLES op lists %d samples (%d queued for upload)", k, nup);
    }
done:
    if (E) { for (i = 0; i < ne; i++) free(E[i].full); free(E); }
    pathlist_free(&L);
    return out;
}

static void out_push(char *msg, unsigned char *blob, size_t n) {   // caller holds g_outLock
    OutItem *it = (OutItem *)calloc(1, sizeof(OutItem));
    it->msg = msg; it->blob = blob; it->blobn = n;
    if (g_outTail) g_outTail->next = it; else g_outHead = it;
    g_outTail = it;
}

// Every op goes through here: straight out, unless something is queued in the outbox
// (an op waiting for its samples or its upload) -- then behind it, to keep the order. A big one
// always goes to the outbox ("big messages"). The leader's "alive" and the mouse arrows don't
// need the order: never behind an upload (a long one would look like a frozen leader).
// An op with this FL's structure counts in front of its fields ({"t":"op","op":{"sc":[ch,mix,pat],...});
// NULL: as it is.
static char *sc_stamp(const char *msg) {
    static const char OP[] = "{\"t\":\"op\",\"op\":{";
    LONG c = g_sc[SC_CH], x = g_sc[SC_MIX], p = g_sc[SC_PAT];
    size_t n;
    char *out;
    if (c < 0 || x < 0 || p < 0 || strncmp(msg, OP, sizeof(OP) - 1) != 0) return NULL;
    n = strlen(msg) + 80;
    out = (char *)malloc(n);
    sprintf_s(out, n, "%s\"sc\":[%ld,%ld,%ld],\"ep\":%ld,%s", OP, c, x, p, (LONG)g_projEpoch, msg + sizeof(OP) - 1);
    return out;
}

// Sync-only (host controls): what we may still send is asking for things; edits and locks stay here (the server
// drops them too). An edit kept back means this FL differs from the others: switched back, it reloads.
static int sync_only_keeps(const char *msg) {
    static const char OP[] = "{\"t\":\"op\",\"op\":{\"kind\":\"";
    const char *k = msg + sizeof(OP) - 1;
    if (!g_syncOnly) return 0;
    if (strncmp(msg, "{\"t\":\"lock_", 10) == 0) return 1;
    if (strncmp(msg, OP, sizeof(OP) - 1) != 0) return 0;
    if (!strncmp(k, "hello\"", 6) || !strncmp(k, "shared.need\"", 12)) return 0;
    if (!strncmp(k, "state.req\"", 10)) return !g_userResync;   // only the user's (every member reloads with it)
    if (op_changes_song(msg) && InterlockedIncrement(&g_syncKept) == 1) logmsg("SYNC sync-only: this edit stays in this FL (%.80s)", msg);
    return 1;
}

static void ws_send(const char *msg) {
    static const char ALIVE[] = "{\"t\":\"op\",\"op\":{\"kind\":\"alive\"", CUR[] = "{\"t\":\"op\",\"op\":{\"kind\":\"cur\"";
    char *st;
    if (sync_only_keeps(msg)) return;
    if (strncmp(msg, ALIVE, sizeof(ALIVE) - 1) == 0 || strncmp(msg, CUR, sizeof(CUR) - 1) == 0) { ws_send_now(msg); return; }
    if (member_reloading() && op_changes_song(msg)) {
        // the leader's project is on its way: an edit sent now reached the leader after its snapshot for us
        // and was wiped here by the load (the two then differed); the leader's song wins instead
        logmsg("SYNC an edit not sent: this FL is loading the leader's project (%.80s)", msg);
        return;
    }
    if ((st = sc_stamp(msg)) != NULL) msg = st;
    if (!g_isLeader && strstr(msg, "\"kind\":\"state.req\"")) g_projAskedAt = GetTickCount() | 1;   // (member_reloading)
    log_op("out", msg);
    if (op_changes_song(msg)) g_lastOpAt = GetTickCount();
    EnterCriticalSection(&g_outLock);
    if (g_outBusy || g_outHead || strlen(msg) > BIG_MSG) { out_push(_strdup(msg), NULL, 0); SetEvent(g_outEvt); }
    else ws_send_now(msg);
    LeaveCriticalSection(&g_outLock);
    free(st);
}

// An op whose data (an .flp / .fst image) may reference samples. Takes ownership of msg.
static void ws_send_samples(char *msg, const unsigned char *blob, size_t n) {
    unsigned char *copy;
    char *st;
    if (!g_ws || sync_only_keeps(msg)) { free(msg); return; }
    if (member_reloading()) { logmsg("SYNC an edit not sent: this FL is loading the leader's project (%.80s)", msg); free(msg); return; }   // (ws_send)
    if ((st = sc_stamp(msg)) != NULL) { free(msg); msg = st; }
    log_op("out", msg);
    g_lastOpAt = GetTickCount();
    copy = (unsigned char *)malloc(n ? n : 1);
    if (n) memcpy(copy, blob, n);
    EnterCriticalSection(&g_outLock);
    out_push(msg, copy, n);
    LeaveCriticalSection(&g_outLock);
    SetEvent(g_outEvt);
}

static DWORD WINAPI outThread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_outEvt, INFINITE);
        for (;;) {
            OutItem *it;
            EnterCriticalSection(&g_outLock);
            it = g_outHead;
            if (!it) { g_outBusy = 0; LeaveCriticalSection(&g_outLock); break; }
            g_outHead = it->next;
            if (!g_outHead) g_outTail = NULL;
            g_outBusy = 1;
            LeaveCriticalSection(&g_outLock);
            if (it->blob) {
                char *m2 = samples_attach(it->msg, it->blob, it->blobn);
                if (m2) { free(it->msg); it->msg = m2; }
            }
            {
                char *ref = big_put(it->msg);
                if (ref) { free(it->msg); it->msg = ref; }
            }
            EnterCriticalSection(&g_outLock);        // direct senders wait: nothing overtakes this one
            ws_send_now(it->msg);
            LeaveCriticalSection(&g_outLock);
            free(it->msg); free(it->blob); free(it);
        }
    }
}

// ---- receiver ----
static void samples_dir(wchar_t *out, int cap) {         // flim's own: aliases, samples without a song
    flimcfg_flim_dir(out, cap);
    wcscat_s(out, cap, L"\\samples");
}

// The song folder received samples go to ("" = none yet; the leader's is set on its first sample).
// Under g_sampleLock.
static wchar_t g_songDir[MAX_PATH];
static void song_dir_set(const wchar_t *dir) {
    EnterCriticalSection(&g_sampleLock);
    wcscpy_s(g_songDir, MAX_PATH, dir);
    LeaveCriticalSection(&g_sampleLock);
}

// A file at path (or a download on its way to it) that is other content than hex.
static int path_taken(const wchar_t *path, const char *hex) {
    char h[65];
    uint64_t s;
    XJob *j;
    int r = 0;
    EnterCriticalSection(&g_sampleLock);
    for (j = g_dnQ; j && !r; j = j->nx) r = _wcsicmp(j->path, path) == 0 && strcmp(j->hex, hex) != 0;
    LeaveCriticalSection(&g_sampleLock);
    if (r || !file_exists_w(path)) return r;
    if (placeholder_of(path, h, &s)) return strcmp(h, hex) != 0;
    return !sample_hash(path, h, &s) || strcmp(h, hex) != 0;
}

// Where a received sample is kept: in the song folder under its own file name (<stem>_<8 hex><ext>
// if that name holds other content), without one in samples\<first 16 hex>\<its file name>.
static void cache_path(const wchar_t *song, const char *hex, const wchar_t *p, wchar_t *out, int cap) {
    wchar_t dir[MAX_PATH], name[MAX_PATH], *ext;
    const wchar_t *b = p + wcslen(p);
    int i;
    while (b > p && b[-1] != L'\\' && b[-1] != L'/') b--;
    wcsncpy_s(name, MAX_PATH, *b ? b : L"sample.wav", _TRUNCATE);
    for (i = 0; name[i]; i++) if (wcschr(L"\\/:*?\"<>|", name[i]) || name[i] < 32) name[i] = L'_';
    if (song[0]) {
        swprintf_s(out, cap, L"%s\\%s", song, name);
        if (!path_taken(out, hex)) return;
        ext = wcsrchr(name, L'.');
        swprintf_s(out, cap, L"%s\\%.*s_%.8S%s", song, ext ? (int)(ext - name) : (int)wcslen(name), name, hex, ext ? ext : L"");
        return;
    }
    samples_dir(dir, MAX_PATH);
    swprintf_s(out, cap, L"%s\\%.16S\\%s", dir, hex, name);
}

typedef struct { wchar_t *p; char hex[65]; uint64_t size, ms; wchar_t cache[MAX_PATH * 2]; int need; } REnt;

// Queue a download (once per content). path: where to put it; if the same content is already
// on its way, path is changed to where that one goes.
static void download_queue(const char *hex, uint64_t size, wchar_t *path, int cap, int ph) {
    XJob *j, **t;
    EnterCriticalSection(&g_sampleLock);
    for (t = &g_dnQ; *t; t = &(*t)->nx)
        if (strcmp((*t)->hex, hex) == 0) {
            wcsncpy_s(path, cap, (*t)->path, _TRUNCATE);
            (*t)->ph |= ph;
            LeaveCriticalSection(&g_sampleLock);
            return;
        }
    j = (XJob *)calloc(1, sizeof(XJob));
    strcpy_s(j->hex, 65, hex);
    j->size = size;
    j->ph = ph;
    wcsncpy_s(j->path, MAX_PATH * 2, path, _TRUNCATE);
    j->first = j->next = GetTickCount();
    *t = j;
    g_dnTotal += size;
    LeaveCriticalSection(&g_sampleLock);
    SetEvent(g_dnEvt);
}
static int download_pending(const char *hex) {
    XJob *j;
    int r = 0;
    EnterCriticalSection(&g_sampleLock);
    for (j = g_dnQ; j && !r; j = j->nx) r = strcmp(j->hex, hex) == 0;
    LeaveCriticalSection(&g_sampleLock);
    return r;
}

// Copy src's content into dst, keeping dst the same file (hard links to it see the new data).
static int overwrite_in_place(const wchar_t *src, const wchar_t *dst) {
    HANDLE in = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL), out;
    unsigned char *buf;
    DWORD got, w;
    int ok = 1;
    if (in == INVALID_HANDLE_VALUE) return 0;
    out = CreateFileW(dst, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return 0; }        // e.g. FL streams it from disk right now
    buf = (unsigned char *)malloc(BLOB_CHUNK);
    for (;;) {
        if (!ReadFile(in, buf, BLOB_CHUNK, &got, NULL)) { ok = 0; break; }
        if (!got) break;
        if (!WriteFile(out, buf, got, &w, NULL) || w != got) { ok = 0; break; }
    }
    if (ok) ok = SetEndOfFile(out) != 0;
    free(buf);
    CloseHandle(out);
    CloseHandle(in);
    return ok;
}

// One try at a queued download: 1 = done, 0 = try again later, -1 = give up.
static int download_try(const XJob *j) {
    wchar_t part[MAX_PATH * 2 + 8], dir[MAX_PATH * 2], *slash;
    char what[80], got[65] = "";
    uint64_t sz;
    FILETIME mt;
    DWORD st;
    wcscpy_s(dir, MAX_PATH * 2, j->path);
    slash = wcsrchr(dir, L'\\');
    if (slash) { *slash = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
    swprintf_s(part, MAX_PATH * 2 + 8, L"%s.part", j->path);
    sprintf_s(what, sizeof(what), "/blob/%s", j->hex);
    st = blob_http(L"GET", what, NULL, 0, NULL, 0, part, got, NULL, 0);
    if (st == 200 && strcmp(got, j->hex) == 0) {
        int ok = overwrite_in_place(part, j->path);
        DeleteFileW(part);
        if (!ok) { logmsg("SAMPLES %S is in use: writing it again shortly", j->path); return 0; }
        if (file_info(j->path, &sz, &mt)) hash_put(j->path, sz, mt, j->hex);
        {   // aliases that ended up as copies instead of hard links (another drive) get the data too;
            // the units using an alias are reloaded like those using the file (full path: aliases
            // can have a short generic name)
            int i, k = 0;
            wchar_t *other[64];
            EnterCriticalSection(&g_sampleLock);
            for (i = 0; i < g_nph && k < 64; i++)
                if (strcmp(g_ph[i].hex, j->hex) == 0 && _wcsicmp(g_ph[i].path, j->path) != 0) {
                    other[k++] = _wcsdup(g_ph[i].path);
                    if (g_nreload == g_capreload) { g_capreload = g_capreload ? g_capreload * 2 : 16; g_reload = (wchar_t **)realloc(g_reload, sizeof(wchar_t *) * g_capreload); }
                    g_reload[g_nreload++] = _wcsdup(g_ph[i].path);
                }
            LeaveCriticalSection(&g_sampleLock);
            for (i = 0; i < k; i++) {
                uint64_t os;                         // a hard link already shows the new size
                if (!(file_info(other[i], &os, NULL) && os == j->size) && !overwrite_in_place(j->path, other[i]))
                    logmsg("SAMPLES could not fill in %S", other[i]);
                free(other[i]);
            }
        }
        placeholder_done(j->hex);
        logmsg("SAMPLES downloaded %S (%llu bytes)", j->path, (unsigned long long)j->size);
        return 1;
    }
    DeleteFileW(part);
    if (st == 200) logmsg("SAMPLES %S: content mismatch, trying again", j->path);
    if (GetTickCount() - j->first > DL_GIVEUP_MS) {
        logmsg("SAMPLES gave up on %S (HTTP %lu): the sender's upload never arrived", j->path, st);
        return -1;
    }
    return 0;                                    // 404: the sender is still uploading it
}

// Downloads in the background, retrying until the sender's upload is on the server. A finished
// placeholder is queued for reloading (poll thread -> reload_poll).
static DWORD WINAPI dnThread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_dnEvt, 500);
        for (;;) {
            XJob *j, c, **t;
            DWORD now = GetTickCount();
            int r, idle = 0;
            EnterCriticalSection(&g_sampleLock);
            for (j = g_dnQ; j && (LONG)(now - j->next) < 0; j = j->nx) {}
            if (j) c = *j;                           // work on a copy: a disconnect may free the queue
            if (!g_dnQ && g_dnTotal) { g_dnDone = g_dnTotal = 0; idle = 1; }
            LeaveCriticalSection(&g_sampleLock);
            if (idle) xfer_end();
            if (!j || !g_blobOn) break;
            xfer_show(0);
            r = download_try(&c);
            EnterCriticalSection(&g_sampleLock);
            for (t = &g_dnQ; *t && strcmp((*t)->hex, c.hex) != 0; t = &(*t)->nx) {}
            if (*t) {
                if (r) { XJob *d = *t; *t = d->nx; c.ph = d->ph; free(d); }
                else (*t)->next = GetTickCount() + DL_RETRY_MS;
            }
            if (r > 0 && c.ph) {
                if (g_nreload == g_capreload) { g_capreload = g_capreload ? g_capreload * 2 : 16; g_reload = (wchar_t **)realloc(g_reload, sizeof(wchar_t *) * g_capreload); }
                g_reload[g_nreload++] = _wcsdup(base_name(c.path));
            }
            LeaveCriticalSection(&g_sampleLock);
        }
    }
}

// Inbox thread: make the samples an inbound op lists available here (or a placeholder for them)
// and record the path rewrites for its data.
static void samples_resolve(const char *m) {
    const char *q = strstr(m, "\"samples\":[");
    REnt *R = NULL;
    int nr = 0, cap = 0, i, nneed = 0, nfail = 0, nph = 0;
    wchar_t song[MAX_PATH];
    if (!q) return;
    if (!g_isLeader && strstr(m, "\"kind\":\"project.load\"")) {     // joining: its song folder
        join_dir_of(m, g_joinDir, MAX_PATH, g_joinFlp, MAX_PATH * 2);
        g_joinMsg = m;
        wcscpy_s(song, MAX_PATH, g_joinDir);
    } else {
        EnterCriticalSection(&g_sampleLock);
        wcscpy_s(song, MAX_PATH, g_songDir);
        LeaveCriticalSection(&g_sampleLock);
        if (!song[0] && g_isLeader) {                                 // the leader's: named after its open project
            wchar_t cur[MAX_PATH];
            fl_ustr(PROJNAME_VA, cur, MAX_PATH);
            song_dir_of(cur, song, MAX_PATH);
            song_dir_set(song);
        }
    }
    if (song[0]) SHCreateDirectoryExW(NULL, song, NULL);
    q += 11;
    while (*q == '{') {
        const char *e = strchr(q, '}'), *pp = strstr(q, "\"p\":\""), *hh = strstr(q, "\"h\":\""), *ss = strstr(q, "\"s\":");
        if (!e) break;
        if (pp && pp < e && hh && hh < e && ss && ss < e) {
            const char *pe = strchr(pp + 5, '"');
            size_t bn;
            unsigned char *b;
            if (pe && pe < e && is_hex64(hh + 5)) {
                if (nr == cap) { cap = cap ? cap * 2 : 16; R = (REnt *)realloc(R, sizeof(REnt) * cap); }
                memset(&R[nr], 0, sizeof(REnt));
                b = b64dec(pp + 5, (size_t)(pe - pp - 5), &bn);
                R[nr].p = (wchar_t *)calloc(bn / 2 + 1, 2);
                memcpy(R[nr].p, b, bn / 2 * 2);
                free(b);
                memcpy(R[nr].hex, hh + 5, 64);
                R[nr].size = _strtoui64(ss + 4, NULL, 10);
                {
                    const char *mm = strstr(q, "\"ms\":");
                    if (mm && mm < e) R[nr].ms = _strtoui64(mm + 5, NULL, 10);
                }
                if (R[nr].p[0]) nr++; else free(R[nr].p);
            }
        }
        q = e + 1;
        if (*q == ',') q++;
    }
    for (i = 0; i < nr; i++) {
        wchar_t full[SAMPLE_PATH_MAX], found[SAMPLE_PATH_MAX];
        const wchar_t *have = NULL;
        char hx[65];
        uint64_t sz;
        // in our shared folder: left as written, the folder brings the file (if it isn't here yet, the
        // plugin gets it reloaded when it is: state_has_placeholder). NULL also drops an earlier rewrite.
        // (Listed anyway: someone whose Windows isn't on C: has another shared path, sent as a sample.)
        if (expand_path(R[i].p, full, SAMPLE_PATH_MAX) && in_shared(full)) { map_set(R[i].p, NULL); continue; }
        if (!g_noLocal && !is_net_path(R[i].p) && expand_path(R[i].p, full, SAMPLE_PATH_MAX) && sample_hash(full, hx, &sz) && strcmp(hx, R[i].hex) == 0)
            have = full;                                // the same file is right where the peer has it
        else if (!g_noLocal && find_by_hash(R[i].hex, found, SAMPLE_PATH_MAX)) have = found;
        if (have && !song[0]) { map_set(R[i].p, have == full ? NULL : have); continue; }
        cache_path(song, R[i].hex, R[i].p, R[i].cache, MAX_PATH * 2);
        if (have) {
            // the song folder gets a hard link (or copy) too, so it and its zip have every sample.
            // ponytail: a copy (sample on another drive) blocks the inbox while it runs
            if (!file_exists_w(R[i].cache) && !CreateHardLinkW(R[i].cache, have, NULL) && !CopyFileW(have, R[i].cache, TRUE))
                wcscpy_s(R[i].cache, MAX_PATH * 2, have);
            map_set(R[i].p, R[i].cache);
            continue;
        }
        if (sample_hash(R[i].cache, hx, &sz) && strcmp(hx, R[i].hex) == 0) { map_set(R[i].p, R[i].cache); continue; }
        if (placeholder_format(R[i].cache) && (placeholder_format(R[i].cache) != 3 || R[i].ms)) {   // (compressed: with its length only)
            // a silent placeholder now (unless this content is already on its way: same file),
            // the real data in the background
            int queued = download_pending(R[i].hex);
            download_queue(R[i].hex, R[i].size, R[i].cache, MAX_PATH * 2, 1);
            if (!queued) {
                wchar_t dir[MAX_PATH * 2], *slash;
                wcscpy_s(dir, MAX_PATH * 2, R[i].cache);
                slash = wcsrchr(dir, L'\\');
                if (slash) { *slash = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
                if (!placeholder_write(R[i].cache, R[i].ms ? R[i].ms : R[i].size / 176))
                    logmsg("SAMPLES could not write a placeholder at %S", R[i].cache);
            }
            placeholder_add(R[i].cache, R[i].hex, R[i].size);
            map_set(R[i].p, R[i].cache);
            nph++;
            continue;
        }
        download_queue(R[i].hex, R[i].size, R[i].cache, MAX_PATH * 2, 0);
        R[i].need = 1;
        nneed++;
    }
    // formats without a placeholder (sf2..., or a compressed one of unknown length): this op waits for them
    for (i = 0; i < nr; i++) {
        char hx[65];
        uint64_t sz;
        if (!R[i].need) continue;
        while (g_blobOn && download_pending(R[i].hex)) Sleep(200);
        if (sample_hash(R[i].cache, hx, &sz) && strcmp(hx, R[i].hex) == 0) map_set(R[i].p, R[i].cache);
        else { nfail++; logmsg("SAMPLES could not get %S: it will be missing", R[i].p); }
    }
    logmsg("SAMPLES op lists %d samples: %d placeholders (downloading), %d waited for, %d missing", nr, nph, nneed - nfail, nfail);
    for (i = 0; i < nr; i++) free(R[i].p);
    free(R);
}

// ---- project zips: song zips (member) and backups ----
// A portable copy of a project: the .flp (named flpName in the zip) and every sample it uses,
// flat, like FL's zipped loop packages; samples that are still placeholders are left out (FL then
// reports them missing). Writer: common/flim_zip.h. Returns flim_zip's count (or -1).
static int zip_project(const wchar_t *flp, const unsigned char *d, size_t dn, const wchar_t *flpName, const wchar_t *zip, int *left) {
    PathList L;
    wchar_t **src, **nm, full[SAMPLE_PATH_MAX];
    char hx[65];
    uint64_t sz;
    int i, j, n = 1, r;
    memset(&L, 0, sizeof(L));
    flp_walk(d, dn, collect_cb, &L);
    src = (wchar_t **)calloc((size_t)L.n + 1, sizeof(wchar_t *));
    nm = (wchar_t **)calloc((size_t)L.n + 1, sizeof(wchar_t *));
    src[0] = _wcsdup(flp);
    nm[0] = _wcsdup(flpName);
    *left = 0;
    for (i = 0; i < L.n; i++) {
        if (!expand_path(L.paths[i], full, SAMPLE_PATH_MAX) || !file_exists_w(full)) continue;
        if (placeholder_of(full, hx, &sz)) { (*left)++; continue; }
        for (j = 0; j < n && _wcsicmp(src[j], full) != 0; j++) {}
        if (j < n) continue;
        src[n] = _wcsdup(full);
        nm[n] = _wcsdup(base_name(full));
        for (j = 0; j < n && _wcsicmp(nm[j], nm[n]) != 0; j++) {}
        if (j < n) {                                      // two samples with one name: the second gets a number
            size_t cap = wcslen(nm[n]) + 16;
            wchar_t *u = (wchar_t *)malloc(cap * sizeof(wchar_t));
            swprintf_s(u, cap, L"%d %s", n, nm[n]);
            free(nm[n]);
            nm[n] = u;
        }
        n++;
    }
    pathlist_free(&L);
    r = flim_zip(zip, n, (const wchar_t *const *)src, (const wchar_t *const *)nm);
    for (i = 0; i < n; i++) { free(src[i]); free(nm[i]); }
    free(src); free(nm);
    return r;
}

// Member: <session copy>.zip next to it, once the joined project's downloads are in.
static SRWLOCK g_zipLock = SRWLOCK_INIT;                         // one zip written at a time
static DWORD WINAPI zipThread(LPVOID p) {
    wchar_t *flp = (wchar_t *)p, zip[MAX_PATH * 2];
    unsigned char *d;
    size_t n = 0;
    int left = 0, r;
    for (;;) {                                                   // the joined project's samples are queued by now
        int busy;
        EnterCriticalSection(&g_sampleLock);
        busy = g_dnQ != NULL;
        LeaveCriticalSection(&g_sampleLock);
        if (!busy || !g_blobOn) break;
        Sleep(500);
    }
    wcscpy_s(zip, MAX_PATH * 2, flp);
    wcscpy_s(zip + wcslen(zip) - 4, 5, L".zip");                 // <name>.flp -> <name>.zip
    AcquireSRWLockExclusive(&g_zipLock);
    if ((d = read_file_w(flp, &n)) != NULL) {
        r = zip_project(flp, d, n, base_name(flp), zip, &left);
        if (r >= 0) logmsg("PROJECT zipped the project and %d samples to %S%s", r - 1, zip, left ? " (samples that never arrived are left out)" : "");
        else logmsg("PROJECT no zip of %S: over 4 GB, or a file could not be read", flp);
        free(d);
    }
    ReleaseSRWLockExclusive(&g_zipLock);
    free(flp);
    return 0;
}
static void zip_song_async(const wchar_t *flp) {
    HANDLE h = CreateThread(NULL, 0, zipThread, _wcsdup(flp), 0, NULL);
    if (h) CloseHandle(h);
}

// ---------------- project backups ----------------
// Every backup_min minutes (flim.ini, set in flim's window; 0 = off), if the project changed since
// the last one: <song folder>\Backups\<song> <yyyy-mm-dd hhmmss>.zip, a project zip (above) of a
// snapshot (FL's own project writer, as for joins: the open project and its name are untouched).
// The newest BACKUP_KEEP per song stay.
#define BACKUP_KEEP   20
static volatile LONG g_backupMin = 10;          // (declared before gui_command too)
static uint64_t g_backupHash;
static DWORD    g_backupAt;
typedef struct { wchar_t flp[MAX_PATH], zip[MAX_PATH * 2], dir[MAX_PATH * 2], song[MAX_PATH]; unsigned char *d; size_t n; } Backup;
static int wcs_cmp(const void *a, const void *b) { return wcscmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b); }
static DWORD WINAPI backupThread(LPVOID p) {
    Backup *bk = (Backup *)p;
    wchar_t pat[MAX_PATH * 2 + 8], name[MAX_PATH + 8];
    WIN32_FIND_DATAW fd;
    HANDLE fh;
    int i, left = 0, r;
    SHCreateDirectoryExW(NULL, bk->dir, NULL);
    swprintf_s(name, MAX_PATH + 8, L"%s.flp", bk->song);
    r = zip_project(bk->flp, bk->d, bk->n, name, bk->zip, &left);
    if (r > 0) logmsg("PROJECT backup: %S (%d samples)", bk->zip, r - 1);
    else logmsg("PROJECT backup failed: %S (over 4 GB, or a sample could not be read)", bk->zip);
    {   // only the newest BACKUP_KEEP (names sort by time)
        wchar_t *z[512];
        int k = 0;
        swprintf_s(pat, MAX_PATH * 2 + 8, L"%s\\*.zip", bk->dir);
        if ((fh = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
            do if (k < 512) z[k++] = _wcsdup(fd.cFileName); while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
        qsort(z, (size_t)k, sizeof(wchar_t *), wcs_cmp);
        for (i = 0; i < k; i++) {
            if (i < k - BACKUP_KEEP) { swprintf_s(pat, MAX_PATH * 2 + 8, L"%s\\%s", bk->dir, z[i]); DeleteFileW(pat); }
            free(z[i]);
        }
    }
    DeleteFileW(bk->flp);
    free(bk->d);
    free(bk);
    return 0;
}
// Main thread (task window timer): a backup if one is due and the project changed.
static void backup_tick(void) {
    DWORD now = GetTickCount();
    wchar_t cur[MAX_PATH];
    SYSTEMTIME t;
    Backup *bk;
    uint64_t h;
    char why[200];
    if (!g_backupAt) g_backupAt = now;                            // the first one a full interval after FL started
    if (!g_backupMin || now - g_backupAt < (DWORD)g_backupMin * 60000) return;
    if (g_projBusy || fl_loading() || fl_presets_loading() || main_busy(why, sizeof(why))) return;   // next minute
    g_backupAt = now;
    bk = (Backup *)calloc(1, sizeof(Backup));
    if (!(bk->d = project_snapshot(&bk->n))) { free(bk); return; }
    h = fnv64(bk->d, bk->n);
    if (h == g_backupHash) { free(bk->d); free(bk); return; }
    g_backupHash = h;
    fl_ustr(PROJNAME_VA, cur, MAX_PATH);
    song_of(cur, bk->song, MAX_PATH);
    song_dir_of(cur, bk->dir, MAX_PATH * 2);
    wcscat_s(bk->dir, MAX_PATH * 2, L"\\Backups");
    GetLocalTime(&t);
    swprintf_s(bk->zip, MAX_PATH * 2, L"%s\\%s %04d-%02d-%02d %02d%02d%02d.zip", bk->dir, bk->song, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    swprintf_s(bk->flp, MAX_PATH, L"%S\\backup_%ld.flp", g_tmpDir, InterlockedIncrement(&g_tmpSeq));
    if (!write_file_w(bk->flp, bk->d, bk->n)) { free(bk->d); free(bk); return; }
    {
        HANDLE th = CreateThread(NULL, 0, backupThread, bk, 0, NULL);
        if (th) CloseHandle(th); else { DeleteFileW(bk->flp); free(bk->d); free(bk); }
    }
}
static void backup_setting(void) {
    int on, m = flimcfg_backup_min(&on);
    g_backupMin = on ? m : 0;
}

// Every inbound message comes through here (receive thread).
static void leader_signal(const char *m);
static void inbox_msg(const char *m) {
    int queue;
    if (strstr(m, "\"t\":\"op\"") && (strstr(m, "\"kind\":\"alive\"") || strstr(m, "\"kind\":\"saving\""))) { leader_signal(m); return; }   // never waits behind downloads
    EnterCriticalSection(&g_inLock);
    queue = g_inBusy || g_inHead || strstr(m, "\"samples\":[") != NULL || is_big_ref(m);
    if (queue) {
        InItem *it = (InItem *)calloc(1, sizeof(InItem));
        it->msg = _strdup(m);
        if (g_inTail) g_inTail->next = it; else g_inHead = it;
        g_inTail = it;
    }
    LeaveCriticalSection(&g_inLock);
    if (queue) SetEvent(g_inEvt);
    else handle_msg(m);
}

static DWORD WINAPI inThread(LPVOID p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_inEvt, INFINITE);
        for (;;) {
            InItem *it;
            EnterCriticalSection(&g_inLock);
            it = g_inHead;
            if (!it) { g_inBusy = 0; LeaveCriticalSection(&g_inLock); break; }
            g_inHead = it->next;
            if (!g_inHead) g_inTail = NULL;
            g_inBusy = 1;
            LeaveCriticalSection(&g_inLock);
            if (is_big_ref(it->msg)) {
                char *real = big_get(it->msg);
                free(it->msg);
                it->msg = real ? real : _strdup("{}");
            }
            if (strstr(it->msg, "\"samples\":[")) samples_resolve(it->msg);
            handle_msg(it->msg);
            g_joinMsg = NULL;
            free(it->msg); free(it);
        }
    }
}

// Connected: where the blob store is (same server, same password).
static void samples_connected(const NetUrl *u, const char *pw) {
    EnterCriticalSection(&g_sampleLock);
    g_blobUrl = *u;
    sha_hex(pw, strlen(pw), g_blobKey);
    InterlockedExchange(&g_blobOn, 1);
    LeaveCriticalSection(&g_sampleLock);
}
// Disconnected: drop what was waiting to go out or to be applied.
static void sh_disconnected(void);
static void samples_disconnected(void) {
    EnterCriticalSection(&g_sampleLock);
    InterlockedExchange(&g_blobOn, 0);
    while (g_upQ) { XJob *n = g_upQ->nx; free(g_upQ); g_upQ = n; }
    while (g_dnQ) { XJob *n = g_dnQ->nx; free(g_dnQ); g_dnQ = n; }    // placeholders stay until a later session fetches them
    g_upDone = g_upTotal = g_dnDone = g_dnTotal = 0;
    g_songDir[0] = 0;
    LeaveCriticalSection(&g_sampleLock);
    EnterCriticalSection(&g_outLock);
    while (g_outHead) { OutItem *n = g_outHead->next; free(g_outHead->msg); free(g_outHead->blob); free(g_outHead); g_outHead = n; }
    g_outTail = NULL;
    LeaveCriticalSection(&g_outLock);
    EnterCriticalSection(&g_inLock);
    while (g_inHead) { InItem *n = g_inHead->next; free(g_inHead->msg); free(g_inHead); g_inHead = n; }
    g_inTail = NULL;
    LeaveCriticalSection(&g_inLock);
    sh_disconnected();
}

// ---------------- shared folder ----------------
// One folder with the same path on every PC (flimcfg_shared_dir: normally C:\Users\Public\Documents\flim\
// Shared) whose files are the same for everyone in the session. A plugin that loads a sample from there
// finds it on every PC: the path in its state is right everywhere, so flim never rewrites it (in_shared:
// samples_resolve leaves those paths alone, the file comes through here). That works for any plugin,
// whatever it does with its state (Serum 2 and Kontakt compress theirs, so flim can't see their samples).
// Versions decide which content a path has, not file times (a copied file keeps its old time, and clocks
// differ): a change made here gets max(its version, the highest anyone sent) + 1 and goes out; a higher
// version from someone replaces ours; the same version with other content (two changes at once) goes to
// the higher hash. A version only this PC had is kept in <root>\..\Shared backups\<time>\ before it is
// replaced. A file deleted here stays deleted: only a newer version brings it back (deletes don't sync).
// The table survives restarts: %APPDATA%\flim\shared-<root hash>.tsv.
//   {"t":"op","op":{"kind":"shared.files","full":0|1|2[,"root":"<b64 UTF-16>"],"f":[{"r":"<b64 UTF-16 path
//    in the folder>","h":"<sha256>","s":<bytes>,"v":<version>},...]}}
// full 1: everything we have, when we join; everyone answers with full 2 (not answered). Changes go out
// as full 0 after a scan (every SH_SCAN_MS), their files uploaded first. A download that finds no blob
// (evicted, or a full list's file never uploaded) asks its sender at most every SH_NEED_MS:
//   {"t":"op","op":{"kind":"shared.need","h":"<sha256>","to":"<sender id>"}}
// shThread owns the table: it scans, decides and puts files in place. shDlThread only downloads verified
// .part files (one at a time), so a big download doesn't hold up the scans.
#define SH_SCAN_MS     2000
#define SH_MAX_FILES   10000
#define SH_REL_MAX     200
#define SH_NEED_MS     30000
#define SH_FREE_MIN    (2ull << 30)        // a download leaves this much free on the folder's drive
#define SH_BACKUP_DAYS 30                  // older backups go to the Recycle Bin
#define SH_RELOAD_MS   10000               // finished files reload the plugins that use them in batches
typedef struct {
    wchar_t *rel;
    uint64_t size, t, v, seen;             // the file when hashed (size, time); its version; the highest version anyone sent
    uint64_t ps, pt;                       // what the last scan saw: a change counts once two scans agree
    char hex[65];                          // its content ("": none here)
    unsigned char del, local, dirty, found; // deleted here; content made here (no one else has it); to send; seen this scan
} ShFile;
typedef struct ShJob {
    wchar_t rel[SH_REL_MAX + 1];
    char hex[65], from[64], who[64];
    uint64_t size, v, tsize, tt;           // tsize, tt: the file here when queued (tsize ~0: none), to spot a change since
    int got, warned;                       // got: its .part is complete and verified
    DWORD first, next, asked;
    struct ShJob *nx;
} ShJob;
typedef struct ShMsg { char *m; struct ShMsg *nx; } ShMsg;
static ShFile *g_shf; static int g_nshf, g_capshf, g_shHint;     // shThread
static ShJob  *g_shJobs;                                         // shThread
static ShMsg  *g_shIn, *g_shInTail;                              // g_shLock
static CRITICAL_SECTION g_shLock;
static HANDLE g_shEvt, g_shDlEvt;
static volatile LONG g_shFull, g_shGen, g_shOn;   // what to send everything as next (1 join, 2 answer); bumped on disconnect; in a room
static wchar_t g_shTable[MAX_PATH], g_shBackups[MAX_PATH];
static int g_shSave;                                             // the table changed: write it (shThread)
static struct { int busy, done; LONG gen; ShJob j; DWORD status; } g_shDl;   // the download worker's job (g_shLock)

static uint64_t ft_ms(FILETIME f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10000; }

// Names flim's own and other programs' unfinished files have, and Windows' folder files: never shared.
static int sh_skip_name(const wchar_t *nm) {
    static const wchar_t *const END[] = { L".part", L".crdownload", L".tmp", L".partial", L".download" };
    size_t n = wcslen(nm), i, k;
    if (!_wcsnicmp(nm, L"~flim-", 6) || !wcsncmp(nm, L"~$", 2) || !_wcsicmp(nm, L"desktop.ini") || !_wcsicmp(nm, L"thumbs.db")) return 1;
    for (i = 0; i < sizeof(END) / sizeof(END[0]); i++) { k = wcslen(END[i]); if (n > k && !_wcsicmp(nm + n - k, END[i])) return 1; }
    return 0;
}
// A path in the folder as someone names it: names below the folder only (no drive, no "..", no stream,
// no device name), of a file a sample may be (content_file: no programs or plugin binaries; and no
// scripts or shell files either).
static int sh_rel_ok(const wchar_t *r) {
    static const wchar_t *const DEV[] = { L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$" };
    static const wchar_t *const NO[] = { L".hta", L".reg", L".inf", L".js", L".jse", L".vbe", L".wsf", L".wsh", L".psm1",
        L".msp", L".msix", L".appx", L".jar", L".chm", L".pif", L".application", L".gadget", L".library-ms",
        L".search-ms", L".searchconnector-ms", L".settingcontent-ms", L".url", L".website", L".scf" };
    size_t n = wcslen(r), i, s = 0, j, k, x;
    wchar_t w[SH_REL_MAX + 2] = L"\\", up[8];
    if (!n || n > SH_REL_MAX) return 0;
    wcscat_s(w, SH_REL_MAX + 2, r);
    if (!content_file(w, n + 1)) return 0;               // (it takes a path: "\x.wav")
    for (i = 0; i < sizeof(NO) / sizeof(NO[0]); i++) { k = wcslen(NO[i]); if (n > k && !_wcsicmp(r + n - k, NO[i])) return 0; }
    for (i = 0; i <= n; i++) {
        if (r[i] && r[i] != L'\\') {
            if (r[i] < 32 || wcschr(L"/:*?\"<>|", r[i])) return 0;
            continue;
        }
        k = i - s;
        if (!k || r[i - 1] == L'.' || r[i - 1] == L' ') return 0;          // empty, ".", "..", "name." / "name "
        if (i == n && sh_skip_name(r + s)) return 0;
        for (j = 0; j < k && r[s + j] != L'.'; j++) {}
        while (j && r[s + j - 1] == L' ') j--;
        if (j <= 7) {                                                       // a device name, with any extension
            for (x = 0; x < j; x++) up[x] = towupper(r[s + x]);
            up[j] = 0;
            for (x = 0; x < sizeof(DEV) / sizeof(DEV[0]); x++) if (!wcscmp(up, DEV[x])) return 0;
            if (j == 4 && (!wcsncmp(up, L"COM", 3) || !wcsncmp(up, L"LPT", 3)) && up[3] >= L'0' && up[3] <= L'9') return 0;
        }
        s = i + 1;
    }
    return 1;
}
// root\rel (after a check that it is still inside the root and short enough for Windows); 0 = no.
static int sh_target(const wchar_t *rel, wchar_t *out, int cap) {
    wchar_t full[MAX_PATH * 2];
    size_t n = wcslen(g_shRoot);
    if (!n || _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", g_shRoot, rel) < 0) return 0;
    if (!GetFullPathNameW(out, MAX_PATH * 2, full, NULL) || wcslen(full) >= MAX_PATH - 24) return 0;
    return _wcsnicmp(full, g_shRoot, n) == 0 && full[n] == L'\\' && _wcsicmp(full, out) == 0;
}
// No folder between the root and p is a link (a junction there would lead outside the root).
static int sh_no_links(const wchar_t *p) {
    wchar_t d[MAX_PATH];
    size_t i;
    wcsncpy_s(d, MAX_PATH, p, _TRUNCATE);
    for (i = wcslen(g_shRoot) + 1; d[i]; i++) {
        DWORD a;
        if (d[i] != L'\\') continue;
        d[i] = 0;
        a = GetFileAttributesW(d);
        d[i] = L'\\';
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
    }
    return 1;
}
static void sh_part(const wchar_t *full, const char *hex, wchar_t *out, int cap) {   // its download, next to it
    const wchar_t *b = base_name(full);
    _snwprintf_s(out, cap, _TRUNCATE, L"%.*s~flim-%.16S.part", (int)(b - full), full, hex);
}
static int sh_find(const wchar_t *rel) {      // (scans and lists go in the same order: try after the last hit first)
    int i, k;
    for (k = 0; k < g_nshf; k++) {
        i = (g_shHint + k) % g_nshf;
        if (_wcsicmp(g_shf[i].rel, rel) == 0) { g_shHint = i + 1; return i; }
    }
    return -1;
}
static ShFile *sh_entry(const wchar_t *rel) {
    int i = sh_find(rel);
    if (i >= 0) return &g_shf[i];
    if (g_nshf == g_capshf) { g_capshf = g_capshf ? g_capshf * 2 : 64; g_shf = (ShFile *)realloc(g_shf, sizeof(ShFile) * g_capshf); }
    memset(&g_shf[g_nshf], 0, sizeof(ShFile));
    g_shf[g_nshf].rel = _wcsdup(rel);
    return &g_shf[g_nshf++];
}
// Once per file and reason: why something in the folder doesn't reach the others.
static void sh_warn(const wchar_t *rel, const char *why) {
    static wchar_t *said[64];
    static int n;
    int i;
    for (i = 0; i < n; i++) if (!_wcsicmp(said[i], rel)) return;
    if (n < 64) said[n++] = _wcsdup(rel);
    logmsg("SHARED %S isn't shared: %s", rel, why);
}

// ---- the table on disk: v, seen, del, local, size, time, hash, path (UTF-8), one file per line ----
static void sh_load(void) {
    FILE *f = NULL;
    char line[1200];
    if (_wfopen_s(&f, g_shTable, L"rb") != 0 || !f) return;
    while (fgets(line, sizeof(line), f)) {
        char *p = line, *e;
        uint64_t x[6];
        int k;
        wchar_t rel[SH_REL_MAX + 1];
        ShFile *s;
        for (k = 0; k < 6; k++) { x[k] = _strtoui64(p, &e, 10); if (*e != '\t') break; p = e + 1; }
        if (k < 6 || strlen(p) < 66 || p[64] != '\t' || (p[0] != '-' && !is_hex64(p))) continue;
        e = p + 65;
        e[strcspn(e, "\r\n")] = 0;
        if (!MultiByteToWideChar(CP_UTF8, 0, e, -1, rel, SH_REL_MAX + 1) || !sh_rel_ok(rel)) continue;
        s = sh_entry(rel);
        s->v = x[0]; s->seen = x[1]; s->del = (unsigned char)x[2]; s->local = (unsigned char)x[3];
        s->size = s->ps = x[4]; s->t = s->pt = x[5];
        if (p[0] != '-') { memcpy(s->hex, p, 64); s->hex[64] = 0; }
    }
    fclose(f);
}
static void sh_store(void) {
    static char none[65];
    wchar_t tmp[MAX_PATH + 8];
    FILE *f = NULL;
    int i;
    if (!none[0]) memset(none, '-', 64);                    // (no content here: a version known, nothing yet)
    swprintf_s(tmp, MAX_PATH + 8, L"%s.new", g_shTable);
    if (_wfopen_s(&f, tmp, L"wb") != 0 || !f) return;
    for (i = 0; i < g_nshf; i++) {
        ShFile *s = &g_shf[i];
        char rel[SH_REL_MAX * 3 + 1];
        if (!s->hex[0] && !s->seen) continue;
        WideCharToMultiByte(CP_UTF8, 0, s->rel, -1, rel, sizeof(rel), NULL, NULL);
        fprintf(f, "%llu\t%llu\t%d\t%d\t%llu\t%llu\t%s\t%s\n", (unsigned long long)s->v, (unsigned long long)s->seen, s->del, s->local,
                (unsigned long long)s->size, (unsigned long long)s->t, s->hex[0] ? s->hex : none, rel);
    }
    fclose(f);
    MoveFileExW(tmp, g_shTable, MOVEFILE_REPLACE_EXISTING);
    g_shSave = 0;
}

// ---- scanning ----
static int sh_quiet(const wchar_t *full) {       // nobody is writing it (a writer's handle refuses read-only sharing)
    HANDLE h = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}
static void sh_scan_dir(const wchar_t *dir, const wchar_t *rel0, int *count, int first) {
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH], rel[SH_REL_MAX + 2], full[MAX_PATH];
    HANDLE h;
    if (_snwprintf_s(pat, MAX_PATH, _TRUNCATE, L"%s\\*", dir) < 0) return;
    h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *nm = fd.cFileName;
        uint64_t size, t;
        char hx[65];
        ShFile *f;
        if (!wcscmp(nm, L".") || !wcscmp(nm, L"..") || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (_snwprintf_s(rel, SH_REL_MAX + 2, _TRUNCATE, rel0[0] ? L"%s\\%s" : L"%s%s", rel0, nm) < 0 ||
            _snwprintf_s(full, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, nm) < 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { sh_warn(rel, "it is a link (put the files themselves in the folder)"); continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcslen(rel) < SH_REL_MAX) sh_scan_dir(full, rel, count, first);
            continue;
        }
        if (sh_skip_name(nm)) {
            if (first && !_wcsnicmp(nm, L"~flim-", 6)) DeleteFileW(full);   // a download a crash left behind
            continue;
        }
        if (wcslen(rel) > SH_REL_MAX || !sh_target(rel, pat, MAX_PATH)) { sh_warn(rel, "its path is too long"); continue; }
        if (!sh_rel_ok(rel)) continue;                      // programs and the like: the rule the others' files follow too
        size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (size > SAMPLE_MAX_BYTES) { sh_warn(rel, "it is over 2000 MB"); continue; }
        if (++*count > SH_MAX_FILES) break;
        t = ft_ms(fd.ftLastWriteTime);
        f = sh_entry(rel);
        f->found = 1;
        if (!f->del && f->hex[0] && f->size == size && f->t == t) continue;
        if (f->ps != size || f->pt != t) { f->ps = size; f->pt = t; continue; }   // changing (or just appeared): next scan
        if (!sh_quiet(full) || !hash_file(full, hx)) continue;                  // still being written
        if (f->del || strcmp(hx, f->hex) != 0) {            // new content here: the next version
            strcpy_s(f->hex, sizeof(f->hex), hx);
            f->v = (f->v > f->seen ? f->v : f->seen) + 1;
            f->local = 1; f->dirty = 1; f->del = 0;
        }
        f->size = size; f->t = t;
        g_shSave = 1;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void sh_scan(int first) {
    int i, n = 0;
    SHCreateDirectoryExW(NULL, g_shRoot, NULL);
    for (i = 0; i < g_nshf; i++) g_shf[i].found = 0;
    sh_scan_dir(g_shRoot, L"", &n, first);
    if (n > SH_MAX_FILES) {
        static int said;
        if (!said) { said = 1; logmsg("SHARED the shared folder has more than %d files: only %d of them are shared", SH_MAX_FILES, SH_MAX_FILES); }
    }
    for (i = 0; i < g_nshf; i++)                            // deleted here: it stays deleted (a newer version brings it back)
        if (!g_shf[i].found && g_shf[i].hex[0] && !g_shf[i].del) { g_shf[i].del = 1; g_shf[i].dirty = 0; g_shSave = 1; }
}

// ---- sending ----
// full 0: what changed here (after the server got those files); 1, 2: every file we have, announced only
// (a peer whose download finds no blob asks for it: shared.need).
static void sh_send(int full) {
    int i, n = 0, o = 0;
    size_t cap;
    char *m;
    wchar_t path[MAX_PATH];
    for (i = 0; i < g_nshf; i++) if (g_shf[i].hex[0] && !g_shf[i].del && (full || g_shf[i].dirty)) n++;
    if (!n && !full) return;
    if (!full) {
        char *body = (char *)malloc((size_t)n * 65 + 1), *resp = (char *)malloc((size_t)n * 65 + 16), *line, *ctx = NULL;
        DWORD st;
        for (i = 0; i < g_nshf; i++)
            if (g_shf[i].hex[0] && !g_shf[i].del && g_shf[i].dirty) o += sprintf_s(body + o, (size_t)n * 65 + 1 - o, "%s\n", g_shf[i].hex);
        resp[0] = 0;
        st = blob_http(L"POST", "/blobs/missing", NULL, 0, body, (DWORD)o, NULL, NULL, resp, n * 65 + 16);
        if (st == 200)
            for (line = strtok_s(resp, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx))
                for (i = 0; i < g_nshf; i++)
                    if (strcmp(g_shf[i].hex, line) == 0 && sh_target(g_shf[i].rel, path, MAX_PATH)) { upload_queue(path, g_shf[i].hex, g_shf[i].size, 0); break; }
        free(body); free(resp);
        if (st != 200) return;                              // the server can't be asked: still to send, at the next scan
    }
    cap = 300 + (wcslen(g_shRoot) * 2 + 3) / 3 * 4;
    for (i = 0; i < g_nshf; i++) cap += 160 + (wcslen(g_shf[i].rel) * 2 + 3) / 3 * 4;
    m = (char *)malloc(cap);
    o = sprintf_s(m, cap, "{\"t\":\"op\",\"op\":{\"kind\":\"shared.files\",\"full\":%d", full);
    if (full) {
        o += sprintf_s(m + o, cap - o, ",\"root\":\"");
        b64enc((const unsigned char *)g_shRoot, (int)wcslen(g_shRoot) * 2, m + o);
        o += (int)strlen(m + o);
        m[o++] = '"';
    }
    o += sprintf_s(m + o, cap - o, ",\"f\":[");
    for (i = 0, n = 0; i < g_nshf; i++) {
        ShFile *f = &g_shf[i];
        if (!f->hex[0] || f->del || (!full && !f->dirty)) continue;
        o += sprintf_s(m + o, cap - o, "%s{\"r\":\"", n++ ? "," : "");
        b64enc((const unsigned char *)f->rel, (int)wcslen(f->rel) * 2, m + o);
        o += (int)strlen(m + o);
        o += sprintf_s(m + o, cap - o, "\",\"h\":\"%s\",\"s\":%llu,\"v\":%llu}", f->hex, (unsigned long long)f->size, (unsigned long long)f->v);
        f->dirty = 0;
    }
    sprintf_s(m + o, cap - o, "]}}");
    ws_send(m);
    free(m);
    if (full) logmsg("SHFILE listed all %d files for the others", n);
    else logmsg("SHARED sent %d file%s to the others", n, n == 1 ? "" : "s");
}

// ---- receiving ----
static void sh_job_add(const wchar_t *rel, const wchar_t *full, const char *hex, uint64_t size, uint64_t v, const char *id, const char *who) {
    ShJob *j, **pp;
    uint64_t sz;
    FILETIME ft;
    for (pp = &g_shJobs; *pp && _wcsicmp((*pp)->rel, rel) != 0; pp = &(*pp)->nx) {}
    if (*pp) {                                              // one for this file already: the newer one stays
        wchar_t part[MAX_PATH];
        j = *pp;
        if (j->v > v || (j->v == v && strcmp(j->hex, hex) >= 0)) return;
        sh_part(full, j->hex, part, MAX_PATH);
        DeleteFileW(part);
        *pp = j->nx;
    } else {
        j = (ShJob *)calloc(1, sizeof(ShJob));
        j->first = GetTickCount();
        if (file_info(full, &sz, &ft)) { j->tsize = sz; j->tt = ft_ms(ft); } else j->tsize = ~0ull;
    }
    wcscpy_s(j->rel, SH_REL_MAX + 1, rel);
    strcpy_s(j->hex, 65, hex);
    strncpy_s(j->from, sizeof(j->from), id, _TRUNCATE);
    strncpy_s(j->who, sizeof(j->who), who, _TRUNCATE);
    j->size = size; j->v = v; j->got = 0; j->next = 0;
    for (pp = &g_shJobs; *pp && (*pp)->size <= size; pp = &(*pp)->nx) {}   // small files first
    j->nx = *pp;
    *pp = j;
}
static void sh_manifest(const char *m) {
    const char *q = strstr(m, "\"f\":["), *fr = strstr(m, "\"from\":\""), *ro = strstr(m, "\"root\":\"");
    char id[64] = "", who[64] = "someone";
    int i, full = 0, cnt = 0;
    int_after(m, NULL, "\"full\":", &full);
    if (fr) {
        for (i = 0; fr[8 + i] && fr[8 + i] != '"' && i < 63; i++) id[i] = fr[8 + i];
        id[i] = 0;
        peer_name(id, who, sizeof(who));
    }
    if (ro) {
        static int said;
        const char *e = strchr(ro + 8, '"');
        size_t bn;
        unsigned char *b = e ? b64dec(ro + 8, (size_t)(e - ro - 8), &bn) : NULL;
        wchar_t *w = b ? (wchar_t *)calloc(bn / 2 + 1, 2) : NULL;
        if (w) {
            memcpy(w, b, bn / 2 * 2);
            if (_wcsicmp(w, g_shRoot) != 0 && !said) {
                said = 1;
                logmsg("SHARED %s's shared folder is %S, yours is %S: files sync, but plugins won't find each other's", who, w, g_shRoot);
            }
        }
        free(w); free(b);
    }
    if (full == 1) InterlockedCompareExchange(&g_shFull, 2, 0);   // a newcomer: they get our list (unless ours goes out anyway)
    if (!q) return;
    for (q += 5; *q == '{' && cnt < SH_MAX_FILES; cnt++) {
        const char *e = strchr(q, '}'), *rr = strstr(q, "\"r\":\""), *hh = strstr(q, "\"h\":\""), *ss = strstr(q, "\"s\":"), *vv = strstr(q, "\"v\":");
        if (!e) break;
        if (rr && rr < e && hh && hh < e && ss && ss < e && vv && vv < e && is_hex64(hh + 5)) {
            const char *re = strchr(rr + 5, '"');
            size_t bn = 0;
            unsigned char *b = re && re < e ? b64dec(rr + 5, (size_t)(re - rr - 5), &bn) : NULL;
            wchar_t rel[SH_REL_MAX + 1], full_[MAX_PATH];
            char hex[65];
            uint64_t size = _strtoui64(ss + 4, NULL, 10), v = _strtoui64(vv + 4, NULL, 10);
            ShFile *f;
            memcpy(hex, hh + 5, 64); hex[64] = 0;
            rel[0] = 0;
            if (b && bn / 2 <= SH_REL_MAX) { memcpy(rel, b, bn / 2 * 2); rel[bn / 2] = 0; }
            free(b);
            if (!rel[0] || wcslen(rel) != bn / 2 || !sh_rel_ok(rel) || size > SAMPLE_MAX_BYTES || !v || !sh_target(rel, full_, MAX_PATH)) {
                logmsg("SHFILE %s's file \"%S\" isn't a name flim takes into the shared folder: skipped", who, rel);
            } else {
                f = sh_entry(rel);
                if (v > f->seen) { f->seen = v; g_shSave = 1; }
                if (f->hex[0] && !f->del && !strcmp(f->hex, hex)) {             // the same here
                    if (v > f->v) f->v = v;
                    f->local = 0;
                    g_shSave = 1;
                } else if (f->del) {
                    if (v > f->v) sh_job_add(rel, full_, hex, size, v, id, who);  // changed after we deleted ours
                } else if (!f->hex[0]) {
                    if (!file_exists_w(full_)) sh_job_add(rel, full_, hex, size, v, id, who);
                    // (a file here the scan hasn't taken yet: it becomes a newer version and goes out)
                } else if (v > f->v || (v == f->v && strcmp(hex, f->hex) > 0)) sh_job_add(rel, full_, hex, size, v, id, who);
                else f->dirty = 1;                                                // ours is newer: they get it next
            }
        }
        q = e + 1;
        if (*q == ',') q++;
    }
}
static void sh_need(const char *m) {             // someone's download of our file found no blob: upload it
    const char *h = strstr(m, "\"h\":\""), *to = strstr(m, "\"to\":\"");
    char pat[96];
    int i;
    wchar_t full[MAX_PATH];
    char hx[65];
    sprintf_s(pat, sizeof(pat), "\"to\":\"%s\"", g_myId);
    if (!h || !to || !strstr(m, pat) || !is_hex64(h + 5)) return;
    for (i = 0; i < g_nshf; i++)
        if (!g_shf[i].del && !strncmp(g_shf[i].hex, h + 5, 64) && sh_target(g_shf[i].rel, full, MAX_PATH) &&
            hash_file(full, hx) && !strcmp(hx, g_shf[i].hex)) {
            char resp[80] = "";
            if (blob_http(L"POST", "/blobs/missing", NULL, 0, hx, 64, NULL, NULL, resp, sizeof(resp)) == 200 && !strstr(resp, hx)) return;   // there by now
            upload_queue(full, g_shf[i].hex, g_shf[i].size, 0);
            logmsg("SHFILE uploading %S again: someone's download found nothing", g_shf[i].rel);
            return;
        }
}

// A version only we have, before the others' replaces it: <root>\..\Shared backups\<time>\<rel> (a hard link
// to it, same drive: no copy). 0: no backup (then it isn't replaced).
static int sh_backup(const wchar_t *full, const wchar_t *rel, wchar_t *out, int cap) {
    wchar_t dir[MAX_PATH * 2], *slash;
    SYSTEMTIME st;
    const wchar_t *ext = wcsrchr(rel, L'.');
    char hx[65];
    GetLocalTime(&st);
    swprintf_s(out, cap, L"%s\\%04d-%02d-%02d %02d.%02d.%02d\\%s", g_shBackups, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, rel);
    if (wcslen(out) >= MAX_PATH - 1 && hash_file(full, hx))
        swprintf_s(out, cap, L"%s\\%04d-%02d-%02d %02d.%02d.%02d\\%.16S%s", g_shBackups, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, hx, ext ? ext : L"");
    if (wcslen(out) >= MAX_PATH - 1) return 0;
    wcscpy_s(dir, MAX_PATH * 2, out);
    slash = wcsrchr(dir, L'\\');
    *slash = 0;
    SHCreateDirectoryExW(NULL, dir, NULL);
    return CreateHardLinkW(out, full, NULL) || CopyFileW(full, out, FALSE);
}
// Older backups go to the Recycle Bin (folders are named by their time, so the name says how old).
static void sh_backups_prune(void) {
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH], cut[32];
    SYSTEMTIME st;
    FILETIME ft;
    ULARGE_INTEGER u;
    HANDLE h;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)SH_BACKUP_DAYS * 24 * 3600 * 10000000;
    ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    swprintf_s(cut, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    swprintf_s(pat, MAX_PATH, L"%s\\*", g_shBackups);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || wcslen(fd.cFileName) != 19 || wcsncmp(fd.cFileName, cut, 10) >= 0) continue;
        {
            wchar_t from[MAX_PATH + 2] = L"";
            SHFILEOPSTRUCTW op;
            swprintf_s(from, MAX_PATH + 1, L"%s\\%s", g_shBackups, fd.cFileName);
            from[wcslen(from) + 1] = 0;                     // a double-NUL-terminated list
            memset(&op, 0, sizeof(op));
            op.wFunc = FO_DELETE; op.pFrom = from;
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            SHFileOperationW(&op);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The download worker: one GET at a time into the job's .part, sha256-checked while it streams.
static DWORD WINAPI shDlThread(LPVOID p) {
    (void)p;
    for (;;) {
        ShJob j;
        wchar_t full[MAX_PATH], part[MAX_PATH];
        char what[80], got[65] = "";
        DWORD st = 0;
        WaitForSingleObject(g_shDlEvt, INFINITE);
        EnterCriticalSection(&g_shLock);
        if (!g_shDl.busy || g_shDl.done) { LeaveCriticalSection(&g_shLock); continue; }
        j = g_shDl.j;
        LeaveCriticalSection(&g_shLock);
        if (sh_target(j.rel, full, MAX_PATH)) {
            sh_part(full, j.hex, part, MAX_PATH);
            sprintf_s(what, sizeof(what), "/blob/%s", j.hex);
            st = blob_http(L"GET", what, NULL, 0, NULL, 0, part, got, NULL, 0);
            if (st == 200 && strcmp(got, j.hex) != 0) st = 0;
            if (st != 200) DeleteFileW(part);
        }
        EnterCriticalSection(&g_shLock);
        g_shDl.status = st;
        g_shDl.done = 1;
        LeaveCriticalSection(&g_shLock);
        SetEvent(g_shEvt);
    }
}

typedef struct { int n; uint64_t bytes; char who[64]; wchar_t *names; size_t len, cap; DWORD at; } ShBatch;
static void sh_batch_add(ShBatch *b, const ShJob *j, const wchar_t *full) {
    const wchar_t *nm = base_name(full);
    size_t k = wcslen(nm) + 1;
    if (!b->n) b->at = GetTickCount();
    b->n++; b->bytes += j->size;
    strcpy_s(b->who, sizeof(b->who), j->who);
    if (b->len + k + 1 > b->cap) { b->cap = (b->len + k + 1) * 2; b->names = (wchar_t *)realloc(b->names, b->cap * sizeof(wchar_t)); }
    memcpy(b->names + b->len, nm, k * sizeof(wchar_t));
    b->len += k;
}
// Finished files: one window line, and the plugins that use them load them again (one pass for a batch).
static void sh_batch_flush(ShBatch *b) {
    const wchar_t *nm;
    if (!b->n) return;
    EnterCriticalSection(&g_sampleLock);
    for (nm = b->names; nm < b->names + b->len; nm += wcslen(nm) + 1) {
        if (g_nreload == g_capreload) { g_capreload = g_capreload ? g_capreload * 2 : 16; g_reload = (wchar_t **)realloc(g_reload, sizeof(wchar_t *) * g_capreload); }
        g_reload[g_nreload++] = _wcsdup(nm);
    }
    LeaveCriticalSection(&g_sampleLock);
    logmsg("SHARED got %d file%s (%.1f MB) from %s", b->n, b->n == 1 ? "" : "s", b->bytes / 1048576.0, b->who);
    b->n = 0; b->bytes = 0; b->len = 0;
}
// A downloaded file into place. 1: done (or no longer wanted), 0: try again later.
static int sh_finish(ShJob *j, ShBatch *b) {
    wchar_t full[MAX_PATH], part[MAX_PATH], bak[MAX_PATH * 2] = L"";
    uint64_t sz;
    FILETIME ft;
    ShFile *f;
    int here;
    if (!sh_target(j->rel, full, MAX_PATH)) return 1;
    sh_part(full, j->hex, part, MAX_PATH);
    f = sh_entry(j->rel);
    here = file_info(full, &sz, &ft);
    if (here ? j->tsize != sz || j->tt != ft_ms(ft) : j->tsize != ~0ull) {
        DeleteFileW(part);                                  // changed here meanwhile: that one counts (the scan sends it)
        logmsg("SHFILE %S changed here while %s's came: kept yours", j->rel, j->who);
        return 1;
    }
    if (here && f->local && f->hex[0] && strcmp(f->hex, j->hex) != 0) {
        if (!sh_backup(full, j->rel, bak, MAX_PATH * 2)) {
            if (!j->warned) { j->warned = 1; logmsg("SHARED %s has a newer %S, but yours couldn't be backed up: not replaced yet", j->who, j->rel); }
            j->next = GetTickCount() + SH_NEED_MS;
            return 0;
        }
    }
    if (!MoveFileExW(part, full, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!j->warned) { j->warned = 1; logmsg("SHARED %S is in use here: it is replaced as soon as it's free", j->rel); }
        if (bak[0]) DeleteFileW(bak);                       // backed up again next time
        return 0;
    }
    if (!file_info(full, &sz, &ft)) { sz = j->size; GetSystemTimeAsFileTime(&ft); }
    strcpy_s(f->hex, sizeof(f->hex), j->hex);
    f->v = j->v; f->size = f->ps = sz; f->t = f->pt = ft_ms(ft);
    f->local = 0; f->dirty = 0; f->del = 0;
    g_shSave = 1;
    if (bak[0]) logmsg("SHARED %s's newer %S replaced yours (yours is in %S)", j->who, j->rel, bak);
    logmsg("SHFILE got %S from %s (%llu bytes, version %llu)", j->rel, j->who, (unsigned long long)j->size, (unsigned long long)j->v);
    sh_batch_add(b, j, full);
    return 1;
}
static void sh_status(void) {                    // "getting N files" in the session status, unless samples show theirs
    static DWORD at;
    static int shown;
    DWORD now = GetTickCount();
    int n = 0;
    uint64_t bytes = 0;
    ShJob *j;
    for (j = g_shJobs; j; j = j->nx) { n++; bytes += j->size; }
    if (g_upTotal || g_dnTotal) return;
    if (!n) { if (shown && g_peersJson[0]) net_show_connected(g_peersJson); shown = 0; return; }
    if (now - at < 1000) return;
    at = now; shown = 1;
    net_status(NET_ON, "Shared folder: getting %d file%s (%.1f MB)", n, n == 1 ? "" : "s", bytes / 1048576.0);
}
// The download jobs: hand the next one to the worker (or copy it from a file here with that content), take
// what it finished, put finished files in place.
static void sh_jobs(LONG gen, ShBatch *b) {
    ShJob **pp, *j;
    DWORD now = GetTickCount();
    int busy, done;
    DWORD st;
    EnterCriticalSection(&g_shLock);
    busy = g_shDl.busy; done = g_shDl.done; st = g_shDl.status;
    if (busy && done) {
        for (j = g_shJobs; j && !(g_shDl.gen == gen && !_wcsicmp(j->rel, g_shDl.j.rel) && !strcmp(j->hex, g_shDl.j.hex)); j = j->nx) {}
        if (j) {
            if (st == 200) j->got = 1;
            else {
                j->next = now + DL_RETRY_MS;
                // not on the server (any more, or a full list's file): ask its sender (after its own upload had time)
                if (st == 404 && now - j->first >= 5000 && now - j->asked >= SH_NEED_MS) {
                    char m[220];
                    j->asked = now;
                    sprintf_s(m, sizeof(m), "{\"t\":\"op\",\"op\":{\"kind\":\"shared.need\",\"h\":\"%s\",\"to\":\"%s\"}}", j->hex, j->from);
                    ws_send(m);
                }
            }
        }
        g_shDl.busy = g_shDl.done = 0;
        busy = 0;
    }
    LeaveCriticalSection(&g_shLock);
    for (pp = &g_shJobs; (j = *pp) != NULL && gen == g_shGen; ) {
        wchar_t full[MAX_PATH], part[MAX_PATH];
        if (j->got) {
            if ((LONG)(now - j->next) >= 0 && sh_finish(j, b)) { *pp = j->nx; free(j); continue; }
            if (!j->next || (LONG)(now - j->next) >= 0) j->next = now + DL_RETRY_MS;
            pp = &j->nx;
            continue;
        }
        if (now - j->first > DL_GIVEUP_MS) {
            logmsg("SHARED gave up on %S from %s: it never reached the server", j->rel, j->who);
            *pp = j->nx; free(j); continue;
        }
        if (busy || (LONG)(now - j->next) < 0 || !sh_target(j->rel, full, MAX_PATH)) { pp = &j->nx; continue; }
        {
            ULARGE_INTEGER fr;
            int i;
            if (GetDiskFreeSpaceExW(g_shRoot, &fr, NULL, NULL) && fr.QuadPart < j->size + SH_FREE_MIN) {
                logmsg("SHARED no room for %S (%.1f MB) on the shared folder's drive: skipped", j->rel, j->size / 1048576.0);
                *pp = j->nx; free(j); continue;
            }
            if (!sh_no_links(full)) { logmsg("SHARED %S: a folder on its way is a link, not written", j->rel); *pp = j->nx; free(j); continue; }
            wcscpy_s(part, MAX_PATH, full);
            *(wchar_t *)base_name(part) = 0;
            SHCreateDirectoryExW(NULL, part, NULL);
            sh_part(full, j->hex, part, MAX_PATH);
            for (i = 0; i < g_nshf; i++) {                  // the same content here already (moved or renamed): copy it
                wchar_t src[MAX_PATH];
                char hx[65];
                if (g_shf[i].del || strcmp(g_shf[i].hex, j->hex) || !sh_target(g_shf[i].rel, src, MAX_PATH)) continue;
                if (CopyFileW(src, part, FALSE) && hash_file(part, hx) && !strcmp(hx, j->hex)) { j->got = 1; break; }
                DeleteFileW(part);
            }
            if (j->got) continue;                           // finished on the next pass
            EnterCriticalSection(&g_shLock);
            g_shDl.j = *j; g_shDl.gen = gen; g_shDl.busy = 1; g_shDl.done = 0;
            LeaveCriticalSection(&g_shLock);
            SetEvent(g_shDlEvt);
            busy = 1;
        }
        pp = &j->nx;
    }
}

// handle_msg (any thread): a shared.files / shared.need for shThread. sh_joined: our list goes out.
static void sh_inbox(const char *m) {
    ShMsg *x = (ShMsg *)calloc(1, sizeof(ShMsg));
    x->m = _strdup(m);
    EnterCriticalSection(&g_shLock);
    if (g_shInTail) g_shInTail->nx = x; else g_shIn = x;
    g_shInTail = x;
    LeaveCriticalSection(&g_shLock);
    SetEvent(g_shEvt);
}
static void sh_joined(void) { InterlockedExchange(&g_shOn, 1); InterlockedExchange(&g_shFull, 1); SetEvent(g_shEvt); }
static void sh_disconnected(void) {                          // its downloads stop too, and so do the scans
    InterlockedExchange(&g_shOn, 0);
    InterlockedIncrement(&g_shGen);
    EnterCriticalSection(&g_shLock);
    while (g_shIn) { ShMsg *n = g_shIn->nx; free(g_shIn->m); free(g_shIn); g_shIn = n; }
    g_shInTail = NULL;
    LeaveCriticalSection(&g_shLock);
}

static DWORD WINAPI shThread(LPVOID p) {
    DWORD lastScan = 0, lastSave = 0;
    LONG gen = g_shGen;
    int first = 1, hinted = 0;
    ShBatch b;
    (void)p;
    memset(&b, 0, sizeof(b));
    {   // the folder, and where its table and backups go
        wchar_t d[MAX_PATH], low[MAX_PATH], *slash;
        DWORD a;
        size_t i;
        flimcfg_shared_dir(d, MAX_PATH);
        a = GetFileAttributesW(d);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            logmsg("SHARED %S is a link: the shared folder is off (make it a real folder)", d);
            return 0;
        }
        wcscpy_s(g_shBackups, MAX_PATH, d);
        slash = wcsrchr(g_shBackups, L'\\');
        if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_shBackups), L"Shared backups");
        for (i = 0; d[i]; i++) low[i] = towlower(d[i]);
        low[i] = 0;
        if (!flim_appdata(g_shTable, MAX_PATH)) return 0;
        swprintf_s(g_shTable + wcslen(g_shTable), MAX_PATH - wcslen(g_shTable), L"\\flim\\shared-%08llx.tsv",
                   (unsigned long long)(fnv64((const unsigned char *)low, i * 2) & 0xffffffffull));
        wcscpy_s(g_shRoot, MAX_PATH, d);                    // in_shared from here on
        sh_load();
        sh_backups_prune();
    }
    for (;;) {
        DWORD now;
        WaitForSingleObject(g_shEvt, 500);
        now = GetTickCount();
        if (gen != g_shGen) {                               // disconnected: what was on its way is dropped
            gen = g_shGen;
            while (g_shJobs) {
                wchar_t full[MAX_PATH], part[MAX_PATH];
                ShJob *n = g_shJobs->nx;
                if (sh_target(g_shJobs->rel, full, MAX_PATH)) { sh_part(full, g_shJobs->hex, part, MAX_PATH); DeleteFileW(part); }
                free(g_shJobs);
                g_shJobs = n;
            }
            sh_batch_flush(&b);
        }
        if (g_shSave && now - lastSave >= 5000) { sh_store(); lastSave = now; }
        if (!g_blobOn || !g_shOn) continue;
        for (;;) {
            ShMsg *x;
            EnterCriticalSection(&g_shLock);
            x = g_shIn;
            if (x) { g_shIn = x->nx; if (!g_shIn) g_shInTail = NULL; }
            LeaveCriticalSection(&g_shLock);
            if (!x) break;
            if (strstr(x->m, "\"kind\":\"shared.need\"")) sh_need(x->m); else sh_manifest(x->m);
            free(x->m); free(x);
        }
        if (g_shFull || now - lastScan >= SH_SCAN_MS) {
            LONG full;
            sh_scan(first);
            first = 0;
            lastScan = GetTickCount();
            full = InterlockedExchange(&g_shFull, 0);
            sh_send((int)full);
            if (!hinted && g_nshf == 0) {
                hinted = 1;
                logmsg("SHARED files in %S reach everyone at the same path: load samples for Serum 2, Kontakt and other plugins from there", g_shRoot);
            }
        }
        sh_jobs(gen, &b);
        if (b.n && (!g_shJobs || now - b.at >= SH_RELOAD_MS)) sh_batch_flush(&b);
        sh_status();
    }
}

// ---------------- connection (one net thread per connect) ----------------
// net_connect starts a thread that opens the socket, joins the room and runs the receive
// loop. Each connect / disconnect bumps g_netGen; a thread whose generation is stale
// (the user disconnected or reconnected meanwhile) no longer touches the status.
typedef struct { LONG gen; char url[512]; char pw[256]; } NetReq;
static volatile LONG g_netGen = 0;
static volatile LONG g_pollStarted = 0;
static char g_room[64] = "FLIM";

static void json_esc(const char *in, char *out, int cap) {
    int o = 0;
    for (; *in && o < cap - 7; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += sprintf_s(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

// Forget every lock: they belong to the connection (the server drops them on disconnect, or when it makes us sync-only).
static void locks_forget(void) {
    int i;
    EnterCriticalSection(&g_slotLock);
    for (i = 0; i < g_nslot; i++) { g_slots[i].lockedByMe = 0; g_slots[i].lockedByPeer = 0; }
    LeaveCriticalSection(&g_slotLock);
    EnterCriticalSection(&g_ulockLock);
    for (i = 0; i < g_nulocks; i++) { g_ulocks[i].mine = g_ulocks[i].granted = g_ulocks[i].peer = 0; g_ulocks[i].owner[0] = 0; }
    LeaveCriticalSection(&g_ulockLock);
}
static void net_reset(void) {
    EnterCriticalSection(&g_netTextLock);
    g_nfocus = 0;                                         // where the others were: belongs to the session
    LeaveCriticalSection(&g_netTextLock);
    InterlockedExchange(&g_isLeader, 0);
    InterlockedExchange(&g_syncOnly, 0);                  // (the server says again, if it still holds)
    gui_push(GUI_PEOPLE, "0\t\n");
    locks_forget();
    EnterCriticalSection(&g_ulockLock);
    g_peersJson[0] = 0;
    LeaveCriticalSection(&g_ulockLock);
}

static DWORD WINAPI netThread(LPVOID p) {
    NetReq *r = (NetReq *)p;
    NetUrl u;
    NetConn c;
    char err[200], name[2 * sizeof(g_myName)], pw[600], hello[1024];
    unsigned char *buf = NULL;
    DWORD cap = 0, len = 0;

    if (!parse_url(r->url, &u)) { net_status(NET_ERR, "Bad server address"); goto done; }
    logmsg("NET connecting to %s (%s, port %u)", r->url, u.secure ? "wss" : "ws", u.port);
    if (!ws_open(&u, &c, err, sizeof(err))) {
        logmsg("NET connect to %s failed: %s", r->url, err);
        if (r->gen == g_netGen) net_status(NET_ERR, "Can't connect: %s", err);
        goto done;
    }
    EnterCriticalSection(&g_sendLock);
    if (r->gen != g_netGen) {                       // cancelled while connecting
        LeaveCriticalSection(&g_sendLock);
        net_close_handles(&c);
        goto done;
    }
    g_ws = c.ws;
    strcpy_s(g_netShown, sizeof(g_netShown), u.shown);
    g_netErr[0] = 0;
    InterlockedExchange(&g_kicked, 0);
    LeaveCriticalSection(&g_sendLock);
    samples_connected(&u, r->pw);
    net_status(NET_CONNECTING, "Joining %s...", u.shown);

    json_esc(g_myName, name, sizeof(name));
    json_esc(r->pw, pw, sizeof(pw));
    sprintf_s(hello, sizeof(hello),
        "{\"t\":\"join_room\",\"room\":\"%s\",\"password\":\"%s\",\"user\":{\"id\":\"%s\",\"name\":\"%s\"}}",
        g_room, pw, g_myId, name);
    ws_send(hello);
    SecureZeroMemory(hello, sizeof(hello)); SecureZeroMemory(pw, sizeof(pw));
    logmsg("joining room %s as %s (%s)", g_room, g_myName, g_myId);
    if (!InterlockedExchange(&g_pollStarted, 1)) CreateThread(NULL, 0, pollThread, NULL, 0, NULL);

    for (;;) {
        DWORD got = 0, e;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (cap - len < 65536) { cap = cap ? cap * 2 : 262144; buf = (unsigned char *)realloc(buf, cap); }
        e = WinHttpWebSocketReceive(c.ws, buf + len, cap - len - 1, &got, &type);
        if (e) { logmsg("NET receive ended (%lu)", e); break; }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT code = 0; char reason[124] = { 0 }; DWORD rl = 0;
            WinHttpWebSocketQueryCloseStatus(c.ws, &code, reason, sizeof(reason) - 1, &rl);
            logmsg("NET server closed the connection (%u %.*s)", code, (int)rl, reason);
            break;
        }
        len += got;
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            continue;                               // more of the same message follows
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) { buf[len] = 0; inbox_msg((char *)buf); }
        len = 0;
    }

    EnterCriticalSection(&g_sendLock);
    if (g_ws == c.ws) g_ws = NULL;
    LeaveCriticalSection(&g_sendLock);
    samples_disconnected();
    if (r->gen == g_netGen) {                       // not our own Disconnect: tell the user plainly
        char a[400];
        net_reset();
        g_leaderAliveAt = 0;
        if (g_netErr[0]) net_status(NET_ERR, "%s", g_netErr);
        else net_status(NET_ERR, "Connection to %s lost", u.shown);
        if (g_kicked) sprintf_s(a, sizeof(a), "%s.\n\nYou can't join this session again unless the host lets you back in "
                                "(or until it ends). Your FL keeps the project as it is.", g_netErr);
        else sprintf_s(a, sizeof(a), "You're no longer in the session: %s.\n\nIf the PC hosting it stopped hosting (or went offline), "
                  "the session is over until it hosts again. Your FL keeps the project as it is; Connect joins again.",
                  g_netErr[0] ? g_netErr : "the connection to the server was lost");
        gui_push(GUI_ALERT, a);
    }
    net_close_handles(&c);
done:
    free(buf);
    SecureZeroMemory(r, sizeof(*r));
    free(r);
    return 0;
}

static void net_connect(const char *url, const char *pw) {
    NetReq *r;
    HANDLE th;
    if (g_syncOff[0]) { net_status(NET_ERR, "Sync is off: flim doesn't know this FL Studio (%S) yet", g_flBuild); return; }
    r = (NetReq *)calloc(1, sizeof(NetReq));
    r->gen = InterlockedIncrement(&g_netGen);
    strncpy_s(r->url, sizeof(r->url), url, _TRUNCATE);
    strncpy_s(r->pw, sizeof(r->pw), pw ? pw : "", _TRUNCATE);
    {
        NetUrl u;
        if (parse_url(url, &u)) net_status(NET_CONNECTING, "Connecting to %s...", u.shown);
    }
    th = CreateThread(NULL, 0, netThread, r, 0, NULL);
    if (th) CloseHandle(th);
}

// Main thread. Sends a close frame; the net thread sees the server's reply and cleans up.
static void net_disconnect(void) {
    InterlockedIncrement(&g_netGen);
    EnterCriticalSection(&g_sendLock);
    if (g_ws) {
        WinHttpWebSocketShutdown(g_ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        g_ws = NULL;
    }
    LeaveCriticalSection(&g_sendLock);
    net_reset();
    g_hostSavingUntil = 0;
    g_projAskedAt = 0;
    g_haveSong = 0;                 // (a new connection: its first project is a join)
    g_leaderAliveAt = 0;
    net_status(NET_OFF, "Not connected");
}

// ---------------- splash ----------------
// FL draws its startup fruit from <FL>\Artwork\Skins\Default\Title.png (read with CreateFileW
// right after TApplication is created). In this process only, that open is redirected to the
// bean (IDR_SPLASH, written to %TEMP%\flim\<pid>\splash.png). The file on disk is untouched,
// so FL started without flim keeps its own fruit. kernel32!CreateFileW is a "jmp [rip+x]" stub
// into kernelbase: we swap the stub's pointer slot (data, not code). It must be in place
// before FL's main thread runs, so it is installed from DllMain (the launcher resumes FL only
// once LoadLibrary has returned).
typedef HANDLE (WINAPI *createfilew_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static createfilew_t g_realCreateFileW = NULL;
static wchar_t g_splashPath[MAX_PATH];
static volatile LONG g_splashState = 0;        // 0 not written yet, 1 ready, -1 failed

static int splash_write(void) {
    HRSRC r = FindResourceW(g_hinst, MAKEINTRESOURCEW(IDR_SPLASH), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_hinst, r) : NULL;
    const void *data = g ? LockResource(g) : NULL;
    DWORD n = r ? SizeofResource(g_hinst, r) : 0, wrote = 0;
    wchar_t dir[MAX_PATH];
    HANDLE f;
    if (!data || !n) return 0;
    GetTempPathW(MAX_PATH, dir);
    wcscat_s(dir, MAX_PATH, L"flim");
    CreateDirectoryW(dir, NULL);
    swprintf_s(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"\\%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
    swprintf_s(g_splashPath, MAX_PATH, L"%s\\splash.png", dir);
    f = g_realCreateFileW(g_splashPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WriteFile(f, data, n, &wrote, NULL);
    CloseHandle(f);
    return wrote == n;
}

static int is_fl_title_png(LPCWSTR p) {
    static const wchar_t tail[] = L"\\Artwork\\Skins\\Default\\Title.png";
    size_t n, t = (sizeof(tail) / sizeof(wchar_t)) - 1;
    if (!p) return 0;
    n = wcslen(p);
    return n >= t && _wcsicmp(p + n - t, tail) == 0;
}

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                      DWORD disp, DWORD flags, HANDLE tmpl) {
    if (g_splashState >= 0 && disp == OPEN_EXISTING && !(access & GENERIC_WRITE) && is_fl_title_png(name)) {
        if (g_splashState == 0) InterlockedExchange(&g_splashState, splash_write() ? 1 : -1);
        if (g_splashState == 1) {
            HANDLE h = g_realCreateFileW(g_splashPath, access, share, sa, disp, flags, tmpl);
            if (h != INVALID_HANDLE_VALUE) return h;
        }
    }
    return g_realCreateFileW(name, access, share, sa, disp, flags, tmpl);
}

static void splash_hook_install(void) {
    unsigned char *stub = (unsigned char *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFileW");
    void **slot;
    DWORD old;
    if (!stub) return;
    if (stub[0] == 0x48) stub++;                                  // optional REX.W
    if (stub[0] != 0xFF || stub[1] != 0x25) return;               // not "jmp [rip+disp32]": keep FL's fruit
    slot = (void **)(stub + 6 + *(int32_t *)(stub + 2));
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return;
    g_realCreateFileW = (createfilew_t)*slot;
    InterlockedExchangePointer(slot, (void *)hook_CreateFileW);
    VirtualProtect(slot, sizeof(void *), old, &old);
}

// ---------------- FL addresses ----------------
// At start every signature of fl_addrs.h (client/dev/flsigs.py; FL2026-internals.md "Signatures") is
// looked up in FLEngine_x64.dll's .text. All found exactly once: the addresses come from there. Any
// missing or ambiguous: FL FLIM_FL_BUILD keeps the table's own VAs; any other build runs with sync
// off (FL untouched, flim.exe's window says why). Struct offsets and vmt slots can't be found that
// way: fl_sanity checks the ones it can once FL has settled.
static const struct { const char *name; int off, nx; const char *pat; } FL_SIGS[FA_N] = {
#define FLA(n, va, off, nx, pat) { #n, off, nx, pat },
#include "fl_addrs.h"
#undef FLA
};
static const unsigned char *g_flText;
static size_t g_flTextN;

static int hexd(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
// Places in [lo, lo + n) that match pat ("55 48 ?? ..."): the count (2 = two or more), *at = the first.
static int sig_find(const unsigned char *lo, size_t n, const char *pat, const unsigned char **at) {
    unsigned char b[160], w[160];          // byte, 1 = any byte
    int len = 0, k = -1, i, hits = 0;
    const unsigned char *p, *end;
    for (i = 0; pat[i] && pat[i + 1] && len < (int)sizeof(b); i += pat[i + 2] ? 3 : 2, len++) {
        w[len] = pat[i] == '?';
        b[len] = w[len] ? 0 : (unsigned char)(hexd(pat[i]) << 4 | hexd(pat[i + 1]));
        if (k < 0 && !w[len]) k = len;
    }
    if (k < 0 || (size_t)len > n) return 0;
    end = lo + n - len + k + 1;            // b[k] at p: the match lies inside [lo, lo + n)
    for (p = lo + k; p < end && (p = (const unsigned char *)memchr(p, b[k], end - p)) != NULL; p++) {
        const unsigned char *s = p - k;
        for (i = 0; i < len && (w[i] || s[i] == b[i]); i++) ;
        if (i == len && !hits++) *at = s;
        if (hits > 1) break;
    }
    return hits;
}

// Engine thread, first thing. 0 = sync off (g_syncOff says why).
static int fl_addrs_resolve(void) {
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_flBase + ((IMAGE_DOS_HEADER *)g_flBase)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    uint32_t got[FA_N];
    char bad[900] = "";
    wchar_t path[MAX_PATH];
    WORD v[4] = { 0 };
    int i, nbad = 0, differ = 0, ours;
    DWORD t0 = GetTickCount();
    GetModuleFileNameW((HMODULE)g_flBase, path, MAX_PATH);
    file_version(path, v);
    swprintf_s(g_flBuild, 32, L"%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    ours = wcscmp(g_flBuild, FLIM_FL_BUILD) == 0;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sh[i].Name, ".text", 6) == 0) { g_flText = (unsigned char *)g_flBase + sh[i].VirtualAddress; g_flTextN = sh[i].Misc.VirtualSize; }
    for (i = 0; i < FA_N; i++) {
        const unsigned char *at = NULL;
        int hits = g_flText ? sig_find(g_flText, g_flTextN, FL_SIGS[i].pat, &at) : 0;
        size_t rva = 0;
        if (hits == 1) {
            rva = (size_t)(at - (unsigned char *)g_flBase);
            if (FL_SIGS[i].off >= 0) rva += FL_SIGS[i].nx + *(const int32_t *)(at + FL_SIGS[i].off);
            if (rva >= nt->OptionalHeader.SizeOfImage) hits = -1;
        }
        if (hits != 1) {
            nbad++;
            sprintf_s(bad + strlen(bad), sizeof(bad) - strlen(bad), "%s%s (%s)", bad[0] ? ", " : "", FL_SIGS[i].name,
                      hits == 0 ? "not found" : hits < 0 ? "outside FL" : "found twice");
            continue;
        }
        got[i] = (uint32_t)rva + IMG_BASE;
        if (ours && got[i] != g_va[i]) {
            differ++;
            logmsg("FL addresses: %s found at %#x, but FL %S's table says %#x", FL_SIGS[i].name, got[i], FLIM_FL_BUILD, g_va[i]);
        }
    }
    if (!nbad && !differ) {
        memcpy(g_va, got, sizeof(g_va));
        logmsg("FL addresses: FL %S, all %d signatures found once in %lu ms, using them%s", g_flBuild, FA_N, GetTickCount() - t0,
               ours ? " (equal to this build's table)" : "");
        return 1;
    }
    if (nbad) logmsg("FL addresses: FL %S, %d of %d signatures failed: %s", g_flBuild, nbad, FA_N, bad);
    if (ours) { logmsg("FL addresses: using FL %S's own table", FLIM_FL_BUILD); return 1; }
    sprintf_s(g_syncOff, sizeof(g_syncOff), "%d of FL's %d functions and globals flim uses were not found: %s", nbad, FA_N, bad);
    return 0;
}

// Once FL has settled: offsets and vmt slots signatures can't find. Each failed check is named in bad.
static int fl_text_has(const void *p) { return (const unsigned char *)p >= g_flText && (const unsigned char *)p < g_flText + g_flTextN; }
static int ustr_ok(const wchar_t *s) { return !s || (((const uint16_t *)s)[-6] == 1200 && ((const uint16_t *)s)[-5] == 2); }   // UnicodeString header
#define MIX_INDEX 0x28                 // mixer track: its own index (int)
#define CH_INDEX  0x794                // channel object: its index (int)
static int sane_mixer(void) {
    int n = **(int **)(g_flBase + (MIXCNT_VA - IMG_BASE)), t;
    if (n < 3 || n > 502) return 0;
    for (t = 0; t < n; t++)
        if (*(int *)(mix_rec(t) + MIX_INDEX) != t || !ustr_ok(*(wchar_t **)(mix_rec(t) + MIX_NAME))) return 0;
    return 1;
}
static int sane_channels(void) {
    void **items;
    int n = chan_list(&items), i;
    for (i = 0; i < n; i++) {
        char *c = (char *)items[i];
        void **vmt = *(void ***)c;
        if (*(int *)(c + CH_INDEX) != i || *(uint8_t *)(c + CH_SELECTED) > 1 || !ustr_ok(*(wchar_t **)(c + CH_NAME)) ||
            !fl_text_has(vmt[CH_SETNAME_VSLOT]) || !fl_text_has(vmt[CH_SETCOLOR_VSLOT])) return 0;
    }
    return 1;
}
static int sane_patterns(void) {       // note arrays: element size at +0x10
    int p;
    for (p = 1; p < MAX_PAT; p++) {
        char *a = *(char **)(pat_rec(p) + PAT_NOTES);
        if ((a && *(uint32_t *)(a + 0x10) != NOTE_SIZE) || !ustr_ok(*(wchar_t **)(pat_rec(p) + PAT_NAME))) return 0;
    }
    return 1;
}
static int sane_clips(void) {
    char *a = arr_clips(0);
    return !a || *(uint32_t *)(a + 0x10) == CLIP_SIZE;
}
static int sane_project(void) {        // the vmt slots flim calls / hooks
    void **vmt = *(void ***)fl_project();
    int k;
    if (!fl_text_has(vmt[FLPROJ_LOAD_SLOT])) return 0;
    for (k = 0; k < PATOP_N; k++) if (!fl_text_has(vmt[PATOP_SLOT[k] / 8])) return 0;
    return 1;
}
static int fl_sanity(char *bad, int cap) {
    static const struct { const char *what; int (*check)(void); } C[] = {
        { "mixer tracks", sane_mixer }, { "channels", sane_channels }, { "patterns", sane_patterns },
        { "playlist clips", sane_clips }, { "project object", sane_project } };
    int i, n = 0, ok;
    bad[0] = 0;
    for (i = 0; i < (int)(sizeof(C) / sizeof(C[0])); i++) {
        __try { ok = C[i].check(); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
        if (!ok) { n++; sprintf_s(bad + strlen(bad), cap - strlen(bad), "%s%s", bad[0] ? ", " : "", C[i].what); }
    }
    return n;
}

// Sync off: FL runs untouched; only our task window lives (on this thread), so flim.exe's window gets
// the reason and its Connect is answered (net_connect refuses).
static void sync_off_idle(void) {
    char a[1400];
    MSG m;
    logmsg("SYNC OFF: %s", g_syncOff);
    task_window_create();
    net_status(NET_ERR, "Sync is off: flim doesn't know this FL Studio (%S) yet", g_flBuild);
    sprintf_s(a, sizeof(a), "flim can't sync with this FL Studio (version %S), so nothing is shared: %s.\n\n"
              "FL itself works normally. A flim update for this FL version is needed (flim was made for FL %S).",
              g_flBuild, g_syncOff, FLIM_FL_BUILD);
    gui_push(GUI_ALERT, a);
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// ---------------- boot ----------------
static DWORD WINAPI engineMain(LPVOID p) {
    (void)p;
    srand(GetTickCount() ^ GetCurrentProcessId());
    logmsg("=== flim v." FLIM_VERSION " - engine v4.7 (connect button + wss; params + plugin state + channels + native move + fx clear + notes + project files + playlist clips + unit locks + names/colors/routing + plugin locks + samples + markers/time signature + pattern markers/time signatures + pattern ops + plugin samples + focus + tempo/EQ/slot mix + per-channel/per-track locks + sample placeholders + plugin window checks + joined song folder/zip + mixer track insert/delete) loaded ===");

    HMODULE fl = NULL;
    int tries;
    for (tries = 0; tries < 600 && !fl; tries++) { fl = GetModuleHandleW(FL_MODULE); if (!fl) Sleep(50); }
    if (!fl) { logmsg("FLEngine module not found"); return 0; }
    g_flBase = (char *)fl;
    if (!fl_addrs_resolve()) { sync_off_idle(); return 0; }
    g_rec = (rec_t)(g_flBase + REC_RVA);
    g_setEnabled = (setenabled_t)(g_flBase + MUTE_RVA);
    g_savePreset = (presetfile_t)(g_flBase + (SAVEPRESET_VA - IMG_BASE));
    g_loadPreset = (presetfile_t)(g_flBase + (LOADPRESET_VA - IMG_BASE));
    g_hasPlugin  = (hasplugin_t)(g_flBase + (HASPLUGIN_VA - IMG_BASE));
    g_getPlugin  = (getplugin_t)(g_flBase + (GETPLUGIN_VA - IMG_BASE));
    g_intfClear  = (intfclear_t)(g_flBase + (INTFCLEAR_VA - IMG_BASE));
    g_addChannel = (addchan_t)(g_flBase + (ADDCHAN_VA - IMG_BASE));
    g_delChannel = (delchan_t)(g_flBase + (DELCHAN_VA - IMG_BASE));
    g_resolve    = (resolver_t)(g_flBase + (RESOLVER_VA - IMG_BASE));
    g_moveChans  = (movechan_t)(g_flBase + (MOVECHAN_VA - IMG_BASE));
    g_noteArr    = (notearr_t)(g_flBase + (NOTEARR_VA - IMG_BASE));
    g_noteDel    = (notedel_t)(g_flBase + (NOTEDEL_VA - IMG_BASE));
    g_gridRefresh = (gridrefresh_t)(g_flBase + (GRIDREFRESH_VA - IMG_BASE));
    logmsg("FLEngine base=%p rec=%p setEnabled=%p", (void *)fl, (void *)g_rec, (void *)g_setEnabled);
    logmsg("splash hook %s, splash %ld", g_realCreateFileW ? "on" : "OFF (kernel32 stub not recognised)", g_splashState);

    // per-process scratch dir for plugin state files
    GetTempPathA(MAX_PATH, g_tmpDir);
    strcat_s(g_tmpDir, MAX_PATH, "flim");
    CreateDirectoryA(g_tmpDir, NULL);
    sprintf_s(g_tmpDir + strlen(g_tmpDir), MAX_PATH - strlen(g_tmpDir), "\\%lu", GetCurrentProcessId());
    CreateDirectoryA(g_tmpDir, NULL);

    {
        char *slash;
        GetModuleFileNameA(NULL, g_fxDeletePreset, MAX_PATH);
        slash = strrchr(g_fxDeletePreset, '\\');
        if (slash) slash[1] = 0;
        strcat_s(g_fxDeletePreset, MAX_PATH, "Data\\System\\(delete) effect.fst");
        if (GetFileAttributesA(g_fxDeletePreset) == INVALID_FILE_ATTRIBUTES) logmsg("WARNING: %s not found", g_fxDeletePreset);
    }

    // Wait for the main window (FL fully booted), then hook its message loop.
    if (install_mainthread_hook() != 0) return 0;

    // FL loads the startup template a moment after its window appears, which
    // resets every param. Join only once params have been quiet for SETTLE_MS,
    // otherwise that reset would be taken for edits (or undo the joined state).
    {
        const DWORD SETTLE_MS = 3000, SETTLE_MAX_MS = 60000;
        DWORD start = GetTickCount(), quietSince;
        while (!fl_ready() && GetTickCount() - start < SETTLE_MAX_MS) Sleep(100);
        logmsg("FL ready after %lu ms", GetTickCount() - start);
        Sleep(500);
        quietSince = GetTickCount();
        rescan();
        while (GetTickCount() - quietSince < SETTLE_MS && GetTickCount() - start < SETTLE_MAX_MS) {
            Sleep(250);
            LONG before = g_nslot;
            int i, changed = 0;
            rescan();
            for (i = 0; i < g_nslot; i++) {
                uint32_t v = read_value(g_slots[i].id);
                if (v != g_slots[i].last) { g_slots[i].last = v; changed++; }
            }
            if (changed || g_nslot != before) quietSince = GetTickCount();
        }
        logmsg("settled after %lu ms (%ld params)", GetTickCount() - start, g_nslot);
        EnumWindows(findMainForm, (LPARAM)&g_mainForm);
        logmsg("main form %p", (void *)g_mainForm);
        {
            char bad[200];
            if (!fl_sanity(bad, sizeof(bad))) logmsg("FL sanity checks passed");
            else if (wcscmp(g_flBuild, FLIM_FL_BUILD) == 0) logmsg("FL sanity checks failed: %s (FL %S's own build: carrying on)", bad, g_flBuild);
            else {
                sprintf_s(g_syncOff, sizeof(g_syncOff), "FL's data doesn't look the way flim expects (%s)", bad);
                UnhookWindowsHookEx(g_hook);
                UnhookWindowsHookEx(g_hookCwp);
                sync_off_idle();
                return 0;
            }
        }
        patops_install();
        save_detour_install();
        delchan_detour_install();
        chsolo_detour_install();
        midiin_detour_install();
        loadpreset_detour_install();
        InterlockedExchange(&g_uiWanted, 1);
        PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    }

    {   // who we are: flim.ini name (flim.exe's setup), FLIM_NAME overrides (dev: two FLs on one PC)
        wchar_t w[128], clean[FLIM_NAME_MAX + 1];
        char env[600], pw[256] = "";
        if (GetEnvironmentVariableW(L"FLIM_NAME", w, 128) == 0) flimcfg_get(L"name", w, 128);
        if (flim_sanitize_name(w, clean, FLIM_NAME_MAX + 1))
            WideCharToMultiByte(CP_UTF8, 0, clean, -1, g_myName, sizeof(g_myName), NULL, NULL);
        else sprintf_s(g_myName, sizeof(g_myName), "FL-%lu", GetCurrentProcessId());
        if (GetEnvironmentVariableA("FLIM_ROOM", env, sizeof(env)) > 0) {
            int i, o = 0;                               // room goes into JSON unescaped: keep it simple
            for (i = 0; env[i] && o < (int)sizeof(g_room) - 1; i++)
                if (isalnum((unsigned char)env[i]) || env[i] == '-' || env[i] == '_') g_room[o++] = env[i];
            g_room[o] = 0;
            if (!o) strcpy_s(g_room, sizeof(g_room), "FLIM");
        }
        sprintf_s(g_myId, sizeof(g_myId), "fl-%lu-%u", GetCurrentProcessId(), (unsigned)rand());
        g_plugLockMs = flimcfg_lock_s() * 1000;
        backup_setting();
        g_logDetailed = flimcfg_log_detailed();
        g_showCursors = flimcfg_show_cursors();
        logmsg("I am %s (%s); not connected until flim's window connects%s", g_myName, g_myId, g_logDetailed ? " (detailed log)" : "");

        // flim.exe's Join / Host (and tests): connect right away. UTF-8, like the Connect dialog.
        {
            wchar_t wv[600];
            env[0] = 0;
            if (GetEnvironmentVariableW(L"FLIM_AUTOCONNECT", wv, 600) > 0) WideCharToMultiByte(CP_UTF8, 0, wv, -1, env, sizeof(env), NULL, NULL);
            else if (GetEnvironmentVariableW(L"FLIM_SERVER", wv, 600) > 0) {
                char srv[400];
                WideCharToMultiByte(CP_UTF8, 0, wv, -1, srv, sizeof(srv), NULL, NULL);
                sprintf_s(env, sizeof(env), "ws://%s", srv);
            }
            if (env[0]) {
                if (GetEnvironmentVariableW(L"FLIM_PASSWORD", wv, 600) > 0) WideCharToMultiByte(CP_UTF8, 0, wv, -1, pw, sizeof(pw), NULL, NULL);
                SetEnvironmentVariableW(L"FLIM_PASSWORD", NULL);    // not left in FL's environment
                SecureZeroMemory(wv, sizeof(wv));
                net_connect(env, pw);
                SecureZeroMemory(pw, sizeof(pw));
            }
        }
    }
    // This thread installed the main-thread hooks, and Windows removes a hook when the
    // thread that set it exits: it must stay alive for as long as FL runs.
    for (;;) Sleep(INFINITE);
}

// ---------------- crash dump (always; FLIM_CRASHDUMP=0 turns it off) ----------------
// The first access violation at an address outside every loaded module (code that was unloaded,
// e.g. a plugin's DLL) writes %TEMP%\flim_crash_<pid>.dmp, then lets the exception go on as usual.
// flim.exe puts it in the crash report when FL ends (launcher: crash_collect).
typedef BOOL (WINAPI *minidump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, void *, void *);
static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep) {
    static volatile LONG done = 0;
    HMODULE m;
    wchar_t path[MAX_PATH];
    minidump_t dump;
    MINIDUMP_EXCEPTION_INFORMATION mx;
    HANDLE f;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &m)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&done, 1)) return EXCEPTION_CONTINUE_SEARCH;
    dump = (minidump_t)GetProcAddress(LoadLibraryW(L"dbghelp.dll"), "MiniDumpWriteDump");
    GetTempPathW(MAX_PATH, path);
    swprintf_s(path + wcslen(path), MAX_PATH - wcslen(path), L"flim_crash_%lu.dmp", GetCurrentProcessId());
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    mx.ThreadId = GetCurrentThreadId(); mx.ExceptionPointers = ep; mx.ClientPointers = FALSE;
    if (dump && f != INVALID_HANDLE_VALUE)
        dump(GetCurrentProcess(), GetCurrentProcessId(), f,
             MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules, &mx, NULL, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    logmsg("CRASH: access violation in unloaded code at %p (thread %lu): dump %S",
           ep->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(), path);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hinst = h;
        DisableThreadLibraryCalls(h);
        splash_hook_install();
        InitializeCriticalSection(&g_sendLock);
        InitializeCriticalSection(&g_slotLock);
        InitializeCriticalSection(&g_qLock);
        InitializeCriticalSection(&g_heldLock);
        InitializeCriticalSection(&g_ulockLock);
        InitializeCriticalSection(&g_netTextLock);
        InitializeCriticalSection(&g_guiLock);
        InitializeCriticalSection(&g_sampleLock);
        InitializeCriticalSection(&g_outLock);
        InitializeCriticalSection(&g_inLock);
        InitializeCriticalSection(&g_autoLock);
        g_outEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_inEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_upEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_dnEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
        {   // flim.exe's window, which shows our status and log
            wchar_t gv[32];
            if (GetEnvironmentVariableW(L"FLIM_GUI", gv, 32) > 0) g_gui = (HWND)(uintptr_t)wcstoull(gv, NULL, 16);
            SetEnvironmentVariableW(L"FLIM_GUI", NULL);
            g_guiEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
            CreateThread(NULL, 0, guiThread, NULL, 0, NULL);
        }
        {
            char v[8];
            g_noLocal = GetEnvironmentVariableA("FLIM_SAMPLE_NOLOCAL", v, sizeof(v)) && v[0] == '1';
            g_curNoCover = GetEnvironmentVariableA("FLIM_CURSOR_NOCOVER", v, sizeof(v)) && v[0] == '1';
            if (!(GetEnvironmentVariableA("FLIM_CRASHDUMP", v, sizeof(v)) && v[0] == '0')) AddVectoredExceptionHandler(1, crash_veh);
        }
        CreateThread(NULL, 0, outThread, NULL, 0, NULL);
        CreateThread(NULL, 0, inThread, NULL, 0, NULL);
        CreateThread(NULL, 0, upThread, NULL, 0, NULL);
        CreateThread(NULL, 0, dnThread, NULL, 0, NULL);
        InitializeCriticalSection(&g_shLock);
        g_shEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_shDlEvt = CreateEventW(NULL, FALSE, FALSE, NULL);
        CreateThread(NULL, 0, shThread, NULL, 0, NULL);
        CreateThread(NULL, 0, shDlThread, NULL, 0, NULL);
        CreateThread(NULL, 0, engineMain, NULL, 0, NULL);
    }
    return TRUE;
}

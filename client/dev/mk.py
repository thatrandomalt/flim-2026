# Helpers for poking at FL 2026's state through the probe (see README.md; addresses and what the
# functions do: FL2026-internals.md). A, B = pids of the FLs restart_test.ps1 starts as "A" (leader)
# and "B" (member), read from the engine log. An FL of another build (flrun.py) works too: its
# addresses come from the engine's signatures (V(p, name) below).
import ctypes, fl, json, struct, os, re, subprocess, sys, time

LOG = os.path.join(os.environ['TEMP'], 'flim_engine.log')

def session_pids(log=LOG):
    a = b = None
    for l in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'\[(\d+)\] I am (A|B) ', l)
        if m:
            if m.group(2) == 'A': a = int(m.group(1))
            else: b = int(m.group(1))
    return a, b

try: A, B = session_pids()
except OSError: A = B = None

def probe(pid):
    try: p = fl.Probe(pid)
    except Exception:
        fl.inject(pid); time.sleep(0.5); p = fl.Probe(pid)
    p.v = build_vas(pid)
    return p

# The FL addresses of the build pid runs: flsigs.py --json on its FLEngine_x64.dll (cached per DLL).
def build_vas(pid):
    k = ctypes.WinDLL('kernel32')
    h = k.OpenProcess(0x1000, False, pid)                        # PROCESS_QUERY_LIMITED_INFORMATION
    buf, n = ctypes.create_unicode_buffer(1024), ctypes.c_ulong(1024)
    k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)); k.CloseHandle(h)
    dll = os.path.join(os.path.dirname(buf.value), 'FLEngine_x64.dll')
    st = os.stat(dll)
    cache = os.path.join(os.environ['TEMP'], 'flim_vas_%d_%d.json' % (st.st_size, int(st.st_mtime)))
    if not os.path.exists(cache):
        here = os.path.dirname(os.path.abspath(__file__))
        out = subprocess.run([sys.executable, os.path.join(here, 'flsigs.py'), '--json'], cwd=here, capture_output=True,
                             text=True, env=dict(os.environ, FLRE_DLL=dll)).stdout
        json.loads(out)
        open(cache, 'w').write(out)
    return json.load(open(cache))

# Address `name` in p's FL: the signature's, or (26.1.7.5654) the constant below.
def V(p, name):
    v = getattr(p, 'v', {})
    if name in v: return v[name]
    if v.get('REC', REC) != REC: raise KeyError('%s: no signature, and this FL is not 26.1.7.5654' % name)
    return globals()[name]

def ustr(p, addr): return p.ustr(addr) if addr else ''
def va(p, addr): return addr - p.base + 0x400000                # absolute (e.g. from a vmt) -> VA for p.call

# ---- FL 2026 26.1.7.5654 (VAs, image base 0x400000) ----
REC = 0x1046a70
MIXARR, MIXCNT, MIX_STRIDE, MIX_SLOTS = 0x15e06a0, 0x15e2198, 0x1478, 0x134c
CHANLIST, PATARR, ARRS, ARRCUR = 0x15e2218, 0x15e2aa0, 0x196eb60, 0x15d65f0
PLFORM, PRFORM, MAINFORM, FLPROJ = 0x15e35c8, 0x15e2478, 0x15e13b8, 0x15e4790
TSNUM, TSDEN, PROJNAME, CURPAT = 0x15e39c0, 0x15e1c70, 0x16bcc98, 0x15e4018
MIXINS, MIXDEL, MIXMOVE, MIXNAME, HASPLUGIN = 0x12a46f0, 0x12a5280, 0x12a4490, 0x12be6f0, 0x132f7c0
SAVEPRESET, LOADPRESET = 0xf54570, 0xf54760
MIXFOCUS = 0x193fa10                                            # int: focused mixer track
HASPLUG = HASPLUGIN

def rec_get(p, id_): return p.call(V(p, 'REC'), id_, 0, 2) & 0xffffffff
def rec_set(p, id_, v): return p.call(V(p, 'REC'), id_, v, 0x11) & 0xffffffff
def mix_id(t, sub): return 0x70000000 + (t << 22) + sub         # 0x1fc0 vol, 0x1fc1 pan ... (engine MIX_SUBS)

# ---- mixer ----
def mix_count(p): return p.d(p.g(V(p, 'MIXCNT')))
def mix_rec(p, t): return p.g(V(p, 'MIXARR')) + t * MIX_STRIDE
def mix_name(p, t): return ustr(p, p.q(mix_rec(p, t) + 0x0c))
def fx_slot(p, t, s): return p.q(mix_rec(p, t) + MIX_SLOTS + 8 * s)
def has_plug(p, t, s=0): return p.call(V(p, 'HASPLUGIN'), fx_slot(p, t, s)) & 0xff
def mix_ids(p): return [fx_slot(p, t, 0) for t in range(mix_count(p))]   # track identities
def mix_insert(p, pos, n=1, flags=7): return p.call(V(p, 'MIXINS'), pos, n, flags)   # 7: what the Insert menu passes
def mix_delete(p, tracks, flags=0x12):                          # 0x12: the Delete menu (after its dialogs)
    m = p.alloc(16 + 4 * len(tracks))                           # a Delphi dynamic array: refcount, length, ints
    p.w(m + 4, struct.pack('<iq', 0x3fffffff, len(tracks)) + struct.pack('<%di' % len(tracks), *tracks))
    return p.call(V(p, 'MIXDEL'), m + 16, len(tracks) - 1, flags)
def mix_move(p, t, d=1):                                        # FL's "move selected" (+1 right / -1 left)
    for k in range(mix_count(p)): p.w(mix_rec(p, k) + 0x19, bytes([k == t]))
    p.w(p.a(V(p, 'MIXFOCUS')), struct.pack('<i', t))            # the focused track moves too
    return p.call(V(p, 'MIXMOVE'), d, 1, 0)

# ---- channels, patterns, playlist ----
def chans(p):
    lst = p.q(p.g(V(p, 'CHANLIST'))); items = p.q(lst + 8); n = p.d(lst + 0x10)
    return [p.q(items + 8 * i) for i in range(n)]
def chan_names(p): return [ustr(p, p.q(c + 0x50)) for c in chans(p)]
def pat_rec(p, i): return p.g(V(p, 'PATARR')) + i * 0xC0
def pat_names(p, n=7): return [ustr(p, p.q(pat_rec(p, i))) for i in range(1, n + 1)]
def notes(p, pat):
    a = p.q(pat_rec(p, pat) + 0x28)
    if not a: return []
    n = p.d(a + 0x14); d = p.q(a + 8); sz = p.d(a + 0x10)
    return [p.r(d + i * sz, sz) for i in range(n)]
def arr(p, i=None):
    if i is None: i = p.d(p.a(V(p, 'ARRCUR')))
    return p.q(p.g(V(p, 'ARRS')) + 8 * i)
def clips(p, i=None):
    ca = p.q(arr(p, i) + 0x14)
    n = p.d(ca + 0x14); d = p.q(ca + 8); sz = p.d(ca + 0x10)
    return sz, [p.r(d + k * sz, sz) for k in range(n)]
def _marks(p, l):
    n = p.q(l - 8) if l else 0
    return [(p.d(l + k * 0x34), p.d(l + k * 0x34 + 4), ustr(p, p.q(l + k * 0x34 + 8)), p.r(l + k * 0x34 + 0x10, 0x24).hex()) for k in range(n)]
def marks(p, i=None): return _marks(p, p.q(arr(p, i) + 0x21c48))    # playlist markers: (pos, action, name, tail)
def pmarks(p, pat): return _marks(p, p.q(pat_rec(p, pat) + 0x30))    # pattern's: time signature = action 8, tail[0:2]
def ts(p): return p.d(p.g(V(p, 'TSNUM'))), p.d(p.g(V(p, 'TSDEN')))
def proj(p): return p.q(p.q(p.g(V(p, 'FLPROJ'))) + 0x10)
def patop(p, slot, *args):                                      # project vmt: clone 0xe8, delete 0xf8, insert 0x118, split 0x120
    pr = proj(p)
    return p.call(va(p, p.q(p.q(pr) + slot)), pr, *args)

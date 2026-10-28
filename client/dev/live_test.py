# Live sync checks on two FLs started by restart_test.ps1 (A = leader, B = member, FL 2026
# 26.1.7.5654): make a change in A (or B) through FL's own functions via the probe, wait, compare.
#   python live_test.py [test ...]            (no names: all, in order; e.g. "mix_insert mix_delete")
# FLIM_TEST_VST=<.fst of a plugin to load into a mixer slot> (default: Xfer OTT from FL's plugin database).
# FLIM_TEST_URL=<host:port of the server A and B are in> (default localhost:8080): reconnects, host controls' raw client.
import sys, os, re, time, struct, math, threading
import fl
from mk import *

sys.stdout.reconfigure(errors='replace')                     # project names the console can't show
a, b = probe(A), probe(B)
ok = fail = 0

# flim only syncs what someone does: a param that moves with nobody at the keyboard is FL's own
# (automation, modulation). The probe makes no input, so a background "user" keeps tapping an
# unused key (F24, key up) in both FLs while the tests run.
quiet = {'on': False}                                           # True: the background user pauses (nobody at the keyboard)
def _user():
    import ctypes
    while True:
        if quiet['on']: time.sleep(0.2); continue
        for w in fl.windows((A, B)):
            if w[2] == 'TFruityLoopsMainForm': ctypes.windll.user32.PostMessageW(w[1], 0x0101, 0x87, 0xC0000001)
        time.sleep(0.5)
threading.Thread(target=_user, daemon=True).start()

def check(name, fa, fb, wait=2.5):
    global ok, fail
    t0 = time.time()
    while True:
        va_, vb = fa(), fb()
        if va_ == vb or time.time() - t0 > wait: break
        time.sleep(0.25)
    good = va_ == vb
    ok += good; fail += not good
    s = '%-36s %s   A=%r B=%r' % (name, 'OK  ' if good else 'FAIL', va_, vb)
    print(s if not good else s[:200])

def closer(pid, secs=4):                        # FL opens name boxes after some pattern ops
    def run():
        for _ in range(secs * 4):
            time.sleep(0.25); fl.close_dialogs((pid,))
    threading.Thread(target=run, daemon=True).start()

def fl_dir():
    try:
        for l in open(os.path.join(os.environ['APPDATA'], 'flim', 'flim.ini'), encoding='utf-16'):
            if l.startswith('fl_dir='): return l.strip()[7:]
    except OSError: pass
    return r'C:\Program Files\Image-Line\FL Studio 2026'
VST_FST = os.environ.get('FLIM_TEST_VST', os.path.join(os.environ['USERPROFILE'],
          r'Documents\Image-Line\FL Studio\Presets\Plugin database\Installed\Effects\VST3\OTT.fst'))
DELETE_FST = os.path.join(fl_dir(), r'Data\System\(delete) effect.fst')
TMP = os.environ['TEMP']
URL = os.environ.get('FLIM_TEST_URL', 'localhost:8080')         # the server A and B are in (reconnects, the raw client)

def make_wav(path, hz):
    n = 44100
    data = b''.join(struct.pack('<h', int(8000 * math.sin(i * 2 * math.pi * hz / 44100))) for i in range(n))
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                           struct.pack('<IHHIIHH', 16, 1, 1, 44100, 88200, 2, 16) + b'data' + struct.pack('<I', len(data)) + data)

def fst_with_sample(src, dst, wav, replace=False):
    # the .fst again with a sample path event (196) after the plugin name (203); replace: instead of its own 196.
    # FL 2026: byte 172 escapes a 32-bit id, sized by its top bits when >= 0x100 (see flp.py)
    d = open(src, 'rb').read()
    j = d.find(b'FLdt'); p = j + 8; out = bytearray(d[:j + 8])
    path = (wav + '\0').encode('utf-16le')
    def varint(n):
        o = b''
        while True:
            c = n & 0x7f; n >>= 7
            o += bytes([c | (0x80 if n else 0)])
            if not n: return o
    while p < len(d):
        q = p; e = d[q]; q += 1
        if e == 172: e = struct.unpack_from('<I', d, q)[0]; q += 4
        c = e >> 6 if e < 0x100 else e >> 30
        if c < 3: n = (1, 2, 4)[c]
        else:
            n = sh = 0
            while True:
                x = d[q]; q += 1; n |= (x & 0x7f) << sh; sh += 7
                if x < 128: break
        if replace and e == 196: out += bytes([196]) + varint(len(path)) + path
        else: out += d[p:q + n]
        if e == 203 and not replace: out += bytes([196]) + varint(len(path)) + path
        p = q + n
    out[j + 4:j + 8] = struct.pack('<I', len(out) - j - 8)
    open(dst, 'wb').write(out)

# ---- tests ----
def t_params():
    ids = {'tempo': 0x40000005, 'mix1 vol': mix_id(1, 0x1fc0), 'mix1 EQ gain0': mix_id(1, 0x1fd0),
           'mix1 slot0 mix': 0x70000000 + ((1 << 6 | 0) << 16) + 0x1f01, 'ch0 pitch': (0 << 16) | 4, 'ch0 vol': 0,
           'swing': 0x40000001}
    for nm, i in ids.items():
        cur = rec_get(a, i)
        rec_set(a, i, (cur + 2345) % 12800 if nm != 'tempo' else 123456)
        check('param ' + nm, lambda i=i: rec_get(a, i), lambda i=i: rec_get(b, i))

def t_main_volume():                                            # each person's own listening level
    before = rec_get(b, 0x40000000)
    rec_set(a, 0x40000000, (rec_get(a, 0x40000000) + 2345) % 12800); time.sleep(2)
    check('main volume stays local', lambda: rec_get(b, 0x40000000), lambda: before)

def t_mixer_name():
    a.call(MIXNAME, 2, a.mkstr('FlimMix'), 1)
    check('mixer name', lambda: mix_name(a, 2), lambda: mix_name(b, 2))

def t_mixer_mute():
    for on, nm in ((0, 'mixer mute'), (1, 'mixer unmute')):          # (FL fades a mute in: both reach it a moment later)
        a.call(0x127b9b0, 3, on)
        check(nm, lambda: (a.r(mix_rec(a, 3) + 0x18, 1), b.r(mix_rec(b, 3) + 0x18, 1)), lambda on=on: (bytes([on]),) * 2, wait=4)

def t_route():
    mixer = a.q(a.g(0x15e22d8))
    a.call(0x12a33c0, mixer, 4, 5, 1, 1); a.call(0x12a28f0, mixer)
    check('route 4->5 on', lambda: a.r(mix_rec(a, 4) + 0x394 + 5 * 8 + 4, 1), lambda: b.r(mix_rec(b, 4) + 0x394 + 5 * 8 + 4, 1))

def t_steps():
    grid = a.q(chans(a)[0] + 0x7ac)
    pat = a.d(a.g(CURPAT))                      # A's current pattern (B's may differ: it isn't synced)
    for pos in (0, 4, 8, 12): a.call(0x12d4140, grid, pos, 1, 1, 100)
    check('steps -> notes (current pattern)', lambda: sorted(n[:8] for n in notes(a, pat)),
          lambda: sorted(n[:8] for n in notes(b, pat)), wait=4)

def t_pattern_name():
    a.call(0x12d0fc0, 2, a.mkstr('FlimPat'))
    check('pattern name', lambda: ustr(a, a.q(pat_rec(a, 2))), lambda: ustr(b, b.q(pat_rec(b, 2))))

def t_timesig():
    a.call(0x1366990, a.q(a.g(MAINFORM)), 3)
    check('time signature', lambda: ts(a), lambda: ts(b))

def t_marker():
    a.call(0x10637c0, arr(a) + 0x21c48, 384, a.mkstr('FlimMark'), 0, 4, 4)
    a.call(0xe53810, a.q(a.g(PLFORM)))
    check('playlist marker', lambda: [m[:3] for m in marks(a)], lambda: [m[:3] for m in marks(b)])

def t_add_channel():
    a.call(0x1004230, -1, 0, 1)
    check('channel added', lambda: len(chans(a)), lambda: len(chans(b)), wait=8)
    check('channel names', lambda: chan_names(a), lambda: chan_names(b), wait=4)

def t_chan_name():
    ch = chans(a)[-1]
    a.call(va(a, a.q(a.q(ch) + 0x70)), ch, a.mkstr('FlimChan'))
    check('channel rename', lambda: chan_names(a), lambda: chan_names(b))

def t_clip():
    ca = a.q(arr(a) + 0x14); vmt = a.q(ca)
    rec = a.alloc(0x58)
    a.call(va(a, a.q(vmt + 16)), ca, rec)                      # default record
    for pos, trk in ((0, 499), (768, 498)):                     # pattern 1 (0x5000 + 1), 384 ticks long
        a.w(rec, struct.pack('<IIIH', pos, 0x5000 + 1, 384, trk))
        a.call(va(a, a.q(vmt + 8)), ca, rec)
    a.call(0xe39560, a.q(a.g(PLFORM)))
    check('playlist clips', lambda: sorted(r[:16] for r in clips(a)[1]), lambda: sorted(r[:16] for r in clips(b)[1]), wait=4)

# A clip FL changes by itself (nobody at the keyboard: FL recomputing a stretched audio clip after the other's
# clips were applied) is taken as known, not sent: sent, the two FLs ping-ponged it.
def t_clip_self():
    ca = a.q(arr(a) + 0x14); n = a.d(ca + 0x14); d = a.q(ca + 8)
    if not n: print('   (no clips: skipped)'); return
    rec = d + (n - 1) * 0x58
    quiet['on'] = True; time.sleep(2)                           # (input_recent: 1.5 s)
    pos = os.path.getsize(LOG)
    a.w(rec + 0x34, struct.pack('<f', 0.625))
    time.sleep(3)
    quiet['on'] = False
    check("clip changed by FL itself: taken as known on A", lambda: any('taken as known, not sent' in l for l in log_since(pos, A)), lambda: True, wait=0)
    check("...not sent to B", lambda: any('CLIPS sent' in l for l in log_since(pos, A)), lambda: False, wait=0)
    a.w(rec + 0x34, struct.pack('<f', 1.0)); time.sleep(3)      # (and back, as FL would: also not sent)

def plug_state(p, t, s, tag):                                   # the slot's .fst minus the wrapper header
    path = os.path.join(TMP, 'flim_live_%s.fst' % tag)          # (the header holds each person's editor window)
    if os.path.exists(path): os.remove(path)
    p.call(V(p, 'SAVEPRESET'), p.mkstr(path), 48, fx_slot(p, t, s), 1)
    d = open(path, 'rb').read() if os.path.exists(path) else b''
    return d[d.find(b'XTSV'):]

def t_vst():
    if not os.path.exists(VST_FST): print('   (no %s: VST tests skipped)' % VST_FST); return
    a.call(0xf54760, a.mkstr(VST_FST), 48, fx_slot(a, 1, 0), 0)
    check('VST in slot', lambda: plug_state(a, 1, 0, 'a')[:64], lambda: plug_state(b, 1, 0, 'b')[:64], wait=12)   # the same plugin (the slot may have held another)
    time.sleep(1)
    sa, sb = plug_state(a, 1, 0, 'a'), plug_state(b, 1, 0, 'b')
    check('VST full state equal', lambda: sa, lambda: sb, wait=0)
    pid = 0x70000000 + ((1 << 6 | 0) << 16) + 0x8000
    rec_set(a, pid, (rec_get(a, pid) + 3000) % 65536)
    check('VST param 0', lambda: rec_get(a, pid), lambda: rec_get(b, pid), wait=4)

# A knob's "Type in value..." box (TNameEditForm) is a top-level window of its own: typing a value there is work in
# the plugin window it came from, so that plugin gets checked again (a state-only value, like the sampler's time
# stretch, went out late). Here: a pattern insert's rename box stands in for it (its pattern op holds flim's tasks
# until it closes, so the check comes after).
def t_typein():
    import ctypes
    if not has_plug(a, 1, 0): return
    u = ctypes.windll.user32
    def checks(pos): return [l for l in log_since(pos, A) if 'PLUGFOCUS: window' in l and ' is fx 1/0' in l]
    slot = fx_slot(a, 1, 0); show = va(a, a.q(a.q(slot) + 0xE0)); a.call(show, slot, 1); time.sleep(2)
    win = [k for p, h, c, ti in fl.windows((A,)) for k, cc in [(h, c)] + fl.children(h) if cc == 'TPluginForm' and u.IsWindowVisible(k)]
    if not win: print('   (no plugin window: typein skipped)'); return
    pos = os.path.getsize(LOG); fl.post_click(win[0])
    check('typein: a click in the plugin window checks it', lambda: len(checks(pos)) > 0, lambda: True, wait=10)
    time.sleep(1); pos = os.path.getsize(LOG)
    def typer():
        for _ in range(40):
            time.sleep(0.25)
            for q, h, c, t in fl.windows((A,)):
                if c == 'TNameEditForm':
                    tgt = next((k for k, cc in fl.children(h) if 'Edit' in cc), h)
                    for ch in 'ABC':
                        u.PostMessageW(tgt, 0x100, ord(ch), 0); u.PostMessageW(tgt, 0x102, ord(ch.lower()), 0); u.PostMessageW(tgt, 0x101, ord(ch), 0)
                        time.sleep(0.2)
                    time.sleep(0.5); u.PostMessageW(h, 0x10, 0, 0); return
    threading.Thread(target=typer, daemon=True).start()
    patop(a, 0x118, 3, 0, 0, 0, 0)
    check('typein: typing in its box checks it again', lambda: len(checks(pos)) > 0, lambda: True, wait=10)
    a.call(show, slot, 0)

def t_vst_remove():
    if not os.path.exists(VST_FST): return
    time.sleep(5)                                               # let the editor FL opened on load finish starting
    a.call(0xf54760, a.mkstr(DELETE_FST), 48, fx_slot(a, 1, 0), 0)
    check('VST removed (fx.clear)', lambda: has_plug(a, 1), lambda: has_plug(b, 1), wait=6)

def t_patop_insert():
    a.call(0x12d0fc0, 3, a.mkstr('P3')); time.sleep(1.5)
    closer(A); patop(a, 0x118, 2, 0, 0, 0, 0)                   # insert before pattern 2
    time.sleep(0.5); fl.close_dialogs((A,))
    check('pattern insert', lambda: pat_names(a), lambda: pat_names(b), wait=5)

def t_patop_clone():
    buf = a.alloc(8); a.w(buf, struct.pack('<i', 3))
    closer(A); patop(a, 0xe8, buf, 0, 0, 0, 0)                  # clone([3])
    time.sleep(0.5); fl.close_dialogs((A,))
    check('pattern clone', lambda: pat_names(a), lambda: pat_names(b), wait=5)

def t_patop_move():                                             # (pats, delta, -1): each swaps with the one delta away
    a.call(0x12d0fc0, 2, a.mkstr('MV')); time.sleep(1.5)       # (two empty unnamed patterns swapped: no change)
    buf = a.alloc(8); a.w(buf, struct.pack('<i', 2))
    pos = os.path.getsize(LOG)
    patop(a, 0x108, buf, 0, 2, -1 & 0xffffffffffffffff, 0)
    check('pattern move', lambda: pat_names(a), lambda: pat_names(b), wait=5)
    check('pattern move: its map matched', lambda: any('isn\'t old pattern' in l for l in log_since(pos, A) + log_since(pos, B)), lambda: False, wait=0)

def t_patop_split():                                            # notes on two channels in pattern 3, split by channel
    if len(chans(a)) < 2: add_named(a, 1, 'SplitTwo'); time.sleep(2)
    src = next((n for p in range(1, 12) for n in notes(a, p)), bytes(24))
    for ch, at in ((0, 0), (1, 96)):
        r = bytearray(src); struct.pack_into('<I', r, 0, at); struct.pack_into('<H', r, 6, ch)
        arr = a.call(0x12d16e0, 3, 0); rec = a.alloc(24); a.w(rec, bytes(r)); a.call(va(a, a.q(a.q(arr) + 8)), arr, rec)
    a.call(PATLENUPD, 3, 1)
    time.sleep(2)
    buf = a.alloc(8); a.w(buf, struct.pack('<i', 3))
    pos = os.path.getsize(LOG)
    def answer():
        for _ in range(16):
            time.sleep(0.25)
            fl.confirm((A,), 0) if any(w[2] == 'TMsgForm' for w in fl.windows((A,))) else None
    threading.Thread(target=answer, daemon=True).start()
    patop(a, 0x120, buf, 0, 0, 0, 0)
    check('pattern split', lambda: pat_names(a, 12), lambda: pat_names(b, 12), wait=6)
    check('pattern split: notes', lambda: [sorted(unsel(notes(a, p))) for p in range(1, 12)], lambda: [sorted(unsel(notes(b, p))) for p in range(1, 12)], wait=4)
    check('pattern split: its map matched', lambda: any('isn\'t old pattern' in l for l in log_since(pos, A) + log_since(pos, B)), lambda: False, wait=0)

# A channel routed to a mixer track; the track then moved in a quick burst of single steps (Shift + mouse
# wheel over it, or the keyboard shortcut). The target is a channel param whose value is a track number.
def target(p, ch): return rec_get(p, (ch << 16) | 8)
def t_target_move():
    rec_set(a, (0 << 16) | 8, 3)
    check('channel target set', lambda: target(b, 0), lambda: 3, wait=4)
    seq = [(3, 1), (4, 1), (5, 1), (6, 1), (7, -1), (6, -1), (5, -1), (4, 1), (5, 1)]   # one wheel notch each
    for t, d in seq:
        mix_move(a, t, d); time.sleep(0.06)
    check('channel target after the moves (A)', lambda: target(a, 0), lambda: 6, wait=3)
    check('channel target after the moves (B)', lambda: target(b, 0), lambda: target(a, 0), wait=5)
    time.sleep(4)                                               # (and it stays: nothing echoes back)
    check('channel target stays', lambda: target(b, 0), lambda: target(a, 0), wait=0)
    for t, d in [(6, -1), (5, -1)]:                             # B moves it back while A's still fresh
        wait_reload(b); mix_move(b, t, d); time.sleep(0.06)
    check('channel target after B\'s moves', lambda: target(a, 0), lambda: target(b, 0), wait=5)

# Shift + mouse wheel over a channel / a playlist track header moves it one step per notch: quick bursts.
# An FL asks for / loads the leader's project and catches up: a user can't act then (input held) and it sends
# nothing, but the probe bypasses that, so test edits on that side wait for it like a user would.
_rl = {}
def reloading(pid):
    st = _rl.setdefault(pid, {'pos': os.path.getsize(LOG), 'loading': False})
    with open(LOG, 'rb') as f:
        f.seek(st['pos']); data = f.read()
    end = data.rfind(bytes([10])) + 1                       # (whole lines only; the rest next time)
    st['pos'] += end
    for l in data[:end].decode('utf-8', 'replace').splitlines():
        if '[%d]' % pid not in l: continue
        if 'kind":"state.req"' in l and 'OP out' in l or 'PROJECT received' in l: st['loading'] = True
        elif 'PROJECT caught up' in l: st['loading'] = False
    return st['loading']
def wait_reload(p):
    pid = A if p is a else B; t0 = time.time()
    while reloading(pid) and time.time() - t0 < 15: time.sleep(0.2)
def chan_step(p, i, d):                                         # FL's move selected (4 up / 5 down) with only channel i
    wait_reload(p)
    for k, c in enumerate(chans(p)): p.w(c + 0x32c, bytes([k == i]))
    p.call(0x1042800, p.q(p.g(0x15e14a0)), 4 if d < 0 else 5, p.d(p.g(0x15e3a60)), 1)
def rack(p): return [(n, target(p, i)) for i, n in enumerate(chan_names(p))]
def t_chan_wheel():
    while len(chans(a)) < 5: add_named(a, len(chans(a)), 'W%d' % len(chans(a))); time.sleep(1.5)
    for i in range(len(chans(a))): rec_set(a, (i << 16) | 8, i + 1)   # each routed to its own track
    time.sleep(3)
    check('rack before', lambda: rack(a), lambda: rack(b), wait=4)
    for i, d in [(0, 1), (1, 1), (2, 1), (3, -1), (2, -1)]:      # channel 0 down 3, back up 2
        chan_step(a, i, d); time.sleep(0.06)
    check('rack after A\'s burst', lambda: rack(a), lambda: rack(b), wait=6)
    def burst_b():
        for i, d in [(4, -1), (3, -1), (2, -1)]: chan_step(b, i, d); time.sleep(0.06)
    t = threading.Thread(target=burst_b); t.start()
    for i, d in [(0, 1), (1, 1)]: chan_step(a, i, d); time.sleep(0.06)
    t.join()
    check('rack after both at once', lambda: rack(a), lambda: rack(b), wait=30)

def pl_names(p, n=8): return [ustr(p, p.q(arr(p) + t * 0x114 + 0x24)) for t in range(1, n + 1)]
def pl_step(p, t, d):                                           # FL's move selected playlist tracks: up (-1) / down (+1)
    wait_reload(p)
    for k in range(1, 12): p.w(arr(p) + k * 0x114 + 0xe5, bytes([k == t]))   # (+0xe5: selected)
    p.call(0x12eb580, arr(p), d & 0xffffffff, t)                # (arr, direction, track: -1 = the selected one)
def t_pl_wheel():
    for t in range(1, 7): a.call(0x12e60f0, arr(a), t, a.mkstr('T%d' % t), 0x404040 + t)
    ca = a.q(arr(a) + 0x14); vmt = a.q(ca); rec = a.alloc(0x58)
    a.call(va(a, a.q(vmt + 16)), ca, rec)
    for t in (2, 4):                                            # a clip on tracks 2 and 4
        a.w(rec, struct.pack('<IIIH', 96 * t, 0x5000 + 1, 384, 500 - t)); a.call(va(a, a.q(vmt + 8)), ca, rec)
    a.call(0xe39560, a.q(a.g(PLFORM)))
    time.sleep(3)
    pl = lambda p: (pl_names(p), sorted(r[:16] for r in clips(p)[1]))
    check('playlist before', lambda: pl(a), lambda: pl(b), wait=5)
    for t, d in [(2, 1), (3, 1), (4, 1), (5, -1), (4, -1)]:      # track 2 down 3, back up 2
        pl_step(a, t, d); time.sleep(0.06)
    check('playlist after A\'s burst', lambda: pl(a), lambda: pl(b), wait=8)
    def burst_b():
        for t, d in [(6, -1), (5, -1)]: pl_step(b, t, d); time.sleep(0.06)
    th = threading.Thread(target=burst_b); th.start()
    for t, d in [(1, 1), (2, 1)]: pl_step(a, t, d); time.sleep(0.06)
    th.join()
    check('playlist after both at once', lambda: pl(a), lambda: pl(b), wait=30)

# A playlist track's link: an audio track (linked to a mixer track, FL's sub_12F38F0) or an instrument track
# (linked to a channel by the channel's own id, +0x794). Then the linked things move.
def pl_link(p, t):
    r = arr(p) + t * 0x114; lt = p.d(r + 0xd8)
    if lt == 1: return (1, p.d(r + 0xdc))
    if lt == 3: ids = [p.d(c + 0x794) for c in chans(p)]; i = p.d(r + 0xe0); return (3, ids.index(i) if i in ids else -1)
    return (lt, -1)
def links_pl(p): return [pl_link(p, t) for t in range(1, 9)]
def t_pl_links():
    while len(chans(a)) < 3: add_named(a, len(chans(a)), 'L%d' % len(chans(a))); time.sleep(1.5)
    a.call(0x12f38f0, arr(a), 3, 5)                             # track 3: audio track on insert 5
    check('audio track link', lambda: links_pl(a), lambda: links_pl(b), wait=5)
    r = arr(a) + 4 * 0x114                                      # track 4: instrument track of channel 1
    a.w(r + 0xd8, struct.pack('<i', 3)); a.w(r + 0xe0, struct.pack('<i', a.d(chans(a)[1] + 0x794)))
    check('instrument track link', lambda: links_pl(a), lambda: links_pl(b), wait=5)
    for t, d in [(5, 1), (6, 1), (7, -1)]: mix_move(a, t, d); time.sleep(0.06)   # its insert moved in a burst
    check('links after the insert moved', lambda: links_pl(a), lambda: links_pl(b), wait=6)
    chan_step(a, 1, -1); time.sleep(0.06); chan_step(a, 0, 1)   # its channel moved up and back
    chan_step(b, 2, -1)                                         # (and B moves another at the same time)
    check('links after channels moved', lambda: links_pl(a), lambda: links_pl(b), wait=30)
    for t, d in [(3, 1), (4, 1)]: pl_step(a, t, d); time.sleep(0.06)   # the linked tracks moved in the playlist
    check('links after playlist tracks moved', lambda: links_pl(a), lambda: links_pl(b), wait=8)
    colors = lambda p: [p.d(c + 0x78) for c in chans(p)]       # (a linked channel takes no track's color on B only)
    check('channel colors after the links', lambda: colors(a), lambda: colors(b), wait=4)

def t_patlen():
    a.w(pat_rec(a, 2) + 0x44, struct.pack('<i', 4 * a.d(a.g(0x15e25f8))))   # 4 beats of PPQ
    a.call(0x12d2350, 2)
    check('pattern length', lambda: a.d(pat_rec(a, 2) + 0x44), lambda: b.d(pat_rec(b, 2) + 0x44), wait=4)

def t_pat_timesig():
    a.call(0x10637c0, pat_rec(a, 2) + 0x30, 0, a.mkstr('3/4'), 8, 3, 4)    # action 8 = time signature at 0
    a.call(0x12d17a0, 2, 0); a.call(0x12d0b60, pat_rec(a, 2))
    check('pattern time signature marker', lambda: [m[:2] + (m[3][:4],) for m in pmarks(a, 2)],
          lambda: [m[:2] + (m[3][:4],) for m in pmarks(b, 2)], wait=4)

def t_colors():
    a.call(0x12c17e0, mix_rec(a, 2), 0x0000ff00)
    check('mixer color', lambda: a.d(mix_rec(a, 2) + 4), lambda: b.d(mix_rec(b, 2) + 4))
    c = chans(a)[0]; a.call(va(a, a.q(a.q(c) + 0x78)), c, 0x00ff0000)
    check('channel color', lambda: [a.d(x + 0x78) for x in chans(a)], lambda: [b.d(x + 0x78) for x in chans(b)])

def t_pltrack():
    a.call(0x12e60f0, arr(a), 3, a.mkstr('FlimTrack'), 0x000000ff)
    check('playlist track name', lambda: ustr(a, a.q(arr(a) + 3 * 0x114 + 0x24)), lambda: ustr(b, b.q(arr(b) + 3 * 0x114 + 0x24)))

def t_chan_order():
    cs = chans(a)
    for i, c in enumerate(cs): a.w(c + 0x32c, bytes([i == len(cs) - 1]))
    a.call(0x1042800, a.q(a.g(0x15e14a0)), 4, a.d(a.g(0x15e3a60)), 1)   # move selected up
    check('channel reorder', lambda: chan_names(a), lambda: chan_names(b), wait=5)

# A tester cancelled a slow project load: FL kept 1 channel of 44, flim sent 43 deletes and emptied the leader's
# song too. Most channels gone in one scan is the project: B (a member) loads A's project again, A keeps its channels.
def call_many(p, pid, fn, arglists):
    """Call FL function fn (VA) once per (a1, a2, a3) in one main-thread call, so flim's scans see only the end."""
    import ctypes
    k = ctypes.windll.kernel32
    k.OpenProcess.restype = ctypes.c_void_p; k.VirtualAllocEx.restype = ctypes.c_void_p
    k.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
    k.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
    code = b'\x48\x83\xec\x28'                                                        # sub rsp, 0x28
    for a1, a2, a3 in arglists:
        code += b'\xb9' + struct.pack('<I', a1 & 0xffffffff) + b'\xba' + struct.pack('<I', a2 & 0xffffffff)   # mov ecx / mov edx
        code += b'\x41\xb8' + struct.pack('<I', a3 & 0xffffffff)                     # mov r8d
        code += b'\x48\xb8' + struct.pack('<Q', p.a(fn)) + b'\xff\xd0'               # mov rax, fn; call rax
    code += b'\x48\x83\xc4\x28\xc3'                                                   # add rsp, 0x28; ret
    h = k.OpenProcess(0x1F0FFF, False, pid)
    mem = k.VirtualAllocEx(h, None, len(code), 0x3000, 0x40)
    k.WriteProcessMemory(h, mem, code, len(code), None)
    k.CloseHandle(ctypes.c_void_p(h))
    return p.call(va(p, mem))

def t_mass_delete():
    while len(chans(a)) < 7: add_named(a, len(chans(a)), 'Mass%d' % len(chans(a)))
    check('mass delete: setup', lambda: len(chans(b)), lambda: len(chans(a)), wait=10)
    n = len(chans(a)); time.sleep(2); pos = os.path.getsize(LOG)
    call_many(b, B, 0x1004a60, [(i, 0, 3) for i in range(n - 1, 0, -1)])            # all but channel 0, in one go
    time.sleep(4)
    check('mass delete: A keeps its channels', lambda: len(chans(a)), lambda: n, wait=0)
    check('mass delete: B took it as the project', lambda: any('gone at once' in l for l in log_since(pos, B)), lambda: True, wait=0)
    check('mass delete: B has A\'s channels again', lambda: chan_names(b), lambda: chan_names(a), wait=30)

# A load of the leader's project that FL ends with most channels missing (cancelled half-way): loaded again once.
# Here: B's re-sync is set off as in mass_delete, then most channels go again right after FL opened it (inside the
# settle window), as a stopped load leaves them.
def t_short_load():
    while len(chans(a)) < 7: add_named(a, len(chans(a)), 'Short%d' % len(chans(a)))
    check('short load: setup', lambda: len(chans(b)), lambda: len(chans(a)), wait=10)
    n = len(chans(a)); time.sleep(2); pos = os.path.getsize(LOG)
    call_many(b, B, 0x1004a60, [(i, 0, 3) for i in range(n - 1, 0, -1)])
    t0 = time.time()
    while time.time() - t0 < 20 and not any('PROJECT opened' in l for l in log_since(pos, B)): time.sleep(0.05)
    call_many(b, B, 0x1004a60, [(i, 0, 3) for i in range(len(chans(b)) - 1, 0, -1)])
    check("short load: B saw it didn't load completely", lambda: any("didn't load completely" in l or 'loading it again' in l for l in log_since(pos, B)), lambda: True, wait=15)
    check("short load: B has A's channels", lambda: chan_names(b), lambda: chan_names(a), wait=30)
    check('short load: A kept its channels', lambda: len(chans(a)), lambda: n, wait=0)

def t_chan_delete():
    a.call(0x1004a60, len(chans(a)) - 1, 0, 3)
    check('channel delete', lambda: len(chans(a)), lambda: len(chans(b)), wait=5)

def t_sample():
    wav, src, dst = (os.path.join(TMP, n) for n in ('flim live test.wav', 'flim_live_ch0.fst', 'flim_live_smp.fst'))
    make_wav(wav, 220 + time.time() % 100)                      # new content each run: a real upload
    a.call(0xf54570, a.mkstr(src), 32, chans(a)[0], 1)
    fst_with_sample(src, dst, wav)
    a.call(0x1004230, -1, 0, 1)
    a.call(0xf54760, a.mkstr(dst), 32, chans(a)[-1], 0)
    check('channel with sample added', lambda: len(chans(a)), lambda: len(chans(b)), wait=10)

# Another sample into an existing Sampler, through FL's loader (its own state with another sample path, loaded with
# LoadPresetFile, which FL's browser drop onto a channel calls too). No knob moves and no plugin window gets
# input: the preset-load detour is what makes it work in that channel. (A raw .wav handed to LoadPresetFile
# from here returns 2 and loads nothing; the drop's other calls weren't reproduced.)
def wav_in(p, i, tag):                                          # the .wav p's channel i plays (its bytes)
    s = chan_state_bytes(p, i, tag)
    for k in (0, 1):
        m = re.search(r'[A-Za-z]:\\[^\x00-\x1f]*?\.wav', s[k:].decode('utf-16le', 'replace'))
        if m and os.path.exists(m.group()): return open(m.group(), 'rb').read()
def t_sample_drop():
    wav = os.path.join(TMP, 'flim live drop.wav')
    make_wav(wav, 600 + time.time() % 100)                      # new content each run: B downloads it
    i = next((i for i in range(len(chans(a))) if wav_in(a, i, 'a') is not None), None)   # a Sampler playing a .wav
    if i is None: print('   (no channel plays a .wav: skipped)'); return
    want, ch = open(wav, 'rb').read(), chans(a)[i]
    src, dst = (os.path.join(TMP, n) for n in ('flim_live_sampler.fst', 'flim_live_sampler2.fst'))
    a.call(V(a, 'SAVEPRESET'), a.mkstr(src), 32, ch, 1)
    fst_with_sample(src, dst, wav, replace=True)
    a.call(V(a, 'LOADPRESET'), a.mkstr(dst), 32, ch, 1)
    check('another sample into a Sampler', lambda: (wav_in(a, i, 'a') == want, True), lambda: (True, wav_in(b, i, 'b') == want), wait=15)
    # LoadPresetFile can leave FL's running-loads count (FLLOADS) raised; flim, which waits while FL loads, then
    # held everything on that FL for good
    check('...FL\'s load count back to 0', lambda: a.d(a.g(0x15e3908)), lambda: 0)

# Any file a plugin loads travels: an SF2 Player with a soundfont from a user folder (B must download it:
# restart_test.ps1 -NoLocalB). Its path is a Delphi short string in the plugin data (length byte, no end).
SF2_FST = os.path.join(fl_dir(), r'Data\Patches\Plugin database\Generators\Misc\SoundFont Player.fst')
def fst_with_sf2(src, dst, sf2):
    # the factory preset again, its soundfont (in event 213, after a length byte) replaced by sf2
    d = open(src, 'rb').read()
    j = d.find(b'FLdt'); p = j + 8; out = bytearray(d[:j + 8])
    def varint(n):
        o = b''
        while True:
            c = n & 0x7f; n >>= 7
            o += bytes([c | (0x80 if n else 0)])
            if not n: return o
    while p < len(d):
        q = p; e = d[q]; q += 1
        if e == 172: e = struct.unpack_from('<I', d, q)[0]; q += 4
        c = e >> 6 if e < 0x100 else e >> 30
        if c < 3: n = (1, 2, 4)[c]
        else:
            n = sh = 0
            while True:
                x = d[q]; q += 1; n |= (x & 0x7f) << sh; sh += 7
                if x < 128: break
        v = d[q:q + n]
        i = v.find(b'%FLStudioFactoryData%')
        if e == 213 and i > 0:
            s = sf2.encode('mbcs')
            v = v[:i - 1] + bytes([len(s)]) + s + v[i + v[i - 1]:]
            out += bytes([213]) + varint(len(v)) + v
        else: out += d[p:q + n]
        p = q + n
    out[j + 4:j + 8] = struct.pack('<I', len(out) - j - 8)
    open(dst, 'wb').write(out)
def sf2_in(p, ch, tag):                                  # (path, bytes) of the soundfont p's channel ch points at
    path = os.path.join(TMP, 'flim_live_sf2_%s.fst' % tag)
    if os.path.exists(path): os.remove(path)
    p.call(V(p, 'SAVEPRESET'), p.mkstr(path), 32, chans(p)[ch], 1)
    d = open(path, 'rb').read() if os.path.exists(path) else b''
    m = re.search(rb'[A-Za-z]:\\[^\x00-\x1f]*?\.sf2', d)
    f = m.group().decode('mbcs') if m else ''
    return f, (open(f, 'rb').read() if f and os.path.exists(f) else None)
def t_sf2():
    if not os.path.exists(SF2_FST): print('   (no %s: SF2 test skipped)' % SF2_FST); return
    folder = os.path.join(os.environ['USERPROFILE'], 'Documents', 'flim live test soundfonts')
    os.makedirs(folder, exist_ok=True)
    for k, name in enumerate(('Strings A.sf2', 'Strings B.sf2')):
        sf2 = os.path.join(folder, name)
        with open(sf2, 'wb') as f:                       # a real upload each run: a new tail (RIFF readers stop at its end)
            f.write(open(os.path.join(fl_dir(), r'Data\Patches\Soundfonts\STR_Ensemble.sf2'), 'rb').read() + b'flim%f' % time.time())
        dst = os.path.join(TMP, 'flim_live_sf2_%d.fst' % k)
        fst_with_sf2(SF2_FST, dst, sf2)
        if k == 0: a.call(0x1004230, -1, 0, 1)            # a new channel; then the second soundfont into the same one
        a.call(0xf54760, a.mkstr(dst), 32, chans(a)[-1], 1)
        want = open(sf2, 'rb').read()
        check('SF2 soundfont %s arrives' % 'AB'[k], lambda: (len(chans(a)), True),
              lambda: (len(chans(b)), sf2_in(b, -1, 'b')[1] == want), wait=20)

# kHs plugins keep their state as a zip stamped with the save time: no two saves are the same. Each sweep
# pass sent such a plugin from whichever FL had any input (each apply reloads it: a glitch), and after one
# click in its window, every key elsewhere counted as work in it (sent every 3 s). Its entry times aren't
# a change any more (state_hash), so a click in its window gets it checked and sends nothing.
def t_drift():
    import ctypes
    def state(t, s):
        path = os.path.join(TMP, 'flim_live_drift.fst')
        if os.path.exists(path): os.remove(path)
        a.call(V(a, 'SAVEPRESET'), a.mkstr(path), 48, fx_slot(a, t, s), 1)
        return open(path, 'rb').read() if os.path.exists(path) else b''
    def sent(pos): return [l.split(' unit=')[1].split(' (')[0] for l in log_since(pos, A) + log_since(pos, B) if 'sent plug.state' in l]
    def checked(pos): return [l.split(' is ')[1].split(' (')[0] for l in log_since(pos, A) if 'PLUGFOCUS: window' in l and ' is ' in l]
    def title(k): w = ctypes.create_unicode_buffer(200); ctypes.windll.user32.GetWindowTextW(k, w, 200); return w.value
    time.sleep(5); pos = os.path.getsize(LOG)
    time.sleep(35)                                              # a sweep pass, the fake user busy in both
    check('drift: nothing sent with nobody in a plugin', lambda: sent(pos), lambda: [], wait=0)
    gate = next(((t, s) for t in range(mix_count(a)) for s in range(10) if has_plug(a, t, s) and b'kHs Gate' in state(t, s)), None)
    if not gate: print('   (no kHs Gate in the project: its window part skipped)'); return
    t, s = gate; slot = fx_slot(a, t, s); show = va(a, a.q(a.q(slot) + 0xE0))   # holder->vmt[0xE0](holder, show)
    a.call(show, slot, 1); time.sleep(1.5)
    win = [k for p, h, c, ti in fl.windows((A,)) for k, cc in [(h, c)] + fl.children(h)
           if cc == 'TPluginForm' and title(k).startswith('kHs Gate') and '(Insert %d)' % t in title(k)]
    if not win: print('   (kHs Gate\'s window not found: its window part skipped)'); return
    pos = os.path.getsize(LOG); fl.post_click(win[0]); time.sleep(6)
    check('drift: a click in its window checks it, sends nothing', lambda: (checked(pos), sent(pos)), lambda: (['fx %d/%d' % gate], []), wait=0)
    pos = os.path.getsize(LOG); time.sleep(12)
    check('drift: keys elsewhere afterwards don\'t', lambda: sent(pos), lambda: [], wait=0)
    a.call(show, slot, 0)

def routes(p, t):                                               # where track t sends (+0x394: {int level, byte on} per track)
    n = mix_count(p); r = p.r(mix_rec(p, t) + 0x394, n * 8)
    return [d for d in range(n) if r[d * 8 + 4]]
def all_routes(p): return [routes(p, t) for t in range(mix_count(p))]
def route_only(p, src, dst):                                    # src sends to dst only (SetRoute, no undo; RoutingChanged)
    m = p.q(p.g(0x15e22d8))
    for d in routes(p, src): p.call(0x12a33c0, m, src, d, 0, 0)
    p.call(0x12a33c0, m, src, dst, 1, 0); p.call(0x12a28f0, m)
def mix_state(p):
    n = mix_count(p)
    return n, [mix_name(p, t) for t in range(n)], [rec_get(p, mix_id(t, 0x1fc0)) for t in range(1, n - 1)], [has_plug(p, t) for t in range(1, n - 1)], all_routes(p)

def t_mix_insert():
    a.call(MIXNAME, 3, a.mkstr('Three'), 1); a.call(MIXNAME, 4, a.mkstr('Four'), 1)
    rec_set(a, mix_id(4, 0x1fc0), 4321)
    if os.path.exists(VST_FST): a.call(0xf54760, a.mkstr(VST_FST), 48, fx_slot(a, 4, 0), 0)
    check('mixer setup (names, volume, plugin)', lambda: mix_state(a), lambda: mix_state(b), wait=12)
    mix_insert(a, 3, 2)
    check('mixer: A inserts 2 tracks at 3', lambda: mix_state(a), lambda: mix_state(b), wait=8)

def t_mix_delete():
    mix_delete(b, [3, 4])
    check('mixer: B deletes 3 and 4', lambda: mix_state(a), lambda: mix_state(b), wait=8)

def t_mix_append():
    n = mix_count(a)
    mix_insert(a, n - 1, 3)                                     # before Current: "Increase number of tracks"
    check('mixer: A appends 3 tracks', lambda: mix_state(a), lambda: mix_state(b), wait=8)
    mix_delete(a, [n - 1, n, n + 1])
    check('mixer: A deletes them again', lambda: mix_state(a), lambda: mix_state(b), wait=8)

# Routing a channel to a new mixer track inserts it already named after the channel: the name was taken as
# known with the insert and never went out ("mixer names differ", a reload after each one).
def t_mix_named_insert():
    pos = 5
    mix_insert(a, pos); a.call(MIXNAME, pos, a.mkstr('Routed here'), 1)   # (in one go: before flim's scan sees the insert)
    check('mixer: a new track named at once: B has the name', lambda: mix_name(b, pos), lambda: 'Routed here', wait=8)
    check('mixer: names equal', lambda: [mix_name(a, t) for t in range(mix_count(a))], lambda: [mix_name(b, t) for t in range(mix_count(b))], wait=4)

def t_mix_move():
    mix_move(a, 3, 1)
    check('mixer: A moves track 3 right', lambda: mix_state(a), lambda: mix_state(b), wait=15)

# Moves travel as one reorder the others replay with FL's own move. Sent as each track's sends one by one,
# FL refused routes that closed a loop with the half-moved matrix, and 24+ changed rows weren't sent at all.
def t_mix_routes():
    mix_insert(a, 4)
    check('routes: insert', lambda: mix_count(a), lambda: mix_count(b), wait=8)
    route_only(a, 4, 3)
    check('routes: new track feeds 3', lambda: routes(a, 4), lambda: routes(b, 4), wait=6)
    mix_move(a, 4, -1)                                          # ...and moves left past it
    check('routes: moved left past the track it feeds', lambda: mix_state(a), lambda: mix_state(b), wait=10)
    while mix_count(a) < 28: mix_insert(a, mix_count(a) - 1, 28 - mix_count(a))
    check('routes: 28 tracks', lambda: mix_count(a), lambda: mix_count(b), wait=10)
    a.call(MIXNAME, 24, a.mkstr('Bus'), 1)
    for t in range(1, 23):                                      # 22 senders into the bus (a few per pass)
        route_only(a, t, 24)
        if t % 5 == 0: time.sleep(1)
    check('routes: 22 tracks into the bus', lambda: mix_state(a), lambda: mix_state(b), wait=10)
    mix_insert(a, 25); time.sleep(0.3); mix_move(a, 25, -1)     # a new track swapped with the bus
    check('routes: bus swapped with a new track', lambda: mix_state(a), lambda: mix_state(b), wait=12)
    check('routes: the senders still feed the bus', lambda: {tuple(routes(a, t)) for t in range(1, 23)}, lambda: {(25,)}, wait=2)
    for k in range(3): mix_move(a, 25, -1); time.sleep(0.15); mix_move(a, 24, -1); time.sleep(0.15)   # back and forth, fast
    check('routes: fast moves', lambda: mix_state(a), lambda: mix_state(b), wait=12)

# Solo is each person's own: FL solos by muting the other tracks, which must not reach the others.
MUTE, SOLO = 0x127b9b0, 0x127c2a0                               # setTrackEnabled(t, on); solo(t, 1 on / 0 off, 2: mixer default)
def mixb(p, t, off): return p.r(mix_rec(p, t) + off, 1)[0]     # +0x18 enabled, +0x1a soloed, +0x1c enabled before the solo
def mutes(p): return [mixb(p, t, 0x18) for t in range(mix_count(p))]

def t_solo():
    a.call(MUTE, 6, 0)                                          # (FL fades it in while its audio runs)
    check('mute 6 (shared)', lambda: (mixb(a, 6, 0x18), mixb(b, 6, 0x18)), lambda: (0, 0), wait=4)
    before = mutes(b)
    a.call(SOLO, 3, 1, 2); time.sleep(2.5)
    check('solo: A has 3 soloed', lambda: (mixb(a, 3, 0x1a), mixb(a, 4, 0x18)), lambda: (1, 0))
    check('solo: B keeps its mutes', lambda: mutes(b), lambda: before)
    b.call(MUTE, 5, 0); b.call(MUTE, 6, 1)                      # B mutes 5 and unmutes 6 meanwhile
    check('solo: A stays soloed, keeps B\'s mutes', lambda: (mixb(a, 3, 0x1a), mixb(a, 5, 0x1c), mixb(a, 6, 0x1c)), lambda: (1, 0, 1))
    a.call(SOLO, 3, 0, 2)
    check('unsolo: A has the shared mutes', lambda: mutes(a), lambda: mutes(b), wait=4)
    before = mutes(b)
    a.call(SOLO, 3, 1, 2); time.sleep(1.5); a.call(MUTE, 7, 0)  # a mute click ends FL's solo, the rest left muted
    time.sleep(2.5)
    check('mute ends solo: B unchanged', lambda: mutes(b), lambda: before)
    check('mute ends solo: A back to shared', lambda: mutes(a), lambda: mutes(b), wait=4)
    before = mutes(b)                                            # FL's solo click: the mute on mouse down, the solo ~0.2 s later
    a.call(MUTE, 8, 0); time.sleep(0.2); a.call(SOLO, 3, 1, 2); time.sleep(2.5)
    check('mute just before a solo: B unchanged', lambda: mutes(b), lambda: before)
    a.call(SOLO, 3, 0, 2)
    check('...unsolo: A back to shared', lambda: mutes(a), lambda: mutes(b), wait=4)

# The channel rack's solo is each person's own as well: FL's toggle turns the other channels off.
CHSOLO = 0xffaaa0                                               # toggle(channel, flags): solo / unsolo
def chon(p): return [rec_get(p, i << 16 | 7) for i in range(len(chans(p)))]   # REC_Chan_Mute: 1 on
def t_chan_solo():
    while len(chans(a)) < 4: add_named(a, len(chans(a)), 'S%d' % len(chans(a))); time.sleep(1.5)
    check('channel solo: channels on both', lambda: len(chans(b)), lambda: len(chans(a)), wait=10)
    for i, c in enumerate(chans(a)):                            # (FL's solo clicks each mute button, whose REC id the
        a.w(a.q(c + 0x7bc) + 24, struct.pack('<I', i << 16 | 7))   # channel rack gives it: 0 on channels made by ADDCH)
    rec_set(a, 3 << 16 | 7, 0)
    check('channel 3 off (shared)', lambda: (chon(a)[3], chon(b)[3]), lambda: (0, 0), wait=4)
    before = chon(b)
    a.call(CHSOLO, chans(a)[1], 0); time.sleep(2.5)
    check('channel solo: A has only 1 on', lambda: chon(a), lambda: [int(i == 1) for i in range(len(chans(a)))])
    check('channel solo: B keeps its on/off', lambda: chon(b), lambda: before, wait=3)
    rec_set(b, 2 << 16 | 7, 0); rec_set(b, 3 << 16 | 7, 1)       # B turns 2 off and 3 on meanwhile
    check('channel solo: A stays soloed', lambda: chon(a), lambda: [int(i == 1) for i in range(len(chans(a)))], wait=3)
    a.call(CHSOLO, chans(a)[1], 0)
    check('channel unsolo: A has the shared on/off', lambda: chon(a), lambda: chon(b), wait=4)
    check('...with B\'s changes', lambda: chon(a)[2:4], lambda: [0, 1], wait=0)
    before = chon(b)
    a.call(CHSOLO, chans(a)[1], 0); time.sleep(2); rec_set(a, 0 << 16 | 7, 1)   # a click turning another channel on ends it
    time.sleep(3)
    check('click ends channel solo: B unchanged', lambda: chon(b), lambda: before)
    check('click ends channel solo: A back to shared', lambda: chon(a), lambda: chon(b), wait=4)
    before = chon(b)                                            # the leader's project while A is soloed: no solo in it
    a.call(CHSOLO, chans(a)[1], 0); time.sleep(2.5)
    pos = os.path.getsize(LOG); engine_cmd(B, GUI_RESYNC)
    check('channel solo, B reloads: B loaded the project', lambda: any('PROJECT caught up' in l or 'PROJECT received' in l for l in log_since(pos, B)), lambda: True, wait=40)
    time.sleep(3)
    check('...with the shared on/off, not A\'s solo', lambda: chon(b), lambda: before, wait=5)
    a.call(CHSOLO, chans(a)[1], 0)
    check('...unsolo: the same on both', lambda: chon(a), lambda: chon(b), wait=5)

# Clips naming a channel this FL doesn't have yet (its chan.add still being made ready, e.g. its sample) were
# added at once, and FL crashed drawing the playlist. Now they wait for the channel.
def t_clip_hold():
    import base64
    ca = a.q(arr(a, 0) + 0x14); rec = a.alloc(0x58)
    a.call(va(a, a.q(a.q(ca) + 16)), ca, rec)                   # FL's default clip record
    n = len(chans(a))
    r = bytearray(a.r(rec, 0x58)); struct.pack_into('<IIIH', r, 0, 0, n << 16 | 0x5000, 384, 490)   # channel n: not there yet
    pos = os.path.getsize(LOG)
    x = RawWs('raw-clip-%d' % os.getpid(), 'RawClip'); x.pump(1)
    x.send({'t': 'op', 'op': {'kind': 'clips.set', 'key': 'clips:0:490', 'arr': 0, 'trk': 490, 'data': base64.b64encode(bytes(r)).decode()}})
    x.pump(2)
    check('clips for a missing channel: held on B', lambda: any('name a channel this FL doesn\'t have yet' in l for l in log_since(pos, B)), lambda: True, wait=4)
    check('...and B still runs', lambda: len(chans(b)), lambda: n, wait=0)
    add_named(a, n, 'Held')                                     # the channel arrives at B (chan.add from A)
    check('channel arrives: B applies the held clips', lambda: [c[:16] for c in clips(b, 0)[1] if c[12:14] == struct.pack('<H', 490)], lambda: [bytes(r[:16])], wait=10)
    x.send({'t': 'op', 'op': {'kind': 'clips.set', 'key': 'clips:0:490', 'arr': 0, 'trk': 490, 'data': ''}}); x.pump(2); x.close()
    check('...emptied again', lambda: [c for c in clips(b, 0)[1] if c[12:14] == struct.pack('<H', 490)], lambda: [], wait=5)

# Quick mute + unmute. While audio runs FL fades a mute out, and an unmute during the fade is lost: the
# two queued behind other work apply as one, and a track that ends up against flim's apply gets it again.
def t_mute_fast():
    import ctypes
    t, sleep = 4, ctypes.cast(ctypes.windll.kernel32.Sleep, ctypes.c_void_p).value
    pos = os.path.getsize(LOG)
    busy = threading.Thread(target=lambda: b.call(va(b, sleep), 2500)); busy.start()   # B's main thread busy
    time.sleep(0.3); a.call(MUTE, t, 0); time.sleep(0.5); a.call(MUTE, t, 1); busy.join()
    check('mute + unmute while B is busy', lambda: (mixb(a, t, 0x18), mixb(b, t, 0x18)), lambda: (1, 1), wait=4)
    check('...B applies only the last', lambda: sum('APPLY id=0x%08x' % (0xF0000000 | t) in l for l in log_since(pos, B)) <= 1, lambda: True)   # (none: the mute hadn't settled, MUTE_SETTLE_MS)
    a.call(MUTE, t, 0)
    check('mute (shared)', lambda: mixb(b, t, 0x18), lambda: 0, wait=4)
    pos = os.path.getsize(LOG); a.call(MUTE, t, 1); t0 = time.time()
    while mixb(b, t, 0x18) != 1 and time.time() - t0 < 4: pass
    b.w(mix_rec(b, t) + 0x18, b'\x00')                          # as if FL's fade had swallowed the unmute
    check('swallowed unmute applied again', lambda: (mixb(a, t, 0x18), mixb(b, t, 0x18)), lambda: (1, 1), wait=3)
    check('...and logged', lambda: any('MUTE track %d ended up muted' % t in l for l in log_since(pos, B)), lambda: True)
    time.sleep(1); b.call(MUTE, t, 0)                           # later, B's own mute is an edit as usual
    check('B mutes it afterwards', lambda: mixb(a, t, 0x18), lambda: 0, wait=4)
    b.call(MUTE, t, 1)
    check('B unmutes it', lambda: mixb(a, t, 0x18), lambda: 1, wait=4)

# Fruity Delay 3 locked its user out: the other FL sent its state back after applying their edits.
FD3_FST = os.path.join(fl_dir(), r'Data\Patches\Plugin database\Effects\Delay reverb\Fruity Delay 3.fst')
def fx_id(t, s, p): return 0x70000000 + (((t << 6) | s) << 16) + 0x8000 + p
def log_since(pos, pid):
    with open(LOG, 'rb') as f:
        f.seek(pos); return [l for l in f.read().decode('utf-8', 'replace').splitlines() if '[%d]' % pid in l]

def t_fd3_lock():
    t, s, p = 6, 1, 14
    a.call(V(a, 'LOADPRESET'), a.mkstr(FD3_FST), 48, fx_slot(a, t, s), 0)
    check('Fruity Delay 3 arrives', lambda: has_plug(a, t, s), lambda: has_plug(b, t, s), wait=10)
    time.sleep(5)
    pos = os.path.getsize(LOG)
    for k in range(15):                                         # A works a knob; B's user is busy elsewhere
        rec_set(a, fx_id(t, s, p), 1000 + 120 * k); time.sleep(0.7)
    time.sleep(8)                                               # B's state checks get round to it
    check('FD3: B has A\'s edits', lambda: rec_get(a, fx_id(t, s, p)), lambda: rec_get(b, fx_id(t, s, p)), wait=4)
    u = 'fx %d/%d' % (t, s)                                     # B must neither send its state nor put an old one back
    bad = [l[13:110] for l in log_since(pos, A) if 'snapped back' in l or 'PLUG applied unit=' + u in l]
    bad += [l[13:110] for l in log_since(pos, B) if u + ': reverting' in l or 'PLUG sent plug.state unit=' + u in l]
    check('FD3: B leaves A\'s plugin alone', lambda: bad[:2], lambda: [])

# Channels FL puts in the middle, quickly: Clone inserts its copy right after the source
# (AddChannel(source + 1, ...), then the source's state copied in), undo puts deleted ones back in place.
ADDCH, CHCOPY = 0x1004230, 0x1005590                            # AddChannel(pos or -1, select, group) -> channel; Clone's copy(src, dst, 0)
def add_named(p, pos, name):
    c = p.call(ADDCH, pos, 0, 0)
    p.call(va(p, p.q(p.q(c) + 0x70)), c, p.mkstr(name))
    return c

def t_chan_insert():
    add_named(a, 1, 'Ins1')
    check('channel inserted at 1', lambda: chan_names(a), lambda: chan_names(b), wait=8)
    for k, pos in enumerate((0, 3, -1)): add_named(a, pos, 'Burst%d' % k)    # faster than flim's 0.5 s scan
    check('3 channels inserted at once', lambda: chan_names(a), lambda: chan_names(b), wait=10)
    a.call(0x1004a60, 2, 0, 3); add_named(a, 1, 'AfterDel')                  # a delete and an insert together
    check('delete + insert at once', lambda: chan_names(a), lambda: chan_names(b), wait=10)
    last = len(chans(a)) - 1
    rec_set(a, last << 16, 4321); time.sleep(1)                              # something for the clone to copy: volume
    src = chans(a)[last]; c = a.call(ADDCH, 1, 0, 0); a.call(CHCOPY, src, c, 0)   # what Clone does
    check('clone into 1: names', lambda: chan_names(a), lambda: chan_names(b), wait=10)
    check('clone into 1: its volume', lambda: rec_get(a, 1 << 16), lambda: rec_get(b, 1 << 16), wait=10)

def chan_state_bytes(p, i, tag):
    path = os.path.join(TMP, 'flim_live_chstate_%s.fst' % tag)
    p.call(V(p, 'SAVEPRESET'), p.mkstr(path), 32, chans(p)[i], 1)
    return open(path, 'rb').read()

# The shared folder: the same files everywhere, newest wins (the replaced one is backed up), programs and
# flim's partial downloads stay put, and a sample loaded from it keeps its path on the other side. The two
# FLs on this PC each have their own (restart_test.ps1: FLIM_SHARED_DIR).
SH_A, SH_B = (os.path.join(TMP, 'flim_shared_' + x) for x in 'AB')
def rd(p): return open(p, 'rb').read() if os.path.exists(p) else None
def engine_cmd(pid, kind, text=None):                           # what flim.exe's window sends the engine (GUI_*)
    import ctypes, ctypes.wintypes as W
    u32 = ctypes.windll.user32
    u32.FindWindowExW.restype = W.HWND
    u32.FindWindowExW.argtypes = [W.HWND, W.HWND, W.LPCWSTR, W.LPCWSTR]
    u32.SendMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
    class CDS(ctypes.Structure): _fields_ = [('dwData', ctypes.c_size_t), ('cbData', W.DWORD), ('lpData', ctypes.c_void_p)]
    h = None
    while True:
        h = u32.FindWindowExW(W.HWND(-3), h, 'FlimTasks', None)   # HWND_MESSAGE
        if not h: return False
        p = W.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid: break
    buf = ctypes.create_string_buffer(text.encode('utf-8') + b'\0') if text is not None else None
    cds = CDS(kind, len(buf) if buf else 0, ctypes.cast(buf, ctypes.c_void_p).value if buf else None)
    u32.SendMessageW(h, 0x4A, 0, ctypes.addressof(cds))
    return True
def t_shared():
    import glob, shutil
    tag = '%d' % (time.time() % 100000)
    fa = os.path.join(SH_A, 'Drums ' + tag, 'kick %s.wav' % tag)
    rel = os.path.relpath(fa, SH_A)
    fb = os.path.join(SH_B, rel)
    os.makedirs(os.path.dirname(fa), exist_ok=True)
    make_wav(fa, 200 + int(tag) % 100)
    for junk in ('tool %s.exe' % tag, '~flim-%s.part' % tag, 'pack %s.zip.crdownload' % tag): open(os.path.join(SH_A, junk), 'wb').write(b'MZ junk')
    check('shared: A\'s new file reaches B', lambda: rd(fa), lambda: rd(fb), wait=20)
    old = rd(fa)
    time.sleep(1.5)
    make_wav(fb, 500 + int(tag) % 100)                          # B edits it later: B's is the next version
    check('shared: B\'s newer version reaches A', lambda: rd(fa), lambda: rd(fb), wait=20)
    bak = glob.glob(os.path.join(os.path.dirname(SH_A), 'Shared backups', '*', rel))
    check('shared: A\'s old one is in Shared backups', lambda: any(rd(x) == old for x in bak), lambda: True)
    check('shared: programs and unfinished files stay put', lambda: [os.path.exists(os.path.join(SH_B, j)) for j in ('tool %s.exe' % tag, '~flim-%s.part' % tag, 'pack %s.zip.crdownload' % tag)], lambda: [False] * 3)
    fb2 = os.path.join(SH_B, 'pad %s.wav' % tag)
    make_wav(fb2, 330)
    check('shared: B\'s new file reaches A', lambda: rd(os.path.join(SH_A, 'pad %s.wav' % tag)), lambda: rd(fb2), wait=20)
    src = os.path.join(TMP, 'flim_live_old_pad.wav')            # copied over it: a 2019 file (a copy keeps its date)
    make_wav(src, 777); os.utime(src, (1560000000, 1560000000))
    time.sleep(1)
    shutil.copy2(src, os.path.join(SH_A, 'pad %s.wav' % tag))
    check('shared: an old-dated copy over it still wins', lambda: rd(fb2), lambda: rd(src), wait=20)
    os.remove(fa)                                               # deleted at A: B still has it and lists it again
    time.sleep(5)
    engine_cmd(B, 4); time.sleep(2); engine_cmd(B, 3, URL + '\n\nB')   # B reconnects (Disconnect, Connect)
    time.sleep(25)
    check('shared: a delete sticks when B lists its files again', lambda: (os.path.exists(fa), os.path.exists(fb)), lambda: (False, True))
    # a channel playing a file at B's shared path (between real PCs it's the same path for everyone; here only
    # B takes it for its shared folder): B leaves it as it is instead of pointing it at a downloaded copy
    pad = os.path.join(SH_B, 'pad %s.wav' % tag)
    src, dst = (os.path.join(TMP, n) for n in ('flim_live_ch0.fst', 'flim_live_shsmp.fst'))
    a.call(0xf54570, a.mkstr(src), 32, chans(a)[0], 1)
    fst_with_sample(src, dst, pad)
    a.call(0x1004230, -1, 0, 1)
    a.call(0xf54760, a.mkstr(dst), 32, chans(a)[-1], 0)
    check('shared: channel with a shared sample added', lambda: len(chans(a)), lambda: len(chans(b)), wait=10)
    check('shared: B\'s channel keeps the shared path', lambda: pad.encode('utf-16le') in chan_state_bytes(b, -1, 'b'), lambda: True, wait=10)

def t_chan_race():
    for p, nm in ((a, 'RaceA'), (b, 'RaceB')): add_named(p, 1, nm)            # both at once: one side re-syncs
    check('both insert at once: same list after', lambda: chan_names(a), lambda: chan_names(b), wait=30)

# B plays while A's notes land in patterns B has no note array for yet. FL makes one then under its song
# lock, which its audio thread holds while it waits for the note lock: flim holding the note lock around
# that deadlocked FL (hang report 2026-10-21 175345).
PLAYING = 0x193fb0c                                             # int: FL is playing
def space(pid):                                                 # FL's play / stop key
    import ctypes
    for w in fl.windows((pid,)):
        if w[2] == 'TFruityLoopsMainForm':
            ctypes.windll.user32.PostMessageW(w[1], 0x100, 0x20, 0x00390001); ctypes.windll.user32.PostMessageW(w[1], 0x101, 0x20, 0xC0390001)
def responds(pid, ms=3000):                                     # FL's main thread answers a message
    import ctypes, ctypes.wintypes as W
    r = ctypes.c_size_t()
    for w in fl.windows((pid,)):
        if w[2] == 'TFruityLoopsMainForm':
            return bool(ctypes.windll.user32.SendMessageTimeoutW(w[1], 0, 0, 0, 2, ms, ctypes.byref(r)))   # WM_NULL, SMTO_ABORTIFHUNG
    return False
def unsel(recs): return [r[:0x13] + bytes([r[0x13] & 0x7f]) + r[0x14:] for r in recs]   # selection is each person's own
PATLENUPD = 0x12d17a0                                           # UpdatePatternLength(p, 1): FL's own after a note edit
# A pattern's length is cached in its record (+0x48...) and sizes its playlist clips. Notes applied from
# someone else left the receiver's cache stale, and it cut the writer's clips back to its old length.
def t_pattern_length_notes():
    sz, rs = clips(a)
    pats = sorted({struct.unpack_from('<I', r, 4)[0] >> 16 for r in rs if struct.unpack_from('<I', r, 4)[0] >> 16 >= 0x5000})
    src = next(iter(notes(a, 1)), None)
    if not pats or src is None: print('   (no pattern clips / notes: skipped)'); return
    p = pats[0] - 0x5000
    def plen(x): return struct.unpack_from('<iii', x.r(pat_rec(x, p) + 0x48, 12))
    def plens(x):
        return sorted(struct.unpack_from('<I', r, 8)[0] for r in clips(x)[1] if struct.unpack_from('<I', r, 4)[0] >> 16 == 0x5000 + p)
    r = bytearray(src); struct.pack_into('<I', r, 0, plen(a)[0] + 2 * 384); struct.pack_into('<H', r, 6, 0)
    arr = a.call(0x12d16e0, p, 0); rec = a.alloc(24); a.w(rec, bytes(r)); a.call(va(a, a.q(a.q(arr) + 8)), arr, rec)
    a.call(PATLENUPD, p, 1)
    check('pattern longer by notes: B\'s length', lambda: plen(a), lambda: plen(b), wait=8)
    check('pattern longer by notes: its clips', lambda: plens(a), lambda: plens(b), wait=8)

def t_notes_playing():
    src = next(iter(notes(a, 1)), None)
    if src is None: print('   (no notes in pattern 1: skipped)'); return
    fresh = [p for p in range(100, 999) if not a.q(pat_rec(a, p) + 0x28) and not b.q(pat_rec(b, p) + 0x28)][:20]
    if not b.d(b.a(PLAYING)): space(B)
    time.sleep(1)
    rec = a.alloc(24); a.w(rec, src)
    for p in fresh:
        arr = a.call(0x12d16e0, p, 0)                           # NoteArray(p, 0): A isn't playing
        a.call(va(a, a.q(a.q(arr) + 8)), arr, rec)              # vmt[1]: add the note
        time.sleep(0.15)
    check('notes into new patterns, B playing: B responds', lambda: responds(B), lambda: True, wait=15)
    check('notes into new patterns, B playing', lambda: [unsel(notes(a, p)) for p in fresh], lambda: [unsel(notes(b, p)) for p in fresh], wait=15)
    if responds(B, 1000) and b.d(b.a(PLAYING)): space(B)

# Automation clips: FL keeps their links in one list (*LINKS -> [MIDI, internal]) of 28-byte records,
# +8 source (the clip's channel << 16), +0x10 target. They travel with the clip's state.
LINKS = 0x15e0a00
def links(p):                                                   # (source, target, flags, smoothing), in FL's order
    L = p.q(p.g(LINKS) + 8); n = p.d(L + 16); it = p.q(L + 8)
    return [struct.unpack('<IIIIi', p.r(p.q(it + 8 * i) + 8, 20)) for i in range(n)]
def t_auto_clip():
    target = mix_id(5, 0x1fc0)                                  # mixer track 5's volume
    n = len(chans(a))
    a.call(0x1191a50, target, 1, 0, 1, 1)                       # FL's "Create automation clip"
    check('automation clip: channel added', lambda: len(chans(b)), lambda: n + 1, wait=10)
    check('automation clip: its link reached B', lambda: links(a), lambda: links(b), wait=10)
    before = rec_get(b, target)
    quiet['on'] = True; time.sleep(2)                           # (moved by FL, nobody at the keyboard: by hand it goes out, auto_hand)
    rec_set(a, target, (rec_get(a, target) + 2345) % 12800); time.sleep(2.5)
    quiet['on'] = False
    check('automated value stays each FL\'s own', lambda: rec_get(b, target), lambda: before)
def t_auto_add():                                               # another link out of a clip in the middle: B's loader
    srcs = sorted(set(l[0] for l in links(a)))                  # appends it (unsorted), flim sorts
    src = srcs[len(srcs) // 2]
    r = a.call(0xf82530, 8)                                     # FL's new link record (8: internal)
    a.w(r + 8, struct.pack('<IIIIi', src, 0, mix_id(6, 0x1fc0), 8, 469))
    L = a.q(a.g(LINKS) + 8); n = a.d(L + 16)
    if a.d(L + 20) <= n: print('   (link list full: skipped)'); return
    a.w(a.q(L + 8) + 8 * n, struct.pack('<Q', r)); a.w(L + 16, struct.pack('<i', n + 1))
    a.call(0xf825b0, L)                                         # FL's sort, as its own "add target link" does
    check('automation link added: reached B', lambda: links(a), lambda: links(b), wait=10)
def t_auto_remove():
    srcs = sorted(set(l[0] for l in links(a)))
    if not srcs: print('   (no automation links: skipped)'); return
    src = srcs[min(1, len(srcs) - 1)]
    a.call(0xf832b0, src, src | 0xffff)                         # FL's delete of a source's links
    check('automation links removed: gone on B too', lambda: links(a), lambda: links(b), wait=10)
def t_auto_both():                                              # both make a clip at the same moment: the two chan.adds
    n = len(chans(a))                                           # cross; the leader keeps B's after its own (it was lost)
    go = threading.Event()
    def mk(p, t): go.wait(); p.call(0x1191a50, t, 1, 0, 1, 1)
    th = [threading.Thread(target=mk, args=(a, mix_id(7, 0x1fc0))), threading.Thread(target=mk, args=(b, mix_id(8, 0x1fc0)))]
    for t in th: t.start()
    go.set()
    for t in th: t.join()
    check('two clips at once: both kept', lambda: (len(chans(a)), len(chans(b))), lambda: (n + 2, n + 2), wait=20)
    check('two clips at once: same channels', lambda: chan_names(a), lambda: chan_names(b), wait=10)
    check('two clips at once: both linked', lambda: links(a), lambda: links(b), wait=10)
    check('two clips at once: B\'s clip has its link', lambda: sorted(set(l[0] >> 16 for l in links(a)) & {n, n + 1}), lambda: [n, n + 1], wait=0)

# An automated knob moved by hand while the song is stopped goes out like any other; while an FL plays, its
# own automation drives it (nothing sent from there, nothing applied there).
def t_auto_hand():
    v3, v4 = mix_id(7, 0x1fc0), mix_id(8, 0x1fc0)
    for t in (v3, v4):
        if t not in [l[2] for l in links(a)]: a.call(0x1191a50, t, 1, 0, 1, 1); time.sleep(2)
    check('automated: links on both', lambda: links(a), lambda: links(b), wait=10)
    time.sleep(2)                                               # (a busy FL holds the background user's keys: no input meanwhile)
    rec_set(a, v3, 5000)
    check('automated, stopped: A moves it, B has it', lambda: (rec_get(a, v3), rec_get(b, v3)), lambda: (5000, 5000), wait=5)
    rec_set(b, v4, 7000)
    check('automated, stopped: B moves it, A has it', lambda: (rec_get(a, v4), rec_get(b, v4)), lambda: (7000, 7000), wait=5)
    space(A); time.sleep(1.5)
    check('automated: A plays', lambda: a.d(a.a(PLAYING)), lambda: 1, wait=3)
    rec_set(a, v3, 9000); time.sleep(4)
    check('automated, A playing: not sent from A', lambda: rec_get(b, v3), lambda: 5000, wait=0)
    rec_set(b, v4, 3000); time.sleep(4)
    check('automated, A playing: not applied in A', lambda: rec_get(a, v4) != 3000, lambda: True, wait=0)
    space(A); time.sleep(2)
    check('automated: A stopped', lambda: a.d(a.a(PLAYING)), lambda: 0, wait=3)
    rec_set(a, v3, 2000)
    check('automated, stopped again: A\'s move reaches B', lambda: (rec_get(a, v3), rec_get(b, v3)), lambda: (2000, 2000), wait=5)

# FL's "Move down" on a track's first effect swaps two slot objects inside the track (sub_1233B30); flim
# took slot 0's object for the track and replayed it as a track deleted and inserted.
CURSLOT = 0x15e2c38                                             # global -> the effect slot whose menu is open
def slot_plugin(p, t, s, tag):                                  # the plugin's name in an effect slot ('' = empty)
    import flp
    if not has_plug(p, t, s): return ''
    path = os.path.join(TMP, 'flim_live_slot_%s.fst' % tag)
    if os.path.exists(path): os.remove(path)
    p.call(V(p, 'SAVEPRESET'), p.mkstr(path), 48, fx_slot(p, t, s), 1)
    for e, v in flp.events(open(path, 'rb').read()):
        if e in (201, 203): return v.decode('utf-16le', 'replace').rstrip('\0')
    return '?'
# Arrangements (the playlist's Arrangements menu): add / rename / delete from either side reach the other, each person
# stays on their own arrangement (also across a reload of the leader's project), a clone's content comes along, and a
# clip edit right before switching arrangement still goes out.
def arr_count(p): return p.q(p.g(V(p, 'ARRS')) - 8)
def arr_names(p): return [ustr(p, p.q(arr(p, i) + 8)) for i in range(arr_count(p))]
def arr_cur(p): return p.d(p.a(V(p, 'ARRCUR')))
def t_arrangements():
    if arr_count(a) < 2: a.call(V(a, 'ADDARR'), 0, 0)
    check('arrangements: two on both', lambda: arr_names(a), lambda: arr_names(b), wait=8)
    a.call(V(a, 'SETARR'), 1); b.call(V(b, 'SETARR'), 0); time.sleep(1)
    bobj = arr(b, 0)
    a.call(V(a, 'INSARR'), 1)
    check('arrangement inserted by A: same list on B', lambda: arr_names(a), lambda: arr_names(b), wait=8)
    check('...B still on its own arrangement', lambda: arr(b, arr_cur(b)), lambda: bobj, wait=0)
    a.call(V(a, 'USTRASG'), arr(a, 1) + 8, a.mkstr('Renamed by A'))
    check('arrangement renamed by A', lambda: arr_names(b)[1], lambda: 'Renamed by A', wait=8)
    n = arr_count(b); b.call(V(b, 'ADDARR'), 0, 0)
    check('arrangement added by B: A has it', lambda: arr_names(a), lambda: arr_names(b), wait=8)
    check('...one more', lambda: arr_count(a), lambda: n + 1, wait=0)
    a.call(V(a, 'DELARR'), 1, 1, 0)
    check('arrangement deleted by A: same list on B', lambda: arr_names(a), lambda: arr_names(b), wait=8)
    check('...B still on its own arrangement', lambda: arr(b, arr_cur(b)), lambda: bobj, wait=0)
    # a clone: inserted at 1 and filled with clips inside one main-thread call
    src = [r for r in clips(a, 0)[1]][:2] or []
    if src:
        import ctypes
        src = [r[:0x20] + b'\0' * 4 + r[0x24:] for r in src]     # (no clip id: FL's ids are unique, a copy with one moved the original)
        buf = a.alloc(0x58 * len(src)); a.w(buf, b''.join(src))
        code = b'\x48\x83\xec\x28\xb9\x01\x00\x00\x00\x48\xb8' + struct.pack('<Q', a.a(V(a, 'INSARR'))) + b'\xff\xd0'
        for i in range(len(src)):
            code += b'\x48\xb8' + struct.pack('<Q', a.a(V(a, 'ARRS'))) + b'\x48\x8b\x00\x48\x8b\x48\x08\x48\x8b\x49\x14\x48\x8b\x01'
            code += b'\x48\xba' + struct.pack('<Q', buf + i * 0x58) + b'\xff\x50\x08'
        code += b'\x48\x83\xc4\x28\xc3'
        k = ctypes.windll.kernel32
        k.OpenProcess.restype = ctypes.c_void_p; k.VirtualAllocEx.restype = ctypes.c_void_p
        k.VirtualAllocEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_ulong, ctypes.c_ulong]
        k.WriteProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p]
        h = k.OpenProcess(0x1F0FFF, False, A); mem = k.VirtualAllocEx(h, None, len(code), 0x3000, 0x40)
        k.WriteProcessMemory(h, mem, code, len(code), None); k.CloseHandle(ctypes.c_void_p(h))
        a.call(va(a, mem))
        check('arrangement cloned by A: its clips on B', lambda: sorted(r[:0x20] for r in clips(b, 1)[1]), lambda: sorted(r[:0x20] for r in clips(a, 1)[1]), wait=8)
    # a clip edit, then a switch away at once
    c0 = arr_cur(a); ca = a.q(arr(a, c0) + 0x14); nn = a.d(ca + 0x14)
    if nn:
        rec = a.q(ca + 8) + (nn - 1) * 0x58; a.w(rec, struct.pack('<I', a.d(rec) + 192)); a.call(0xe39560, a.q(a.g(PLFORM)))
        trk = a.r(rec + 12, 2)                                  # (its track only: the others' lengths can differ a while, FL2026-internals.md)
        a.call(V(a, 'SETARR'), 0 if c0 else 1)
        check('clip edit then a switch: it went out', lambda: sorted(r[:0x20] for r in clips(b, c0)[1] if r[12:14] == trk), lambda: sorted(r[:0x20] for r in clips(a, c0)[1] if r[12:14] == trk), wait=8)
    # B reloads the leader's project: still on its own arrangement and pattern
    b.call(V(b, 'SETARR'), 0); b.call(V(b, 'PATSELECT'), 3); a.call(V(a, 'SETARR'), 1); a.call(V(a, 'PATSELECT'), 1); time.sleep(1)
    pos = os.path.getsize(LOG)
    call_many(b, B, 0x1004a60, [(i, 0, 3) for i in range(len(chans(b)) - 1, 0, -1)]) if len(chans(b)) > 4 else None
    if len(chans(a)) > 4:
        check('reload: B loaded the project', lambda: any('PROJECT caught up' in l for l in log_since(pos, B)), lambda: True, wait=40)
        check("reload: B kept its own arrangement and pattern", lambda: (arr_cur(b), b.d(b.g(V(b, 'CURPAT')))), lambda: (0, 3), wait=3)
    a.call(V(a, 'SETARR'), 0); b.call(V(b, 'SETARR'), 0); a.call(V(a, 'PATSELECT'), 1); b.call(V(b, 'PATSELECT'), 1)   # (later tests compare the current one)

# A knob turned on a MIDI controller is someone using FL: FL's MIDI input callback (sub_BA2180) is fed a control
# change (a zeroed device record: +189 set skips its PostMessage, +128 not FL's clock device) with nobody at the keyboard.
MIDIINCB = 0xba2180
def t_midi_input():
    dev = a.alloc(512); a.w(dev + 128, struct.pack('<I', 0xffffffff)); a.w(dev + 189, b'\x01')
    t = mix_id(9, 0x1fc0)
    quiet['on'] = True; time.sleep(2)                           # (input_recent: 1.5 s)
    rec_set(a, t, 4321); time.sleep(3)
    check('no input: a moved knob stays FL\'s own', lambda: rec_get(b, t) != 4321, lambda: True, wait=0)
    a.call(MIDIINCB, 0, 0x3C3, dev, 0x7F07B0, 0)                # MIM_DATA: CC 7 = 127 on channel 1
    rec_set(a, t, 6543)
    check('MIDI controller: the knob it moved syncs', lambda: rec_get(b, t), lambda: 6543, wait=5)
    time.sleep(2)
    a.call(MIDIINCB, 0, 0x3C3, dev, 0x7F3C90, 0)                # a note (on) isn't a controller
    rec_set(a, t, 2222); time.sleep(3)
    check('MIDI note only: stays FL\'s own', lambda: rec_get(b, t) != 2222, lambda: True, wait=0)
    quiet['on'] = False

# An MP3 / FLAC / OGG sample gets a silent placeholder of its length, as WAV / AIFF do (FL's loader goes by the
# content, so a WAV under its name loads). Needs B to download: restart_test.ps1 -NoLocalB. A Sampler's frame
# count is at *(ch + 0x330) + 0x94.
def smp_frames(p, c): s = p.q(c + 0x330); return p.d(s + 0x94) if s else None
def t_sample_compressed():
    import subprocess
    if not os.environ.get('ISO_NOLOCAL_B') and not os.environ.get('FLIM_TEST_NOLOCAL_B'):
        print('   (B uses local copies: sample_compressed needs -NoLocalB, set FLIM_TEST_NOLOCAL_B=1: skipped)'); return
    for ext in ('mp3', 'flac', 'ogg'):
        f = os.path.join(TMP, 'flim live %s %d.%s' % (ext, os.getpid(), ext))
        hz = 300 + int(time.time() * 7) % 400                   # new content each run: a real upload
        subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-f', 'lavfi', '-i', 'sine=f=%d:d=2.5:sample_rate=44100' % hz, '-ac', '2', f], check=True)
        src, dst = os.path.join(TMP, 'flim_live_cmp0.fst'), os.path.join(TMP, 'flim_live_cmp.fst')
        a.call(0xf54570, a.mkstr(src), 32, chans(a)[0], 1)
        fst_with_sample(src, dst, f)
        pos = os.path.getsize(LOG); n = len(chans(a))
        a.call(0x1004230, -1, 0, 1); a.call(0xf54760, a.mkstr(dst), 32, chans(a)[-1], 0)
        check('%s sample: channel added on B' % ext, lambda: len(chans(b)), lambda: n + 1, wait=15)
        check('...with a placeholder (not waited for)', lambda: any(re.search(r'SAMPLES op lists \d+ samples: [1-9]\d* placeholders', l) for l in log_since(pos, B)), lambda: True, wait=5)
        def secs(p):                                            # (each FL resamples to its own rate: 44.1 or 48 kHz)
            fr = smp_frames(p, chans(p)[n]) or 0
            return min((abs(fr / r - 2.5), round(fr / r, 1)) for r in (44100, 48000))[1]
        check('...and the real sound (2.5 s, not the placeholder)', lambda: secs(b), lambda: secs(a), wait=20)

# What someone has selected reaches the others (drawn as outlines on their editors: not checked here, the test FLs
# run minimized): A selects notes and clips, B's log says how many.
def t_sel():
    p = a.d(a.g(V(a, 'CURPAT'))); pa = a.call(0x12d16e0, p, 0); n = a.d(pa + 0x14); d = a.q(pa + 8)
    if n < 2:
        rec = a.alloc(24)
        for k in range(3): a.w(rec, struct.pack('<IHHIH', k * 96, 0, 0, 48, 60 + k) + bytes(10)); a.call(va(a, a.q(a.q(pa) + 8)), pa, rec)
        time.sleep(1); n = a.d(pa + 0x14); d = a.q(pa + 8)
    pos = os.path.getsize(LOG)
    for k in range(2): a.w(d + k * 24 + 0x13, bytes([a.r(d + k * 24 + 0x13, 1)[0] | 0x80]))
    check('selection: B hears of 2 notes', lambda: any(re.search(r'SEL \S+: 2 notes selected in pattern %d' % p, l) for l in log_since(pos, B)), lambda: True, wait=4)
    for k in range(n): a.w(d + k * 24 + 0x13, bytes([a.r(d + k * 24 + 0x13, 1)[0] & 0x7f]))
    check('...and of none again', lambda: any(re.search(r'SEL \S+: 0 notes selected', l) for l in log_since(pos, B)), lambda: True, wait=4)
    ca = a.q(arr(a) + 0x14); nc = a.d(ca + 0x14); cd = a.q(ca + 8)
    if nc:
        pos = os.path.getsize(LOG)
        a.w(cd + 0x13, bytes([a.r(cd + 0x13, 1)[0] | 0x80]))
        check('selection: B hears of 1 clip', lambda: any(re.search(r'SEL \S+: 0 notes selected in pattern \d+, 1 clips', l) for l in log_since(pos, B)), lambda: True, wait=4)
        a.w(cd + 0x13, bytes([a.r(cd + 0x13, 1)[0] & 0x7f]))
    check('...no edits went out for it', lambda: [l for l in log_since(pos, A) if 'NOTES sent' in l or 'CLIPS sent' in l], lambda: [], wait=1)

def t_arr_many():                                               # playlist tracks of arrangements past the 8th (META_ARR_MAX)
    n0 = arr_count(a)
    while arr_count(a) < 11: a.call(V(a, 'ADDARR'), 0, 0); time.sleep(0.6)
    check('11 arrangements on both', lambda: arr_count(b), lambda: arr_count(a), wait=10)
    a.call(0x12e60f0, arr(a, 10), 3, a.mkstr('Arr11Track'), 0x0000ff00)
    check('arrangement 11: playlist track name', lambda: ustr(b, b.q(arr(b, 10) + 3 * 0x114 + 0x24)), lambda: 'Arr11Track', wait=8)
    while arr_count(a) > n0: a.call(V(a, 'DELARR'), arr_count(a) - 1, 1, 0); time.sleep(0.6)
    check('...back to %d on both' % n0, lambda: arr_count(b), lambda: n0, wait=10)

def t_slot_move():
    t = 4
    if not (has_plug(a, t, 0) and has_plug(a, t, 1)): print('   (track %d needs effects in slots 0 and 1: skipped)' % t); return
    pos = os.path.getsize(LOG)
    a.w(a.g(CURSLOT), struct.pack('<Q', fx_slot(a, t, 0)))
    a.call(0x1233b30, 1, 0)                                     # move it to slot 1, no undo step
    check('effect moved down in A', lambda: [slot_plugin(a, t, s, 'a') for s in range(3)], lambda: [slot_plugin(b, t, s, 'b') for s in (1, 0, 2)], wait=0)
    check('effect move: B\'s slots', lambda: [slot_plugin(a, t, s, 'a') for s in range(3)], lambda: [slot_plugin(b, t, s, 'b') for s in range(3)], wait=15)
    check('effect move: not a track delete', lambda: [l for l in log_since(pos, A) if 'MIX tracks changed' in l], lambda: [])
    check('effect move: links follow', lambda: links(a), lambda: links(b), wait=10)

# Quick changes: bursts of everyday edits as fast as a person clicks, first from A alone, then from both at
# once; afterwards everything the two FLs share must be the same. FLIM_STRESS_SEED / _OPS vary it.
def shared_state(p):
    n = mix_count(p)
    return {'channels': chan_names(p), 'mixer names': [mix_name(p, t) for t in range(n)], 'mutes': mutes(p),
            'routes': all_routes(p), 'mixer volumes': [rec_get(p, mix_id(t, 0x1fc0)) for t in range(1, n - 1)],
            'effects': [[slot_plugin(p, t, s, 'st') for s in range(3)] for t in range(1, n - 1)],
            'links': links(p), 'notes': [unsel(notes(p, pt)) for pt in range(1, 12)], 'clips': sorted(r[:16] for r in clips(p)[1])}
def t_stress():
    import random
    rnd = random.Random(int(os.environ.get('FLIM_STRESS_SEED', '7')))
    src = next(iter(notes(a, 1)), bytes(24))
    def inner(p): return rnd.randrange(1, mix_count(p) - 1)
    def rename_ch(p, k): c = rnd.choice(chans(p)); p.call(va(p, p.q(p.q(c) + 0x70)), c, p.mkstr('S%d' % k))
    def add_ch(p, k): add_named(p, rnd.randrange(len(chans(p)) + 1), 'Add%d' % k)
    def del_ch(p, k): p.call(0x1004a60, rnd.randrange(len(chans(p))), 0, 3)
    def mute(p, k): p.call(MUTE, inner(p), rnd.randrange(2))
    def mixname(p, k): p.call(MIXNAME, inner(p), p.mkstr('M%d' % k), 1)
    def vol(p, k): rec_set(p, mix_id(inner(p), 0x1fc0), rnd.randrange(12800))
    def route(p, k): s = inner(p); route_only(p, s, rnd.choice([0] + [d for d in range(1, mix_count(p) - 1) if d != s]))
    def mixins(p, k): mix_insert(p, inner(p))
    def mixmove(p, k): mix_move(p, rnd.randrange(2, mix_count(p) - 2), rnd.choice((-1, 1)))
    def fxmove(p, k):
        full = [t for t in range(1, mix_count(p) - 1) if has_plug(p, t, 0)]
        if not full: return
        t = rnd.choice(full); p.w(p.g(CURSLOT), struct.pack('<Q', fx_slot(p, t, 0))); p.call(0x1233b30, 1, 0)
    def note(p, k):
        r = bytearray(src); struct.pack_into('<I', r, 0, rnd.randrange(64) * 24); struct.pack_into('<H', r, 6, rnd.randrange(len(chans(p))))
        pat = rnd.randrange(1, 12)
        arr = p.call(0x12d16e0, pat, 0); rec = p.alloc(24); p.w(rec, bytes(r)); p.call(va(p, p.q(p.q(arr) + 8)), arr, rec)
        p.call(PATLENUPD, pat, 1)                               # as FL's own note editing: the pattern's length again
    def autoclip(p, k): p.call(0x1191a50, mix_id(inner(p), 0x1fc0), 1, 0, 1, 1)
    ops = [rename_ch, add_ch, del_ch, mute, mixname, vol, route, mixins, mixmove, fxmove, note, note, vol, mute, autoclip]
    n = int(os.environ.get('FLIM_STRESS_OPS', '40'))
    for phase, who in (('A alone', (a,)), ('both', (a, b))):
        for k in range(n):
            p = rnd.choice(who); op = rnd.choice(ops)
            try: op(p, k)
            except Exception as e: print('   %s on %s: %s' % (op.__name__, 'A' if p is a else 'B', e))
            time.sleep(rnd.choice((0.03, 0.1, 0.25, 0.5)))
        time.sleep(5)
        try:                                                    # what differs before any re-sync repairs it
            sa, sb = shared_state(a), shared_state(b)
            for key in sa:
                x, y = sa[key], sb[key]
                if x == y: continue
                d = [(i, x[i], y[i]) for i in range(len(x)) if x[i] != y[i]][:6] if len(x) == len(y) else (x, y)
                print('   (%s: %s differ at first: %r)' % (phase, key, d))
        except RuntimeError: pass
        sa = sb = None
        t0 = time.time()
        while time.time() - t0 < 90:                            # a re-sync (the leader's project again) takes a while
            try: sa, sb = shared_state(a), shared_state(b)
            except RuntimeError as e: print('   (an FL was busy: %s; again)' % e); time.sleep(3); continue
            if sa == sb: break
            time.sleep(3)
        for key in sa: check('stress (%s): %s' % (phase, key), lambda key=key: sa[key], lambda key=key: sb[key], wait=0)

# A and B at the same moment (a thread each): structure ops cross and edits travel by old numbers (the
# structure count, FL2026-internals.md). Converging may take one reload of the leader's project.
def t_stress_both():
    import random
    src = next(iter(notes(a, 1)), bytes(24))
    def answerer(pid, secs=4):                               # split's message boxes (one channel: "makes no sense";
        def run():                                           # else its question): their first button
            import ctypes
            for _ in range(secs * 4):
                time.sleep(0.25)
                for q, h, c, t in fl.windows((pid,)):
                    if c == 'TNameEditForm': ctypes.windll.user32.PostMessageW(h, 0x10, 0, 0)
                    if c == 'TMsgForm':
                        btns = [x for x, cc in fl.children(h) if cc == 'TQuickFocusBtn']
                        if btns: fl.post_click(btns[0])
        threading.Thread(target=run, daemon=True).start()
    def worker(p, odd, seed):
        rnd = random.Random(seed)
        pid = B if odd else A
        def one(v):
            buf = p.alloc(8); p.w(buf, struct.pack('<i', v)); return buf
        def inner(): return rnd.randrange(1, mix_count(p) - 1)
        def note():
            r = bytearray(src); struct.pack_into('<I', r, 0, rnd.randrange(64) * 24); struct.pack_into('<H', r, 6, rnd.randrange(len(chans(p))))
            pat = rnd.randrange(1, 12)
            arr = p.call(0x12d16e0, pat, 0); rec = p.alloc(24); p.w(rec, bytes(r)); p.call(va(p, p.q(p.q(arr) + 8)), arr, rec)
            p.call(PATLENUPD, pat, 1)
        ops = [lambda k: (lambda c: p.call(va(p, p.q(p.q(c) + 0x70)), c, p.mkstr('S%d' % k)))(rnd.choice(chans(p))),
               lambda k: add_named(p, rnd.randrange(len(chans(p)) + 1), 'Add%d' % k),
               lambda k: len(chans(p)) > 3 and p.call(0x1004a60, rnd.randrange(len(chans(p))), 0, 3),
               lambda k: p.call(MUTE, inner(), rnd.randrange(2)),
               lambda k: p.call(MIXNAME, inner(), p.mkstr('M%d' % k), 1),
               lambda k: rec_set(p, mix_id(inner(), 0x1fc0), rnd.randrange(12800)),
               lambda k: mix_insert(p, inner()),
               lambda k: mix_move(p, rnd.randrange(2, mix_count(p) - 2), rnd.choice((-1, 1))),
               lambda k: note(), lambda k: note(),
               lambda k: (closer(pid), patop(p, 0x118, rnd.randrange(1, 10), 0, 0, 0, 0)),          # pattern insert
               lambda k: (closer(pid), patop(p, 0xe8, one(rnd.randrange(1, 10)), 0, 0, 0, 0)),      # clone
               lambda k: patop(p, 0x108, one(rnd.randrange(2, 10)), 0, rnd.choice((-1, 1, 2)), -1 & 0xffffffffffffffff, 0),   # move
               lambda k: (answerer(pid), patop(p, 0x120, one(rnd.randrange(1, 10)), 0, 0, 0, 0))]  # split by channel
        st = {'pos': pos0, 'loading': False}
        def reloading():                                     # this FL asked for / loads the leader's project: a user
            with open(LOG, 'rb') as f:                       # can't act then (input held), so neither does the probe
                f.seek(st['pos']); data = f.read()
            st['pos'] += data.rfind(b'\n') + 1               # (whole lines only; the rest next time)
            for l in data[:data.rfind(b'\n') + 1].decode('utf-8', 'replace').splitlines():
                if '[%d]' % pid not in l: continue
                if 'kind":"state.req"' in l and 'OP out' in l or 'PROJECT received' in l: st['loading'] = True
                elif 'PROJECT caught up' in l: st['loading'] = False    # (its held changes have run too)
            return st['loading']
        for k in range(int(os.environ.get('FLIM_STRESS_OPS', '40'))):
            t0 = time.time()
            while reloading() and time.time() - t0 < 15: time.sleep(0.2)
            try: rnd.choice(ops)(k * 2 + odd)
            except Exception as e: print('   op on %s: %s' % ('AB'[odd], e))
            time.sleep(rnd.choice((0.0, 0.02, 0.05, 0.15)))
    pos0 = os.path.getsize(LOG)
    seed = int(os.environ.get('FLIM_STRESS_SEED', '11'))
    ts = [threading.Thread(target=worker, args=(p, odd, seed + odd * 1000)) for odd, p in ((0, a), (1, b))]
    for t in ts: t.start()
    for t in ts: t.join()
    def state(p):                                           # (notes at the same spot: in either order)
        s = shared_state(p); s['notes'] = [sorted(unsel(notes(p, pt))) for pt in range(1, 61)]
        s['patterns'] = [(i, ustr(p, p.q(pat_rec(p, i))), p.d(pat_rec(p, i) + 8), p.d(pat_rec(p, i) + 0x44)) for i in range(1, 61)]   # name, color, length
        s['mutes'] = s['mutes'][:-1]                        # (not the Current track: FL's save / load doesn't keep its mute)
        s['channel colors'] = [p.d(c + 0x78) for c in chans(p)]
        return s
    time.sleep(6)
    try:                                                    # what differs before any re-sync repairs it
        sa, sb = state(a), state(b)
        for key in sa:
            x, y = sa[key], sb[key]
            if x != y:
                d = [(i, x[i], y[i]) for i in range(len(x)) if x[i] != y[i]][:4] if len(x) == len(y) else (x, y)
                print('   (stress_both: %s differ at first: %r)' % (key, d))
    except RuntimeError: pass
    t0 = time.time()
    while time.time() - t0 < 90:
        try: sa, sb = state(a), state(b)
        except RuntimeError: time.sleep(3); continue
        if sa == sb: break
        time.sleep(3)
    for key in sa: check('stress_both: ' + key, lambda key=key: sa[key], lambda key=key: sb[key], wait=0)

# A song saved while soloed has the solo in its file. The leader opening it is soloed (FL's doing); the
# others get it without, every track enabled (opening it sets every track's +0x1c: what unsolo gives back).
OPENSTR = 0x16bcca8                                             # FL's open handler (WM 0x566) opens the file named here
def open_song(p, pid, path):
    import ctypes, ctypes.wintypes
    p.w(p.a(OPENSTR), struct.pack('<Q', p.mkstr(path)))
    for w in fl.windows((pid,)):
        if w[2] == 'TFruityLoopsMainForm': ctypes.windll.user32.PostMessageW(w[1], 0x566, 0, 0)
    def left(k):
        r = ctypes.wintypes.RECT(); ctypes.windll.user32.GetWindowRect(k, ctypes.byref(r)); return r.left
    for _ in range(12):                                         # "Save changes to ...?" Yes / No / Cancel: No
        time.sleep(0.5)
        for q, h, c, t in fl.windows((pid,)):
            btns = sorted((k for k, cc in fl.children(h) if cc == 'TQuickFocusBtn'), key=left) if c == 'TMsgForm' else []
            if len(btns) == 3: fl.post_click(btns[1]); return

def t_solo_song():
    path = os.path.join(TMP, 'flim_live_soloed.flp')
    a.call(SOLO, 3, 1, 2); time.sleep(1.5)
    a.call(V(a, 'SAVEPROJ'), a.mkstr(path), 0)
    a.call(SOLO, 3, 0, 2); time.sleep(1.5)
    pos = os.path.getsize(LOG)
    open_song(a, A, path)
    check('soloed song: B opened it', lambda: any('PROJECT opened' in l for l in log_since(pos, B)), lambda: True, wait=30)
    time.sleep(3)
    check('soloed song: A opened it soloed', lambda: mixb(a, 3, 0x1a), lambda: 1)
    check('soloed song: B has no solo', lambda: [mixb(b, t, 0x1a) for t in range(mix_count(b))], lambda: [0] * mix_count(b))
    check('soloed song: B\'s mutes are A\'s shared ones', lambda: mutes(b), lambda: [mixb(a, t, 0x1c) for t in range(mix_count(a))])
    a.call(SOLO, 3, 0, 2)
    check('soloed song: A unsolos to the same', lambda: mutes(a), lambda: mutes(b), wait=4)

# While the leader's project loads on a member, the member's clicks and keys are held off (they'd be lost to
# the load) and a small popup says why; afterwards they get through again.
def t_reload_lock():
    import ctypes
    path = os.path.join(TMP, 'flim_live_reload.flp')
    a.call(V(a, 'SAVEPROJ'), a.mkstr(path), 0)
    main_b = next(w[1] for w in fl.windows((B,)) if w[2] == 'TFruityLoopsMainForm')
    pos = os.path.getsize(LOG)
    seen = {'popup': False}
    def press():                                            # B presses a key in its FL while the project loads
        t0 = time.time()
        while time.time() - t0 < 20 and not any('PROJECT received' in l for l in log_since(pos, B)): time.sleep(0.02)
        for _ in range(8):                                  # (F24 down / up: harmless when it gets through)
            ctypes.windll.user32.PostMessageW(main_b, 0x0100, 0x87, 1)
            ctypes.windll.user32.PostMessageW(main_b, 0x0101, 0x87, 0xC0000001)
            time.sleep(0.1)
            seen['popup'] = seen['popup'] or any(w[2] == 'FlimSaving' for w in fl.windows((B,)))
        while not seen['popup'] and time.time() - t0 < 20 and not any('PROJECT settled' in l for l in log_since(pos, B)):
            seen['popup'] = any(w[2] == 'FlimSaving' for w in fl.windows((B,)))   # (it opens once FL's main thread is free)
            time.sleep(0.05)
    t = threading.Thread(target=press, daemon=True); t.start()
    open_song(a, A, path)
    t.join()
    check("reload lock: B's key press held off", lambda: any('project is loading: held off' in l for l in log_since(pos, B)), lambda: True, wait=3)
    check('reload lock: B showed the popup', lambda: seen['popup'], lambda: True, wait=0)
    check('reload lock: B loaded the project', lambda: any('PROJECT settled' in l for l in log_since(pos, B)), lambda: True, wait=30)
    time.sleep(1)
    pos2 = os.path.getsize(LOG)
    ctypes.windll.user32.PostMessageW(main_b, 0x0100, 0x87, 1)
    ctypes.windll.user32.PostMessageW(main_b, 0x0101, 0x87, 0xC0000001)
    time.sleep(1)
    check("reload lock: B's keys get through after", lambda: any('held off' in l for l in log_since(pos2, B)), lambda: False, wait=0)

# Host controls (the leader's, through its engine as flim's window sends them: GUI_ADMIN; the server must allow the
# leader: the Node server does, the built-in host with "Let the session leader use the people controls too").
GUI_RESYNC, GUI_ADMIN = 8, 10
def fl_id(pid):                                                 # its id in the session ("I am A (fl-...)")
    for l in open(LOG, encoding='utf-8', errors='replace'):
        m = re.search(r'\[%d\] I am \S+ \((\S+)\)' % pid, l)
        if m: return m.group(1)
def admin(pid, action, who, on=0): engine_cmd(pid, GUI_ADMIN, '%s\t%s\t%d' % (action, who, on))
def logged(pos, pid, text, wait=0):
    t0 = time.time()
    while True:
        if any(text in l for l in log_since(pos, pid)): return True
        if time.time() - t0 >= wait: return False
        time.sleep(0.25)

class RawWs:                                                    # a bare WebSocket client, no FL: what the server lets through
    def __init__(self, uid, name, room='FLIM'):
        import socket, base64
        host, port = URL.split('://')[-1].split(':')
        self.s = socket.create_connection((host, int(port)), timeout=5)
        self.s.sendall(('GET / HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n'
                        'Sec-WebSocket-Key: %s\r\n\r\n' % (host, port, base64.b64encode(os.urandom(16)).decode())).encode())
        r = b''
        while b'\r\n\r\n' not in r: r += self.s.recv(4096)
        self.buf, self.got = r.split(b'\r\n\r\n', 1)[1], []
        self.send({'t': 'join_room', 'room': room, 'password': '', 'user': {'id': uid, 'name': name}})
    def send(self, obj):
        import json
        d, m = json.dumps(obj, separators=(',', ':')).encode(), os.urandom(4)   # (compact, as flim sends: host.c relays it as is)
        h = bytes([0x81, 0x80 | len(d)]) if len(d) < 126 else bytes([0x81, 0xfe]) + struct.pack('>H', len(d))
        self.s.sendall(h + m + bytes(c ^ m[i & 3] for i, c in enumerate(d)))
    def _need(self, n):
        while len(self.buf) < n:
            k = self.s.recv(1 << 20)
            if not k: raise EOFError
            self.buf += k
    def pump(self, secs=1.0):                                   # read what comes for secs; text messages go to self.got
        import json, socket
        self.s.settimeout(0.2); t0 = time.time()
        while time.time() - t0 < secs:
            try:
                self._need(2); n = self.buf[1] & 127; o = 2
                if n == 126: self._need(4); n = struct.unpack('>H', self.buf[2:4])[0]; o = 4
                elif n == 127: self._need(10); n = struct.unpack('>Q', self.buf[2:10])[0]; o = 10
                self._need(o + n); op, d = self.buf[0] & 15, self.buf[o:o + n]; self.buf = self.buf[o + n:]
                if op == 1: self.got.append(json.loads(d))
                elif op == 8: return False                      # closed by the server
            except socket.timeout: pass
            except (EOFError, OSError): return False
        return True
    def close(self): self.s.close()

def t_give_host():
    ida, idb = fl_id(A), fl_id(B)
    pos = os.path.getsize(LOG)
    admin(A, 'give_host', idb)
    check('give host: B leads now', lambda: logged(pos, B, 'made you the session leader', 8), lambda: True, wait=0)
    check('...A is told', lambda: logged(pos, A, 'the session leader (instead of you)', 3), lambda: True, wait=0)
    t, v = 0x40000005, 0
    v = (rec_get(b, t) + 10000) % 200000 + 100000
    rec_set(b, t, v)
    check('give host: the new leader\'s edit reaches A', lambda: rec_get(a, t), lambda: rec_get(b, t), wait=4)
    i = mix_id(9, 0x1fc1); rec_set(a, i, (rec_get(a, i) + 1111) % 12800)
    check('give host: a member\'s (A) edit reaches B', lambda: rec_get(b, i), lambda: rec_get(a, i), wait=4)
    pos2 = os.path.getsize(LOG)
    admin(A, 'sync_only', idb, 1)                               # A isn't the leader any more: refused
    check('give host: the old leader can\'t use the controls', lambda: logged(pos2, A, 'not_allowed', 4), lambda: True, wait=0)
    admin(B, 'give_host', ida)
    check('give host: back to A', lambda: logged(pos2, A, 'made you the session leader', 8), lambda: True, wait=0)
    time.sleep(2)
    check('give host: nobody reloaded', lambda: any('PROJECT received' in l for l in log_since(pos, A) + log_since(pos, B)), lambda: False, wait=0)

def t_sync_only():
    idb = fl_id(B)
    pos = os.path.getsize(LOG)
    admin(A, 'sync_only', idb, 1)
    check('sync-only: B is told', lambda: logged(pos, B, 'made this FL sync-only', 6), lambda: True, wait=0)
    i = mix_id(10, 0x1fc1); before = rec_get(a, i)
    rec_set(b, i, (before + 3333) % 12800); time.sleep(4)
    check("sync-only: B's edit doesn't reach A", lambda: rec_get(a, i), lambda: before, wait=0)
    check('...B kept it (log)', lambda: logged(pos, B, 'sync-only: this edit stays', 2), lambda: True, wait=0)
    j = mix_id(11, 0x1fc1); rec_set(a, j, (rec_get(a, j) + 2222) % 12800)
    check("sync-only: A's edit reaches B", lambda: rec_get(b, j), lambda: rec_get(a, j), wait=4)
    # the server drops a sync-only member's ops (not only the engine): a raw client, normal first, then sync-only
    xid = 'raw-x-%d' % os.getpid()
    x = RawWs(xid, 'RawX%d' % (os.getpid() % 10000)); x.pump(1.5)          # (sync-only sticks to a name: one per run)
    t = 0x40000001                                              # swing
    v1 = (rec_get(a, t) + 1000) % 12800
    x.send({'t': 'op', 'op': {'kind': 'param.set', 'id': t, 'v': v1}})
    check('sync-only: a raw member\'s op reaches A (normal)', lambda: rec_get(a, t), lambda: v1, wait=4)
    admin(A, 'sync_only', xid, 1); x.pump(1.5)
    x.send({'t': 'op', 'op': {'kind': 'param.set', 'id': t, 'v': (v1 + 1000) % 12800}}); x.pump(3)
    check('sync-only: the server drops its op', lambda: rec_get(a, t), lambda: v1, wait=0)
    x.close(); time.sleep(1)
    x = RawWs(xid, 'RawX%d' % (os.getpid() % 10000)); x.pump(1.5)
    check('sync-only: it sticks when they reconnect', lambda: any(p.get('sync') for m in x.got if m.get('t') == 'presence'
                                                                  for p in m['peers'] if p['id'] == xid), lambda: True, wait=0)
    admin(A, 'sync_only', xid, 0); x.pump(1); x.close()
    pos = os.path.getsize(LOG)
    admin(A, 'sync_only', idb, 0)
    check('sync-only off: B is told', lambda: logged(pos, B, 'switched this FL back from sync-only', 6), lambda: True, wait=0)
    check('...B reloads the leader\'s project (its edit stayed local)', lambda: logged(pos, B, 'PROJECT settled', 40), lambda: True, wait=0)
    check("...B matches A again", lambda: rec_get(b, i), lambda: rec_get(a, i), wait=4)
    k = mix_id(12, 0x1fc1); rec_set(b, k, (rec_get(b, k) + 1234) % 12800)
    check("...B's edits reach A again", lambda: rec_get(a, k), lambda: rec_get(b, k), wait=4)

def t_kick():
    ida, idb = fl_id(A), fl_id(B)
    pos = os.path.getsize(LOG)
    admin(A, 'kick', idb)
    check('kick: B is told and disconnected', lambda: logged(pos, B, 'removed you from the session', 6), lambda: True, wait=0)
    check('...A sees B removed', lambda: logged(pos, A, '"banned":[{"id":"%s"' % idb, 6), lambda: True, wait=0)
    pos = os.path.getsize(LOG)
    engine_cmd(B, 3, URL + '\n\nB')
    check("kick: B can't join again", lambda: logged(pos, B, 'You were removed from this session', 8), lambda: True, wait=0)
    admin(A, 'unban', idb); time.sleep(1)
    pos = os.path.getsize(LOG)
    engine_cmd(B, 3, URL + '\n\nB')
    check('kick: let back in, B joins and loads the project', lambda: logged(pos, B, 'PROJECT settled', 40), lambda: True, wait=0)
    i = mix_id(13, 0x1fc1); rec_set(a, i, (rec_get(a, i) + 777) % 12800)
    check('...and gets edits again', lambda: rec_get(b, i), lambda: rec_get(a, i), wait=4)
    # IP ban: B is out, and so is anyone else from its address (here: a raw client with another id and name, same PC)
    pos = os.path.getsize(LOG)
    admin(A, 'ban_ip', idb)
    check('ban IP: B is told and disconnected', lambda: logged(pos, B, 'banned you from this server', 6), lambda: True, wait=0)
    check('...A sees it', lambda: logged(pos, A, '"ip":true', 6), lambda: True, wait=0)
    x = RawWs('raw-y-%d' % os.getpid(), 'RawY'); x.pump(1.5); x.close()
    check('ban IP: another client from that address is refused', lambda: [m.get('code') for m in x.got if m.get('t') == 'error'], lambda: ['kicked'], wait=0)
    admin(A, 'unban', idb); time.sleep(1)
    pos = os.path.getsize(LOG)
    engine_cmd(B, 3, URL + '\n\nB')
    check('IP unban: B joins again', lambda: logged(pos, B, 'PROJECT settled', 40), lambda: True, wait=0)
    x = RawWs('raw-y-%d' % os.getpid(), 'RawY'); x.pump(1.5); x.close()
    check('...and so can others from that address', lambda: [m['t'] for m in x.got][:1], lambda: ['welcome'], wait=0)

if __name__ == '__main__':
    T = {k[2:]: v for k, v in list(globals().items()) if k.startswith('t_')}
    for t in sys.argv[1:] or list(T):
        T[t]()
    print('%d ok, %d failed' % (ok, fail))

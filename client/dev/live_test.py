# Live sync checks on two FLs started by restart_test.ps1 (A = leader, B = member, FL 2026
# 26.1.7.5654): make a change in A (or B) through FL's own functions via the probe, wait, compare.
#   python live_test.py [test ...]            (no names: all, in order; e.g. "mix_insert mix_delete")
# FLIM_TEST_VST=<.fst of a plugin to load into a mixer slot> (default: Xfer OTT from FL's plugin database).
import sys, os, time, struct, math, threading
import fl
from mk import *

sys.stdout.reconfigure(errors='replace')                     # project names the console can't show
a, b = probe(A), probe(B)
ok = fail = 0

# flim only syncs what someone does: a param that moves with nobody at the keyboard is FL's own
# (automation, modulation). The probe makes no input, so a background "user" keeps tapping an
# unused key (F24, key up) in both FLs while the tests run.
def _user():
    import ctypes
    while True:
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

def make_wav(path, hz):
    n = 44100
    data = b''.join(struct.pack('<h', int(8000 * math.sin(i * 2 * math.pi * hz / 44100))) for i in range(n))
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' +
                           struct.pack('<IHHIIHH', 16, 1, 1, 44100, 88200, 2, 16) + b'data' + struct.pack('<I', len(data)) + data)

def fst_with_sample(src, dst, wav):
    # the .fst again with a sample path event (196) after the plugin name (203).
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
        out += d[p:q + n]
        if e == 203: out += bytes([196]) + varint(len(path)) + path
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

def t_mixer_name():
    a.call(MIXNAME, 2, a.mkstr('FlimMix'), 1)
    check('mixer name', lambda: mix_name(a, 2), lambda: mix_name(b, 2))

def t_mixer_mute():
    for on, nm in ((0, 'mixer mute'), (1, 'mixer unmute')):
        a.call(0x127b9b0, 3, on)
        check(nm, lambda: a.r(mix_rec(a, 3) + 0x18, 1), lambda: b.r(mix_rec(b, 3) + 0x18, 1))

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

def mix_state(p):
    n = mix_count(p)
    return n, [mix_name(p, t) for t in range(n)], [rec_get(p, mix_id(t, 0x1fc0)) for t in range(1, n - 1)], [has_plug(p, t) for t in range(1, n - 1)]

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

def t_mix_move():
    mix_move(a, 3, 1)
    check('mixer: A moves track 3 right (edits)', lambda: mix_state(a), lambda: mix_state(b), wait=15)

if __name__ == '__main__':
    T = {k[2:]: v for k, v in list(globals().items()) if k.startswith('t_')}
    for t in sys.argv[1:] or list(T):
        T[t]()
    print('%d ok, %d failed' % (ok, fail))

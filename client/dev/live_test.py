# Live sync checks on two FLs started by restart_test.ps1 (A = leader, B = member, FL 2026
# 26.1.7.5654): make a change in A (or B) through FL's own functions via the probe, wait, compare.
#   python live_test.py [test ...]            (no names: all, in order; e.g. "mix_insert mix_delete")
# FLIM_TEST_VST=<.fst of a plugin to load into a mixer slot> (default: Xfer OTT from FL's plugin database).
import sys, os, re, time, struct, math, threading
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

# Quick mute + unmute. While audio runs FL fades a mute out, and an unmute during the fade is lost: the
# two queued behind other work apply as one, and a track that ends up against flim's apply gets it again.
def t_mute_fast():
    import ctypes
    t, sleep = 4, ctypes.cast(ctypes.windll.kernel32.Sleep, ctypes.c_void_p).value
    pos = os.path.getsize(LOG)
    busy = threading.Thread(target=lambda: b.call(va(b, sleep), 2500)); busy.start()   # B's main thread busy
    time.sleep(0.3); a.call(MUTE, t, 0); time.sleep(0.5); a.call(MUTE, t, 1); busy.join()
    check('mute + unmute while B is busy', lambda: (mixb(a, t, 0x18), mixb(b, t, 0x18)), lambda: (1, 1), wait=4)
    check('...B applies only the last', lambda: sum('APPLY id=0x%08x' % (0xF0000000 | t) in l for l in log_since(pos, B)), lambda: 1)
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
    engine_cmd(B, 4); time.sleep(2); engine_cmd(B, 3, 'localhost:8080\n\nB')   # B reconnects (Disconnect, Connect)
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
    rec_set(a, target, (rec_get(a, target) + 2345) % 12800); time.sleep(2.5)
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
    def worker(p, odd, seed):
        rnd = random.Random(seed)
        pid = B if odd else A
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
               lambda k: (closer(pid), patop(p, 0xe8, (lambda buf: (p.w(buf, struct.pack('<i', rnd.randrange(1, 10))), buf)[1])(p.alloc(8)), 0, 0, 0, 0))]   # clone
        for k in range(int(os.environ.get('FLIM_STRESS_OPS', '40'))):
            try: rnd.choice(ops)(k * 2 + odd)
            except Exception as e: print('   op on %s: %s' % ('AB'[odd], e))
            time.sleep(rnd.choice((0.0, 0.02, 0.05, 0.15)))
    seed = int(os.environ.get('FLIM_STRESS_SEED', '11'))
    ts = [threading.Thread(target=worker, args=(p, odd, seed + odd * 1000)) for odd, p in ((0, a), (1, b))]
    for t in ts: t.start()
    for t in ts: t.join()
    def state(p):                                           # (notes at the same spot: in either order)
        s = shared_state(p); s['notes'] = [sorted(unsel(notes(p, pt))) for pt in range(1, 16)]
        s['patterns'] = pat_names(p, 15); return s
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

if __name__ == '__main__':
    T = {k[2:]: v for k, v in list(globals().items()) if k.startswith('t_')}
    for t in sys.argv[1:] or list(T):
        T[t]()
    print('%d ok, %d failed' % (ok, fail))

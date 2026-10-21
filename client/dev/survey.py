# Which plugin states (event 213 in .fst) hold UTF-16 sample paths, and do they parse as TLV trees?
import os, re, sys, collections
from flp import events
from tlv import parse
ROOTS = [r'C:\Program Files (x86)\Image-Line\FL Studio 2025\Data', os.path.expandvars(r'%USERPROFILE%\Documents\Image-Line')]
AUD = re.compile(r'\.(wav|mp3|ogg|flac|aif|aiff|wv|rx2|rex|sf2)$', re.I)

def leaves(v, a, b, depth=0):
    recs = parse(v, a, b)
    if recs is None: return None
    out = []
    for tag, h, s, e in recs:
        p = v[s:e]
        txt = None
        if len(p) >= 8 and len(p) % 2 == 0:
            try:
                t = p.decode('utf-16le').rstrip('\0')
                if t and all(ord(c) >= 32 for c in t) and AUD.search(t): txt = t
            except UnicodeDecodeError: pass
        if txt: out.append((tag, txt)); continue
        if depth < 8 and e - s >= 12:
            k = leaves(v, s, e, depth + 1)
            if k: out += k
    return out

stats = collections.defaultdict(lambda: [0, 0, 0, None])
for root in ROOTS:
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if not f.lower().endswith('.fst'): continue
            path = os.path.join(dp, f)
            try: d = open(path, 'rb').read()
            except OSError: continue
            if not d.startswith(b'FLhd'): continue
            try: evs = events(d)
            except Exception: continue
            plug = None
            for e, v in evs:
                if e == 201:   # plugin internal name
                    plug = v.decode('utf-16le', 'replace').rstrip('\0')
            for e, v in evs:
                if e != 213: continue
                has = [m.group().decode('utf-16le') for m in re.finditer(rb'(?:[ -~]\x00){6,}', v) if AUD.search(m.group().decode('utf-16le'))]
                if not has: continue
                st = stats[plug]
                st[0] += 1
                got = None
                for off in (0, 4, 8):
                    got = leaves(v, off, len(v))
                    if got: break
                if got and len(got) >= len(has): st[1] += 1
                elif got: st[2] += 1
                if st[3] is None: st[3] = (path, has[:2], got[:2] if got else None)
for k, (n, ok, part, ex) in sorted(stats.items(), key=lambda x: -x[1][0]):
    print('%-28s files=%d tlv_all=%d tlv_partial=%d' % (k, n, ok, part))
    print('    e.g.', ex[0][-90:], ex[1][:1], ex[2][:1] if ex[2] else None)

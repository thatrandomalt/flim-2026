import struct, sys
from flp import events

def parse(v, a, b, depth=0, out=None):
    """TLV records [tag u32][len u64][payload] exactly covering v[a:b]; None if not."""
    recs = []
    p = a
    while p < b:
        if b - p < 12: return None
        tag, ln = struct.unpack('<IQ', v[p:p+12])
        if ln > b - p - 12: return None
        recs.append((tag, p, p + 12, p + 12 + ln))
        p += 12 + ln
    return recs

def tree(v, a, b, depth=0, maxdepth=6):
    recs = parse(v, a, b)
    if recs is None: return None
    out = []
    for tag, h, s, e in recs:
        kids = tree(v, s, e, depth + 1) if depth < maxdepth and e - s >= 12 else None
        out.append((tag, h, s, e, kids))
    return out

def show(t, v, ind=0, lim=40):
    for tag, h, s, e, kids in t[:lim]:
        prev = v[s:min(e, s + 24)]
        txt = ''
        try:
            if len(v[s:e]) % 2 == 0 and all(32 <= c < 127 for c in v[s:e:2][:20]) and v[s+1:e:2][:20].count(0) == len(v[s+1:e:2][:20]):
                txt = repr(v[s:e].decode('utf-16le')[:80])
        except Exception: pass
        print('  ' * ind + 'tag %x len %d %s' % (tag, e - s, txt or prev.hex()))
        if kids: show(kids, v, ind + 1, lim)

if __name__ == '__main__':
    f = sys.argv[1]; off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    for e, v in events(open(f, 'rb').read()):
        if e == 213:
            for o in range(0, 16, 4):
                t = tree(v, o, len(v))
                print('offset', o, 'parses' if t else 'no')
                if t: show(t, v); break

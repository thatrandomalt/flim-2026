import struct, sys
def events(d):
    j = d.find(b'FLdt'); p = j + 8; out = []
    while p < len(d):
        e = d[p]; p += 1
        if e == 172:                     # FL 2026: escape, a 32-bit id follows (>= 0x100: sized by top bits)
            e = struct.unpack_from('<I', d, p)[0]; p += 4
        c = e >> 6 if e < 0x100 else e >> 30
        if c == 0: v = d[p:p+1]; p += 1
        elif c == 1: v = d[p:p+2]; p += 2
        elif c == 2: v = d[p:p+4]; p += 4
        else:
            n = 0; sh = 0
            while True:
                b = d[p]; p += 1; n |= (b & 0x7f) << sh; sh += 7
                if b < 128: break
            v = d[p:p+n]; p += n
        out.append((e, v))
    return out

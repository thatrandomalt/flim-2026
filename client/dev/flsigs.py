# Signatures for the engine's FL addresses: rewrites ../engine/fl_addrs.h.
# For every FLA(name, va, ...) line there, finds a byte pattern (?? = any byte) that occurs exactly once
# in .text of FLEngine_x64.dll and leads back to va: a function's first bytes, or for a global (and the
# marker RTTI) an instruction that addresses it rip-relative, with the offset of its rel32 (off) and of
# the next instruction (nx). Bytes that move between builds are wildcards: rip-relative displacements,
# branch targets and anything the base relocations touch.
#
# Porting to a new FL build (FLRE_DLL=<path> for a DLL other than the installed FL's):
#   python flsigs.py --rebase   rows whose pattern still matches once get their new VA; lists the others
#   (find those by hand, put their VAs in, set FLIM_FL_BUILD), then
#   python flsigs.py            new patterns from the VAs, each checked the way the engine does (one
#                               match, decodes to va) before anything is written
#   python flsigs.py --json     only print the DLL's addresses ({name: va}); mk.py uses it for other builds
import bisect, os, re, sys
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP
from flre import DLL, BASE, TEXT, rd, func_of, xrefs, text_arr

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'engine', 'fl_addrs.h')
MIN, MAX = 24, 96          # pattern length: unique and at least MIN bytes (fewer false matches in a new build)
BACK = 4                   # a reference pattern may start up to this many instructions before the reference
REFS = 80                  # references tried per global (MAINOBJ has over a thousand)

T = text_arr()
T0 = TEXT[0]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def relocated():
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_BASERELOC']])
    m = np.zeros(len(T) + 8, dtype=bool)
    for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
        for e in blk.entries:
            if e.type == 10:                          # DIR64
                o = e.rva + BASE - T0
                if 0 <= o < len(T): m[o:o + 8] = True
    return m
RELOC = relocated()

def insns(start, end):
    return list(md.disasm(rd(start, end - start), start))

def wild(ins):
    # which of the instruction's bytes change when FL is rebuilt
    w = [False] * ins.size
    def cover(o, n):
        for i in range(o, min(o + n, ins.size)): w[i] = True
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP: cover(ins.disp_offset, ins.disp_size)
        if op.type == X86_OP_IMM and (ins.group(CS_GRP_JUMP) or ins.group(CS_GRP_CALL)): cover(ins.imm_offset, ins.imm_size)
    o = ins.address - T0
    for i in range(ins.size):
        if RELOC[o + i]: w[i] = True
    return w

def shortest(seq, must_end=0):
    # seq: instructions from the pattern's start. The shortest prefix (whole instructions, at least
    # MIN bytes and past must_end) that matches once in .text -> (bytes, wildcards) or None.
    s = seq[0].address - T0
    pb, pw, idx = [], [], None
    for ins in seq:
        for b, w in zip(ins.bytes, wild(ins)):
            i = len(pb)
            pb.append(b); pw.append(w)
            if w: continue
            if idx is None: idx = np.nonzero(T[i:len(T) - MAX + i] == b)[0]
            else: idx = idx[T[idx + i] == b]
        if len(pb) > MAX: return None
        if len(pb) >= MIN and ins.address + ins.size >= must_end and idx is not None and len(idx) == 1:
            assert idx[0] == s
            return pb, pw
    return None

def fmt(pb, pw):
    return ' '.join('??' if w else '%02x' % b for b, w in zip(pb, pw))

def func_sig(va):
    p = shortest(insns(va, va + MAX + 16))
    return (-1, 0, p) if p else None

fast = Cs(CS_ARCH_X86, CS_MODE_64)

def ref_sigs(va):
    # globals: instructions that address va rip-relative (disp32, target = next instruction + disp)
    best = None
    for pos in xrefs(va)[:REFS]:
        f = func_of(pos)
        if not f: continue
        starts = [a for a, n, _, _ in fast.disasm_lite(rd(f[0], pos + 16 - f[0]), f[0])]   # instruction boundaries
        k = bisect.bisect_right(starts, pos) - 1
        if k < 0: continue
        ins = insns(starts[k], starts[k] + 16)[0]
        if not (ins.address + ins.disp_offset == pos and ins.disp_size == 4 and any(
                op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP for op in ins.operands)): continue
        if ins.address + ins.size + op_disp(ins) != va: continue
        for b in range(min(k, BACK) + 1):
            s = starts[k - b]
            p = shortest(insns(s, min(f[1], s + MAX + 16)), ins.address + ins.size)
            if p and (not best or len(p[0]) < len(best[2][0])):
                best = (pos - s, ins.address + ins.size - s, p)
        if best and len(best[2][0]) <= MIN + 4: break
    return best

def op_disp(ins):
    return int.from_bytes(ins.bytes[ins.disp_offset:ins.disp_offset + 4], 'little', signed=True)

def find(off, nx, pat):
    # the engine's view: (number of matches in .text, the address the only one decodes to)
    pb = [None if t == '??' else int(t, 16) for t in pat.split()]
    idx = None
    for i, b in enumerate(pb):
        if b is None: continue
        idx = np.nonzero(T[i:len(T) - len(pb) + i + 1] == b)[0] if idx is None else idx[T[idx + i] == b]
    if idx is None or len(idx) != 1: return (0 if idx is None else len(idx)), None
    m = T0 + int(idx[0])
    return 1, m if off < 0 else m + nx + int.from_bytes(T[idx[0] + off:idx[0] + off + 4].tobytes(), 'little', signed=True)

def check(va, off, nx, pat):
    n, got = find(off, nx, pat)
    return 'matches %d times' % n if n != 1 else None if got == va else 'decodes to %x' % got

src = open(OUT).read()
head = src[:re.search(r'^FLA\(', src, re.M).start()]
ROW = r'^FLA\((\w+),\s*(0x[0-9a-fA-F]+),\s*(-?\d+),\s*(\d+),\s*"([^"]*)"\)'
if '--json' in sys.argv:
    # this DLL's addresses as JSON {name: va}, for the rows whose pattern matches once (mk.py: other builds)
    import json
    out = {}
    for m in re.finditer(ROW, src, re.M):
        n, got = find(int(m.group(3)), int(m.group(4)), m.group(5))
        if n == 1: out[m.group(1)] = got
    sys.exit(print(json.dumps(out)))
if '--rebase' in sys.argv:
    # new FL build: move each row whose pattern still matches once to where it matches; list the rest
    lost = []
    def moved(m):
        n, got = find(int(m.group(3)), int(m.group(4)), m.group(5))
        if n != 1: lost.append('%s (%s)' % (m.group(1), 'not found' if not n else 'found %d times' % n)); return m.group(0)
        return m.group(0).replace(m.group(2), '%#x' % got, 1)
    src = re.sub(ROW, moved, src, flags=re.M)
    open(OUT, 'w', newline='\n').write(src)
    if lost: sys.exit('rebased; find these by hand, then run flsigs.py: ' + ', '.join(lost))
    sys.exit(print('rebased: every signature matched'))
rows = re.findall(r'^FLA\((\w+),\s*(0x[0-9a-fA-F]+)', src, re.M)
out, bad = [], []
for name, v in rows:
    va = int(v, 16)
    r = func_sig(va) if T0 <= va < T0 + len(T) and func_of(va) and func_of(va)[0] == va else None
    r = r or ref_sigs(va)
    if not r:
        bad.append(name); out.append('FLA(%-12s %#9x, -1,  0, "")' % (name + ',', va)); continue
    off, nx, (pb, pw) = r
    pat = fmt(pb, pw)
    why = check(va, off, nx, pat)
    if why: bad.append('%s (%s)' % (name, why))
    out.append('FLA(%-12s %#9x, %2d, %2d, "%s")' % (name + ',', va, off, nx, pat))
    print('%-12s %#9x %-4s %3d bytes%s' % (name, va, 'func' if off < 0 else 'ref', len(pb), '  ' + why if why else ''))
if bad:
    sys.exit('no signature for: ' + ', '.join(bad) + ' (fl_addrs.h not written)')
open(OUT, 'w', newline='\n').write(head + '\n'.join(out) + '\n')
print('%d signatures -> %s' % (len(out), os.path.normpath(OUT)))

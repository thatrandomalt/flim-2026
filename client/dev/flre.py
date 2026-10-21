# RE helpers for FLEngine_x64.dll (VAs at image base 0x400000, like the engine's *_VA defines)
import sys, struct, re, os, pickle, bisect
import numpy as np
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = os.environ.get('FLRE_DLL', r'C:\Program Files\Image-Line\FL Studio 2026\FLEngine_x64.dll')
HERE = os.path.dirname(os.path.abspath(__file__))
BASE = 0x400000

data = open(DLL, 'rb').read()
pe = pefile.PE(DLL, fast_load=True)
SECS = [(s.VirtualAddress + BASE, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData, s.Name.rstrip(b'\0')) for s in pe.sections]
TEXT = SECS[0]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False

def off(va):
    for v, sz, raw, rsz, n in SECS:
        if v <= va < v + min(sz, rsz): return raw + va - v
    return None

def va_of(o):
    for v, sz, raw, rsz, n in SECS:
        if raw <= o < raw + rsz: return v + o - raw
    return None

def rd(va, n):
    o = off(va); return data[o:o + n] if o is not None else b""
def q(va): return struct.unpack('<Q', rd(va, 8))[0]
def d(va): return struct.unpack('<I', rd(va, 4))[0]

# .pdata function table
_pd = None
def pdata():
    global _pd
    if _pd is None:
        dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
        o = off(dd.VirtualAddress + BASE)
        raw = np.frombuffer(data[o:o + dd.Size // 12 * 12], dtype='<u4').reshape(-1, 3)
        raw = raw[raw[:, 0] != 0]
        order = np.argsort(raw[:, 0])
        _pd = (raw[order, 0].astype(np.int64) + BASE, raw[order, 1].astype(np.int64) + BASE)
    return _pd

def func_of(va):
    st, en = pdata()
    i = np.searchsorted(st, va, side='right') - 1
    if i >= 0 and st[i] <= va < en[i]:
        # merge chained entries (same function split in pieces): start = first
        return int(st[i]), int(en[i])
    return None

def dis(va, n=None, end=None, show=True):
    if end is None and n is None:
        f = func_of(va); end = f[1] if f else va + 0x200
    code = rd(va, (end - va) if end else n * 15)
    out = []
    for i, ins in enumerate(md.disasm(code, va)):
        if n is not None and i >= n: break
        if end is not None and ins.address >= end: break
        line = '%x: %s %s' % (ins.address, ins.mnemonic, ins.op_str)
        # annotate rip-relative
        m = re.search(r'\[rip ([+-]) 0x([0-9a-f]+)\]', ins.op_str)
        if m:
            t = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1))
            line += '   ; -> %x%s' % (t, name_at(t))
        if ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
            t = resolve(int(ins.op_str, 16))
            if t != int(ins.op_str, 16): line += '   ; => %x' % t
            line += name_at(t)
        out.append(line)
    if show: print('\n'.join(out))
    return out

def in_text(va): return TEXT[0] <= va < TEXT[0] + TEXT[1]

def resolve(t):
    # protector call stubs: E9 rel32 (+junk) outside .text
    for _ in range(4):
        if in_text(t): return t
        b = rd(t, 5)
        if not b or b[0] != 0xE9: return t
        t = t + 5 + struct.unpack('<i', b[1:5])[0]
    return t

_stubs = None
def stubs_to(target):
    global _stubs
    if _stubs is None:
        _stubs = {}
        for v, sz, raw, rsz, n in SECS[1:]:
            if n in (b'.rsrc', b'.reloc', b'.pdata', b'.data', b'.bss'): continue
            a = np.frombuffer(data[raw:raw + min(sz, rsz)], dtype=np.uint8)
            if len(a) < 6: continue
            idx = np.nonzero(a[:-5] == 0xE9)[0]
            disp = a[idx+1].astype(np.int64) | (a[idx+2].astype(np.int64) << 8) | (a[idx+3].astype(np.int64) << 16) | (a[idx+4].astype(np.int64) << 24)
            disp = np.where(disp >= 2**31, disp - 2**32, disp)
            tg = idx + v + 5 + disp
            ok = (tg >= TEXT[0]) & (tg < TEXT[0] + TEXT[1])
            for i, t in zip(idx[ok], tg[ok]):
                _stubs.setdefault(int(t), []).append(int(i + v))
    return _stubs.get(target, [])

def callers(target):
    res = calls_to(target)
    for s in stubs_to(target): res += calls_to(s)
    return sorted(set(res))

def show_callers(target):
    for c in callers(target):
        f = func_of(c)
        print('%x in %s%s' % (c, hex(f[0]) if f else '?', name_at(f[0]) if f else ''))

# Delphi method tables: [len:u16][addr:u64][name:shortstring]
_names = None
def names():
    global _names
    p = os.path.join(os.environ.get('TEMP', HERE), 'flre_names_%d.pkl' % len(data))   # name index cache, per DLL build
    if _names is None and os.path.exists(p): _names = pickle.load(open(p, 'rb'))
    if _names is None:
        _names = {}
        tv, tsz = TEXT[0], TEXT[1]
        for m in re.finditer(rb'[\x0b-\x7f]\x00([\x00-\xff]{8})([\x03-\x60])([A-Za-z_][A-Za-z0-9_]{2,95})', data):
            ln = m.group(2)[0]
            nm = m.group(3)
            if len(nm) < ln: continue
            nm = nm[:ln]
            if struct.unpack('<H', data[m.start():m.start()+2])[0] != 2 + 8 + 1 + ln: continue
            a = struct.unpack('<Q', m.group(1))[0]
            if tv <= a < tv + tsz: _names.setdefault(a, nm.decode())
        pickle.dump(_names, open(p, 'wb'))
    return _names

def name_at(va):
    n = names().get(va)
    return ('  <' + n + '>') if n else ''

def find_name(pat):
    r = re.compile(pat)
    return sorted((a, n) for a, n in names().items() if r.search(n))

# xrefs: rip-relative disp32 anywhere in .text pointing to target(s)
_tx = None
def text_arr():
    global _tx
    if _tx is None:
        v, sz, raw, rsz, n = TEXT
        _tx = np.frombuffer(data[raw:raw + min(sz, rsz)], dtype=np.uint8)
    return _tx

def xrefs(target, kind='rip'):
    t = text_arr()
    v0 = TEXT[0]
    n = len(t) - 4
    disp = (t[:n].astype(np.int64) | (t[1:n+1].astype(np.int64) << 8) | (t[2:n+2].astype(np.int64) << 16) | (t[3:n+3].astype(np.int64) << 24))
    disp = np.where(disp >= 2**31, disp - 2**32, disp)
    pos = np.arange(n, dtype=np.int64) + v0
    res = []
    # disp32 at pos: next-instr = pos+4 (+ imm bytes for some instrs; try 0,1,4)
    for extra in (0, 1, 2, 4):
        hits = np.nonzero(pos + 4 + extra + disp == target)[0]
        for h in hits:
            res.append(int(pos[h]))
    return sorted(set(res))

def calls_to(target):
    t = text_arr(); v0 = TEXT[0]
    n = len(t) - 4
    disp = (t[1:n+1].astype(np.int64) | (t[2:n+2].astype(np.int64) << 8) | (t[3:n+3].astype(np.int64) << 16) | (t[4:n+4].astype(np.int64) << 24))[:n-1]
    disp = np.where(disp >= 2**31, disp - 2**32, disp)
    pos = np.arange(n - 1, dtype=np.int64) + v0
    ok = ((t[:n-1] == 0xE8) | (t[:n-1] == 0xE9)) & (pos + 5 + disp == target)
    return [int(x) for x in pos[ok]]

def ins_at(va):
    # find the instruction containing va by disassembling from the function start
    f = func_of(va)
    s = f[0] if f else va - 16
    for ins in md.disasm(rd(s, va - s + 16), s):
        if ins.address <= va < ins.address + ins.size: return ins
    return None

def show_xrefs(target, ctx=0):
    for x in xrefs(target):
        ins = ins_at(x)
        f = func_of(x)
        fn = (' in %x%s' % (f[0], name_at(f[0]))) if f else ''
        print('%x: %s %s%s' % (ins.address, ins.mnemonic, ins.op_str, fn) if ins else '%x ?' % x)

def strs(s, wide=True):
    out = []
    for enc in (['ascii', 'utf-16le'] if wide else ['ascii']):
        for m in re.finditer(re.escape(s.encode(enc)), data):
            out.append((enc, va_of(m.start())))
    return out


def meth(nm):
    out = []
    for m in re.finditer(bytes([len(nm)]) + nm.encode() + rb'(?![A-Za-z0-9_])', data):
        a = struct.unpack('<Q', data[m.start()-8:m.start()])[0]
        if in_text(a): out.append(a)
    return out

def near(va, before=0x60, after=0x80):
    # find a start that decodes to an instruction boundary at va
    for k in range(before, before + 40):
        s = va - k
        addrs = [i.address for i in md.disasm(rd(s, k + 16), s)]
        if va in addrs:
            return dis(s, end=va + after)
    return dis(va, end=va + after)

if __name__ == '__main__':
    exec(sys.argv[1])

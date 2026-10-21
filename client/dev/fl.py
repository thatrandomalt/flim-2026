# Client for the probe DLL + screen/input helpers.
import ctypes, ctypes.wintypes as W, os, sys, time, struct, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, 'probe', 'probe.dll')
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32 = ctypes.WinDLL('user32', use_last_error=True)
k32.OpenProcess.restype = W.HANDLE
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, W.DWORD, W.DWORD]
k32.WriteProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [W.HMODULE, ctypes.c_char_p]
k32.GetModuleHandleW.restype = W.HMODULE
k32.CreateRemoteThread.restype = W.HANDLE
k32.CreateRemoteThread.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, W.DWORD, ctypes.c_void_p]

def pids(name='FL64.exe'):
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ' + name, '/FO', 'CSV', '/NH'], capture_output=True, text=True).stdout
    return [int(l.split('","')[1]) for l in out.strip().splitlines() if l.startswith('"')]

def inject(pid, dll=PROBE):
    h = k32.OpenProcess(0x1F0FFF, False, pid)
    p = dll.encode('utf-16le') + b'\0\0'
    mem = k32.VirtualAllocEx(h, None, len(p), 0x3000, 4)
    k32.WriteProcessMemory(h, mem, p, len(p), None)
    ll = k32.GetProcAddress(k32.GetModuleHandleW('kernel32.dll'), b'LoadLibraryW')
    t = k32.CreateRemoteThread(h, None, 0, ll, mem, 0, None)
    k32.WaitForSingleObject(t, 10000)
    return bool(t)

class Probe:
    def __init__(self, pid=None):
        self.pid = pid or pids()[0]
        name = r'\\.\pipe\flimprobe_%d' % self.pid
        for _ in range(100):
            try: self.f = open(name, 'r+b', buffering=0); break
            except OSError: time.sleep(0.1)
        else: raise RuntimeError('no pipe')
        self.base = int(self.cmd('base'), 16)
    def cmd(self, s):
        self.f.write((s + '\n').encode('utf-8'))
        buf = b''
        while not buf.endswith(b'\n'): buf += self.f.read(1 << 20) if False else self._rd()
        return buf[:-1].decode('utf-8', 'replace')
    def _rd(self):
        return os.read(self.f.fileno(), 1 << 21)
    def a(self, va): return self.base + va - 0x400000
    def r(self, addr, n):
        x = self.cmd('r %x %d' % (addr, n))
        if x.startswith('ERR'): return None
        return bytes.fromhex(x)
    def rv(self, va, n): return self.r(self.a(va), n)
    def q(self, addr):
        b = self.r(addr, 8); return struct.unpack('<Q', b)[0] if b else None
    def d(self, addr):
        b = self.r(addr, 4); return struct.unpack('<i', b)[0] if b else None
    def g(self, va): return self.q(self.a(va))    # read pointer global
    def w(self, addr, b): return self.cmd('w %x %s' % (addr, b.hex()))
    def ustr(self, addr): return self.cmd('ustr %x' % addr)
    def mkstr(self, s): return int(self.cmd('mkstr ' + s), 16)
    def alloc(self, n): return int(self.cmd('alloc %d' % n), 16)
    def call(self, va, *args, thread=False):
        a = ' '.join('%d' % (x & 0xFFFFFFFFFFFFFFFF) for x in args)
        r = self.cmd(('callt' if thread else 'call') + ' %x %s' % (self.a(va), a))
        if r.startswith('ERR') or r.startswith('fault'): raise RuntimeError(r)
        return int(r, 16)

def hexdump(b, base=0, w=16):
    for i in range(0, len(b), w):
        c = b[i:i+w]
        print('%08x  %-*s %s' % (base + i, w * 3, ' '.join('%02x' % x for x in c), ''.join(chr(x) if 32 <= x < 127 else '.' for x in c)))

# ---- screen / input ----
def screenshot(path=os.path.join(os.environ.get('TEMP', HERE), 'fl_shot.png'), region=None):
    ps = ("Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
          "$b=[System.Windows.Forms.SystemInformation]::VirtualScreen;")
    if region: ps += "$b=New-Object System.Drawing.Rectangle(%d,%d,%d,%d);" % region
    ps += ("$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height;$g=[System.Drawing.Graphics]::FromImage($bmp);"
           "$g.CopyFromScreen($b.Left,$b.Top,0,0,$bmp.Size);$bmp.Save('%s');" % path)
    subprocess.run(['powershell', '-NoProfile', '-Command', ps])
    return path

u32.SetCursorPos.argtypes = [ctypes.c_int, ctypes.c_int]
def click(x, y, right=False, double=False):
    u32.SetCursorPos(x, y); time.sleep(0.05)
    dn, up = (0x8, 0x10) if right else (0x2, 0x4)
    for _ in range(2 if double else 1):
        u32.mouse_event(dn, 0, 0, 0, 0); time.sleep(0.03); u32.mouse_event(up, 0, 0, 0, 0); time.sleep(0.05)

def key(vk, mods=()):
    for m in mods: u32.keybd_event(m, 0, 0, 0)
    u32.keybd_event(vk, 0, 0, 0); time.sleep(0.03); u32.keybd_event(vk, 0, 2, 0)
    for m in reversed(mods): u32.keybd_event(m, 0, 2, 0)
    time.sleep(0.05)

def typetext(s):
    for ch in s:
        vk = u32.VkKeyScanW(ord(ch))
        mods = (0x10,) if vk & 0x100 else ()
        key(vk & 0xff, mods)

VK_SHIFT, VK_CTRL, VK_ALT = 0x10, 0x11, 0x12

def fl_rect():
    h = u32.FindWindowW('TFruityLoopsMainForm', None)
    r = W.RECT(); u32.GetWindowRect(h, ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top

def shot_fl(path=os.path.join(os.environ.get('TEMP', HERE), 'fl_shot.png'), crop=None):
    x, y, w, h = fl_rect()
    if crop: x, y, w, h = x + crop[0], y + crop[1], crop[2], crop[3]
    return screenshot(path, (x, y, w, h))

def windows(pids_):
    out = []
    def cb(h, l):
        pid = W.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in pids_ and u32.IsWindowVisible(h):
            c = ctypes.create_unicode_buffer(64); u32.GetClassNameW(h, c, 64)
            t = ctypes.create_unicode_buffer(128); u32.GetWindowTextW(h, t, 128)
            out.append((pid.value, h, c.value, t.value))
        return True
    u32.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, W.HWND, W.LPARAM)(cb), 0)
    return out

def close_dialogs(pids_, cls=('TNameEditForm',)):
    for pid, h, c, t in windows(pids_):
        if c in cls: u32.PostMessageW(h, 0x10, 0, 0); print('closed', pid, c, t)

def children(h):
    kids = []
    def cb(k, l):
        cc = ctypes.create_unicode_buffer(64); u32.GetClassNameW(k, cc, 64)
        kids.append((k, cc.value)); return True
    u32.EnumChildWindows(h, ctypes.WINFUNCTYPE(ctypes.c_bool, W.HWND, W.LPARAM)(cb), 0)
    return kids

def post_click(h):
    r = W.RECT(); u32.GetClientRect(h, ctypes.byref(r))
    lp = ((r.bottom // 2) << 16) | (r.right // 2)
    u32.PostMessageW(h, 0x201, 1, lp); u32.PostMessageW(h, 0x202, 0, lp)

def confirm(pids_, which=0):
    for pid, h, c, t in windows(pids_):
        if c == 'TMsgForm':
            btns = [k for k, cc in children(h) if cc == 'TQuickFocusBtn']
            post_click(btns[which]); print('clicked button', which, 'of', len(btns), 'in', pid, t)

def form(pid, cls):
    for p, h, c, t in windows((pid,)):
        if c != 'TFruityLoopsMainForm': continue
        for k, cc in children(h):
            if cc == cls: return k
def poke(h):
    u32.PostMessageW(h, 0x100, 0x10, 0); u32.PostMessageW(h, 0x101, 0x10, 0xC0000001)

# Start an FL Studio from any folder with the flim engine in it, the way flim.exe does (started
# suspended, engine loaded, resumed), without flim.exe or flim.ini: e.g. a second, older FL build.
#   python flrun.py "<FL folder>" <name> [ws://localhost:8080]  -> prints FL's pid
# The engine is client\out\flim_engine.dll (build.bat). It connects to the url at start (FLIM_AUTOCONNECT).
# Start it from PowerShell or cmd: an FL started from a sandboxed shell (Claude Code's Bash tool) couldn't
# open 32-bit VSTs through FL's bridge ("dblue Glitch v1.3 could not be opened"); the same FL started
# from PowerShell could.
import ctypes, ctypes.wintypes as W, os, sys
k32 = ctypes.WinDLL('kernel32', use_last_error=True)

class SI(ctypes.Structure):
    _fields_ = [('cb', W.DWORD), ('r', W.LPWSTR), ('desk', W.LPWSTR), ('title', W.LPWSTR)] + \
               [(n, W.DWORD) for n in ('x', 'y', 'cx', 'cy', 'cxc', 'cyc', 'attr', 'flags')] + \
               [('show', W.WORD), ('r2', W.WORD), ('r3', ctypes.c_void_p), ('i', W.HANDLE), ('o', W.HANDLE), ('e', W.HANDLE)]
class PI(ctypes.Structure):
    _fields_ = [('hp', W.HANDLE), ('ht', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]
k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, W.DWORD, W.DWORD]
k32.WriteProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
k32.GetProcAddress.restype = ctypes.c_void_p
k32.GetProcAddress.argtypes = [W.HMODULE, ctypes.c_char_p]
k32.GetModuleHandleW.restype = W.HMODULE
k32.CreateRemoteThread.restype = W.HANDLE
k32.CreateRemoteThread.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, W.DWORD, ctypes.c_void_p]

def run(fl_dir, name, url=None):
    dll = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'out', 'flim_engine.dll'))
    env = dict(os.environ, FLIM_NAME=name)
    if url: env['FLIM_AUTOCONNECT'] = url
    block = ''.join('%s=%s\0' % kv for kv in sorted(env.items(), key=lambda kv: kv[0].upper())) + '\0'
    si, pi = SI(), PI()
    si.cb = ctypes.sizeof(SI)
    if not k32.CreateProcessW(os.path.join(fl_dir, 'FL64.exe'), None, None, None, False, 0x4 | 0x400,   # suspended, unicode env
                              ctypes.create_unicode_buffer(block), fl_dir, ctypes.byref(si), ctypes.byref(pi)):
        raise OSError('CreateProcess failed: %d' % ctypes.get_last_error())
    p = (dll + '\0').encode('utf-16-le')
    mem = k32.VirtualAllocEx(pi.hp, None, len(p), 0x3000, 4)
    k32.WriteProcessMemory(pi.hp, mem, p, len(p), None)
    ll = k32.GetProcAddress(k32.GetModuleHandleW('kernel32.dll'), b'LoadLibraryW')
    t = k32.CreateRemoteThread(pi.hp, None, 0, ll, mem, 0, None)
    k32.WaitForSingleObject(t, 15000)
    k32.ResumeThread(pi.ht)
    return pi.pid

if __name__ == '__main__':
    print(run(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None))

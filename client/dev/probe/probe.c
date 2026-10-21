// Dev probe: injected into FL, answers line commands on \\.\pipe\flimprobe_<pid>.
//   base | r ADDR N | w ADDR HEX | ustr ADDR | mkstr TEXT | alloc N | call ADDR a1..a8 | callm (main thread)
// ADDR: hex absolute, or v<hex> = Ghidra VA (image base 0x400000) inside FLEngine_x64.dll.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *g_base;
static HINSTANCE g_inst;
static HHOOK g_hook;
static HWND g_wnd;
typedef uint64_t (*fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static volatile LONG g_callPending;
static uint64_t g_fn, g_args[8], g_ret;
static volatile LONG g_callDone;
static char g_callErr[64];

static uint64_t parse_addr(const char *s) {
    if (*s == 'v') return (uint64_t)(g_base + (strtoull(s + 1, NULL, 16) - 0x400000));
    return strtoull(s, NULL, 16);
}

static void do_call(void) {
    __try { g_ret = ((fn8)g_fn)(g_args[0], g_args[1], g_args[2], g_args[3], g_args[4], g_args[5], g_args[6], g_args[7]); g_callErr[0] = 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { sprintf_s(g_callErr, sizeof(g_callErr), "fault %08lx", GetExceptionCode()); }
}

// Main-thread calls run from a message-only window, not inside the hook: destroying a D3D plugin
// from inside a WH_GETMESSAGE hook chain leaves a dangling DXGI hook (FL then dies in unloaded
// dxgi.dll). The hook only creates the window on FL's main thread.
#define WM_PROBE_CALL (WM_APP + 0x51)
static HWND g_callWnd;
static LRESULT CALLBACK call_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m != WM_PROBE_CALL) return DefWindowProcW(h, m, wp, lp);
    if (InterlockedExchange(&g_callPending, 0)) { do_call(); InterlockedExchange(&g_callDone, 1); }
    return 0;
}
static LRESULT CALLBACK hook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !g_callWnd) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = call_proc; wc.hInstance = g_inst; wc.lpszClassName = L"FlimProbeCalls";
        RegisterClassW(&wc);
        g_callWnd = CreateWindowExW(0, L"FlimProbeCalls", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_inst, NULL);
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static BOOL CALLBACK findw(HWND h, LPARAM lp) {
    DWORD pid; char c[64];
    GetWindowThreadProcessId(h, &pid);
    GetClassNameA(h, c, sizeof(c));
    if (pid == GetCurrentProcessId() && strcmp(c, "TFruityLoopsMainForm") == 0) { *(HWND *)lp = h; return FALSE; }
    return TRUE;
}

static void reply(HANDLE p, const char *s) { DWORD w; WriteFile(p, s, (DWORD)strlen(s), &w, NULL); WriteFile(p, "\n", 1, &w, NULL); }

static void handle(HANDLE p, char *line) {
    static char out[1 << 20];
    char *cmd = strtok(line, " "), *a = strtok(NULL, " ");
    if (!cmd) { reply(p, "?"); return; }
    if (!strcmp(cmd, "base")) { sprintf_s(out, sizeof(out), "%p", g_base); reply(p, out); return; }
    if (!strcmp(cmd, "r") && a) {
        unsigned char *src = (unsigned char *)parse_addr(a);
        char *n = strtok(NULL, " ");
        size_t cnt = n ? strtoul(n, NULL, 0) : 16, i;
        if (cnt > sizeof(out) / 2 - 2) cnt = sizeof(out) / 2 - 2;
        __try { for (i = 0; i < cnt; i++) sprintf_s(out + i * 2, 3, "%02x", src[i]); out[cnt * 2] = 0; }
        __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, sizeof(out), "ERR fault"); }
        reply(p, out); return;
    }
    if (!strcmp(cmd, "w") && a) {
        unsigned char *dst = (unsigned char *)parse_addr(a);
        char *h = strtok(NULL, " ");
        size_t i, n = h ? strlen(h) / 2 : 0;
        __try { for (i = 0; i < n; i++) { unsigned v; sscanf_s(h + i * 2, "%2x", &v); dst[i] = (unsigned char)v; } reply(p, "ok"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { reply(p, "ERR fault"); }
        return;
    }
    if (!strcmp(cmd, "ustr") && a) {
        wchar_t *s = (wchar_t *)parse_addr(a);
        __try {
            if (!s) strcpy_s(out, sizeof(out), "");
            else { int n = *(int *)((char *)s - 4); if (n < 0 || n > 100000) n = 0; { int k = n ? WideCharToMultiByte(CP_UTF8, 0, s, n, out, sizeof(out) - 1, NULL, NULL) : 0; out[k] = 0; } }
        } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(out, sizeof(out), "ERR fault"); }
        reply(p, out); return;
    }
    if (!strcmp(cmd, "mkstr")) {
        char *t = a ? a + strlen(a) + 1 : NULL;      // rest of line, spaces allowed
        char txt[1024] = "";
        if (a) { strcpy_s(txt, sizeof(txt), a); if (t && *t) { strcat_s(txt, sizeof(txt), " "); strcat_s(txt, sizeof(txt), t); } }
        int n = MultiByteToWideChar(CP_UTF8, 0, txt, -1, NULL, 0) - 1;
        unsigned char *m = (unsigned char *)calloc(1, 16 + (n + 1) * 2);
        *(uint16_t *)(m + 4) = 1200; *(uint16_t *)(m + 6) = 2; *(int32_t *)(m + 8) = -1; *(int32_t *)(m + 12) = n;
        MultiByteToWideChar(CP_UTF8, 0, txt, -1, (wchar_t *)(m + 16), n + 1);
        sprintf_s(out, sizeof(out), "%p", m + 16); reply(p, out); return;
    }
    if (!strcmp(cmd, "alloc") && a) { sprintf_s(out, sizeof(out), "%p", calloc(1, strtoul(a, NULL, 0))); reply(p, out); return; }
    if ((!strcmp(cmd, "call") || !strcmp(cmd, "callt")) && a) {
        int i; char *x;
        g_fn = parse_addr(a);
        for (i = 0; i < 8; i++) { x = strtok(NULL, " "); g_args[i] = x ? (x[0] == 'v' ? parse_addr(x) : strtoull(x, NULL, 0)) : 0; }
        if (!strcmp(cmd, "callt")) do_call();        // on the pipe thread
        else {
            DWORD t0 = GetTickCount();
            g_callDone = 0; g_callPending = 1;
            while (!g_callDone && GetTickCount() - t0 < 10000) {
                if (g_callWnd) PostMessageW(g_callWnd, WM_PROBE_CALL, 0, 0);
                else PostMessageW(g_wnd, WM_NULL, 0, 0);         // wakes the hook, which makes the window
                Sleep(5);
            }
            if (!g_callDone) { g_callPending = 0; reply(p, "ERR timeout"); return; }
        }
        sprintf_s(out, sizeof(out), "%s%llx", g_callErr[0] ? g_callErr : "", (unsigned long long)g_ret); reply(p, out); return;
    }
    reply(p, "ERR unknown");
}

static DWORD WINAPI server(LPVOID x) {
    char name[64], buf[65536];
    int tries;
    (void)x;
    for (tries = 0; tries < 600 && !g_wnd; tries++) { EnumWindows(findw, (LPARAM)&g_wnd); if (!g_wnd) Sleep(100); }
    g_base = (char *)GetModuleHandleW(L"FLEngine_x64.dll");
    if (g_wnd) g_hook = SetWindowsHookExW(WH_GETMESSAGE, hook, g_inst, GetWindowThreadProcessId(g_wnd, NULL));
    sprintf_s(name, sizeof(name), "\\\\.\\pipe\\flimprobe_%lu", GetCurrentProcessId());
    for (;;) {
        HANDLE p = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 1 << 21, 1 << 16, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) return 1;
        if (ConnectNamedPipe(p, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
            size_t len = 0; DWORD r;
            while (ReadFile(p, buf + len, (DWORD)(sizeof(buf) - 1 - len), &r, NULL) && r) {
                char *nl;
                len += r; buf[len] = 0;
                while ((nl = strchr(buf, '\n')) != NULL) {
                    *nl = 0; if (nl > buf && nl[-1] == '\r') nl[-1] = 0;
                    handle(p, buf);
                    len -= (nl + 1 - buf); memmove(buf, nl + 1, len + 1);
                }
            }
        }
        DisconnectNamedPipe(p); CloseHandle(p);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) {
    (void)x;
    if (r == DLL_PROCESS_ATTACH) g_inst = h;
    if (r == DLL_PROCESS_ATTACH) CreateThread(NULL, 0, server, NULL, 0, NULL);
    return TRUE;
}

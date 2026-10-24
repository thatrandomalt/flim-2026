// flim.exe: a window to join a session (starts FL, which connects) or host one on this PC (flim.exe
// itself is the server, see host.c). FL is started suspended, flim_engine.dll is loaded into it with
// LoadLibraryW remote-thread injection, then it resumes. On-disk FL is untouched.
// First run (or FL not found, or `flim.exe --setup`): a setup dialog asks for the
// FL Studio 2026 folder and a user name; both are saved in %APPDATA%\flim\flim.ini.

#include <windows.h>
#include <shobjidl.h>
#include <stdio.h>
#include "../common/flim_config.h"
#include "../common/flim_res.h"
#include "../common/flim_version.h"
#include "../common/flim_zip.h"
#include "host.h"
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dbghelp.lib")
#include <dbghelp.h>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#include <uxtheme.h>
#include <dwmapi.h>
#include <commctrl.h>
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t *DEFAULT_FL_DIR = L"C:\\Program Files\\Image-Line\\FL Studio 2026";

static void fail_box(HWND owner, const wchar_t *fmt, ...) {
    wchar_t msg[1024];
    va_list ap; va_start(ap, fmt); vswprintf_s(msg, 1024, fmt, ap); va_end(ap);
    MessageBoxW(owner, msg, L"flim", MB_OK | MB_ICONERROR);
}

// Accepts the install folder or the FL64.exe path itself; normalizes `dir` to the folder.
// Returns 1 if it holds FL Studio 2026 (FL64.exe + FLEngine_x64.dll, version 26.x).
static int check_fl_dir(wchar_t *dir, wchar_t *why, int whycap, wchar_t *ver, int vercap) {
    wchar_t f[MAX_PATH * 2];
    WORD v[4];
    size_t n;
    while ((n = wcslen(dir)) > 0 && (dir[n - 1] == L' ' || dir[n - 1] == L'"')) dir[n - 1] = 0;
    while (dir[0] == L' ' || dir[0] == L'"') memmove(dir, dir + 1, wcslen(dir) * sizeof(wchar_t));
    n = wcslen(dir);
    if (n > 4 && _wcsicmp(dir + n - 4, L".exe") == 0) { wchar_t *s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    while ((n = wcslen(dir)) > 3 && dir[n - 1] == L'\\') dir[n - 1] = 0;
    ver[0] = 0;
    if (!dir[0]) { swprintf_s(why, whycap, L"Pick the FL Studio 2026 folder."); return 0; }
    {
        DWORD a = GetFileAttributesW(dir);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            swprintf_s(why, whycap, L"That folder doesn't exist."); return 0;
        }
    }
    swprintf_s(f, MAX_PATH * 2, L"%s\\FL64.exe", dir);
    if (GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES) { swprintf_s(why, whycap, L"No FL64.exe in that folder."); return 0; }
    if (!file_version(f, v)) { swprintf_s(why, whycap, L"FL64.exe has no version info - not FL Studio?"); return 0; }
    swprintf_s(ver, vercap, L"%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    if (v[0] != FLIM_FL_MAJOR) {
        swprintf_s(why, whycap, L"FL64.exe there is version %s, not FL Studio 2026 (26.x).", ver); return 0;
    }
    swprintf_s(f, MAX_PATH * 2, L"%s\\FLEngine_x64.dll", dir);
    if (GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES) { swprintf_s(why, whycap, L"No FLEngine_x64.dll in that folder."); return 0; }
    if (file_version(f, v)) swprintf_s(ver, vercap, L"%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    why[0] = 0;
    return 1;
}

static void browse_folder(HWND dlg) {
    IFileOpenDialog *fd = NULL;
    IShellItem *it = NULL;
    wchar_t *path = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&fd)))
        return;
    fd->lpVtbl->SetOptions(fd, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->lpVtbl->SetTitle(fd, L"Select the FL Studio 2026 folder (contains FL64.exe)");
    if (SUCCEEDED(fd->lpVtbl->Show(fd, dlg)) && SUCCEEDED(fd->lpVtbl->GetResult(fd, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
            SetDlgItemTextW(dlg, IDC_FLDIR, path);
            CoTaskMemFree(path);
        }
        it->lpVtbl->Release(it);
    }
    fd->lpVtbl->Release(fd);
}

static void show_fl_info(HWND dlg) {
    wchar_t dir[MAX_PATH * 2], why[256], ver[64], line[320];
    GetDlgItemTextW(dlg, IDC_FLDIR, dir, MAX_PATH * 2);
    if (check_fl_dir(dir, why, 256, ver, 64)) swprintf_s(line, 320, L"\x2714 FL Studio %s found.", ver);
    else swprintf_s(line, 320, L"\x2716 %s", why);
    SetDlgItemTextW(dlg, IDC_FLINFO, line);
}

static void theme_init(HWND dlg);
static void theme_button(const DRAWITEMSTRUCT *di);
static void theme_listitem(const DRAWITEMSTRUCT *di);
static INT_PTR theme_color(HWND dlg, UINT m, HDC dc, HWND c);
static INT_PTR CALLBACK setup_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DRAWITEM && ((const DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON) { theme_button((const DRAWITEMSTRUCT *)lp); return TRUE; }
    if (m >= WM_CTLCOLORMSGBOX && m <= WM_CTLCOLORSTATIC) return theme_color(dlg, m, (HDC)wp, (HWND)lp);
    switch (m) {
    case WM_INITDIALOG: {
        wchar_t v[MAX_PATH * 2];
        HICON ic = LoadIconW(GetModuleHandleW(NULL), L"IDI_FLIM");
        theme_init(dlg);
        if (ic) { SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)ic); SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ic); }
        flimcfg_get(L"fl_dir", v, MAX_PATH * 2);
        if (!v[0]) wcscpy_s(v, MAX_PATH * 2, DEFAULT_FL_DIR);
        SetDlgItemTextW(dlg, IDC_FLDIR, v);
        flimcfg_get(L"name", v, MAX_PATH * 2);
        SetDlgItemTextW(dlg, IDC_NAME, v);
        SendDlgItemMessageW(dlg, IDC_NAME, EM_LIMITTEXT, 64, 0);
        show_fl_info(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE: browse_folder(dlg); return TRUE;
        case IDC_FLDIR: if (HIWORD(wp) == EN_CHANGE) show_fl_info(dlg); return TRUE;
        case IDOK: {
            wchar_t dir[MAX_PATH * 2], why[256], ver[64], raw[128], name[FLIM_NAME_MAX + 1];
            GetDlgItemTextW(dlg, IDC_FLDIR, dir, MAX_PATH * 2);
            if (!check_fl_dir(dir, why, 256, ver, 64)) {
                fail_box(dlg, L"%s", why);
                SetFocus(GetDlgItem(dlg, IDC_FLDIR));
                return TRUE;
            }
            GetDlgItemTextW(dlg, IDC_NAME, raw, 128);
            if (!flim_sanitize_name(raw, name, FLIM_NAME_MAX + 1)) {
                fail_box(dlg, L"Enter a name with at least one letter or digit.");
                SetFocus(GetDlgItem(dlg, IDC_NAME));
                return TRUE;
            }
            flimcfg_set(L"fl_dir", dir);
            flimcfg_set(L"name", name);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

static int config_ok(void) {
    wchar_t dir[MAX_PATH * 2], name[128], why[256], ver[64];
    flimcfg_get(L"fl_dir", dir, MAX_PATH * 2);
    flimcfg_get(L"name", name, 128);
    return name[0] && check_fl_dir(dir, why, 256, ver, 64);
}

// Start FL with the engine. url (optional): the engine connects to it right away, with pw.
// gui (optional): this window, which then gets FL's status and log. Returns FL's process handle
// (the caller closes it), NULL if FL didn't start.
static HANDLE launch(const wchar_t *flDir, const wchar_t *url, const wchar_t *pw, HWND gui) {
    wchar_t fl[MAX_PATH * 2], dll[MAX_PATH], gv[32];
    // the child inherits these (the engine reads FLIM_AUTOCONNECT / FLIM_PASSWORD / FLIM_GUI at start)
    SetEnvironmentVariableW(L"FLIM_AUTOCONNECT", url && url[0] ? url : NULL);
    SetEnvironmentVariableW(L"FLIM_PASSWORD", url && url[0] && pw ? pw : NULL);
    swprintf_s(gv, 32, L"%llx", (unsigned long long)(uintptr_t)gui);
    SetEnvironmentVariableW(L"FLIM_GUI", gui ? gv : NULL);

    // any FL 2026 build: the engine finds FL's functions by signature, and turns sync off (and says so
    // in this window) if it can't
    swprintf_s(fl, MAX_PATH * 2, L"%s\\FL64.exe", flDir);

    // engine DLL sits next to flim.exe
    GetModuleFileNameW(NULL, dll, MAX_PATH);
    wchar_t *slash = wcsrchr(dll, L'\\');
    if (slash) wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - dll), L"flim_engine.dll");
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) {
        fail_box(gui, L"flim_engine.dll was not found next to flim.exe:\n%s", dll);
        return NULL;
    }

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // FL's working directory = its install folder, as when started from its own shortcut
    if (!CreateProcessW(fl, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, flDir, &si, &pi)) {
        SetEnvironmentVariableW(L"FLIM_PASSWORD", NULL);
        fail_box(gui, L"Could not start FL Studio (error %lu):\n%s", GetLastError(), fl);
        return NULL;
    }
    SetEnvironmentVariableW(L"FLIM_PASSWORD", NULL);     // FL has its copy

    SIZE_T nbytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void *remote = VirtualAllocEx(pi.hProcess, NULL, nbytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote || !WriteProcessMemory(pi.hProcess, remote, dll, nbytes, NULL)) {
        fail_box(gui, L"Could not load the flim engine into FL (alloc/write error %lu).", GetLastError());
        TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return NULL;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE loadlib = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW");
    HANDLE th = CreateRemoteThread(pi.hProcess, NULL, 0, loadlib, remote, 0, NULL);
    if (!th) {
        fail_box(gui, L"Could not load the flim engine into FL (CreateRemoteThread error %lu).", GetLastError());
        TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return NULL;
    }
    WaitForSingleObject(th, 15000);

    ResumeThread(pi.hThread);

    VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
    CloseHandle(th); CloseHandle(pi.hThread);
    return pi.hProcess;
}

// ---- an FL that was opened without flim ----
// Started from its own shortcut, FL has no engine. Join / Connect then loads the engine into it (the
// same LoadLibraryW remote thread as launch(), just later) instead of starting a second FL; the
// engine settles, its task window appears and gets the connect from this window. Also the way round
// FL starting badly with the engine already in it (reported under Wine).
static DWORD plain_fl_pid(void) {                  // an FL main window whose process has no engine
    HWND w = NULL;
    while ((w = FindWindowExW(NULL, w, L"TFruityLoopsMainForm", NULL)) != NULL) {
        DWORD pid = 0, ep = 0;
        HWND e = NULL;
        int has = 0;
        GetWindowThreadProcessId(w, &pid);
        while (!has && (e = FindWindowExW(HWND_MESSAGE, e, L"FlimTasks", NULL)) != NULL) { GetWindowThreadProcessId(e, &ep); has = ep == pid; }
        if (!has) return pid;
    }
    return 0;
}
static HWND engine_window_of(DWORD pid) {
    HWND e = NULL;
    while ((e = FindWindowExW(HWND_MESSAGE, e, L"FlimTasks", NULL)) != NULL) {
        DWORD ep = 0;
        GetWindowThreadProcessId(e, &ep);
        if (ep == pid) return e;
    }
    return NULL;
}
// Load the engine into a running FL. Returns its process handle (the caller tracks it) or NULL.
static HANDLE attach_engine(DWORD pid, HWND gui) {
    wchar_t dll[MAX_PATH], *slash;
    HANDLE p, th;
    void *remote;
    SIZE_T nbytes;
    GetModuleFileNameW(NULL, dll, MAX_PATH);
    if ((slash = wcsrchr(dll, L'\\')) != NULL) wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - dll), L"flim_engine.dll");
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { fail_box(gui, L"flim_engine.dll was not found next to flim.exe:\n%s", dll); return NULL; }
    p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION |
                    PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
    if (!p) { fail_box(gui, L"Could not open the running FL (error %lu). Close it and Join again: flim then starts FL itself.", GetLastError()); return NULL; }
    nbytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    remote = VirtualAllocEx(p, NULL, nbytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    th = remote && WriteProcessMemory(p, remote, dll, nbytes, NULL)
             ? CreateRemoteThread(p, NULL, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"), remote, 0, NULL)
             : NULL;
    if (!th) {
        fail_box(gui, L"Could not load the flim engine into the running FL (error %lu).", GetLastError());
        if (remote) VirtualFreeEx(p, remote, 0, MEM_RELEASE);
        CloseHandle(p);
        return NULL;
    }
    WaitForSingleObject(th, 15000);
    VirtualFreeEx(p, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    return p;
}

// ---------------- main window: join or host, and FL's status ----------------
// Join: FL starts and connects to the address; while our FL runs, the button connects or
// disconnects it instead. Host: flim.exe itself is the server (host.c), on this PC's port,
// optionally opened on the router with UPnP; the window has to stay open while hosting (closing
// it ends the session for everyone). The window stays open with FL too: the engine reports its
// status (connection, who is where, sample transfers) and a log here, and takes Connect /
// Disconnect from here (WM_COPYDATA: the engine's "flim window link"). Nothing of flim is drawn
// into FL's own UI.
#define WM_UPNP_DONE (WM_APP + 0x50)     // wParam: 1 = port opened; lParam: malloc'd public address (or NULL)
#define WM_FL_EXITED (WM_APP + 0x51)     // wParam: the process handle that ended
#define WM_ALERT     (WM_APP + 0x52)     // lParam: malloc'd text for a popup (receiver frees)
static HINSTANCE g_inst;
static int g_hostPort, g_upnpOn;
static wchar_t g_lan[64];
static HANDLE g_flProc;                  // our FL (one at a time)
static HWND g_engine;                    // its engine (task window), once it has reported
static int g_netState = NET_OFF;         // as it last reported
static DWORD g_flPid;                    // our FL (the last one: Export log takes its lines)

static int fl_running(void) { return g_flProc && WaitForSingleObject(g_flProc, 0) == WAIT_TIMEOUT; }
static DWORD WINAPI fl_watch(LPVOID p) {
    HANDLE *a = (HANDLE *)p;              // { process (our duplicate), window, id }
    WaitForSingleObject(a[0], INFINITE);
    PostMessageW((HWND)a[1], WM_FL_EXITED, (WPARAM)a[2], 0);
    CloseHandle(a[0]);
    free(a);
    return 0;
}
// From now on proc (owned) is our FL: its exit is reported to the window, and a hang of it saved.
static DWORD WINAPI hang_watch(LPVOID p);
static void fl_track(HWND dlg, HANDLE proc) {
    HANDLE *a = (HANDLE *)malloc(3 * sizeof(HANDLE)), *h = (HANDLE *)malloc(2 * sizeof(HANDLE));
    if (g_flProc) CloseHandle(g_flProc);
    g_flProc = proc;
    g_flPid = GetProcessId(proc);
    a[1] = (HANDLE)dlg; a[2] = proc;
    if (!DuplicateHandle(GetCurrentProcess(), proc, GetCurrentProcess(), &a[0], SYNCHRONIZE, FALSE, 0)) free(a);
    else CloseHandle(CreateThread(NULL, 0, fl_watch, a, 0, NULL));
    h[1] = (HANDLE)dlg;
    if (!DuplicateHandle(GetCurrentProcess(), proc, GetCurrentProcess(), &h[0],
                         SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, 0)) free(h);
    else CloseHandle(CreateThread(NULL, 0, hang_watch, h, 0, NULL));
}
static int engine_send(HWND dlg, int kind, const char *text) {
    COPYDATASTRUCT cd;
    DWORD_PTR r = 0;
    cd.dwData = (ULONG_PTR)kind; cd.cbData = text ? (DWORD)strlen(text) + 1 : 0; cd.lpData = (void *)text;
    return g_engine && SendMessageTimeoutW(g_engine, WM_COPYDATA, (WPARAM)dlg, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r) != 0;
}
// A flim FL that already runs (this window was closed and opened again) reports here from now on.
static int adopt_engine(HWND dlg, HWND eng) {
    DWORD pid = 0;
    HANDLE p;
    if (!eng) return 0;
    GetWindowThreadProcessId(eng, &pid);
    if (!(p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, pid))) return 0;
    fl_track(dlg, p);
    g_engine = eng;
    engine_send(dlg, GUI_HELLO, NULL);
    return 1;
}
static void adopt_running_fl(HWND dlg) { adopt_engine(dlg, FindWindowExW(HWND_MESSAGE, NULL, L"FlimTasks", NULL)); }

// ---- which FL (flim.ini auto_attach=0): Join lists the running FLs instead of taking the first one ----
#define PICK_MAX 16
static DWORD g_pickPid[PICK_MAX];
static int g_pickN, g_pickSel;
static INT_PTR CALLBACK pick_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DRAWITEM && ((const DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON) { theme_button((const DRAWITEMSTRUCT *)lp); return TRUE; }
    if (m == WM_DRAWITEM && ((const DRAWITEMSTRUCT *)lp)->CtlType == ODT_LISTBOX) { theme_listitem((const DRAWITEMSTRUCT *)lp); return TRUE; }
    if (m == WM_MEASUREITEM && ((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_LISTBOX) {   // a row: the font's height, padded
        HDC dc = GetDC(dlg);
        HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0));
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, of); ReleaseDC(dlg, dc);
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = (UINT)(tm.tmHeight + tm.tmHeight / 2);
        return TRUE;
    }
    if (m >= WM_CTLCOLORMSGBOX && m <= WM_CTLCOLORSTATIC) return theme_color(dlg, m, (HDC)wp, (HWND)lp);
    if (m == WM_INITDIALOG) {
        HWND w = NULL, list = GetDlgItem(dlg, IDC_P_LIST);
        theme_init(dlg);
        g_pickN = 0;
        while (g_pickN < PICK_MAX && (w = FindWindowExW(NULL, w, L"TFruityLoopsMainForm", NULL)) != NULL) {
            DWORD pid = 0;
            int k, hh = -1, mm = 0;
            wchar_t t[160], line[320];
            FILETIME c, x, kt, ut, lc;
            SYSTEMTIME st;
            HANDLE p;
            GetWindowThreadProcessId(w, &pid);
            for (k = 0; k < g_pickN && g_pickPid[k] != pid; k++) {}
            if (k < g_pickN) continue;
            if (!GetWindowTextW(w, t, 160)) wcscpy_s(t, 160, L"FL Studio");
            if ((p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) != NULL) {
                if (GetProcessTimes(p, &c, &x, &kt, &ut) && FileTimeToLocalFileTime(&c, &lc) && FileTimeToSystemTime(&lc, &st)) { hh = st.wHour; mm = st.wMinute; }
                CloseHandle(p);
            }
            if (hh >= 0) swprintf_s(line, 320, L"%s  (process %lu, started %02d:%02d)%s", t, pid, hh, mm, engine_window_of(pid) ? L"  - flim is in it" : L"");
            else swprintf_s(line, 320, L"%s  (process %lu)%s", t, pid, engine_window_of(pid) ? L"  - flim is in it" : L"");
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line);
            g_pickPid[g_pickN++] = pid;
        }
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)L"Open a new FL Studio");
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    if (m == WM_COMMAND) {
        int id = LOWORD(wp);
        if (id == IDC_P_LIST && HIWORD(wp) == LBN_DBLCLK) id = IDOK;
        if (id == IDOK) { g_pickSel = (int)SendDlgItemMessageW(dlg, IDC_P_LIST, LB_GETCURSEL, 0, 0); EndDialog(dlg, IDOK); return TRUE; }
        if (id == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
    }
    return FALSE;
}
// The FL process to use: its id, 0 = open a new FL, -1 = cancelled.
static long pick_fl(HWND dlg) {
    if (DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PICKFL), dlg, pick_proc) != IDOK || g_pickSel < 0) return -1;
    return g_pickSel < g_pickN ? (long)g_pickPid[g_pickSel] : 0;
}
static void session_buttons(HWND dlg) {
    int on = g_netState == NET_ON || g_netState == NET_CONNECTING, fl = fl_running();
    SetDlgItemTextW(dlg, IDC_J_JOIN, !fl ? L"Join" : on ? L"Disconnect" : L"Connect");
    EnableWindow(GetDlgItem(dlg, IDC_M_OPENFL), !fl);
    EnableWindow(GetDlgItem(dlg, IDC_S_RESYNC), fl && g_netState == NET_ON);
}

static void wide_to_utf8(const wchar_t *w, char *out, int cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}
static void secret_get_w(const wchar_t *key, wchar_t *out, int cap) {
    char u[512];
    flimcfg_get_secret(key, u, sizeof(u));
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, cap);
    SecureZeroMemory(u, sizeof(u));
}
static void secret_set_w(const wchar_t *key, const wchar_t *w) {
    char u[512];
    wide_to_utf8(w, u, sizeof(u));
    flimcfg_set_secret(key, u[0] ? u : NULL);
    SecureZeroMemory(u, sizeof(u));
}
static void main_flinfo(HWND dlg) {
    wchar_t dir[MAX_PATH * 2], why[256], ver[64], line[MAX_PATH * 2 + 80];
    flimcfg_get(L"fl_dir", dir, MAX_PATH * 2);
    if (check_fl_dir(dir, why, 256, ver, 64)) swprintf_s(line, MAX_PATH * 2 + 80, L"FL Studio %s  \x00B7  %s", ver, dir);
    else swprintf_s(line, MAX_PATH * 2 + 80, L"\x2716 %s", why);
    SetDlgItemTextW(dlg, IDC_M_FLINFO, line);
}
// The song name box -> flim.ini (empty = none). An FL that leads a session with an untitled
// project saves it under this name (the engine's leader_name_project).
static void main_save_song(HWND dlg) {
    wchar_t s[128];
    size_t n;
    GetDlgItemTextW(dlg, IDC_M_SONG, s, 128);
    for (n = wcslen(s); n && (s[n - 1] == L' ' || s[n - 1] == L'.'); ) s[--n] = 0;
    flimcfg_set(L"song_name", s[0] && s[0] != L' ' ? s : NULL);
}
// The name box -> flim.ini. 0 (and a message) if it has nothing usable.
static int main_save_name(HWND dlg) {
    wchar_t raw[128], name[FLIM_NAME_MAX + 1];
    GetDlgItemTextW(dlg, IDC_M_NAME, raw, 128);
    if (!flim_sanitize_name(raw, name, FLIM_NAME_MAX + 1)) {
        fail_box(dlg, L"Enter a name with at least one letter or digit.");
        SetFocus(GetDlgItem(dlg, IDC_M_NAME));
        return 0;
    }
    flimcfg_set(L"name", name);
    SetDlgItemTextW(dlg, IDC_M_NAME, name);
    main_save_song(dlg);
    return 1;
}
static int main_launch(HWND dlg, const wchar_t *url, const wchar_t *pw) {
    wchar_t dir[MAX_PATH * 2], why[256], ver[64];
    HANDLE p;
    flimcfg_get(L"fl_dir", dir, MAX_PATH * 2);
    if (!check_fl_dir(dir, why, 256, ver, 64)) { fail_box(dlg, L"%s\n\nPick your FL Studio folder with \"FL folder...\".", why); return 0; }
    if (!(p = launch(dir, url, pw, dlg))) return 0;
    fl_track(dlg, p);
    g_engine = NULL;
    g_netState = NET_OFF;
    SetDlgItemTextW(dlg, IDC_S_STATUS, L"FL is starting...");
    session_buttons(dlg);
    return 1;
}
// One log for FL's events and the host's, newest at the bottom: a read-only text box, so any of it can
// be selected and copied (Ctrl+C, right-click > Copy, Ctrl+A for all). A new line doesn't move a view
// scrolled up to read, nor a selection. The oldest quarter goes once it holds LOG_KEEP_CHARS.
#define LOG_KEEP_CHARS 500000
static void main_log(HWND dlg, const wchar_t *line) {
    HWND ed = GetDlgItem(dlg, IDC_S_LOG);
    wchar_t s[1100];
    SYSTEMTIME t;
    DWORD a = 0, b = 0;
    SCROLLINFO si;
    int len = GetWindowTextLengthW(ed), first = (int)SendMessageW(ed, EM_GETFIRSTVISIBLELINE, 0, 0), hpos = GetScrollPos(ed, SB_HORZ), follow;
    si.cbSize = sizeof(si); si.fMask = SIF_ALL;
    follow = !GetScrollInfo(ed, SB_VERT, &si) || si.nPos + (int)si.nPage > si.nMax;    // at the bottom (or not scrolling yet)
    GetLocalTime(&t);
    swprintf_s(s, 1100, L"%s%02d:%02d:%02d  %s", len ? L"\r\n" : L"", t.wHour, t.wMinute, t.wSecond, line);
    SendMessageW(ed, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    SendMessageW(ed, WM_SETREDRAW, FALSE, 0);
    SendMessageW(ed, EM_SETSEL, len, len);
    SendMessageW(ed, EM_REPLACESEL, FALSE, (LPARAM)s);
    if (len > LOG_KEEP_CHARS) {
        int cut = (int)SendMessageW(ed, EM_LINEINDEX, SendMessageW(ed, EM_GETLINECOUNT, 0, 0) / 4, 0);
        int lines = (int)SendMessageW(ed, EM_LINEFROMCHAR, cut, 0);
        SendMessageW(ed, EM_SETSEL, 0, cut);
        SendMessageW(ed, EM_REPLACESEL, FALSE, (LPARAM)L"");
        a = a > (DWORD)cut ? a - cut : 0;
        b = b > (DWORD)cut ? b - cut : 0;
        first = first > lines ? first - lines : 0;
    }
    SendMessageW(ed, EM_SETSEL, a, b);
    if (follow) SendMessageW(ed, WM_VSCROLL, SB_BOTTOM, 0);
    else SendMessageW(ed, EM_LINESCROLL, 0, first - (int)SendMessageW(ed, EM_GETFIRSTVISIBLELINE, 0, 0));
    SendMessageW(ed, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, hpos), 0);
    SendMessageW(ed, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ed, NULL, TRUE);
}
static void crash_dir(wchar_t *out, int cap) { flimcfg_flim_dir(out, cap); wcscat_s(out, cap, L"\\Crash reports"); }
static void open_folder(const wchar_t *d) {
    SHCreateDirectoryExW(NULL, d, NULL);
    ShellExecuteW(NULL, L"open", d, NULL, NULL, SW_SHOWNORMAL);
}

// ---- "Clear cache...": what flim can make again (README "Where flim keeps things") ----
// The plugin sample links (Documents\flim\samples), the built-in host's sample store and the engines'
// scratch files (%TEMP%\flim). Never songs, backups, the shared folder or settings.
static void cache_dirs(wchar_t d[3][MAX_PATH]) {
    wchar_t *la = NULL;
    flimcfg_flim_dir(d[0], MAX_PATH); wcscat_s(d[0], MAX_PATH, L"\\samples");
    d[1][0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la))) swprintf_s(d[1], MAX_PATH, L"%s\\flim\\host-samples", la);
    CoTaskMemFree(la);
    GetTempPathW(MAX_PATH, d[2]); wcscat_s(d[2], MAX_PATH, L"flim");
}
// Bytes under dir; with del, its contents are deleted too (dir itself stays). Files in use are left.
static unsigned long long tree_walk(const wchar_t *dir, int del) {
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW f;
    HANDLE h;
    unsigned long long n = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &f)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(f.cFileName, L".") || !wcscmp(f.cFileName, L"..")) continue;
        swprintf_s(p, MAX_PATH, L"%s\\%s", dir, f.cFileName);
        if (f.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (del) RemoveDirectoryW(p); continue; }   // (never follow a link)
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { n += tree_walk(p, del); if (del) RemoveDirectoryW(p); }
        else {
            n += ((unsigned long long)f.nFileSizeHigh << 32) | f.nFileSizeLow;
            if (del) { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        }
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return n;
}
static void clear_cache(HWND dlg) {
    wchar_t d[3][MAX_PATH], msg[256];
    unsigned long long n = 0, left = 0;
    int i;
    if (FindWindowExW(NULL, NULL, L"TFruityLoopsMainForm", NULL) || host_running()) {
        fail_box(dlg, L"Close FL Studio%s first: a session may still be using the cache.", host_running() ? L" and stop hosting" : L"");
        return;
    }
    cache_dirs(d);
    for (i = 0; i < 3; i++) if (d[i][0]) n += tree_walk(d[i], 0);
    if (!n) { main_log(dlg, L"Clear cache: the cache is already empty."); return; }
    swprintf_s(msg, 256, L"Delete flim's cache (%.1f MB)?\n\nflim makes it again when it needs it. Your songs, backups, the shared "
                         L"folder and your settings aren't touched.", n / 1048576.0);
    if (MessageBoxW(dlg, msg, L"Clear cache", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK) return;
    for (i = 0; i < 3; i++) if (d[i][0]) { tree_walk(d[i], 1); left += tree_walk(d[i], 0); }
    swprintf_s(msg, 256, L"Clear cache: freed %.1f MB%s.", (n - left) / 1048576.0, left ? L" (some files were in use and stay)" : L"");
    main_log(dlg, msg);
}
// "Change...": another projects folder (song folders made from now on go there; old ones stay).
static void pick_projects_dir(HWND dlg) {
    IFileOpenDialog *fd = NULL;
    IShellItem *it = NULL, *cur = NULL;
    wchar_t *path = NULL, now[MAX_PATH * 2], msg[MAX_PATH * 2 + 120];
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&fd))) return;
    flimcfg_projects_dir(now, MAX_PATH * 2);
    SHCreateDirectoryExW(NULL, now, NULL);
    fd->lpVtbl->SetOptions(fd, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    fd->lpVtbl->SetTitle(fd, L"Where should flim keep your songs? (one folder per song goes in here)");
    if (SUCCEEDED(SHCreateItemFromParsingName(now, NULL, &IID_IShellItem, (void **)&cur))) { fd->lpVtbl->SetFolder(fd, cur); cur->lpVtbl->Release(cur); }
    if (SUCCEEDED(fd->lpVtbl->Show(fd, dlg)) && SUCCEEDED(fd->lpVtbl->GetResult(fd, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &path))) {
            flimcfg_set(L"projects_dir", path);
            swprintf_s(msg, MAX_PATH * 2 + 120, L"Projects folder: %s (each song gets its own folder in it)", path);
            main_log(dlg, msg);
            CoTaskMemFree(path);
        }
        it->lpVtbl->Release(it);
    }
    fd->lpVtbl->Release(fd);
}

// ---- crash reports ----
// When our FL ends in a crash (an exception exit code, FL's own crash log, or flim's dump of a jump
// into unloaded code) everything that helps goes into Documents\flim\crash reports\FL crash
// <date time>.zip: FL's crash logs and its startup log from that run (FL's user data folder,
// Support\Logs), flim's log lines for that FL and flim's dump.
typedef struct { wchar_t *src[64], *name[64]; int n; } FileList;
static void list_add(FileList *L, const wchar_t *src, const wchar_t *name) {
    if (L->n < 64) { L->src[L->n] = _wcsdup(src); L->name[L->n] = name ? _wcsdup(name) : NULL; L->n++; }
}
// Files in dir newer than t (newest only: just the newest one).
static void add_newer(FileList *L, const wchar_t *dir, const FILETIME *t, int newest) {
    wchar_t pat[MAX_PATH * 2], best[MAX_PATH * 2] = L"";
    WIN32_FIND_DATAW fd;
    FILETIME bt = *t;
    HANDLE h;
    swprintf_s(pat, MAX_PATH * 2, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || CompareFileTime(&fd.ftLastWriteTime, newest ? &bt : t) <= 0) continue;
        swprintf_s(pat, MAX_PATH * 2, L"%s\\%s", dir, fd.cFileName);
        if (newest) { wcscpy_s(best, MAX_PATH * 2, pat); bt = fd.ftLastWriteTime; }
        else list_add(L, pat, NULL);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (newest && best[0]) list_add(L, best, NULL);
}
// flim's lines for one FL (the log is shared by every FL) -> a file of their own.
static int engine_log_of(DWORD pid, DWORD code, const wchar_t *out) {
    wchar_t tmp[MAX_PATH], src[MAX_PATH];
    FILE *in = NULL, *o = NULL;
    char line[8192], tag[32];
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(src, MAX_PATH, L"%sflim_engine.log", tmp);
    sprintf_s(tag, sizeof(tag), "[%lu]", pid);
    if (_wfopen_s(&in, src, L"rb") != 0 || !in) return 0;
    if (_wfopen_s(&o, out, L"wb") != 0 || !o) { fclose(in); return 0; }
    if (code) fprintf(o, "FL (pid %lu) exit code 0x%08lX\r\n", pid, code);
    while (fgets(line, sizeof(line), in)) if (strstr(line, tag)) fputs(line, o);
    fclose(o);
    fclose(in);
    return 1;
}

// ---- hang reports ----
// FL "not responding" for HANG_REPORT_S: a small dump of it (every thread's stack and what it points
// at, so a deadlock can be read) and flim's log go into Documents\flim\Crash reports\FL hang
// <date time>.zip. Once per hang; FL may well come back. The dump stays in %TEMP% until FL exits: if it
// is closed while not responding, it goes into that report too (people close it long before 20 s).
#define HANG_REPORT_S 8
static DWORD WINAPI hang_watch(LPVOID p) {
    HANDLE *a = (HANDLE *)p;                  // { process (ours to close), window }
    HANDLE proc = a[0];
    HWND dlg = (HWND)a[1];
    DWORD pid = GetProcessId(proc);
    int hungFor = 0, reported = 0;
    free(a);
    while (WaitForSingleObject(proc, 2000) == WAIT_TIMEOUT) {
        HWND w = NULL;
        while ((w = FindWindowExW(NULL, w, L"TFruityLoopsMainForm", NULL)) != NULL) {
            DWORD wp = 0;
            GetWindowThreadProcessId(w, &wp);
            if (wp == pid) break;
        }
        {   // answers a no-op message within 3 s? (IsHungAppWindow doesn't notice a stuck FL)
            DWORD_PTR r;
            DWORD t0 = GetTickCount();
            if (!w || SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, 3000, &r)) { hungFor = 0; reported = 0; continue; }
            hungFor += 2 + (int)((GetTickCount() - t0) / 1000);
        }
        if (hungFor >= HANG_REPORT_S && !reported) {
            wchar_t dir[MAX_PATH * 2], dmp[MAX_PATH * 2], log[MAX_PATH], zip[MAX_PATH * 2], tmp[MAX_PATH];
            const wchar_t *src[2], *names[2] = { L"FL64.dmp", L"flim_engine.log" };
            SYSTEMTIME t;
            HANDLE f;
            int n = 0;
            reported = 1;
            GetTempPathW(MAX_PATH, tmp);
            swprintf_s(dmp, MAX_PATH * 2, L"%sflim_hang_%lu.dmp", tmp, pid);
            swprintf_s(log, MAX_PATH, L"%sflim_hang_%lu.txt", tmp, pid);
            f = CreateFileW(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                if (MiniDumpWriteDump(proc, pid, f, (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                      MiniDumpWithUnloadedModules | MiniDumpWithHandleData), NULL, NULL, NULL)) src[n++] = dmp;
                CloseHandle(f);
            }
            if (engine_log_of(pid, 0, log)) { if (!n) names[0] = names[1]; src[n++] = log; }
            crash_dir(dir, MAX_PATH * 2);
            SHCreateDirectoryExW(NULL, dir, NULL);
            GetLocalTime(&t);
            swprintf_s(zip, MAX_PATH * 2, L"%s\\FL hang %04d-%02d-%02d %02d%02d%02d.zip", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            if (n && flim_zip(zip, n, src, names) > 0) {
                wchar_t *msg = (wchar_t *)malloc(sizeof(wchar_t) * (MAX_PATH * 2 + 200));
                swprintf_s(msg, MAX_PATH * 2 + 200, L"FL has not responded for %d s. Saved a hang report: %s (\"Crash reports\" opens the folder)", hungFor, zip);
                PostMessageW(dlg, WM_HOST_LOG, 0, (LPARAM)msg);      // the log, as the host's lines are
            }
            DeleteFileW(log);                         // (the dump: crash_collect)
        }
    }
    CloseHandle(proc);
    return 0;
}

static void crash_collect(HWND dlg, HANDLE proc) {
    FILETIME start, e, k, u;
    DWORD code = 0, pid = GetProcessId(proc), sz = sizeof(wchar_t) * MAX_PATH;
    wchar_t ud[MAX_PATH], dir[MAX_PATH * 2], tmp[MAX_PATH], log[MAX_PATH], zip[MAX_PATH * 2], msg[MAX_PATH * 3];
    wchar_t hang[MAX_PATH];
    FileList L;
    SYSTEMTIME t;
    int i, n, crashed, closedHung;
    memset(&L, 0, sizeof(L));
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(hang, MAX_PATH, L"%sflim_hang_%lu.dmp", tmp, GetProcessId(proc));
    if (!GetExitCodeProcess(proc, &code) || !GetProcessTimes(proc, &start, &e, &k, &u)) { DeleteFileW(hang); return; }
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Image-Line\\Shared\\Paths", L"Shared data", RRF_RT_REG_SZ, NULL, ud, &sz) != ERROR_SUCCESS) {
        SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, ud);
        wcscat_s(ud, MAX_PATH, L"\\Image-Line");
    }
    if (ud[0] && ud[wcslen(ud) - 1] == L'\\') ud[wcslen(ud) - 1] = 0;
    swprintf_s(dir, MAX_PATH * 2, L"%s\\FL Studio\\Support\\Logs\\Crash", ud);
    add_newer(&L, dir, &start, 0);
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(dir, MAX_PATH * 2, L"%sflim_crash_%lu.dmp", tmp, pid);
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) list_add(&L, dir, NULL);
    // 0xCFFFFFFF: Windows closed FL while it wasn't responding ("Close the program"); Task Manager gives 1
    closedHung = code == 0xCFFFFFFFu;
    crashed = L.n > 0 || (code >= 0xC0000000u && code < 0xD0000000u);      // an exception code
    if (crashed) {
        swprintf_s(dir, MAX_PATH * 2, L"%s\\FL Studio\\Support\\Logs\\Startup", ud);
        add_newer(&L, dir, &start, 1);
        swprintf_s(log, MAX_PATH, L"%sflim_log_%lu.txt", tmp, pid);
        if (engine_log_of(pid, code, log)) list_add(&L, log, L"flim_engine.log");
        if (GetFileAttributesW(hang) != INVALID_FILE_ATTRIBUTES) list_add(&L, hang, L"FL64 not responding.dmp");
        crash_dir(dir, MAX_PATH * 2);
        SHCreateDirectoryExW(NULL, dir, NULL);
        GetLocalTime(&t);
        swprintf_s(zip, MAX_PATH * 2, L"%s\\FL %s %04d-%02d-%02d %02d%02d%02d.zip", dir, closedHung ? L"not responding" : L"crash",
                   t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        n = flim_zip(zip, L.n, (const wchar_t *const *)L.src, (const wchar_t *const *)L.name);
        DeleteFileW(log);
        if (n > 0) swprintf_s(msg, MAX_PATH * 3, L"%s (exit code 0x%08lX). Report: %s  (\"Crash reports\" opens the folder: send it to whoever helps you with flim)",
                              closedHung ? L"FL stopped responding and was closed" : L"FL crashed", code, zip);
        else swprintf_s(msg, MAX_PATH * 3, L"%s (exit code 0x%08lX). The report could not be written.", closedHung ? L"FL stopped responding and was closed" : L"FL crashed", code);
        main_log(dlg, msg);
    }
    DeleteFileW(hang);
    for (i = 0; i < L.n; i++) { free(L.src[i]); free(L.name[i]); }
}

// ---- Export log ----
// What helps find out what went wrong in a session, as one zip to send: flim's log lines of our FL (the
// end of the whole log file if there's no FL of ours to pick out), this window's log, the built-in
// host's log, and the versions and settings (never a password). Documents\flim\Logs\flim log <time>.zip.
static int file_tail(const wchar_t *src, const wchar_t *dst, long long keep) {   // the last `keep` bytes, from a line start
    FILE *in = NULL, *o = NULL;
    char buf[65536];
    size_t n;
    long long sz;
    if (_wfopen_s(&in, src, L"rb") != 0 || !in) return 0;
    _fseeki64(in, 0, SEEK_END);
    sz = _ftelli64(in);
    _fseeki64(in, sz > keep ? sz - keep : 0, SEEK_SET);
    if (sz > keep) { int c; while ((c = fgetc(in)) != EOF && c != '\n') {} }
    if (_wfopen_s(&o, dst, L"wb") != 0 || !o) { fclose(in); return 0; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, o);
    fclose(o); fclose(in);
    return 1;
}
static int write_utf8(const wchar_t *path, const wchar_t *text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *u = (char *)malloc(n > 0 ? n : 1);
    FILE *o = NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, text, -1, u, n, NULL, NULL) || _wfopen_s(&o, path, L"wb") != 0 || !o) { free(u); return 0; }
    fwrite("\xEF\xBB\xBF", 1, 3, o);
    fwrite(u, 1, (size_t)n - 1, o);
    fclose(o);
    free(u);
    return 1;
}
static void export_log(HWND dlg) {
    wchar_t tmp[MAX_PATH], src[MAX_PATH], eng[MAX_PATH], win[MAX_PATH], info[MAX_PATH], host[MAX_PATH], dir[MAX_PATH * 2], zip[MAX_PATH * 2];
    wchar_t msg[MAX_PATH * 3], *text, fl[600], name[128], url[600], proj[MAX_PATH * 2], prod[128] = L"", ver[64] = L"", build[32] = L"";
    FileList L;
    SYSTEMTIME t;
    DWORD sz;
    int i, n, len, on, bmin = flimcfg_backup_min(&on);
    memset(&L, 0, sizeof(L));
    GetTempPathW(MAX_PATH, tmp);
    GetLocalTime(&t);
    swprintf_s(src, MAX_PATH, L"%sflim_engine.log", tmp);
    swprintf_s(eng, MAX_PATH, L"%sflim_export_engine.log", tmp);
    if ((g_flPid && engine_log_of(g_flPid, 0, eng)) || file_tail(src, eng, 16 << 20)) list_add(&L, eng, L"flim_engine.log");
    len = GetWindowTextLengthW(GetDlgItem(dlg, IDC_S_LOG));
    text = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    GetDlgItemTextW(dlg, IDC_S_LOG, text, len + 1);
    swprintf_s(win, MAX_PATH, L"%sflim_export_window.log", tmp);
    if (write_utf8(win, text)) list_add(&L, win, L"flim_window.log");
    free(text);
    swprintf_s(host, MAX_PATH, L"%sflim_host.log", tmp);
    if (GetFileAttributesW(host) != INVALID_FILE_ATTRIBUTES) list_add(&L, host, L"flim_host.log");
    sz = sizeof(prod); RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName", RRF_RT_REG_SZ, NULL, prod, &sz);
    sz = sizeof(ver); RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", RRF_RT_REG_SZ, NULL, ver, &sz);
    sz = sizeof(build); RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", RRF_RT_REG_SZ, NULL, build, &sz);
    GetDlgItemTextW(dlg, IDC_M_FLINFO, fl, 600);
    flimcfg_get(L"name", name, 128);
    flimcfg_get(L"server_url", url, 600);
    flimcfg_projects_dir(proj, MAX_PATH * 2);
    text = (wchar_t *)malloc(4096 * sizeof(wchar_t));
    swprintf_s(text, 4096,
        L"flim v.%s\r\n%s\r\n%s %s, build %s\r\nExported %04d-%02d-%02d %02d:%02d:%02d (this PC's time)\r\n"
        L"FL: %s (pid %lu)\r\nDetailed log: %s\r\nName: %s\r\nServer: %s\r\nProjects folder: %s\r\n"
        L"Plugin lock: %d s; project backups: %s, every %d min\r\n",
        FLIM_VERSION_W, fl, prod, ver, build, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        fl_running() ? L"running" : L"not running", g_flPid, flimcfg_log_detailed() ? L"on" : L"off", name, url, proj,
        flimcfg_lock_s(), on ? L"on" : L"off", bmin);
    swprintf_s(info, MAX_PATH, L"%sflim_export_info.txt", tmp);
    if (write_utf8(info, text)) list_add(&L, info, L"info.txt");
    free(text);
    flimcfg_flim_dir(dir, MAX_PATH * 2);
    wcscat_s(dir, MAX_PATH * 2, L"\\Logs");
    SHCreateDirectoryExW(NULL, dir, NULL);
    swprintf_s(zip, MAX_PATH * 2, L"%s\\flim log %04d-%02d-%02d %02d%02d%02d.zip", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    n = L.n ? flim_zip(zip, L.n, (const wchar_t *const *)L.src, (const wchar_t *const *)L.name) : 0;
    DeleteFileW(eng); DeleteFileW(win); DeleteFileW(info);
    for (i = 0; i < L.n; i++) { free(L.src[i]); free(L.name[i]); }
    if (n <= 0) { fail_box(dlg, L"The log could not be saved."); return; }
    swprintf_s(msg, MAX_PATH * 3, L"Log exported: %s (send this file to whoever helps you with flim)", zip);
    main_log(dlg, msg);
    swprintf_s(msg, MAX_PATH * 3, L"/select,\"%s\"", zip);
    ShellExecuteW(NULL, L"open", L"explorer.exe", msg, NULL, SW_SHOWNORMAL);
}

static void main_people(HWND dlg) {
    wchar_t p[1024], line[1100];
    int n = host_peers(p, 1024);
    if (!host_running()) line[0] = 0;
    else if (!n) wcscpy_s(line, 1100, L"Nobody connected yet.");
    else swprintf_s(line, 1100, L"Connected (%d): %s", n, p);
    SetDlgItemTextW(dlg, IDC_H_PEOPLE, line);
}
static int private_ip(const wchar_t *ip) {        // where friends outside couldn't reach (or carrier-grade NAT)
    int a = 0, b = 0;
    if (swscanf_s(ip, L"%d.%d", &a, &b) != 2) return 0;
    return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b <= 127) || a == 0;
}
static DWORD WINAPI upnp_thread(LPVOID p) {
    HWND dlg = (HWND)p;
    wchar_t ext[64];
    int ok = host_upnp(g_hostPort, 1, ext, 64);
    PostMessageW(dlg, WM_UPNP_DONE, ok, ok && ext[0] ? (LPARAM)_wcsdup(ext) : 0);
    return 0;
}
static void main_host_status(HWND dlg, const wchar_t *extra) {
    wchar_t s[600];
    swprintf_s(s, 600, L"Hosting on port %d.  On this network: ws://%s:%d%s%s", g_hostPort, g_lan[0] ? g_lan : L"localhost", g_hostPort,
               extra ? L"\n" : L"", extra ? extra : L"");
    SetDlgItemTextW(dlg, IDC_H_STATUS, s);
}
static void main_host_controls(HWND dlg, int on) {
    static const int ids[] = { IDC_H_PORT, IDC_H_PW, IDC_H_UPNP, IDC_H_ALSOFL };
    int i;
    for (i = 0; i < 4; i++) EnableWindow(GetDlgItem(dlg, ids[i]), !on);
    SetDlgItemTextW(dlg, IDC_H_START, on ? L"Stop hosting" : L"Start hosting");
    if (!on) { SetDlgItemTextW(dlg, IDC_H_STATUS, L"Not hosting."); SetDlgItemTextW(dlg, IDC_H_SHARE, L""); }
    main_people(dlg);
}
static void main_host_stop(void) {
    if (!host_running()) return;
    host_stop();
    if (g_upnpOn) { host_upnp(g_hostPort, 0, NULL, 0); g_upnpOn = 0; }   // close the port again
}
static void engine_connect(HWND dlg, const wchar_t *url, const wchar_t *pw) {   // "<url>\n<password>\n<name>"
    wchar_t name[128], w[1200];
    char m[3600];
    flimcfg_get(L"name", name, 128);
    swprintf_s(w, 1200, L"%s\n%s\n%s", url, pw, name);
    wide_to_utf8(w, m, sizeof(m));
    SecureZeroMemory(w, sizeof(w));
    if (!engine_send(dlg, GUI_CONNECT, m)) fail_box(dlg, L"FL didn't answer: is it busy (a dialog open)?");
    SecureZeroMemory(m, sizeof(m));
}
// An FL we loaded the engine into late: once the engine's task window is there (it waits for FL to
// settle first), it is introduced to this window and told to connect.
#define ATTACH_TIMER  7
#define ATTACH_MAX_S  90
static wchar_t g_attachUrl[512], g_attachPw[256];
static int g_attachWait;
static void attach_tick(HWND dlg) {
    HWND e = fl_running() ? engine_window_of(g_flPid) : NULL;
    if (!e && fl_running() && ++g_attachWait < ATTACH_MAX_S * 2) return;
    KillTimer(dlg, ATTACH_TIMER);
    if (e) {
        g_engine = e;
        engine_send(dlg, GUI_HELLO, NULL);
        engine_connect(dlg, g_attachUrl, g_attachPw);
    } else if (fl_running()) main_log(dlg, L"The flim engine didn't start in the running FL. Close FL and Join again: flim then starts FL itself.");
    SecureZeroMemory(g_attachPw, sizeof(g_attachPw));
}
// Our FL connects to url: started connected, or (already running) told to connect. An FL opened
// without flim gets the engine first.
static void main_connect(HWND dlg, const wchar_t *url, const wchar_t *pw) {
    if (!fl_running()) {
        DWORD pid;
        HANDLE p;
        if (flimcfg_auto_attach()) pid = plain_fl_pid();
        else {
            long r = pick_fl(dlg);
            if (r < 0) return;
            pid = (DWORD)r;
            if (pid && engine_window_of(pid)) {           // flim is in it already: it reports here from now on
                if (adopt_engine(dlg, engine_window_of(pid)))
                    main_log(dlg, L"Using that FL: flim was already in it. Click Connect if it isn't connected.");
                session_buttons(dlg);
                return;
            }
        }
        if (pid && (p = attach_engine(pid, dlg)) != NULL) {
            fl_track(dlg, p);
            g_engine = NULL;
            g_netState = NET_OFF;
            wcscpy_s(g_attachUrl, 512, url);
            wcscpy_s(g_attachPw, 256, pw);
            g_attachWait = 0;
            SetTimer(dlg, ATTACH_TIMER, 500, NULL);
            SetDlgItemTextW(dlg, IDC_S_STATUS, L"Starting flim in the FL that's already open...");
            main_log(dlg, L"FL was already open: loaded flim into it. It joins in a few seconds.");
            session_buttons(dlg);
            return;
        }
        if (pid) return;                                      // attach_engine said why
        if (main_launch(dlg, url, pw)) main_log(dlg, L"Opened FL: it joins as soon as it has started.");
        return;
    }
    if (!g_engine) { fail_box(dlg, L"FL is still starting: try again in a moment."); return; }
    engine_connect(dlg, url, pw);
}
static void main_join(HWND dlg) {
    wchar_t url[512], pw[256];
    if (fl_running() && (g_netState == NET_ON || g_netState == NET_CONNECTING)) {
        engine_send(dlg, GUI_DISCONNECT, NULL);
        return;
    }
    GetDlgItemTextW(dlg, IDC_J_URL, url, 512);
    GetDlgItemTextW(dlg, IDC_J_PW, pw, 256);
    if (!url[0]) { fail_box(dlg, L"Enter the server address you got from whoever hosts."); SetFocus(GetDlgItem(dlg, IDC_J_URL)); return; }
    if (!main_save_name(dlg)) return;
    flimcfg_set(L"server_url", url);
    if (IsDlgButtonChecked(dlg, IDC_J_REMEMBER)) secret_set_w(L"server_pw", pw); else flimcfg_set(L"server_pw", NULL);
    main_connect(dlg, url, pw);
    SecureZeroMemory(pw, sizeof(pw));
}
static void main_host_start(HWND dlg) {
    wchar_t portS[16], pw[256], share[128];
    char pwu[512], err[200];
    int port;
    GetDlgItemTextW(dlg, IDC_H_PORT, portS, 16);
    port = _wtoi(portS);
    if (port < 1 || port > 65535) { fail_box(dlg, L"Pick a port between 1 and 65535 (8080 is fine)."); return; }
    if (!main_save_name(dlg)) return;
    GetDlgItemTextW(dlg, IDC_H_PW, pw, 256);
    if (!pw[0] && MessageBoxW(dlg, L"Host without a password? Anyone who has the address could join.", L"flim",
                              MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    wide_to_utf8(pw, pwu, sizeof(pwu));
    if (!host_start(port, pwu, dlg, err, sizeof(err))) {
        wchar_t w[200];
        MultiByteToWideChar(CP_UTF8, 0, err, -1, w, 200);
        SecureZeroMemory(pwu, sizeof(pwu));
        fail_box(dlg, L"Can't host: %s", w);
        return;
    }
    SecureZeroMemory(pwu, sizeof(pwu));
    g_hostPort = port;
    swprintf_s(portS, 16, L"%d", port);
    flimcfg_set(L"host_port", portS);
    secret_set_w(L"host_pw", pw);
    flimcfg_set(L"host_upnp", IsDlgButtonChecked(dlg, IDC_H_UPNP) ? L"1" : L"0");
    flimcfg_set(L"host_alsofl", IsDlgButtonChecked(dlg, IDC_H_ALSOFL) ? L"1" : L"0");
    host_lan_ip(g_lan, 64);
    main_host_controls(dlg, 1);
    swprintf_s(share, 128, L"ws://%s:%d", g_lan[0] ? g_lan : L"localhost", port);
    SetDlgItemTextW(dlg, IDC_H_SHARE, share);
    if (IsDlgButtonChecked(dlg, IDC_H_UPNP)) {
        main_host_status(dlg, L"Asking your router to open the port...");
        CloseHandle(CreateThread(NULL, 0, upnp_thread, dlg, 0, NULL));
    } else main_host_status(dlg, L"For people outside your network: forward this TCP port to this PC in your router.");
    main_log(dlg, L"If Windows asks whether flim may use the network, allow it (private networks at least).");
    if (IsDlgButtonChecked(dlg, IDC_H_ALSOFL)) {
        swprintf_s(share, 128, L"localhost:%d", port);
        main_connect(dlg, share, pw);
    }
    SecureZeroMemory(pw, sizeof(pw));
}
static void main_copy(HWND dlg) {
    wchar_t s[256];
    size_t n;
    HGLOBAL g;
    GetDlgItemTextW(dlg, IDC_H_SHARE, s, 256);
    if (!s[0] || !OpenClipboard(dlg)) return;
    EmptyClipboard();
    n = (wcslen(s) + 1) * sizeof(wchar_t);
    if ((g = GlobalAlloc(GMEM_MOVEABLE, n)) != NULL) {
        memcpy(GlobalLock(g), s, n);
        GlobalUnlock(g);
        SetClipboardData(CF_UNICODETEXT, g);
    }
    CloseClipboard();
}

static int wnd_is_edit(HWND h) { wchar_t c[16]; return GetClassNameW(h, c, 16) && !_wcsicmp(c, L"Edit"); }
static int wnd_is_button(HWND h) { wchar_t c[16]; return GetClassNameW(h, c, 16) && !_wcsicmp(c, L"Button"); }
// ---- the window's look: dark plum, light text, the bean's pink on the main actions ----
// The accent (the bean itself, #FF4D99) is only on the main actions (Join, Start hosting, OK), with dark text
// on it (5.9:1). Text is 4.5:1+ on its background; control edges (buttons, checkboxes) 3:1+.
#define C_BG        RGB(0x1A, 0x12, 0x16)
#define C_FIELD     RGB(0x26, 0x1A, 0x20)      // text boxes, lists, the log; white buttons' fill
#define C_TEXT      RGB(0xF6, 0xE9, 0xEF)
#define C_MUTED     RGB(0xB3, 0x9A, 0xA7)
#define C_ACCENT    RGB(0xFF, 0x4D, 0x99)      // the bean
#define C_ACCENT_DN RGB(0xE8, 0x45, 0x8A)
#define C_ONACCENT  RGB(0x1A, 0x12, 0x16)
#define C_EDGE      RGB(0x84, 0x6A, 0x79)      // a control's outline
#define C_FRAME     RGB(0x4A, 0x35, 0x40)      // a section's frame (decoration)
#define C_PRESSED   RGB(0x33, 0x23, 0x2B)
#define C_OFF       RGB(0x2A, 0x20, 0x26)
#define C_OFFTEXT   RGB(0x7A, 0x65, 0x70)
static HBRUSH g_brBg, g_brField;
static HFONT g_fontHead;
static void round_rect(HDC dc, const RECT *r, COLORREF fill, COLORREF edge, int rad) {
    HBRUSH b = CreateSolidBrush(fill), ob = (HBRUSH)SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, edge), op = (HPEN)SelectObject(dc, p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}
// A section frame (group box), painted here: the classic one's white etched line glares on dark. Its caption sits
// on the frame's top edge; the inside is left to the controls in it.
static LRESULT CALLBACK group_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r, t;
        wchar_t s[128];
        SIZE ts;
        HFONT of = (HFONT)SelectObject(dc, g_fontHead);
        HPEN p = CreatePen(PS_SOLID, 1, C_FRAME), op = (HPEN)SelectObject(dc, p);
        HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        GetClientRect(h, &r);
        GetWindowTextW(h, s, 128);
        GetTextExtentPoint32W(dc, s, (int)wcslen(s), &ts);
        RoundRect(dc, r.left, r.top + ts.cy / 2, r.right, r.bottom, 8, 8);
        SetRect(&t, r.left + 6, r.top, r.left + 6 + ts.cx, r.top + ts.cy);
        FillRect(dc, &t, g_brBg);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, C_TEXT);
        DrawTextW(dc, s, -1, &t, DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, ob); SelectObject(dc, op); SelectObject(dc, of);
        DeleteObject(p);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefSubclassProc(h, m, wp, lp);
}
// A scrolling text box / list (the log): Windows draws its border light; ours goes over it, in the dark edge color.
static LRESULT CALLBACK frame_proc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    LRESULT r = DefSubclassProc(h, m, wp, lp);
    (void)id; (void)ref;
    if (m == WM_NCPAINT) {
        HDC dc = GetWindowDC(h);
        RECT w;
        HBRUSH b = CreateSolidBrush(C_FRAME);
        GetWindowRect(h, &w);
        OffsetRect(&w, -w.left, -w.top);
        FrameRect(dc, &w, b);
        InflateRect(&w, -1, -1);
        FrameRect(dc, &w, g_brField);
        DeleteObject(b);
        ReleaseDC(h, dc);
    }
    return r;
}
// Dark title bar, dark scrollbars (the log, lists), our own push buttons and section frames. Checkboxes keep their
// state and are drawn in theme_check (NM_CUSTOMDRAW): themed, their text is black whatever color is asked for.
static void theme_init(HWND dlg) {
    HWND c = NULL;
    BOOL dark = TRUE;
    if (!g_brBg) { g_brBg = CreateSolidBrush(C_BG); g_brField = CreateSolidBrush(C_FIELD); }
    if (!g_fontHead) {                                         // section captions: the dialog's font, bold
        LOGFONTW lf;
        GetObjectW((HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0), sizeof(lf), &lf);
        lf.lfWeight = FW_SEMIBOLD;
        g_fontHead = CreateFontIndirectW(&lf);
    }
    DwmSetWindowAttribute(dlg, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    while ((c = FindWindowExW(dlg, c, NULL, NULL)) != NULL) {
        LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE), type = st & BS_TYPEMASK;
        if (!wnd_is_button(c)) {                               // (the light theme's text-box outline glares on dark)
            wchar_t cls[16];
            if ((st & (WS_VSCROLL | WS_HSCROLL)) || (GetClassNameW(c, cls, 16) && !_wcsicmp(cls, L"ListBox"))) {   // dark scrollbars;
                // its border repainted (frame_proc). (A short list drops WS_VSCROLL until it needs it.)
                SetWindowTheme(c, L"DarkMode_Explorer", NULL);
                SetWindowSubclass(c, frame_proc, 2, 0);
                SetWindowPos(c, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            } else if (wnd_is_edit(c)) SetWindowTheme(c, L"DarkMode_CFD", NULL);
            continue;
        }
        if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) SetWindowLongPtrW(c, GWL_STYLE, (st & ~(LONG_PTR)BS_TYPEMASK) | BS_OWNERDRAW);
        else if (type == BS_GROUPBOX) SetWindowSubclass(c, group_proc, 1, 0);
    }
}
static void theme_button(const DRAWITEMSTRUCT *di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    wchar_t t[128];
    int prim = di->CtlID == IDC_J_JOIN || di->CtlID == IDC_H_START || di->CtlID == IDOK;
    int off = (di->itemState & ODS_DISABLED) != 0, down = (di->itemState & ODS_SELECTED) != 0, focus = (di->itemState & ODS_FOCUS) != 0;
    COLORREF fill = off ? C_OFF : prim ? (down ? C_ACCENT_DN : C_ACCENT) : (down ? C_PRESSED : C_FIELD);
    COLORREF ink = off ? C_OFFTEXT : prim ? C_ONACCENT : C_TEXT;
    HFONT of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    FillRect(dc, &r, g_brBg);                                  // (the corners)
    round_rect(dc, &r, fill, off ? C_OFF : prim ? fill : C_EDGE, 10);
    GetWindowTextW(di->hwndItem, t, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ink);
    DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (focus && !(di->itemState & ODS_NOFOCUSRECT)) {         // keyboard focus (Windows hides it for mouse users)
        RECT f = r;
        InflateRect(&f, -3, -3);
        SetBkColor(dc, fill);
        DrawFocusRect(dc, &f);
    }
    SelectObject(dc, of);
}
// An owner-drawn list's row: the selected one in the accent (dark text on it), the rest on the field color.
static void theme_listitem(const DRAWITEMSTRUCT *di) {
    wchar_t s[400];
    RECT r = di->rcItem;
    int sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH b;
    if (di->itemID == (UINT)-1) return;
    b = CreateSolidBrush(sel ? C_ACCENT : C_FIELD);
    FillRect(di->hDC, &r, b);
    DeleteObject(b);
    SendMessageW(di->hwndItem, LB_GETTEXT, di->itemID, (LPARAM)s);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, sel ? C_ONACCENT : C_TEXT);
    r.left += 4;
    DrawTextW(di->hDC, s, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (di->itemState & ODS_FOCUS) { SetBkColor(di->hDC, sel ? C_ACCENT : C_FIELD); DrawFocusRect(di->hDC, &di->rcItem); }
}
// NM_CUSTOMDRAW from a checkbox: box, tick and label in our colors (the control keeps its own checked state).
// Returns the dialog result, or -1 for anything that isn't a checkbox's prepaint.
static LRESULT theme_check(const NMCUSTOMDRAW *cd) {
    HWND h = cd->hdr.hwndFrom;
    LONG_PTR type = GetWindowLongPtrW(h, GWL_STYLE) & BS_TYPEMASK;
    HDC dc = cd->hdc;
    RECT r = cd->rc, box, txt;
    wchar_t s[160];
    int on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED, off = !IsWindowEnabled(h);
    int sz, mid;
    HFONT of;
    TEXTMETRICW tm;
    if (cd->hdr.code != NM_CUSTOMDRAW || !wnd_is_button(h) || (type != BS_AUTOCHECKBOX && type != BS_CHECKBOX)) return -1;
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    of = (HFONT)SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &tm);
    sz = tm.tmAscent; mid = (r.top + r.bottom) / 2;
    FillRect(dc, &r, g_brBg);
    SetRect(&box, r.left, mid - sz / 2, r.left + sz, mid - sz / 2 + sz);
    round_rect(dc, &box, off ? C_OFF : on ? C_ACCENT : C_FIELD, off ? C_OFFTEXT : on ? C_ACCENT : C_EDGE, 5);
    if (on) {                                                  // the tick
        POINT pt[3] = { { box.left + sz * 2 / 9, mid }, { box.left + sz * 4 / 9, box.bottom - sz * 3 / 11 }, { box.right - sz * 2 / 9, box.top + sz * 3 / 11 } };
        HPEN p = CreatePen(PS_SOLID, sz / 7 > 1 ? sz / 7 : 2, off ? C_OFFTEXT : C_ONACCENT), op = (HPEN)SelectObject(dc, p);
        Polyline(dc, pt, 3);
        SelectObject(dc, op); DeleteObject(p);
    }
    GetWindowTextW(h, s, 160);
    SetRect(&txt, box.right + sz / 2, r.top, r.right, r.bottom);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, off ? C_OFFTEXT : C_TEXT);
    DrawTextW(dc, s, -1, &txt, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if ((cd->uItemState & CDIS_FOCUS) && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT f = txt;
        DrawTextW(dc, s, -1, &f, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_CALCRECT);
        f.top = r.top; f.bottom = r.bottom; InflateRect(&f, 2, 0);
        SetBkColor(dc, C_BG); SetTextColor(dc, C_TEXT);
        DrawFocusRect(dc, &f);
    }
    SelectObject(dc, of);
    return CDRF_SKIPDEFAULT;
}
// WM_CTLCOLOR*: dark behind everything, darker-raised text fields (the read-only log and address too). 0: not ours.
static INT_PTR theme_color(HWND dlg, UINT m, HDC dc, HWND c) {
    (void)dlg;
    if (m == WM_CTLCOLORDLG) return (INT_PTR)g_brBg;
    if (m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX || (m == WM_CTLCOLORSTATIC && wnd_is_edit(c))) {
        SetTextColor(dc, C_TEXT); SetBkColor(dc, C_FIELD);
        return (INT_PTR)g_brField;
    }
    if (m == WM_CTLCOLORSTATIC || m == WM_CTLCOLORBTN) {
        SetTextColor(dc, IsWindowEnabled(c) ? C_TEXT : C_OFFTEXT); SetBkColor(dc, C_BG); SetBkMode(dc, TRANSPARENT);
        return (INT_PTR)g_brBg;
    }
    return 0;
}

static INT_PTR CALLBACK main_proc(HWND dlg, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT *)lp)->CtlType == ODT_BUTTON) { theme_button((const DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_CTLCOLORDLG: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
        return theme_color(dlg, m, (HDC)wp, (HWND)lp);
    case WM_NOTIFY: {
        LRESULT r = theme_check((const NMCUSTOMDRAW *)lp);
        if (r == -1) break;
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, r);
        return TRUE; }
    case WM_INITDIALOG: {
        wchar_t v[512];
        HICON ic = LoadIconW(g_inst, L"IDI_FLIM");
        theme_init(dlg);
        if (ic) { SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)ic); SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ic); }
        SetWindowTextW(dlg, L"flim v." FLIM_VERSION_W);
        flimcfg_get(L"name", v, 512); SetDlgItemTextW(dlg, IDC_M_NAME, v);
        SendDlgItemMessageW(dlg, IDC_M_NAME, EM_LIMITTEXT, 64, 0);
        flimcfg_get(L"song_name", v, 512); SetDlgItemTextW(dlg, IDC_M_SONG, v);
        SendDlgItemMessageW(dlg, IDC_M_SONG, EM_LIMITTEXT, 80, 0);
        // (no cue banner: Windows draws it in a fixed grey that is too faint on the dark theme; the label says it)
        main_flinfo(dlg);
        flimcfg_get(L"server_url", v, 512); SetDlgItemTextW(dlg, IDC_J_URL, v);
        secret_get_w(L"server_pw", v, 512); SetDlgItemTextW(dlg, IDC_J_PW, v);
        CheckDlgButton(dlg, IDC_J_REMEMBER, v[0] ? BST_CHECKED : BST_UNCHECKED);
        SecureZeroMemory(v, sizeof(v));
        flimcfg_get(L"host_port", v, 512); SetDlgItemTextW(dlg, IDC_H_PORT, v[0] ? v : L"8080");
        secret_get_w(L"host_pw", v, 512); SetDlgItemTextW(dlg, IDC_H_PW, v);
        SecureZeroMemory(v, sizeof(v));
        flimcfg_get(L"host_upnp", v, 512); CheckDlgButton(dlg, IDC_H_UPNP, wcscmp(v, L"0") ? BST_CHECKED : BST_UNCHECKED);
        flimcfg_get(L"host_alsofl", v, 512); CheckDlgButton(dlg, IDC_H_ALSOFL, wcscmp(v, L"1") == 0 ? BST_CHECKED : BST_UNCHECKED);
        main_host_controls(dlg, 0);
        SetDlgItemInt(dlg, IDC_S_LOCK, flimcfg_lock_s(), FALSE);
        SendDlgItemMessageW(dlg, IDC_S_LOCK, EM_LIMITTEXT, 2, 0);
        SendDlgItemMessageW(dlg, IDC_S_LOG, EM_SETLIMITTEXT, 0, 0);
        CheckDlgButton(dlg, IDC_S_DETAIL, flimcfg_log_detailed() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_S_CURSORS, flimcfg_show_cursors() ? BST_CHECKED : BST_UNCHECKED);
        {
            int on, m = flimcfg_backup_min(&on);
            CheckDlgButton(dlg, IDC_M_BACKUP, on ? BST_CHECKED : BST_UNCHECKED);
            SetDlgItemInt(dlg, IDC_M_BACKUPMIN, m, FALSE);
            SendDlgItemMessageW(dlg, IDC_M_BACKUPMIN, EM_LIMITTEXT, 3, 0);
        }
        SetDlgItemTextW(dlg, IDC_S_STATUS, L"FL isn't running.");
        CheckDlgButton(dlg, IDC_M_AUTOATTACH, flimcfg_auto_attach() ? BST_CHECKED : BST_UNCHECKED);
        if (flimcfg_auto_attach()) adopt_running_fl(dlg);
        else main_log(dlg, L"Attach to FL automatically is off: Join asks which FL Studio to use.");
        session_buttons(dlg);
        return TRUE;
    }
    case WM_COPYDATA: {                  // from our FL's engine: its status or a log line
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        wchar_t w[2048];
        if (wp) g_engine = (HWND)wp;
        if (cd->lpData && cd->cbData) {
            int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)cd->lpData, (int)cd->cbData, w, 2047);
            w[n > 0 ? n : 0] = 0;
            if (cd->dwData == GUI_STATUS) {
                wchar_t *tab = wcschr(w, L'\t');
                g_netState = _wtoi(w);
                SetDlgItemTextW(dlg, IDC_S_STATUS, tab ? tab + 1 : w);
                session_buttons(dlg);
            } else if (cd->dwData == GUI_LOG) main_log(dlg, w);
            else if (cd->dwData == GUI_ALERT) {    // shown after this message returns (FL's sender doesn't wait on it)
                main_log(dlg, w);
                PostMessageW(dlg, WM_ALERT, 0, (LPARAM)_wcsdup(w));
            }
        }
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }
    case WM_TIMER:
        if (wp == ATTACH_TIMER) { attach_tick(dlg); return TRUE; }
        break;
    case WM_ALERT: {                     // the session leader left, the connection dropped...
        wchar_t *t = (wchar_t *)lp;
        ShowWindow(dlg, SW_RESTORE);
        FlashWindow(dlg, TRUE);
        MessageBoxW(dlg, t, L"flim", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        free(t);
        return TRUE;
    }
    case WM_FL_EXITED:
        if ((HANDLE)wp != g_flProc) return TRUE;   // an FL from before
        crash_collect(dlg, g_flProc);
        CloseHandle(g_flProc);
        g_flProc = NULL; g_engine = NULL; g_netState = NET_OFF;
        SetDlgItemTextW(dlg, IDC_S_STATUS, L"FL isn't running.");
        main_log(dlg, L"FL closed.");
        session_buttons(dlg);
        return TRUE;
    case WM_CTLCOLORSTATIC:              // the status in the color of the connection; the rest: theme_color
        if ((HWND)lp == GetDlgItem(dlg, IDC_S_STATUS)) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, g_netState == NET_ON ? RGB(0x5f, 0xd3, 0x8d) : g_netState == NET_CONNECTING ? RGB(0xf0, 0xb3, 0x5a)
                           : g_netState == NET_ERR ? RGB(0xff, 0x7a, 0x70) : C_MUTED);
            SetBkMode(dc, TRANSPARENT);
            return (INT_PTR)g_brBg;
        }
        return theme_color(dlg, m, (HDC)wp, (HWND)lp);   // (the log and the address are read-only edits: white)
    case WM_HOST_LOG:
        main_log(dlg, (const wchar_t *)lp);
        free((void *)lp);
        return TRUE;
    case WM_HOST_PEERS:
        main_people(dlg);
        return TRUE;
    case WM_UPNP_DONE: {
        wchar_t *ext = (wchar_t *)lp, msg[400], share[128];
        if (!host_running()) { if (wp) host_upnp(g_hostPort, 0, NULL, 0); free(ext); return TRUE; }   // stopped meanwhile
        g_upnpOn = (int)wp;
        if (wp && ext && !private_ip(ext)) {
            swprintf_s(share, 128, L"ws://%s:%d", ext, g_hostPort);
            SetDlgItemTextW(dlg, IDC_H_SHARE, share);
            swprintf_s(msg, 400, L"Over the internet: %s (port opened on your router).", share);
            main_log(dlg, msg);
        } else if (wp) {
            swprintf_s(msg, 400, L"Your router opened the port, but its address (%s) isn't reachable from the internet (your provider "
                                 L"shares one address between customers). Friends outside can't join directly: use a VPN like Tailscale.",
                       ext ? ext : L"unknown");
            main_log(dlg, msg);
        } else {
            swprintf_s(msg, 400, L"Your router didn't open the port (UPnP off or not supported). For people outside your network, "
                                 L"forward TCP port %d to %s in your router.", g_hostPort, g_lan);
            main_log(dlg, msg);
        }
        main_host_status(dlg, wp && ext && !private_ip(ext) ? msg : L"Outside your network: see the log below.");
        free(ext);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_J_JOIN: main_join(dlg); return TRUE;
        case IDC_H_START:
            if (host_running()) {
                if (MessageBoxW(dlg, L"Stop hosting? Everyone connected to this PC is disconnected.", L"flim", MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
                main_host_stop();
                main_host_controls(dlg, 0);
            } else main_host_start(dlg);
            return TRUE;
        case IDC_H_COPY: main_copy(dlg); return TRUE;
        case IDC_S_LOCK:                 // each valid value is saved and FL told; leaving the box shows the saved one
            if (HIWORD(wp) == EN_CHANGE) {
                UINT s = GetDlgItemInt(dlg, IDC_S_LOCK, NULL, FALSE);
                wchar_t v[16];
                if (s < 1 || s > FLIM_LOCK_S_MAX || (int)s == flimcfg_lock_s()) return TRUE;
                swprintf_s(v, 16, L"%u", s);
                flimcfg_set(L"plugin_lock_s", v);
                engine_send(dlg, GUI_SETTINGS, NULL);
            } else if (HIWORD(wp) == EN_KILLFOCUS) SetDlgItemInt(dlg, IDC_S_LOCK, flimcfg_lock_s(), FALSE);
            return TRUE;
        case IDC_M_BACKUP:
            flimcfg_set(L"backup_on", IsDlgButtonChecked(dlg, IDC_M_BACKUP) ? L"1" : L"0");
            engine_send(dlg, GUI_SETTINGS, NULL);
            return TRUE;
        case IDC_M_BACKUPMIN: {          // like the lock seconds: valid values saved as typed
            int on;
            UINT m = GetDlgItemInt(dlg, IDC_M_BACKUPMIN, NULL, FALSE);
            if (HIWORD(wp) == EN_CHANGE && m >= 1 && m <= 240 && (int)m != flimcfg_backup_min(&on)) {
                wchar_t v[16];
                swprintf_s(v, 16, L"%u", m);
                flimcfg_set(L"backup_min", v);
                engine_send(dlg, GUI_SETTINGS, NULL);
            } else if (HIWORD(wp) == EN_KILLFOCUS) SetDlgItemInt(dlg, IDC_M_BACKUPMIN, flimcfg_backup_min(&on), FALSE);
            return TRUE; }
        case IDC_M_SONG:                 // typed in while FL runs: an untitled project gets it now
            if (HIWORD(wp) == EN_KILLFOCUS) { main_save_song(dlg); engine_send(dlg, GUI_SETTINGS, NULL); }
            return TRUE;
        case IDC_S_CURSORS:              // the others' mouse arrows in this FL
            flimcfg_set(L"show_cursors", IsDlgButtonChecked(dlg, IDC_S_CURSORS) == BST_CHECKED ? NULL : L"0");
            engine_send(dlg, GUI_SETTINGS, NULL);
            return TRUE;
        case IDC_S_DETAIL: {
            int on = IsDlgButtonChecked(dlg, IDC_S_DETAIL) == BST_CHECKED;
            flimcfg_set(L"log_detailed", on ? L"1" : NULL);
            engine_send(dlg, GUI_SETTINGS, NULL);
            main_log(dlg, on ? L"Detailed log on: every change sent and received goes into flim's log file, and this window "
                               L"shows nearly everything. \"Export log...\" saves it all to send."
                             : L"Detailed log off.");
            return TRUE; }
        case IDC_S_EXPORT: export_log(dlg); return TRUE;
        case IDC_M_AUTOATTACH: {
            int on = IsDlgButtonChecked(dlg, IDC_M_AUTOATTACH) == BST_CHECKED;
            flimcfg_set(L"auto_attach", on ? NULL : L"0");
            if (on && !fl_running()) { adopt_running_fl(dlg); session_buttons(dlg); }
            main_log(dlg, on ? L"Attach to FL automatically: flim uses the FL that's running (or starts one)."
                             : L"Attach to FL automatically off: Join asks which FL Studio to use.");
            return TRUE; }
        case IDC_S_RESYNC:
            if (MessageBoxW(dlg, L"Load the session leader's project again, so this FL matches the host?\n\n"
                                 L"Changes you made that haven't reached the others yet may be lost.\n\n"
                                 L"(If you are the session leader, everyone else loads your project instead, and "
                                 L"their changes that haven't reached you yet may be lost.)",
                            L"Resync from host", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK)
                engine_send(dlg, GUI_RESYNC, NULL);
            return TRUE;
        case IDC_M_PROJECTS: { wchar_t d[MAX_PATH * 2]; flimcfg_projects_dir(d, MAX_PATH * 2); open_folder(d); return TRUE; }
        case IDC_M_PROJDIR: pick_projects_dir(dlg); return TRUE;
        case IDC_M_CLEARCACHE: clear_cache(dlg); return TRUE;
        case IDC_M_CRASHES: { wchar_t d[MAX_PATH * 2]; crash_dir(d, MAX_PATH * 2); open_folder(d); return TRUE; }
        case IDC_M_SHARED: { wchar_t d[MAX_PATH * 2]; flimcfg_shared_dir(d, MAX_PATH * 2); open_folder(d); return TRUE; }
        case IDC_M_CHANGEFL:
            if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETUP), dlg, setup_proc, 0) == IDOK) {
                wchar_t v[128];
                flimcfg_get(L"name", v, 128); SetDlgItemTextW(dlg, IDC_M_NAME, v);
                main_flinfo(dlg);
            }
            return TRUE;
        case IDC_M_OPENFL:
            if (!main_save_name(dlg) || fl_running()) return TRUE;
            main_launch(dlg, NULL, NULL);
            return TRUE;
        case IDCANCEL:
            if (host_running()) {
                if (MessageBoxW(dlg, L"You're hosting: closing flim ends the session for everyone. Close anyway?", L"flim",
                                MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return TRUE;
                main_host_stop();
            }
            if (fl_running())                              // flim stays loaded in that FL until it restarts
                MessageBoxW(dlg, L"FL Studio stays open, with flim still loaded in it.\n\n"
                                 L"When you're done using flim, close FL Studio and open it again before working on "
                                 L"your own: some strange behavior can be left over in FL until it restarts.",
                            L"flim", MB_OK | MB_ICONINFORMATION);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    int argc = 0, i, setup = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev; (void)cmd; (void)show;
    for (i = 1; i < argc; i++)
        if (_wcsicmp(argv[i], L"--setup") == 0 || _wcsicmp(argv[i], L"/setup") == 0) setup = 1;
    LocalFree(argv);
    g_inst = inst;

    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (setup || !config_ok()) {
        if (DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETUP), NULL, setup_proc, 0) != IDOK) return 1;
    }
    {   // dev loop (restart_test.ps1): FLIM_AUTOCONNECT in the environment starts FL connected, no window
        wchar_t url[600], pw[256], dir[MAX_PATH * 2];
        if (GetEnvironmentVariableW(L"FLIM_AUTOCONNECT", url, 600) > 0) {
            HANDLE p;
            if (!GetEnvironmentVariableW(L"FLIM_PASSWORD", pw, 256)) pw[0] = 0;
            flimcfg_get(L"fl_dir", dir, MAX_PATH * 2);
            p = launch(dir, url, pw, NULL);
            if (p) CloseHandle(p);
            return p ? 0 : 1;
        }
    }
    DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_proc, 0);
    CoUninitialize();
    return 0;
}

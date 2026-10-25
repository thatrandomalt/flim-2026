// flim settings, shared by flim.exe (launcher) and flim_engine.dll (engine).
// Stored in %APPDATA%\flim\flim.ini, section [flim]:
//   fl_dir     FL Studio 2026 install folder (holds FL64.exe)          - launcher
//   name       user name shown to peers (sanitized, see flim_sanitize_name)
//   server_url last server URL typed into the Connect dialog           - engine
//   server_pw  last password, DPAPI-encrypted for this Windows user (hex)
//   plugin_lock_s  seconds a plugin stays locked to us after our last change of it (1-10)
//   backup_on / backup_min  project backup zips (<song folder>\Backups), every N minutes
//   projects_dir   where the song folders go (default Documents\flim\Projects)
//   song_name      name for an untitled project the session leader starts with (optional)
//   log_detailed   1: the detailed log (every op, a structure digest; flim's window shows nearly every line)
//   host_leaderctl 1: hosting without joining, the session leader may use the people controls too  - launcher
// The file is created as UTF-16 so the profile API keeps non-ASCII paths and names.
#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "version.lib")

#define FLIM_NAME_MAX  24
#define FLIM_FL_MAJOR  26                   // FL Studio 2026 = file version 26.x
#define FLIM_FL_BUILD  L"26.1.7.5654"       // the build engine/fl_addrs.h's VAs and signatures were taken from:
                                            // other builds run on the signatures alone (the engine checks)

// File version a.b.c.d of an exe/dll; 0 if it has none.
static int file_version(const wchar_t *file, WORD v[4]) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(file, &h);
    VS_FIXEDFILEINFO *fi = NULL;
    UINT fl = 0;
    int ok = 0;
    void *buf;
    if (!n) return 0;
    buf = malloc(n);
    if (GetFileVersionInfoW(file, 0, n, buf) && VerQueryValueW(buf, L"\\", (void **)&fi, &fl) && fi) {
        v[0] = HIWORD(fi->dwFileVersionMS); v[1] = LOWORD(fi->dwFileVersionMS);
        v[2] = HIWORD(fi->dwFileVersionLS); v[3] = LOWORD(fi->dwFileVersionLS);
        ok = 1;
    }
    free(buf);
    return ok;
}

// flim.exe's window <-> the engine in FL: WM_COPYDATA, dwData = one of these, UTF-8 text
// (the engine's "flim window link" section has the formats).
enum { GUI_STATUS = 1, GUI_LOG, GUI_CONNECT, GUI_DISCONNECT, GUI_HELLO, GUI_SETTINGS, GUI_ALERT, GUI_RESYNC, GUI_PEOPLE, GUI_ADMIN };
enum { NET_OFF, NET_CONNECTING, NET_ON, NET_ERR };   // the connection state in GUI_STATUS

// %APPDATA%: from the environment first. The engine reads its settings while FL starts up, and
// asking the shell for the same folder from a second thread then crashed FL under Wine (FL's own
// SHGetPathFromIDListW for CSIDL_APPDATA faulted inside Wine's shell32).
static int flim_appdata(wchar_t *out, int cap) {
    DWORD n = GetEnvironmentVariableW(L"APPDATA", out, cap);
    if (n && n < (DWORD)cap) return 1;
    return SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, out));
}
static int flimcfg_path(wchar_t *out, int cap) {
    wchar_t dir[MAX_PATH];
    if (!flim_appdata(dir, MAX_PATH)) return 0;
    swprintf_s(out, cap, L"%s\\flim", dir);
    CreateDirectoryW(out, NULL);
    wcscat_s(out, cap, L"\\flim.ini");
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, "\xff\xfe", 2, &w, NULL); CloseHandle(h); }
    }
    return 1;
}

static void flimcfg_get(const wchar_t *key, wchar_t *out, int cap) {
    wchar_t ini[MAX_PATH];
    out[0] = 0;
    if (flimcfg_path(ini, MAX_PATH)) GetPrivateProfileStringW(L"flim", key, L"", out, cap, ini);
}

static void flimcfg_set(const wchar_t *key, const wchar_t *val) {   // val NULL = delete the key
    wchar_t ini[MAX_PATH];
    if (flimcfg_path(ini, MAX_PATH)) WritePrivateProfileStringW(L"flim", key, val, ini);
}

#define FLIM_LOCK_S_MAX 10
static int flimcfg_lock_s(void) {
    wchar_t v[16];
    int s;
    flimcfg_get(L"plugin_lock_s", v, 16);
    s = v[0] ? _wtoi(v) : FLIM_LOCK_S_MAX;
    return s < 1 ? 1 : s > FLIM_LOCK_S_MAX ? FLIM_LOCK_S_MAX : s;
}

static int flimcfg_log_detailed(void) {
    wchar_t v[8];
    flimcfg_get(L"log_detailed", v, 8);
    return v[0] && _wtoi(v) != 0;
}
// flim.exe attaches to a running FL by itself, unless turned off ("auto_attach=0": Join asks which FL)
static int flimcfg_auto_attach(void) {
    wchar_t v[8];
    flimcfg_get(L"auto_attach", v, 8);
    return !v[0] || _wtoi(v) != 0;
}
// the others' mouse arrows over the piano roll / playlist: on unless turned off ("show_cursors=0")
static int flimcfg_show_cursors(void) {
    wchar_t v[8];
    flimcfg_get(L"show_cursors", v, 8);
    return !v[0] || _wtoi(v) != 0;
}

// Documents\flim (crash reports, flim's own sample cache) and the projects folder (flim.ini
// projects_dir, default Documents\flim\Projects): one folder per song, see the engine's song folders.
static void flimcfg_flim_dir(wchar_t *out, int cap) {
    wchar_t d[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, 0, d))) wcscpy_s(d, MAX_PATH, L"C:");
    swprintf_s(out, cap, L"%s\\flim", d);
}
static void flimcfg_projects_dir(wchar_t *out, int cap) {
    flimcfg_get(L"projects_dir", out, cap);
    if (!out[0]) { flimcfg_flim_dir(out, cap); wcscat_s(out, cap, L"\\Projects"); }
}
// The shared folder: Public Documents\flim\Shared (C:\Users\Public\Documents\flim\Shared), the same path
// on every PC, so a plugin's path to a file in it is right everywhere. Not a setting for that reason;
// FLIM_SHARED_DIR moves it for tests (two FLs on one PC).
static void flimcfg_shared_dir(wchar_t *out, int cap) {
    wchar_t d[MAX_PATH];
    if (GetEnvironmentVariableW(L"FLIM_SHARED_DIR", out, cap) - 1u < (DWORD)cap - 1) return;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_DOCUMENTS, NULL, 0, d))) wcscpy_s(d, MAX_PATH, L"C:\\Users\\Public\\Documents");
    swprintf_s(out, cap, L"%s\\flim\\Shared", d);
}

#define FLIM_BACKUP_MIN_DEF 10
static int flimcfg_backup_min(int *on) {           // minutes (1-240); *on: backups enabled
    wchar_t v[16];
    int m;
    flimcfg_get(L"backup_on", v, 16);
    *on = !v[0] || _wtoi(v) != 0;
    flimcfg_get(L"backup_min", v, 16);
    m = v[0] ? _wtoi(v) : FLIM_BACKUP_MIN_DEF;
    return m < 1 ? 1 : m > 240 ? 240 : m;
}

// Names travel to peers and into FL's hint bar: letters/digits (any script), space,
// '-', '_', '.'; whitespace runs collapse to one space, trimmed, at most FLIM_NAME_MAX.
// Returns 0 when nothing usable is left (no letter or digit at all).
static int flim_sanitize_name(const wchar_t *in, wchar_t *out, int cap) {
    int o = 0, gap = 0, alnum = 0;
    if (cap > FLIM_NAME_MAX + 1) cap = FLIM_NAME_MAX + 1;
    for (; *in && o < cap - 1; in++) {
        wchar_t c = *in;
        if (c == L' ' || c == L'\t') { gap = o > 0; continue; }
        if (c >= 0xD800 && c <= 0xDFFF) continue;                   // no emoji / surrogates
        if (IsCharAlphaNumericW(c)) alnum = 1;
        else if (c != L'-' && c != L'_' && c != L'.') continue;
        if (gap) { if (o >= cap - 2) break; out[o++] = L' '; gap = 0; }
        out[o++] = c;
    }
    out[o] = 0;
    return alnum;
}

// Password at rest: DPAPI (only this Windows user can decrypt), stored as hex.
static void flimcfg_set_secret(const wchar_t *key, const char *utf8) {
    DATA_BLOB in, out;
    wchar_t *hex;
    DWORD i;
    if (!utf8 || !*utf8) { flimcfg_set(key, NULL); return; }
    in.pbData = (BYTE *)utf8; in.cbData = (DWORD)strlen(utf8);
    if (!CryptProtectData(&in, L"flim", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) return;
    hex = (wchar_t *)malloc(((size_t)out.cbData * 2 + 1) * sizeof(wchar_t));
    for (i = 0; i < out.cbData; i++) swprintf_s(hex + i * 2, 3, L"%02x", out.pbData[i]);
    hex[out.cbData * 2] = 0;
    flimcfg_set(key, hex);
    free(hex);
    LocalFree(out.pbData);
}

static void flimcfg_get_secret(const wchar_t *key, char *out, int cap) {
    wchar_t hex[4096];
    DATA_BLOB in, plain;
    size_t n, i;
    out[0] = 0;
    flimcfg_get(key, hex, 4096);
    n = wcslen(hex) / 2;
    if (!n) return;
    in.pbData = (BYTE *)malloc(n); in.cbData = (DWORD)n;
    for (i = 0; i < n; i++) { unsigned v = 0; swscanf_s(hex + i * 2, L"%2x", &v); in.pbData[i] = (BYTE)v; }
    if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        if ((int)plain.cbData < cap) { memcpy(out, plain.pbData, plain.cbData); out[plain.cbData] = 0; }
        SecureZeroMemory(plain.pbData, plain.cbData);
        LocalFree(plain.pbData);
    }
    free(in.pbData);
}

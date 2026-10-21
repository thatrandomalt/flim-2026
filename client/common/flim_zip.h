// A plain .zip writer shared by the engine (song zips, project backups) and flim.exe (crash
// reports): files stored uncompressed (audio barely compresses), UTF-8 names, no ZIP64.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t flimzip_crc(uint32_t c, const unsigned char *p, size_t n) {
    static uint32_t T[256];
    uint32_t k, j, v;
    size_t i;
    if (!T[1])
        for (k = 0; k < 256; k++) { for (v = k, j = 0; j < 8; j++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; T[k] = v; }
    c = ~c;
    for (i = 0; i < n; i++) c = T[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
static int flimzip_put(HANDLE f, const void *p, DWORD n) { DWORD w; return WriteFile(f, p, n, &w, NULL) && w == n; }

// src[i] goes into zip as names[i] (NULL: src's file name). Written to <zip>.part, then renamed.
// Returns the number of files written, or -1 (a file could not be read, or over 4 GB in all).
static int flim_zip(const wchar_t *zip, int n, const wchar_t *const *src, const wchar_t *const *names) {
    wchar_t part[MAX_PATH * 2];
    HANDLE out;
    unsigned char *cd = NULL, *buf, e[22];
    size_t cdn = 0;
    uint64_t off = 0;
    int i, ok = 1;
    swprintf_s(part, MAX_PATH * 2, L"%s.part", zip);
    out = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) return -1;
    buf = (unsigned char *)malloc(256 * 1024);
    for (i = 0; ok && i < n; i++) {
        const wchar_t *nmw = names && names[i] ? names[i] : src[i] + wcslen(src[i]);
        unsigned char lh[30], fix[12], *c;
        char name[MAX_PATH * 3];
        uint64_t at = off, got = 0;
        uint32_t crc = 0;
        WORD dd = 0, dt = 0;
        FILETIME ft, lt;
        LARGE_INTEGER li;
        HANDLE in;
        DWORD k;
        int nl;
        if (!names || !names[i]) while (nmw > src[i] && nmw[-1] != L'\\' && nmw[-1] != L'/') nmw--;
        nl = WideCharToMultiByte(CP_UTF8, 0, nmw, -1, name, sizeof(name), NULL, NULL) - 1;
        in = CreateFileW(src[i], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (nl <= 0 || i == 0xFFFF || in == INVALID_HANDLE_VALUE) { if (in != INVALID_HANDLE_VALUE) CloseHandle(in); ok = 0; break; }
        GetFileTime(in, NULL, NULL, &ft);
        FileTimeToLocalFileTime(&ft, &lt);
        FileTimeToDosDateTime(&lt, &dd, &dt);
        memset(lh, 0, sizeof(lh));
        *(uint32_t *)lh = 0x04034b50; *(uint16_t *)(lh + 4) = 20; *(uint16_t *)(lh + 6) = 0x0800;   // UTF-8 name, stored
        *(uint16_t *)(lh + 10) = dt; *(uint16_t *)(lh + 12) = dd; *(uint16_t *)(lh + 26) = (uint16_t)nl;
        ok = flimzip_put(out, lh, 30) && flimzip_put(out, name, nl);
        while (ok && (ok = ReadFile(in, buf, 256 * 1024, &k, NULL)) != 0 && k) {
            crc = flimzip_crc(crc, buf, k);
            got += k;
            ok = at + 30 + nl + got < 0xFFFF0000ull && flimzip_put(out, buf, k);   // ponytail: no ZIP64
        }
        CloseHandle(in);
        if (!ok) break;
        *(uint32_t *)fix = crc; *(uint32_t *)(fix + 4) = (uint32_t)got; *(uint32_t *)(fix + 8) = (uint32_t)got;
        li.QuadPart = (LONGLONG)at + 14;                             // crc and sizes, now that they're known
        ok = SetFilePointerEx(out, li, NULL, FILE_BEGIN) && flimzip_put(out, fix, 12);
        li.QuadPart = 0;
        if (!ok || !SetFilePointerEx(out, li, NULL, FILE_END)) { ok = 0; break; }
        off = at + 30 + nl + got;
        cd = (unsigned char *)realloc(cd, cdn + 46 + nl);
        c = cd + cdn;
        memset(c, 0, 46);
        *(uint32_t *)c = 0x02014b50; *(uint16_t *)(c + 4) = 20; *(uint16_t *)(c + 6) = 20; *(uint16_t *)(c + 8) = 0x0800;
        *(uint16_t *)(c + 12) = dt; *(uint16_t *)(c + 14) = dd; memcpy(c + 16, fix, 12);
        *(uint16_t *)(c + 28) = (uint16_t)nl; *(uint32_t *)(c + 42) = (uint32_t)at;
        memcpy(c + 46, name, nl);
        cdn += 46 + nl;
    }
    free(buf);
    if (ok) {
        memset(e, 0, sizeof(e));
        *(uint32_t *)e = 0x06054b50; *(uint16_t *)(e + 8) = (uint16_t)n; *(uint16_t *)(e + 10) = (uint16_t)n;
        *(uint32_t *)(e + 12) = (uint32_t)cdn; *(uint32_t *)(e + 16) = (uint32_t)off;
        ok = (!cdn || flimzip_put(out, cd, (DWORD)cdn)) && flimzip_put(out, e, 22);
    }
    free(cd);
    CloseHandle(out);
    if (ok && MoveFileExW(part, zip, MOVEFILE_REPLACE_EXISTING)) return n;
    DeleteFileW(part);
    return -1;
}

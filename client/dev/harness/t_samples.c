// Offline test of the plugin-data sample path code (collect + same-length rewrite).
// build.bat, then t_samples.exe: prints OK. Leaves small aliases under Documents\flim\samples.
#include "../../engine/flim_engine.c"

static unsigned char *find_bytes(unsigned char *d, size_t n, const char *s) {
    size_t i, k = strlen(s);
    for (i = 0; i + k <= n; i++) if (memcmp(d + i, s, k) == 0) return d + i;
    return NULL;
}

static size_t put_ev(unsigned char *o, int id, const void *p, size_t n) {
    size_t k = 0, v = n;
    o[k++] = (unsigned char)id;
    do { unsigned char b = (unsigned char)(v & 0x7f); v >>= 7; if (v) b |= 0x80; o[k++] = b; } while (v);
    memcpy(o + k, p, n);
    return k + n;
}

int main(void) {
    static unsigned char plug[4096], img[8192];
    const wchar_t *wpeer = L"D:\\Alex\\Sample Packs\\Very Long Folder Name For Drums\\kick 01.wav";
    const char *apeer = "D:\\Alex\\Sample Packs\\Another Long Folder Name\\snare 02.wav";
    wchar_t local[MAX_PATH], wa[MAX_PATH];
    size_t pn = 0, n = 0, i;
    PathList L;
    int fails = 0;
    InitializeCriticalSection(&g_sampleLock);
    // plugin data in some unknown format: junk, a UTF-16 path + NUL, junk, an ANSI path + NUL, junk
    memcpy(plug, "\x01\x02\x03\x04\x05\x06\x07", 7); pn = 7;
    memcpy(plug + pn, wpeer, wcslen(wpeer) * 2 + 2); pn += wcslen(wpeer) * 2 + 2;
    memcpy(plug + pn, "\x10\x20\x30", 3); pn += 3;
    memcpy(plug + pn, apeer, strlen(apeer) + 1); pn += strlen(apeer) + 1;
    memcpy(plug + pn, "\xaa\xbb", 2); pn += 2;
    // an .fst image: FLhd + FLdt with one 213 event
    memcpy(img, "FLhd", 4); *(uint32_t *)(img + 4) = 6; memset(img + 8, 0, 6); n = 14;
    memcpy(img + n, "FLdt", 4); n += 8;
    n += put_ev(img + n, EV_PLUGIN_DATA, plug, pn);
    *(uint32_t *)(img + 18) = (uint32_t)(n - 22);

    memset(&L, 0, sizeof(L));
    flp_walk(img, n, collect_cb, &L);
    printf("collected %d paths\n", L.n);
    for (i = 0; i < (size_t)L.n; i++) printf("  %S\n", L.paths[i]);
    if (L.n != 2) fails++;

    // the receiver has the files at these local places
    {   // a stand-in for the downloaded sample
        FILE *f = NULL;
        GetTempPathW(MAX_PATH, local);
        wcscat_s(local, MAX_PATH, L"flim_harness_kick.wav");
        if (_wfopen_s(&f, local, L"wb") == 0 && f) { fputs("RIFF", f); fclose(f); }
    }
    MultiByteToWideChar(CP_ACP, 0, apeer, -1, wa, MAX_PATH);
    map_set(wpeer, local);
    map_set(wa, local);
    {
        unsigned char *d = (unsigned char *)malloc(n);
        size_t dn = n;
        memcpy(d, img, n);
        samples_rewrite(&d, &dn);
        printf("image %zu -> %zu bytes\n", n, dn);
        if (dn != n) fails++;
        memset(&L, 0, sizeof(L));
        flp_walk(d, dn, collect_cb, &L);
        for (i = 0; i < (size_t)L.n; i++) {
            printf("  now %S (exists %d)\n", L.paths[i], file_exists_w(L.paths[i]));
            if (!file_exists_w(L.paths[i]) || wcsstr(L.paths[i], L"Alex")) fails++;
        }
        if (memcmp(d + 22 + 3, "\x01\x02\x03\x04\x05\x06\x07", 7) != 0) { printf("junk before the path changed\n"); fails++; }
    }
    {   // any file a plugin loads: SF2 Player keeps its soundfont as a Delphi short string (length byte,
        // no end mark, 0xFF filler after it); a VST wrapper names its plugin binary after an 8-byte length
        // (a record id byte follows); a VST may write "D:/..." in UTF-16
        static const char sf2peer[] = "F:\\SF\\Piano.sf2", vst[] = "C:\\Program Files\\Common Files\\VST3\\OTT.vst3";
        const wchar_t *wslash = L"D:/Alex/Wavetables/Saw Stack.wav";
        wchar_t sf2local[MAX_PATH], wsf2[MAX_PATH];
        unsigned char *d;
        size_t dn, at;
        uint64_t vl = sizeof(vst) - 1;
        FILE *f = NULL;
        GetTempPathW(MAX_PATH, sf2local);
        wcscat_s(sf2local, MAX_PATH, L"flim_harness_piano.sf2");
        if (_wfopen_s(&f, sf2local, L"wb") == 0 && f) { fputs("sfbk", f); fclose(f); }
        pn = 0;
        memcpy(plug, "\x06\0\0\0\x01\0\0\0\xff\xff\xff\xff", 12); pn = 12;
        plug[pn++] = (unsigned char)(sizeof(sf2peer) - 1);
        memcpy(plug + pn, sf2peer, sizeof(sf2peer) - 1); pn += sizeof(sf2peer) - 1;
        memcpy(plug + pn, "\xff\xff\xff\xff\x00\x01", 6); pn += 6;
        memcpy(plug + pn, "\x02\0\0\0", 4); pn += 4;
        memcpy(plug + pn, &vl, 8); pn += 8;
        memcpy(plug + pn, vst, sizeof(vst) - 1); pn += sizeof(vst) - 1;
        memcpy(plug + pn, "8\0\0\0", 4); pn += 4;
        memcpy(plug + pn, wslash, wcslen(wslash) * 2 + 2); pn += wcslen(wslash) * 2 + 2;
        memcpy(img, "FLhd", 4); *(uint32_t *)(img + 4) = 6; memset(img + 8, 0, 6); n = 14;
        memcpy(img + n, "FLdt", 4); n += 8;
        n += put_ev(img + n, EV_PLUGIN_DATA, plug, pn);
        *(uint32_t *)(img + 18) = (uint32_t)(n - 22);

        memset(&L, 0, sizeof(L));
        flp_walk(img, n, collect_cb, &L);
        printf("plugin files: %d paths\n", L.n);
        for (i = 0; i < (size_t)L.n; i++) printf("  %S\n", L.paths[i]);
        MultiByteToWideChar(CP_ACP, 0, sf2peer, -1, wsf2, MAX_PATH);
        if (L.n != 2 || wcscmp(L.paths[0], wslash) != 0 || wcscmp(L.paths[1], wsf2) != 0) { printf("wrong paths\n"); fails++; }
        pathlist_free(&L);

        // receiver: the soundfont arrived at a longer local path than the 15 chars in the data: no
        // same-length name fits, so the short string takes the whole local path and a new length
        map_set(wsf2, sf2local);
        d = (unsigned char *)malloc(n);
        dn = n;
        memcpy(d, img, n);
        samples_rewrite(&d, &dn);
        memset(&L, 0, sizeof(L));
        flp_walk(d, dn, collect_cb, &L);
        printf("rewritten: %zu -> %zu bytes\n", n, dn);
        for (i = 0; i < (size_t)L.n; i++) printf("  now %S\n", L.paths[i]);
        if (L.n != 2 || wcscmp(L.paths[1], sf2local) != 0) { printf("soundfont not pointed at the local file\n"); fails++; }
        {
            char a[MAX_PATH];
            unsigned char *p;
            WideCharToMultiByte(CP_ACP, 0, sf2local, -1, a, MAX_PATH, NULL, NULL);
            p = find_bytes(d, dn, a);
            at = p ? (size_t)(p - d) : 0;
            if (!at || d[at - 1] != strlen(a) || memcmp(d + at + strlen(a), "\xff\xff\xff\xff\x00\x01", 6) != 0) {
                printf("length byte or what follows is wrong\n"); fails++;
            }
        }
        if (!find_bytes(d, dn, vst)) { printf("the plugin binary's path changed\n"); fails++; }
        pathlist_free(&L);
        free(d);
        DeleteFileW(sf2local);
    }
    {   // the shared folder: what's in it (any slashes or case, not Shared2), and the names peers may send
        static const struct { const wchar_t *rel; int ok; } R[] = {
            { L"kick.wav", 1 }, { L"Drums\\808 kick.wav", 1 }, { L"a b\\c.d\\e f.flac", 1 }, { L"x.fxp", 1 },
            { L"..\\x.wav", 0 }, { L"a\\..\\x.wav", 0 }, { L".\\x.wav", 0 }, { L"\\x.wav", 0 }, { L"a\\\\x.wav", 0 },
            { L"C:\\x.wav", 0 }, { L"x.wav:evil", 0 }, { L"a/x.wav", 0 }, { L"x.exe", 0 }, { L"x.dll", 0 },
            { L"x.vst3", 0 }, { L"x.lnk", 0 }, { L"noext", 0 }, { L"CON.wav", 0 }, { L"con", 0 }, { L"nul.txt", 0 },
            { L"a\\COM1.wav", 0 }, { L"LPT9.mp3", 0 }, { L"COM10.wav", 1 }, { L"CONX.wav", 1 }, { L"x.wav.", 0 },
            { L"dir.\\x.wav", 0 }, { L"dir \\x.wav", 0 }, { L"x?.wav", 0 }, { L"x\x01.wav", 0 }, { L"", 0 },
            { L"desktop.ini", 0 }, { L"a\\Thumbs.db", 0 }, { L"x.wav.part", 0 }, { L"~flim-0123.part", 0 }, { L"pack.zip.crdownload", 0 },
            { L"~$x.wav", 0 }, { L"x.hta", 0 }, { L"x.reg", 0 }, { L"x.js", 0 }, { L"x.url", 0 }, { L"Kits\\desktop.ini.wav", 1 },
        };
        int k;
        wchar_t t[MAX_PATH];
        wcscpy_s(g_shRoot, MAX_PATH, L"C:\\Users\\Public\\Documents\\flim\\Shared");
        if (!in_shared(L"c:/users/public/documents/flim/shared/Drums/kick.wav") || !in_shared(L"C:\\Users\\Public\\Documents\\flim\\Shared\\x.wav") ||
            in_shared(L"C:\\Users\\Public\\Documents\\flim\\Shared2\\x.wav") || in_shared(L"C:\\Users\\Public\\Documents\\flim\\Shared") ||
            in_shared(L"D:\\x.wav")) { printf("in_shared wrong\n"); fails++; }
        for (k = 0; k < (int)(sizeof(R) / sizeof(R[0])); k++)
            if (sh_rel_ok(R[k].rel) != R[k].ok) { printf("sh_rel_ok(\"%S\") should be %d\n", R[k].rel, R[k].ok); fails++; }
        if (!sh_target(L"Drums\\kick.wav", t, MAX_PATH) || wcscmp(t, L"C:\\Users\\Public\\Documents\\flim\\Shared\\Drums\\kick.wav") != 0) { printf("sh_target wrong\n"); fails++; }
        g_shRoot[0] = 0;
    }
    {   // a stuck main thread is reported with its stack: a thread parked in Sleep stands in for it
        DWORD tid;
        HANDLE th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Sleep, (LPVOID)(uintptr_t)3000, 0, &tid);
        char last[2048] = "", line[2048];
        FILE *f = NULL;
        wchar_t lp[MAX_PATH];
        Sleep(300);
        g_mainTid = tid; g_runType = T_SNAP; g_runUnit = 1; g_runA = 4; g_runB = 2;
        main_stuck(6000);
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
        GetTempPathW(MAX_PATH, lp);
        wcscat_s(lp, MAX_PATH, L"flim_engine.log");
        if (_wfopen_s(&f, lp, L"rb") == 0 && f) {
            while (fgets(line, sizeof(line), f)) if (strstr(line, "STUCK main thread:")) strcpy_s(last, sizeof(last), line);
            fclose(f);
        }
        printf("stuck report: %.200s", last);
        if (!strstr(last, "ntdll.dll+") || !strstr(last, "KERNEL")) { printf("no stack in the stuck report\n"); fails++; }
    }
    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails;
}

// Offline test of the plugin-data sample path code (collect + same-length rewrite).
// build.bat, then t_samples.exe: prints OK. Leaves small aliases under Documents\flim\samples.
#include "../../engine/flim_engine.c"

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
    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails;
}

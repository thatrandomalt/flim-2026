// Offline test of local solo against a fake mixer in place of FL's (its MIXARR / MIXCNT globals).
// build.bat, then t_solo.exe: prints OK.
#include "../../engine/flim_engine.c"

static char *g_tracks;
static int g_enabledCalls;
static void fake_set_enabled(int t, int on) { g_tracks[(size_t)t * MIX_STRIDE + MIX_ENABLED] = (char)on; g_enabledCalls++; }
static char *trk(int t, int off) { return g_tracks + (size_t)t * MIX_STRIDE + off; }
static uint32_t mute(int t) { Sleep(20); return read_value(MUTE_ID(t)); }    // past solo_on's once-per-tick cache

static size_t put_ev(unsigned char *o, int id, const void *p, size_t n) {
    size_t k = 0, v = n;
    o[k++] = (unsigned char)id;
    do { unsigned char b = (unsigned char)(v & 0x7f); v >>= 7; if (v) b |= 0x80; o[k++] = b; } while (v);
    memcpy(o + k, p, n);
    return k + n;
}

int main(void) {
    static char img[0x2000];                 // FL's image around the two globals
    static unsigned char flp[1024];
    int count = 8, fails = 0, t;
    size_t n;
    Slot s;
    g_tracks = (char *)calloc(count, MIX_STRIDE);
    g_flBase = img - (MIXARR_VA - IMG_BASE);
    *(char **)img = g_tracks;
    *(int **)(img + (MIXCNT_VA - MIXARR_VA)) = &count;
    g_setEnabled = fake_set_enabled;
    for (t = 0; t < count; t++) *trk(t, MIX_ENABLED) = 1;
    *trk(3, MIX_ENABLED) = 0;                                    // muted by the user

    // FL solos track 5: saves each track's state, flags 5, mutes the rest
    for (t = 0; t < count; t++) { *trk(t, MIX_PRESOLO) = *trk(t, MIX_ENABLED); *trk(t, MIX_SOLO) = t == 5; *trk(t, MIX_ENABLED) = t == 5; }
    if (mute(4) != 1 || mute(3) != 0 || mute(5) != 1) { printf("soloed: mutes read as the solo's, not the shared state\n"); fails++; }

    // someone mutes 4 and unmutes 3 meanwhile: kept for the unsolo, the solo stays as it is
    write_value(MUTE_ID(4), 0);
    write_value(MUTE_ID(3), 1);
    if (g_enabledCalls || *trk(4, MIX_PRESOLO) != 0 || *trk(3, MIX_PRESOLO) != 1 || *trk(3, MIX_ENABLED) != 0 || !*trk(5, MIX_SOLO)) {
        printf("soloed: a remote mute went through FL's mute (ends the solo) or got lost\n"); fails++;
    }
    if (mute(4) != 0 || mute(3) != 1) { printf("soloed: remote mutes don't read back\n"); fails++; }

    // a project snapshot meanwhile has no solo, and the shared mute states
    memcpy(flp, "FLhd", 4); *(uint32_t *)(flp + 4) = 6; memset(flp + 8, 0, 6); n = 14;
    memcpy(flp + n, "FLdt", 4); n += 8;
    for (t = 0; t < count; t++) {
        uint32_t ev[3] = { 0, 0x4c, 0 };
        ev[1] = (*trk(t, MIX_ENABLED) ? INS_ENABLED : 0) | (*trk(t, MIX_SOLO) ? INS_SOLO : 0) | 0x44;
        n += put_ev(flp + n, EV_INSFLAGS, ev, sizeof(ev));
    }
    *(uint32_t *)(flp + 18) = (uint32_t)(n - 22);
    { int k = 0; flp_walk(flp, n, solostrip_cb, &k); }
    for (t = 0; t < count; t++) {
        uint32_t f = *(uint32_t *)(flp + 22 + t * 14 + 2 + 4);
        uint32_t want = (*trk(t, MIX_PRESOLO) ? INS_ENABLED : 0) | 0x44;
        if (f != want) { printf("snapshot track %d flags 0x%x, want 0x%x\n", t, f, want); fails++; }
    }

    // a mute change counts on the second pass that reads it, and not while solo is changing
    memset(&s, 0, sizeof(s));
    s.id = MUTE_ID(2); s.last = 1;
    g_soloChangedAt = GetTickCount() - SOLO_HOLD_MS;
    if (mute_settled(&s, 0, GetTickCount()) || !mute_settled(&s, 0, GetTickCount())) { printf("mute change not confirmed on the second pass\n"); fails++; }
    if (mute_settled(&s, 0, GetTickCount()) || !mute_settled(&s, 1, GetTickCount())) { printf("mute pending not reset\n"); fails++; }
    g_soloChangedAt = GetTickCount();
    if (mute_settled(&s, 0, GetTickCount()) || mute_settled(&s, 0, GetTickCount())) { printf("mute change taken during the solo hold\n"); fails++; }

    // FL unsolos: each track gets its kept state back; nothing to tell anyone
    for (t = 0; t < count; t++) { *trk(t, MIX_SOLO) = 0; *trk(t, MIX_ENABLED) = *trk(t, MIX_PRESOLO); }
    if (mute(4) != 0 || mute(3) != 1 || mute(5) != 1) { printf("unsoloed: wrong mutes\n"); fails++; }
    write_value(MUTE_ID(6), 0);
    if (g_enabledCalls != 1 || *trk(6, MIX_ENABLED) != 0) { printf("not soloed: a remote mute must use FL's mute\n"); fails++; }

    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails;
}

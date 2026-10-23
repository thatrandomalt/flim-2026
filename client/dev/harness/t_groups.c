// Offline test of per-channel / per-track sync (group diff + merge), sample placeholders and the
// joined project's song folder + zip.
// build.bat builds it next to t_samples; t_groups.exe prints OK.
#include "../../engine/flim_engine.c"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void note(unsigned char *r, uint32_t pos, uint16_t ch, uint16_t key) {
    memset(r, 0, NOTE_SIZE);
    *(uint32_t *)r = pos; *(uint16_t *)(r + NOTE_CH_OFS) = ch; *(uint16_t *)(r + 12) = key;
}
static void clip(unsigned char *r, uint32_t pos, uint16_t trk, uint32_t id) {
    memset(r, 0, CLIP_SIZE);
    *(uint32_t *)r = pos; *(uint16_t *)(r + CLIP_TRK_OFS) = trk; *(uint32_t *)(r + CLIP_ID_OFS) = id;
}

int main(void) {
    unsigned char base[4 * NOTE_SIZE], cur[5 * NOTE_SIZE], *g;
    int gs[16], ng;
    unsigned n;
    InitializeCriticalSection(&g_sampleLock);
    InitializeCriticalSection(&g_ulockLock);
    InitializeCriticalSection(&g_outLock);                          // lock requests go to ws_send (not connected: dropped)
    InitializeCriticalSection(&g_sendLock);

    // channel 0: two notes, channel 1: one note
    note(base, 0, 0, 60); note(base + NOTE_SIZE, 96, 0, 62); note(base + 2 * NOTE_SIZE, 0, 1, 36);
    // same notes in another order, one selected, plus a new note on channel 1
    note(cur, 96, 0, 62); note(cur + NOTE_SIZE, 0, 1, 36); note(cur + 2 * NOTE_SIZE, 0, 0, 60);
    cur[NOTE_SEL_OFS] |= NOTE_SEL_BIT;
    note(cur + 3 * NOTE_SIZE, 48, 1, 38);
    ng = group_diff(1, cur, 4, base, 3, gs, 16);
    CHECK(ng == 1 && gs[0] == 1);                                   // only channel 1 changed
    ng = group_diff(1, cur, 3, base, 3, gs, 16);
    CHECK(ng == 0);                                                 // order + selection are not edits
    ng = group_diff(1, cur, 1, base, 3, gs, 16);                    // cur: just (96,ch0)
    CHECK(ng == 2 && gs[0] == 0 && gs[1] == 1);                     // ch0 lost a note, ch1 emptied
    g = recs_group(1, cur, 4, 1, &n);
    CHECK(n == 2 && rec_group(1, g) == 1 && rec_group(1, g + NOTE_SIZE) == 1);
    free(g);
    g = recs_group(1, cur, 4, 7, &n);
    CHECK(n == 0 && g == NULL);

    {   // two people in one pattern: A edits channel 0, B edits channel 1 at the same time
        UState flB, recv;
        unsigned char a0[2 * NOTE_SIZE];
        memset(&flB, 0, sizeof(flB)); memset(&recv, 0, sizeof(recv));
        unit_agreed(1, 5, base, 3, -1);                             // B's agreed/base = base
        ustate_set(&flB, base, 3, NOTE_SIZE);
        note(flB.d + 2 * NOTE_SIZE, 0, 1, 40);                      // B's unsent edit: ch1 note moved to key 40
        note(a0, 0, 0, 65); note(a0 + NOTE_SIZE, 192, 0, 67);       // A's notes.set for channel 0 arrives
        ustate_merge(&flB, 1, a0, 2, 0);                            // what notes_apply does to FL here
        unit_agreed(1, 5, flB.d, flB.n, 0);                         // ... and the read-back becomes agreed for ch0
        {
            unsigned char *b;
            unsigned bn;
            CHECK(unit_base(1, 5, &b, &bn));
            ng = group_diff(1, flB.d, flB.n, b, bn, gs, 16);
            CHECK(ng == 1 && gs[0] == 1);                           // B still sends its ch1 edit, no echo of ch0
            free(b);
        }
        CHECK(flB.n == 3);
        unit_before_send("pat:5:1", 1, 5, flB.d, flB.n, 1);         // B sends ch1 (lock request, not granted yet)
        {
            unsigned char *b;
            unsigned bn;
            unit_base(1, 5, &b, &bn);
            CHECK(group_diff(1, flB.d, flB.n, b, bn, gs, 16) == 0); // nothing left to send
            free(b);
            g = recs_group(1, g_patLast[5].d, g_patLast[5].n, 1, &n);
            CHECK(n == 1 && *(uint16_t *)(g + 12) == 36);           // agreed ch1 is still the old note until the grant
            free(g);
            unit_commit("pat:5:1");                                 // grant
            g = recs_group(1, g_patLast[5].d, g_patLast[5].n, 1, &n);
            CHECK(n == 1 && *(uint16_t *)(g + 12) == 40);
            free(g);
            g = recs_group(1, g_patLast[5].d, g_patLast[5].n, 0, &n);
            CHECK(n == 2 && *(uint16_t *)(g + 12) == 65);           // A's channel 0 is kept
            free(g);
        }
        free(flB.d); free(recv.d);
    }

    {   // clips: ids are per instance, tracks are the groups
        unsigned char a[3 * CLIP_SIZE], b[3 * CLIP_SIZE];
        clip(a, 0, 499, 1); clip(a + CLIP_SIZE, 384, 499, 2); clip(a + 2 * CLIP_SIZE, 0, 497, 3);
        clip(b, 384, 499, 17); clip(b + CLIP_SIZE, 0, 497, 18); clip(b + 2 * CLIP_SIZE, 0, 499, 19);
        CHECK(group_diff(0, a, 3, b, 3, gs, 16) == 0);              // renumbered + reordered: same playlist
        *(uint16_t *)(b + CLIP_TRK_OFS) = 498;                      // clip dragged from track 1 to track 2
        ng = group_diff(0, a, 3, b, 3, gs, 16);
        CHECK(ng == 2 && gs[0] == 498 && gs[1] == 499);
    }

    {   int isPat, idx, g2;
        CHECK(unit_key_parse("pat:12:3", &isPat, &idx, &g2) && isPat && idx == 12 && g2 == 3);
        CHECK(unit_key_parse("pat:12", &isPat, &idx, &g2) && isPat && g2 == -1);
        CHECK(unit_key_parse("clips:0:499", &isPat, &idx, &g2) && !isPat && idx == 0 && g2 == 499);
        CHECK(!unit_key_parse("plug:ch:3", &isPat, &idx, &g2));
        CHECK(!unit_key_parse("pat:0:1", &isPat, &idx, &g2));       // pattern 0 doesn't exist
    }

    {   // placeholders
        wchar_t p[MAX_PATH], q[MAX_PATH];
        char hx[65];
        uint64_t sz, ms;
        size_t fn;
        unsigned char *f;
        GetTempPathW(MAX_PATH, p);
        wcscpy_s(q, MAX_PATH, p);
        wcscat_s(p, MAX_PATH, L"flim_t_placeholder.wav");
        wcscat_s(q, MAX_PATH, L"flim_t_placeholder.aiff");
        CHECK(placeholder_write(p, 2500));
        ms = wav_ms(p);
        CHECK(ms >= 2490 && ms <= 2500);                            // a real WAV of the right length
        f = read_file_w(p, &fn);
        CHECK(f && fn == 44 + 20000 && f[44] == 0x80 && f[fn - 1] == 0x80);
        free(f);
        CHECK(placeholder_write(q, 1001));
        f = read_file_w(q, &fn);
        CHECK(f && memcmp(f, "FORM", 4) == 0 && memcmp(f + 8, "AIFFCOMM", 8) == 0 && memcmp(f + 38, "SSND", 4) == 0);
        CHECK(f && fn == 54 + 8008);                                // 8008 frames, even: no pad byte
        free(f);
        CHECK(!placeholder_write(L"C:\\nowhere\\x.mp3", 1000));     // no placeholder format for mp3
        placeholder_add(p, "ab", 123);
        CHECK(placeholder_of(p, hx, &sz) && strcmp(hx, "ab") == 0 && sz == 123);
        placeholder_done("ab");
        CHECK(!placeholder_of(p, hx, &sz));
        {   // the download fills the placeholder in place
            wchar_t src[MAX_PATH];
            GetTempPathW(MAX_PATH, src);
            wcscat_s(src, MAX_PATH, L"flim_t_real.bin");
            write_file_w(src, (const unsigned char *)"real sample data", 16);
            CHECK(overwrite_in_place(src, p));
            f = read_file_w(p, &fn);
            CHECK(f && fn == 16 && memcmp(f, "real sample data", 16) == 0);
            free(f);
            DeleteFileW(src);
        }
        DeleteFileW(p); DeleteFileW(q);
    }
    {   // FL's "plugin is missing" warning during our loads: the name between FL's highlight bars;
        // shown the first time per plugin, answered (not shown) after that
        wchar_t nm[128];
        CHECK(plugin_warning(L"Warning|The plugin |Serum 2| is missing. Please make sure it was installed correctly.", nm, 128) &&
              wcscmp(nm, L"Serum 2") == 0);
        CHECK(!plugin_warning(L"Warning|The plugin you want to open is of an unknown type.", nm, 128));
        CHECK(!plugin_warning(L"Delete tracks|This will |delete| 2 selected mixer track(s).", nm, 128));
        CHECK(plugin_warning_seen(L"Serum 2") == 0 && plugin_warning_seen(L"Serum 2") == 1 && plugin_warning_seen(L"Pigments") == 0);
    }
    {   // plugin window bytes: a VST wrapper state with the editor open at one place vs closed at
        // another hashes the same; loading keeps our own window bytes
        static unsigned char s1[160], s2[160];
        unsigned char *ev212, *rec2;
        size_t n = 0;
        static const wchar_t NAME[] = L"Fruity Wrapper";
        memcpy(s1, "FLhd\x06\x00\x00\x00\x00\x00\x02\x00\x60\x00" "FLdt\x00\x00\x00\x00", 22); n = 22;
        s1[n++] = 201; s1[n++] = sizeof(NAME); memcpy(s1 + n, NAME, sizeof(NAME)); n += sizeof(NAME);
        s1[n++] = 212; s1[n++] = 52; ev212 = s1 + n; n += 52;
        s1[n++] = 213; s1[n++] = 4 + 12 + 25; s1[n] = 0x0c; n += 4;
        *(uint32_t *)(s1 + n) = 2; *(uint64_t *)(s1 + n + 4) = 25; rec2 = s1 + n + 12; n += 12 + 25;
        *(uint32_t *)(s1 + 18) = (uint32_t)(n - 22);
        ev212[16] = 0x41; *(int32_t *)(ev212 + 36) = 112; *(int32_t *)(ev212 + 40) = 168;    // open at 112,168
        *(int32_t *)(ev212 + 44) = 236; *(int32_t *)(ev212 + 48) = 458;                    // 236x458
        *(int32_t *)(rec2 + 1) = 567; *(int32_t *)(rec2 + 5) = 805; rec2[12] = 0x7d;     // editor 567x805
        memcpy(s2, s1, n);
        s2[ev212 - s1 + 16] = 0x40; *(int32_t *)(s2 + (ev212 - s1) + 36) = 300;            // closed, moved,
        *(int32_t *)(s2 + (ev212 - s1) + 44) = 235; *(int32_t *)(s2 + (ev212 - s1) + 48) = 457;   // a pixel smaller
        *(int32_t *)(s2 + (rec2 - s1) + 1) = 160; *(int32_t *)(s2 + (rec2 - s1) + 5) = 25;
        CHECK(state_hash(s1, n) == state_hash(s2, n));
        s2[rec2 - s1 + 12] = 0x7e;                                  // a real state byte
        CHECK(state_hash(s1, n) != state_hash(s2, n));
        winbytes_keep(s2, n, s1, n);                                // loading s2 while we have s1
        CHECK(s2[ev212 - s1 + 16] == 0x41 && *(int32_t *)(s2 + (ev212 - s1) + 36) == 112 && *(int32_t *)(s2 + (rec2 - s1) + 1) == 567);
        CHECK(*(int32_t *)(s2 + (ev212 - s1) + 44) == 236 && *(int32_t *)(s2 + (ev212 - s1) + 48) == 458);
        winbytes_keep(s2, n, NULL, 0);                              // nothing of ours: editor stays closed
        CHECK(s2[ev212 - s1 + 16] == 0x40);
    }
    {   // a zip in plugin data (Kilohearts' states): saves 2 s apart hash the same, a real byte doesn't,
        // and neither do the times in bytes that only look like an archive
        static unsigned char s[2][160];
        unsigned char *z = NULL;
        size_t n = 0;
        int k;
        for (k = 0; k < 2; k++) {
            unsigned char *x = s[k];
            memcpy(x, "FLhd\x06\x00\x00\x00\x00\x00\x02\x00\x60\x00" "FLdt\x00\x00\x00\x00", 22); n = 22;
            x[n++] = 213; x[n++] = 4 + 112; n += 4;                  // plugin data: 4 other bytes, then the archive
            z = x + n; n += 112;
            memcpy(z, "PK\3\4", 4); z[26] = 6; memcpy(z + 30, "s.json{}", 8);                 // local header, name, data
            memcpy(z + 38, "PK\1\2", 4); z[38 + 28] = 6; memcpy(z + 38 + 46, "s.json", 6);    // its directory record (offset 0)
            memcpy(z + 90, "PK\5\6", 4); z[90 + 8] = z[90 + 10] = 1; z[90 + 12] = 52; z[90 + 16] = 38;   // 1 entry, 52 bytes at 38
            *(uint16_t *)(z + 10) = *(uint16_t *)(z + 38 + 12) = (uint16_t)(0x189d + k);       // DOS time: s[1] 2 s later
            *(uint16_t *)(z + 12) = *(uint16_t *)(z + 38 + 14) = 0x5d56;
            *(uint32_t *)(x + 18) = (uint32_t)(n - 22);
        }
        CHECK(state_hash(s[0], n) == state_hash(s[1], n));
        CHECK(*(uint16_t *)(z + 10) == 0x189e);                      // (only state_hash's copy is cleared)
        z[37] = ']';                                                  // a real state byte
        CHECK(state_hash(s[0], n) != state_hash(s[1], n));
        z[37] = '}';
        s[0][z - s[1] + 90 + 10] = z[90 + 10] = 2;                    // the end record counts an entry that isn't there
        CHECK(state_hash(s[0], n) != state_hash(s[1], n));
    }
    {   // mixer track structure on a fake mixer, with FL's record recycling as seen live: an insert
        // turns the old Current record into the first new track and adds a fresh Current; a delete
        // makes the deleted record the new Current. The diff must see just the inserts / deletes.
        static char img[0x2000], arr[10 * MIX_STRIDE];
        static void *old[10];
        int cnt, nd, ni, del[10], ins[10], from[10], got[4];
        static const int O[] = { 1, 2, 3, 4, 5, 6 }, INSL[] = { 1, 2, 6, 7, 3, 4, 5, 8 }, DELL[] = { 1, 2, 4, 5, 3 }, MOVL[] = { 1, 3, 2, 4, 5, 6 };
        int k;
        g_flBase = img - (MIXARR_VA - IMG_BASE);
        *(char **)img = arr;
        *(int **)(img + (MIXCNT_VA - MIXARR_VA)) = &cnt;
#define LAYOUT(L) do { cnt = (int)(sizeof(L) / sizeof(L[0])); for (k = 0; k < cnt; k++) *(void **)(arr + (size_t)k * MIX_STRIDE + MIX_SLOTS_OFF) = (void *)(uintptr_t)L[k]; } while (0)
        for (k = 0; k < 6; k++) old[k] = (void *)(uintptr_t)O[k];
        LAYOUT(INSL);                                               // 2 inserted at 2
        CHECK(mix_diff(old, 6, del, &nd, ins, &ni, from) == 8 && nd == 0 && ni == 2 && ins[0] == 2 && ins[1] == 3);
        CHECK(from[0] == 0 && from[1] == 1 && from[2] == -1 && from[3] == -1 && from[4] == 2 && from[6] == 4 && from[7] == 5);
        LAYOUT(DELL);                                               // track 2 deleted
        CHECK(mix_diff(old, 6, del, &nd, ins, &ni, from) == 5 && nd == 1 && del[0] == 2 && ni == 0 && from[2] == 3 && from[4] == 5);
        LAYOUT(MOVL);                                               // 1 and 2 swapped: not structural
        CHECK(mix_diff(old, 6, del, &nd, ins, &ni, from) == 6 && nd == 0 && ni == 0 && from[1] == 2 && from[2] == 1);
#undef LAYOUT
        CHECK(json_ints("{\"del\":[3,4,10],\"ins\":[]}", "\"del\":[", got, 4) == 3 && got[2] == 10);
        CHECK(json_ints("{\"del\":[3,4,10],\"ins\":[]}", "\"ins\":[", got, 4) == 0);
        g_flBase = NULL;
    }
    {   // a channel task queued before the channels moved still goes to its channel (fake channel list)
        static char img[64], list[0x20];
        static char *listp = list;
        void *items[2] = { (void *)0x1000, (void *)0x2000 };
        QItem it;
        g_flBase = img - (CHANLIST_VA - IMG_BASE);
        *(char ***)img = &listp;
        *(void ***)(list + 8) = items; *(int *)(list + 0x10) = 2;
        memset(&it, 0, sizeof(it));
        it.type = T_SNAP; it.unit = U_CH; it.a = 0; it.chobj = items[0];
        items[0] = (void *)0x2000; items[1] = (void *)0x1000;      // channel 1 moved up
        CHECK(task_rebind(&it) && it.a == 1);
        *(int *)(list + 0x10) = 1;                                   // ... and then deleted
        CHECK(!task_rebind(&it));
        it.chobj = NULL; it.a = 5;                                   // not bound to a channel: left alone
        CHECK(task_rebind(&it) && it.a == 5);
        g_flBase = NULL;
    }
    {   // structure counts (channels, mixer tracks, patterns): someone's edits made before structure changes they
        // hadn't seen move through them; a structure op made at another count of its kind crossed one (leader: skip)
        static const int mixIns[7] = { 0, 1, 2, -1, 3, 4, 5 };   // a mixer track inserted at 3 (6 -> 7 tracks)
        unsigned char *r;
        QItem it;
#define SC(c, m, p) (it.sc[0] = (c), it.sc[1] = (m), it.sc[2] = (p))
        g_sc[SC_CH] = 20; g_sc[SC_MIX] = 10; g_sc[SC_PAT] = 30; g_isLeader = 1;
        sc_done(SC_MIX, mixIns, 7);                                 // mixer 10 -> 11
        sc_done_del(1, 4);                                          // channel 1 of 4 deleted: channels 20 -> 21
        CHECK(g_sc[SC_CH] == 21 && g_sc[SC_MIX] == 11);
        memset(&it, 0, sizeof(it));
        it.type = T_PARAM; it.peer = 1; SC(21, 10, 30);
        it.id = 0x70000000u + (((4u << 6) | 0u) << 16) + 0x1fc0u;   // track 4's volume -> track 5's
        CHECK(sc_rebase(&it) && it.id == 0x70000000u + ((5u << 6) << 16) + 0x1fc0u);
        it.id = (2u << 16) | 5u; SC(20, 11, 30);                    // channel 2's param -> channel 1's
        CHECK(sc_rebase(&it) && it.id == ((1u << 16) | 5u));
        it.id = (1u << 16) | 5u; SC(20, 10, 30);                    // the deleted channel's: dropped
        CHECK(!sc_rebase(&it));
        it.id = (3u << 16) | 5u; SC(21, 11, 30);                    // made after both: as it is
        CHECK(sc_rebase(&it) && it.id == ((3u << 16) | 5u));
        it.id = (3u << 16) | CH_TARGET_SUB; it.v = 4; SC(21, 10, 30);   // a channel routed to track 4 -> track 5 (the
        CHECK(sc_rebase(&it) && it.id == ((3u << 16) | CH_TARGET_SUB) && it.v == 5);   // value is a track number)
        it.v = 0; SC(21, 10, 30);                                   // master stays master
        CHECK(sc_rebase(&it) && it.v == 0);
        CHECK(sc_mix_map_value(4, 10) == 5 && sc_mix_map_value(2, 10) == 2 && sc_mix_map_value(4, 11) == 4);
        memset(&it, 0, sizeof(it));
        it.type = T_META; it.peer = 1; SC(20, 10, 30);
        it.path = _strdup("{\"t\":\"op\",\"op\":{\"kind\":\"meta.set\",\"e\":\"rt\",\"i\":4,\"r\":[[0,12800],[3,100],[5,7]]}}");
        CHECK(sc_rebase(&it) && strcmp(it.path, "{\"t\":\"op\",\"op\":{\"kind\":\"meta.set\",\"e\":\"rt\",\"i\":5,\"r\":[[0,12800],[4,100],[6,7]]}}") == 0);
        free(it.path);
        memset(&it, 0, sizeof(it));
        it.type = T_NOTES; it.peer = 1; SC(20, 11, 30); it.b = -1; it.mode = 3;
        it.order = (int *)(r = (unsigned char *)calloc(3, NOTE_SIZE));
        r[NOTE_CH_OFS] = 0; r[NOTE_SIZE + NOTE_CH_OFS] = 2; r[2 * NOTE_SIZE + NOTE_CH_OFS] = 1;
        CHECK(sc_rebase(&it) && it.mode == 2 && r[NOTE_CH_OFS] == 0 && r[NOTE_SIZE + NOTE_CH_OFS] == 1);   // ch 1's note gone
        free(it.order);
        memset(&it, 0, sizeof(it));
        it.type = T_MIXSTRUCT; it.peer = 1; SC(21, 10, 30); it.path = _strdup("{}");
        CHECK(!sc_rebase(&it));                                     // crossed the mixer insert: skipped (and freed)
        SC(20, 11, 30); it.path = NULL;
        CHECK(sc_rebase(&it));                                      // only a channel op it hadn't seen: no crossing
        memset(&it, 0, sizeof(it));
        it.type = T_DELCH; it.peer = 1; SC(20, 11, 30);             // a channel op that hadn't seen the channel delete:
        CHECK(!sc_rebase(&it));                                     // crossed, whatever the other counts
        {   // pattern ops: the maps measured on FL 2026
            static int f[MAX_PAT];
            int p4 = 4, c36[2] = { 6, 3 }, d26[2] = { 2, 6 }, m3[1] = { 3 }, m34[2] = { 3, 4 }, m36[2] = { 3, 6 };
            CHECK(patop_map(PATOP_INSERT, &p4, 1, 0, 0, f) && f[3] == 3 && f[4] == -1 && f[5] == 4);
            CHECK(patop_map(0, c36, 2, 0, 0, f) && f[3] == 3 && f[4] == -1 && f[5] == 4 && f[7] == 6 && f[8] == -1 && f[9] == 7);
            CHECK(patop_map(1, d26, 2, 0, 0, f) && f[1] == 1 && f[2] == 3 && f[4] == 5 && f[5] == 7);
            CHECK(patop_map(2, m3, 1, 3, -1, f) && f[3] == 6 && f[6] == 3 && f[4] == 4);          // N1 N2 N6 N4 N5 N3
            CHECK(patop_map(2, m34, 2, 2, -1, f) && f[3] == 5 && f[4] == 6 && f[5] == 3 && f[6] == 4);   // N1 N2 N5 N6 N3 N4
            CHECK(patop_map(2, m36, 2, -1, -1, f) && f[2] == 3 && f[3] == 2 && f[5] == 6 && f[6] == 5);  // N1 N3 N2 N4 N6 N5
            CHECK(patop_map(2, m3, 1, 0, 7, f) && f[3] == 3 && f[7] == 7);                         // target mode: nothing
            CHECK(patop_map(3, m3, 1, 0, 0, f) && f[3] == 3);
            g_patPreCh[3] = 2;                                      // split: pattern 3 keeps channel one, a new one after it
            CHECK(patop_map(5, m3, 1, 0, 0, f) && f[3] == 3 && f[4] == -1 && f[5] == 4);
            patop_map(PATOP_INSERT, &p4, 1, 0, 0, f);
            sc_done(SC_PAT, f, MAX_PAT);                            // patterns 30 -> 31
        }
        memset(&it, 0, sizeof(it));
        it.type = T_NOTES; it.peer = 1; SC(21, 11, 30); it.a = 5; it.b = -1; it.mode = 0; it.order = (int *)malloc(1);
        CHECK(sc_rebase(&it) && it.a == 6);                         // pattern 5's notes -> pattern 6's
        free(it.order);
        memset(&it, 0, sizeof(it));
        it.type = T_CLIPS; it.peer = 1; SC(20, 11, 30); it.mode = 3;
        it.order = (int *)(r = (unsigned char *)calloc(3, CLIP_SIZE));
        *(uint32_t *)(r + 4) = (2u << 16) | 0x5000u;                // channel 2's clip -> channel 1's
        *(uint32_t *)(r + CLIP_SIZE + 4) = ((0x5000u + 5) << 16) | 0x5000u;   // pattern 5's -> pattern 6's
        *(uint32_t *)(r + 2 * CLIP_SIZE + 4) = (1u << 16) | 0x5000u;          // the deleted channel's: gone
        CHECK(sc_rebase(&it) && it.mode == 2 && *(uint32_t *)(r + 4) == ((1u << 16) | 0x5000u) &&
              *(uint32_t *)(r + CLIP_SIZE + 4) == (((0x5000u + 6) << 16) | 0x5000u));
        free(it.order);
        memset(&it, 0, sizeof(it));
        it.type = T_META; it.peer = 1; SC(21, 11, 30);
        it.path = _strdup("{\"t\":\"op\",\"op\":{\"kind\":\"meta.set\",\"e\":\"pt\",\"i\":4,\"c\":1,\"n\":\"\"}}");
        CHECK(sc_rebase(&it) && strstr(it.path, "\"i\":5,") != NULL);
        free(it.path);
        memset(&it, 0, sizeof(it));                                 // a new channel names mixer tracks (its automation
        it.type = T_ADDCH; it.peer = 1; SC(21, 10, 31);             // link): a mixer change crosses it, a pattern op doesn't
        CHECK(!sc_rebase(&it));
        SC(21, 11, 30);
        CHECK(sc_rebase(&it));
#undef SC
        g_isLeader = 0;
    }
    {   // FL 2026 projects: event byte 172 escapes a 32-bit id (here a varlen and a byte one); the
        // sample path after them must still be found
        static const unsigned char img[] = { 'F','L','h','d', 6,0,0,0, 0,0,2,0,0x60,0, 'F','L','d','t', 36,0,0,0,
            0xac, 0x01,0x01,0x00,0xc0, 4, 'a',0,'b',0,
            0xac, 0x00,0x01,0x00,0x00, 0x00,
            196, 18, 'C',0,':',0,'\\',0,'x',0,'.',0,'w',0,'a',0,'v',0,0,0 };
        PathList L;
        memset(&L, 0, sizeof(L));
        CHECK(flp_walk(img, sizeof(img), collect_cb, &L) && L.n == 1 && L.n && wcscmp(L.paths[0], L"C:\\x.wav") == 0);
        pathlist_free(&L);
    }
    {   // song folders: one per song, named without ".flp" / "_flim_MMDD_hhmm"
        wchar_t s[MAX_PATH];
        song_of(L"C:\\x\\My Song_flim_1021_2150.flp", s, MAX_PATH); CHECK(wcscmp(s, L"My Song") == 0);
        song_of(L"beat.flp", s, MAX_PATH);                          CHECK(wcscmp(s, L"beat") == 0);
        song_of(NULL, s, MAX_PATH);                                 CHECK(wcscmp(s, L"untitled") == 0);
        song_of(L"a:b.flp", s, MAX_PATH);                           CHECK(wcscmp(s, L"a_b") == 0);
    }
    {   // song folder: a name held by other content gets a hash suffix; a project zip has the .flp
        // and the samples it uses, but no placeholders
        wchar_t dir[MAX_PATH], a[MAX_PATH], ph[MAX_PATH], bak[MAX_PATH], zp[MAX_PATH], out[MAX_PATH * 2], flp[MAX_PATH];
        char hx[65];
        uint64_t sz;
        unsigned char *z, fd[1024];
        size_t zn, fk;
        int left = -1, i;
        CHECK(flimzip_crc(0, (const unsigned char *)"123456789", 9) == 0xCBF43926u);   // the CRC-32 check value
        GetTempPathW(MAX_PATH, dir);
        wcscat_s(dir, MAX_PATH, L"flim_t_song");
        SHCreateDirectoryExW(NULL, dir, NULL);
        swprintf_s(a, MAX_PATH, L"%s\\kick.wav", dir);
        swprintf_s(ph, MAX_PATH, L"%s\\snare.wav", dir);
        swprintf_s(bak, MAX_PATH, L"%s\\song.flp.bak", dir);
        swprintf_s(zp, MAX_PATH, L"%s.zip", dir);
        write_file_w(a, (const unsigned char *)"kick one", 8);
        write_file_w(ph, (const unsigned char *)"x", 1);
        write_file_w(bak, (const unsigned char *)"old", 3);
        placeholder_add(ph, "cd", 1);
        cache_path(dir, "1234567890abcdef", L"D:\\lib\\kick.wav", out, MAX_PATH * 2);
        CHECK(wcscmp(base_name(out), L"kick_12345678.wav") == 0);
        CHECK(sample_hash(a, hx, &sz));
        cache_path(dir, hx, L"D:\\lib\\kick.wav", out, MAX_PATH * 2);
        CHECK(_wcsicmp(out, a) == 0);                               // same content: same file
        {   // a minimal .flp using kick.wav and snare.wav (event 196: sample path)
            const wchar_t *use[2] = { a, ph };
            memcpy(fd, "FLhd\x06\0\0\0\0\0\0\0\0\0FLdt\0\0\0\0", 22);
            fk = 22;
            for (i = 0; i < 2; i++) {
                size_t L = (wcslen(use[i]) + 1) * 2;
                fd[fk++] = 196; fd[fk++] = (unsigned char)(0x80 | (L & 0x7f)); fd[fk++] = (unsigned char)(L >> 7);
                memcpy(fd + fk, use[i], L); fk += L;
            }
            *(uint32_t *)(fd + 18) = (uint32_t)(fk - 22);
            swprintf_s(flp, MAX_PATH, L"%s\\song_flim_1021_2150.flp", dir);
            write_file_w(flp, fd, fk);
        }
        CHECK(zip_project(flp, fd, fk, L"song.flp", zp, &left) == 2 && left == 1);   // the .flp and kick.wav
        z = read_file_w(zp, &zn);
        CHECK(z && zn > 22 + 30 + 8 + fk);
        if (z && zn > 22 + 30 + 8 + fk) {
            unsigned char *e = z + zn - 22, *k2 = z + 30 + 8 + fk;
            CHECK(*(uint32_t *)e == 0x06054b50 && *(uint16_t *)(e + 10) == 2);
            CHECK(*(uint32_t *)z == 0x04034b50 && memcmp(z + 30, "song.flp", 8) == 0);
            CHECK(*(uint32_t *)k2 == 0x04034b50 && memcmp(k2 + 30, "kick.wav", 8) == 0 && memcmp(k2 + 38, "kick one", 8) == 0);
            CHECK(*(uint32_t *)(k2 + 14) == flimzip_crc(0, (const unsigned char *)"kick one", 8) && *(uint32_t *)(k2 + 18) == 8);
        }
        free(z);
        placeholder_done("cd");
        DeleteFileW(a); DeleteFileW(ph); DeleteFileW(bak); DeleteFileW(zp); DeleteFileW(flp); RemoveDirectoryW(dir);
    }
    {   // a sample whose path is gone is found where FL finds it: same name, in the project's folder
        wchar_t d[MAX_PATH], prj[MAX_PATH], kick[MAX_PATH], got[MAX_PATH];
        GetTempPathW(MAX_PATH, d); wcscat_s(d, MAX_PATH, L"flim_locate");
        CreateDirectoryW(d, NULL);
        swprintf_s(prj, MAX_PATH, L"%s\\song.flp", d);
        swprintf_s(kick, MAX_PATH, L"%s\\kick 1.wav", d);
        write_file_w(kick, (const unsigned char *)"x", 1);
        CHECK(sample_locate(L"C:\\Users\\Someone\\AppData\\Local\\Temp\\Image-Line\\{1}\\kick 1.wav", prj, got, MAX_PATH) && wcscmp(got, kick) == 0);
        CHECK(sample_locate(kick, L"", got, MAX_PATH) && wcscmp(got, kick) == 0);          // still there: itself
        CHECK(!sample_locate(L"C:\\nowhere\\snare.wav", prj, got, MAX_PATH));
        CHECK(!sample_locate(L"C:\\nowhere\\kick 1.wav", L"", got, MAX_PATH));            // no project file
        DeleteFileW(kick); RemoveDirectoryW(d);
    }
    {   // a state saved by another FL build is the same state: version events don't count, real ones do
        static const unsigned char HD[] = "FLhd\x06\0\0\0\0\0\x01\0\x60\0FLdt";
        unsigned char x[3][96];
        size_t xn[3];
        int k;
        for (k = 0; k < 3; k++) {
            const char *ver = k == 1 ? "26.1.5.5613" : "26.1.7.5654";
            const wchar_t *nm = k == 1 ? L"FL Studio 26.1.5" : L"FL Studio 26.1.7";
            size_t o = 0, vl = strlen(ver) + 1, nl = (wcslen(nm) + 1) * 2;
            memcpy(x[k], HD, 18); o = 22;
            x[k][o++] = 199; x[k][o++] = (unsigned char)vl; memcpy(x[k] + o, ver, vl); o += vl;
            x[k][o++] = 159; *(uint32_t *)(x[k] + o) = k == 1 ? 5613 : 5654; o += 4;
            x[k][o++] = 172; *(uint32_t *)(x[k] + o) = 0xC0000101u; o += 4; x[k][o++] = (unsigned char)nl; memcpy(x[k] + o, nm, nl); o += nl;
            x[k][o++] = 64; x[k][o++] = 0; x[k][o++] = (unsigned char)(k == 2 ? 1 : 0);   // a real event (channel 0 / 1)
            *(uint32_t *)(x[k] + 18) = (uint32_t)(o - 22);
            xn[k] = o;
        }
        CHECK(state_hash(x[0], xn[0]) == state_hash(x[1], xn[1]));
        CHECK(state_hash(x[0], xn[0]) != state_hash(x[2], xn[2]));
    }
    {   // a big message's reference, relayed by the server, put back together with the real op
        char *m = op_splice("{\"t\":\"op\",\"seq\":7,\"from\":\"fl-1\",\"op\":{\"kind\":\"big\",\"h\":\"ab\",\"n\":9,\"key\":\"plug:fx:1:0\"}}",
                            "{\"t\":\"op\",\"op\":{\"kind\":\"plug.state\",\"key\":\"plug:fx:1:0\",\"data\":\"QUJD\"}}");
        CHECK(m && strcmp(m, "{\"t\":\"op\",\"seq\":7,\"from\":\"fl-1\",\"op\":{\"kind\":\"plug.state\",\"key\":\"plug:fx:1:0\",\"data\":\"QUJD\"}}") == 0);
        free(m);
        CHECK(op_splice("{\"t\":\"op\"}", "{\"t\":\"op\",\"op\":{}}") == NULL);
        CHECK(is_big_ref("{\"t\":\"op\",\"seq\":1,\"op\":{\"kind\":\"big\"}}") && !is_big_ref("{\"t\":\"op\",\"op\":{\"kind\":\"plug.state\"}}"));
    }
    {   // FL address signatures, on the installed FL's FLEngine_x64.dll mapped as an image (not run)
        static const unsigned char t[] = { 1, 2, 3, 0xcc, 0xcc, 0xcc, 0xcc, 9, 1, 2, 3 };
        const unsigned char *at = NULL;
        uint32_t fixed[FA_N];
        HMODULE m = LoadLibraryExW(L"C:\\Program Files\\Image-Line\\FL Studio 2026\\FLEngine_x64.dll", NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        CHECK(sig_find(t, sizeof(t), "01 02 03", &at) == 2 && at == t);
        CHECK(sig_find(t, sizeof(t), "?? cc 09 ?? 02", &at) == 1 && at == t + 5);
        CHECK(sig_find(t, sizeof(t), "03 09", &at) == 0);
        CHECK(sig_find(t, sizeof(t), "02 03 ?? ??", &at) == 1 && at == t + 1);   // the one at t + 9 would run past the end
        if (m) {
            memcpy(fixed, g_va, sizeof(fixed));
            g_flBase = (char *)((uintptr_t)m & ~(uintptr_t)3);
            CHECK(fl_addrs_resolve() == 1);                        // all found ("another build": no file version here)
            CHECK(memcmp(fixed, g_va, sizeof(fixed)) == 0);        // and equal to the 26.1.7.5654 table
            g_flBase = NULL;
            FreeLibrary(m);
        } else printf("(FL 2026 not installed: signatures not checked)\n");
    }
    printf(fails ? "FAIL (%d)\n" : "OK\n", fails);
    return fails;
}

// flim's built-in server (see host.h): a C port of server/src (server.ts, session.ts, blobs.ts) with
// what the engine uses: join_room, op, lock_acquire / lock_release, admin (host controls), bye, the ping
// liveness check and the sample store (POST /blobs/missing, HEAD / GET / PUT /blob/<sha256>), plus GET /health.
// One thread reads each connection; WebSocket clients also get a writer thread draining their send
// queue, so a slow connection never holds up the others. Session state is under one lock.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <natupnp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "host.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#define MAX_PAYLOAD   (64u * 1024 * 1024)          // a project snapshot in base64
#define MAX_QUEUED    (512u * 1024 * 1024)         // a client this far behind is dropped
#define LOCK_TTL_MS   30000                        // locks whose owner went quiet are released
#define PING_MS       15000
#define FILE_MAX      (2000ull * 1024 * 1024)
#define STORE_MAX     (4ull * 1024 * 1024 * 1024)  // sample store size; least recently used go first
#define HDR_MAX       16384
// Limits for a server open to the internet (as server/src/server.ts; from upstream flim's public server):
#define CONNS_PER_IP  12                           // open connections per address (WebSocket and HTTP)
#define FAIL_MAX      8                            // wrong passwords / sample store keys per address in FAIL_WINDOW_MS:
#define FAIL_WINDOW_MS (10 * 60 * 1000)            //   then refused until the window has passed
#define RATE_PER_S    400.0                        // messages per connection (token bucket), burst RATE_BURST;
#define RATE_BURST    4000.0                       //   beyond it the connection is closed
#define JOIN_TIMEOUT_MS 20000                      // a WebSocket must join a session this soon
#define IPS_MAX       512

typedef struct Buf { volatile LONG refs; size_t n; unsigned char d[1]; } Buf;
typedef struct QNode { Buf *b; struct QNode *next; } QNode;
typedef struct Session { char code[40], leader[72]; unsigned seq; struct Lock *locks; struct Ban *bans, *syncs; struct Session *next; int merge; } Session;
typedef struct Lock { char key[200], owner[72]; DWORD at; struct Lock *next; } Lock;
typedef struct Ban { char id[72], name[40], ip[64]; struct Ban *next; } Ban;   // removed (kick / IP ban) until let back in; sync-only
typedef struct Client {
    SOCKET s;
    Session *sess;
    char id[72], name[40], ip[64];                         // ip: the address it connects from (IP bans)
    int leader, sync;                                      // sync: sync-only (gets everything, sends nothing)
    volatile LONG alive, closed, refs;
    CRITICAL_SECTION ql;
    HANDLE qev;
    QNode *qh, *qt;
    size_t queued;
    double tokens;                                         // message rate (RATE_PER_S / RATE_BURST): its reader thread only
    DWORD tokensAt, since;                                 // since: connected (JOIN_TIMEOUT_MS)
    struct Client *next;
} Client;

static CRITICAL_SECTION g_lk;
static int     g_lkInit;
static SOCKET  g_ls = INVALID_SOCKET;
static volatile LONG g_run;
static Client *g_clients;
static Session *g_sessions;
static unsigned char g_pwHash[32];
static int     g_hasPw;
static char    g_keyHex[65];                       // sha256 hex of the password: the x-flim-key header
static wchar_t g_blobDir[MAX_PATH];
static HWND    g_notify;
static Ban    *g_ipBans;                            // IP bans: every session on this server (admin_do)
static volatile LONG g_leaderCtl;                  // the session leader may use the host controls too (host_leader_controls)
static volatile LONG g_maxPeople = 8;              // people per session, the leader included (host_set_max_people)
static volatile LONG g_hostMerge;                  // sessions started from now on merge notes (host_set_notes_merge)
// Per address: open connections and wrong passwords (g_ipLk). A slot is free again once it has neither.
typedef struct { char ip[64]; int conns, fails; DWORD failSince; } IpEnt;
static IpEnt   g_ips[IPS_MAX];
static CRITICAL_SECTION g_ipLk;
static IpEnt *ip_ent(const char *ip, int create) {         // (caller holds g_ipLk)
    int i, freeAt = -1;
    DWORD now = GetTickCount();
    for (i = 0; i < IPS_MAX; i++) {
        IpEnt *e = &g_ips[i];
        if (e->ip[0] && e->fails && now - e->failSince > FAIL_WINDOW_MS) e->fails = 0;
        if (e->ip[0] && !e->conns && !e->fails) e->ip[0] = 0;
        if (e->ip[0] && strcmp(e->ip, ip) == 0) return e;
        if (!e->ip[0] && freeAt < 0) freeAt = i;
    }
    if (!create || freeAt < 0) return NULL;
    memset(&g_ips[freeAt], 0, sizeof(IpEnt));
    strcpy_s(g_ips[freeAt].ip, sizeof(g_ips[freeAt].ip), ip);
    return &g_ips[freeAt];
}
static int ip_conn_begin(const char *ip) {                 // 0 = too many from there already
    IpEnt *e;
    int ok = 1;
    EnterCriticalSection(&g_ipLk);
    if ((e = ip_ent(ip, 1)) != NULL) { if (e->conns >= CONNS_PER_IP) ok = 0; else e->conns++; }
    LeaveCriticalSection(&g_ipLk);
    return ok;
}
static void ip_conn_end(const char *ip) {
    IpEnt *e;
    EnterCriticalSection(&g_ipLk);
    if ((e = ip_ent(ip, 0)) != NULL && e->conns > 0) e->conns--;
    LeaveCriticalSection(&g_ipLk);
}
static int ip_locked(const char *ip) {
    IpEnt *e;
    int locked;
    EnterCriticalSection(&g_ipLk);
    locked = (e = ip_ent(ip, 0)) != NULL && e->fails >= FAIL_MAX;
    LeaveCriticalSection(&g_ipLk);
    return locked;
}
static void hlog(const char *fmt, ...);
static void ip_fail(const char *ip) {
    IpEnt *e;
    int n = 0;
    EnterCriticalSection(&g_ipLk);
    if ((e = ip_ent(ip, 1)) != NULL) { if (!e->fails) e->failSince = GetTickCount(); n = ++e->fails; }
    LeaveCriticalSection(&g_ipLk);
    if (n == FAIL_MAX) hlog("%s: %d wrong passwords, refused for %d min", ip, FAIL_MAX, FAIL_WINDOW_MS / 60000);
}
static void peer_ip(SOCKET s, char *out, int cap) {
    struct sockaddr_storage pa;
    int pl = sizeof(pa);
    out[0] = 0;
    if (getpeername(s, (struct sockaddr *)&pa, &pl) != 0) return;
    if (pa.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&pa)->sin6_addr, out, cap);
    else inet_ntop(AF_INET, &((struct sockaddr_in *)&pa)->sin_addr, out, cap);
    if (_strnicmp(out, "::ffff:", 7) == 0) memmove(out, out + 7, strlen(out + 7) + 1);   // IPv4 on the dual-stack socket
}

// ---------------- log ----------------
static void hlog(const char *fmt, ...) {
    char a[600], path[MAX_PATH];
    wchar_t *w;
    SYSTEMTIME t;
    FILE *f = NULL;
    va_list ap;
    va_start(ap, fmt); vsprintf_s(a, sizeof(a), fmt, ap); va_end(ap);
    GetLocalTime(&t);
    GetTempPathA(MAX_PATH, path);
    strcat_s(path, MAX_PATH, "flim_host.log");
    if (fopen_s(&f, path, "a") == 0 && f) { fprintf(f, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, a); fclose(f); }
    if (!g_notify) return;
    w = (wchar_t *)malloc(700 * sizeof(wchar_t));
    swprintf_s(w, 700, L"%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    MultiByteToWideChar(CP_UTF8, 0, a, -1, w + wcslen(w), 690 - (int)wcslen(w));
    if (!PostMessageW(g_notify, WM_HOST_LOG, 0, (LPARAM)w)) free(w);
}
static void peers_changed(void) { if (g_notify) PostMessageW(g_notify, WM_HOST_PEERS, 0, 0); }

// ---------------- hashing ----------------
typedef struct { BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE h; } Hash;
static int hash_begin(Hash *x, LPCWSTR alg) {
    memset(x, 0, sizeof(*x));
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&x->a, alg, NULL, 0))) return 0;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(x->a, &x->h, NULL, 0, NULL, 0, 0))) { BCryptCloseAlgorithmProvider(x->a, 0); return 0; }
    return 1;
}
static void hash_add(Hash *x, const void *p, size_t n) { BCryptHashData(x->h, (PUCHAR)p, (ULONG)n, 0); }
static void hash_end(Hash *x, unsigned char *out, ULONG n) {
    BCryptFinishHash(x->h, out, n, 0);
    BCryptDestroyHash(x->h);
    BCryptCloseAlgorithmProvider(x->a, 0);
}
static void sha256(const void *p, size_t n, unsigned char out[32]) {
    Hash x;
    memset(out, 0, 32);
    if (hash_begin(&x, BCRYPT_SHA256_ALGORITHM)) { hash_add(&x, p, n); hash_end(&x, out, 32); }
}
static void tohex(const unsigned char *d, int n, char *out) {
    int i;
    for (i = 0; i < n; i++) sprintf_s(out + i * 2, 3, "%02x", d[i]);
}
static int is_hex64(const char *h) {
    int i;
    for (i = 0; i < 64; i++) if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return 0;
    return 1;
}
static int same32(const unsigned char *a, const unsigned char *b) {   // constant time
    int i, d = 0;
    for (i = 0; i < 32; i++) d |= a[i] ^ b[i];
    return d == 0;
}
static int password_ok(const char *given) {
    unsigned char h[32];
    if (!g_hasPw) return 1;
    sha256(given, strlen(given), h);
    return same32(h, g_pwHash);
}

// ---------------- JSON (just enough) ----------------
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
// End of the JSON value at p, or NULL if it is malformed.
static const char *js_skip(const char *p) {
    int depth = 0;
    p = js_ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p ? p + 1 : NULL;
    }
    if (*p != '{' && *p != '[') {
        while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') p++;
        return p;
    }
    for (; *p; p++) {
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
            if (!*p) return NULL;
        } else if (*p == '{' || *p == '[') depth++;
        else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
    }
    return NULL;
}
// The value of `key` in the object at obj, or NULL.
static const char *js_get(const char *obj, const char *key) {
    size_t kl = strlen(key);
    const char *p = js_ws(obj);
    if (*p != '{') return NULL;
    for (p++;;) {
        const char *ks, *ke;
        p = js_ws(p);
        if (*p != '"') return NULL;
        ks = p + 1;
        if (!(ke = js_skip(p))) return NULL;
        p = js_ws(ke);
        if (*p != ':') return NULL;
        p = js_ws(p + 1);
        if ((size_t)(ke - 1 - ks) == kl && memcmp(ks, key, kl) == 0) return p;
        if (!(p = js_skip(p))) return NULL;
        p = js_ws(p);
        if (*p != ',') return NULL;
        p++;
    }
}
// A string value, unescaped to UTF-8. 0 if v is no string.
static int js_str(const char *v, char *out, int cap) {
    int o = 0;
    if (!v || *v != '"') { if (cap) out[0] = 0; return 0; }
    for (v++; *v && *v != '"'; v++) {
        unsigned c = (unsigned char)*v;
        char enc[4];
        int k, ne = 1;
        if (c >= 0x80) { if (o < cap - 1) out[o++] = (char)c; continue; }   // raw UTF-8 passes through
        if (c == '\\' && v[1]) {
            v++;
            switch (*v) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                char h[5] = { 0 };
                for (k = 0; k < 4 && v[1]; k++) h[k] = *++v;
                c = (unsigned)strtoul(h, NULL, 16);
                if (c >= 0xD800 && c <= 0xDFFF) c = '?';      // no surrogate pairs (names are restricted anyway)
                break; }
            default: c = (unsigned char)*v;
            }
        }
        if (c < 0x80) enc[0] = (char)c;
        else if (c < 0x800) { enc[0] = (char)(0xC0 | (c >> 6)); enc[1] = (char)(0x80 | (c & 63)); ne = 2; }
        else { enc[0] = (char)(0xE0 | (c >> 12)); enc[1] = (char)(0x80 | ((c >> 6) & 63)); enc[2] = (char)(0x80 | (c & 63)); ne = 3; }
        for (k = 0; k < ne && o < cap - 1; k++) out[o++] = enc[k];
    }
    out[o] = 0;
    return 1;
}
static void js_esc(const char *in, char *out, int cap) {
    int o = 0;
    for (; *in && o < cap - 7; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += sprintf_s(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}
// Names are shown to peers: no control or quote-ish characters (as server.ts sanitizeUser).
static void clean(const char *in, char *out, int max) {
    int o = 0;
    for (; *in && o < max; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\' || c == '<' || c == '>') continue;
        out[o++] = (char)c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    while (out[0] == ' ') memmove(out, out + 1, strlen(out));
}

// ---------------- sending ----------------
static Buf *frame(int opcode, const void *p, size_t n) {
    size_t h = n < 126 ? 2 : n < 65536 ? 4 : 10;
    Buf *b = (Buf *)malloc(sizeof(Buf) + h + n);
    int i;
    b->refs = 1; b->n = h + n;
    b->d[0] = (unsigned char)(0x80 | opcode);
    if (n < 126) b->d[1] = (unsigned char)n;
    else if (n < 65536) { b->d[1] = 126; b->d[2] = (unsigned char)(n >> 8); b->d[3] = (unsigned char)n; }
    else { b->d[1] = 127; for (i = 0; i < 8; i++) b->d[2 + i] = (unsigned char)((uint64_t)n >> (56 - 8 * i)); }
    if (n) memcpy(b->d + h, p, n);
    return b;
}
static void buf_release(Buf *b) { if (InterlockedDecrement(&b->refs) == 0) free(b); }

static void client_release(Client *c) {
    if (InterlockedDecrement(&c->refs)) return;
    while (c->qh) { QNode *n = c->qh->next; buf_release(c->qh->b); free(c->qh); c->qh = n; }
    closesocket(c->s);
    CloseHandle(c->qev);
    DeleteCriticalSection(&c->ql);
    free(c);
}
static void enqueue(Client *c, Buf *b) {
    QNode *q;
    EnterCriticalSection(&c->ql);
    if (c->closed) { LeaveCriticalSection(&c->ql); return; }
    if (c->queued + b->n > MAX_QUEUED) {
        hlog("%s is too far behind: disconnected", c->name);
        c->closed = 1;
        shutdown(c->s, SD_BOTH);
        LeaveCriticalSection(&c->ql);
        SetEvent(c->qev);
        return;
    }
    q = (QNode *)malloc(sizeof(QNode));
    InterlockedIncrement(&b->refs);
    q->b = b; q->next = NULL;
    if (c->qt) c->qt->next = q; else c->qh = q;
    c->qt = q;
    c->queued += b->n;
    LeaveCriticalSection(&c->ql);
    SetEvent(c->qev);
}
static void send_str(Client *c, const char *s) {
    Buf *b = frame(1, s, strlen(s));
    enqueue(c, b);
    buf_release(b);
}
static int send_all(SOCKET s, const void *p, size_t n) {
    const char *q = (const char *)p;
    while (n) {
        int k = send(s, q, n > (1 << 30) ? (1 << 30) : (int)n, 0);
        if (k <= 0) return 0;
        q += k; n -= (size_t)k;
    }
    return 1;
}
static DWORD WINAPI writer_thread(LPVOID p) {
    Client *c = (Client *)p;
    for (;;) {
        QNode *q;
        WaitForSingleObject(c->qev, 1000);
        for (;;) {
            EnterCriticalSection(&c->ql);
            q = c->qh;
            if (q) { c->qh = q->next; if (!c->qh) c->qt = NULL; c->queued -= q->b->n; }
            LeaveCriticalSection(&c->ql);
            if (!q) break;
            if (!send_all(c->s, q->b->d, q->b->n)) { c->closed = 1; shutdown(c->s, SD_BOTH); }
            buf_release(q->b);
            free(q);
        }
        if (c->closed) break;
    }
    shutdown(c->s, SD_BOTH);                               // queued goodbyes are out: unblock the reader
    client_release(c);
    return 0;
}

// ---------------- sessions ----------------
static void peers_json(Session *s, char *out, int cap) {
    Client *c;
    int o = sprintf_s(out, cap, "[");
    for (c = g_clients; c; c = c->next) {
        char id[160], nm[100];
        if (c->sess != s || o > cap - 400) continue;
        js_esc(c->id, id, sizeof(id)); js_esc(c->name, nm, sizeof(nm));
        o += sprintf_s(out + o, cap - o, "%s{\"id\":\"%s\",\"name\":\"%s\",\"role\":\"%s\"%s}", o > 1 ? "," : "", id, nm, c->leader ? "leader" : "member",
                       c->sync ? ",\"sync\":true" : "");
    }
    sprintf_s(out + o, cap - o, "]");
}
// {"t":"presence","peers":[...],"leaderControls":<bool>,"banned":[{"id","name"[,"ip":true]}...]}: who is in, who leads,
// who is sync-only, who was removed (or IP banned), and whether the leader may use the host controls.
static void presence_msg(Session *s, char *out, int cap) {
    Ban *b;
    int k;
    int o = sprintf_s(out, cap, "{\"t\":\"presence\",\"peers\":");
    peers_json(s, out + o, cap - o - 600);
    o += (int)strlen(out + o);
    o += sprintf_s(out + o, cap - o, ",\"leaderControls\":%s,\"banned\":[", g_leaderCtl ? "true" : "false");
    for (k = 0; k < 2; k++)                                // removals, then IP bans (never the address itself)
        for (b = k ? g_ipBans : s->bans; b && o < cap - 400; b = b->next) {
            char id[160], nm[100];
            js_esc(b->id, id, sizeof(id)); js_esc(b->name, nm, sizeof(nm));
            o += sprintf_s(out + o, cap - o, "%s{\"id\":\"%s\",\"name\":\"%s\"%s}", out[o - 1] == '[' ? "" : ",", id, nm, k ? ",\"ip\":true" : "");
        }
    sprintf_s(out + o, cap - o, "]}");
}
static void broadcast(Session *s, const char *msg, Client *except) {
    Client *c;
    Buf *b = frame(1, msg, strlen(msg));
    for (c = g_clients; c; c = c->next) if (c->sess == s && c != except) enqueue(c, b);
    buf_release(b);
}
static void lock_msg(char *out, int cap, const char *key, int granted, const char *owner) {
    char k[420], o[160];
    js_esc(key, k, sizeof(k)); js_esc(owner, o, sizeof(o));
    sprintf_s(out, cap, "{\"t\":\"lock\",\"key\":\"%s\",\"granted\":%s,\"owner\":\"%s\"}", k, granted ? "true" : "false", o);
}
static void released_msg(char *out, int cap, const char *key) {
    char k[420];
    js_esc(key, k, sizeof(k));
    sprintf_s(out, cap, "{\"t\":\"lock_released\",\"key\":\"%s\"}", k);
}
static void send_error(Client *c, const char *code, const char *msg) {
    char m[400], e[300];
    js_esc(msg, e, sizeof(e));
    sprintf_s(m, sizeof(m), "{\"t\":\"error\",\"code\":\"%s\",\"msg\":\"%s\"}", code, e);
    send_str(c, m);
}

// caller holds g_lk
static void session_add(Session *s, Client *c) {
    static char peers[16384], m[17000];
    char id[160], nm[100], code[90];
    Lock *l;
    int o;
    c->sess = s;
    peers_json(s, peers, sizeof(peers));
    js_esc(c->id, id, sizeof(id)); js_esc(c->name, nm, sizeof(nm)); js_esc(s->code, code, sizeof(code));
    sprintf_s(m, sizeof(m), "{\"t\":\"welcome\",\"session\":\"%s\",\"role\":\"%s\",\"you\":{\"id\":\"%s\",\"name\":\"%s\"},\"peers\":%s,\"notes\":\"%s\"}",
              code, c->leader ? "leader" : "member", id, nm, peers, s->merge ? "merge" : "lock");
    send_str(c, m);
    send_str(c, "{\"t\":\"snapshot\",\"flp\":null,\"epoch\":0}");
    o = sprintf_s(m, sizeof(m), "{\"t\":\"lock_state\",\"locks\":[");
    for (l = s->locks; l && o < (int)sizeof(m) - 700; l = l->next) {
        char k[420], ow[160];
        js_esc(l->key, k, sizeof(k)); js_esc(l->owner, ow, sizeof(ow));
        o += sprintf_s(m + o, sizeof(m) - o, "%s{\"key\":\"%s\",\"owner\":\"%s\"}", l == s->locks ? "" : ",", k, ow);
    }
    sprintf_s(m + o, sizeof(m) - o, "]}");
    send_str(c, m);
    presence_msg(s, m, sizeof(m));
    broadcast(s, m, NULL);                                // (the newcomer too: leaderControls, who was removed)
    peers_changed();
}
static void session_free(Session *s) {
    Session **pp;
    while (s->locks) { Lock *n = s->locks->next; free(s->locks); s->locks = n; }
    while (s->bans) { Ban *n = s->bans->next; free(s->bans); s->bans = n; }
    while (s->syncs) { Ban *n = s->syncs->next; free(s->syncs); s->syncs = n; }
    for (pp = &g_sessions; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
    free(s);
}
// A client left (caller holds g_lk): its locks go; if it led, the longest-connected member leads now.
static void locks_drop(Session *s, const char *owner) {     // (caller holds g_lk)
    Lock **pl;
    char m[600];
    for (pl = &s->locks; *pl; ) {
        if (strcmp((*pl)->owner, owner) == 0) {
            Lock *d = *pl;
            *pl = d->next;
            released_msg(m, sizeof(m), d->key);
            broadcast(s, m, NULL);
            free(d);
        } else pl = &(*pl)->next;
    }
}
static void presence_all(Session *s) {
    static char m[20000];
    presence_msg(s, m, sizeof(m));                         // the roles in it tell everyone who leads
    broadcast(s, m, NULL);
}
static void session_remove(Client *c) {
    Session *s = c->sess;
    Client *o, *heir = NULL;
    if (!s) return;
    c->sess = NULL;
    locks_drop(s, c->id);
    for (o = g_clients; o; o = o->next) if (o->sess == s) heir = o;     // newest first: the last is the oldest
    if (c->leader && heir) {
        c->leader = 0;
        heir->leader = 1;
        heir->sync = 0;
        strcpy_s(s->leader, sizeof(s->leader), heir->id);
        hlog("%s (leader) left %s: %s leads now", c->name, s->code, heir->name);
    } else hlog("%s left %s", c->name, s->code);
    if (heir) presence_all(s);
    else session_free(s);
    peers_changed();
}

// ---------------- host controls ----------------
// Give host (make someone the session leader: their FL holds the project from now on; nobody reloads), remove
// someone (kick: they can't join this session again until let back in, or until it ends), ban their IP address
// (ban_ip: removed, and nobody from that address, id or name joins any session on this server until let back in
// or the host stops), sync-only (they get everything but nothing of theirs goes to the others; their locks go;
// it sticks if they reconnect). From this PC's window (host_admin, by = NULL), or from the session leader ("admin"
// message) when host_leader_controls allows it. Caller holds g_lk. Returns "" when done, else why not.
static Ban *ban_find(Ban *l, const char *id, const char *name, const char *ip) {
    for (; l; l = l->next)
        if (strcmp(l->id, id) == 0 || _stricmp(l->name, name) == 0 || (ip && ip[0] && l->ip[0] && strcmp(l->ip, ip) == 0)) return l;
    return NULL;
}
static void ban_add(Ban **l, const Client *t, int ip) {
    Ban *b;
    if (ban_find(*l, t->id, t->name, NULL)) return;
    b = (Ban *)calloc(1, sizeof(Ban));
    strcpy_s(b->id, sizeof(b->id), t->id); strcpy_s(b->name, sizeof(b->name), t->name);
    if (ip) strcpy_s(b->ip, sizeof(b->ip), t->ip);
    b->next = *l; *l = b;
}
static int ban_del(Ban **l, const char *id, char *name, int cap) {   // 1 = it was there
    for (; *l; l = &(*l)->next)
        if (strcmp((*l)->id, id) == 0) {
            Ban *d = *l;
            *l = d->next;
            strcpy_s(name, cap, d->name);
            free(d);
            return 1;
        }
    return 0;
}
static int banned(Session *s, const Client *c) {
    return ban_find(s->bans, c->id, c->name, NULL) || ban_find(g_ipBans, c->id, c->name, c->ip);
}
static const char *admin_do(Client *by, const char *action, const char *id, int on) {
    Client *t = NULL, *c;
    Session *s = NULL;
    const char *who = by ? by->name : "The host";
    char m[400], e[300];
    if (strcmp(action, "unban") == 0) {                   // lifts a removal and an IP ban
        char name[40] = "";
        int got = ban_del(&g_ipBans, id, name, sizeof(name));
        for (s = by ? by->sess : g_sessions; s; s = by ? NULL : s->next) got |= ban_del(&s->bans, id, name, sizeof(name));
        if (!got) return "Nobody by that id was removed.";
        hlog("%s let %s back in", who, name);
        for (s = g_sessions; s; s = s->next) presence_all(s);
        peers_changed();
        return "";
    }
    for (c = g_clients; c && !t; c = c->next) if (c->sess && strcmp(c->id, id) == 0 && (!by || c->sess == by->sess)) t = c;
    if (!t) return "That person isn't in the session (any more).";
    s = t->sess;
    if (strcmp(action, "give_host") == 0) {
        if (t->leader) return "";
        for (c = g_clients; c; c = c->next) if (c->sess == s) c->leader = 0;
        t->leader = 1;
        t->sync = 0;
        ban_del(&s->syncs, t->id, m, 40);
        strcpy_s(s->leader, sizeof(s->leader), t->id);
        hlog("%s made %s the session leader", who, t->name);
    } else if (strcmp(action, "kick") == 0 || strcmp(action, "ban_ip") == 0) {
        int ip = action[0] == 'b';
        if (t == by) return "You can't remove yourself.";
        if (ip) ban_add(&g_ipBans, t, 1); else ban_add(&s->bans, t, 0);
        hlog("%s %s %s (%s)", who, ip ? "banned the IP address of" : "removed", t->name, ip ? t->ip : s->code);
        sprintf_s(e, sizeof(e), ip ? "%s banned you from this server" : "%s removed you from the session", who);
        js_esc(e, m, 300);
        sprintf_s(e, sizeof(e), "{\"t\":\"error\",\"code\":\"kicked\",\"msg\":\"%s\"}", m);
        send_str(t, e);
        InterlockedExchange(&t->closed, 1); SetEvent(t->qev);   // the writer sends it, then hangs up: session_remove
    } else if (strcmp(action, "sync_only") == 0) {
        char nm[40];
        if (t->leader && on) return "The session leader can't be sync-only: make someone else the leader first.";
        if (on) ban_add(&s->syncs, t, 0); else ban_del(&s->syncs, t->id, nm, sizeof(nm));   // (kept for a reconnect)
        if (t->sync == on) return "";
        t->sync = on;
        if (on) locks_drop(s, t->id);
        hlog("%s set %s to %s", who, t->name, on ? "sync-only" : "normal (edits reach everyone again)");
    } else return "Unknown action.";
    presence_all(s);
    peers_changed();
    return "";
}

static Lock *lock_find(Session *s, const char *key) {
    Lock *l;
    for (l = s->locks; l; l = l->next) if (strcmp(l->key, key) == 0) return l;
    return NULL;
}

// One text message from a client (msg is NUL-terminated).
static void on_message(Client *c, char *msg, size_t n) {
    char t[32], m[1200];
    (void)n;
    {   // message rate: a token bucket per connection (its reader thread is the only one here)
        DWORD now = GetTickCount();
        c->tokens += (now - c->tokensAt) / 1000.0 * RATE_PER_S;
        if (c->tokens > RATE_BURST) c->tokens = RATE_BURST;
        c->tokensAt = now;
        if (c->tokens < 1) {
            hlog("%s: more than %d messages a second, disconnected", c->name[0] ? c->name : c->ip, (int)RATE_PER_S);
            send_error(c, "rate", "Too many messages: disconnected.");
            InterlockedExchange(&c->closed, 1); SetEvent(c->qev);
            return;
        }
        c->tokens -= 1;
    }
    js_str(js_get(msg, "t"), t, sizeof(t));
    EnterCriticalSection(&g_lk);
    if (c->closed) { LeaveCriticalSection(&g_lk); return; }   // removed (kick): nothing of theirs counts any more
    if (!c->sess) {
        const char *u = js_get(msg, "user");
        char room[80], pw[300], raw[200];
        Session *s;
        if (strcmp(t, "join_room") != 0) { send_error(c, "not_joined", "Send join_room first."); LeaveCriticalSection(&g_lk); return; }
        js_str(js_get(msg, "password"), pw, sizeof(pw));
        if (u) { js_str(js_get(u, "id"), raw, sizeof(raw)); clean(raw, c->id, 64); js_str(js_get(u, "name"), raw, sizeof(raw)); clean(raw, c->name, 24); }
        if (!c->id[0]) sprintf_s(c->id, sizeof(c->id), "anon-%u", (unsigned)GetTickCount());
        if (!c->name[0]) strcpy_s(c->name, sizeof(c->name), "Anonymous");
        if (ip_locked(c->ip)) {
            SecureZeroMemory(pw, sizeof(pw));
            send_error(c, "too_many_failures", "Too many wrong passwords from your address: try again in a few minutes.");
            c->id[0] = 0;
            InterlockedExchange(&c->closed, 1); SetEvent(c->qev);
            LeaveCriticalSection(&g_lk);
            return;
        }
        if (!password_ok(pw)) {
            SecureZeroMemory(pw, sizeof(pw));
            ip_fail(c->ip);
            hlog("wrong password from %s (%s)", c->name, c->ip);
            send_error(c, "bad_password", "Wrong server password.");
            c->id[0] = 0;
            LeaveCriticalSection(&g_lk);
            Sleep(1000);                                  // makes guessing slow
            InterlockedExchange(&c->closed, 1); SetEvent(c->qev);
            return;
        }
        SecureZeroMemory(pw, sizeof(pw));
        js_str(js_get(msg, "room"), room, sizeof(room));
        {
            int i;
            if (!room[0]) strcpy_s(room, sizeof(room), "FLIM");
            room[32] = 0;
            for (i = 0; room[i]; i++) room[i] = (char)toupper((unsigned char)room[i]);
        }
        for (s = g_sessions; s && strcmp(s->code, room) != 0; s = s->next) {}
        if (s && banned(s, c)) {
            hlog("%s was removed from %s: not let in", c->name, room);
            send_error(c, "kicked", "You were removed from this session. The host can let you back in");
            c->id[0] = 0;
            InterlockedExchange(&c->closed, 1); SetEvent(c->qev);
            LeaveCriticalSection(&g_lk);
            return;
        }
        if (!strstr(msg, "\"notes.merge\"") && (s ? s->merge : g_hostMerge)) {   // (its edits would never reach the others)
            hlog("%s: this session merges notes, which their flim can't do: not let in", c->name);
            send_error(c, "notes_merge", "This session merges notes, which your flim can't do yet. Update flim to join.");
            c->id[0] = 0;
            InterlockedExchange(&c->closed, 1); SetEvent(c->qev);
            LeaveCriticalSection(&g_lk);
            return;
        }
        if (!s) {
            s = (Session *)calloc(1, sizeof(Session));
            s->merge = g_hostMerge;
            strcpy_s(s->code, sizeof(s->code), room);
            strcpy_s(s->leader, sizeof(s->leader), c->id);
            s->next = g_sessions; g_sessions = s;
            c->leader = 1;
            hlog("%s started session %s (leader)", c->name, room);
        } else {
            int others = 0;
            Client *o, *p;
            for (o = g_clients; o; o = o->next) {             // people other than this one (a reconnect's old connection
                if (o->sess != s || strcmp(o->id, c->id) == 0) continue;   // may still be here until it times out)
                for (p = g_clients; p != o && !(p->sess == s && strcmp(p->id, o->id) == 0); p = p->next) {}
                if (p == o) others++;
            }
            if (others >= g_maxPeople) {
                char e[120];
                hlog("%s: %s is full (%ld people), not let in", c->name, room, g_maxPeople);
                sprintf_s(e, sizeof(e), "The session is full (%ld people).", g_maxPeople);
                send_error(c, "full", e);
                c->id[0] = 0;
                InterlockedExchange(&c->closed, 1); SetEvent(c->qev);
                LeaveCriticalSection(&g_lk);
                return;
            }
            c->sync = ban_find(s->syncs, c->id, c->name, NULL) != NULL;   // sync-only sticks across a reconnect
            hlog("%s joined %s%s", c->name, room, c->sync ? " (sync-only)" : "");
        }
        session_add(s, c);
        LeaveCriticalSection(&g_lk);
        return;
    }
    if (strcmp(t, "op") == 0) {
        const char *op = js_get(msg, "op"), *end = op ? js_skip(op) : NULL;
        char kind[40] = "", key[200] = "";
        Session *s = c->sess;
        Lock *l;
        if (!end) { LeaveCriticalSection(&g_lk); return; }
        js_str(js_get(op, "kind"), kind, sizeof(kind));
        js_str(js_get(op, "key"), key, sizeof(key));
        if (c->sync && strcmp(kind, "state.req") != 0 && strcmp(kind, "hello") != 0 && strcmp(kind, "shared.need") != 0 && strcmp(kind, "notes.mergereq") != 0) {
            LeaveCriticalSection(&g_lk);                  // sync-only: only asking for things goes through
            return;
        }
        if (strcmp(kind, "cur") != 0 && strcmp(kind, "alive") != 0)   // editing keeps the user's locks fresh
            for (l = s->locks; l; l = l->next) if (strcmp(l->owner, c->id) == 0) l->at = GetTickCount();
        if (key[0] && (l = lock_find(s, key)) != NULL && strcmp(l->owner, c->id) != 0) {
            char e[300];
            sprintf_s(e, sizeof(e), "Unit %.150s is locked by %.60s", key, l->owner);
            send_error(c, "locked", e);
        } else {
            size_t rl = (size_t)(end - op), cap = rl + 200;
            char *out = (char *)malloc(cap), id[160];
            int o;
            js_esc(c->id, id, sizeof(id));
            o = sprintf_s(out, cap, "{\"t\":\"op\",\"seq\":%u,\"from\":\"%s\",\"op\":", ++s->seq, id);
            memcpy(out + o, op, rl);
            out[o + rl] = '}'; out[o + rl + 1] = 0;
            broadcast(s, out, c);
            free(out);
        }
    } else if (strcmp(t, "lock_acquire") == 0 || strcmp(t, "lock_release") == 0) {
        char key[200];
        Session *s = c->sess;
        Lock *l;
        js_str(js_get(msg, "key"), key, sizeof(key));
        l = lock_find(s, key);
        if (c->sync) {}                                    // sync-only: edits nothing, locks nothing
        else if (t[5] == 'a') {
            DWORD now = GetTickCount();
            if (l && strcmp(l->owner, c->id) != 0 && now - l->at < LOCK_TTL_MS) {
                lock_msg(m, sizeof(m), key, 0, l->owner);
                send_str(c, m);
            } else {
                if (!l) { l = (Lock *)calloc(1, sizeof(Lock)); strcpy_s(l->key, sizeof(l->key), key); l->next = s->locks; s->locks = l; }
                strcpy_s(l->owner, sizeof(l->owner), c->id);
                l->at = now;
                lock_msg(m, sizeof(m), key, 1, c->id);
                broadcast(s, m, NULL);                    // the owner and everyone else
            }
        } else if (l && strcmp(l->owner, c->id) == 0) {
            Lock **pp;
            for (pp = &s->locks; *pp != l; pp = &(*pp)->next) {}
            *pp = l->next;
            free(l);
            released_msg(m, sizeof(m), key);
            broadcast(s, m, NULL);
        }
    } else if (strcmp(t, "admin") == 0) {               // the session leader's host controls
        char action[32], id[200];
        const char *r, *on = js_get(msg, "on");
        js_str(js_get(msg, "action"), action, sizeof(action));
        js_str(js_get(msg, "id"), id, sizeof(id));
        if (!c->leader || !g_leaderCtl) send_error(c, "not_allowed", "Only the PC hosting this session can do that.");
        else if ((r = admin_do(c, action, id, on && strncmp(on, "true", 4) == 0))[0]) send_error(c, "admin", r);
    } else if (strcmp(t, "bye") == 0) {
        InterlockedExchange(&c->closed, 1);
        shutdown(c->s, SD_BOTH);
    } else if (strcmp(t, "snapshot") != 0 && strcmp(t, "project_swap") != 0) {
        send_error(c, "unknown", "Unknown message type.");
    }
    LeaveCriticalSection(&g_lk);
}

// ---------------- WebSocket ----------------
static int recv_all(SOCKET s, void *p, size_t n, const char **pre, size_t *pren) {
    char *q = (char *)p;
    while (n && pre && *pren) { *q++ = **pre; (*pre)++; (*pren)--; n--; }
    while (n) {
        int k = recv(s, q, n > (1 << 20) ? (1 << 20) : (int)n, 0);
        if (k <= 0) return 0;
        q += k; n -= (size_t)k;
    }
    return 1;
}
static void b64(const unsigned char *in, int n, char *out) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, o = 0;
    for (i = 0; i < n; i += 3) {
        int a = in[i], b = i + 1 < n ? in[i + 1] : 0, c = i + 2 < n ? in[i + 2] : 0;
        out[o++] = T[a >> 2]; out[o++] = T[((a & 3) << 4) | (b >> 4)];
        out[o++] = i + 1 < n ? T[((b & 15) << 2) | (c >> 6)] : '=';
        out[o++] = i + 2 < n ? T[c & 63] : '=';
    }
    out[o] = 0;
}

static void ws_client(SOCKET s, const char *key, const char *pre, size_t pren) {
    char resp[300], acc[200], hdr[120];
    unsigned char sha[20];
    Client *c;
    Hash x;
    char *msg = NULL;
    size_t mlen = 0;
    int ok = 1;
    sprintf_s(acc, sizeof(acc), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    if (!hash_begin(&x, BCRYPT_SHA1_ALGORITHM)) return;
    hash_add(&x, acc, strlen(acc));
    hash_end(&x, sha, 20);
    b64(sha, 20, hdr);
    sprintf_s(resp, sizeof(resp), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", hdr);
    if (!send_all(s, resp, strlen(resp))) return;
    {   // a slow peer can take long to read a project; liveness comes from pings instead
        DWORD zero = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&zero, sizeof(zero));
    }
    c = (Client *)calloc(1, sizeof(Client));
    c->s = s; c->alive = 1; c->refs = 2;
    c->tokens = RATE_BURST; c->tokensAt = c->since = GetTickCount();
    {
        struct sockaddr_storage pa;
        int pl = sizeof(pa);
        if (getpeername(s, (struct sockaddr *)&pa, &pl) == 0) {
            if (pa.ss_family == AF_INET6) inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&pa)->sin6_addr, c->ip, sizeof(c->ip));
            else inet_ntop(AF_INET, &((struct sockaddr_in *)&pa)->sin_addr, c->ip, sizeof(c->ip));
            if (_strnicmp(c->ip, "::ffff:", 7) == 0) memmove(c->ip, c->ip + 7, strlen(c->ip + 7) + 1);   // IPv4 on the dual-stack socket
        }
    }
    InitializeCriticalSection(&c->ql);
    c->qev = CreateEventW(NULL, FALSE, FALSE, NULL);
    EnterCriticalSection(&g_lk);
    c->next = g_clients; g_clients = c;
    LeaveCriticalSection(&g_lk);
    CloseHandle(CreateThread(NULL, 0, writer_thread, c, 0, NULL));
    while (ok && g_run && !c->closed) {
        unsigned char h[2], ext[8], mask[4];
        uint64_t n;
        int op, fin, i;
        unsigned char *pay;
        if (!recv_all(s, h, 2, &pre, &pren)) break;
        fin = h[0] & 0x80; op = h[0] & 15; n = h[1] & 127;
        if (n == 126) { if (!recv_all(s, ext, 2, &pre, &pren)) break; n = ((uint64_t)ext[0] << 8) | ext[1]; }
        else if (n == 127) { if (!recv_all(s, ext, 8, &pre, &pren)) break; for (n = 0, i = 0; i < 8; i++) n = (n << 8) | ext[i]; }
        if (!(h[1] & 0x80) || n > MAX_PAYLOAD || mlen + n > MAX_PAYLOAD) break;   // clients must mask; size cap
        if (!recv_all(s, mask, 4, &pre, &pren)) break;
        pay = (unsigned char *)malloc((size_t)n + 1);
        if (!recv_all(s, pay, (size_t)n, &pre, &pren)) { free(pay); break; }
        for (i = 0; (uint64_t)i < n; i++) pay[i] ^= mask[i & 3];
        if (op == 8) {                                    // close: answer and stop
            Buf *b = frame(8, pay, n >= 2 ? 2 : 0);
            enqueue(c, b); buf_release(b);
            free(pay);
            break;
        }
        if (op == 9) { Buf *b = frame(10, pay, (size_t)n); enqueue(c, b); buf_release(b); free(pay); continue; }
        if (op == 10) { c->alive = 1; free(pay); continue; }
        if (op == 1 || op == 2 || op == 0) {
            msg = (char *)realloc(msg, mlen + (size_t)n + 1);
            memcpy(msg + mlen, pay, (size_t)n);
            mlen += (size_t)n;
            free(pay);
            c->alive = 1;
            if (fin) {
                msg[mlen] = 0;
                on_message(c, msg, mlen);
                free(msg); msg = NULL; mlen = 0;
            }
            continue;
        }
        free(pay);
    }
    free(msg);
    EnterCriticalSection(&g_lk);
    if (c->sess) { if (c->id[0]) hlog("%s disconnected", c->name); session_remove(c); }
    {
        Client **pp;
        for (pp = &g_clients; *pp; pp = &(*pp)->next) if (*pp == c) { *pp = c->next; break; }
    }
    LeaveCriticalSection(&g_lk);
    InterlockedExchange(&c->closed, 1);
    SetEvent(c->qev);
    client_release(c);
}

// ---------------- HTTP: health + sample store ----------------
static void http_resp(SOCKET s, int code, const char *ctype, const void *body, size_t n) {
    char h[300];
    const char *txt = code == 200 ? "OK" : code == 201 ? "Created" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized"
                    : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : code == 413 ? "Payload Too Large" : "Error";
    sprintf_s(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", code, txt, ctype, n);
    if (send_all(s, h, strlen(h)) && n) send_all(s, body, n);
}
static void blob_path(const char *hex, wchar_t *out, int cap) { swprintf_s(out, cap, L"%s\\%S", g_blobDir, hex); }
static int blob_exists(const char *hex, uint64_t *size) {
    wchar_t p[MAX_PATH + 80];
    WIN32_FILE_ATTRIBUTE_DATA a;
    blob_path(hex, p, MAX_PATH + 80);
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    if (size) *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return 1;
}
typedef struct { wchar_t name[80]; uint64_t size; FILETIME t; } BlobEnt;
static int blob_cmp(const void *a, const void *b) { return CompareFileTime(&((const BlobEnt *)a)->t, &((const BlobEnt *)b)->t); }
// Keep the store under STORE_MAX: the least recently used files go (GET refreshes a file's write time).
static void evict(void) {
    wchar_t pat[MAX_PATH + 8], p[MAX_PATH + 80];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BlobEnt *E = NULL;
    int n = 0, cap = 0, i;
    uint64_t total = 0;
    swprintf_s(pat, MAX_PATH + 8, L"%s\\*", g_blobDir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (wcslen(fd.cFileName) != 64 || (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 256; E = (BlobEnt *)realloc(E, sizeof(BlobEnt) * cap); }
        wcscpy_s(E[n].name, 80, fd.cFileName);
        E[n].size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        E[n].t = fd.ftLastWriteTime;
        total += E[n].size;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (total > STORE_MAX) {
        qsort(E, n, sizeof(BlobEnt), blob_cmp);
        for (i = 0; i < n && total > STORE_MAX; i++) {
            swprintf_s(p, MAX_PATH + 80, L"%s\\%s", g_blobDir, E[i].name);
            if (DeleteFileW(p)) { total -= E[i].size; hlog("sample store full: dropped %.12S", E[i].name); }
        }
    }
    free(E);
}
static int read_body(SOCKET s, const char *pre, size_t pren, char *out, size_t n) {
    return recv_all(s, out, n, &pre, &pren);
}

static void http_request(SOCKET s, const char *method, const char *path, uint64_t clen, int hasLen,
                         const char *key, const char *pre, size_t pren, const char *ip) {
    unsigned char kh[32], wh[32];
    char hex[65];
    if (strcmp(path, "/health") == 0 || strcmp(path, "/") == 0) { http_resp(s, 200, "text/plain", "flim-server ok\n", 15); return; }
    if (strcmp(path, "/blobs/missing") != 0 && !(strncmp(path, "/blob/", 6) == 0 && strlen(path) == 70 && is_hex64(path + 6))) {
        http_resp(s, 404, "text/plain", "", 0);
        return;
    }
    if (g_hasPw) {                                        // x-flim-key = sha256 hex of the password
        if (ip_locked(ip)) { http_resp(s, 429, "text/plain", "", 0); return; }   // (wrong passwords: refused a while)
        sha256(key, strlen(key), kh);
        sha256(g_keyHex, 64, wh);
        if (!same32(kh, wh)) { ip_fail(ip); http_resp(s, 401, "text/plain", "", 0); return; }
    }
    if (strcmp(path, "/blobs/missing") == 0) {
        char *body, *out, *tok, *ctx = NULL;
        size_t o = 0;
        if (strcmp(method, "POST") != 0) { http_resp(s, 405, "text/plain", "", 0); return; }
        if (!hasLen || clen > 1024 * 1024) { http_resp(s, 413, "text/plain", "", 0); return; }
        body = (char *)malloc((size_t)clen + 1);
        out = (char *)malloc((size_t)clen + 2);
        if (!read_body(s, pre, pren, body, (size_t)clen)) { free(body); free(out); return; }
        body[clen] = 0;
        for (tok = strtok_s(body, " \t\r\n", &ctx); tok; tok = strtok_s(NULL, " \t\r\n", &ctx))
            if (strlen(tok) == 64 && is_hex64(tok) && !blob_exists(tok, NULL)) o += sprintf_s(out + o, (size_t)clen + 2 - o, "%s%s", o ? "\n" : "", tok);
        http_resp(s, 200, "text/plain", out, o);
        free(body); free(out);
        return;
    }
    memcpy(hex, path + 6, 64); hex[64] = 0;
    if (strcmp(method, "HEAD") == 0 || strcmp(method, "GET") == 0) {
        wchar_t p[MAX_PATH + 80];
        uint64_t size;
        HANDLE f;
        char h[300];
        if (!blob_exists(hex, &size)) { http_resp(s, 404, "text/plain", "", 0); return; }
        blob_path(hex, p, MAX_PATH + 80);
        f = CreateFileW(p, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (f == INVALID_HANDLE_VALUE) { http_resp(s, 404, "text/plain", "", 0); return; }
        sprintf_s(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %llu\r\nConnection: close\r\n\r\n",
                  (unsigned long long)size);
        if (send_all(s, h, strlen(h)) && method[0] == 'G') {
            unsigned char *buf = (unsigned char *)malloc(1 << 20);
            DWORD got;
            FILETIME now;
            GetSystemTimeAsFileTime(&now);
            SetFileTime(f, NULL, NULL, &now);              // recently used stays (LRU)
            while (ReadFile(f, buf, 1 << 20, &got, NULL) && got) if (!send_all(s, buf, got)) break;
            free(buf);
        }
        CloseHandle(f);
        return;
    }
    if (strcmp(method, "PUT") == 0) {
        wchar_t tmp[MAX_PATH + 80], dst[MAX_PATH + 80];
        unsigned char *buf, d[32];
        char got[65];
        uint64_t left = clen;
        HANDLE f = INVALID_HANDLE_VALUE;
        Hash x;
        int exists = blob_exists(hex, NULL), ok = 1;
        if (!hasLen || clen > FILE_MAX) { http_resp(s, 413, "text/plain", "", 0); return; }
        if (!exists) {
            swprintf_s(tmp, MAX_PATH + 80, L"%s\\.%S.%lu.part", g_blobDir, hex, GetCurrentThreadId());
            f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            if (f == INVALID_HANDLE_VALUE || !hash_begin(&x, BCRYPT_SHA256_ALGORITHM)) { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); http_resp(s, 400, "text/plain", "", 0); return; }
        }
        buf = (unsigned char *)malloc(1 << 20);
        while (left && ok) {                              // already stored: the body is read and dropped
            size_t want = left > (1 << 20) ? (1 << 20) : (size_t)left;
            DWORD w;
            if (!recv_all(s, buf, want, &pre, &pren)) { ok = 0; break; }
            if (!exists) { hash_add(&x, buf, want); if (!WriteFile(f, buf, (DWORD)want, &w, NULL) || w != want) ok = 0; }
            left -= want;
        }
        free(buf);
        if (exists) { if (ok) http_resp(s, 200, "text/plain", "", 0); return; }
        CloseHandle(f);
        hash_end(&x, d, 32);
        tohex(d, 32, got);
        blob_path(hex, dst, MAX_PATH + 80);
        if (!ok || strcmp(got, hex) != 0) {
            DeleteFileW(tmp);
            if (ok) { hlog("rejected a sample upload: content doesn't match %.12s", hex); http_resp(s, 400, "text/plain", "", 0); }
            return;
        }
        if (!MoveFileExW(tmp, dst, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); http_resp(s, 400, "text/plain", "", 0); return; }
        hlog("stored a sample (%.1f MB)", clen / 1048576.0);
        http_resp(s, 201, "text/plain", "", 0);
        evict();
        return;
    }
    http_resp(s, 405, "text/plain", "", 0);
}

// ---------------- connections ----------------
static int hdr_get(const char *hdrs, const char *name, char *out, int cap) {
    size_t nl = strlen(name);
    const char *p = hdrs;
    while ((p = strstr(p, "\r\n")) != NULL) {
        const char *v, *e;
        p += 2;
        if (_strnicmp(p, name, nl) != 0 || p[nl] != ':') continue;
        v = p + nl + 1;
        while (*v == ' ' || *v == '\t') v++;
        e = strstr(v, "\r\n");
        if (!e) return 0;
        while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
        if (e - v >= cap) return 0;
        memcpy(out, v, e - v); out[e - v] = 0;
        return 1;
    }
    return 0;
}

static DWORD WINAPI conn_thread(LPVOID p) {
    SOCKET s = (SOCKET)(uintptr_t)p;
    char *buf = (char *)malloc(HDR_MAX + 1), *end = NULL, method[16] = "", path[600] = "", v[300], ip[64];
    int n = 0;
    peer_ip(s, ip, sizeof(ip));                           // (accept_thread counted it: ip_conn_end below)
    DWORD to = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));   // a request has 15 s to arrive
    while (n < HDR_MAX) {
        int k = recv(s, buf + n, HDR_MAX - n, 0);
        if (k <= 0) break;
        n += k; buf[n] = 0;
        if ((end = strstr(buf, "\r\n\r\n")) != NULL) break;
    }
    if (end) {
        const char *pre = end + 4;
        size_t pren = (size_t)(buf + n - pre);
        end[2] = 0;                                       // headers end with one CRLF now (for hdr_get)
        sscanf_s(buf, "%15s %599s", method, (unsigned)sizeof(method), path, (unsigned)sizeof(path));
        {
            char *q = strchr(path, '?');
            if (q) *q = 0;
        }
        if (hdr_get(buf, "Upgrade", v, sizeof(v)) && _stricmp(v, "websocket") == 0 && hdr_get(buf, "Sec-WebSocket-Key", v, sizeof(v)))
            ws_client(s, v, pre, pren);                   // ws_client owns the socket from here
        else {
            char key[200] = "", cl[40] = "";
            int hasLen = hdr_get(buf, "Content-Length", cl, sizeof(cl));
            hdr_get(buf, "x-flim-key", key, sizeof(key));
            to = 120000;                                  // big uploads: generous per-read timeout
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
            http_request(s, method, path, hasLen ? _strtoui64(cl, NULL, 10) : 0, hasLen, key, pre, pren, ip);
            shutdown(s, SD_SEND);
            closesocket(s);
        }
    } else closesocket(s);
    free(buf);
    ip_conn_end(ip);
    return 0;
}

static DWORD WINAPI accept_thread(LPVOID p) {
    (void)p;
    while (g_run) {
        SOCKET c = accept(g_ls, NULL, NULL);
        HANDLE t;
        char ip[64];
        if (c == INVALID_SOCKET) { if (!g_run) break; Sleep(50); continue; }
        peer_ip(c, ip, sizeof(ip));
        if (!ip_conn_begin(ip)) {                         // one address can't fill the machine with connections (threads)
            static DWORD said;
            if (GetTickCount() - said > 10000) { said = GetTickCount(); hlog("%s: more than %d connections, refused", ip, CONNS_PER_IP); }
            closesocket(c);
            continue;
        }
        t = CreateThread(NULL, 0, conn_thread, (LPVOID)(uintptr_t)c, 0, NULL);
        if (t) CloseHandle(t); else { closesocket(c); ip_conn_end(ip); }
    }
    return 0;
}

// Pings (dead clients are dropped) and the lock TTL sweep.
static DWORD WINAPI timer_thread(LPVOID p) {
    DWORD lastPing = GetTickCount(), lastSweep = GetTickCount();
    (void)p;
    while (g_run) {
        DWORD now = GetTickCount();
        Sleep(250);
        if (now - lastSweep >= 5000) {
            Session *s;
            char m[600];
            lastSweep = now;
            EnterCriticalSection(&g_lk);
            for (s = g_sessions; s; s = s->next) {
                Lock **pl = &s->locks;
                while (*pl) {
                    if (now - (*pl)->at >= LOCK_TTL_MS) {
                        Lock *d = *pl;
                        *pl = d->next;
                        released_msg(m, sizeof(m), d->key);
                        broadcast(s, m, NULL);
                        free(d);
                    } else pl = &(*pl)->next;
                }
            }
            LeaveCriticalSection(&g_lk);
        }
        if (now - lastPing >= PING_MS) {
            Client *c;
            Buf *ping = frame(9, "", 0);
            lastPing = now;
            EnterCriticalSection(&g_lk);
            for (c = g_clients; c; c = c->next) {
                if (!c->alive) { shutdown(c->s, SD_BOTH); continue; }   // no pong / message since the last ping
                if (!c->sess && now - c->since > JOIN_TIMEOUT_MS) { shutdown(c->s, SD_BOTH); continue; }   // never joined
                c->alive = 0;
                enqueue(c, ping);
            }
            LeaveCriticalSection(&g_lk);
            buf_release(ping);
        }
    }
    return 0;
}

// ---------------- public ----------------
int host_running(void) { return g_run != 0; }
void host_set_max_people(int n) { InterlockedExchange(&g_maxPeople, n < 2 ? 2 : n > 64 ? 64 : n); }
void host_set_notes_merge(int on) { InterlockedExchange(&g_hostMerge, on != 0); }

int host_start(int port, const char *password, HWND notify, char *err, int errcap) {
    WSADATA wd;
    SOCKET ls;
    int no = 0, yes = 1;
    struct sockaddr_in6 a6;
    if (g_run) return 1;
    if (!g_lkInit) { InitializeCriticalSection(&g_lk); InitializeCriticalSection(&g_ipLk); g_lkInit = 1; }
    WSAStartup(MAKEWORD(2, 2), &wd);
    g_notify = notify;
    g_hasPw = password && password[0];
    if (g_hasPw) {
        unsigned char k[32];
        sha256(password, strlen(password), g_pwHash);
        sha256(password, strlen(password), k);
        tohex(k, 32, g_keyHex);
    }
    {
        wchar_t base[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) GetTempPathW(MAX_PATH, base);
        swprintf_s(g_blobDir, MAX_PATH, L"%s\\flim\\host-samples", base);
        SHCreateDirectoryExW(NULL, g_blobDir, NULL);
    }
    // one dual-stack socket (IPv6 + IPv4); plain IPv4 if IPv6 is off
    ls = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (ls != INVALID_SOCKET) {
        setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&no, sizeof(no));
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons((u_short)port);
        a6.sin6_addr = in6addr_any;
        if (bind(ls, (struct sockaddr *)&a6, sizeof(a6)) != 0) { closesocket(ls); ls = INVALID_SOCKET; }
    }
    if (ls == INVALID_SOCKET) {
        struct sockaddr_in a4;
        ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        memset(&a4, 0, sizeof(a4));
        a4.sin_family = AF_INET;
        a4.sin_port = htons((u_short)port);
        a4.sin_addr.s_addr = INADDR_ANY;
        if (ls == INVALID_SOCKET || bind(ls, (struct sockaddr *)&a4, sizeof(a4)) != 0) {
            int e = WSAGetLastError();
            if (ls != INVALID_SOCKET) closesocket(ls);
            if (e == WSAEADDRINUSE) sprintf_s(err, errcap, "Port %d is already in use (another server running?)", port);
            else sprintf_s(err, errcap, "Can't listen on port %d (error %d)", port, e);
            return 0;
        }
    }
    (void)yes;
    if (listen(ls, SOMAXCONN) != 0) { sprintf_s(err, errcap, "Can't listen on port %d", port); closesocket(ls); return 0; }
    g_ls = ls;
    InterlockedExchange(&g_run, 1);
    CloseHandle(CreateThread(NULL, 0, accept_thread, NULL, 0, NULL));
    CloseHandle(CreateThread(NULL, 0, timer_thread, NULL, 0, NULL));
    hlog("hosting on port %d%s", port, g_hasPw ? " (password set)" : " (no password: anyone with the address can join)");
    return 1;
}

void host_stop(void) {
    Client *c;
    if (!g_run) return;
    InterlockedExchange(&g_run, 0);
    closesocket(g_ls);
    g_ls = INVALID_SOCKET;
    EnterCriticalSection(&g_lk);
    for (c = g_clients; c; c = c->next) shutdown(c->s, SD_BOTH);
    while (g_ipBans) { Ban *n = g_ipBans->next; free(g_ipBans); g_ipBans = n; }   // (they last as long as the server)
    LeaveCriticalSection(&g_lk);
    hlog("stopped hosting");
    peers_changed();
}

int host_lan_ip(wchar_t *out, int cap) {
    WSADATA wd;
    SOCKET s;
    struct sockaddr_in a, me;
    int n = sizeof(me), ok = 0;
    out[0] = 0;
    WSAStartup(MAKEWORD(2, 2), &wd);
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);         // connecting a UDP socket sends nothing: it only picks the route
    if (s == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 && getsockname(s, (struct sockaddr *)&me, &n) == 0)
        ok = InetNtopW(AF_INET, &me.sin_addr, out, cap) != NULL;
    closesocket(s);
    return ok;
}

// UPnP through Windows' NATUPnP COM objects (natupnp.h), GUIDs spelled out here.
static const GUID CLSID_FlimUPnPNAT = { 0xAE1E00AA, 0x3FD5, 0x403C, { 0x8A, 0x27, 0x2B, 0xBD, 0xC3, 0x0C, 0xD0, 0xE1 } };
static const GUID IID_FlimIUPnPNAT  = { 0xB171C812, 0xCC76, 0x485A, { 0x94, 0xD8, 0xB6, 0xB3, 0xA2, 0x79, 0x4E, 0x99 } };
int host_upnp(int port, int add, wchar_t *ext, int extcap) {
    IUPnPNAT *nat = NULL;
    IStaticPortMappingCollection *col = NULL;
    IStaticPortMapping *map = NULL;
    wchar_t lan[64];
    BSTR proto, client, desc;
    int ok = 0, i;
    HRESULT co;
    if (ext && extcap) ext[0] = 0;
    if (add && !host_lan_ip(lan, 64)) return 0;
    co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    proto = SysAllocString(L"TCP"); client = SysAllocString(add ? lan : L""); desc = SysAllocString(L"flim");
    if (SUCCEEDED(CoCreateInstance(&CLSID_FlimUPnPNAT, NULL, CLSCTX_INPROC_SERVER, &IID_FlimIUPnPNAT, (void **)&nat)) && nat) {
        for (i = 0; i < 4 && !col; i++) {                 // empty until the router has been discovered
            nat->lpVtbl->get_StaticPortMappingCollection(nat, &col);
            if (!col) Sleep(1500);
        }
        if (col) {
            if (add) {
                col->lpVtbl->Remove(col, port, proto);    // an old mapping of ours, if any
                if (SUCCEEDED(col->lpVtbl->Add(col, port, proto, port, client, VARIANT_TRUE, desc, &map)) && map) {
                    BSTR e = NULL;
                    if (ext && SUCCEEDED(map->lpVtbl->get_ExternalIPAddress(map, &e)) && e) { wcsncpy_s(ext, extcap, e, _TRUNCATE); SysFreeString(e); }
                    map->lpVtbl->Release(map);
                    ok = 1;
                }
            } else ok = SUCCEEDED(col->lpVtbl->Remove(col, port, proto));
            col->lpVtbl->Release(col);
        }
        nat->lpVtbl->Release(nat);
    }
    SysFreeString(proto); SysFreeString(client); SysFreeString(desc);
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

int host_people(HostPerson *out, int max) {
    Session *s;
    Client *c;
    Ban *b;
    int n = 0;
    if (!g_lkInit) return 0;
    EnterCriticalSection(&g_lk);
    for (s = g_sessions; s; s = s->next) {
        for (c = g_clients; c; c = c->next) {
            if (c->sess != s || c->closed || n >= max) continue;
            strcpy_s(out[n].id, sizeof(out[n].id), c->id); strcpy_s(out[n].name, sizeof(out[n].name), c->name);
            out[n].leader = c->leader; out[n].sync = c->sync; out[n].banned = 0;
            n++;
        }
        for (b = s->bans; b && n < max; b = b->next, n++) {
            strcpy_s(out[n].id, sizeof(out[n].id), b->id); strcpy_s(out[n].name, sizeof(out[n].name), b->name);
            out[n].leader = out[n].sync = 0; out[n].banned = 1;
        }
    }
    for (b = g_ipBans; b && n < max; b = b->next, n++) {
        strcpy_s(out[n].id, sizeof(out[n].id), b->id); strcpy_s(out[n].name, sizeof(out[n].name), b->name);
        out[n].leader = out[n].sync = 0; out[n].banned = 2;
    }
    LeaveCriticalSection(&g_lk);
    return n;
}

int host_admin(const char *action, const char *id, int on, char *err, int errcap) {
    const char *r = "Not hosting.";                       // (admin_do's answers are literals)
    if (g_lkInit) {
        EnterCriticalSection(&g_lk);
        if (g_run) r = admin_do(NULL, action, id, on);
        LeaveCriticalSection(&g_lk);
    }
    strcpy_s(err, errcap, r);
    return !r[0];
}

void host_leader_controls(int on) {
    Session *s;
    if (!g_lkInit) { InitializeCriticalSection(&g_lk); InitializeCriticalSection(&g_ipLk); g_lkInit = 1; }
    EnterCriticalSection(&g_lk);
    if (InterlockedExchange(&g_leaderCtl, on) != on) {
        hlog(on ? "the session leader can use the host controls too" : "only this PC can use the host controls");
        for (s = g_sessions; s; s = s->next) presence_all(s);
    }
    LeaveCriticalSection(&g_lk);
}

// flim's built-in server (host.c): the same protocol as server/ (WebSocket relay + sample store),
// run inside flim.exe so hosting needs nothing installed.
#pragma once
#include <windows.h>

#define WM_HOST_LOG    (WM_APP + 0x41)   // lParam: malloc'd wchar_t* line (receiver frees)
#define WM_HOST_PEERS  (WM_APP + 0x42)   // the people list changed: call host_people

// Start listening on `port` (all interfaces). password "" = anyone may join. notify gets the messages above.
int  host_start(int port, const char *password, HWND notify, char *err, int errcap);
void host_stop(void);
int  host_running(void);
// Who is in the sessions (UTF-8), then the people removed from them (banned = 1) and IP banned (2). Returns the count.
typedef struct { char id[72], name[40]; int leader, sync, banned; } HostPerson;
int  host_people(HostPerson *out, int max);
// Host controls, from this PC: action "give_host" / "kick" / "ban_ip" / "sync_only" (on = 1 / 0) / "unban" (lifts a
// kick and an IP ban) for the person with that id. 1 = done; else err says why not.
int  host_admin(const char *action, const char *id, int on, char *err, int errcap);
// 1: the session leader may use the host controls too (from their flim window). Default 0.
void host_leader_controls(int on);
// People per session, the leader included (2..64). Default 8; flim.ini host_max_people.
void host_set_max_people(int n);
// This PC's address on its local network (the interface that reaches the internet). 0 = none.
int  host_lan_ip(wchar_t *out, int cap);
// Ask the router (UPnP) to forward TCP `port` to this PC (add=1) or stop (add=0). Blocks for a few
// seconds. On success ext gets the router's public address when it reports one. 1 = done.
int  host_upnp(int port, int add, wchar_t *ext, int extcap);

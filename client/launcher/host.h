// flim's built-in server (host.c): the same protocol as server/ (WebSocket relay + sample store),
// run inside flim.exe so hosting needs nothing installed.
#pragma once
#include <windows.h>

#define WM_HOST_LOG    (WM_APP + 0x41)   // lParam: malloc'd wchar_t* line (receiver frees)
#define WM_HOST_PEERS  (WM_APP + 0x42)   // the people list changed: call host_peers

// Start listening on `port` (all interfaces). password "" = anyone may join. notify gets the messages above.
int  host_start(int port, const char *password, HWND notify, char *err, int errcap);
void host_stop(void);
int  host_running(void);
// "Alex (leader), Sam" per session; returns the number of people.
int  host_peers(wchar_t *out, int cap);
// This PC's address on its local network (the interface that reaches the internet). 0 = none.
int  host_lan_ip(wchar_t *out, int cap);
// Ask the router (UPnP) to forward TCP `port` to this PC (add=1) or stop (add=0). Blocks for a few
// seconds. On success ext gets the router's public address when it reports one. 1 = done.
int  host_upnp(int port, int add, wchar_t *ext, int extcap);

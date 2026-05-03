// net_sdl.c -- stub UDP network driver (returns -1 from Init to disable UDP)
// Single-player works through net_loop.c; UDP multiplayer is disabled for now.

#include "quakedef.h"
#include "net_udp.h"

int  UDP_Init(void)                                    { return -1; }
void UDP_Shutdown(void)                                {}
void UDP_Listen(qboolean state)                        { (void)state; }
int  UDP_OpenSocket(int port)                          { (void)port; return -1; }
int  UDP_CloseSocket(int socket)                       { (void)socket; return 0; }
int  UDP_Connect(int socket, struct qsockaddr *addr)   { (void)socket; (void)addr; return -1; }
int  UDP_CheckNewConnections(void)                     { return -1; }
int  UDP_Read(int socket, byte *buf, int len, struct qsockaddr *addr)
                                                       { (void)socket; (void)buf; (void)len; (void)addr; return -1; }
int  UDP_Write(int socket, byte *buf, int len, struct qsockaddr *addr)
                                                       { (void)socket; (void)buf; (void)len; (void)addr; return -1; }
int  UDP_Broadcast(int socket, byte *buf, int len)     { (void)socket; (void)buf; (void)len; return -1; }
char *UDP_AddrToString(struct qsockaddr *addr)         { (void)addr; return ""; }
int  UDP_StringToAddr(char *string, struct qsockaddr *addr) { (void)string; (void)addr; return -1; }
int  UDP_GetSocketAddr(int socket, struct qsockaddr *addr)  { (void)socket; (void)addr; return -1; }
int  UDP_GetNameFromAddr(struct qsockaddr *addr, char *name){ (void)addr; (void)name; return -1; }
int  UDP_GetAddrFromName(char *name, struct qsockaddr *addr){ (void)name; (void)addr; return -1; }
int  UDP_AddrCompare(struct qsockaddr *addr1, struct qsockaddr *addr2) { (void)addr1; (void)addr2; return -1; }
int  UDP_GetSocketPort(struct qsockaddr *addr)         { (void)addr; return 0; }
int  UDP_SetSocketPort(struct qsockaddr *addr, int port){ (void)addr; (void)port; return 0; }

// ---------------------------------------------------------------------------
// Net driver / LAN driver tables (replaces net_win.c)
// ---------------------------------------------------------------------------

// Forward declarations from net_loop.c and net_dgrm.c
extern int        Loop_Init(void);
extern void       Loop_Listen(qboolean);
extern void       Loop_SearchForHosts(qboolean);
extern qsocket_t *Loop_Connect(char *);
extern qsocket_t *Loop_CheckNewConnections(void);
extern int        Loop_GetMessage(qsocket_t *);
extern int        Loop_SendMessage(qsocket_t *, sizebuf_t *);
extern int        Loop_SendUnreliableMessage(qsocket_t *, sizebuf_t *);
extern qboolean   Loop_CanSendMessage(qsocket_t *);
extern qboolean   Loop_CanSendUnreliableMessage(qsocket_t *);
extern void       Loop_Close(qsocket_t *);
extern void       Loop_Shutdown(void);

extern int        Datagram_Init(void);
extern void       Datagram_Listen(qboolean);
extern void       Datagram_SearchForHosts(qboolean);
extern qsocket_t *Datagram_Connect(char *);
extern qsocket_t *Datagram_CheckNewConnections(void);
extern int        Datagram_GetMessage(qsocket_t *);
extern int        Datagram_SendMessage(qsocket_t *, sizebuf_t *);
extern int        Datagram_SendUnreliableMessage(qsocket_t *, sizebuf_t *);
extern qboolean   Datagram_CanSendMessage(qsocket_t *);
extern qboolean   Datagram_CanSendUnreliableMessage(qsocket_t *);
extern void       Datagram_Close(qsocket_t *);
extern void       Datagram_Shutdown(void);

// net_driver_t: name, initialized, Init, Listen, SearchForHosts, Connect,
//               CheckNewConnections, QGetMessage, QSendMessage, SendUnreliableMessage,
//               CanSendMessage, CanSendUnreliableMessage, Close, Shutdown, controlSock
net_driver_t net_drivers[MAX_NET_DRIVERS] =
{
    {
        "Loopback", false,
        Loop_Init, Loop_Listen, Loop_SearchForHosts,
        Loop_Connect, Loop_CheckNewConnections,
        Loop_GetMessage, Loop_SendMessage, Loop_SendUnreliableMessage,
        Loop_CanSendMessage, Loop_CanSendUnreliableMessage,
        Loop_Close, Loop_Shutdown
    },
    {
        "Datagram", false,
        Datagram_Init, Datagram_Listen, Datagram_SearchForHosts,
        Datagram_Connect, Datagram_CheckNewConnections,
        Datagram_GetMessage, Datagram_SendMessage, Datagram_SendUnreliableMessage,
        Datagram_CanSendMessage, Datagram_CanSendUnreliableMessage,
        Datagram_Close, Datagram_Shutdown
    }
};
int net_numdrivers = 2;

net_landriver_t net_landrivers[MAX_NET_DRIVERS] =
{
    {
        "SDL UDP", false, 0,
        UDP_Init, UDP_Shutdown, UDP_Listen,
        UDP_OpenSocket, UDP_CloseSocket, UDP_Connect,
        UDP_CheckNewConnections,
        UDP_Read, UDP_Write, UDP_Broadcast,
        UDP_AddrToString, UDP_StringToAddr,
        UDP_GetSocketAddr, UDP_GetNameFromAddr, UDP_GetAddrFromName,
        UDP_AddrCompare, UDP_GetSocketPort, UDP_SetSocketPort
    }
};
int net_numlandrivers = 1;

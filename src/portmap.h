#ifndef TOH_PORTMAP_H
#define TOH_PORTMAP_H

/*
 * Opening the host's port on the router (UPnP IGD via miniupnpc, then NAT-PMP via libnatpmp)
 * and collecting the addresses for the join code. The mapping runs in its own thread: router
 * discovery takes seconds. The lease is one hour and renewed every 30 minutes; portmap_stop
 * removes it. Built without the libraries (no TOH_HAVE_PORTMAP), portmap_available() is false.
 */

#include "netmsg.h"

#define PORTMAP_LEASE_S 3600
#define PORTMAP_RENEW_S 1800

typedef struct Portmap Portmap;

typedef struct {
    bool done;           /* the first attempt finished */
    const char *method;  /* "UPnP", "NAT-PMP" or NULL when no mapping worked */
    bool have_external;  /* external IPv4 address from the router */
    uint8_t external[4];
    bool cgnat;          /* the router's external address is private or shared (100.64/10) */
} PortmapStatus;

bool     portmap_available(void);
Portmap *portmap_start(uint16_t port, void (*wake)(void *), void *ctx);
bool     portmap_status(Portmap *pm, PortmapStatus *out); /* thread-safe copy; false for NULL */
void     portmap_stop(Portmap *pm);                       /* removes the mapping; NULL is fine */

/* Addresses for the join code: the router's external IPv4 (st may be NULL), the machine's global
 * IPv6 addresses, then its LAN / VPN IPv4 addresses; 127.0.0.1 when there is nothing else. */
int portmap_collect_addrs(const PortmapStatus *st, NetAddr *out, int max);

/* 100.64.0.0/10 or a private (RFC 1918) address */
bool portmap_is_cgnat(const uint8_t a[4]);

#endif

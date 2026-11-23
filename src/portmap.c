/* Router port mapping and join code addresses. See portmap.h. */
#define _DEFAULT_SOURCE /* getifaddrs */
#include "portmap.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#ifdef TOH_HAVE_PORTMAP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#include <natpmp.h>
#include <sys/select.h>
#endif

bool portmap_is_cgnat(const uint8_t a[4]) {
    return (a[0] == 100 && (a[1] & 0xC0) == 64) || a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) ||
           (a[0] == 192 && a[1] == 168);
}

/* ---- addresses ----------------------------------------------------------------------------------- */

static bool have_addr(const NetAddr *out, int n, const NetAddr *a) {
    for (int i = 0; i < n; i++)
        if (out[i].family == a->family && memcmp(out[i].addr, a->addr, a->family == 4 ? 4 : 16) == 0) return true;
    return false;
}

int portmap_collect_addrs(const PortmapStatus *st, NetAddr *out, int max) {
    int n = 0;
    if (st && st->have_external && !st->cgnat && n < max) {
        out[n] = (NetAddr){ .family = 4 };
        memcpy(out[n++].addr, st->external, 4);
    }
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) == 0) {
        for (int pass = 0; pass < 2; pass++) { /* global IPv6 first, then IPv4 */
            for (struct ifaddrs *i = ifs; i && n < max; i = i->ifa_next) {
                if (!i->ifa_addr) continue;
                NetAddr a = { 0 };
                if (pass == 0 && i->ifa_addr->sa_family == AF_INET6) {
                    const uint8_t *b = ((struct sockaddr_in6 *)i->ifa_addr)->sin6_addr.s6_addr;
                    if ((b[0] & 0xE0) != 0x20) continue; /* 2000::/3 only */
                    a.family = 6;
                    memcpy(a.addr, b, 16);
                } else if (pass == 1 && i->ifa_addr->sa_family == AF_INET) {
                    const uint8_t *b = (const uint8_t *)&((struct sockaddr_in *)i->ifa_addr)->sin_addr;
                    if (b[0] == 127 || (b[0] == 169 && b[1] == 254) || b[0] == 0) continue;
                    a.family = 4;
                    memcpy(a.addr, b, 4);
                } else {
                    continue;
                }
                if (!have_addr(out, n, &a)) out[n++] = a;
            }
        }
        freeifaddrs(ifs);
    }
    if (n == 0 && max > 0) out[n++] = (NetAddr){ .family = 4, .addr = { 127, 0, 0, 1 } };
    return n;
}

/* ---- mapping ----------------------------------------------------------------------------------------- */

#ifndef TOH_HAVE_PORTMAP

bool portmap_available(void) { return false; }
Portmap *portmap_start(uint16_t port, void (*wake)(void *), void *ctx) {
    (void)port;
    (void)wake;
    (void)ctx;
    return NULL;
}
bool portmap_status(Portmap *pm, PortmapStatus *out) {
    (void)pm;
    memset(out, 0, sizeof(*out));
    return false;
}
void portmap_stop(Portmap *pm) { (void)pm; }

#else

#define DISCOVER_MS     2000
#define NATPMP_MAX_MS   3000

struct Portmap {
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    PortmapStatus st; /* under mu */
    uint16_t port;
    void (*wake)(void *);
    void *ctx;
    /* thread only */
    bool upnp_ok;
    struct UPNPUrls urls;
    struct IGDdatas data;
    char lanaddr[64];
};

static void set_external(PortmapStatus *st, const char *text) {
    struct in_addr a;
    if (!text || inet_pton(AF_INET, text, &a) != 1) return;
    st->have_external = true;
    memcpy(st->external, &a, 4);
    st->cgnat = portmap_is_cgnat(st->external);
}

/* Returns UPNPCOMMAND_SUCCESS or the router's error code. */
static int upnp_add(Portmap *pm) {
    char port[8], lease[16];
    snprintf(port, sizeof(port), "%u", pm->port);
    snprintf(lease, sizeof(lease), "%d", PORTMAP_LEASE_S);
    int r = UPNP_AddPortMapping(pm->urls.controlURL, pm->data.first.servicetype, port, port, pm->lanaddr,
                                "Tales of Honor", "TCP", NULL, lease);
    if (r == 725) /* OnlyPermanentLeasesSupported */
        r = UPNP_AddPortMapping(pm->urls.controlURL, pm->data.first.servicetype, port, port, pm->lanaddr,
                                "Tales of Honor", "TCP", NULL, "0");
    return r;
}

static bool upnp_map(Portmap *pm, PortmapStatus *st) {
    int err = 0;
    struct UPNPDev *devs = upnpDiscover(DISCOVER_MS, NULL, NULL, 0, 0, 2, &err);
    if (!devs) {
        snprintf(st->why, sizeof(st->why), "no UPnP router found");
        return false;
    }
    char wanaddr[64] = "";
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
    int r = UPNP_GetValidIGD(devs, &pm->urls, &pm->data, pm->lanaddr, sizeof(pm->lanaddr), wanaddr, sizeof(wanaddr));
#else
    int r = UPNP_GetValidIGD(devs, &pm->urls, &pm->data, pm->lanaddr, sizeof(pm->lanaddr));
    if (r == 2) r = 3; /* before API 18, 2 meant "IGD not connected" */
#endif
    freeUPNPDevlist(devs);
    if (r <= 0) {
        snprintf(st->why, sizeof(st->why), "no UPnP internet gateway found");
        return false;
    }
    if (r != 1 && r != 2) { /* 1: connected IGD, 2: IGD with a private external address */
        snprintf(st->why, sizeof(st->why), "the UPnP router is not connected to the internet");
        FreeUPNPUrls(&pm->urls);
        return false;
    }
    char ext[40] = "";
    if (UPNP_GetExternalIPAddress(pm->urls.controlURL, pm->data.first.servicetype, ext) == UPNPCOMMAND_SUCCESS)
        set_external(st, ext);
    else if (wanaddr[0])
        set_external(st, wanaddr);
    if (r == 2) st->cgnat = true;
    int add = upnp_add(pm);
    if (add != UPNPCOMMAND_SUCCESS) {
        const char *text = strupnperror(add);
        snprintf(st->why, sizeof(st->why), "the router refused UPnP port mapping (%d %s)", add, text ? text : "");
        if (add == 606 || add == 403) /* not authorized */
            st->hint = "The router does not let this computer open ports. Allow it in the router's settings "
                       "(FRITZ!Box: Internet > Permit Access > this computer > Permit independent port sharing).";
        FreeUPNPUrls(&pm->urls);
        return false;
    }
    pm->upnp_ok = true;
    return true;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Waits for one NAT-PMP answer, at most NATPMP_MAX_MS. */
static int natpmp_wait(natpmp_t *n, natpmpresp_t *resp) {
    uint64_t end = now_ms() + NATPMP_MAX_MS;
    int r;
    do {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(n->s, &fds);
        struct timeval tv;
        if (getnatpmprequesttimeout(n, &tv) < 0) return -1;
        select(n->s + 1, &fds, NULL, NULL, &tv);
        r = readnatpmpresponseorretry(n, resp);
    } while (r == NATPMP_TRYAGAIN && now_ms() < end);
    return r;
}

static bool natpmp_request(uint16_t port, int lifetime, PortmapStatus *st) {
    natpmp_t n;
    natpmpresp_t resp;
    if (initnatpmp(&n, 0, 0) < 0) return false;
    bool ok = false;
    if (st && sendpublicaddressrequest(&n) >= 0 && natpmp_wait(&n, &resp) >= 0 &&
        resp.type == NATPMP_RESPTYPE_PUBLICADDRESS) {
        set_external(st, inet_ntoa(resp.pnu.publicaddress.addr));
    }
    if (sendnewportmappingrequest(&n, NATPMP_PROTOCOL_TCP, port, port, (uint32_t)lifetime) >= 0 &&
        natpmp_wait(&n, &resp) >= 0)
        ok = true;
    closenatpmp(&n);
    return ok;
}

static void map_once(Portmap *pm, PortmapStatus *st) {
    memset(st, 0, sizeof(*st));
    if (pm->upnp_ok) { /* renewal */
        if (upnp_add(pm) == UPNPCOMMAND_SUCCESS) {
            st->method = "UPnP";
            return;
        }
        FreeUPNPUrls(&pm->urls);
        pm->upnp_ok = false;
    }
    if (upnp_map(pm, st)) {
        st->method = "UPnP";
    } else if (natpmp_request(pm->port, PORTMAP_LEASE_S, st)) {
        st->method = "NAT-PMP";
    } else {
        size_t n = strlen(st->why);
        snprintf(st->why + n, sizeof(st->why) - n, "%sno NAT-PMP answer", n ? "; " : "");
    }
}

static void unmap(Portmap *pm, const PortmapStatus *st) {
    if (pm->upnp_ok) {
        char port[8];
        snprintf(port, sizeof(port), "%u", pm->port);
        UPNP_DeletePortMapping(pm->urls.controlURL, pm->data.first.servicetype, port, "TCP", NULL);
        FreeUPNPUrls(&pm->urls);
        pm->upnp_ok = false;
    } else if (st->method) {
        natpmp_request(pm->port, 0, NULL);
    }
}

static void *thread_main(void *arg) {
    Portmap *pm = arg;
    PortmapStatus st;
    map_once(pm, &st);
    st.done = true;
    pthread_mutex_lock(&pm->mu);
    pm->st = st;
    pthread_mutex_unlock(&pm->mu);
    if (pm->wake) pm->wake(pm->ctx);

    pthread_mutex_lock(&pm->mu);
    while (!pm->stop) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += PORTMAP_RENEW_S;
        while (!pm->stop && pthread_cond_timedwait(&pm->cv, &pm->mu, &until) == 0) {}
        if (pm->stop) break;
        pthread_mutex_unlock(&pm->mu);
        if (st.method) { /* renew the lease; keep the first status for the log */
            PortmapStatus again;
            map_once(pm, &again);
        }
        pthread_mutex_lock(&pm->mu);
    }
    pthread_mutex_unlock(&pm->mu);
    unmap(pm, &st);
    return NULL;
}

bool portmap_available(void) { return true; }

Portmap *portmap_start(uint16_t port, void (*wake)(void *), void *ctx) {
    Portmap *pm = calloc(1, sizeof(*pm));
    if (!pm) return NULL;
    pm->port = port;
    pm->wake = wake;
    pm->ctx = ctx;
    pthread_mutex_init(&pm->mu, NULL);
    pthread_cond_init(&pm->cv, NULL);
    if (pthread_create(&pm->thread, NULL, thread_main, pm) != 0) {
        pthread_mutex_destroy(&pm->mu);
        pthread_cond_destroy(&pm->cv);
        free(pm);
        return NULL;
    }
    return pm;
}

bool portmap_status(Portmap *pm, PortmapStatus *out) {
    memset(out, 0, sizeof(*out));
    if (!pm) return false;
    pthread_mutex_lock(&pm->mu);
    *out = pm->st;
    pthread_mutex_unlock(&pm->mu);
    return true;
}

void portmap_stop(Portmap *pm) {
    if (!pm) return;
    pthread_mutex_lock(&pm->mu);
    pm->stop = true;
    pthread_cond_signal(&pm->cv);
    pthread_mutex_unlock(&pm->mu);
    pthread_join(pm->thread, NULL); /* waits for a running discovery (a few seconds at most) */
    pthread_mutex_destroy(&pm->mu);
    pthread_cond_destroy(&pm->cv);
    free(pm);
}

#endif

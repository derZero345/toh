/* toh-relay: lets a GM host without an open port. See relay.h and deploy/relay/README.md. */
#include "relay.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    RelayLimits lim;
    relay_default_limits(&lim);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            char *end;
            long port = strtol(argv[++i], &end, 10);
            if (*end || port < 1 || port > 65535) {
                fprintf(stderr, "--port: expected 1..65535\n");
                return 2;
            }
            lim.port = (uint16_t)port;
        } else {
            fprintf(stderr, "usage: %s [--port N]   (default %u)\n", argv[0], lim.port);
            return 2;
        }
    }

    /* SIGTERM (docker stop) and SIGINT end the relay; the relay thread inherits the blocked mask */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    char err[128];
    Relay *r = relay_start(&lim, stdout, err, sizeof(err));
    if (!r) {
        fprintf(stderr, "toh-relay: %s\n", err);
        return 1;
    }
    int sig;
    sigwait(&set, &sig);
    printf("stopping\n");
    relay_stop(r);
    return 0;
}

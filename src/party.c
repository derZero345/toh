#include "party.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool is_character(const Entity *e) { return e && e->type == ENTITY_CHARACTER; }

static bool owned_by(const Entity *e, const char *player) {
    return e->owner[0] && strcasecmp(e->owner, player) == 0;
}

bool party_shared(const Entity *a, const Entity *b) {
    return is_character(a) && is_character(b) && a->party[0] && strcasecmp(a->party, b->party) == 0;
}

bool party_visible(const Entity *e, const char *player) {
    if (!is_character(e) || !e->party[0]) return false;
    for (int i = 0; i < gh->entities.count; i++) {
        const Entity *c = gh->entities.items[i];
        if (is_character(c) && owned_by(c, player) && party_shared(c, e)) return true;
    }
    return false;
}

static int by_id(const void *a, const void *b) {
    uint32_t x = (*(Entity *const *)a)->id, y = (*(Entity *const *)b)->id;
    return (x > y) - (x < y);
}

void party_log(const char *viewer) {
    int n = gh->entities.count, lines = 0;
    Entity **es = malloc(sizeof(Entity *) * (size_t)(n ? n : 1));
    if (!es) abort();
    if (n) memcpy(es, gh->entities.items, sizeof(Entity *) * (size_t)n);
    qsort(es, (size_t)n, sizeof(Entity *), by_id);
    for (int i = 0; i < n; i++) {
        Entity *first = es[i];
        if (!is_character(first) || !first->party[0]) continue;
        bool listed = false; /* an older member came first */
        for (int j = 0; j < i && !listed; j++) listed = party_shared(es[j], first);
        if (listed || (viewer && !party_visible(first, viewer))) continue;
        char line[1024];
        int len = snprintf(line, sizeof(line), "%s:", first->party);
        for (int j = i; j < n && len < (int)sizeof(line); j++) {
            if (!party_shared(es[j], first)) continue;
            char name[CHAR_NAME_MAX + 16];
            entity_display_name(es[j], name, sizeof(name));
            len += snprintf(line + len, sizeof(line) - (size_t)len, "%s %s (%s)", j == i ? "" : ",", name,
                            es[j]->owner[0] ? es[j]->owner : "NPC");
        }
        console_log("%s", line);
        lines++;
    }
    if (!lines) console_log("%s", viewer ? "None of your characters is in a party" : "No parties");
    free(es);
}

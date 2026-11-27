/* Recover actions (see recover.h). */
#include "recover.h"

#include <string.h>

const RecoverActionDef RECOVER_ACTIONS[RECOVER_COUNT] = {
    [RECOVER_SMALL] = { "stamina.recover", "Recover", SLOT_MINOR, false },
    [RECOVER_FULL] = { "stamina.full_recover", "Full Recover", SLOT_MAJOR, true },
};

int recover_action_by_id(const char *id) {
    for (int i = 0; id && i < RECOVER_COUNT; i++)
        if (strcmp(RECOVER_ACTIONS[i].id, id) == 0) return i;
    return -1;
}

static bool valid(RecoverAction a) { return (int)a >= 0 && a < RECOVER_COUNT; }

OpResult recover_check(const Entity *e, RecoverAction a) {
    if (!e || e->type != ENTITY_CHARACTER) return OPR_NO_ENTITY;
    if (!valid(a)) return OPR_INVALID;
    OpResult r = action_turn_check(e, RECOVER_ACTIONS[a].slot);
    if (r != OPR_OK) return r;
    if (e->maxStamina <= 0) return OPR_NO_STAMINA_STAT;
    if (e->stamina >= e->maxStamina) return OPR_STAMINA_FULL;
    return OPR_OK;
}

int recover_roll(RecoverAction a) {
    if (!valid(a) || RECOVER_ACTIONS[a].full) return 0;
    int die;
    return dice_roll(1, RECOVER_DIE, 0, &die);
}

void recover_apply(Entity *e, RecoverAction a, int rolled) {
    const RecoverActionDef *d = &RECOVER_ACTIONS[a];
    action_use_slot(e, d->slot);
    e->stamina = d->full ? e->maxStamina : hp_clamp((long long)e->stamina + rolled, e->maxStamina);
    char name[CHAR_NAME_MAX + 16];
    entity_display_name(e, name, sizeof(name));
    if (d->full) console_log("%s: %s, stamina %d/%d", name, d->name, e->stamina, e->maxStamina);
    else console_log("%s: %s 1d%d = %d, stamina %d/%d", name, d->name, RECOVER_DIE, rolled, e->stamina, e->maxStamina);
}

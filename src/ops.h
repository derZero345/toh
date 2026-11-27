#ifndef TOH_OPS_H
#define TOH_OPS_H

/*
 * Every change to the shared table is an Op. ops_submit checks whether the local user
 * may do it (ops_check) and then applies it (ops_apply), the only code that changes
 * table state. Offline the local user is the GM. In a session (plan 3) clients send
 * their ops to the host, which checks them again and broadcasts the ones it applied.
 * Design: docs/superpowers/specs/2026-11-21-multiplayer-design.md, section 3.
 */

#include "app.h"

typedef enum {
    OP_MOVE,             /* id -> tile */
    OP_SPAWN,            /* specs; ops_apply stores the new entities' ids in ids */
    OP_DELETE,           /* ids */
    OP_SET_STATS,        /* ids; statMask says which of the values below change; an attribute bit also sets the
                          * stats it gives (attrs.h) unless they are in the mask too */
    OP_SET_ICON,         /* ids, image */
    OP_SET_BACKGROUND,   /* image, NULL = none */
    OP_GRID_ORIENTATION, /* toggles flat / pointy */
    OP_WAYPOINT_SET,     /* name, tile: creates the waypoint or moves it */
    OP_WAYPOINT_DELETE,  /* name */
    OP_INIT_ADD,         /* ids, values: one roll per id */
    OP_INIT_REMOVE,      /* ids */
    OP_INIT_CLEAR,
    OP_INIT_SHOW,        /* toggles the turn order markers */
    OP_INIT_NEXT,        /* id: the entity whose turn it is now (0 = none); stale ops are refused */
    OP_SET_OWNER,        /* ids, name: player name, "" = no owner */
    OP_SET_HEALTH,       /* ids, healthMode, value: set or change current HP (owners too) */
    OP_SET_NAME,         /* ids, charName (owners too) */
    OP_SET_STAMINA,      /* ids, healthMode (HEALTH_SET / HEALTH_CHANGE / STAMINA_SPEND), value (owners too) */
    OP_SET_PARTY,        /* ids, party ("" = no party); GM only */
    OP_MOVE_ACTION,      /* id, action (MoveAction), tile: a move action on the character's turn (moves.h) */
    OP_SET_TEST_MODE,    /* value: 1 on, 0 off; GM only (moves.h: what test mode lifts) */
    OP_RECOVER_ACTION,   /* id, action (RecoverAction), value: the host's roll (recover.h) */
    OP_ATTACK_ACTION,    /* id, action (AttackAction), tile, value: AttackDir; from the host also ids: the characters
                          * hit, and values: the damage roll per outcome, then each character's outcome (attacks.h) */
    OP_BLOCK_ACTION,     /* id, action (BlockAction), tile, value: the blocked directions, DIR_BIT bits (blocks.h) */
    OP_COUNT
} OpType;

enum { STAT_SIZE = 1, STAT_MAXHEALTH = 2, STAT_SPEED = 4, STAT_INITIATIVE = 8, STAT_MAXSTAMINA = 16,
       STAT_STR = 32, STAT_AGI = 64, STAT_COG = 128, STAT_SPI = 256, STAT_ALL = 511 };
#define STAT_ATTR(a) (STAT_STR << (a)) /* the bit of attribute a (attrs.h) */
#define STAT_ATTRS (STAT_STR | STAT_AGI | STAT_COG | STAT_SPI)
enum { HEALTH_SET = 0, HEALTH_CHANGE = 1 }; /* OP_SET_HEALTH: value is the new HP / is added to it */
enum { STAMINA_SPEND = 2 };                 /* OP_SET_STAMINA: subtract value; refused below 0 */

/* An entity that OP_SPAWN creates. */
typedef struct {
    EntityType type;
    Texture *image; /* borrowed; the new entity takes its own reference */
    Hex tile;
    int size, maxHealth, health, attack, speed, initiative; /* characters only */
    int maxStamina, stamina;                                /* characters only */
    int attr[ATTR_COUNT];                                   /* characters only */
    char name[CHAR_NAME_MAX]; /* characters: "" = no name */
    char party[CHAR_NAME_MAX]; /* characters: "" = no party */
} EntitySpec;

typedef struct Op {
    OpType type;
    uint32_t id; /* OP_MOVE */
    Hex tile;    /* OP_MOVE, OP_WAYPOINT_SET */
    DA(uint32_t) ids;
    DA(double) values;    /* OP_INIT_ADD; OP_ATTACK_ACTION: damage rolls and outcomes */
    DA(EntitySpec) specs; /* OP_SPAWN */
    Texture *image;       /* OP_SET_ICON, OP_SET_BACKGROUND; borrowed */
    int statMask;         /* OP_SET_STATS */
    int size, maxHealth, speed, initiative, maxStamina;
    int attr[ATTR_COUNT]; /* OP_SET_STATS: the attribute bits' values */
    char name[WAYPOINT_NAME_MAX]; /* OP_WAYPOINT_*: waypoint name, OP_SET_OWNER: player name */
    int healthMode, value;        /* OP_SET_HEALTH, OP_SET_STAMINA; value: OP_SET_TEST_MODE, OP_RECOVER_ACTION */
    int action;                   /* OP_MOVE_ACTION: MoveAction, OP_RECOVER_ACTION: RecoverAction,
                                   * OP_ATTACK_ACTION: AttackAction, OP_BLOCK_ACTION: BlockAction */
    char charName[CHAR_NAME_MAX]; /* OP_SET_NAME */
    char party[CHAR_NAME_MAX];    /* OP_SET_PARTY: "" = no party */
    bool remote;  /* someone else's op: leave the local selection and path preview alone */
    bool keepIds; /* OP_SPAWN from the host: create the entities with the ids in ids */
} Op;

/* Who sends an op. */
typedef struct {
    bool gm;
    char name[PLAYER_NAME_MAX]; /* players: their name, matched against Entity.owner */
} OpAuthor;

typedef enum {
    OPR_OK,
    OPR_GM_ONLY,
    OPR_NOT_YOURS,
    OPR_NOT_YOUR_TURN,
    OPR_OUT_OF_RANGE,
    OPR_NO_ENTITY,
    OPR_STALE,
    OPR_INVALID,
    OPR_ALREADY_OWNED,
    OPR_CLAIM_PENDING, /* session: the player's earlier claims and new characters (3) are still waiting for the GM */
    OPR_TOO_FAST,      /* session: too many rolls in a short time */
    OPR_TILE_TAKEN,    /* session: a requested character doesn't fit on its tile(s) */
    OPR_NO_STAMINA,    /* OP_SET_STAMINA, OP_MOVE_ACTION: spending would go below 0 */
    OPR_NOT_IN_COMBAT, /* OP_MOVE_ACTION: no initiative is running */
    OPR_ACTION_USED,   /* OP_MOVE_ACTION: the Major / Minor action is used this turn */
    OPR_MOVE_FIRST,    /* OP_MOVE_ACTION: Sprint needs another move action first */
    OPR_CANT_MOVE,     /* OP_MOVE_ACTION: speed 0 */
    OPR_USE_ACTION,    /* OP_MOVE by a player during initiative: move with a move action */
    OPR_NO_STAMINA_STAT, /* OP_RECOVER_ACTION: the character has no max stamina */
    OPR_STAMINA_FULL,    /* OP_RECOVER_ACTION: stamina is at its maximum */
    OPR_BLOCK_LOCKED,    /* OP_BLOCK_ACTION: the blocker's last block on that tile failed (blocks.h) */
} OpResult;
#define OPR_LAST OPR_BLOCK_LOCKED

/*
 * Notes for ops_check / ops_apply:
 * - ops_apply requires an op that passed ops_check (e.g. values.count == ids.count for OP_INIT_ADD).
 * - ops_apply looks ids up again one by one, so unknown or duplicate ids and walls in
 *   character-only ops are skipped harmlessly.
 * - ops_check does not limit GM moves to the movement range; the UI keeps the ":gm off"
 *   limit before calling ops_submit.
 * - Owner names match case-insensitively (ASCII).
 */
void       op_free(Op *op); /* frees the lists, not the borrowed textures */
EntitySpec op_spec_from_entity(const Entity *e);

OpAuthor    ops_local_author(void); /* GM offline and on the host, the player's name on a client */
OpResult    ops_check(const Op *op, const OpAuthor *author);
void        ops_apply(Op *op); /* op must have passed ops_check */
/* Checks op as the local user and applies it (on a client: sends it to the host, which applies
 * and broadcasts it). Logs the reason when it is refused. */
OpResult    ops_submit(Op *op);
const char *ops_result_text(OpResult r);

#endif

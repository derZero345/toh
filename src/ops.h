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
    OP_SET_STATS,        /* ids; statMask says which of the values below change */
    OP_SET_ICON,         /* ids, image */
    OP_SET_BACKGROUND,   /* image, NULL = none */
    OP_GRID_ORIENTATION, /* toggles flat / pointy */
    OP_WAYPOINT_SET,     /* name, tile: creates the waypoint or moves it */
    OP_WAYPOINT_DELETE,  /* name */
    OP_INIT_ADD,         /* ids, values: one roll per id */
    OP_INIT_REMOVE,      /* ids */
    OP_INIT_CLEAR,
    OP_INIT_SHOW,        /* toggles the turn order markers */
    OP_INIT_NEXT,
    OP_SET_OWNER,        /* ids, name: player name, "" = no owner */
    OP_COUNT
} OpType;

enum { STAT_SIZE = 1, STAT_MAXHEALTH = 2, STAT_AC = 4, STAT_SPEED = 8, STAT_INITIATIVE = 16 };

/* An entity that OP_SPAWN creates. */
typedef struct {
    EntityType type;
    Texture *image; /* borrowed; the new entity takes its own reference */
    Hex tile;
    int size, maxHealth, health, attack, ac, speed, initiative; /* characters only */
} EntitySpec;

typedef struct Op {
    OpType type;
    uint32_t id; /* OP_MOVE */
    Hex tile;    /* OP_MOVE, OP_WAYPOINT_SET */
    DA(uint32_t) ids;
    DA(double) values;    /* OP_INIT_ADD */
    DA(EntitySpec) specs; /* OP_SPAWN */
    Texture *image;       /* OP_SET_ICON, OP_SET_BACKGROUND; borrowed */
    int statMask;         /* OP_SET_STATS */
    int size, maxHealth, ac, speed, initiative;
    char name[WAYPOINT_NAME_MAX]; /* OP_WAYPOINT_*: waypoint name, OP_SET_OWNER: player name */
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
    OPR_INVALID,
} OpResult;

void       op_free(Op *op); /* frees the lists, not the borrowed textures */
EntitySpec op_spec_from_entity(const Entity *e);

OpAuthor    ops_local_author(void);
OpResult    ops_check(const Op *op, const OpAuthor *author);
void        ops_apply(Op *op); /* op must have passed ops_check */
/* Checks op as the local user and applies it. Logs the reason when it is refused. */
OpResult    ops_submit(Op *op);
const char *ops_result_text(OpResult r);

#endif

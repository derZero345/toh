/* Multiplayer sessions: commands, host and client message handling, image cache. See session.h. */
#include "session.h"
#include "net.h"
#include "portmap.h"
#include "presence.h"
#include "savefmt.h"

#include <sodium.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PLAYERS (NET_MAX_CONNS + 1) /* the GM and every connection */
#define ROLL_LIST_MAX 20                /* longer rolls only show the total in the log */

typedef struct {
    uint8_t h[NET_HASH_SIZE];
} Hash;
typedef DA(Hash) HashList;

typedef struct {
    bool used, welcomed, leaving;
    uint8_t id;
    uint8_t role; /* NET_ROLE_* */
    int conn;     /* host: the player's connection */
    char name[PLAYER_NAME_MAX];
    uint32_t color;
    HashList known; /* host: images this player has */
    NetPresence presence; /* the latest; never our own on a client */
    bool presenceDirty;   /* host: not forwarded yet */
    uint64_t presenceSent; /* host: when it was last forwarded */
} Player;

typedef struct {
    uint8_t player;
    uint32_t entity;
} Claim;

typedef struct {
    Hash hash;
    Texture *tex; /* one reference held by the cache */
} CachedImage;

typedef enum { ROLE_OFFLINE, ROLE_HOST, ROLE_JOINING, ROLE_CLIENT } Role;

static const uint32_t PALETTE[] = { 0x3A7BFF, 0xE5484D, 0x2DA44E, 0x8E4EC6, 0xF76B15, 0x12A5B5, 0xD6409F, 0x7CB518 };
static const char *const PALETTE_NAMES[] = { "blue", "red", "green", "purple", "orange", "cyan", "pink", "lime" };
#define NPALETTE ((int)(sizeof(PALETTE) / sizeof(PALETTE[0])))

static struct {
    Role role;
    Net *net;
    uint16_t port;
    char myName[PLAYER_NAME_MAX];
    uint8_t myId;
    Player players[MAX_PLAYERS]; /* host: [0] is the GM; client: the list from SESSION */
    DA(CachedImage) images;
    /* host */
    DA(Claim) claims;
    bool claimAsked;
    Portmap *pm;
    bool codeReady;
    JoinCode code; /* host: our code; client: the code we joined with */
    /* client */
    uint32_t seq;
    bool rejoined;
    /* own presence */
    bool presenceDirty;
    uint64_t presenceSent;
    uint8_t *sentPresence; /* the last one sent, encoded; NULL = send the next one in any case */
    size_t sentPresenceLen;
} S = { .port = NET_DEFAULT_PORT };

static const OpAuthor GM_AUTHOR = { .gm = true };

/* ---- small helpers ----------------------------------------------------------------------------- */

static void wake(void *ctx) {
    (void)ctx;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = gh->netEvent;
    SDL_PushEvent(&ev); /* thread-safe */
}

void session_init(void) { gh->netEvent = SDL_RegisterEvents(1); }
void session_set_port(uint16_t port) { S.port = port; }
bool session_is_host(void) { return S.role == ROLE_HOST; }
bool session_is_client(void) { return S.role == ROLE_JOINING || S.role == ROLE_CLIENT; }
const char *session_my_name(void) { return S.myName; }

bool session_ready(int n) {
    if (S.role == ROLE_CLIENT) return true;
    if (S.role != ROLE_HOST) return false;
    int count = 0;
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (S.players[i].welcomed) count++;
    return count >= n;
}

static const char *color_name(uint32_t c) {
    if (c == SESSION_GM_COLOR) return "gold";
    for (int i = 0; i < NPALETTE; i++)
        if (PALETTE[i] == c) return PALETTE_NAMES[i];
    return "?";
}

static void entity_label(const Entity *e, char *out, size_t cap) {
    snprintf(out, cap, "character #%u [%d|%d]", e->id, e->tile.row, e->tile.col);
}

static Player *player_by_id(uint8_t id) {
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (S.players[i].used && S.players[i].id == id) return &S.players[i];
    return NULL;
}

static Player *player_by_conn(int conn) {
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (S.players[i].used && S.players[i].conn == conn) return &S.players[i];
    return NULL;
}

static void player_clear(Player *p) {
    da_free(&p->known);
    netpresence_free(&p->presence);
    memset(p, 0, sizeof(*p));
}

static void forget_sent_presence(void) {
    free(S.sentPresence);
    S.sentPresence = NULL;
    S.sentPresenceLen = 0;
    S.presenceDirty = true;
}

static void players_clear(void) {
    for (int i = 0; i < MAX_PLAYERS; i++) player_clear(&S.players[i]);
    forget_sent_presence();
}

/* ---- image cache: hash -> texture -------------------------------------------------------------- */

static Texture *cache_find(const uint8_t h[NET_HASH_SIZE]) {
    for (int i = 0; i < S.images.count; i++)
        if (memcmp(S.images.items[i].hash.h, h, NET_HASH_SIZE) == 0) return S.images.items[i].tex;
    return NULL;
}

static void cache_add(const uint8_t h[NET_HASH_SIZE], Texture *t) {
    if (cache_find(h)) return;
    CachedImage c = { .tex = t };
    memcpy(c.hash.h, h, NET_HASH_SIZE);
    vkr_texture_retain(t);
    da_push(&S.images, c);
}

static void cache_clear(void) {
    for (int i = 0; i < S.images.count; i++) vkr_texture_release(S.images.items[i].tex);
    da_free(&S.images);
}

/* The hash that names t on the wire. Encodes t as WebP first if it has no WebP data yet. */
static bool texture_hash(Texture *t, uint8_t kind, uint8_t out[NET_HASH_SIZE]) {
    size_t len;
    int bkind, bw, bh;
    const uint8_t *blob = vkr_texture_blob(t, &len, &bkind, &bw, &bh);
    if (!blob) {
        const uint8_t *px = vkr_texture_pixels(t);
        SaveBlob b;
        if (!px || !savefmt_encode_image(px, vkr_texture_width(t), vkr_texture_height(t), kind, &b)) return false;
        vkr_texture_set_blob(t, b.webp, b.len, kind, b.w, b.h); /* the texture owns it now */
        blob = vkr_texture_blob(t, &len, &bkind, &bw, &bh);
        if (!blob) return false;
    }
    netmsg_image_hash(blob, len, out);
    cache_add(out, t);
    return true;
}

/* Hashes of every image on the table (background, entities). */
static void table_hashes(HashList *out) {
    Hash h;
    if (gh->backgroundImage && texture_hash(gh->backgroundImage, SAVE_IMG_LOSSY, h.h)) da_push(out, h);
    for (int i = 0; i < gh->entities.count; i++)
        if (texture_hash(gh->entities.items[i]->image, SAVE_IMG_LOSSLESS, h.h)) da_push(out, h);
}

/* ---- Op <-> NetOp ------------------------------------------------------------------------------- */

static void netop_free(NetOp *o) {
    free(o->ids);
    free(o->values);
    free(o->specs);
    memset(o, 0, sizeof(*o));
}

static bool net_from_op(const Op *op, NetOp *o) {
    memset(o, 0, sizeof(*o));
    o->type = (uint8_t)op->type;
    o->id = op->id;
    o->row = op->tile.row;
    o->col = op->tile.col;
    o->nids = (uint32_t)op->ids.count;
    o->ids = malloc(sizeof(uint32_t) * (o->nids ? o->nids : 1));
    o->nvalues = (uint32_t)op->values.count;
    o->values = malloc(sizeof(double) * (o->nvalues ? o->nvalues : 1));
    o->nspecs = (uint32_t)op->specs.count;
    o->specs = calloc(o->nspecs ? o->nspecs : 1, sizeof(NetSpec));
    if (!o->ids || !o->values || !o->specs) abort();
    if (o->nids) memcpy(o->ids, op->ids.items, sizeof(uint32_t) * o->nids);
    if (o->nvalues) memcpy(o->values, op->values.items, sizeof(double) * o->nvalues);
    o->stat_mask = (uint8_t)op->statMask;
    o->size = op->size;
    o->max_health = op->maxHealth;
    o->ac = op->ac;
    o->speed = op->speed;
    o->initiative = op->initiative;
    snprintf(o->name, sizeof(o->name), "%s", op->name);
    if (op->type == OP_SET_ICON || (op->type == OP_SET_BACKGROUND && op->image)) {
        o->has_image = true;
        uint8_t kind = op->type == OP_SET_BACKGROUND ? SAVE_IMG_LOSSY : SAVE_IMG_LOSSLESS;
        if (!op->image || !texture_hash(op->image, kind, o->image)) return false;
    }
    for (uint32_t i = 0; i < o->nspecs; i++) {
        const EntitySpec *s = &op->specs.items[i];
        NetSpec *n = &o->specs[i];
        n->type = s->type == ENTITY_WALL ? SAVE_ENT_WALL : SAVE_ENT_CHARACTER;
        n->row = s->tile.row;
        n->col = s->tile.col;
        n->size = s->size;
        n->max_health = s->maxHealth;
        n->health = s->health;
        n->attack = s->attack;
        n->ac = s->ac;
        n->speed = s->speed;
        n->initiative = s->initiative;
        if (!s->image || !texture_hash(s->image, SAVE_IMG_LOSSLESS, n->image)) return false;
    }
    return true;
}

/* Images come from the cache; false when one is unknown (the sender broke the protocol). */
static bool op_from_net(const NetOp *o, Op *op) {
    memset(op, 0, sizeof(*op));
    if (o->type >= OP_COUNT) return false;
    op->type = (OpType)o->type;
    op->id = o->id;
    op->tile = (Hex){ o->row, o->col };
    for (uint32_t i = 0; i < o->nids; i++) da_push(&op->ids, o->ids[i]);
    for (uint32_t i = 0; i < o->nvalues; i++) da_push(&op->values, o->values[i]);
    op->statMask = o->stat_mask;
    op->size = o->size;
    op->maxHealth = o->max_health;
    op->ac = o->ac;
    op->speed = o->speed;
    op->initiative = o->initiative;
    snprintf(op->name, sizeof(op->name), "%s", o->name);
    if (o->has_image && !(op->image = cache_find(o->image))) return false;
    for (uint32_t i = 0; i < o->nspecs; i++) {
        const NetSpec *n = &o->specs[i];
        EntitySpec s = { .type = n->type == SAVE_ENT_WALL ? ENTITY_WALL : ENTITY_CHARACTER,
                         .image = cache_find(n->image), .tile = { n->row, n->col }, .size = n->size,
                         .maxHealth = n->max_health, .health = n->health, .attack = n->attack, .ac = n->ac,
                         .speed = n->speed, .initiative = n->initiative };
        if (!s.image) return false;
        da_push(&op->specs, s);
    }
    return true;
}

/* ---- dice ----------------------------------------------------------------------------------------- */

static void log_roll(const char *who, const char *spec, const int *rolls, int n, int mod, int total) {
    char line[NETMSG_MAX_TEXT];
    int len = snprintf(line, sizeof(line), "%s rolled %s: ", who, spec);
    if (n <= ROLL_LIST_MAX) {
        for (int i = 0; i < n && len < (int)sizeof(line); i++)
            len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%d", i ? " + " : "", rolls[i]);
        if (mod && len < (int)sizeof(line))
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %c %d", mod > 0 ? '+' : '-', abs(mod));
        if (len < (int)sizeof(line)) snprintf(line + len, sizeof(line) - (size_t)len, " = %d", total);
    } else if (len < (int)sizeof(line)) {
        snprintf(line + len, sizeof(line) - (size_t)len, "%d", total);
    }
    console_log("%s", line);
}

/* ---- host ----------------------------------------------------------------------------------------- */

static void send_log(Player *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void send_log(Player *p, const char *fmt, ...) {
    NetMsg m = { .type = NM_LOG };
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m.text, sizeof(m.text), fmt, ap);
    va_end(ap);
    net_send(S.net, p->conn, &m);
}

static void send_image(Player *p, const uint8_t h[NET_HASH_SIZE]) {
    for (int i = 0; i < p->known.count; i++)
        if (memcmp(p->known.items[i].h, h, NET_HASH_SIZE) == 0) return;
    Texture *t = cache_find(h);
    size_t len;
    int kind, w, hh;
    const uint8_t *blob = t ? vkr_texture_blob(t, &len, &kind, &w, &hh) : NULL;
    if (!blob) return; /* cannot happen: hashed textures have WebP data */
    NetMsg m = { .type = NM_IMAGE, .image_kind = (uint8_t)kind, .data = blob, .data_len = len };
    memcpy(m.hash, h, NET_HASH_SIZE);
    if (!net_send(S.net, p->conn, &m)) return;
    Hash k;
    memcpy(k.h, h, NET_HASH_SIZE);
    da_push(&p->known, k);
}

/* Sends op (applied by author) to every player, with the images they don't have yet. */
static void broadcast_applied(const NetOp *o, uint8_t author) {
    NetMsg m = { .type = NM_OP_APPLIED, .player = author, .op = *o };
    for (int i = 1; i < MAX_PLAYERS; i++) {
        Player *p = &S.players[i];
        if (!p->welcomed || p->leaving) continue;
        if (o->has_image) send_image(p, o->image);
        for (uint32_t k = 0; k < o->nspecs; k++) send_image(p, o->specs[k].image);
        net_send(S.net, p->conn, &m);
    }
}

static void broadcast(const NetMsg *m, const Player *except) {
    for (int i = 1; i < MAX_PLAYERS; i++) {
        Player *p = &S.players[i];
        if (p->welcomed && !p->leaving && p != except) net_send(S.net, p->conn, m);
    }
}

void session_broadcast_op(const Op *op) {
    if (S.role != ROLE_HOST) return;
    NetOp o;
    if (net_from_op(op, &o)) broadcast_applied(&o, 0);
    else console_log("Could not send this change to the players (an image could not be encoded)");
    netop_free(&o);
}

static void host_roll(Player *author, const char *spec) {
    int num, sides, mod;
    if (!dice_parse(spec, &num, &sides, &mod)) return;
    int *rolls = calloc((size_t)(num > 0 ? num : 1), sizeof(int));
    if (!rolls) abort();
    int total = dice_roll(num, sides, mod, rolls);
    console_show_dice(rolls, num, total);
    log_roll(author->name, spec, rolls, num, mod, total);

    NetOp o = { .type = NOP_ROLL, .nvalues = (uint32_t)num + 1 };
    snprintf(o.dice, sizeof(o.dice), "%s", spec);
    o.values = malloc(sizeof(double) * o.nvalues);
    if (!o.values) abort();
    for (int i = 0; i < num; i++) o.values[i] = rolls[i];
    o.values[num] = total;
    broadcast_applied(&o, author->id);
    free(o.values);
    free(rolls);
}

/* Encodes the table for players; logs and returns false when it cannot be sent. */
static bool encode_snapshot(uint8_t **snap, size_t *len) {
    int err = save_encode_table(snap, len);
    if (err != SAVE_OK) {
        console_log("Could not send the table: %s", savefmt_strerror((SaveError)err));
        return false;
    }
    if (*len > NETMSG_MAX_BLOB) {
        free(*snap);
        console_log("The table is too large to send to players (over %u MiB)", NETMSG_MAX_BLOB >> 20);
        return false;
    }
    return true;
}

static void send_session(Player *p) {
    NetMsg m = { .type = NM_SESSION, .nentity_ids = (uint32_t)gh->entities.count };
    m.entity_ids = malloc(sizeof(uint32_t) * (m.nentity_ids ? m.nentity_ids : 1));
    NetPlayer list[MAX_PLAYERS];
    if (!m.entity_ids) abort();
    for (int i = 0; i < gh->entities.count; i++) m.entity_ids[i] = gh->entities.items[i]->id;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *q = &S.players[i];
        if (!q->welcomed) continue;
        NetPlayer *np = &list[m.nplayers++];
        *np = (NetPlayer){ .id = q->id, .role = q->role, .color = q->color };
        snprintf(np->name, sizeof(np->name), "%s", q->name);
    }
    m.players = list;
    net_send(S.net, p->conn, &m);
    free(m.entity_ids);
}

/* SNAPSHOT + SESSION; afterwards the player knows every image on the table. */
static void send_table(Player *p, const uint8_t *snap, size_t len, const HashList *hashes) {
    NetMsg m = { .type = NM_SNAPSHOT, .data = snap, .data_len = len };
    net_send(S.net, p->conn, &m);
    da_clear(&p->known);
    for (int i = 0; i < hashes->count; i++) da_push(&p->known, hashes->items[i]);
    send_session(p);
}

static void reject(Player *p, uint8_t reason) {
    NetMsg m = { .type = NM_REJECT, .reason = reason };
    net_send(S.net, p->conn, &m);
    net_close(S.net, p->conn);
    p->leaving = true;
}

static NetPlayer net_player(const Player *p) {
    NetPlayer np = { .id = p->id, .role = p->role, .color = p->color };
    snprintf(np.name, sizeof(np.name), "%s", p->name);
    return np;
}

static void welcome(Player *p, const char *name) {
    uint8_t *snap;
    size_t len;
    if (!encode_snapshot(&snap, &len)) {
        reject(p, NET_REJECT_SNAPSHOT);
        return;
    }
    /* the smallest free id and the first free color */
    for (int id = 1; id < 256; id++)
        if (!player_by_id((uint8_t)id)) {
            p->id = (uint8_t)id;
            break;
        }
    int welcomed = 0;
    p->color = 0;
    for (int c = 0; c < NPALETTE && !p->color; c++) {
        bool taken = false;
        for (int i = 1; i < MAX_PLAYERS; i++)
            if (S.players[i].welcomed && S.players[i].color == PALETTE[c]) taken = true;
        if (!taken) p->color = PALETTE[c];
    }
    for (int i = 1; i < MAX_PLAYERS; i++) welcomed += S.players[i].welcomed;
    if (!p->color) p->color = PALETTE[welcomed % NPALETTE];
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->role = NET_ROLE_PLAYER;
    p->welcomed = true;

    NetMsg w = { .type = NM_WELCOME, .player = p->id };
    net_send(S.net, p->conn, &w);
    HashList hashes = { 0 };
    table_hashes(&hashes);
    send_table(p, snap, len, &hashes);
    da_free(&hashes);
    free(snap);

    NetMsg j = { .type = NM_PLAYER_JOINED, .info = net_player(p) };
    broadcast(&j, p);
    for (int i = 0; i < MAX_PLAYERS; i++) { /* everyone's current presence */
        const Player *q = &S.players[i];
        if (!q->welcomed || q == p) continue;
        NetMsg pm = { .type = NM_PRESENCE, .player = q->id, .presence = q->presence };
        net_send(S.net, p->conn, &pm);
    }
    console_log("%s joined the table.", p->name);
}

static void drop(Player *p, const char *why) {
    console_log("Disconnected %s: %s", p->welcomed ? p->name : "a new connection", why);
    net_close(S.net, p->conn);
    p->leaving = true;
}

static void ask_next_claim(void);

static void host_op(Player *p, const NetOp *o, uint32_t seq) {
    NetMsg rej = { .type = NM_OP_REJECTED, .seq = seq };
    if (o->type == NOP_ROLL) {
        int num, sides, mod;
        if (!dice_parse(o->dice, &num, &sides, &mod)) {
            rej.reason = OPR_INVALID;
            net_send(S.net, p->conn, &rej);
        } else {
            host_roll(p, o->dice);
        }
        return;
    }
    if (o->type == NOP_CLAIM) {
        Entity *e = entity_by_id(o->id);
        if (!e || e->type != ENTITY_CHARACTER) rej.reason = OPR_NO_ENTITY;
        else if (e->owner[0]) rej.reason = OPR_ALREADY_OWNED;
        if (rej.reason) {
            net_send(S.net, p->conn, &rej);
            return;
        }
        da_push(&S.claims, ((Claim){ .player = p->id, .entity = o->id }));
        ask_next_claim();
        return;
    }
    Op op;
    if (!op_from_net(o, &op)) {
        op_free(&op);
        drop(p, "it sent an invalid action");
        return;
    }
    OpAuthor a = { .gm = false };
    snprintf(a.name, sizeof(a.name), "%s", p->name);
    OpResult r = ops_check(&op, &a);
    if (r != OPR_OK) {
        rej.reason = (uint8_t)r;
        net_send(S.net, p->conn, &rej);
    } else {
        op.remote = true;
        ops_apply(&op);
        NetOp applied;
        if (net_from_op(&op, &applied)) broadcast_applied(&applied, p->id);
        netop_free(&applied);
    }
    op_free(&op);
}

static void host_message(Player *p, const NetMsg *m) {
    if (p->leaving) return;
    if (!p->welcomed) {
        if (m->type != NM_HELLO) {
            drop(p, "it did not introduce itself");
        } else if (m->protocol != NET_PROTOCOL_VERSION) {
            reject(p, NET_REJECT_VERSION);
        } else {
            bool taken = false;
            for (int i = 0; i < MAX_PLAYERS; i++)
                if (S.players[i].welcomed && strcasecmp(S.players[i].name, m->name) == 0) taken = true;
            if (taken) reject(p, NET_REJECT_NAME_TAKEN);
            else welcome(p, m->name);
        }
        return;
    }
    if (m->type == NM_OP) {
        host_op(p, &m->op, m->seq);
    } else if (m->type == NM_PRESENCE) {
        netpresence_free(&p->presence);
        netpresence_copy(&p->presence, &m->presence);
        p->presenceDirty = true; /* forwarded by session_tick */
        session_tick();
    } else {
        drop(p, "it sent an unexpected message");
    }
}

static void host_event(NetEvent *ev) {
    Player *p = player_by_conn(ev->conn);
    switch (ev->type) {
    case NET_EV_CONNECTED:
        for (int i = 1; i < MAX_PLAYERS; i++) {
            if (S.players[i].used) continue;
            p = &S.players[i];
            p->used = true;
            p->conn = ev->conn;
            p->id = 0; /* assigned in welcome */
            return;
        }
        net_close(S.net, ev->conn); /* cannot happen: net.c caps connections */
        break;
    case NET_EV_MESSAGE:
        if (p) host_message(p, &ev->msg);
        break;
    case NET_EV_CLOSED:
        if (!p) break;
        if (p->welcomed) {
            if (ev->end == NET_END_BAD_DATA) console_log("%s sent invalid data and was disconnected.", p->name);
            else if (!p->leaving || ev->end != NET_END_NORMAL) console_log("%s left the table.", p->name);
            NetMsg left = { .type = NM_PLAYER_LEFT, .info = net_player(p) };
            p->welcomed = false;
            broadcast(&left, p);
            for (int i = S.claims.count - 1; i >= 0; i--)
                if (S.claims.items[i].player == p->id && !(i == 0 && S.claimAsked)) da_remove_at(&S.claims, i);
        }
        player_clear(p);
        break;
    }
}

/* ---- claims -------------------------------------------------------------------------------------- */

static void claim_answer(bool yes, void *ctx) {
    (void)ctx;
    S.claimAsked = false;
    if (S.claims.count == 0) return;
    Claim c = S.claims.items[0];
    da_remove_at(&S.claims, 0);
    Player *p = player_by_id(c.player);
    Entity *e = entity_by_id(c.entity);
    if (!p || !p->welcomed || p->leaving || !e) return;
    char label[64];
    entity_label(e, label, sizeof(label));
    if (!yes) {
        send_log(p, "The GM declined your claim.");
        return;
    }
    if (e->owner[0]) {
        send_log(p, "%s already has an owner.", label);
        return;
    }
    Op op = { .type = OP_SET_OWNER };
    da_push(&op.ids, e->id);
    snprintf(op.name, sizeof(op.name), "%s", p->name);
    if (ops_submit(&op) == OPR_OK) {
        send_log(p, "The GM gave you %s.", label);
        console_log("%s now belongs to %s.", label, p->name);
    }
    op_free(&op);
}

static void ask_next_claim(void) {
    while (S.role == ROLE_HOST && !S.claimAsked && S.claims.count > 0 && !console_confirm_busy()) {
        Claim *c = &S.claims.items[0];
        Player *p = player_by_id(c->player);
        Entity *e = entity_by_id(c->entity);
        if (!p || !p->welcomed || !e || e->owner[0]) { /* outdated */
            da_remove_at(&S.claims, 0);
            continue;
        }
        char label[64], q[160];
        entity_label(e, label, sizeof(label));
        snprintf(q, sizeof(q), "%s wants to claim %s. Allow? (y/n)", p->name, label);
        gfx_set_console_visibility(true);
        S.claimAsked = console_confirm(q, claim_answer, NULL);
        return;
    }
}

void session_prompt_free(void) { ask_next_claim(); }

/* ---- client -------------------------------------------------------------------------------------- */

static void end_client(void) {
    bool was_joined = S.role == ROLE_CLIENT;
    net_free(S.net);
    S.net = NULL;
    S.role = ROLE_OFFLINE;
    players_clear();
    cache_clear();
    if (was_joined) save_mark_modified(); /* the table is now an unsaved offline table */
}

static void lost_sync(const char *what) {
    fprintf(stderr, "lost sync: %s\n", what);
    if (!S.rejoined) {
        console_log("Lost sync with the host, rejoining...");
        S.rejoined = true;
        net_free(S.net);
        players_clear();
        cache_clear();
        S.role = ROLE_JOINING;
        S.net = net_join(&S.code, wake, NULL);
        if (!S.net) end_client();
        return;
    }
    console_log("Lost sync with the host.");
    end_client();
}

static const char *player_name(uint8_t id) {
    Player *p = player_by_id(id);
    return p ? p->name : "Someone";
}

static void client_applied(const NetMsg *m) {
    const NetOp *o = &m->op;
    if (o->type == NOP_ROLL) {
        int num = o->nvalues > 0 ? (int)o->nvalues - 1 : 0;
        int *rolls = calloc((size_t)(num ? num : 1), sizeof(int));
        if (!rolls) abort();
        int sum = 0;
        for (int i = 0; i < num; i++) sum += rolls[i] = (int)o->values[i];
        int total = o->nvalues ? (int)o->values[num] : 0;
        console_show_dice(rolls, num, total);
        log_roll(player_name(m->player), o->dice, rolls, num, total - sum, total);
        free(rolls);
        return;
    }
    Op op;
    if (!op_from_net(o, &op)) {
        op_free(&op);
        lost_sync("unknown image");
        return;
    }
    op.keepIds = true;
    OpResult r = ops_check(&op, &GM_AUTHOR); /* the host checked the rules; this only detects a desync */
    if (r != OPR_OK) {
        op_free(&op);
        lost_sync(ops_result_text(r));
        return;
    }
    op.remote = m->player != S.myId;
    ops_apply(&op);
    /* our own move: input.c dropped the selection when it sent the move; select the character again */
    if (op.type == OP_MOVE && !op.remote && gh->selectedEntityTiles.count == 0) {
        Entity *e = entity_by_id(op.id);
        if (e) io_select_entity(e);
    }
    op_free(&op);
}

static void client_image(const NetMsg *m) {
    uint8_t h[NET_HASH_SIZE];
    netmsg_image_hash(m->data, m->data_len, h);
    int w, hh;
    uint8_t *rgba;
    if (memcmp(h, m->hash, NET_HASH_SIZE) != 0 || !savefmt_decode_image(m->data, m->data_len, &w, &hh, &rgba)) {
        lost_sync("bad image");
        return;
    }
    Texture *t = vkr_texture_create(w, hh, rgba, false);
    free(rgba);
    uint8_t *blob = malloc(m->data_len);
    if (!blob) abort();
    memcpy(blob, m->data, m->data_len);
    vkr_texture_set_blob(t, blob, m->data_len, m->image_kind, w, hh);
    cache_add(h, t);
    vkr_texture_release(t); /* the cache holds it */
}

static void client_message(const NetMsg *m) {
    switch (m->type) {
    case NM_WELCOME:
        S.myId = m->player;
        break;
    case NM_SNAPSHOT: {
        if (!save_apply_snapshot(m->data, m->data_len)) {
            lost_sync("snapshot does not load");
            return;
        }
        HashList hashes = { 0 };
        table_hashes(&hashes); /* fills the cache */
        da_free(&hashes);
        break;
    }
    case NM_SESSION: {
        if (!entities_set_ids(m->entity_ids, m->nentity_ids)) {
            lost_sync("entity ids do not match the snapshot");
            return;
        }
        players_clear();
        const char *gm = "the GM";
        for (uint32_t i = 0; i < m->nplayers && i < MAX_PLAYERS; i++) {
            Player *p = &S.players[i];
            *p = (Player){ .used = true, .welcomed = true, .id = m->players[i].id, .role = m->players[i].role,
                           .color = m->players[i].color };
            snprintf(p->name, sizeof(p->name), "%s", m->players[i].name);
            if (p->role == NET_ROLE_GM) gm = p->name;
        }
        forget_sent_presence(); /* the host gets ours again */
        if (S.role == ROLE_JOINING) {
            S.role = ROLE_CLIENT;
            gh->gameMaster = false;
            console_log("Joined %s's table as %s.", gm, S.myName);
        }
        break;
    }
    case NM_PLAYER_JOINED:
        for (int i = 0; i < MAX_PLAYERS; i++) {
            Player *p = &S.players[i];
            if (p->used) continue;
            *p = (Player){ .used = true, .welcomed = true, .id = m->info.id, .role = m->info.role, .color = m->info.color };
            snprintf(p->name, sizeof(p->name), "%s", m->info.name);
            break;
        }
        console_log("%s joined the table.", m->info.name);
        break;
    case NM_PLAYER_LEFT: {
        Player *p = player_by_id(m->info.id);
        if (p) player_clear(p);
        console_log("%s left the table.", m->info.name);
        break;
    }
    case NM_OP_APPLIED:
        if (S.role == ROLE_CLIENT) client_applied(m);
        break;
    case NM_OP_REJECTED:
        console_log("%s", ops_result_text(m->reason <= OPR_ALREADY_OWNED ? (OpResult)m->reason : OPR_INVALID));
        break;
    case NM_IMAGE:
        client_image(m);
        break;
    case NM_LOG:
        console_log("%s", m->text);
        break;
    case NM_PRESENCE: {
        Player *p = player_by_id(m->player);
        if (!p || p->id == S.myId) break;
        netpresence_free(&p->presence);
        netpresence_copy(&p->presence, &m->presence);
        break;
    }
    case NM_REJECT: {
        const char *msg = "The host refused the connection.";
        char buf[96];
        if (m->reason == NET_REJECT_NAME_TAKEN) {
            snprintf(buf, sizeof(buf), "The name \"%s\" is already in use at this table.", S.myName);
            msg = buf;
        } else if (m->reason == NET_REJECT_VERSION) {
            msg = "The host runs a different version of ToH.";
        } else if (m->reason == NET_REJECT_FULL) {
            msg = "The table is full.";
        } else if (m->reason == NET_REJECT_KICKED) {
            msg = "You were removed from the table by the GM.";
        } else if (m->reason == NET_REJECT_SNAPSHOT) {
            msg = "The host could not send its table.";
        }
        console_log("%s", msg);
        end_client();
        break;
    }
    case NM_CLOSE:
        console_log("%s", m->reason == NET_CLOSE_KICKED ? "You were removed from the table by the GM."
                                                        : "The GM closed the session.");
        end_client();
        break;
    default:
        break;
    }
}

static void client_event(NetEvent *ev) {
    switch (ev->type) {
    case NET_EV_CONNECTED: {
        NetMsg hello = { .type = NM_HELLO, .protocol = NET_PROTOCOL_VERSION };
        snprintf(hello.name, sizeof(hello.name), "%s", S.myName);
        net_send(S.net, ev->conn, &hello);
        break;
    }
    case NET_EV_MESSAGE:
        client_message(&ev->msg);
        break;
    case NET_EV_CLOSED:
        if (S.role == ROLE_CLIENT) console_log("Connection to the host lost.");
        else if (ev->end == NET_END_UNREACHABLE)
            console_log("Could not reach the host. Is the GM still hosting and the port open?");
        else console_log("The host rejected the join code. Ask the GM for a new one.");
        end_client();
        break;
    }
}

void session_send_op(const Op *op) {
    if (S.role != ROLE_CLIENT) {
        console_log("Wait until you have joined the table");
        return;
    }
    NetMsg m = { .type = NM_OP, .seq = ++S.seq };
    if (net_from_op(op, &m.op) && !net_send(S.net, 1, &m)) console_log("Could not send that to the host");
    netop_free(&m.op);
}

/* ---- hosting ---------------------------------------------------------------------------------------- */

static void finish_hosting(const PortmapStatus *st) {
    S.codeReady = true;
    S.code.naddrs = (uint8_t)portmap_collect_addrs(st, S.code.addrs, NET_MAX_ADDRS);
    char code[JOINCODE_MAX_LEN];
    if (!joincode_encode(&S.code, code)) {
        console_log("Could not make a join code");
        return;
    }
    SDL_SetClipboardText(code);
    const char *file = getenv("TOH_JOINCODE_FILE"); /* test hook */
    if (file) {
        FILE *f = fopen(file, "w");
        if (f) {
            fprintf(f, "%s\n", code);
            fclose(f);
        }
    }
    if (!st) {
        console_log("Hosting on port %u (router port mapping off). Join code copied to clipboard.", S.code.port);
    } else if (st->method) {
        console_log("Hosting on port %u. Router port opened via %s. Join code copied to clipboard.", S.code.port, st->method);
    } else {
        console_log("Hosting on port %u. Router did not open the port: forward TCP %u to this computer, or use a VPN "
                    "such as Tailscale. Join code copied (LAN/VPN address only).", S.code.port, S.code.port);
    }
    if (st && st->cgnat)
        console_log("Your router is behind carrier-grade NAT (%u.%u.x.x): players outside your network cannot "
                    "reach you without a VPN such as Tailscale.", st->external[0], st->external[1]);
    console_log("Join code: %s", code);
}

static bool name_ok(const char *name) {
    if (savefmt_owner_valid(name)) return true;
    console_log("Names are 1-24 characters without spaces");
    return false;
}

void session_cmd_host(const char *name) {
    if (S.role != ROLE_OFFLINE) {
        console_log(S.role == ROLE_HOST ? "You are already hosting" : "Leave the table first (:leave)");
        return;
    }
    if (!name_ok(name)) return;
    uint8_t *snap;
    size_t len;
    if (!encode_snapshot(&snap, &len)) return;
    free(snap);

    memset(&S.code, 0, sizeof(S.code));
    randombytes_buf(S.code.secret, NET_SECRET_SIZE);
    char err[128];
    S.net = net_host(S.port, S.code.secret, wake, NULL, err, sizeof(err));
    if (!S.net) {
        console_log("Could not host: %s", err);
        return;
    }
    S.code.port = net_port(S.net);
    S.role = ROLE_HOST;
    snprintf(S.myName, sizeof(S.myName), "%s", name);
    players_clear();
    Player *gm = &S.players[0];
    *gm = (Player){ .used = true, .welcomed = true, .id = 0, .role = NET_ROLE_GM, .color = SESSION_GM_COLOR };
    snprintf(gm->name, sizeof(gm->name), "%s", name);
    S.codeReady = false;

    const char *off = getenv("TOH_NO_PORTMAP");
    if ((off && *off && strcmp(off, "0") != 0) || !portmap_available()) {
        finish_hosting(NULL);
        return;
    }
    S.pm = portmap_start(S.code.port, wake, NULL);
    console_log("Hosting on port %u. Opening the router port...", S.code.port);
}

static void end_host(void) {
    NetMsg bye = { .type = NM_CLOSE, .reason = NET_CLOSE_HOST_ENDED };
    broadcast(&bye, NULL);
    net_free(S.net); /* sends what is queued first */
    S.net = NULL;
    portmap_stop(S.pm); /* removes the router mapping */
    S.pm = NULL;
    da_free(&S.claims);
    S.claimAsked = false;
    players_clear();
    cache_clear();
    sodium_memzero(&S.code, sizeof(S.code));
    S.role = ROLE_OFFLINE;
}

void session_cmd_unhost(void) {
    if (S.role != ROLE_HOST) {
        console_log("You are not hosting");
        return;
    }
    end_host();
    console_log("Session closed.");
}

void session_table_replaced(void) {
    if (S.role != ROLE_HOST) return;
    uint8_t *snap;
    size_t len;
    if (!encode_snapshot(&snap, &len)) {
        console_log("Ending the session: the players cannot get this table.");
        end_host();
        return;
    }
    HashList hashes = { 0 };
    table_hashes(&hashes);
    for (int i = 1; i < MAX_PLAYERS; i++)
        if (S.players[i].welcomed && !S.players[i].leaving) send_table(&S.players[i], snap, len, &hashes);
    da_free(&hashes);
    free(snap);
}

/* ---- other commands ---------------------------------------------------------------------------------- */

void session_cmd_join(const char *code, const char *name) {
    if (S.role != ROLE_OFFLINE) {
        console_log(S.role == ROLE_HOST ? "End your session first (:unhost)" : "Leave the table first (:leave)");
        return;
    }
    JoinCode c;
    if (!joincode_decode(code, &c)) {
        console_log("That is not a valid join code.");
        return;
    }
    if (!name_ok(name)) return;
    S.code = c;
    snprintf(S.myName, sizeof(S.myName), "%s", name);
    S.rejoined = false;
    S.seq = 0;
    S.net = net_join(&S.code, wake, NULL);
    if (!S.net) {
        console_log("Could not start the network");
        return;
    }
    S.role = ROLE_JOINING;
    console_log("Joining...");
}

void session_cmd_leave(void) {
    if (!session_is_client()) {
        console_log(S.role == ROLE_HOST ? "The GM ends the session with :unhost" : "You are not at a table");
        return;
    }
    end_client();
    console_log("You left the table.");
}

void session_cmd_players(void) {
    if (S.role == ROLE_OFFLINE || S.role == ROLE_JOINING) {
        console_log("You are not at a table");
        return;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &S.players[i];
        if (!p->welcomed) continue;
        bool me = S.role == ROLE_HOST ? i == 0 : p->id == S.myId;
        console_log("%s%s - %s, %s", p->name, me ? " (you)" : "", p->role == NET_ROLE_GM ? "GM" : "player",
                    color_name(p->color));
    }
}

void session_cmd_kick(const char *name) {
    if (S.role != ROLE_HOST) {
        console_log("%s", ops_result_text(OPR_GM_ONLY));
        return;
    }
    for (int i = 1; i < MAX_PLAYERS; i++) {
        Player *p = &S.players[i];
        if (!p->welcomed || p->leaving || strcasecmp(p->name, name) != 0) continue;
        NetMsg bye = { .type = NM_CLOSE, .reason = NET_CLOSE_KICKED };
        net_send(S.net, p->conn, &bye);
        net_close(S.net, p->conn);
        p->leaving = true;
        console_log("Removed %s from the table.", p->name);
        return;
    }
    console_log("Nobody called \"%s\" is at the table", name);
}

void session_cmd_claim(void) {
    if (S.role == ROLE_HOST) {
        console_log("As GM, give characters with :c owner <name>");
        return;
    }
    if (S.role != ROLE_CLIENT) {
        console_log("You are not at a table");
        return;
    }
    Entity *found = NULL;
    int count = 0;
    for (int i = 0; i < gh->selectedEntityTiles.count; i++) {
        Entity *e = entity_at(gh->selectedEntityTiles.items[i]);
        if (!e || e == found) continue;
        found = e;
        count++;
    }
    if (count != 1 || found->type != ENTITY_CHARACTER) {
        console_log("Select exactly one character to claim");
        return;
    }
    if (found->owner[0]) {
        console_log("%s", ops_result_text(OPR_ALREADY_OWNED));
        return;
    }
    NetMsg m = { .type = NM_OP, .seq = ++S.seq, .op = { .type = NOP_CLAIM, .id = found->id } };
    char label[64];
    entity_label(found, label, sizeof(label));
    if (net_send(S.net, 1, &m)) console_log("Asked the GM for %s.", label);
}

bool session_roll(const char *spec) {
    if (S.role == ROLE_OFFLINE) return false;
    if (S.role == ROLE_HOST) {
        host_roll(&S.players[0], spec);
    } else if (S.role == ROLE_CLIENT) {
        NetMsg m = { .type = NM_OP, .seq = ++S.seq, .op = { .type = NOP_ROLL } };
        snprintf(m.op.dice, sizeof(m.op.dice), "%s", spec);
        if (!net_send(S.net, 1, &m)) console_log("Could not send that to the host");
    } else {
        console_log("Wait until you have joined the table");
    }
    return true;
}

/* ---- event pump --------------------------------------------------------------------------------------- */

void session_poll(void) {
    NetEvent ev;
    while (S.net && net_next(S.net, &ev)) {
        if (S.role == ROLE_HOST) host_event(&ev);
        else client_event(&ev);
        net_event_free(&ev);
    }
    PortmapStatus st;
    if (S.role == ROLE_HOST && S.pm && !S.codeReady && portmap_status(S.pm, &st) && st.done) finish_hosting(&st);
}

void session_shutdown(void) {
    if (S.role == ROLE_HOST) end_host();
    else if (session_is_client()) {
        net_free(S.net);
        S.net = NULL;
        S.role = ROLE_OFFLINE;
        players_clear();
        cache_clear();
    }
}

/* ---- presence ------------------------------------------------------------------------------------------ */

void session_presence_changed(void) { S.presenceDirty = true; }

static bool presence_empty(const NetPresence *p) {
    return !p->ntiles && !p->nentities && !p->npath && !p->nfootprint && !p->nlines && !p->has_circle && !p->has_cone;
}

/* Collects our presence and sends it when it differs from the last one sent. */
static void send_own_presence(uint64_t now) {
    NetMsg m = { .type = NM_PRESENCE, .player = S.role == ROLE_HOST ? S.players[0].id : 0 };
    presence_collect(&m.presence);
    uint8_t *buf;
    size_t len;
    if (!netmsg_encode(&m, &buf, &len)) {
        netpresence_free(&m.presence);
        return;
    }
    if (S.sentPresence && len == S.sentPresenceLen && memcmp(buf, S.sentPresence, len) == 0) {
        free(buf);
        netpresence_free(&m.presence);
        return;
    }
    free(S.sentPresence);
    S.sentPresence = buf;
    S.sentPresenceLen = len;
    S.presenceSent = now;
    if (S.role == ROLE_HOST) {
        broadcast(&m, NULL);
        Player *gm = &S.players[0];
        netpresence_free(&gm->presence);
        gm->presence = m.presence; /* new players get it in welcome */
        return;
    }
    net_send(S.net, 1, &m);
    netpresence_free(&m.presence);
}

static int wait_left(uint64_t now, uint64_t last) {
    uint64_t gone = now - last;
    return gone >= SESSION_PRESENCE_MS ? 0 : (int)(SESSION_PRESENCE_MS - gone);
}

int session_tick(void) {
    if (S.role != ROLE_HOST && S.role != ROLE_CLIENT) return -1;
    uint64_t now = SDL_GetTicks();
    int next = -1;
    if (S.presenceDirty) {
        int w = wait_left(now, S.presenceSent);
        if (w == 0) {
            S.presenceDirty = false;
            send_own_presence(now);
        } else {
            next = w;
        }
    }
    if (S.role != ROLE_HOST) return next;
    for (int i = 1; i < MAX_PLAYERS; i++) { /* forward the players' latest presence, coalesced */
        Player *p = &S.players[i];
        if (!p->welcomed || !p->presenceDirty) continue;
        int w = wait_left(now, p->presenceSent);
        if (w > 0) {
            if (next < 0 || w < next) next = w;
            continue;
        }
        p->presenceDirty = false;
        p->presenceSent = now;
        NetMsg m = { .type = NM_PRESENCE, .player = p->id, .presence = p->presence };
        broadcast(&m, p);
    }
    return next;
}

void session_draw_presence(void) {
    if (S.role != ROLE_HOST && S.role != ROLE_CLIENT) return;
    RemotePresence list[MAX_PLAYERS];
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &S.players[i];
        if (!p->welcomed || presence_empty(&p->presence)) continue;
        if (S.role == ROLE_HOST ? i == 0 : p->id == S.myId) continue;
        list[n++] = (RemotePresence){ &p->presence, p->color, p->name };
    }
    presence_draw(list, n);
}

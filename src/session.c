/* Multiplayer sessions: commands, host and client message handling, image cache. See session.h. */
#include "session.h"
#include "net.h"
#include "portmap.h"
#include "draw.h"
#include "presence.h"
#include "relaymsg.h"
#include "savefmt.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sodium.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_PLAYERS (NET_MAX_CONNS + 1) /* the GM and every connection */
#define ROLL_LIST_MAX 20                /* longer rolls only show the total in the log */
#define CLAIMS_PER_PLAYER SESSION_REQUESTS_MAX             /* host: a player's requests (claims, new characters) waiting for the GM */
#define ROLL_WINDOW_MS 10000            /* host: a player rolls at most ROLLS_PER_WINDOW times in this */
#define ROLLS_PER_WINDOW 10
#define BAN_KEY_MAX 64
#define OWNER_ABSENT_COLOR 0x9AA0A6u    /* ring of a character whose owner is not at the table */

typedef struct {
    uint8_t h[NET_HASH_SIZE];
} Hash;
typedef DA(Hash) HashList;

typedef struct {
    bool used, welcomed, leaving;
    uint8_t id;
    uint8_t role; /* NET_ROLE_* */
    int conn;     /* host: the player's connection */
    char addr[BAN_KEY_MAX]; /* host: ban_key of the player's address ("" = unknown) */
    uint64_t rollWindow;    /* host: start of the current ROLL_WINDOW_MS */
    int rolls;              /* host: rolls in it */
    char name[PLAYER_NAME_MAX];
    uint32_t color;
    HashList known; /* host: images this player has */
    NetPresence presence; /* the latest; never our own on a client */
    bool presenceDirty;   /* host: not forwarded yet */
    uint64_t presenceSent; /* host: when it was last forwarded */
} Player;

/* host: something a player asked the GM for, answered y/n in the console one at a time */
typedef enum { REQ_CLAIM, REQ_CREATE } RequestKind;
typedef struct {
    RequestKind kind;
    uint8_t player;
    uint32_t entity; /* REQ_CLAIM: the character */
    EntitySpec spec; /* REQ_CREATE: the new character; spec.image holds one texture reference */
    uint32_t seq;    /* REQ_CREATE: the player's CREATE seq, for the answer */
} Request;

/* host: someone the GM banned. A join is turned away when its name or its address matches. */
typedef struct {
    char name[PLAYER_NAME_MAX];
    char addr[BAN_KEY_MAX]; /* ban_key; "" = the name only (they were not at the table) */
} Ban;

typedef struct {
    Hash hash;
    Texture *tex; /* one reference held by the cache */
} CachedImage;

/* client: a create request the GM has not answered yet */
typedef struct {
    uint32_t seq;
    char name[CHAR_NAME_MAX];
} PendingCreate;

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
    DA(Request) requests; /* [0] is the one asked about while claimAsked */
    bool claimAsked;
    DA(Ban) bans; /* for as long as we host */
    Portmap *pm;
    bool codeReady;
    JoinCode code; /* host: our code; client: the code we joined with */
    char relay[NET_RELAY_ADDR_MAX]; /* "" = no relay */
    /* client */
    uint32_t seq;
    DA(PendingCreate) pending; /* our create requests the GM hasn't answered, oldest first */
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

void session_init(void) {
    gh->netEvent = SDL_RegisterEvents(1);
    const char *relay = getenv("TOH_RELAY");
#ifdef TOH_RELAY_DEFAULT
    if (!relay) relay = TOH_RELAY_DEFAULT;
#endif
    snprintf(S.relay, sizeof(S.relay), "%s", relay ? relay : "");
}

/* False (and says so) when the relay address cannot be used. */
static bool relay_addr_ok(void) {
    char host[NET_RELAY_ADDR_MAX];
    uint16_t port;
    if (relay_parse_addr(S.relay, host, sizeof(host), &port)) return true;
    console_log("The relay address \"%s\" is not valid; set TOH_RELAY=host:port.", S.relay);
    return false;
}
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

bool session_owner_color(const char *owner, uint32_t *color, bool *mine) {
    if ((S.role != ROLE_HOST && S.role != ROLE_CLIENT) || !owner[0]) return false;
    *mine = strcasecmp(owner, S.myName) == 0;
    *color = OWNER_ABSENT_COLOR;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (S.players[i].welcomed && strcasecmp(S.players[i].name, owner) == 0) *color = S.players[i].color;
    return true;
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

/* What an address ban matches: the IPv4 address (also when IPv4-mapped), or the IPv6 /64, since one
 * IPv6 host easily has a whole /64. "" when addr is not an IP address. */
static void ban_key(const char *addr, char *out, size_t cap) {
    struct in_addr a4;
    struct in6_addr a6;
    char ip[INET6_ADDRSTRLEN];
    out[0] = '\0';
    if (inet_pton(AF_INET, addr, &a4) == 1) {
        snprintf(out, cap, "%s", addr);
    } else if (inet_pton(AF_INET6, addr, &a6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&a6)) {
            inet_ntop(AF_INET, a6.s6_addr + 12, ip, sizeof(ip));
            snprintf(out, cap, "%s", ip);
        } else {
            memset(a6.s6_addr + 8, 0, 8);
            inet_ntop(AF_INET6, &a6, ip, sizeof(ip));
            snprintf(out, cap, "%s/64", ip);
        }
    }
}

/* The ban that turns away a join as name from addr (a ban_key), or NULL. */
static const Ban *ban_match(const char *name, const char *addr) {
    for (int i = 0; i < S.bans.count; i++) {
        const Ban *b = &S.bans.items[i];
        if (strcasecmp(b->name, name) == 0 || (addr[0] && strcmp(b->addr, addr) == 0)) return b;
    }
    return NULL;
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
    o->stat_mask = (uint16_t)op->statMask;
    o->size = op->size;
    o->max_health = op->maxHealth;
    o->speed = op->speed;
    o->initiative = op->initiative;
    o->max_stamina = op->maxStamina;
    memcpy(o->attr, op->attr, sizeof(o->attr));
    snprintf(o->name, sizeof(o->name), "%s", op->name);
    o->health_mode = (uint8_t)op->healthMode;
    o->value = op->value;
    snprintf(o->char_name, sizeof(o->char_name), "%s", op->charName);
    snprintf(o->party, sizeof(o->party), "%s", op->party);
    o->action = (uint8_t)op->action;
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
        n->speed = s->speed;
        n->initiative = s->initiative;
        n->max_stamina = s->maxStamina;
        n->stamina = s->stamina;
        memcpy(n->attr, s->attr, sizeof(n->attr));
        snprintf(n->name, sizeof(n->name), "%s", s->name);
        snprintf(n->party, sizeof(n->party), "%s", s->party);
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
    op->speed = o->speed;
    op->initiative = o->initiative;
    op->maxStamina = o->max_stamina;
    memcpy(op->attr, o->attr, sizeof(op->attr));
    snprintf(op->name, sizeof(op->name), "%s", o->name);
    op->healthMode = o->health_mode;
    op->value = o->value;
    snprintf(op->charName, sizeof(op->charName), "%s", o->char_name);
    snprintf(op->party, sizeof(op->party), "%s", o->party);
    op->action = o->action;
    if (o->has_image && !(op->image = cache_find(o->image))) return false;
    for (uint32_t i = 0; i < o->nspecs; i++) {
        const NetSpec *n = &o->specs[i];
        EntitySpec s = { .type = n->type == SAVE_ENT_WALL ? ENTITY_WALL : ENTITY_CHARACTER,
                         .image = cache_find(n->image), .tile = { n->row, n->col }, .size = n->size,
                         .maxHealth = n->max_health, .health = n->health, .attack = n->attack,
                         .speed = n->speed, .initiative = n->initiative,
                         .maxStamina = n->max_stamina, .stamina = n->stamina };
        memcpy(s.attr, n->attr, sizeof(s.attr));
        if (!s.image) return false;
        snprintf(s.name, sizeof(s.name), "%s", n->name);
        snprintf(s.party, sizeof(s.party), "%s", n->party);
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
    if (gh->testMode) { /* not part of the table: sent like the op that turned it on */
        NetMsg tm = { .type = NM_OP_APPLIED, .player = 0, .op = { .type = NOP_SET_TEST_MODE, .value = 1 } };
        net_send(S.net, p->conn, &tm);
    }

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

static void ask_next_request(void);

static void request_free(Request *r) {
    if (r->kind == REQ_CREATE) vkr_texture_release(r->spec.image);
    r->spec.image = NULL;
}

/* Removes request i and releases its texture. */
static void request_remove(int i) {
    request_free(&S.requests.items[i]);
    da_remove_at(&S.requests, i);
}

static int requests_of(uint8_t player) {
    int n = 0;
    for (int i = 0; i < S.requests.count; i++) n += S.requests.items[i].player == player;
    return n;
}

/* The new character fits there (Tales of Honor: tiles hold TILE_BULK, walls are full). */
static bool tiles_free(Hex tile, int size) { return character_fits(tile, size, NULL); }

static void send_create_answer(Player *p, uint32_t seq, bool accepted) {
    NetMsg a = { .type = NM_CREATE_ANSWER, .seq = seq, .accepted = accepted };
    net_send(S.net, p->conn, &a);
}

/* NM_CREATE: a player asks for a new character of their own. */
static void host_create(Player *p, const NetMsg *m) {
    NetMsg rej = { .type = NM_OP_REJECTED, .seq = m->seq };
    Hex tile = { m->spec.row, m->spec.col };
    int w = 0, h = 0;
    uint8_t *rgba = NULL;
    if (requests_of(p->id) >= CLAIMS_PER_PLAYER) rej.reason = OPR_CLAIM_PENDING;
    else if (!tiles_free(tile, m->spec.size)) rej.reason = OPR_TILE_TAKEN;
    else if (!savefmt_decode_image(m->data, m->data_len, &w, &h, &rgba) || w > NET_CREATE_MAX_DIM ||
             h > NET_CREATE_MAX_DIM)
        rej.reason = OPR_INVALID;
    for (int a = 0; a < NETMSG_ATTRS && !rej.reason; a++)
        if (m->spec.attr[a] < ATTR_CREATE_MIN || m->spec.attr[a] > ATTR_CREATE_MAX) rej.reason = OPR_INVALID;
    if (rej.reason) {
        free(rgba);
        net_send(S.net, p->conn, &rej);
        send_create_answer(p, m->seq, false);
        return;
    }
    Texture *t = vkr_texture_create_kept(w, h, rgba); /* takes rgba */
    uint8_t *blob = malloc(m->data_len);
    if (!blob) abort();
    memcpy(blob, m->data, m->data_len);
    vkr_texture_set_blob(t, blob, m->data_len, SAVE_IMG_LOSSLESS, w, h);
    Request r = { .kind = REQ_CREATE, .player = p->id };
    r.seq = m->seq;
    r.spec = (EntitySpec){ .type = ENTITY_CHARACTER, .image = t, .tile = tile, .size = m->spec.size,
                           .maxHealth = m->spec.max_health, .health = m->spec.max_health,
                           .speed = m->spec.speed, .initiative = m->spec.initiative,
                           .maxStamina = m->spec.max_stamina, .stamina = m->spec.max_stamina };
    memcpy(r.spec.attr, m->spec.attr, sizeof(r.spec.attr));
    snprintf(r.spec.name, sizeof(r.spec.name), "%s", m->spec.name);
    da_push(&S.requests, r);
    console_log("%s asks for a new character.", p->name);
    ask_next_request();
}

static void host_op(Player *p, const NetOp *o, uint32_t seq) {
    NetMsg rej = { .type = NM_OP_REJECTED, .seq = seq };
    if (o->type == NOP_ROLL) {
        int num, sides, mod;
        uint64_t now = SDL_GetTicks();
        if (now - p->rollWindow >= ROLL_WINDOW_MS) {
            p->rollWindow = now;
            p->rolls = 0;
        }
        if (!dice_parse(o->dice, &num, &sides, &mod)) rej.reason = OPR_INVALID;
        else if (++p->rolls > ROLLS_PER_WINDOW) rej.reason = OPR_TOO_FAST;
        if (rej.reason) net_send(S.net, p->conn, &rej);
        else host_roll(p, o->dice);
        return;
    }
    if (o->type == NOP_CLAIM) {
        Entity *e = entity_by_id(o->id);
        bool again = false;
        for (int i = 0; i < S.requests.count; i++) {
            const Request *r = &S.requests.items[i];
            if (r->player == p->id && r->kind == REQ_CLAIM && r->entity == o->id) again = true;
        }
        if (!e || e->type != ENTITY_CHARACTER) rej.reason = OPR_NO_ENTITY;
        else if (e->owner[0]) rej.reason = OPR_ALREADY_OWNED;
        else if (again || requests_of(p->id) >= CLAIMS_PER_PLAYER) rej.reason = OPR_CLAIM_PENDING;
        if (rej.reason) {
            net_send(S.net, p->conn, &rej);
            return;
        }
        da_push(&S.requests, ((Request){ .kind = REQ_CLAIM, .player = p->id, .entity = o->id }));
        ask_next_request();
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
    const Ban *b;
    if (p->leaving) return;
    if (!p->welcomed) {
        if (m->type != NM_HELLO) {
            drop(p, "it did not introduce itself");
        } else if (m->protocol != NET_PROTOCOL_VERSION) {
            reject(p, NET_REJECT_VERSION);
        } else if ((b = ban_match(m->name, p->addr))) {
            console_log("Turned away %s: banned as %s%s. :unban %s lets them in again.", m->name, b->name,
                        strcasecmp(b->name, m->name) ? " (same address)" : "", b->name);
            reject(p, NET_REJECT_KICKED);
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
    } else if (m->type == NM_CREATE) {
        host_create(p, m);
    } else if (m->type == NM_PRESENCE) {
        netpresence_free(&p->presence);
        netpresence_copy(&p->presence, &m->presence);
        p->presenceDirty = true; /* forwarded by session_tick */
        session_tick();
    } else {
        drop(p, "it sent an unexpected message");
    }
}

static void end_host(void);
static bool publish_code(char code[JOINCODE_MAX_LEN]);

/* NET_EV_RELAY: the relay accepted our room, or lost it. */
static void host_relay(const NetEvent *ev) {
    if (ev->up && !S.codeReady) {
        S.codeReady = true;
        char code[JOINCODE_MAX_LEN];
        if (!publish_code(code)) return;
        console_log("Hosting via relay %s. Join code copied to clipboard.", S.relay);
        console_log("Join code: %s", code);
    } else if (ev->up) {
        console_log("Relay connection restored.");
    } else if (!S.codeReady) { /* the first attempt failed: we are not hosting */
        if (ev->end == NET_END_RELAY_FULL) console_log("The relay is full (too many rooms). Try again later.");
        else if (ev->end == NET_END_ROOM_TAKEN) console_log("The relay refused the room. Try :host again.");
        else if (ev->end == NET_END_VERSION)
            console_log("The relay speaks a different version. Update the game or the relay.");
        else console_log("Could not reach the relay %s. Use :host --direct to host without it.", S.relay);
        end_host();
    } else {
        console_log("Lost the relay; players cannot join until it is back. Retrying...");
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
            ban_key(ev->addr, p->addr, sizeof(p->addr));
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
            for (int i = S.requests.count - 1; i >= 0; i--)
                if (S.requests.items[i].player == p->id && !(i == 0 && S.claimAsked)) request_remove(i);
        }
        player_clear(p);
        break;
    case NET_EV_RELAY:
        host_relay(ev);
        break;
    }
}

/* ---- requests (claims, new characters) ----------------------------------------------------------- */

static void answer_claim(const Request *r, Player *p, bool yes) {
    Entity *e = entity_by_id(r->entity);
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

static void answer_create(const Request *r, Player *p, bool yes) {
    if (!p || !p->welcomed || p->leaving) return;
    if (!yes) {
        send_log(p, "The GM declined your character.");
        send_create_answer(p, r->seq, false);
        return;
    }
    if (!tiles_free(r->spec.tile, r->spec.size)) {
        console_log("No room on [%d|%d] any more; %s's character was not created.", r->spec.tile.row,
                    r->spec.tile.col, p->name);
        NetMsg rej = { .type = NM_OP_REJECTED, .reason = OPR_TILE_TAKEN };
        net_send(S.net, p->conn, &rej);
        send_create_answer(p, r->seq, false);
        return;
    }
    Op spawn = { .type = OP_SPAWN };
    da_push(&spawn.specs, r->spec);
    if (ops_submit(&spawn) == OPR_OK && spawn.ids.count == 1) {
        Op own = { .type = OP_SET_OWNER };
        da_push(&own.ids, spawn.ids.items[0]);
        snprintf(own.name, sizeof(own.name), "%s", p->name);
        ops_submit(&own);
        op_free(&own);
        send_log(p, "The GM accepted your character.");
        send_create_answer(p, r->seq, true);
        console_log("Created %s's character.", p->name);
    }
    op_free(&spawn);
}

static void request_answer(bool yes, void *ctx) {
    (void)ctx;
    S.claimAsked = false;
    if (S.requests.count == 0) return;
    Request r = S.requests.items[0];
    da_remove_at(&S.requests, 0);
    Player *p = player_by_id(r.player);
    if (r.kind == REQ_CLAIM) answer_claim(&r, p, yes);
    else answer_create(&r, p, yes);
    request_free(&r);
}

static void ask_next_request(void) {
    while (S.role == ROLE_HOST && !S.claimAsked && S.requests.count > 0 && !console_confirm_busy()) {
        Request *c = &S.requests.items[0];
        Player *p = player_by_id(c->player);
        Entity *e = c->kind == REQ_CLAIM ? entity_by_id(c->entity) : NULL;
        if (!p || !p->welcomed || (c->kind == REQ_CLAIM && (!e || e->owner[0]))) { /* outdated */
            request_remove(0);
            continue;
        }
        char q[256];
        if (c->kind == REQ_CLAIM) {
            char label[64];
            entity_label(e, label, sizeof(label));
            snprintf(q, sizeof(q), "%s wants to claim %s. Allow? (y/n)", p->name, label);
        } else {
            /* short: the input line shows about 70 characters */
            char named[CHAR_NAME_MAX + 2] = "";
            if (c->spec.name[0]) snprintf(named, sizeof(named), " %s", c->spec.name);
            char st[24] = "";
            if (c->spec.maxStamina > 0) snprintf(st, sizeof(st), " ST %d", c->spec.maxStamina);
            char at[40];
            snprintf(at, sizeof(at), " STR %d AGI %d COG %d SPI %d", c->spec.attr[ATTR_STR], c->spec.attr[ATTR_AGI],
                     c->spec.attr[ATTR_COG], c->spec.attr[ATTR_SPI]);
            snprintf(q, sizeof(q), "%s: new character%s [%d|%d] size %d HP %d speed %d init %d%s%s. Allow? (y/n)",
                     p->name, named, c->spec.tile.row, c->spec.tile.col, c->spec.size, c->spec.maxHealth,
                     c->spec.speed, c->spec.initiative, st, at);
        }
        gfx_set_console_visibility(true);
        S.claimAsked = console_confirm(q, request_answer, NULL);
        return;
    }
}

void session_draw_request_preview(void) {
    if (S.role != ROLE_HOST || !S.claimAsked || S.requests.count == 0) return;
    const Request *r = &S.requests.items[0];
    if (r->kind != REQ_CREATE) return;
    float x, y, d;
    gfx_character_rect(r->spec.tile, r->spec.size, &x, &y, &d);
    dr_set_alpha(0.5f);
    dr_image(r->spec.image, x, y, d, d);
    dr_set_alpha(1.0f);
}

bool session_request_character(const Image *pic, Hex tile, int size, int maxHealth, int speed,
                               int initiative, int maxStamina, const char *name,
                               const int attr[ATTR_COUNT]) {
    if (S.role != ROLE_CLIENT) {
        console_log("Wait until you have joined the table");
        return false;
    }
    SaveBlob b;
    if (!savefmt_encode_image(pic->px, pic->w, pic->h, SAVE_IMG_LOSSLESS, &b)) {
        console_log("Could not encode the picture");
        return false;
    }
    NetMsg m = { .type = NM_CREATE, .seq = ++S.seq, .data = b.webp, .data_len = b.len };
    m.spec = (NetSpec){ .type = SAVE_ENT_CHARACTER, .row = tile.row, .col = tile.col, .size = size,
                        .max_health = maxHealth, .health = maxHealth,
                        .speed = speed > MAX_SPEED ? MAX_SPEED : speed, .initiative = initiative,
                        .max_stamina = maxStamina, .stamina = maxStamina };
    snprintf(m.spec.name, sizeof(m.spec.name), "%s", name ? name : "");
    if (attr) memcpy(m.spec.attr, attr, sizeof(m.spec.attr));
    bool sent = false;
    if (b.len > NET_CREATE_MAX_BLOB) console_log("The picture is too large");
    else if (!net_send(S.net, 1, &m)) console_log("Could not send that to the host");
    else {
        sent = true;
        console_log("Asked the GM to create your character.");
        PendingCreate pc = { .seq = m.seq };
        snprintf(pc.name, sizeof(pc.name), "%s", name ? name : "");
        da_push(&S.pending, pc);
    }
    free(b.webp);
    return sent;
}

int session_pending_count(void) { return S.role == ROLE_CLIENT ? S.pending.count : 0; }
const char *session_pending_name(int i) {
    return i >= 0 && i < session_pending_count() ? S.pending.items[i].name : "";
}

void session_prompt_free(void) { ask_next_request(); }

/* ---- client -------------------------------------------------------------------------------------- */

static Net *start_join(void) {
    return S.code.relay ? net_join_relay(S.relay, S.code.secret, wake, NULL) : net_join(&S.code, wake, NULL);
}

static void end_client(void) {
    bool was_joined = S.role == ROLE_CLIENT;
    net_free(S.net);
    S.net = NULL;
    S.role = ROLE_OFFLINE;
    gh->testMode = false; /* it was the host's */
    da_clear(&S.pending);
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
        da_clear(&S.pending); /* the host drops a leaving player's requests: they won't be answered */
        S.role = ROLE_JOINING;
        S.net = start_join();
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
    /* our own move: input.c / move_ui.c dropped the selection when it sent the move; select the character again */
    if ((op.type == OP_MOVE || op.type == OP_MOVE_ACTION) && !op.remote && gh->selectedEntities.count == 0) {
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
        gh->testMode = false; /* the host sends its own when it is on */
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
        console_log("%s", ops_result_text(m->reason <= OPR_LAST ? (OpResult)m->reason : OPR_INVALID));
        break;
    case NM_CREATE_ANSWER:
        for (int i = 0; i < S.pending.count; i++)
            if (S.pending.items[i].seq == m->seq) {
                da_remove_at(&S.pending, i); /* keeps the order */
                break;
            }
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
            msg = "The GM banned you from this table.";
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
        else if (ev->end == NET_END_UNREACHABLE && S.code.relay) console_log("Could not reach the relay %s.", S.relay);
        else if (ev->end == NET_END_UNREACHABLE)
            console_log("Could not reach the host. Is the GM still hosting and the port open?");
        else if (ev->end == NET_END_NO_ROOM)
            console_log("No game with this join code is hosted right now. Ask the GM for a new one.");
        else if (ev->end == NET_END_RELAY_FULL) console_log("The relay is full. Try again later.");
        else if (ev->end == NET_END_TIMEOUT) console_log("The host did not answer.");
        else if (ev->end == NET_END_VERSION)
            console_log("The relay speaks a different version. Update the game or the relay.");
        else if (S.code.relay) console_log("The relay closed the connection. Try again.");
        else console_log("The host rejected the join code. Ask the GM for a new one.");
        end_client();
        break;
    case NET_EV_RELAY: /* only hosts get these */
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

/* Copies our join code to the clipboard (and the test hook file). False when there is none. */
static bool publish_code(char code[JOINCODE_MAX_LEN]) {
    if (!joincode_encode(&S.code, code)) {
        console_log("Could not make a join code");
        return false;
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
    return true;
}

static void finish_hosting(const PortmapStatus *st) {
    S.codeReady = true;
    S.code.naddrs = (uint8_t)portmap_collect_addrs(st, S.code.addrs, NET_MAX_ADDRS);
    char code[JOINCODE_MAX_LEN];
    if (!publish_code(code)) return;
    if (!st) {
        console_log("Hosting on port %u (router port mapping off). Join code copied to clipboard.", S.code.port);
    } else if (st->method) {
        console_log("Hosting on port %u. Router port opened via %s. Join code copied to clipboard.", S.code.port, st->method);
    } else {
        console_log("Hosting on port %u. The router did not open the port (%s).", S.code.port,
                    st->why[0] ? st->why : "no answer");
        if (st->hint) console_log("%s", st->hint);
        console_log("Forward TCP %u to this computer by hand, or use a VPN such as Tailscale. Join code copied%s.",
                    S.code.port, st->have_external && !st->cgnat ? "" : " (LAN/VPN addresses only)");
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

void session_cmd_host(const char *name, bool direct) {
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
    bool relay = !direct && S.relay[0];
    if (relay && !relay_addr_ok()) return;
    randombytes_buf(S.code.secret, NET_SECRET_SIZE);
    if (relay) {
        S.code.relay = true;
        S.net = net_host_relay(S.relay, S.code.secret, wake, NULL);
        if (!S.net) {
            console_log("Could not start the network");
            return;
        }
    } else {
        char err[128];
        S.net = net_host(S.port, S.code.secret, wake, NULL, err, sizeof(err));
        if (!S.net) {
            console_log("Could not host: %s", err);
            return;
        }
        S.code.port = net_port(S.net);
    }
    S.role = ROLE_HOST;
    snprintf(S.myName, sizeof(S.myName), "%s", name);
    players_clear();
    Player *gm = &S.players[0];
    *gm = (Player){ .used = true, .welcomed = true, .id = 0, .role = NET_ROLE_GM, .color = SESSION_GM_COLOR };
    snprintf(gm->name, sizeof(gm->name), "%s", name);
    S.codeReady = false;

    if (relay) {
        console_log("Connecting to the relay %s...", S.relay);
        return;
    }
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
    for (int i = S.requests.count - 1; i >= 0; i--) request_remove(i);
    da_free(&S.requests);
    S.claimAsked = false;
    da_free(&S.bans);
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
    if (c.relay && !S.relay[0]) {
        console_log("This join code goes through a relay, but none is set (TOH_RELAY=host:port).");
        return;
    }
    if (c.relay && !relay_addr_ok()) return;
    if (!name_ok(name)) return;
    S.code = c;
    snprintf(S.myName, sizeof(S.myName), "%s", name);
    S.rejoined = false;
    S.seq = 0;
    S.net = start_join();
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

/* The player at the table called name (any case), or NULL. */
static Player *player_by_name(const char *name) {
    for (int i = 1; i < MAX_PLAYERS; i++) {
        Player *p = &S.players[i];
        if (p->welcomed && !p->leaving && strcasecmp(p->name, name) == 0) return p;
    }
    return NULL;
}

static void remove_player(Player *p) {
    NetMsg bye = { .type = NM_CLOSE, .reason = NET_CLOSE_KICKED };
    net_send(S.net, p->conn, &bye);
    net_close(S.net, p->conn);
    p->leaving = true;
}

void session_cmd_kick(const char *name) {
    if (S.role != ROLE_HOST) {
        console_log("%s", ops_result_text(OPR_GM_ONLY));
        return;
    }
    Player *p = player_by_name(name);
    if (!p) {
        console_log("Nobody called \"%s\" is at the table", name);
        return;
    }
    remove_player(p);
    console_log("Removed %s from the table. :ban %s keeps them out.", p->name, p->name);
}

void session_cmd_ban(const char *name) {
    if (S.role != ROLE_HOST) {
        console_log("%s", ops_result_text(OPR_GM_ONLY));
        return;
    }
    if (!name) {
        if (S.bans.count == 0) console_log("Nobody is banned.");
        for (int i = 0; i < S.bans.count; i++)
            console_log("%s%s%s", S.bans.items[i].name, S.bans.items[i].addr[0] ? " - address " : " - name only",
                        S.bans.items[i].addr);
        return;
    }
    if (!name_ok(name)) return;
    for (int i = 0; i < S.bans.count; i++) {
        if (strcasecmp(S.bans.items[i].name, name) == 0) {
            console_log("%s is banned already.", S.bans.items[i].name);
            return;
        }
    }
    Player *p = player_by_name(name);
    Ban b = { 0 };
    snprintf(b.name, sizeof(b.name), "%s", p ? p->name : name);
    if (p) snprintf(b.addr, sizeof(b.addr), "%s", p->addr);
    da_push(&S.bans, b);
    if (p) {
        remove_player(p);
        if (b.addr[0]) console_log("Banned %s and the address %s.", b.name, b.addr);
        else console_log("Banned the name %s.", b.name);
        console_log(":unban %s lifts it.", b.name);
    } else {
        console_log("Nobody called %s is at the table; banned the name only.", b.name);
    }
}

void session_cmd_unban(const char *name) {
    if (S.role != ROLE_HOST) {
        console_log("%s", ops_result_text(OPR_GM_ONLY));
        return;
    }
    for (int i = 0; i < S.bans.count; i++) {
        if (strcasecmp(S.bans.items[i].name, name) != 0) continue;
        console_log("%s may join again.", S.bans.items[i].name);
        da_remove_at(&S.bans, i);
        return;
    }
    console_log("Nobody called \"%s\" is banned. :ban lists the bans.", name);
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
    for (int i = 0; i < gh->selectedEntities.count; i++) {
        Entity *e = entity_by_id(gh->selectedEntities.items[i]);
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

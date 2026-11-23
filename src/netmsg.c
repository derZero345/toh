/* Network message and join code encoding, decoding and validation. See netmsg.h. */
#include "netmsg.h"
#include "wire.h"

#include <math.h>
#include <sodium.h>

/* ---- validation (shared by encoder and decoder) ------------------------------------------ */

static bool coord_ok(int32_t v) { return v >= -SAVEFMT_MAX_COORD && v <= SAVEFMT_MAX_COORD; }

static bool text_ok(const char *s, size_t cap) {
    size_t n = strnlen(s, cap);
    return n < cap && utf8_valid(s, n);
}

static bool ids_ok(const uint32_t *ids, uint32_t n) {
    if (n > SAVEFMT_MAX_ENTITIES || (n && !ids)) return false;
    for (uint32_t i = 0; i < n; i++)
        if (ids[i] == 0) return false;
    return true;
}

static bool dice_ok(const char *s) {
    size_t n = strnlen(s, NETMSG_MAX_DICE);
    if (n == 0 || n >= NETMSG_MAX_DICE) return false;
    for (size_t i = 0; i < n; i++)
        if (!strchr("0123456789dD+-", s[i])) return false;
    return true;
}

static bool spec_ok(const NetSpec *s) {
    if (s->type > SAVE_ENT_WALL || !coord_ok(s->row) || !coord_ok(s->col)) return false;
    return s->type != SAVE_ENT_CHARACTER || s->speed <= SAVEFMT_MAX_SPEED;
}

static bool op_ok(const NetOp *o) {
    if (o->type >= NOP_COUNT || !ids_ok(o->ids, o->nids)) return false;
    if (o->nvalues > SAVEFMT_MAX_ENTITIES || o->nspecs > SAVEFMT_MAX_ENTITIES) return false;
    switch (o->type) {
    case NOP_MOVE:
        return o->id != 0 && coord_ok(o->row) && coord_ok(o->col);
    case NOP_SPAWN:
        if (o->nspecs && !o->specs) return false;
        if (o->nids != 0 && o->nids != o->nspecs) return false;
        for (uint32_t i = 0; i < o->nspecs; i++)
            if (!spec_ok(&o->specs[i])) return false;
        return true;
    case NOP_SET_STATS:
        if (o->stat_mask == 0 || o->stat_mask > NSTAT_ALL) return false;
        return !(o->stat_mask & NSTAT_SPEED) || o->speed <= SAVEFMT_MAX_SPEED;
    case NOP_SET_ICON:
        return o->has_image;
    case NOP_WAYPOINT_SET:
        if (!coord_ok(o->row) || !coord_ok(o->col)) return false;
        /* fall through */
    case NOP_WAYPOINT_DELETE:
        return o->name[0] != '\0' && text_ok(o->name, sizeof(o->name));
    case NOP_INIT_ADD:
        if (o->nvalues != o->nids || (o->nvalues && !o->values)) return false;
        for (uint32_t i = 0; i < o->nvalues; i++)
            if (!isfinite(o->values[i])) return false;
        return true;
    case NOP_SET_OWNER:
        return o->name[0] == '\0' || savefmt_owner_valid(o->name);
    case NOP_ROLL:
        return dice_ok(o->dice);
    case NOP_CLAIM:
        return o->id != 0;
    default: /* DELETE, INIT_REMOVE (ids only), SET_BACKGROUND, INIT_NEXT, GRID / CLEAR / SHOW */
        return true;
    }
}

static bool player_ok(const NetPlayer *p) {
    return p->role <= NET_ROLE_PLAYER && p->color <= NET_COLOR_MAX && savefmt_owner_valid(p->name);
}

static bool blob_ok(const NetMsg *m) { return m->data && m->data_len >= 1 && m->data_len <= NETMSG_MAX_BLOB; }

static bool msg_ok(const NetMsg *m) {
    switch (m->type) {
    case NM_HELLO:         return savefmt_owner_valid(m->name);
    case NM_WELCOME:       return true;
    case NM_REJECT:        return m->reason >= 1 && m->reason <= NET_REJECT_MAX;
    case NM_CLOSE:         return m->reason >= 1 && m->reason <= NET_CLOSE_MAX;
    case NM_OP_REJECTED:   return m->reason >= 1 && m->reason <= NETMSG_MAX_REASON;
    case NM_SNAPSHOT:      return blob_ok(m);
    case NM_IMAGE:         return blob_ok(m) && m->image_kind <= SAVE_IMG_LOSSY;
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT:   return player_ok(&m->info);
    case NM_OP:
    case NM_OP_APPLIED:    return op_ok(&m->op);
    case NM_LOG:           return text_ok(m->text, sizeof(m->text));
    case NM_SESSION:
        if (!ids_ok(m->entity_ids, m->nentity_ids)) return false;
        if (m->nplayers < 1 || m->nplayers > NETMSG_MAX_PLAYERS || !m->players) return false;
        for (uint32_t i = 0; i < m->nplayers; i++)
            if (!player_ok(&m->players[i])) return false;
        return true;
    default:               return false; /* 0, NM_PRESENCE (plan 4), unknown */
    }
}

/* ---- encoding ----------------------------------------------------------------------------- */

static void put_ids(Buf *b, const uint32_t *ids, uint32_t n) {
    put_uv(b, n);
    for (uint32_t i = 0; i < n; i++) put_uv(b, ids[i]);
}

static void put_player(Buf *b, const NetPlayer *p) {
    put_u8(b, p->id);
    put_u8(b, p->role);
    put_uv(b, p->color);
    put_str(b, p->name);
}

static void put_op(Buf *b, const NetOp *o) {
    put_u8(b, o->type);
    switch (o->type) {
    case NOP_MOVE:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        break;
    case NOP_SPAWN:
        put_uv(b, o->nspecs);
        for (uint32_t i = 0; i < o->nspecs; i++) {
            const NetSpec *s = &o->specs[i];
            put_u8(b, s->type);
            put_bytes(b, s->image, NET_HASH_SIZE);
            put_sv(b, s->row);
            put_sv(b, s->col);
            if (s->type == SAVE_ENT_CHARACTER) {
                put_sv(b, s->size);
                put_sv(b, s->max_health);
                put_sv(b, s->health);
                put_sv(b, s->attack);
                put_sv(b, s->ac);
                put_sv(b, s->speed);
                put_sv(b, s->initiative);
            }
        }
        put_ids(b, o->ids, o->nids);
        break;
    case NOP_DELETE:
    case NOP_INIT_REMOVE:
        put_ids(b, o->ids, o->nids);
        break;
    case NOP_SET_STATS: {
        put_ids(b, o->ids, o->nids);
        put_u8(b, o->stat_mask);
        const int32_t v[5] = { o->size, o->max_health, o->ac, o->speed, o->initiative };
        for (int i = 0; i < 5; i++)
            if (o->stat_mask & (1 << i)) put_sv(b, v[i]);
        break;
    }
    case NOP_SET_ICON:
        put_ids(b, o->ids, o->nids);
        put_bytes(b, o->image, NET_HASH_SIZE);
        break;
    case NOP_SET_BACKGROUND:
        put_u8(b, o->has_image);
        if (o->has_image) put_bytes(b, o->image, NET_HASH_SIZE);
        break;
    case NOP_WAYPOINT_SET:
        put_str(b, o->name);
        put_sv(b, o->row);
        put_sv(b, o->col);
        break;
    case NOP_WAYPOINT_DELETE:
        put_str(b, o->name);
        break;
    case NOP_INIT_ADD:
        put_ids(b, o->ids, o->nids);
        for (uint32_t i = 0; i < o->nvalues; i++) put_f64(b, o->values[i]);
        break;
    case NOP_INIT_NEXT:
    case NOP_CLAIM:
        put_uv(b, o->id);
        break;
    case NOP_SET_OWNER:
        put_ids(b, o->ids, o->nids);
        put_str(b, o->name);
        break;
    case NOP_ROLL:
        put_str(b, o->dice);
        break;
    default: /* GRID_ORIENTATION, INIT_CLEAR, INIT_SHOW */
        break;
    }
}

bool netmsg_encode(const NetMsg *m, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!msg_ok(m)) return false;
    Buf b = { 0 };
    put_u8(&b, m->type);
    switch (m->type) {
    case NM_HELLO:
        put_uv(&b, m->protocol);
        put_str(&b, m->name);
        break;
    case NM_WELCOME:
        put_u8(&b, m->player);
        break;
    case NM_REJECT:
    case NM_CLOSE:
        put_u8(&b, m->reason);
        break;
    case NM_SNAPSHOT:
        put_bytes(&b, m->data, m->data_len);
        break;
    case NM_SESSION:
        put_ids(&b, m->entity_ids, m->nentity_ids);
        put_uv(&b, m->nplayers);
        for (uint32_t i = 0; i < m->nplayers; i++) put_player(&b, &m->players[i]);
        break;
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT:
        put_player(&b, &m->info);
        break;
    case NM_OP:
        put_uv(&b, m->seq);
        put_op(&b, &m->op);
        break;
    case NM_OP_APPLIED:
        put_u8(&b, m->player);
        put_op(&b, &m->op);
        break;
    case NM_OP_REJECTED:
        put_uv(&b, m->seq);
        put_u8(&b, m->reason);
        break;
    case NM_IMAGE:
        put_bytes(&b, m->hash, NET_HASH_SIZE);
        put_u8(&b, m->image_kind);
        put_bytes(&b, m->data, m->data_len);
        break;
    case NM_LOG:
        put_str(&b, m->text);
        break;
    }
    if (b.oom) {
        free(b.p);
        return false;
    }
    *out = b.p;
    *out_len = b.len;
    return true;
}

/* ---- decoding ----------------------------------------------------------------------------- */

/* Reads a count of items that take at least min_bytes each, so a short message cannot
 * make the decoder allocate a lot of memory. */
static uint32_t get_count(Rd *r, uint32_t max, size_t min_bytes) {
    uint32_t n = get_uv(r);
    if (r->bad) return 0;
    if (n > max || (size_t)n * min_bytes > r->len - r->pos) {
        r->bad = true;
        return 0;
    }
    return n;
}

static void get_bytes(Rd *r, void *dst, size_t n) {
    if (!rd_need(r, n)) {
        memset(dst, 0, n);
        return;
    }
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
}

/* calloc that marks the reader bad when out of memory; NULL for n == 0. */
static void *get_array(Rd *r, uint32_t n, size_t size) {
    if (r->bad || n == 0) return NULL;
    void *p = calloc(n, size);
    if (!p) r->bad = true;
    return p;
}

static uint32_t *get_ids(Rd *r, uint32_t *n_out) {
    uint32_t n = get_count(r, SAVEFMT_MAX_ENTITIES, 1);
    uint32_t *ids = get_array(r, n, sizeof(uint32_t));
    for (uint32_t i = 0; i < n && !r->bad; i++) ids[i] = get_uv(r);
    *n_out = ids ? n : 0;
    return ids;
}

static void get_player(Rd *r, NetPlayer *p) {
    p->id = get_u8(r);
    p->role = get_u8(r);
    p->color = get_uv(r);
    get_str(r, p->name, sizeof(p->name));
}

static void get_op(Rd *r, NetOp *o) {
    o->type = get_u8(r);
    switch (o->type) {
    case NOP_MOVE:
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        break;
    case NOP_SPAWN: {
        uint32_t n = get_count(r, SAVEFMT_MAX_ENTITIES, 1 + NET_HASH_SIZE + 2);
        o->specs = get_array(r, n, sizeof(NetSpec));
        o->nspecs = o->specs ? n : 0;
        for (uint32_t i = 0; i < o->nspecs && !r->bad; i++) {
            NetSpec *s = &o->specs[i];
            s->type = get_u8(r);
            get_bytes(r, s->image, NET_HASH_SIZE);
            s->row = get_sv(r);
            s->col = get_sv(r);
            if (s->type == SAVE_ENT_CHARACTER) {
                s->size = get_sv(r);
                s->max_health = get_sv(r);
                s->health = get_sv(r);
                s->attack = get_sv(r);
                s->ac = get_sv(r);
                s->speed = get_sv(r);
                s->initiative = get_sv(r);
            }
        }
        o->ids = get_ids(r, &o->nids);
        break;
    }
    case NOP_DELETE:
    case NOP_INIT_REMOVE:
        o->ids = get_ids(r, &o->nids);
        break;
    case NOP_SET_STATS: {
        o->ids = get_ids(r, &o->nids);
        o->stat_mask = get_u8(r);
        int32_t v[5] = { 0 };
        for (int i = 0; i < 5; i++)
            if (o->stat_mask & (1 << i)) v[i] = get_sv(r);
        o->size = v[0];
        o->max_health = v[1];
        o->ac = v[2];
        o->speed = v[3];
        o->initiative = v[4];
        break;
    }
    case NOP_SET_ICON:
        o->ids = get_ids(r, &o->nids);
        o->has_image = true;
        get_bytes(r, o->image, NET_HASH_SIZE);
        break;
    case NOP_SET_BACKGROUND: {
        uint8_t has = get_u8(r);
        if (has > 1) r->bad = true;
        o->has_image = has == 1;
        if (o->has_image) get_bytes(r, o->image, NET_HASH_SIZE);
        break;
    }
    case NOP_WAYPOINT_SET:
        get_str(r, o->name, sizeof(o->name));
        o->row = get_sv(r);
        o->col = get_sv(r);
        break;
    case NOP_WAYPOINT_DELETE:
        get_str(r, o->name, sizeof(o->name));
        break;
    case NOP_INIT_ADD:
        o->ids = get_ids(r, &o->nids);
        if (!r->bad && (size_t)o->nids * 8 > r->len - r->pos) r->bad = true;
        o->values = get_array(r, o->nids, sizeof(double));
        o->nvalues = o->values ? o->nids : 0;
        for (uint32_t i = 0; i < o->nvalues && !r->bad; i++) o->values[i] = get_f64(r);
        break;
    case NOP_INIT_NEXT:
    case NOP_CLAIM:
        o->id = get_uv(r);
        break;
    case NOP_SET_OWNER:
        o->ids = get_ids(r, &o->nids);
        get_str(r, o->name, sizeof(o->name));
        break;
    case NOP_ROLL:
        get_str(r, o->dice, sizeof(o->dice));
        break;
    case NOP_GRID_ORIENTATION:
    case NOP_INIT_CLEAR:
    case NOP_INIT_SHOW:
        break;
    default:
        r->bad = true;
        break;
    }
}

/* SNAPSHOT and IMAGE data: the rest of the message. */
static void get_blob(Rd *r, NetMsg *m) {
    if (r->bad) return;
    m->data = r->p + r->pos;
    m->data_len = r->len - r->pos;
    r->pos = r->len;
}

bool netmsg_decode(const uint8_t *buf, size_t len, NetMsg *out) {
    memset(out, 0, sizeof(*out));
    Rd r = { .p = buf, .len = len };
    out->type = get_u8(&r);
    switch (out->type) {
    case NM_HELLO:
        out->protocol = get_uv(&r);
        get_str(&r, out->name, sizeof(out->name));
        break;
    case NM_WELCOME:
        out->player = get_u8(&r);
        break;
    case NM_REJECT:
    case NM_CLOSE:
        out->reason = get_u8(&r);
        break;
    case NM_SNAPSHOT:
        get_blob(&r, out);
        break;
    case NM_SESSION: {
        out->entity_ids = get_ids(&r, &out->nentity_ids);
        uint32_t n = get_count(&r, NETMSG_MAX_PLAYERS, 4);
        out->players = get_array(&r, n, sizeof(NetPlayer));
        out->nplayers = out->players ? n : 0;
        for (uint32_t i = 0; i < out->nplayers && !r.bad; i++) get_player(&r, &out->players[i]);
        break;
    }
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT:
        get_player(&r, &out->info);
        break;
    case NM_OP:
        out->seq = get_uv(&r);
        get_op(&r, &out->op);
        break;
    case NM_OP_APPLIED:
        out->player = get_u8(&r);
        get_op(&r, &out->op);
        break;
    case NM_OP_REJECTED:
        out->seq = get_uv(&r);
        out->reason = get_u8(&r);
        break;
    case NM_IMAGE:
        get_bytes(&r, out->hash, NET_HASH_SIZE);
        out->image_kind = get_u8(&r);
        get_blob(&r, out);
        break;
    case NM_LOG:
        get_str(&r, out->text, sizeof(out->text));
        break;
    default:
        r.bad = true;
        break;
    }
    if (r.bad || r.pos != r.len || !msg_ok(out)) {
        netmsg_free(out);
        return false;
    }
    return true;
}

void netmsg_free(NetMsg *m) {
    free(m->op.ids);
    free(m->op.values);
    free(m->op.specs);
    free(m->entity_ids);
    free(m->players);
    memset(m, 0, sizeof(*m));
}

void netmsg_image_hash(const uint8_t *webp, size_t len, uint8_t out[NET_HASH_SIZE]) {
    crypto_generichash(out, NET_HASH_SIZE, webp, len, NULL, 0);
}

/* ---- join codes ----------------------------------------------------------------------------- */

static const char B32[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"; /* Crockford */
static const char JOIN_PREFIX[] = "toh1";

#define JOIN_MAX_PAYLOAD (1 + 2 + NET_SECRET_SIZE + 1 + NET_MAX_ADDRS * 17)

static int b32_value(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    const char *p = c ? strchr(B32, c) : NULL;
    return p ? (int)(p - B32) : -1;
}

static bool code_ok(const JoinCode *c) {
    if (c->port == 0 || c->naddrs < 1 || c->naddrs > NET_MAX_ADDRS) return false;
    for (int i = 0; i < c->naddrs; i++)
        if (c->addrs[i].family != 4 && c->addrs[i].family != 6) return false;
    return true;
}

bool joincode_encode(const JoinCode *c, char out[JOINCODE_MAX_LEN]) {
    out[0] = '\0';
    if (!code_ok(c)) return false;
    uint8_t p[JOIN_MAX_PAYLOAD];
    size_t n = 0;
    p[n++] = JOINCODE_VERSION;
    p[n++] = (uint8_t)c->port;
    p[n++] = (uint8_t)(c->port >> 8);
    memcpy(p + n, c->secret, NET_SECRET_SIZE);
    n += NET_SECRET_SIZE;
    p[n++] = c->naddrs;
    for (int i = 0; i < c->naddrs; i++) {
        size_t alen = c->addrs[i].family == 4 ? 4 : 16;
        p[n++] = c->addrs[i].family;
        memcpy(p + n, c->addrs[i].addr, alen);
        n += alen;
    }

    size_t o = 0;
    memcpy(out, JOIN_PREFIX, 4);
    o = 4;
    uint32_t acc = 0;
    int bits = 0, chars = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n) {
            acc = (acc << 8) | p[i];
            bits += 8;
        } else if (bits > 0) { /* pad the last group of bits with zeros */
            acc <<= 5 - bits;
            bits = 5;
        }
        while (bits >= 5) {
            if (chars++ % 4 == 0) out[o++] = '-'; /* "toh1-" and then groups of 4 */
            out[o++] = B32[(acc >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    out[o] = '\0';
    return true;
}

bool joincode_decode(const char *s, JoinCode *out) {
    memset(out, 0, sizeof(*out));
    if (!s || strnlen(s, 2 * JOINCODE_MAX_LEN) >= 2 * JOINCODE_MAX_LEN) return false;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != JOIN_PREFIX[i]) return false;
    }

    uint8_t p[JOIN_MAX_PAYLOAD];
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *q = s + 4; *q; q++) {
        if (*q == '-') continue;
        int v = b32_value(*q);
        if (v < 0) return false;
        acc = (acc << 5) | (uint32_t)v;
        bits += 5;
        if (bits >= 8) {
            if (n == sizeof(p)) return false;
            p[n++] = (uint8_t)(acc >> (bits - 8));
            bits -= 8;
        }
    }
    /* canonical: fewer than 5 padding bits, all zero */
    if (bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return false;

    Rd r = { .p = p, .len = n };
    if (get_u8(&r) != JOINCODE_VERSION) return false;
    out->port = get_u16(&r);
    get_bytes(&r, out->secret, NET_SECRET_SIZE);
    out->naddrs = get_u8(&r);
    if (r.bad || out->naddrs < 1 || out->naddrs > NET_MAX_ADDRS) return false;
    for (int i = 0; i < out->naddrs; i++) {
        out->addrs[i].family = get_u8(&r);
        if (out->addrs[i].family != 4 && out->addrs[i].family != 6) return false;
        get_bytes(&r, out->addrs[i].addr, out->addrs[i].family == 4 ? 4 : 16);
    }
    if (r.bad || r.pos != r.len || !code_ok(out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

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

static bool attrs_ok(const int32_t *a) {
    for (int i = 0; i < NETMSG_ATTRS; i++)
        if (a[i] < SAVEFMT_MIN_ATTR || a[i] > SAVEFMT_MAX_ATTR) return false;
    return true;
}

static bool spec_ok(const NetSpec *s) {
    if (s->type > SAVE_ENT_WALL || !coord_ok(s->row) || !coord_ok(s->col)) return false;
    if (s->name[0] && (s->type != SAVE_ENT_CHARACTER || !savefmt_char_name_valid(s->name))) return false;
    if (s->party[0] && (s->type != SAVE_ENT_CHARACTER || !savefmt_char_name_valid(s->party))) return false;
    if (s->type != SAVE_ENT_CHARACTER) return true;
    return s->speed <= SAVEFMT_MAX_SPEED && s->max_stamina >= 0 && s->max_stamina <= SAVEFMT_MAX_HP &&
           s->stamina >= -SAVEFMT_MAX_HP && s->stamina <= SAVEFMT_MAX_HP && attrs_ok(s->attr) && s->evasion >= 0 &&
           s->evasion <= SAVEFMT_MAX_EVASION;
}

static bool op_ok(const NetOp *o) {
    if (o->type >= NOP_COUNT || !ids_ok(o->ids, o->nids)) return false;
    if (o->nvalues > SAVEFMT_MAX_ENTITIES + NETMSG_ATTACK_ROLLS || o->nspecs > SAVEFMT_MAX_ENTITIES) return false;
    switch (o->type) {
    case NOP_MOVE:
    case NOP_MOVE_ACTION:
        return o->id != 0 && coord_ok(o->row) && coord_ok(o->col);
    case NOP_SPAWN:
        if (o->nspecs && !o->specs) return false;
        if (o->nids != 0 && o->nids != o->nspecs) return false;
        for (uint32_t i = 0; i < o->nspecs; i++)
            if (!spec_ok(&o->specs[i])) return false;
        return true;
    case NOP_SET_STATS:
        if (o->stat_mask == 0 || o->stat_mask > NSTAT_ALL) return false;
        if ((o->stat_mask & NSTAT_MAXSTAMINA) && (o->max_stamina < 0 || o->max_stamina > SAVEFMT_MAX_HP)) return false;
        if ((o->stat_mask & (NSTAT_STR | NSTAT_AGI | NSTAT_COG | NSTAT_SPI)) && !attrs_ok(o->attr)) return false;
        if ((o->stat_mask & NSTAT_EVASION) && (o->evasion < 0 || o->evasion > SAVEFMT_MAX_EVASION)) return false;
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
    case NOP_SET_HEALTH:
        return o->health_mode <= 1 && o->value >= -SAVEFMT_MAX_HP && o->value <= SAVEFMT_MAX_HP;
    case NOP_SET_STAMINA:
        return o->health_mode <= 2 && o->value >= -SAVEFMT_MAX_HP && o->value <= SAVEFMT_MAX_HP &&
               (o->health_mode != 2 || o->value >= 0);
    case NOP_SET_NAME:
        return savefmt_char_name_valid(o->char_name);
    case NOP_SET_PARTY:
        return o->party[0] == '\0' || savefmt_char_name_valid(o->party);
    case NOP_ROLL:
        if (o->nvalues > NETMSG_MAX_ROLLS || (o->nvalues && !o->values)) return false;
        for (uint32_t i = 0; i < o->nvalues; i++)
            if (!isfinite(o->values[i])) return false;
        return dice_ok(o->dice);
    case NOP_CLAIM:
        return o->id != 0;
    case NOP_TAKE_BACK:
        return o->id != 0 && coord_ok(o->row) && coord_ok(o->col) && o->action <= 7 &&
               o->value >= -SAVEFMT_MAX_HP && o->value <= SAVEFMT_MAX_HP;
    case NOP_INIT_NEXT:
        if (o->nvalues > NETMSG_MAX_RESULTS || (o->nvalues && !o->values)) return false;
        for (uint32_t i = 0; i < o->nvalues; i++)
            if (!isfinite(o->values[i])) return false;
        return true;
    case NOP_SET_TEST_MODE:
        return o->value == 0 || o->value == 1;
    case NOP_RECOVER_ACTION: /* ops_check checks the action and the roll's range */
        return o->id != 0 && o->value >= 0 && o->value <= NETMSG_MAX_DIE;
    case NOP_ATTACK_ACTION: /* ops_check checks the action, the range and the rolls against the attack's dice */
        if (o->id == 0 || !coord_ok(o->row) || !coord_ok(o->col)) return false;
        if (o->value < 0 || o->value >= NETMSG_ATTACK_DIRS) return false;
        if (o->nvalues == 0 ? o->nids != 0 : o->nvalues != NETMSG_ATTACK_ROLLS + o->nids) return false;
        if (o->nvalues && !o->values) return false;
        for (uint32_t i = 0; i < o->nvalues; i++) { /* the rolls, then the outcomes */
            double v = o->values[i], max = i < NETMSG_ATTACK_ROLLS ? NETMSG_MAX_DAMAGE : NETMSG_ATTACK_ROLLS - 1;
            if (!isfinite(v) || v != floor(v) || v < 0 || v > max) return false;
        }
        return true;
    case NOP_BLOCK_ACTION: /* ops_check checks the action and the range */
        return o->id != 0 && coord_ok(o->row) && coord_ok(o->col) && o->value >= 1 && o->value <= NETMSG_BLOCK_DIRS;
    case NOP_DODGE_ACTION: /* ops_check checks the action, the tile and the roll's range */
        return o->id != 0 && coord_ok(o->row) && coord_ok(o->col) && o->value >= 0 && o->value <= NETMSG_MAX_DIE;
    default: /* DELETE, INIT_REMOVE (ids only), SET_BACKGROUND, INIT_NEXT, GRID / CLEAR / SHOW */
        return true;
    }
}

static bool hexes_ok(const NetHex *h, uint32_t n, uint32_t max) {
    if (n > max || (n && !h)) return false;
    for (uint32_t i = 0; i < n; i++)
        if (!coord_ok(h[i].row) || !coord_ok(h[i].col)) return false;
    return true;
}

static bool world_ok(int32_t v) { return v >= -NETMSG_MAX_WORLD && v <= NETMSG_MAX_WORLD; }
static bool label_ok(int32_t v) { return v >= 0 && v <= NETMSG_MAX_LABEL; }

static bool circle_ok(const NetCircle *c) {
    return world_ok(c->x) && world_ok(c->y) && c->radius >= 0 && c->radius <= NETMSG_MAX_WORLD && label_ok(c->label);
}

static bool presence_ok(const NetPresence *p) {
    if (!hexes_ok(p->tiles, p->ntiles, NETMSG_MAX_SEL_TILES) || !hexes_ok(p->path, p->npath, NETMSG_MAX_PATH) ||
        !hexes_ok(p->footprint, p->nfootprint, NETMSG_MAX_FOOTPRINT))
        return false;
    if (p->nentities > NETMSG_MAX_SEL_ENTITIES || !ids_ok(p->entities, p->nentities)) return false;
    if (p->nlines > NETMSG_MAX_LINES || (p->nlines && !p->lines)) return false;
    for (uint32_t i = 0; i < p->nlines; i++) {
        const NetLine *l = &p->lines[i];
        if (!world_ok(l->ax) || !world_ok(l->ay) || !world_ok(l->bx) || !world_ok(l->by) || !label_ok(l->label))
            return false;
    }
    if (p->has_circle && !circle_ok(&p->circle)) return false;
    if (p->has_cone && (!circle_ok(&p->cone) || p->cone_start < 0 || p->cone_start > 359 || p->cone_extent < 0 ||
                        p->cone_extent > 360))
        return false;
    return true;
}

static bool player_ok(const NetPlayer *p) {
    return p->role <= NET_ROLE_PLAYER && p->color <= NET_COLOR_MAX && savefmt_owner_valid(p->name);
}

static bool blob_ok(const NetMsg *m) { return m->data && m->data_len >= 1 && m->data_len <= NETMSG_MAX_BLOB; }

static bool msg_ok(const NetMsg *m) {
    switch (m->type) {
    case NM_HELLO: /* no '"': console arguments drop quotes, so :kick / :ban could not name the player */
        return savefmt_owner_valid(m->name) && !strchr(m->name, '"');
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
    case NM_PRESENCE:      return presence_ok(&m->presence);
    case NM_CREATE_ANSWER: return true;
    case NM_CREATE:
        return m->spec.type == SAVE_ENT_CHARACTER && spec_ok(&m->spec) && m->spec.size >= SAVEFMT_MIN_SIZE && m->spec.size <= SAVEFMT_MAX_SIZE &&
               m->spec.health == m->spec.max_health && m->spec.stamina == m->spec.max_stamina && m->data && m->data_len >= 1 &&
               m->data_len <= NET_CREATE_MAX_BLOB;
    case NM_SESSION:
        if (!ids_ok(m->entity_ids, m->nentity_ids)) return false;
        if (m->nplayers < 1 || m->nplayers > NETMSG_MAX_PLAYERS || !m->players) return false;
        for (uint32_t i = 0; i < m->nplayers; i++)
            if (!player_ok(&m->players[i])) return false;
        return true;
    default:               return false; /* 0, unknown */
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

static void put_hexes(Buf *b, const NetHex *h, uint32_t n) {
    put_uv(b, n);
    for (uint32_t i = 0; i < n; i++) {
        put_sv(b, h[i].row);
        put_sv(b, h[i].col);
    }
}

static void put_circle(Buf *b, const NetCircle *c) {
    put_sv(b, c->x);
    put_sv(b, c->y);
    put_sv(b, c->radius);
    put_sv(b, c->label);
}

static void put_presence(Buf *b, const NetPresence *p) {
    put_hexes(b, p->tiles, p->ntiles);
    put_ids(b, p->entities, p->nentities);
    put_hexes(b, p->path, p->npath);
    put_hexes(b, p->footprint, p->nfootprint);
    put_uv(b, p->nlines);
    for (uint32_t i = 0; i < p->nlines; i++) {
        const NetLine *l = &p->lines[i];
        put_sv(b, l->ax);
        put_sv(b, l->ay);
        put_sv(b, l->bx);
        put_sv(b, l->by);
        put_sv(b, l->label);
    }
    put_u8(b, p->has_circle);
    if (p->has_circle) put_circle(b, &p->circle);
    put_u8(b, p->has_cone);
    if (p->has_cone) {
        put_circle(b, &p->cone);
        put_sv(b, p->cone_start);
        put_sv(b, p->cone_extent);
    }
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
                put_sv(b, s->speed);
                put_sv(b, s->initiative);
                put_str(b, s->name);
                put_sv(b, s->max_stamina);
                put_sv(b, s->stamina);
                put_str(b, s->party);
                for (int a = 0; a < NETMSG_ATTRS; a++) put_sv(b, s->attr[a]);
                put_sv(b, s->evasion);
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
        put_uv(b, o->stat_mask);
        const int32_t v[10] = { o->size, o->max_health, o->speed, o->initiative, o->max_stamina,
                                o->attr[0], o->attr[1], o->attr[2], o->attr[3], o->evasion };
        for (int i = 0; i < 10; i++)
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
        put_uv(b, o->id);
        put_uv(b, o->nvalues);
        for (uint32_t i = 0; i < o->nvalues; i++) put_f64(b, o->values[i]);
        break;
    case NOP_CLAIM:
        put_uv(b, o->id);
        break;
    case NOP_TAKE_BACK:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        put_u8(b, o->action);
        put_sv(b, o->value);
        break;
    case NOP_SET_OWNER:
        put_ids(b, o->ids, o->nids);
        put_str(b, o->name);
        break;
    case NOP_SET_HEALTH:
        put_ids(b, o->ids, o->nids);
        put_u8(b, o->health_mode);
        put_sv(b, o->value);
        break;
    case NOP_SET_STAMINA:
        put_ids(b, o->ids, o->nids);
        put_u8(b, o->health_mode);
        put_sv(b, o->value);
        break;
    case NOP_SET_NAME:
        put_ids(b, o->ids, o->nids);
        put_str(b, o->char_name);
        break;
    case NOP_SET_PARTY:
        put_ids(b, o->ids, o->nids);
        put_str(b, o->party);
        break;
    case NOP_MOVE_ACTION:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        put_u8(b, o->action);
        break;
    case NOP_SET_TEST_MODE:
        put_u8(b, (uint8_t)o->value);
        break;
    case NOP_RECOVER_ACTION:
        put_uv(b, o->id);
        put_u8(b, o->action);
        put_sv(b, o->value);
        break;
    case NOP_ATTACK_ACTION:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        put_u8(b, o->action);
        put_u8(b, (uint8_t)o->value);
        put_ids(b, o->ids, o->nids);
        put_uv(b, o->nvalues);
        for (uint32_t i = 0; i < o->nvalues; i++) put_f64(b, o->values[i]);
        break;
    case NOP_BLOCK_ACTION:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        put_u8(b, o->action);
        put_u8(b, (uint8_t)o->value);
        break;
    case NOP_DODGE_ACTION:
        put_uv(b, o->id);
        put_sv(b, o->row);
        put_sv(b, o->col);
        put_u8(b, o->action);
        put_sv(b, o->value);
        break;
    case NOP_ROLL:
        put_str(b, o->dice);
        put_uv(b, o->nvalues);
        for (uint32_t i = 0; i < o->nvalues; i++) put_f64(b, o->values[i]);
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
    case NM_PRESENCE:
        put_u8(&b, m->player);
        put_presence(&b, &m->presence);
        break;
    case NM_CREATE:
        put_uv(&b, m->seq);
        put_sv(&b, m->spec.row);
        put_sv(&b, m->spec.col);
        put_sv(&b, m->spec.size);
        put_sv(&b, m->spec.max_health);
        put_sv(&b, m->spec.speed);
        put_sv(&b, m->spec.initiative);
        put_str(&b, m->spec.name);
        put_sv(&b, m->spec.max_stamina);
        for (int a = 0; a < NETMSG_ATTRS; a++) put_sv(&b, m->spec.attr[a]);
        put_bytes(&b, m->data, m->data_len);
        break;
    case NM_CREATE_ANSWER:
        put_uv(&b, m->seq);
        put_u8(&b, m->accepted ? 1 : 0);
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

static NetHex *get_hexes(Rd *r, uint32_t max, uint32_t *n_out) {
    uint32_t n = get_count(r, max, 2);
    NetHex *h = get_array(r, n, sizeof(NetHex));
    for (uint32_t i = 0; i < n && h && !r->bad; i++) {
        h[i].row = get_sv(r);
        h[i].col = get_sv(r);
    }
    *n_out = h ? n : 0;
    return h;
}

static bool get_flag(Rd *r) {
    uint8_t f = get_u8(r);
    if (f > 1) r->bad = true;
    return f == 1;
}

static void get_circle(Rd *r, NetCircle *c) {
    c->x = get_sv(r);
    c->y = get_sv(r);
    c->radius = get_sv(r);
    c->label = get_sv(r);
}

static void get_presence(Rd *r, NetPresence *p) {
    p->tiles = get_hexes(r, NETMSG_MAX_SEL_TILES, &p->ntiles);
    p->entities = get_ids(r, &p->nentities);
    if (p->nentities > NETMSG_MAX_SEL_ENTITIES) r->bad = true;
    p->path = get_hexes(r, NETMSG_MAX_PATH, &p->npath);
    p->footprint = get_hexes(r, NETMSG_MAX_FOOTPRINT, &p->nfootprint);
    uint32_t n = get_count(r, NETMSG_MAX_LINES, 5);
    p->lines = get_array(r, n, sizeof(NetLine));
    p->nlines = p->lines ? n : 0;
    for (uint32_t i = 0; i < p->nlines && !r->bad; i++) {
        NetLine *l = &p->lines[i];
        l->ax = get_sv(r);
        l->ay = get_sv(r);
        l->bx = get_sv(r);
        l->by = get_sv(r);
        l->label = get_sv(r);
    }
    p->has_circle = get_flag(r);
    if (p->has_circle) get_circle(r, &p->circle);
    p->has_cone = get_flag(r);
    if (p->has_cone) {
        get_circle(r, &p->cone);
        p->cone_start = get_sv(r);
        p->cone_extent = get_sv(r);
    }
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
                s->speed = get_sv(r);
                s->initiative = get_sv(r);
                get_str(r, s->name, sizeof(s->name));
                s->max_stamina = get_sv(r);
                s->stamina = get_sv(r);
                get_str(r, s->party, sizeof(s->party));
                for (int a = 0; a < NETMSG_ATTRS; a++) s->attr[a] = get_sv(r);
                s->evasion = get_sv(r);
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
        uint32_t mask = get_uv(r);
        if (mask > 0xFFFF) r->bad = true;
        o->stat_mask = (uint16_t)mask;
        int32_t v[10] = { 0 };
        for (int i = 0; i < 10; i++)
            if (o->stat_mask & (1 << i)) v[i] = get_sv(r);
        o->size = v[0];
        o->max_health = v[1];
        o->speed = v[2];
        o->initiative = v[3];
        o->max_stamina = v[4];
        for (int a = 0; a < NETMSG_ATTRS; a++) o->attr[a] = v[5 + a];
        o->evasion = v[9];
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
    case NOP_INIT_NEXT: {
        o->id = get_uv(r);
        uint32_t n = get_count(r, NETMSG_MAX_RESULTS, 8);
        o->values = get_array(r, n, sizeof(double));
        o->nvalues = o->values ? n : 0;
        for (uint32_t i = 0; i < o->nvalues && !r->bad; i++) o->values[i] = get_f64(r);
        break;
    }
    case NOP_CLAIM:
        o->id = get_uv(r);
        break;
    case NOP_TAKE_BACK:
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        o->action = get_u8(r);
        o->value = get_sv(r);
        break;
    case NOP_SET_OWNER:
        o->ids = get_ids(r, &o->nids);
        get_str(r, o->name, sizeof(o->name));
        break;
    case NOP_SET_HEALTH:
        o->ids = get_ids(r, &o->nids);
        o->health_mode = get_u8(r);
        o->value = get_sv(r);
        break;
    case NOP_SET_STAMINA:
        o->ids = get_ids(r, &o->nids);
        o->health_mode = get_u8(r);
        o->value = get_sv(r);
        break;
    case NOP_SET_NAME:
        o->ids = get_ids(r, &o->nids);
        get_str(r, o->char_name, sizeof(o->char_name));
        break;
    case NOP_SET_PARTY:
        o->ids = get_ids(r, &o->nids);
        get_str(r, o->party, sizeof(o->party));
        break;
    case NOP_MOVE_ACTION:
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        o->action = get_u8(r);
        break;
    case NOP_SET_TEST_MODE:
        o->value = get_u8(r);
        break;
    case NOP_RECOVER_ACTION:
        o->id = get_uv(r);
        o->action = get_u8(r);
        o->value = get_sv(r);
        break;
    case NOP_ATTACK_ACTION: {
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        o->action = get_u8(r);
        o->value = get_u8(r);
        o->ids = get_ids(r, &o->nids);
        uint32_t n = get_count(r, NETMSG_ATTACK_ROLLS + SAVEFMT_MAX_ENTITIES, 8);
        o->values = get_array(r, n, sizeof(double));
        o->nvalues = o->values ? n : 0;
        for (uint32_t i = 0; i < o->nvalues && !r->bad; i++) o->values[i] = get_f64(r);
        break;
    }
    case NOP_BLOCK_ACTION:
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        o->action = get_u8(r);
        o->value = get_u8(r);
        break;
    case NOP_DODGE_ACTION:
        o->id = get_uv(r);
        o->row = get_sv(r);
        o->col = get_sv(r);
        o->action = get_u8(r);
        o->value = get_sv(r);
        break;
    case NOP_ROLL: {
        get_str(r, o->dice, sizeof(o->dice));
        uint32_t n = get_count(r, NETMSG_MAX_ROLLS, 8);
        o->values = get_array(r, n, sizeof(double));
        o->nvalues = o->values ? n : 0;
        for (uint32_t i = 0; i < o->nvalues && !r->bad; i++) o->values[i] = get_f64(r);
        break;
    }
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
    case NM_PRESENCE:
        out->player = get_u8(&r);
        get_presence(&r, &out->presence);
        break;
    case NM_CREATE:
        out->seq = get_uv(&r);
        out->spec.type = SAVE_ENT_CHARACTER;
        out->spec.row = get_sv(&r);
        out->spec.col = get_sv(&r);
        out->spec.size = get_sv(&r);
        out->spec.max_health = get_sv(&r);
        out->spec.speed = get_sv(&r);
        out->spec.initiative = get_sv(&r);
        get_str(&r, out->spec.name, sizeof(out->spec.name));
        out->spec.max_stamina = get_sv(&r);
        for (int a = 0; a < NETMSG_ATTRS; a++) out->spec.attr[a] = get_sv(&r);
        out->spec.stamina = out->spec.max_stamina;
        out->spec.health = out->spec.max_health;
        get_blob(&r, out);
        break;
    case NM_CREATE_ANSWER: {
        out->seq = get_uv(&r);
        uint8_t a = get_u8(&r);
        if (a > 1) r.bad = true;
        out->accepted = a == 1;
        break;
    }
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

void netpresence_free(NetPresence *p) {
    free(p->tiles);
    free(p->entities);
    free(p->path);
    free(p->footprint);
    free(p->lines);
    memset(p, 0, sizeof(*p));
}

static void *dup_array(const void *src, uint32_t n, size_t size) {
    if (n == 0 || !src) return NULL;
    void *p = malloc((size_t)n * size);
    if (!p) abort();
    memcpy(p, src, (size_t)n * size);
    return p;
}

void netpresence_copy(NetPresence *dst, const NetPresence *src) {
    *dst = *src;
    dst->tiles = dup_array(src->tiles, src->ntiles, sizeof(NetHex));
    dst->entities = dup_array(src->entities, src->nentities, sizeof(uint32_t));
    dst->path = dup_array(src->path, src->npath, sizeof(NetHex));
    dst->footprint = dup_array(src->footprint, src->nfootprint, sizeof(NetHex));
    dst->lines = dup_array(src->lines, src->nlines, sizeof(NetLine));
}

void netmsg_free(NetMsg *m) {
    netpresence_free(&m->presence);
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
    if (c->relay) return c->port == 0 && c->naddrs == 0;
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
    if (c->relay) {
        p[n++] = JOINCODE_VERSION_RELAY;
        memcpy(p + n, c->secret, NET_SECRET_SIZE);
        n += NET_SECRET_SIZE;
    } else {
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
    uint8_t version = get_u8(&r);
    if (version == JOINCODE_VERSION_RELAY) {
        out->relay = true;
        get_bytes(&r, out->secret, NET_SECRET_SIZE);
    } else if (version == JOINCODE_VERSION) {
        out->port = get_u16(&r);
        get_bytes(&r, out->secret, NET_SECRET_SIZE);
        out->naddrs = get_u8(&r);
        if (r.bad || out->naddrs < 1 || out->naddrs > NET_MAX_ADDRS) return false;
        for (int i = 0; i < out->naddrs; i++) {
            out->addrs[i].family = get_u8(&r);
            if (out->addrs[i].family != 4 && out->addrs[i].family != 6) return false;
            get_bytes(&r, out->addrs[i].addr, out->addrs[i].family == 4 ? 4 : 16);
        }
    } else {
        return false;
    }
    if (r.bad || r.pos != r.len || !code_ok(out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

void joincode_room_id(const uint8_t secret[NET_SECRET_SIZE], uint8_t room[NET_ROOM_SIZE]) {
    crypto_generichash(room, NET_ROOM_SIZE, (const uint8_t *)"toh-room", 8, secret, NET_SECRET_SIZE);
}

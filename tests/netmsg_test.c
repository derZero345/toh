/* Unit tests for src/netmsg.c. Run with `make test`.
 * `build/netmsg_test <dir>` additionally writes sample inputs for the fuzzer into dir. */
#include "netmsg.h"

#include <math.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond) do {                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                      \
    } while (0)

/* ---- sample messages ------------------------------------------------------------------- */

static uint32_t ids3[3] = { 7, 300, 70000 };
static double values3[3] = { 0.5, -2.25, 19.0 };
static double rolls3[3] = { 4, 1, 0 }; /* NOP_ATTACK_ACTION: success, scrape, miss */
static uint32_t entity_ids[4] = { 1, 2, 5, 1u << 31 };
static NetPlayer players[2] = {
    { .id = 0, .role = NET_ROLE_GM, .color = 0xE0B000, .name = "GM" },
    { .id = 3, .role = NET_ROLE_PLAYER, .color = 0x3A7BFF, .name = "Ålice" },
};
static const uint8_t blob[5] = { 'R', 'I', 'F', 'F', 0 };
static NetSpec specs2[2];
static NetHex sel_tiles[3] = { { 1, 2 }, { -3, 4 }, { 1000000, -1000000 } };
static uint32_t sel_ids[2] = { 5, 70000 };
static NetHex path3[3] = { { 0, 0 }, { 0, 1 }, { 1, 1 } };
static NetHex foot1[1] = { { 1, 1 } };
static NetLine lines2[2] = { { 0, 0, 512, 256, 10 }, { -256, 300, 1000, -4000, 35 } };

/* Presence with every part set. */
static NetPresence sample_presence(void) {
    return (NetPresence){ .tiles = sel_tiles, .ntiles = 3, .entities = sel_ids, .nentities = 2,
                          .path = path3, .npath = 3, .footprint = foot1, .nfootprint = 1,
                          .lines = lines2, .nlines = 2,
                          .has_circle = true, .circle = { .x = 256, .y = -512, .radius = 700, .label = 15 },
                          .has_cone = true, .cone = { .x = 10, .y = 20, .radius = 900, .label = 25 },
                          .cone_start = 350, .cone_extent = 90 };
}

static void fill_hash(uint8_t h[NET_HASH_SIZE], uint8_t seed) {
    for (int i = 0; i < NET_HASH_SIZE; i++) h[i] = (uint8_t)(seed + i * 7);
}

/* One op of every type, with every field it uses set to a non-default value. */
static NetOp sample_op(int type) {
    NetOp o = { .type = (uint8_t)type };
    switch (type) {
    case NOP_MOVE:
        o.id = 42; o.row = -5; o.col = 1000000;
        break;
    case NOP_SPAWN:
        specs2[0] = (NetSpec){ .type = SAVE_ENT_CHARACTER, .row = 3, .col = -4, .size = 1, .max_health = 30,
                               .health = 12, .attack = 5, .speed = 6, .initiative = -1,
                               .attr = { 1, 2, 0, -1 } };
        fill_hash(specs2[0].image, 1);
        strcpy(specs2[0].name, "Mao");
        strcpy(specs2[0].party, "Silver Hand");
        specs2[0].max_stamina = 9; specs2[0].stamina = 3;
        specs2[1] = (NetSpec){ .type = SAVE_ENT_WALL, .row = -1000000, .col = 9 };
        fill_hash(specs2[1].image, 2);
        o.specs = specs2; o.nspecs = 2;
        o.ids = ids3; o.nids = 2; /* as broadcast by the host */
        break;
    case NOP_DELETE:
    case NOP_INIT_REMOVE:
        o.ids = ids3; o.nids = 3;
        break;
    case NOP_SET_STATS:
        o.ids = ids3; o.nids = 1;
        o.stat_mask = 1 | 4 | 8 | 16 | NSTAT_STR | NSTAT_SPI; o.max_stamina = 7; o.size = -2; o.speed = 21; o.initiative = 4;
        o.attr[0] = 3; o.attr[3] = -2;
        break;
    case NOP_SET_ICON:
        o.ids = ids3; o.nids = 2;
        o.has_image = true; fill_hash(o.image, 9);
        break;
    case NOP_SET_BACKGROUND:
        o.has_image = true; fill_hash(o.image, 11);
        break;
    case NOP_WAYPOINT_SET:
        strcpy(o.name, "camp fire"); o.row = 12; o.col = -13;
        break;
    case NOP_WAYPOINT_DELETE:
        strcpy(o.name, "camp fire");
        break;
    case NOP_INIT_ADD:
        o.ids = ids3; o.nids = 3; o.values = values3; o.nvalues = 3;
        break;
    case NOP_INIT_NEXT:
        o.id = 300;
        break;
    case NOP_SET_OWNER:
        o.ids = ids3; o.nids = 2; strcpy(o.name, "Ålice");
        break;
    case NOP_SET_HEALTH:
        o.ids = ids3; o.nids = 2;
        o.health_mode = 1; o.value = -7;
        break;
    case NOP_SET_STAMINA:
        o.ids = ids3; o.nids = 1;
        o.health_mode = 2; o.value = 4;
        break;
    case NOP_SET_NAME:
        o.ids = ids3; o.nids = 1;
        strcpy(o.char_name, "Mao Clausewitz");
        break;
    case NOP_SET_PARTY:
        o.ids = ids3; o.nids = 2;
        strcpy(o.party, "Heroes");
        break;
    case NOP_MOVE_ACTION:
        o.id = 7; o.row = 3; o.col = -2; o.action = 3;
        break;
    case NOP_SET_TEST_MODE:
        o.value = 1;
        break;
    case NOP_RECOVER_ACTION:
        o.id = 9; o.action = 1; o.value = 4;
        break;
    case NOP_ATTACK_ACTION: /* as broadcast by the host: a damage roll per outcome */
        o.id = 11; o.row = -4; o.col = 6; o.action = 0; o.value = 2;
        o.values = rolls3; o.nvalues = 3;
        break;
    case NOP_ROLL:
        strcpy(o.dice, "2d6+3");
        o.values = values3; o.nvalues = 3; /* as broadcast by the host: dice, then the total */
        break;
    case NOP_CLAIM:
        o.id = 70000;
        break;
    default: /* NOP_GRID_ORIENTATION, NOP_INIT_CLEAR, NOP_INIT_SHOW: no body */
        break;
    }
    return o;
}

/* A message of type t (op messages use op type op_type). */
static NetMsg sample_msg(int t, int op_type) {
    NetMsg m = { .type = (uint8_t)t };
    switch (t) {
    case NM_HELLO: m.protocol = NET_PROTOCOL_VERSION; strcpy(m.name, "Bob"); break;
    case NM_WELCOME: m.player = 3; break;
    case NM_REJECT: m.reason = NET_REJECT_NAME_TAKEN; break;
    case NM_SNAPSHOT: m.data = blob; m.data_len = sizeof(blob); break;
    case NM_SESSION: m.entity_ids = entity_ids; m.nentity_ids = 4; m.players = players; m.nplayers = 2; break;
    case NM_PLAYER_JOINED:
    case NM_PLAYER_LEFT: m.info = players[1]; break;
    case NM_OP: m.seq = 99; m.op = sample_op(op_type); break;
    case NM_OP_APPLIED: m.player = 3; m.op = sample_op(op_type); break;
    case NM_OP_REJECTED: m.seq = 99; m.reason = 2; break;
    case NM_IMAGE: fill_hash(m.hash, 5); m.image_kind = SAVE_IMG_LOSSY; m.data = blob; m.data_len = 4; break;
    case NM_LOG: strcpy(m.text, "Alice rolled 2d6+3: 4 + 2 + 3 = 9"); break;
    case NM_CLOSE: m.reason = NET_CLOSE_KICKED; break;
    case NM_PRESENCE: m.player = 3; m.presence = sample_presence(); break;
    case NM_CREATE_ANSWER: m.seq = 7; m.accepted = true; break;
    case NM_CREATE:
        m.seq = 7;
        m.spec = (NetSpec){ .type = SAVE_ENT_CHARACTER, .row = 3, .col = -4, .size = -2, .max_health = 20,
                            .health = 20, .speed = 6, .initiative = 1, .max_stamina = 8, .stamina = 8 };
        strcpy(m.spec.name, "Nina");
        m.spec.attr[0] = 1; m.spec.attr[1] = 2; m.spec.attr[3] = -1;
        m.data = blob; m.data_len = sizeof(blob);
        break;
    }
    return m;
}

/* ---- helpers ---------------------------------------------------------------------------- */

static bool encode(const NetMsg *m, uint8_t **buf, size_t *len) { return netmsg_encode(m, buf, len); }

/* Encodes m, decodes it and encodes the result again: both encodings must be equal. */
static bool round_trip(const NetMsg *m, NetMsg *decoded) {
    uint8_t *a = NULL, *b = NULL;
    size_t alen = 0, blen = 0;
    bool ok = encode(m, &a, &alen) && netmsg_decode(a, alen, decoded) && encode(decoded, &b, &blen) &&
              alen == blen && memcmp(a, b, alen) == 0;
    free(a);
    free(b);
    return ok;
}

/* Encodes m, lets edit change the bytes and returns whether the result still decodes. */
static bool decodes_after(const NetMsg *m, void (*edit)(uint8_t **buf, size_t *len)) {
    uint8_t *buf;
    size_t len;
    if (!encode(m, &buf, &len)) return false;
    edit(&buf, &len);
    NetMsg d;
    bool ok = netmsg_decode(buf, len, &d);
    netmsg_free(&d);
    free(buf);
    return ok;
}

static void append_zero(uint8_t **buf, size_t *len) {
    *buf = realloc(*buf, *len + 1);
    (*buf)[(*len)++] = 0;
}

static void drop_last(uint8_t **buf, size_t *len) {
    (void)buf;
    (*len)--;
}

/* ---- message tests ---------------------------------------------------------------------- */

static void test_every_message_round_trips(void) {
    for (int t = NM_HELLO; t < NM_COUNT; t++) {
        if (t == NM_OP || t == NM_OP_APPLIED) continue;
        NetMsg m = sample_msg(t, 0), d;
        CHECK(round_trip(&m, &d));
        CHECK(d.type == t);
        netmsg_free(&d);
    }
    for (int t = NM_OP; t <= NM_OP_APPLIED; t++)
        for (int o = 0; o < NOP_COUNT; o++) {
            NetMsg m = sample_msg(t, o), d;
            bool ok = round_trip(&m, &d);
            if (!ok) fprintf(stderr, "op message %d with op %d does not round-trip\n", t, o);
            CHECK(ok);
            CHECK(d.op.type == o);
            netmsg_free(&d);
        }
}

static void test_fields_survive(void) {
    NetMsg m = sample_msg(NM_HELLO, 0), d;
    CHECK(round_trip(&m, &d) && d.protocol == NET_PROTOCOL_VERSION && strcmp(d.name, "Bob") == 0);
    netmsg_free(&d);

    m = sample_msg(NM_SESSION, 0);
    CHECK(round_trip(&m, &d) && d.nentity_ids == 4 && d.entity_ids[3] == 1u << 31 && d.nplayers == 2);
    CHECK(d.players[1].id == 3 && d.players[1].role == NET_ROLE_PLAYER && d.players[1].color == 0x3A7BFF &&
          strcmp(d.players[1].name, "Ålice") == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP_APPLIED, NOP_SPAWN);
    CHECK(round_trip(&m, &d) && d.player == 3 && d.op.nspecs == 2 && d.op.nids == 2 && d.op.ids[1] == 300);
    CHECK(d.op.specs[0].type == SAVE_ENT_CHARACTER && d.op.specs[0].col == -4 && d.op.specs[0].initiative == -1 &&
          d.op.specs[0].health == 12 && memcmp(d.op.specs[0].image, specs2[0].image, NET_HASH_SIZE) == 0);
    CHECK(d.op.specs[1].type == SAVE_ENT_WALL && d.op.specs[1].row == -1000000 && d.op.specs[1].speed == 0);
    CHECK(strcmp(d.op.specs[0].name, "Mao") == 0 && d.op.specs[1].name[0] == '\0');
    CHECK(strcmp(d.op.specs[0].party, "Silver Hand") == 0 && d.op.specs[1].party[0] == '\0');
    CHECK(d.op.specs[0].max_stamina == 9 && d.op.specs[0].stamina == 3);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_SET_HEALTH);
    CHECK(round_trip(&m, &d) && d.op.nids == 2 && d.op.health_mode == 1 && d.op.value == -7);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_SET_STAMINA);
    CHECK(round_trip(&m, &d) && d.op.nids == 1 && d.op.health_mode == 2 && d.op.value == 4);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_SET_NAME);
    CHECK(round_trip(&m, &d) && d.op.nids == 1 && strcmp(d.op.char_name, "Mao Clausewitz") == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_SET_PARTY);
    CHECK(round_trip(&m, &d) && d.op.nids == 2 && strcmp(d.op.party, "Heroes") == 0);
    netmsg_free(&d);
    m = sample_msg(NM_OP, NOP_SET_PARTY); m.op.party[0] = '\0';
    CHECK(round_trip(&m, &d) && d.op.party[0] == '\0'); /* leaving the party */
    netmsg_free(&d);
    m = sample_msg(NM_OP, NOP_MOVE_ACTION);
    CHECK(round_trip(&m, &d) && d.op.id == 7 && d.op.row == 3 && d.op.col == -2 && d.op.action == 3);
    netmsg_free(&d);
    m = sample_msg(NM_OP_APPLIED, NOP_SET_TEST_MODE);
    CHECK(round_trip(&m, &d) && d.op.type == NOP_SET_TEST_MODE && d.op.value == 1);
    netmsg_free(&d);
    m = sample_msg(NM_OP, NOP_SET_TEST_MODE); m.op.value = 0;
    CHECK(round_trip(&m, &d) && d.op.value == 0);
    netmsg_free(&d);
    m = sample_msg(NM_OP_APPLIED, NOP_RECOVER_ACTION);
    CHECK(round_trip(&m, &d) && d.op.id == 9 && d.op.action == 1 && d.op.value == 4);
    netmsg_free(&d);
    m = sample_msg(NM_OP_APPLIED, NOP_ATTACK_ACTION);
    CHECK(round_trip(&m, &d) && d.op.id == 11 && d.op.row == -4 && d.op.col == 6 && d.op.action == 0 &&
          d.op.value == 2 && d.op.nvalues == 3 && d.op.values[0] == 4 && d.op.values[1] == 1 && d.op.values[2] == 0);
    netmsg_free(&d);
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); m.op.nvalues = 0; /* a client sends no rolls */
    CHECK(round_trip(&m, &d) && d.op.nvalues == 0 && d.op.value == 2);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_SET_STATS);
    CHECK(round_trip(&m, &d) && d.seq == 99 && d.op.stat_mask == (29 | NSTAT_STR | NSTAT_SPI) && d.op.attr[0] == 3 && d.op.attr[3] == -2 && d.op.attr[1] == 0 &&
          d.op.max_stamina == 7 && d.op.size == -2 && d.op.speed == 21 &&
          d.op.initiative == 4 && d.op.max_health == 0);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_INIT_ADD);
    CHECK(round_trip(&m, &d) && d.op.nvalues == 3 && d.op.values[1] == -2.25);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_MOVE);
    CHECK(round_trip(&m, &d) && d.op.id == 42 && d.op.row == -5 && d.op.col == 1000000);
    netmsg_free(&d);

    /* decoded data points into the decoded buffer, so keep it until the checks are done */
    m = sample_msg(NM_IMAGE, 0);
    uint8_t *buf;
    size_t len;
    CHECK(encode(&m, &buf, &len) && netmsg_decode(buf, len, &d));
    CHECK(d.image_kind == SAVE_IMG_LOSSY && d.data_len == 4 && d.data == buf + len - 4 &&
          memcmp(d.data, blob, 4) == 0 && memcmp(d.hash, m.hash, NET_HASH_SIZE) == 0);
    netmsg_free(&d);
    free(buf);

    m = sample_msg(NM_LOG, 0);
    CHECK(round_trip(&m, &d) && strcmp(d.text, m.text) == 0);
    netmsg_free(&d);

    m = sample_msg(NM_PRESENCE, 0);
    CHECK(round_trip(&m, &d) && d.player == 3 && d.presence.ntiles == 3 && d.presence.tiles[2].col == -1000000);
    CHECK(d.presence.nentities == 2 && d.presence.entities[1] == 70000 && d.presence.npath == 3 &&
          d.presence.path[2].row == 1 && d.presence.nfootprint == 1 && d.presence.nlines == 2 &&
          d.presence.lines[1].by == -4000 && d.presence.lines[1].label == 35);
    CHECK(d.presence.has_circle && d.presence.circle.y == -512 && d.presence.circle.radius == 700 &&
          d.presence.has_cone && d.presence.cone.radius == 900 && d.presence.cone_start == 350 &&
          d.presence.cone_extent == 90 && d.presence.cone.label == 25);
    /* netpresence_copy makes an independent deep copy */
    NetPresence copy;
    netpresence_copy(&copy, &d.presence);
    netmsg_free(&d);
    CHECK(copy.ntiles == 3 && copy.tiles[1].row == -3 && copy.lines[0].bx == 512 && copy.has_cone);
    netpresence_free(&copy);
    CHECK(copy.tiles == NULL && copy.ntiles == 0);
    /* an empty presence (nothing selected) is valid */
    m = (NetMsg){ .type = NM_PRESENCE };
    CHECK(round_trip(&m, &d) && d.presence.ntiles == 0 && !d.presence.has_circle);
    netmsg_free(&d);

    m = sample_msg(NM_OP, NOP_ROLL);
    CHECK(round_trip(&m, &d) && strcmp(d.op.dice, "2d6+3") == 0 && d.op.nvalues == 3 && d.op.values[2] == 19.0);
    netmsg_free(&d);
    m.op.nvalues = 0; /* a roll from a client has no result yet */
    CHECK(round_trip(&m, &d) && d.op.nvalues == 0);
    netmsg_free(&d);

    /* a spawn from a client carries no ids */
    m = sample_msg(NM_OP, NOP_SPAWN);
    m.op.nids = 0;
    CHECK(round_trip(&m, &d) && d.op.nids == 0 && d.op.ids == NULL);
    netmsg_free(&d);

    /* a background op without an image clears the background */
    m = sample_msg(NM_OP, NOP_SET_BACKGROUND);
    m.op.has_image = false;
    memset(m.op.image, 0, NET_HASH_SIZE);
    CHECK(round_trip(&m, &d) && !d.op.has_image);
    netmsg_free(&d);
}

static void test_truncated_and_trailing(void) {
    for (int t = NM_HELLO; t < NM_COUNT; t++) {
        for (int o = 0; o < ((t == NM_OP || t == NM_OP_APPLIED) ? NOP_COUNT : 1); o++) {
            NetMsg m = sample_msg(t, o);
            /* SNAPSHOT, IMAGE and CREATE data run to the end of the message, so extra bytes are data */
            bool blob_end = t == NM_SNAPSHOT || t == NM_IMAGE || t == NM_CREATE;
            if (!blob_end) CHECK(!decodes_after(&m, append_zero));
            /* every message without its last byte is invalid, except a longer-than-1-byte blob */
            if (!blob_end) CHECK(!decodes_after(&m, drop_last));

            uint8_t *buf;
            size_t len;
            CHECK(encode(&m, &buf, &len));
            for (size_t cut = 0; cut + 1 < len; cut++) {
                NetMsg d;
                bool ok = netmsg_decode(buf, cut, &d);
                netmsg_free(&d);
                if (ok && !blob_end) {
                    fprintf(stderr, "message %d (op %d) cut to %zu bytes decodes\n", t, o, cut);
                    CHECK(!ok);
                }
            }
            free(buf);
        }
    }
}

/* Each case breaks one rule; both the encoder and the decoder must refuse it. */
static void check_invalid(NetMsg m, const char *what) {
    uint8_t *buf = NULL;
    size_t len;
    bool encoded = netmsg_encode(&m, &buf, &len);
    free(buf);
    if (encoded) fprintf(stderr, "encoder accepted: %s\n", what);
    CHECK(!encoded);
}

static void test_encoder_refuses_invalid(void) {
    NetMsg m;
    m = sample_msg(NM_HELLO, 0); strcpy(m.name, "two words"); check_invalid(m, "name with a space");
    m = sample_msg(NM_HELLO, 0); m.name[0] = '\0'; check_invalid(m, "empty name");
    m = sample_msg(NM_REJECT, 0); m.reason = 0; check_invalid(m, "reject reason 0");
    m = sample_msg(NM_REJECT, 0); m.reason = NET_REJECT_MAX + 1; check_invalid(m, "reject reason too big");
    m = sample_msg(NM_CLOSE, 0); m.reason = NET_CLOSE_MAX + 1; check_invalid(m, "close reason too big");
    m = sample_msg(NM_OP_REJECTED, 0); m.reason = NETMSG_MAX_REASON + 1; check_invalid(m, "op reason too big");
    m = sample_msg(NM_SNAPSHOT, 0); m.data_len = 0; check_invalid(m, "empty snapshot");
    m = sample_msg(NM_SNAPSHOT, 0); m.data_len = NETMSG_MAX_BLOB + 1; check_invalid(m, "snapshot too large");
    m = sample_msg(NM_IMAGE, 0); m.image_kind = 2; check_invalid(m, "unknown image kind");
    m = sample_msg(NM_PLAYER_JOINED, 0); m.info.color = 0x1000000; check_invalid(m, "color too big");
    m = sample_msg(NM_PLAYER_JOINED, 0); m.info.role = 2; check_invalid(m, "unknown role");
    m = sample_msg(NM_SESSION, 0); m.nplayers = NETMSG_MAX_PLAYERS + 1; check_invalid(m, "too many players");
    m = sample_msg(NM_SESSION, 0); entity_ids[0] = 0; check_invalid(m, "entity id 0"); entity_ids[0] = 1;
    m = sample_msg(NM_PRESENCE, 0); m.presence.ntiles = NETMSG_MAX_SEL_TILES + 1; check_invalid(m, "too many tiles");
    m = sample_msg(NM_PRESENCE, 0); m.presence.nentities = NETMSG_MAX_SEL_ENTITIES + 1; check_invalid(m, "too many entities");
    m = sample_msg(NM_PRESENCE, 0); m.presence.nfootprint = NETMSG_MAX_FOOTPRINT + 1; check_invalid(m, "footprint too large");
    m = sample_msg(NM_PRESENCE, 0); m.presence.nlines = NETMSG_MAX_LINES + 1; check_invalid(m, "too many lines");
    m = sample_msg(NM_PRESENCE, 0); sel_ids[0] = 0; check_invalid(m, "entity id 0"); sel_ids[0] = 5;
    m = sample_msg(NM_PRESENCE, 0); sel_tiles[0].row = SAVEFMT_MAX_COORD + 1; check_invalid(m, "tile out of range"); sel_tiles[0].row = 1;
    m = sample_msg(NM_PRESENCE, 0); lines2[0].ax = NETMSG_MAX_WORLD + 1; check_invalid(m, "line out of range"); lines2[0].ax = 0;
    m = sample_msg(NM_PRESENCE, 0); lines2[1].label = -1; check_invalid(m, "negative label"); lines2[1].label = 35;
    m = sample_msg(NM_PRESENCE, 0); m.presence.circle.radius = -1; check_invalid(m, "negative radius");
    m = sample_msg(NM_PRESENCE, 0); m.presence.cone_start = 360; check_invalid(m, "cone start 360");
    m = sample_msg(NM_PRESENCE, 0); m.presence.cone_extent = 361; check_invalid(m, "cone wider than a circle");
    m = sample_msg(0, 0); check_invalid(m, "type 0");
    m = sample_msg(NM_COUNT, 0); check_invalid(m, "type NM_COUNT");
    m = sample_msg(NM_OP, NOP_COUNT); check_invalid(m, "op type NOP_COUNT");
    m = sample_msg(NM_OP, NOP_MOVE); m.op.row = SAVEFMT_MAX_COORD + 1; check_invalid(m, "row out of range");
    m = sample_msg(NM_OP, NOP_MOVE); m.op.id = 0; check_invalid(m, "move id 0");
    m = sample_msg(NM_OP, NOP_CLAIM); m.op.id = 0; check_invalid(m, "claim id 0");
    m = sample_msg(NM_OP, NOP_DELETE); ids3[1] = 0; check_invalid(m, "id 0 in list"); ids3[1] = 300;
    m = sample_msg(NM_OP, NOP_DELETE); m.op.nids = SAVEFMT_MAX_ENTITIES + 1; check_invalid(m, "too many ids");
    m = sample_msg(NM_OP, NOP_SPAWN); m.op.nids = 1; check_invalid(m, "spawn ids != specs");
    m = sample_msg(NM_OP, NOP_SPAWN); specs2[0].speed = SAVEFMT_MAX_SPEED + 1; check_invalid(m, "speed too big");
    m = sample_msg(NM_OP, NOP_SPAWN); specs2[1].type = 2; check_invalid(m, "unknown entity type");
    m = sample_msg(NM_OP, NOP_SET_HEALTH); m.op.health_mode = 2; check_invalid(m, "health mode 2");
    m = sample_msg(NM_OP, NOP_SET_HEALTH); m.op.value = SAVEFMT_MAX_HP + 1; check_invalid(m, "health value too big");
    m = sample_msg(NM_OP, NOP_SET_STAMINA); m.op.health_mode = 3; check_invalid(m, "stamina mode 3");
    m = sample_msg(NM_OP, NOP_SET_STAMINA); m.op.value = -1; check_invalid(m, "negative stamina spend");
    m = sample_msg(NM_OP, NOP_SET_NAME); m.op.char_name[0] = '\0'; check_invalid(m, "empty character name");
    m = sample_msg(NM_OP, NOP_SET_PARTY); strcpy(m.op.party, "tab\there"); check_invalid(m, "party with a control character");
    m = sample_msg(NM_OP, NOP_MOVE_ACTION); m.op.id = 0; check_invalid(m, "move action without a character");
    m = sample_msg(NM_OP, NOP_SET_TEST_MODE); m.op.value = 2; check_invalid(m, "test mode neither on nor off");
    m = sample_msg(NM_OP, NOP_RECOVER_ACTION); m.op.id = 0; check_invalid(m, "recover action without a character");
    m = sample_msg(NM_OP, NOP_RECOVER_ACTION); m.op.value = -1; check_invalid(m, "negative recovery roll");
    m = sample_msg(NM_OP, NOP_RECOVER_ACTION); m.op.value = NETMSG_MAX_DIE + 1; check_invalid(m, "recovery roll too big");
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); m.op.id = 0; check_invalid(m, "attack without a character");
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); m.op.value = NETMSG_ATTACK_DIRS; check_invalid(m, "attack direction past the end");
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); m.op.nvalues = 2; check_invalid(m, "two damage rolls");
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); rolls3[1] = -1; check_invalid(m, "negative damage"); rolls3[1] = 1;
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); rolls3[1] = 1.5; check_invalid(m, "fractional damage"); rolls3[1] = 1;
    m = sample_msg(NM_OP, NOP_ATTACK_ACTION); rolls3[0] = NETMSG_MAX_DAMAGE + 1; check_invalid(m, "damage too big"); rolls3[0] = 4;
    m = sample_msg(NM_OP, NOP_SPAWN); strcpy(specs2[1].party, "Heroes"); check_invalid(m, "wall with a party"); specs2[1].party[0] = '\0';
    m = sample_msg(NM_OP, NOP_SPAWN); strcpy(specs2[1].name, "wall"); check_invalid(m, "wall with a name"); specs2[1].name[0] = '\0';
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.stat_mask = 0; check_invalid(m, "empty stat mask");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.stat_mask = NSTAT_ALL + 1; check_invalid(m, "unknown stat bit");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.max_stamina = SAVEFMT_MAX_HP + 1; check_invalid(m, "max stamina too big");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.max_stamina = -1; check_invalid(m, "negative max stamina");
    m = sample_msg(NM_OP, NOP_SET_STATS); m.op.stat_mask = NSTAT_SPEED; m.op.speed = SAVEFMT_MAX_SPEED + 1; check_invalid(m, "stat speed too big");
    m = sample_msg(NM_OP, NOP_SET_ICON); m.op.has_image = false; check_invalid(m, "icon without image");
    m = sample_msg(NM_OP, NOP_INIT_ADD); m.op.nvalues = 2; check_invalid(m, "values != ids");
    m = sample_msg(NM_OP, NOP_INIT_ADD); values3[2] = NAN; check_invalid(m, "NaN initiative"); values3[2] = 19.0;
    m = sample_msg(NM_OP, NOP_INIT_ADD); values3[2] = INFINITY; check_invalid(m, "infinite initiative"); values3[2] = 19.0;
    m = sample_msg(NM_OP, NOP_WAYPOINT_SET); m.op.name[0] = '\0'; check_invalid(m, "empty waypoint name");
    m = sample_msg(NM_OP, NOP_SET_OWNER); strcpy(m.op.name, "a\tb"); check_invalid(m, "owner with a tab");
    m = sample_msg(NM_OP, NOP_ROLL); strcpy(m.op.dice, ""); check_invalid(m, "empty dice spec");
    m = sample_msg(NM_OP, NOP_ROLL); m.op.nvalues = NETMSG_MAX_ROLLS + 1; check_invalid(m, "too many dice results");
    m = sample_msg(NM_OP, NOP_ROLL); values3[0] = NAN; check_invalid(m, "NaN die"); values3[0] = 0.5;
    m = sample_msg(NM_OP, NOP_ROLL); strcpy(m.op.dice, "2d6; rm"); check_invalid(m, "dice spec with other characters");
    m = sample_msg(NM_LOG, 0); strcpy(m.text, "\xff\xfe"); check_invalid(m, "log text not UTF-8");
}

/* Hand-made byte strings the decoder must refuse although each length is in bounds. */
static void test_attr_limits(void) {
    NetMsg m = sample_msg(NM_CREATE, 0);
    m.spec.attr[2] = SAVEFMT_MAX_ATTR + 1;
    check_invalid(m, "create with an attribute over the limit");
    NetMsg op = sample_msg(NM_OP, NOP_SET_STATS);
    op.op.stat_mask = NSTAT_ALL + 1;
    check_invalid(op, "set stats with an unknown bit");
    op = sample_msg(NM_OP, NOP_SET_STATS);
    op.op.attr[0] = SAVEFMT_MIN_ATTR - 1;
    check_invalid(op, "set stats with an attribute under the limit");
    /* CREATE_ANSWER: "accepted" is 0 or 1 */
    NetMsg a = sample_msg(NM_CREATE_ANSWER, 0);
    uint8_t *buf;
    size_t len;
    CHECK(netmsg_encode(&a, &buf, &len));
    buf[len - 1] = 2;
    NetMsg d;
    CHECK(!netmsg_decode(buf, len, &d));
    free(buf);
}

static void test_decoder_refuses_bytes(void) {
    static const struct { const char *what; uint8_t bytes[16]; size_t len; } cases[] = {
        { "empty", { 0 }, 0 },
        { "type 0", { 0 }, 1 },
        { "type NM_COUNT", { NM_COUNT }, 1 },
        { "presence without body", { NM_PRESENCE, 0 }, 2 },
        { "presence circle flag 2", { NM_PRESENCE, 0, 0, 0, 0, 0, 0, 2 }, 8 },
        { "welcome without player", { NM_WELCOME }, 1 },
        { "hello name length past the end", { NM_HELLO, 1, 9, 'B', 'o', 'b' }, 6 },
        { "hello name too long", { NM_HELLO, 1, 25, 'a' }, 4 },
        { "varint longer than 32 bits", { NM_OP_REJECTED, 0xff, 0xff, 0xff, 0xff, 0x7f, 1 }, 7 },
        { "op type past the end", { NM_OP, 1, NOP_COUNT }, 3 },
        { "id count larger than the message", { NM_OP, 1, NOP_DELETE, 0x80, 0x80, 0x04, 1 }, 7 },
        { "snapshot without data", { NM_SNAPSHOT }, 1 },
        { "close reason 0", { NM_CLOSE, 0 }, 2 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        NetMsg d;
        bool ok = netmsg_decode(cases[i].bytes, cases[i].len, &d);
        if (ok) fprintf(stderr, "decoder accepted: %s\n", cases[i].what);
        CHECK(!ok);
        netmsg_free(&d);
    }
}

/* console arguments drop quotes, so :kick / :ban could not name a player called Eve" */
static void test_hello_no_quotes(void) {
    NetMsg m = sample_msg(NM_HELLO, 0);
    uint8_t *buf;
    size_t len;
    strcpy(m.name, "Eve\"");
    CHECK(!netmsg_encode(&m, &buf, &len));
}

static void test_create(void) {
    NetMsg m = sample_msg(NM_CREATE, 0), d;
    CHECK(round_trip(&m, &d) && d.seq == 7 && d.spec.type == SAVE_ENT_CHARACTER && d.spec.row == 3 &&
          d.spec.col == -4 && d.spec.size == -2 && d.spec.max_health == 20 && d.spec.health == 20 &&
          d.spec.speed == 6 && d.spec.initiative == 1 && d.data_len == sizeof(blob) &&
          strcmp(d.spec.name, "Nina") == 0 && d.spec.max_stamina == 8 && d.spec.stamina == 8);
    netmsg_free(&d);
    uint8_t *buf;
    size_t len;
    NetMsg bad = m; bad.spec.type = SAVE_ENT_WALL;        CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; bad.spec.size = SAVEFMT_MAX_SIZE + 1;          CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; bad.spec.speed = SAVEFMT_MAX_SPEED + 1;       CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; bad.spec.stamina = 5;                         CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; bad.spec.max_stamina = bad.spec.stamina = SAVEFMT_MAX_HP + 1; CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; strcpy(bad.spec.name, "a\x01");               CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; bad.data_len = 0;                             CHECK(!netmsg_encode(&bad, &buf, &len));
    bad = m; bad.data_len = NET_CREATE_MAX_BLOB + 1;       CHECK(!netmsg_encode(&bad, &buf, &len));
}

static void test_image_hash(void) {
    uint8_t a[NET_HASH_SIZE], b[NET_HASH_SIZE];
    netmsg_image_hash(blob, sizeof(blob), a);
    crypto_generichash(b, sizeof(b), blob, sizeof(blob), NULL, 0);
    CHECK(memcmp(a, b, NET_HASH_SIZE) == 0);
}

/* ---- join codes -------------------------------------------------------------------------- */

static JoinCode sample_code(void) {
    JoinCode c = { .port = NET_DEFAULT_PORT, .naddrs = 2 };
    for (int i = 0; i < NET_SECRET_SIZE; i++) c.secret[i] = (uint8_t)(i * 37 + 1);
    c.addrs[0] = (NetAddr){ .family = 6, .addr = { 0x20, 0x01, 0x0d, 0xb8, [15] = 1 } };
    c.addrs[1] = (NetAddr){ .family = 4, .addr = { 203, 0, 113, 7 } };
    return c;
}

static bool codes_equal(const JoinCode *a, const JoinCode *b) {
    if (a->relay != b->relay || a->port != b->port || a->naddrs != b->naddrs ||
        memcmp(a->secret, b->secret, NET_SECRET_SIZE)) return false;
    for (int i = 0; i < a->naddrs; i++) {
        int n = a->addrs[i].family == 4 ? 4 : 16;
        if (a->addrs[i].family != b->addrs[i].family || memcmp(a->addrs[i].addr, b->addrs[i].addr, (size_t)n)) return false;
    }
    return true;
}

static void test_joincode_round_trip(void) {
    JoinCode c = sample_code(), d;
    char s[JOINCODE_MAX_LEN];
    CHECK(joincode_encode(&c, s));
    CHECK(strncmp(s, "toh1-", 5) == 0);
    CHECK(strlen(s) > 60 && strlen(s) < 90); /* the spec promises about 75 characters */
    CHECK(joincode_decode(s, &d) && codes_equal(&c, &d));

    /* groups of 4 separated by '-' */
    for (size_t i = 5; s[i]; i++) CHECK(((i - 5) % 5 == 4) == (s[i] == '-'));

    /* case, dashes and the look-alike letters do not matter */
    char t[JOINCODE_MAX_LEN * 2];
    size_t n = 0;
    for (size_t i = 0; s[i]; i++) {
        char ch = s[i];
        if (i >= 5 && ch == '-') continue;          /* drop the groups */
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (i >= 5 && ch == '0') ch = 'o';
        if (i >= 5 && ch == '1') ch = (i % 2) ? 'I' : 'l';
        t[n++] = ch;
        if (i > 5 && i % 3 == 0) t[n++] = '-';      /* other groups */
    }
    t[n] = '\0';
    t[0] = 'T';
    CHECK(joincode_decode(t, &d) && codes_equal(&c, &d));

    /* the largest code fits the buffer */
    JoinCode big = sample_code();
    big.naddrs = NET_MAX_ADDRS;
    for (int i = 0; i < NET_MAX_ADDRS; i++) big.addrs[i] = (NetAddr){ .family = 6, .addr = { 0xfd, (uint8_t)i } };
    CHECK(joincode_encode(&big, s) && strlen(s) < JOINCODE_MAX_LEN && joincode_decode(s, &d) && codes_equal(&big, &d));
}

static void test_joincode_rejects(void) {
    JoinCode c = sample_code(), d;
    char s[JOINCODE_MAX_LEN], t[JOINCODE_MAX_LEN + 8];

    JoinCode bad = c; bad.port = 0;                 CHECK(!joincode_encode(&bad, s));
    bad = c; bad.naddrs = 0;                        CHECK(!joincode_encode(&bad, s));
    bad = c; bad.naddrs = NET_MAX_ADDRS + 1;        CHECK(!joincode_encode(&bad, s));
    bad = c; bad.addrs[1].family = 5;               CHECK(!joincode_encode(&bad, s));

    CHECK(joincode_encode(&c, s));
    CHECK(!joincode_decode("", &d));
    CHECK(!joincode_decode("toh1-", &d));
    CHECK(!joincode_decode("hello", &d));
    snprintf(t, sizeof(t), "toh2%s", s + 4);        CHECK(!joincode_decode(t, &d));  /* other prefix */
    snprintf(t, sizeof(t), "%sU", s);               CHECK(!joincode_decode(t, &d));  /* not in the alphabet */
    snprintf(t, sizeof(t), "%s0", s);               CHECK(!joincode_decode(t, &d));  /* one character too many */
    snprintf(t, sizeof(t), "%s", s);
    t[strlen(t) - 1] = '\0';                        CHECK(!joincode_decode(t, &d));  /* one character missing */
    snprintf(t, sizeof(t), "%s ", s);               CHECK(!joincode_decode(t, &d));  /* trailing space */

    /* The sample's last character holds 1 data bit and 4 padding bits, which must be zero:
     * of all 32 symbols only 0 (00000) and G (10000) decode. */
    snprintf(t, sizeof(t), "%s", s);
    size_t last = strlen(t) - 1;
    const char *alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (int v = 0; v < 32; v++) {
        t[last] = alphabet[v];
        CHECK(joincode_decode(t, &d) == ((v & 15) == 0));
    }
}

static void test_joincode_relay(void) {
    JoinCode c = { .relay = true }, d;
    for (int i = 0; i < NET_SECRET_SIZE; i++) c.secret[i] = (uint8_t)(200 - i);
    char s[JOINCODE_MAX_LEN];
    CHECK(joincode_encode(&c, s));
    CHECK(strncmp(s, "toh1-", 5) == 0 && strlen(s) == 39); /* 17 bytes: 28 characters in 7 groups */
    CHECK(joincode_decode(s, &d) && d.relay && d.port == 0 && d.naddrs == 0 && codes_equal(&c, &d));

    /* address codes still work and are not relay codes */
    JoinCode v1 = sample_code();
    CHECK(joincode_encode(&v1, s) && joincode_decode(s, &d) && !d.relay && codes_equal(&v1, &d));

    /* relay codes carry no port and no addresses */
    JoinCode bad = c;
    bad.port = 1;
    CHECK(!joincode_encode(&bad, s));
    bad = c;
    bad.naddrs = 1;
    bad.addrs[0].family = 4;
    CHECK(!joincode_encode(&bad, s));

    /* the room ID depends on the secret only, and does not reveal it */
    uint8_t r1[NET_ROOM_SIZE], r2[NET_ROOM_SIZE], r3[NET_ROOM_SIZE];
    joincode_room_id(c.secret, r1);
    joincode_room_id(c.secret, r2);
    JoinCode other = c;
    other.secret[0] ^= 1;
    joincode_room_id(other.secret, r3);
    CHECK(memcmp(r1, r2, NET_ROOM_SIZE) == 0);
    CHECK(memcmp(r1, r3, NET_ROOM_SIZE) != 0);
    CHECK(memcmp(r1, c.secret, NET_ROOM_SIZE) != 0);
}

/* ---- fuzzer seeds ------------------------------------------------------------------------ */

/* Fuzzer input: first byte 0 = message, 1 = join code (see tests/fuzz_netmsg.c). */
static void write_seed(const char *dir, const char *name, uint8_t kind, const void *p, size_t n) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputc(kind, f);
    fwrite(p, 1, n, f);
    fclose(f);
}

static void write_seeds(const char *dir) {
    for (int t = NM_HELLO; t < NM_COUNT; t++) {
        for (int o = 0; o < ((t == NM_OP) ? NOP_COUNT : 1); o++) {
            NetMsg m = sample_msg(t, o);
            uint8_t *buf;
            size_t len;
            if (!netmsg_encode(&m, &buf, &len)) { fprintf(stderr, "cannot encode seed %d/%d\n", t, o); exit(1); }
            char name[32];
            snprintf(name, sizeof(name), "msg-%02d-%02d", t, o);
            write_seed(dir, name, 0, buf, len);
            free(buf);
        }
    }
    JoinCode c = sample_code();
    char s[JOINCODE_MAX_LEN];
    joincode_encode(&c, s);
    write_seed(dir, "joincode", 1, s, strlen(s));
}

int main(int argc, char **argv) {
    if (sodium_init() < 0) return 1;
    test_every_message_round_trips();
    test_fields_survive();
    test_truncated_and_trailing();
    test_encoder_refuses_invalid();
    test_decoder_refuses_bytes();
    test_attr_limits();
    test_create();
    test_hello_no_quotes();
    test_image_hash();
    test_joincode_round_trip();
    test_joincode_rejects();
    test_joincode_relay();
    if (argc > 1) write_seeds(argv[1]);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

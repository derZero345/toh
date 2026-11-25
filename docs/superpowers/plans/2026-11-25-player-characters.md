# Player Characters Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show who owns which character (colored rings), and let players request their own character (stats and picture), which the GM accepts or declines.

**Architecture:**
- **Rings:** `session.c` maps an owner name to a color. `graphics.c` draws the rings after the entities.
- **Creation requests:**
  - A new client→host message `NM_CREATE` carries the stats, the tile and the WebP picture.
  - The host checks it and queues it with the claims (`Claim` becomes `Request`), asks the GM y/n, and draws a faint preview for the GM.
  - On `y` the host submits `OP_SPAWN` and `OP_SET_OWNER` like any GM op.
- `NET_PROTOCOL_VERSION` goes to 2.

**Tech Stack:** C11, SDL3, libsodium, libwebp; tests are plain C (`make test`) and `tests/e2e_net.sh` (`make e2e`).

Spec: `docs/superpowers/specs/2026-11-25-player-characters-design.md`.

---

### Task 1: `NM_CREATE` message

**Files:**
- Modify: `src/netmsg.h` (enum, limits, `NetMsg.spec`, version)
- Modify: `src/netmsg.c` (`msg_ok`, encoder, decoder)
- Test: `tests/netmsg_test.c`

- [ ] **Step 1: Failing test.**
  - In `sample_msg` add the case below. `test_every_message_round_trips` then covers `NM_CREATE`, and the fuzz corpus gets a sample automatically.

    ```c
    case NM_CREATE:
        m.seq = 7;
        m.spec = (NetSpec){ .type = SAVE_ENT_CHARACTER, .row = 3, .col = -4, .size = -2, .max_health = 20,
                            .health = 20, .ac = 12, .speed = 6, .initiative = 1 };
        m.data = blob; m.data_len = sizeof(blob);
        break;
    ```

  - Add a test and call it from `main`:

    ```c
    static void test_create(void) {
        NetMsg m = sample_msg(NM_CREATE, 0), d;
        CHECK(round_trip(&m, &d) && d.seq == 7 && d.spec.type == SAVE_ENT_CHARACTER && d.spec.row == 3 &&
              d.spec.col == -4 && d.spec.size == -2 && d.spec.max_health == 20 && d.spec.health == 20 &&
              d.spec.ac == 12 && d.spec.speed == 6 && d.spec.initiative == 1 && d.data_len == sizeof(blob));
        netmsg_free(&d);
        uint8_t *buf; size_t len;
        NetMsg bad = m; bad.spec.type = SAVE_ENT_WALL;        CHECK(!netmsg_encode(&bad, &buf, &len));
        bad = m; bad.spec.size = 4;                            CHECK(!netmsg_encode(&bad, &buf, &len));
        bad = m; bad.spec.speed = SAVEFMT_MAX_SPEED + 1;       CHECK(!netmsg_encode(&bad, &buf, &len));
        bad = m; bad.data_len = 0;                             CHECK(!netmsg_encode(&bad, &buf, &len));
        bad = m; bad.data_len = NET_CREATE_MAX_BLOB + 1;       CHECK(!netmsg_encode(&bad, &buf, &len));
    }
    ```

- [ ] **Step 2:** Run `make build/netmsg_test`. Expected: compile error (`NM_CREATE` is unknown).

- [ ] **Step 3: Implement.** In `netmsg.h`:
  - `NET_PROTOCOL_VERSION` becomes `2`.
  - Add `#define NET_CREATE_MAX_BLOB (1u << 20)` and `#define NET_CREATE_MAX_DIM 512`.
  - Add `NM_CREATE, /* C->H seq, spec (character, no image hash), data: WebP bytes of its picture */` before `NM_COUNT`.
  - Add `NetSpec spec; /* CREATE */` to `NetMsg`.

  In `netmsg.c`:
  - `msg_ok`:

    ```c
    case NM_CREATE:
        return m->spec.type == SAVE_ENT_CHARACTER && spec_ok(&m->spec) && m->spec.size >= -2 &&
               m->spec.size <= 3 && m->spec.health == m->spec.max_health && m->data && m->data_len >= 1 &&
               m->data_len <= NET_CREATE_MAX_BLOB;
    ```

  - Encoder:

    ```c
    case NM_CREATE:
        put_uv(&b, m->seq);
        put_sv(&b, m->spec.row); put_sv(&b, m->spec.col); put_sv(&b, m->spec.size);
        put_sv(&b, m->spec.max_health); put_sv(&b, m->spec.ac); put_sv(&b, m->spec.speed);
        put_sv(&b, m->spec.initiative);
        put_bytes(&b, m->data, m->data_len);
        break;
    ```

  - Decoder:

    ```c
    case NM_CREATE:
        out->seq = get_uv(&r);
        out->spec.type = SAVE_ENT_CHARACTER;
        out->spec.row = get_sv(&r); out->spec.col = get_sv(&r); out->spec.size = get_sv(&r);
        out->spec.max_health = get_sv(&r); out->spec.ac = get_sv(&r); out->spec.speed = get_sv(&r);
        out->spec.initiative = get_sv(&r);
        out->spec.health = out->spec.max_health;
        get_blob(&r, out);
        break;
    ```

- [ ] **Step 4:** Run `make build/netmsg_test && ./build/netmsg_test`. Expected: `0 failed`.

- [ ] **Step 5:** Commit: `netmsg: NM_CREATE (a player's character request), protocol 2`.

### Task 2: `OPR_TILE_TAKEN` and `image_scale_to_fit`

**Files:**
- Modify: `src/ops.h`, `src/ops.c`, `src/image.h`, `src/image.c`
- Modify: `Makefile` (add `src/image.c` to `CORE_SRC`)
- Test: `tests/core_test.c`

- [ ] **Step 1: Failing tests** in `tests/core_test.c`, called from `main`:

  ```c
  static void test_create_helpers(void) {
      CHECK(strcmp(ops_result_text(OPR_TILE_TAKEN), "That tile is taken") == 0);
      Image src = { .w = 1000, .h = 600, .px = calloc(1000 * 600, 4) };
      for (int i = 0; i < 1000 * 600; i++) src.px[i * 4 + 3] = 255;
      Image s = image_scale_to_fit(&src, 512);
      CHECK(s.w == 512 && s.h == 307 && s.px && s.px[3] == 255);
      image_free(&s);
      Image small = image_scale_to_fit(&(Image){ .w = 10, .h = 20, .px = src.px }, 512);
      CHECK(small.w == 10 && small.h == 20 && memcmp(small.px, src.px, 10 * 20 * 4) == 0);
      image_free(&small);
      image_free(&src);
  }
  ```

- [ ] **Step 2:** Run `make build/core_test`. Expected: compile error.

- [ ] **Step 3: Implement.**
  - `ops.h`: add `OPR_TILE_TAKEN, /* session: a requested character's tile is occupied */` after `OPR_TOO_FAST`, and set `#define OPR_LAST OPR_TILE_TAKEN`.
  - `ops.c`: add `case OPR_TILE_TAKEN: return "That tile is taken";`.
  - `image.h`: add `Image image_scale_to_fit(const Image *src, int max); /* box-filtered copy, at most max per side */`.
  - `image.c`:

    ```c
    Image image_scale_to_fit(const Image *src, int max) {
        int big = src->w > src->h ? src->w : src->h;
        int w = src->w, h = src->h;
        if (big > max) {
            w = (int)((long long)src->w * max / big);
            h = (int)((long long)src->h * max / big);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
        }
        Image out = image_alloc(w, h);
        for (int y = 0; y < h; y++) {
            int y0 = (int)((long long)y * src->h / h), y1 = (int)((long long)(y + 1) * src->h / h);
            for (int x = 0; x < w; x++) {
                int x0 = (int)((long long)x * src->w / w), x1 = (int)((long long)(x + 1) * src->w / w);
                unsigned sum[4] = { 0 }, n = 0;
                for (int sy = y0; sy < y1; sy++)
                    for (int sx = x0; sx < x1; sx++, n++)
                        for (int c = 0; c < 4; c++) sum[c] += src->px[((size_t)sy * src->w + sx) * 4 + c];
                for (int c = 0; c < 4; c++) out.px[((size_t)y * w + x) * 4 + c] = (uint8_t)(sum[c] / n);
            }
        }
        return out;
    }
    ```

  - `Makefile`: `CORE_SRC := ... src/savefmt.c src/image.c`. If `image_load_png` needs symbols the core tests lack, stub them in `tests/core_stubs.c`.

- [ ] **Step 4:** Run `make build/core_test && ./build/core_test`. Expected: `0 failed`.

- [ ] **Step 5:** Commit: `ops: OPR_TILE_TAKEN; image_scale_to_fit`.

### Task 3: Ownership rings

**Files:**
- Modify: `src/session.h`, `src/session.c`, `src/graphics.c`, `src/app.h`

- [ ] **Step 1: `session.h`.** Add:

  ```c
  /* Ring color of a character owned by owner: true when one is drawn (in a session, owner not empty).
   * mine: we are that owner. Owners not at the table are grey. */
  bool session_owner_color(const char *owner, uint32_t *color, bool *mine);
  ```

- [ ] **Step 2: `session.c`.** Add `#define OWNER_ABSENT_COLOR 0x9AA0A6u` and:

  ```c
  bool session_owner_color(const char *owner, uint32_t *color, bool *mine) {
      if ((S.role != ROLE_HOST && S.role != ROLE_CLIENT) || !owner[0]) return false;
      *mine = strcasecmp(owner, S.myName) == 0;
      *color = OWNER_ABSENT_COLOR;
      for (int i = 0; i < MAX_PLAYERS; i++)
          if (S.players[i].welcomed && strcasecmp(S.players[i].name, owner) == 0) *color = S.players[i].color;
      return true;
  }
  ```

- [ ] **Step 3: `graphics.c`.**
  - Export `void gfx_character_rect(Hex tile, int size, float *x, float *y, float *d);` in `app.h`. It is the position code from `draw_entities`, which now calls it:

    ```c
    void gfx_character_rect(Hex tile, int size, float *x, float *y, float *d) {
        double hs = gh->hexSize;
        Entity tmp = { .type = ENTITY_CHARACTER, .size = size };
        double ds = entity_draw_size(&tmp);
        Vec2 c = hex_center(tile);
        double left;
        switch (size) {
        case SIZE_LARGE:      left = c.x; break;
        case SIZE_GARGANTUAN: left = c.x - ((ds - 2) * hs / 2); break;
        default:              left = c.x - (ds * hs / 2); break;
        }
        *x = (float)left;
        *y = (float)(c.y - ds * hs / 2);
        *d = (float)(ds * hs);
    }
    ```

  - Add `draw_owner_rings()` and call it in `gfx_draw_frame` right after `draw_entities()`:

    ```c
    static void draw_owner_rings(void) {
        float th = gh->hexSize / 15.0f < 2 ? 2 : gh->hexSize / 15.0f;
        for (int i = 0; i < gh->entities.count; i++) {
            const Entity *e = gh->entities.items[i];
            uint32_t color;
            bool mine;
            if (e->type != ENTITY_CHARACTER || !session_owner_color(e->owner, &color, &mine)) continue;
            float x, y, d;
            gfx_character_rect(e->tile, e->size, &x, &y, &d);
            dr_stroke_oval(x, y, d, d, mine ? th * 2 : th, rgb_hex(color));
        }
    }
    ```

- [ ] **Step 4:** Run `make && make test`. Expected: builds without warnings; all suites pass. The rings are checked in Task 5.

- [ ] **Step 5:** Commit: `Ownership rings in the owner's color during a session`.

### Task 4: Creation requests

**Files:**
- Modify: `src/session.h`, `src/session.c`, `src/graphics.c`

- [ ] **Step 1: Client side.**
  - `session.h`:
    `void session_request_character(const Image *pic, Hex tile, int size, int maxHealth, int ac, int speed, int initiative);`
    and `void session_draw_request_preview(void);`. Include `image.h`.
  - `session.c`:

    ```c
    void session_request_character(const Image *pic, Hex tile, int size, int maxHealth, int ac, int speed,
                                   int initiative) {
        if (S.role != ROLE_CLIENT) return;
        SaveBlob b;
        if (!savefmt_encode_image(pic->px, pic->w, pic->h, SAVE_IMG_LOSSLESS, &b)) {
            console_log("Could not encode the picture");
            return;
        }
        NetMsg m = { .type = NM_CREATE, .seq = ++S.seq, .data = b.webp, .data_len = b.len };
        m.spec = (NetSpec){ .type = SAVE_ENT_CHARACTER, .row = tile.row, .col = tile.col, .size = size,
                            .max_health = maxHealth, .health = maxHealth, .ac = ac,
                            .speed = speed > MAX_SPEED ? MAX_SPEED : speed, .initiative = initiative };
        if (b.len > NET_CREATE_MAX_BLOB) console_log("The picture is too large");
        else if (!net_send(S.net, 1, &m)) console_log("Could not send that to the host");
        else console_log("Asked the GM to create your character.");
        free(b.webp);
    }
    ```

  - `graphics.c` `gfx_spawn_character`: at the start, add
    `if (session_is_client() && gh->selectedTiles.count > 1) { console_log("Select one tile"); return; }`.
  - `graphics.c` `ACTION_SPAWN_CHARACTERS`: right after `image_cut_circle`, add

    ```c
    if (session_is_client()) {
        Image small = image_scale_to_fit(&cut, NET_CREATE_MAX_DIM);
        image_free(&cut);
        if (a->tiles.count == 1)
            session_request_character(&small, a->tiles.items[0], a->size, a->maxHealth, a->ac, a->speed,
                                      a->initiative);
        image_free(&small);
        break;
    }
    ```

    (`graphics.c` needs `netmsg.h` for `NET_CREATE_MAX_DIM`.)

- [ ] **Step 2: Host side, the request queue.** Replace `Claim` / `S.claims` / `ask_next_claim` / `claim_answer`:

  ```c
  typedef enum { REQ_CLAIM, REQ_CREATE } RequestKind;
  typedef struct {
      RequestKind kind;
      uint8_t player;
      uint32_t entity;  /* REQ_CLAIM */
      EntitySpec spec;  /* REQ_CREATE: spec.image holds one texture reference */
  } Request;

  static void request_free(Request *r) {
      if (r->kind == REQ_CREATE) vkr_texture_release(r->spec.image);
      r->spec.image = NULL;
  }

  static int requests_of(uint8_t player) {
      int n = 0;
      for (int i = 0; i < S.requests.count; i++) n += S.requests.items[i].player == player;
      return n;
  }

  /* Removes request i (releasing its texture). */
  static void request_remove(int i) {
      request_free(&S.requests.items[i]);
      da_remove_at(&S.requests, i);
  }

  static bool tiles_free(Hex tile, int size) {
      Hex t[MAX_OCCUPIED];
      int n = character_occupied_tiles(tile, size, t);
      for (int i = 0; i < n; i++)
          if (entity_at(t[i])) return false;
      return true;
  }
  ```

  Update every user of the old names:
  - `S.claims` becomes `S.requests` (`DA(Request)`).
  - The claim limit in `host_op` uses `requests_of(p->id) >= CLAIMS_PER_PLAYER`. "Again" still compares `kind == REQ_CLAIM && entity == o->id`.
  - Pushing a claim: `da_push(&S.requests, ((Request){ .kind = REQ_CLAIM, .player = p->id, .entity = o->id }))`.
  - `NET_EV_CLOSED`: use `request_remove(i)` in the cleanup loop.
  - `end_host`: `for (int i = S.requests.count - 1; i >= 0; i--) request_remove(i); da_free(&S.requests);`.

- [ ] **Step 3: Host receives `NM_CREATE`.** In `host_message`, next to `NM_OP`:
  `else if (m->type == NM_CREATE) host_create(p, m);`, where

  ```c
  static void host_create(Player *p, const NetMsg *m) {
      NetMsg rej = { .type = NM_OP_REJECTED, .seq = m->seq };
      Hex tile = { m->spec.row, m->spec.col };
      int w, h;
      uint8_t *rgba = NULL;
      if (requests_of(p->id) >= CLAIMS_PER_PLAYER) rej.reason = OPR_CLAIM_PENDING;
      else if (!tiles_free(tile, m->spec.size)) rej.reason = OPR_TILE_TAKEN;
      else if (!savefmt_decode_image(m->data, m->data_len, &w, &h, &rgba) || w > NET_CREATE_MAX_DIM ||
               h > NET_CREATE_MAX_DIM)
          rej.reason = OPR_INVALID;
      if (rej.reason) {
          free(rgba);
          net_send(S.net, p->conn, &rej);
          return;
      }
      Texture *t = vkr_texture_create_kept(w, h, rgba); /* takes rgba */
      uint8_t *blob = malloc(m->data_len);
      if (!blob) abort();
      memcpy(blob, m->data, m->data_len);
      vkr_texture_set_blob(t, blob, m->data_len, SAVE_IMG_LOSSLESS, w, h);
      Request r = { .kind = REQ_CREATE, .player = p->id };
      r.spec = (EntitySpec){ .type = ENTITY_CHARACTER, .image = t, .tile = tile, .size = m->spec.size,
                             .maxHealth = m->spec.max_health, .health = m->spec.max_health, .ac = m->spec.ac,
                             .speed = m->spec.speed, .initiative = m->spec.initiative };
      da_push(&S.requests, r);
      ask_next_request();
  }
  ```

- [ ] **Step 4: Asking and answering.**
  - `ask_next_request` (was `ask_next_claim`). An outdated claim is dropped as before; a create request is dropped when its player is gone. The question:

    ```c
    if (c->kind == REQ_CREATE)
        snprintf(q, sizeof(q), "%s wants to create a character at [%d|%d]: size %d, max health %d, AC %d, "
                 "speed %d, initiative %d. Allow? (y/n)", p->name, c->spec.tile.row, c->spec.tile.col,
                 c->spec.size, c->spec.maxHealth, c->spec.ac, c->spec.speed, c->spec.initiative);
    ```

    (`q` grows to 256 bytes.)
  - `request_answer(bool yes, void *ctx)` (was `claim_answer`): it takes `Request r = S.requests.items[0]`, then `da_remove_at` (not `request_remove`; it frees `r` itself at the end). Claims go to the old code. Create requests:

    ```c
    static void answer_create(const Request *r, Player *p, bool yes) {
        if (!p || !p->welcomed || p->leaving) return;
        if (!yes) {
            send_log(p, "The GM declined your character.");
            return;
        }
        if (!tiles_free(r->spec.tile, r->spec.size)) {
            console_log("The tile [%d|%d] is taken now; %s's character was not created.", r->spec.tile.row,
                        r->spec.tile.col, p->name);
            NetMsg rej = { .type = NM_OP_REJECTED, .seq = 0, .reason = OPR_TILE_TAKEN };
            net_send(S.net, p->conn, &rej);
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
            console_log("Created %s's character.", p->name);
        }
        op_free(&spawn);
    }
    ```

  - `session_prompt_free` calls `ask_next_request`.

- [ ] **Step 5: GM preview.**

  ```c
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
  ```

  Call it in `gfx_draw_frame` after `draw_owner_rings()`. `session.c` includes `draw.h`.

- [ ] **Step 6:** Run `make && make test`. Expected: no warnings, all suites pass.

- [ ] **Step 7:** Commit: `Players request their own characters; the GM sees stats and picture and answers y/n`.

### Task 5: End-to-end test

**Files:** Modify `tests/e2e_net.sh`.

- [ ] **Step 1:** Changes to the script:
  - Add tiles `D="440 140"` (empty) and `D2="440 200"`.
  - The client gets `--auto-file "$icon"`.
  - Client, after `client-checked`:

    ```
    --exec "@click $D" --exec ":c speed 6" --exec "@wait 20" --exec "@shot $out/client-requested.png" \
    --exec "@touch $out/requested" --exec "@waitfile $out/created" --exec "@wait 30" \
    --exec "@shot $out/client-created.png" \
    --exec "@click $D" --exec "@down lshift" --exec "@click $D2" --exec "@up lshift" --exec "@wait 30" \
    --exec "@touch $out/client-moved2" \
    ```

  - Host, after the claim screenshot:

    ```
    --exec "@waitfile $out/requested" --exec "@wait 30" --exec "@shot $out/host-request.png" --exec "y" \
    --exec "@touch $out/created" --exec "@waitfile $out/client-moved2" --exec "@wait 30" \
    --exec "@shot $out/host-created.png" \
    ```

  - Checks:
    - `! occupied client-requested.png $D`: the party doesn't see the request.
    - `faint host-request.png $D`: a mean grey between 0.70 and 0.86.
    - `occupied host-created.png $D2`.
    - `ring client-joined.png $A` is blue and `ring client-joined.png $B` is none.

    `ring` samples a 4×4 square on the ring's right edge. The exact offset is measured once from a screenshot (normal size: radius = `sqrt(3)/2 * hexSize`).

- [ ] **Step 2:** Run `make e2e`. Expected: `e2e net (direct): OK` and `e2e net (relay): OK`.

- [ ] **Step 3:** Commit: `e2e: player-created character and ownership rings`.

### Task 6: Docs

**Files:** `playercontrols.md`, `gmcontrols.md`, `README.md`, spec.

- [ ] **Step 1:** Replace the "coming soon" notes with the real instructions:
  - **Player guide:** section 6 create; mention rings in section 2.
  - **GM guide:** section 14, and a "Rings" paragraph.
- [ ] **Step 2:** In the README multiplayer section, add a sentence about the rings and player-created characters.
- [ ] **Step 3:** Fix the spec's order of host checks: the request count first, as implemented.
- [ ] **Step 4:** Commit: `Docs: player-created characters and ownership rings`.

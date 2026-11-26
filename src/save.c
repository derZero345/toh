/* Saving and loading the table: :save, :load, the unsaved-changes guard of :q.
 * Converts between the game state and SaveData; the file format lives in savefmt.c. */
#include "app.h"
#include "savefmt.h"
#include "moves.h"
#include "session.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(SAVEFMT_NAME_MAX == WAYPOINT_NAME_MAX, "waypoint name sizes differ");
_Static_assert(SAVEFMT_MAX_SPEED == MAX_SPEED, "speed limits differ");
_Static_assert(SAVEFMT_MAX_TURN_USED == TURN_ALL, "turn state bits differ");
_Static_assert(SAVEFMT_OWNER_MAX == PLAYER_NAME_MAX, "owner name sizes differ");
_Static_assert(SAVEFMT_CHAR_NAME_MAX == CHAR_NAME_MAX, "character name sizes differ");

#define SAVE_SUFFIX ".tuhh"
#define PATH_MAX_LEN 1024

static char current_file[PATH_MAX_LEN]; /* "" = none yet */
static uint8_t saved_fp[SAVEFMT_TAG_SIZE];
static bool have_fp;

/* ---- game state -> SaveData ---------------------------------------------------------------- */

/* SaveData built from the game. Image data is borrowed from the textures. */
typedef struct {
    SaveData d;
    DA(Texture *) tex; /* tex.items[i] belongs to image i */
    DA(SaveImage) images;
} Gathered;

static uint32_t image_for(Gathered *g, Texture *t, uint8_t kind) {
    for (int i = 0; i < g->tex.count; i++)
        if (g->tex.items[i] == t) return (uint32_t)i;
    SaveImage im = { .kind = kind, .w = vkr_texture_width(t), .h = vkr_texture_height(t), .id = vkr_texture_id(t) };
    size_t len;
    int bkind, bw, bh;
    const uint8_t *blob = vkr_texture_blob(t, &len, &bkind, &bw, &bh);
    if (blob) {
        im.kind = (uint8_t)bkind;
        im.webp = blob;
        im.webp_len = len;
        im.w = bw;
        im.h = bh;
    } else {
        im.rgba = vkr_texture_pixels(t); /* NULL makes savefmt_encode fail with SAVE_ERR_INVALID */
    }
    da_push(&g->tex, t);
    da_push(&g->images, im);
    return (uint32_t)(g->tex.count - 1);
}

static int entity_index(const Entity *e) {
    for (int i = 0; i < gh->entities.count; i++)
        if (gh->entities.items[i] == e) return i;
    return -1;
}

static void gather(Gathered *g) {
    memset(g, 0, sizeof(*g));
    SaveData *d = &g->d;
    d->is_flat = gh->isFlat;
    d->bg_rows = (uint32_t)gh->backgroundRows;
    d->bg_cols = (uint32_t)gh->backgroundCols;
    d->offset_x = gh->gridOffsetX;
    d->offset_y = gh->gridOffsetY;
    d->hex_size = (uint32_t)gh->hexSize;
    snprintf(d->theme, sizeof(d->theme), "%s", gfx_theme()->name);
    if (gh->backgroundImage) {
        d->has_background = true;
        d->background = image_for(g, gh->backgroundImage, SAVE_IMG_LOSSY);
    }

    int n = gh->entities.count;
    d->entities = calloc(n ? (size_t)n : 1, sizeof(*d->entities));
    if (!d->entities) abort();
    d->nentities = (uint32_t)n;
    for (int i = 0; i < n; i++) {
        const Entity *e = gh->entities.items[i];
        SaveEntity *s = &d->entities[i];
        s->type = e->type == ENTITY_WALL ? SAVE_ENT_WALL : SAVE_ENT_CHARACTER;
        s->image = image_for(g, e->image, SAVE_IMG_LOSSLESS);
        s->row = e->tile.row;
        s->col = e->tile.col;
        s->size = e->size;
        s->max_health = e->maxHealth;
        s->health = e->health;
        s->attack = e->attack;
        s->speed = e->speed;
        s->initiative = e->initiative;
        if (e->type == ENTITY_CHARACTER) {
            snprintf(s->owner, sizeof(s->owner), "%s", e->owner);
            snprintf(s->name, sizeof(s->name), "%s", e->name);
            s->max_stamina = e->maxStamina;
            s->stamina = e->stamina;
            snprintf(s->party, sizeof(s->party), "%s", e->party);
            s->turn_used = (uint8_t)(e->turnUsed & TURN_ALL);
        }
    }

    d->init = calloc(gh->init.count ? (size_t)gh->init.count : 1, sizeof(*d->init));
    if (!d->init) abort();
    for (int i = 0; i < gh->init.count; i++) {
        int idx = entity_index(gh->init.items[i].entity);
        if (idx < 0) continue; /* not expected: deleted entities leave the initiative */
        d->init[d->ninit++] = (SaveInit){ (uint32_t)idx, gh->init.items[i].value };
    }
    if (d->ninit > 0) {
        d->current_turn = (uint32_t)gh->currentTurnIndex < d->ninit ? (uint32_t)gh->currentTurnIndex : 0;
        d->turn_started = gh->currentTurn != NULL;
    }
    d->init_markers_visible = gh->initMarkers.count == 0 || gh->initMarkers.items[0]->visible;

    d->waypoints = calloc(gh->waypoints.count ? (size_t)gh->waypoints.count : 1, sizeof(*d->waypoints));
    if (!d->waypoints) abort();
    d->nwaypoints = (uint32_t)gh->waypoints.count;
    for (int i = 0; i < gh->waypoints.count; i++) {
        const Waypoint *w = &gh->waypoints.items[i];
        SaveWaypoint *s = &d->waypoints[i];
        snprintf(s->name, sizeof(s->name), "%s", w->name);
        s->row = w->tile.row;
        s->col = w->tile.col;
    }

    d->images = g->images.items;
    d->nimages = (uint32_t)g->images.count;
}

static void gathered_free(Gathered *g) {
    free(g->d.entities);
    free(g->d.init);
    free(g->d.waypoints);
    da_free(&g->tex);
    da_free(&g->images);
}

/* ---- unsaved changes ---------------------------------------------------------------------- */

static bool current_fingerprint(uint8_t out[SAVEFMT_TAG_SIZE]) {
    Gathered g;
    gather(&g);
    bool ok = savefmt_fingerprint(&g.d, out);
    gathered_free(&g);
    return ok;
}

static void remember_fingerprint(void) { have_fp = current_fingerprint(saved_fp); }

void save_init(void) { remember_fingerprint(); }

bool save_is_modified(void) {
    uint8_t fp[SAVEFMT_TAG_SIZE];
    if (!have_fp || !current_fingerprint(fp)) return true;
    return memcmp(fp, saved_fp, sizeof(fp)) != 0;
}

/* ---- files -------------------------------------------------------------------------------------- */

static void format_size(size_t n, char *out, size_t cap) {
    if (n < 1024) snprintf(out, cap, "%zu B", n);
    else if (n < 1024 * 1024) snprintf(out, cap, "%.1f KiB", n / 1024.0);
    else snprintf(out, cap, "%.1f MiB", n / (1024.0 * 1024.0));
}

/* Writes path.tmp and renames it over path, so an existing save survives a failed write. */
static bool write_atomic(const char *path, const uint8_t *data, size_t len) {
    char tmp[PATH_MAX_LEN + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        console_log("Could not write \"%s\": %s", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    int err = errno;
    if (fclose(f) != 0 && ok) {
        ok = false;
        err = errno;
    }
    if (ok && rename(tmp, path) != 0) {
        ok = false;
        err = errno;
    }
    if (!ok) {
        remove(tmp);
        console_log("Could not write \"%s\": %s", path, strerror(err));
    }
    return ok;
}

static bool read_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        console_log("Could not read \"%s\": %s", path, strerror(errno));
        return false;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        console_log("Could not read \"%s\": %s", path, strerror(errno));
        fclose(f);
        return false;
    }
    if ((unsigned long)size > SAVEFMT_MAX_FILE) {
        console_log("%s", savefmt_strerror(SAVE_ERR_TOO_LARGE));
        fclose(f);
        return false;
    }
    uint8_t *buf = malloc(size ? (size_t)size : 1);
    if (!buf) abort();
    if (fread(buf, 1, (size_t)size, f) != (size_t)size || ferror(f)) {
        console_log("Could not read \"%s\": %s", path, strerror(errno));
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t)size;
    return true;
}

/* ---- saving ------------------------------------------------------------------------------------- */

static void save_to(const char *path_in) {
    char path[PATH_MAX_LEN];
    size_t n = strlen(path_in), sl = strlen(SAVE_SUFFIX);
    bool has_suffix = n >= sl && strcmp(path_in + n - sl, SAVE_SUFFIX) == 0;
    if (snprintf(path, sizeof(path), "%s%s", path_in, has_suffix ? "" : SAVE_SUFFIX) >= (int)sizeof(path)) {
        console_log("Path is too long");
        return;
    }

    uint8_t *file = NULL;
    size_t len = 0;
    SaveError err = save_encode_table(&file, &len);
    if (err != SAVE_OK) {
        console_log("Could not save: %s", savefmt_strerror(err));
        return;
    }
    if (write_atomic(path, file, len)) {
        snprintf(current_file, sizeof(current_file), "%s", path);
        remember_fingerprint();
        char size[32];
        format_size(len, size, sizeof(size));
        console_log("Saved \"%s\" (%s)", path, size);
    }
    free(file);
}

int save_encode_table(uint8_t **out, size_t *out_len) {
    Gathered g;
    gather(&g);
    SaveBlob *blobs = calloc(g.d.nimages ? g.d.nimages : 1, sizeof(*blobs));
    if (!blobs) abort();
    SaveError err = savefmt_encode(&g.d, blobs, out, out_len);
    /* keep freshly encoded images on their textures: the next save doesn't re-encode them */
    for (uint32_t i = 0; i < g.d.nimages; i++) {
        if (!blobs[i].webp) continue;
        Texture *t = g.tex.items[i];
        if (blobs[i].w != vkr_texture_width(t) || blobs[i].h != vkr_texture_height(t))
            console_log("An image was scaled down to %dx%d to fit the save format", blobs[i].w, blobs[i].h);
        vkr_texture_set_blob(t, blobs[i].webp, blobs[i].len, g.d.images[i].kind, blobs[i].w, blobs[i].h);
        blobs[i].webp = NULL;
    }
    savefmt_blobs_free(blobs, g.d.nimages);
    free(blobs);
    gathered_free(&g);
    return err;
}

/* ---- loading ------------------------------------------------------------------------------------ */

static void clear_table(void) {
    entities_delete_type(2); /* all entities; also removes them from the initiative */
    game_clear_initiative();
    da_clear(&gh->waypoints);
    vkr_texture_release(gh->backgroundImage);
    gh->backgroundImage = NULL;
    da_clear(&gh->selectedTiles);
    da_clear(&gh->selectedEntities);
    da_clear(&gh->entityPreviewTiles);
    da_clear(&gh->attackTiles);
    da_clear(&gh->path);
    ts_free(&gh->entityRangeTiles);
    gh->hasSelectedEntity = false;
    da_clear(&gh->measures);
    io_update_total_length();
    gh->hasLineAttack = gh->hasAreaAttack = gh->hasConeAttack = false;
    gh->hasTileUnderMouse = false;
    gh->hasDragStart = false;
}

/* d was fully validated by savefmt_decode, so nothing here can fail.
 * keep_view: a session snapshot; camera, zoom and theme stay as they are. */
static void apply(SaveData *d, bool keep_view) {
    clear_table();

    Texture **tex = calloc(d->nimages ? d->nimages : 1, sizeof(*tex));
    if (!tex) abort();
    for (uint32_t i = 0; i < d->nimages; i++) {
        SaveImage *im = &d->images[i];
        tex[i] = vkr_texture_create(im->w, im->h, im->rgba, false);
        /* the WebP data moves to the texture: saving again won't re-encode it */
        vkr_texture_set_blob(tex[i], (uint8_t *)im->webp, im->webp_len, im->kind, im->w, im->h);
        im->webp = NULL;
        im->webp_len = 0;
    }

    gh->isFlat = d->is_flat;
    gh->backgroundRows = (int)d->bg_rows;
    gh->backgroundCols = (int)d->bg_cols;
    if (!keep_view) {
        gh->hexSize = (int)d->hex_size;
        gh->thickness = gh->hexSize / 30 > 1 ? gh->hexSize / 30 : 1;
        gh->gridOffsetX = d->offset_x;
        gh->gridOffsetY = d->offset_y;
        gfx_change_theme(d->theme); /* logs a note if the theme doesn't exist */
    }
    if (d->has_background) {
        gh->backgroundImage = tex[d->background];
        vkr_texture_retain(gh->backgroundImage);
    }

    for (uint32_t i = 0; i < d->nentities; i++) {
        const SaveEntity *s = &d->entities[i];
        Hex tile = { s->row, s->col };
        Entity *e;
        if (s->type == SAVE_ENT_WALL) {
            e = entity_new_wall(tex[s->image], tile);
        } else {
            e = entity_new_character(tex[s->image], tile, s->size, s->max_health, s->speed, s->initiative);
            e->health = s->health;
            e->attack = s->attack;
            snprintf(e->owner, sizeof(e->owner), "%s", s->owner);
            snprintf(e->name, sizeof(e->name), "%s", s->name);
            e->maxStamina = s->max_stamina;
            e->stamina = hp_clamp(s->stamina, s->max_stamina);
            snprintf(e->party, sizeof(e->party), "%s", s->party);
            e->turnUsed = s->turn_used;
        }
        entity_register(e);
    }

    Entity **init_entities = calloc(d->ninit ? d->ninit : 1, sizeof(*init_entities));
    double *init_values = calloc(d->ninit ? d->ninit : 1, sizeof(*init_values));
    if (!init_entities || !init_values) abort();
    for (uint32_t i = 0; i < d->ninit; i++) {
        init_entities[i] = gh->entities.items[d->init[i].entity];
        init_values[i] = d->init[i].value;
    }
    game_restore_initiative(init_entities, init_values, (int)d->ninit, (int)d->current_turn, d->turn_started,
                            d->init_markers_visible);
    free(init_entities);
    free(init_values);

    for (uint32_t i = 0; i < d->nwaypoints; i++) {
        Waypoint w = { .tile = { d->waypoints[i].row, d->waypoints[i].col } };
        snprintf(w.name, sizeof(w.name), "%s", d->waypoints[i].name);
        da_push(&gh->waypoints, w);
    }

    /* entities and the background hold their own references */
    for (uint32_t i = 0; i < d->nimages; i++) vkr_texture_release(tex[i]);
    free(tex);
    gfx_out_of_bounds_correction();
}

static void load_from(const char *path) {
    uint8_t *buf;
    size_t len;
    if (!read_file(path, &buf, &len)) return;
    SaveData d;
    SaveError err = savefmt_decode(buf, len, &d);
    free(buf);
    if (err != SAVE_OK) {
        console_log("Could not load \"%s\": %s", path, savefmt_strerror(err));
        savefmt_free(&d);
        return;
    }
    apply(&d, false);
    savefmt_free(&d);
    snprintf(current_file, sizeof(current_file), "%s", path);
    remember_fingerprint();
    console_log("Loaded \"%s\"", path);
    if (session_is_host()) session_table_replaced();
}

bool save_apply_snapshot(const uint8_t *buf, size_t len) {
    SaveData d;
    SaveError err = savefmt_decode(buf, len, &d);
    if (err != SAVE_OK) {
        savefmt_free(&d);
        return false;
    }
    apply(&d, true);
    savefmt_free(&d);
    current_file[0] = '\0'; /* :save must not overwrite a file of the player's own with the host's table */
    return true;
}

void save_mark_modified(void) { have_fp = false; }

/* ---- commands ------------------------------------------------------------------------------------ */

static const char *const NO_WRITE = "No write since last change (add ! to override)";

/* "~/x" -> "$HOME/x", as in nvim. */
static const char *expand_home(const char *path, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (path && path[0] == '~' && (path[1] == '/' || path[1] == '\0') && home) {
        snprintf(out, cap, "%s%s", home, path + 1);
        return out;
    }
    return path;
}

void save_command(const char *path) {
    char buf[PATH_MAX_LEN * 2];
    path = expand_home(path, buf, sizeof(buf));
    if (path && *path) save_to(path);
    else if (current_file[0]) save_to(current_file);
    else gfx_request_file(ACTION_SAVE, NULL, 0, 0, 0, 0, 0, NULL, NULL);
}

void load_command(const char *path, bool force) {
    if (session_is_client()) {
        console_log("Only the GM can load a table");
        return;
    }
    if (!force && save_is_modified()) {
        console_log("%s", NO_WRITE);
        return;
    }
    char buf[PATH_MAX_LEN * 2];
    path = expand_home(path, buf, sizeof(buf));
    if (path && *path) load_from(path);
    else gfx_request_file(ACTION_LOAD, NULL, 0, 0, 0, 0, 0, NULL, NULL);
}

void quit_command(bool force) {
    /* a connected player's table is the host's: nothing of theirs gets lost */
    if (!force && !session_is_client() && save_is_modified()) {
        console_log("%s", NO_WRITE);
        return;
    }
    gh->running = false;
}

void save_handle_dialog(ActionKind kind, const char *path) {
    if (!path) {
        console_log(kind == ACTION_SAVE ? "Save canceled" : "Load canceled");
        return;
    }
    if (kind == ACTION_SAVE) save_to(path);
    else if (session_is_client()) console_log("Only the GM can load a table");
    else load_from(path);
}

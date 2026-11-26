/* Stand-ins for the renderer, console and graphics functions that the game core calls,
 * so entity.c, game.c, hex.c, astar.c and ops.c can be tested without a window. */
#include "app.h"
#include "draw.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

App *gh;

char stub_last_log[1024];
int stub_log_count;
Texture *stub_background;

void console_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stub_last_log, sizeof(stub_last_log), fmt, ap);
    va_end(ap);
    stub_log_count++;
}

/* Reference counts per texture pointer, so tests can check that nothing leaks or is released twice. */
static struct { Texture *t; int refs; } stub_tex[16];
int stub_bad_releases;

static int *stub_ref_slot(Texture *t) {
    for (int i = 0; i < 16; i++)
        if (stub_tex[i].t == t) return &stub_tex[i].refs;
    for (int i = 0; i < 16; i++)
        if (!stub_tex[i].t) {
            stub_tex[i].t = t;
            return &stub_tex[i].refs;
        }
    return NULL;
}

int stub_refs(Texture *t) {
    int *r = t ? stub_ref_slot(t) : NULL;
    return r ? *r : 0;
}

void vkr_texture_retain(Texture *t) {
    int *r = t ? stub_ref_slot(t) : NULL;
    if (r) (*r)++;
}

void vkr_texture_release(Texture *t) {
    int *r = t ? stub_ref_slot(t) : NULL;
    if (!r) return;
    if (--*r < 0) stub_bad_releases++;
}

/* Textures the code under test creates (HUD icons): distinct fake pointers with one reference each. */
static char stub_created[8];
static int stub_ncreated;
Texture *vkr_texture_create(int w, int h, const uint8_t *pixels, bool alpha_only) {
    (void)w;
    (void)h;
    (void)pixels;
    (void)alpha_only;
    Texture *t = (Texture *)&stub_created[stub_ncreated++ % 8];
    vkr_texture_retain(t);
    return t;
}

void gfx_grid_offset(int dx, int dy) {
    (void)dx;
    (void)dy;
}
void gfx_out_of_bounds_correction(void) {}
void gfx_set_background_texture(Texture *t) {
    vkr_texture_retain(t);
    vkr_texture_release(stub_background);
    stub_background = t;
}
void gfx_toggle_grid_orientation(void) { gh->isFlat = !gh->isFlat; }

/* Session: the core tests run offline, except where they set stub_client. */
#include "session.h"
bool stub_client;
int stub_sent_ops;
bool session_is_client(void) { return stub_client; }
bool session_is_host(void) { return false; }
const char *session_my_name(void) { return "alice"; }
void session_send_op(const Op *op) {
    (void)op;
    stub_sent_ops++;
}
void session_broadcast_op(const Op *op) { (void)op; }
typedef struct { float x, y, w, h; Color c; } StubFill;
/* Drawing: the HUD draws through these; the tests check layout and input only. Text is 0.5 em per byte. */
void dr_set_alpha(float a) { (void)a; }
StubFill stub_fill_ring[32]; /* the last 32 dr_fill_rect calls */
int stub_fills;               /* how many there were in all */
void dr_fill_rect(float x, float y, float w, float h, Color c) {
    stub_fill_ring[stub_fills++ % 32] = (StubFill){ x, y, w, h, c };
}
void dr_polygon(const Vec2 *pts, int n, float th, Color c) { (void)pts, (void)n, (void)th, (void)c; }
void dr_stroke_rect(float x, float y, float w, float h, float th, Color c) {
    (void)x, (void)y, (void)w, (void)h, (void)th, (void)c;
}
void dr_fill_round_rect(float x, float y, float w, float h, float aw, float ah, Color c) {
    (void)x, (void)y, (void)w, (void)h, (void)aw, (void)ah, (void)c;
}
void dr_fill_oval(float x, float y, float w, float h, Color c) { (void)x, (void)y, (void)w, (void)h, (void)c; }
void dr_stroke_arc(float x, float y, float w, float h, float start, float extent, float th, Color c) {
    (void)x, (void)y, (void)w, (void)h, (void)start, (void)extent, (void)th, (void)c;
}
void dr_line(float x1, float y1, float x2, float y2, float th, Color c) {
    (void)x1, (void)y1, (void)x2, (void)y2, (void)th, (void)c;
}
void dr_image(Texture *t, float x, float y, float w, float h) { (void)t, (void)x, (void)y, (void)w, (void)h; }
void dr_text(FontStyle style, float size, float x, float y, Color c, const char *text) {
    (void)style, (void)size, (void)x, (void)y, (void)c, (void)text;
}
float dr_text_width(FontStyle style, float size, const char *text) {
    (void)style;
    return 0.5f * size * (float)strlen(text);
}
void dr_font_metrics(FontStyle style, float size, float *ascent, float *descent) {
    (void)style;
    *ascent = 0.8f * size;
    *descent = 0.2f * size;
}
Rect console_panel_rect(void) { return (Rect){ 0, gh->winH, gh->winW, 0 }; }
const Theme *gfx_theme(void) { return &gh->themes[gh->currentTheme]; } /* no fade in tests */

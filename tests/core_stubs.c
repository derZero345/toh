/* Stand-ins for the renderer, console and graphics functions that the game core calls,
 * so entity.c, game.c, hex.c, astar.c and ops.c can be tested without a window. */
#include "app.h"

#include <stdarg.h>
#include <stdio.h>

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

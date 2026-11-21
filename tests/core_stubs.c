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

void vkr_texture_retain(Texture *t) { (void)t; }
void vkr_texture_release(Texture *t) { (void)t; }

void gfx_grid_offset(int dx, int dy) {
    (void)dx;
    (void)dy;
}
void gfx_out_of_bounds_correction(void) {}
void gfx_set_background_texture(Texture *t) { stub_background = t; }
void gfx_toggle_grid_orientation(void) { gh->isFlat = !gh->isFlat; }

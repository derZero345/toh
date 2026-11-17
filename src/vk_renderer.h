#ifndef PF_VK_RENDERER_H
#define PF_VK_RENDERER_H

#include <SDL3/SDL.h>
#include "util.h"

/*
 * Minimal Vulkan 2D backend: one pipeline that draws textured, vertex-coloured
 * triangles in logical window coordinates, with MSAA for anti-aliasing.
 */

typedef struct Texture Texture;

typedef struct {
    float x, y;     /* logical window coordinates */
    float u, v;
    uint32_t color; /* R8G8B8A8, little endian (r in the low byte) */
} Vertex;

typedef struct {
    Texture *tex;
    int first, count;           /* vertex range */
    bool scissor;
    float sx, sy, sw, sh;       /* logical scissor rect */
} DrawCmd;

bool vkr_init(SDL_Window *window);
void vkr_shutdown(void);

/* Call when the window was resized. */
void vkr_resize(void);

/* Creates a texture. alpha_only => one byte per pixel, sampled as (1,1,1,a). */
Texture *vkr_texture_create(int w, int h, const uint8_t *pixels, bool alpha_only);
/* Re-uploads rows [y0, y1) of the texture (stalls the GPU; use sparingly). */
void vkr_texture_update(Texture *t, const uint8_t *pixels, int y0, int y1);
void vkr_texture_retain(Texture *t);
void vkr_texture_release(Texture *t);
int  vkr_texture_width(const Texture *t);
int  vkr_texture_height(const Texture *t);

/* Renders and presents one frame. `clear` is RGBA in [0,1].
 * If screenshot_path is non-NULL the presented image is also saved as PNG. */
void vkr_render(const Vertex *verts, int nverts, const DrawCmd *cmds, int ncmds,
                const float clear[4], float logical_w, float logical_h,
                const char *screenshot_path);

#endif

#include "image.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

bool image_load_png(const char *path, Image *out) {
    SDL_Surface *s = SDL_LoadPNG(path);
    if (!s) {
        fprintf(stderr, "Cannot load '%s': %s\n", path, SDL_GetError());
        return false;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!rgba) {
        fprintf(stderr, "Cannot convert '%s': %s\n", path, SDL_GetError());
        return false;
    }
    out->w = rgba->w;
    out->h = rgba->h;
    out->px = malloc((size_t)out->w * out->h * 4);
    if (!out->px) abort();
    for (int y = 0; y < out->h; y++)
        memcpy(out->px + (size_t)y * out->w * 4, (uint8_t *)rgba->pixels + (size_t)y * rgba->pitch,
               (size_t)out->w * 4);
    SDL_DestroySurface(rgba);
    return out->w > 0 && out->h > 0;
}

void image_free(Image *img) {
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}

static Image image_alloc(int w, int h) {
    Image o = { w, h, calloc((size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1), 4) };
    if (!o.px) abort();
    return o;
}

Image image_cut_circle(const Image *src) {
    int d = src->w < src->h ? src->w : src->h;
    Image out = image_alloc(d, d);
    int r = d / 2;
    for (int y = 0; y < d; y++) {
        for (int x = 0; x < d; x++) {
            double dist = sqrt((double)(x - r) * (x - r) + (double)(y - r) * (y - r));
            if (dist <= r)
                memcpy(out.px + ((size_t)y * d + x) * 4, src->px + ((size_t)y * src->w + x) * 4, 4);
        }
    }
    return out;
}

/* Point-in-polygon test (even-odd), same rule as java.awt.geom.Path2D.contains. */
static bool poly_contains(const Vec2 *p, int n, double x, double y) {
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i].y > y) != (p[j].y > y)) &&
            (x < (p[j].x - p[i].x) * (y - p[i].y) / (p[j].y - p[i].y) + p[i].x))
            in = !in;
    }
    return in;
}

Image image_cut_hex(const Image *src, bool flat) {
    int hex_size = src->h / 2;
    double cx = hex_size * 2 / SQRT3, cy = hex_size;
    int radius = (int)(hex_size * 2 / SQRT3);
    Vec2 poly[6];
    for (int i = 0; i < 6; i++) {
        double a = 2 * M_PI / 6 * i + (flat ? 0 : M_PI / 6);
        poly[i] = (Vec2){ cx + radius * cos(a), cy + radius * sin(a) };
    }
    int w = (int)(hex_size * 4 / SQRT3), h = hex_size * 2;
    Image out = image_alloc(w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (x >= src->w || y >= src->h) continue;
            if (poly_contains(poly, 6, x, y))
                memcpy(out.px + ((size_t)y * w + x) * 4, src->px + ((size_t)y * src->w + x) * 4, 4);
        }
    }
    return out;
}

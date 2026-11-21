#ifndef TOH_IMAGE_H
#define TOH_IMAGE_H

#include "util.h"

/* Tightly packed 8-bit RGBA image. */
typedef struct {
    int w, h;
    uint8_t *px;
} Image;

bool image_load_png(const char *path, Image *out);
void image_free(Image *img);

/* Port of Calc.cutCirce: keeps a circle of the top-left min(w,h) square. */
Image image_cut_circle(const Image *src);
/* Port of Calc.cutHex: keeps a hexagon sized by the image height. */
Image image_cut_hex(const Image *src, bool flat);

#endif

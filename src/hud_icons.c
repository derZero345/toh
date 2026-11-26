/* HUD icon registry: icons are referred to by key everywhere else, so swapping the set means changing this table.
 * Files live in resources/images/hud/. */
#include "hud.h"
#include "image.h"

#include <stdio.h>
#include <string.h>

static const struct { const char *key, *file; } ICONS[] = {
    { "placeholder", "placeholder.png" },
};
#define NICONS ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static Texture *textures[NICONS];

void hud_icons_init(const char *resource_dir) {
    for (int i = 0; i < NICONS; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/images/hud/%s", resource_dir, ICONS[i].file);
        Image img;
        if (!image_load_png(path, &img)) {
            fprintf(stderr, "HUD icon missing: %s\n", path);
            continue;
        }
        textures[i] = vkr_texture_create(img.w, img.h, img.px, false);
        image_free(&img);
    }
}

void hud_icons_free(void) {
    for (int i = 0; i < NICONS; i++) {
        vkr_texture_release(textures[i]);
        textures[i] = NULL;
    }
}

Texture *hud_icon(const char *key) {
    for (int i = 0; i < NICONS; i++)
        if (strcmp(ICONS[i].key, key) == 0) return textures[i];
    return NULL;
}

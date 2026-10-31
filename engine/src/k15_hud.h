// SPDX-License-Identifier: GPL-2.0-or-later
// K15 widescreen HUD: draws DOOM's own status graphics (tall digits, face, keys, HU font)
// at arbitrary screen positions through a put-pixel callback.
#ifndef K15_HUD_H
#define K15_HUD_H

#include <stdint.h>

typedef void (*k15_put_fn)(int x, int y, uint32_t rgb, int alpha);

typedef struct {
    int in_level;
    int health, armor, ammo, maxammo, keys;
    const void *face;
} k15_hud_t;

void K15_HudGet(k15_hud_t *h);
void K15_HudDraw(const k15_hud_t *h, k15_put_fn put, int screen_w);
void K15_HudFps(k15_put_fn put, int screen_w, int screen_h, int fps);

// HU-font text; scale is in halves (2 = 1x, 3 = 1.5x, 4 = 2x).
int K15_TextWidth(const char *s, int scale);
void K15_DrawText(k15_put_fn put, int x, int y, const char *s, int scale, uint32_t rgb,
                  int alpha);
int K15_TextHeight(int scale);

#endif

// SPDX-License-Identifier: GPL-2.0-or-later
// Tiny software renderer for the K15 DOOM home menu: a 1020x240 landscape 0xRRGGBB
// canvas, an 8x8 public-domain bitmap font (scaled), and the anti-aliased DOOM wordmark.
#pragma once
#include <stdint.h>

#include "k15_plat.h"

extern uint32_t k15_canvas[K15_SCR_W * K15_SCR_H];

void G_Clear(uint32_t rgb);
void G_VGradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void G_Fill(int x, int y, int w, int h, uint32_t rgb);
void G_Blend(int x, int y, int w, int h, uint32_t rgb, int alpha);
void G_Frame(int x, int y, int w, int h, int t, uint32_t rgb);
void G_Put(int x, int y, uint32_t rgb, int alpha);
// k15_icons.h IC_* icon (16x16) at scale; outline draws only its edge.
void G_Icon(int x, int y, int icon, int scale, uint32_t rgb, int outline);
void G_Triangle(int cx, int cy, int size, int up, uint32_t rgb);

int G_TextW(const char *s, int scale);
int G_TextH(int scale);
// Draws s (ASCII) with its top-left at (x, y), clipped to max_w pixels if max_w > 0
// (with ".." when cut). Returns the drawn width.
int G_Text(int x, int y, const char *s, int scale, uint32_t rgb, int max_w);
void G_TextA(int x, int y, const char *s, int scale, uint32_t rgb, int alpha); // no clipping
void G_TextShadow(int x, int y, const char *s, int scale, uint32_t rgb);
void G_TextCentre(int cx, int y, const char *s, int scale, uint32_t rgb);

// DOOM wordmark of height h at (x, y), filled with a vertical top->bottom gradient.
// Returns its width.
int G_Logo(int x, int y, int h, uint32_t top, uint32_t bottom, int alpha);
int G_LogoWidth(int h);

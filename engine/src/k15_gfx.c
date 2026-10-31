// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>
#include <string.h>

#include "font8x8_basic.h"
#include "k15_gfx.h"
#include "k15_icons.h"
#include "k15_logo.h"

uint32_t k15_canvas[K15_SCR_W * K15_SCR_H];

static inline uint32_t mix(uint32_t a, uint32_t b, int t) // t: 0 -> a, 255 -> b
{
    int r = ((a >> 16) & 255) + ((((int)(b >> 16) & 255) - ((int)(a >> 16) & 255)) * t) / 255;
    int g = ((a >> 8) & 255) + ((((int)(b >> 8) & 255) - ((int)(a >> 8) & 255)) * t) / 255;
    int bl = (a & 255) + ((((int)b & 255) - ((int)a & 255)) * t) / 255;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static inline void put(int x, int y, uint32_t rgb, int alpha)
{
    if ((unsigned)x >= K15_SCR_W || (unsigned)y >= K15_SCR_H || alpha <= 0)
        return;
    uint32_t *p = &k15_canvas[y * K15_SCR_W + x];
    *p = alpha >= 255 ? rgb : mix(*p, rgb, alpha);
}

void G_Put(int x, int y, uint32_t rgb, int alpha)
{
    put(x, y, rgb, alpha);
}

void G_Clear(uint32_t rgb)
{
    for (int i = 0; i < K15_SCR_W * K15_SCR_H; i++)
        k15_canvas[i] = rgb;
}

void G_VGradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int j = 0; j < h; j++)
        G_Fill(x, y + j, w, 1, mix(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}

void G_Blend(int x, int y, int w, int h, uint32_t rgb, int alpha)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > K15_SCR_W ? K15_SCR_W : x + w, y1 = y + h > K15_SCR_H ? K15_SCR_H : y + h;
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            put(i, j, rgb, alpha);
}

void G_Fill(int x, int y, int w, int h, uint32_t rgb)
{
    G_Blend(x, y, w, h, rgb, 255);
}

void G_Frame(int x, int y, int w, int h, int t, uint32_t rgb)
{
    G_Fill(x, y, w, t, rgb);
    G_Fill(x, y + h - t, w, t, rgb);
    G_Fill(x, y + t, t, h - 2 * t, rgb);
    G_Fill(x + w - t, y + t, t, h - 2 * t, rgb);
}

void G_Triangle(int cx, int cy, int size, int up, uint32_t rgb)
{
    for (int j = 0; j < size; j++) {
        int half = up ? j : size - 1 - j;
        G_Fill(cx - half, cy - size / 2 + j, 2 * half + 1, 1, rgb);
    }
}

// 16x16 icon at scale; outline: only the edge pixels of the shape (e.g. an empty star).
void G_Icon(int x, int y, int icon, int scale, uint32_t rgb, int outline)
{
    if (icon < 0 || icon >= IC_N)
        return;
    const char *const *rows = k15_icons[icon];
#define SET(c, r) ((r) >= 0 && (r) < 16 && (c) >= 0 && (c) < 16 && rows[r][c] == 'X')
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16; c++) {
            if (!SET(c, r))
                continue;
            if (outline && SET(c - 1, r) && SET(c + 1, r) && SET(c, r - 1) && SET(c, r + 1))
                continue;
            G_Fill(x + c * scale, y + r * scale, scale, scale, rgb);
        }
#undef SET
}

// ---------------------------------------------------------------- text
int G_TextH(int scale)
{
    return 8 * scale;
}

int G_TextW(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}

static void glyph_a(int x, int y, unsigned char c, int scale, uint32_t rgb, int alpha)
{
    if (c >= 128)
        c = '?';
    for (int r = 0; r < 8; r++) {
        unsigned bits = font8x8_basic[c][r];
        for (int b = 0; b < 8; b++)
            if (bits & (1u << b))
                G_Blend(x + b * scale, y + r * scale, scale, scale, rgb, alpha);
    }
}

static void glyph(int x, int y, unsigned char c, int scale, uint32_t rgb)
{
    glyph_a(x, y, c, scale, rgb, 255);
}

void G_TextA(int x, int y, const char *s, int scale, uint32_t rgb, int alpha)
{
    for (; *s; s++, x += 8 * scale)
        glyph_a(x, y, (unsigned char)*s, scale, rgb, alpha);
}

int G_Text(int x, int y, const char *s, int scale, uint32_t rgb, int max_w)
{
    int n = (int)strlen(s), cw = 8 * scale;
    int fit = max_w > 0 ? max_w / cw : n;
    int cut = n > fit;
    int show = cut ? (fit > 2 ? fit - 2 : 0) : n;
    for (int i = 0; i < show; i++)
        glyph(x + i * cw, y, (unsigned char)s[i], scale, rgb);
    if (cut) {
        glyph(x + show * cw, y, '.', scale, rgb);
        glyph(x + (show + 1) * cw, y, '.', scale, rgb);
        show += 2;
    }
    return show * cw;
}

void G_TextShadow(int x, int y, const char *s, int scale, uint32_t rgb)
{
    int o = scale > 2 ? scale / 2 + 1 : 1;
    G_Text(x + o, y + o, s, scale, 0x000000, 0);
    G_Text(x, y, s, scale, rgb, 0);
}

void G_TextCentre(int cx, int y, const char *s, int scale, uint32_t rgb)
{
    G_TextShadow(cx - G_TextW(s, scale) / 2, y, s, scale, rgb);
}

// ---------------------------------------------------------------- logo
int G_LogoWidth(int h)
{
    return h * K15_LOGO_W / K15_LOGO_H;
}

static int cmp_float(const void *a, const void *b)
{
    float d = *(const float *)a - *(const float *)b;
    return (d > 0) - (d < 0);
}

// Even-odd scanline fill with 4 vertical sub-samples and exact horizontal coverage.
int G_Logo(int x, int y, int h, uint32_t top, uint32_t bottom, int alpha)
{
    enum { SUB = 4, MAXX = 64 };
    int w = G_LogoWidth(h);
    float scale = (float)h / K15_LOGO_H;
    float *cov = calloc((size_t)w + 2, sizeof *cov);
    if (!cov)
        return w;
    for (int row = 0; row < h; row++) {
        memset(cov, 0, ((size_t)w + 2) * sizeof *cov);
        for (int s = 0; s < SUB; s++) {
            float ly = (row + (s + 0.5f) / SUB) / scale;
            float xs[MAXX];
            int nx = 0, base = 0;
            for (int p = 0; p < K15_LOGO_NPOLY; p++) {
                int n = k15_logo_len[p];
                for (int i = 0; i < n; i++) {
                    const short *a = k15_logo_pts[base + i];
                    const short *b = k15_logo_pts[base + (i + 1) % n];
                    if ((a[1] <= ly) == (b[1] <= ly) || nx >= MAXX)
                        continue;
                    float t = (ly - a[1]) / (float)(b[1] - a[1]);
                    xs[nx++] = (a[0] + t * (b[0] - a[0])) * scale;
                }
                base += n;
            }
            qsort(xs, nx, sizeof xs[0], cmp_float);
            for (int i = 0; i + 1 < nx; i += 2) {
                float x0 = xs[i], x1 = xs[i + 1];
                int i0 = (int)x0, i1 = (int)x1;
                if (i0 < 0 || i1 > w)
                    continue;
                if (i0 == i1) {
                    cov[i0] += (x1 - x0) / SUB;
                    continue;
                }
                cov[i0] += (i0 + 1 - x0) / SUB;
                for (int k = i0 + 1; k < i1; k++)
                    cov[k] += 1.0f / SUB;
                cov[i1] += (x1 - i1) / SUB;
            }
        }
        uint32_t c = mix(top, bottom, h > 1 ? row * 255 / (h - 1) : 0);
        for (int k = 0; k <= w; k++) {
            float v = cov[k] > 1 ? 1 : cov[k];
            if (v > 0)
                put(x + k, y + row, c, (int)(v * alpha));
        }
    }
    free(cov);
    return w;
}

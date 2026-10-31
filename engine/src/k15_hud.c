// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <string.h>

#include "d_items.h"
#include "doomstat.h"
#include "i_swap.h"
#include "v_patch.h"
#include "w_wad.h"
#include "z_zone.h"

#include "k15_hud.h"

patch_t *ST_GetFacePatch(void);

static uint32_t pal[256];
static int pal_loaded;

static void load_palette(void)
{
    const byte *p = W_CacheLumpName("PLAYPAL", PU_STATIC);
    for (int i = 0; i < 256; i++)
        pal[i] = (p[i * 3] << 16) | (p[i * 3 + 1] << 8) | p[i * 3 + 2];
    pal_loaded = 1;
}

static patch_t *lump(const char *name)
{
    int n = W_CheckNumForName((char *)name);
    return n < 0 ? NULL : W_CacheLumpNum(n, PU_STATIC);
}

// Draw a patch at (x, y), top-left anchored, scaled by scale/2. If tint is nonzero the
// patch is recoloured by its brightness (used for text).
static int draw_patch(k15_put_fn put, int x, int y, const patch_t *p, int scale,
                      uint32_t tint, int alpha)
{
    if (!p)
        return 0;
    int w = SHORT(p->width), h = SHORT(p->height);
    int dw = w * scale / 2, dh = h * scale / 2;
    short col[256];
    for (int dx = 0; dx < dw; dx++) {
        int sx = dx * 2 / scale;
        for (int i = 0; i < h && i < 256; i++)
            col[i] = -1;
        const column_t *c = (const column_t *)((const byte *)p + LONG(p->columnofs[sx]));
        while (c->topdelta != 0xff) {
            const byte *src = (const byte *)c + 3;
            for (int i = 0; i < c->length && c->topdelta + i < 256; i++)
                col[c->topdelta + i] = src[i];
            c = (const column_t *)((const byte *)c + c->length + 4);
        }
        for (int dy = 0; dy < dh; dy++) {
            int sy = dy * 2 / scale;
            if (sy >= 256 || col[sy] < 0)
                continue;
            uint32_t rgb = pal[col[sy]];
            if (tint) {
                int r = rgb >> 16, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
                int v = r > g ? (r > b ? r : b) : (g > b ? g : b);
                rgb = ((((tint >> 16) & 0xFF) * v / 255) << 16) |
                      ((((tint >> 8) & 0xFF) * v / 255) << 8) | ((tint & 0xFF) * v / 255);
            }
            put(x + dx, y + dy, rgb, alpha);
        }
    }
    return dw;
}

static patch_t *hu_char(char ch)
{
    char name[9];
    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    if (ch < '!' || ch > '_')
        return NULL;
    snprintf(name, sizeof name, "STCFN%.3d", ch);
    return lump(name);
}

int K15_TextHeight(int scale)
{
    return 7 * scale / 2;
}

int K15_TextWidth(const char *s, int scale)
{
    if (!pal_loaded)
        load_palette();
    int w = 0;
    for (; *s; s++) {
        patch_t *p = hu_char(*s);
        w += p ? (SHORT(p->width) * scale / 2) : 4 * scale / 2;
    }
    return w;
}

void K15_DrawText(k15_put_fn put, int x, int y, const char *s, int scale, uint32_t rgb,
                  int alpha)
{
    if (!pal_loaded)
        load_palette();
    for (; *s; s++) {
        patch_t *p = hu_char(*s);
        x += p ? draw_patch(put, x, y, p, scale, rgb, alpha) : 4 * scale / 2;
    }
}

// Tall status-bar number ("STTNUMx"), right-aligned at xr. Returns left edge.
static int draw_tallnum(k15_put_fn put, int xr, int y, int n, int percent, int scale)
{
    char name[9];
    int x = xr;
    if (percent) {
        patch_t *p = lump("STTPRCNT");
        x -= SHORT(p->width) * scale / 2;
        draw_patch(put, x, y, p, scale, 0, 255);
    }
    int neg = n < 0;
    if (neg)
        n = -n;
    do {
        snprintf(name, sizeof name, "STTNUM%d", n % 10);
        patch_t *p = lump(name);
        x -= SHORT(p->width) * scale / 2;
        draw_patch(put, x, y, p, scale, 0, 255);
        n /= 10;
    } while (n);
    if (neg) {
        patch_t *p = lump("STTMINUS");
        x -= SHORT(p->width) * scale / 2;
        draw_patch(put, x, y, p, scale, 0, 255);
    }
    return x;
}

static void plate(k15_put_fn put, int x, int y, int w, int h)
{
    for (int i = 0; i < w; i++)
        for (int j = 0; j < h; j++)
            put(x + i, y + j, 0x000000, 110);
}

void K15_HudGet(k15_hud_t *h)
{
    memset(h, 0, sizeof *h);
    h->in_level = gamestate == GS_LEVEL && playeringame[consoleplayer];
    if (!h->in_level)
        return;
    player_t *pl = &players[consoleplayer];
    h->health = pl->health;
    h->armor = pl->armorpoints;
    ammotype_t a = weaponinfo[pl->readyweapon].ammo;
    h->ammo = a == am_noammo ? -1 : pl->ammo[a];
    h->maxammo = a == am_noammo ? 0 : pl->maxammo[a];
    for (int k = 0; k < NUMCARDS; k++)
        if (pl->cards[k])
            h->keys |= 1 << k;
    h->face = ST_GetFacePatch();
}

#define LABEL_RGB 0xE0E0E0

void K15_HudFps(k15_put_fn put, int screen_w, int screen_h, int fps)
{
    if (!pal_loaded)
        load_palette();
    // bottom-right, styled like the AMMO plate: label + 1.5x tall digits
    int y = screen_h - 32;
    plate(put, screen_w - 112, y - 2, 108, 32);
    K15_DrawText(put, screen_w - 106, y + 9, "FPS", 2, LABEL_RGB, 230);
    draw_tallnum(put, screen_w - 10, y + 2, fps, 0, 3);
}

void K15_HudDraw(const k15_hud_t *h, k15_put_fn put, int screen_w)
{
    if (!h->in_level)
        return;
    if (!pal_loaded)
        load_palette();
    const int ns = 3; // 1.5x tall digits (21x24)
    const int ny = 15;

    // top-left: face, HEALTH, ARMOR
    plate(put, 4, 2, 258, 46);
    if (h->face)
        draw_patch(put, 10, 4, h->face, 3, 0, 255);
    K15_DrawText(put, 60, 5, "HEALTH", 2, LABEL_RGB, 230);
    draw_tallnum(put, 152, ny, h->health, 1, ns);
    K15_DrawText(put, 168, 5, "ARMOR", 2, LABEL_RGB, 230);
    draw_tallnum(put, 256, ny, h->armor, 1, ns);

    // top-right: keys, AMMO
    plate(put, screen_w - 200, 2, 196, 46);
    if (h->ammo >= 0) {
        int w = K15_TextWidth("AMMO", 2);
        K15_DrawText(put, screen_w - 12 - w, 5, "AMMO", 2, LABEL_RGB, 230);
        draw_tallnum(put, screen_w - 10, ny, h->ammo, 0, ns);
    }
    char name[9];
    for (int k = 0; k < NUMCARDS; k++) {
        if (!(h->keys & (1 << k)))
            continue;
        snprintf(name, sizeof name, "STKEYS%d", k);
        int col = k % 3, row = k / 3;
        draw_patch(put, screen_w - 192 + col * 20, 8 + row * 18, lump(name), 4, 0, 255);
    }
}

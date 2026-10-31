// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>

#include "k15_cfg.h"
#include "k15_touch.h"

#define TOP_Y 52
#define ESC_HOLD_SHOW_MS 300

static const uint32_t pill_rgb[TB_NPILL] = {0x909090, 0x4070E0, 0x30B0B0, 0xD0A020};
static const char *const pill_label[TB_NPILL] = {"ESC", "ENTER", "MAP", "WPN"};

static const k15_layout_t presets[LAYOUT_N] = {
    // Joystick left, FIRE/USE right, pills top centre (the original layout).
    [LAYOUT_CLASSIC] = {165, 148, 80, 0, 340,
                        {{340, 6, 78, 34}, {426, 6, 84, 34}, {518, 6, 78, 34}, {604, 6, 78, 34}},
                        {918, 150, 72, 92}, {768, 188, 44, 56}, 340, 690},
    // Mirrored: joystick right, FIRE/USE left.
    [LAYOUT_LEFTY] = {855, 148, 80, 680, 1020,
                      {{338, 6, 78, 34}, {424, 6, 84, 34}, {516, 6, 78, 34}, {602, 6, 78, 34}},
                      {102, 150, 72, 92}, {252, 188, 44, 56}, 330, 680},
    // Joystick + FIRE + ESC only; tap the view to USE.
    [LAYOUT_MINIMAL] = {165, 148, 80, 0, 340, {{340, 6, 78, 34}},
                        {918, 150, 72, 92}, {0, 0, 0, 0}, 340, 830},
    // Larger targets everywhere.
    [LAYOUT_BIG] = {185, 140, 95, 0, 380,
                    {{380, 4, 90, 44}, {478, 4, 100, 44}, {586, 4, 90, 44}, {684, 4, 90, 44}},
                    {915, 148, 88, 104}, {742, 184, 54, 66}, 380, 690},
};

const k15_layout_t *K15_TouchLayout(void)
{
    int l = K15_CfgInt(CFG_LAYOUT);
    return &presets[l >= 0 && l < LAYOUT_N ? l : 0];
}

uint32_t K15_TouchHit(int x, int y)
{
    const k15_layout_t *L = K15_TouchLayout();
    for (int b = 0; b < TB_NPILL; b++)
        if (L->pill[b].w && x >= L->pill[b].x - 4 && x < L->pill[b].x + L->pill[b].w + 4 &&
            y >= 0 && y < L->pill[b].y + L->pill[b].h + 6)
            return 1u << b;
    if (x >= L->joy_x0 && x < L->joy_x1 && y >= TOP_Y - 12) {
        int dx = x - L->joy_cx, dy = y - L->joy_cy, dead = K15_CfgInt(CFG_JOY_DEAD);
        uint32_t m = 0;
        // 2:1 slope so a mostly-vertical push doesn't also turn
        if (dy < -dead && -dy * 2 > abs(dx)) m |= 1u << TB_UP;
        if (dy > dead && dy * 2 > abs(dx)) m |= 1u << TB_DOWN;
        if (dx < -dead && -dx * 2 > abs(dy)) m |= 1u << TB_LEFT;
        if (dx > dead && dx * 2 > abs(dy)) m |= 1u << TB_RIGHT;
        return m;
    }
    if (L->fire.r) {
        int dx = x - L->fire.cx, dy = y - L->fire.cy;
        if (dx * dx + dy * dy <= L->fire.hit_r * L->fire.hit_r)
            return 1u << TB_FIRE;
    }
    if (L->use.r) {
        int dx = x - L->use.cx, dy = y - L->use.cy;
        if (dx * dx + dy * dy <= L->use.hit_r * L->use.hit_r)
            return 1u << TB_USE;
    }
    if (K15_CfgInt(CFG_TAP_USE) && x >= L->game_x0 && x < L->game_x1 && y >= TOP_Y)
        return 1u << TB_GAME;
    return 0;
}

// ---------------------------------------------------------------- drawing
static k15_put_fn put;
static int opacity; // percent of the original look, 20..200

static int A(int a) // apply the transparency setting
{
    a = a * opacity / 100;
    return a > 255 ? 255 : a;
}

static void rect(int x, int y, int w, int h, uint32_t c, int a)
{
    for (int i = x; i < x + w; i++)
        for (int j = y; j < y + h; j++)
            put(i, j, c, a);
}

static void frame(int x, int y, int w, int h, int t, uint32_t c, int a)
{
    rect(x, y, w, t, c, a);
    rect(x, y + h - t, w, t, c, a);
    rect(x, y + t, t, h - 2 * t, c, a);
    rect(x + w - t, y + t, t, h - 2 * t, c, a);
}

// Filled disc (fill_a) with a ring of thickness t (ring_a).
static void disc(int cx, int cy, int r, int t, uint32_t c, int fill_a, int ring_a)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r)
                continue;
            put(cx + dx, cy + dy, c, d2 >= (r - t) * (r - t) ? ring_a : fill_a);
        }
}

void K15_TouchDraw(k15_put_fn put_fn, k15_label_fn label, uint32_t held, uint32_t flash,
                   uint32_t esc_ms)
{
    const k15_layout_t *L = K15_TouchLayout();
    put = put_fn;
    opacity = (100 - K15_CfgInt(CFG_TRANSPARENCY)) * 2;
    if (!K15_CfgInt(CFG_FLASH))
        flash = 0;

    // joystick
    int cx = L->joy_cx, cy = L->joy_cy, r = L->joy_r;
    disc(cx, cy, r, 3, 0xFFFFFF, A(28), A(110));
    disc(cx, cy, K15_CfgInt(CFG_JOY_DEAD), 0, 0xFFFFFF, A(40), A(40));
    static const struct { int b, dx, dy; } arrows[] = {
        {TB_UP, 0, -1}, {TB_DOWN, 0, 1}, {TB_LEFT, -1, 0}, {TB_RIGHT, 1, 0}};
    for (int a = 0; a < 4; a++) {
        uint32_t bit = 1u << arrows[a].b;
        int alpha = flash & bit ? 255 : A(held & bit ? 230 : 110);
        for (int k = 0; k < 16; k++) {
            int along = r - 14 - k;
            int ax = cx + arrows[a].dx * along, ay = cy + arrows[a].dy * along;
            if (arrows[a].dx)
                rect(ax, ay - k, 1, 2 * k + 1, 0xFFFFFF, alpha);
            else
                rect(ax - k, ay, 2 * k + 1, 1, 0xFFFFFF, alpha);
        }
    }

    for (int b = 0; b < TB_NPILL; b++) {
        if (!L->pill[b].w)
            continue;
        int x = L->pill[b].x, y = L->pill[b].y, w = L->pill[b].w, h = L->pill[b].h;
        uint32_t bit = 1u << b;
        const char *text = pill_label[b];
        rect(x, y, w, h, pill_rgb[b], A(held & bit ? 170 : 55));
        if (flash & bit)
            rect(x, y, w, h, 0xFFFFFF, 160);
        uint32_t hold = K15_CfgInt(CFG_ESC_HOLD);
        if (b == TB_ESC && esc_ms >= ESC_HOLD_SHOW_MS && hold > ESC_HOLD_SHOW_MS) {
            // hold-to-exit progress
            int fill = (int)((w - 4) * (esc_ms - ESC_HOLD_SHOW_MS) / (hold - ESC_HOLD_SHOW_MS));
            rect(x + 2, y + 2, fill > w - 4 ? w - 4 : fill, h - 4, 0xE02010, 220);
            text = "EXIT";
        }
        frame(x, y, w, h, 2, pill_rgb[b], A(170));
        label(x + w / 2, y + h / 2, text, 0, A(230));
    }
    const struct { int b, size; uint32_t rgb; const char *text; } ds[] = {
        {TB_FIRE, 2, 0xE03030, "FIRE"}, {TB_USE, 1, 0x30C050, "USE"}};
    for (int d = 0; d < 2; d++) {
        int dcx = d ? L->use.cx : L->fire.cx, dcy = d ? L->use.cy : L->fire.cy;
        int dr = d ? L->use.r : L->fire.r;
        uint32_t bit = 1u << ds[d].b;
        if (!dr)
            continue;
        disc(dcx, dcy, dr, 3, ds[d].rgb, A(held & bit ? 150 : 50), A(180));
        if (flash & bit)
            disc(dcx, dcy, dr, 0, 0xFFFFFF, 150, 150);
        label(dcx, dcy, ds[d].text, ds[d].size, A(230));
    }
}

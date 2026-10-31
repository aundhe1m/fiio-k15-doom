// SPDX-License-Identifier: GPL-2.0-or-later
// In-game touch controls: layout presets (k15.conf K15_LAYOUT etc.), hit testing and drawing
// through put-pixel / label callbacks, so the game overlay and the menu's layout preview
// share one implementation.
#pragma once
#include <stdint.h>

#include "k15_hud.h" // k15_put_fn

enum { TB_ESC, TB_ENTER, TB_MAP, TB_WPN, TB_USE, TB_FIRE, TB_UP, TB_DOWN, TB_LEFT, TB_RIGHT,
       TB_GAME, TB_N };
#define TB_NPILL 4 // TB_ESC..TB_WPN

typedef struct {
    int joy_cx, joy_cy, joy_r, joy_x0, joy_x1; // joystick and its touch zone (x range)
    struct { int x, y, w, h; } pill[TB_NPILL]; // w = 0: not in this layout
    struct { int cx, cy, r, hit_r; } fire, use; // r = 0: not in this layout
    int game_x0, game_x1;                       // tap = USE zone (if enabled)
} k15_layout_t;

// size: 0 pill, 1 USE disc, 2 FIRE disc
typedef void (*k15_label_fn)(int cx, int cy, const char *s, int size, int alpha);

const k15_layout_t *K15_TouchLayout(void); // the configured preset (re-read every call)
uint32_t K15_TouchHit(int x, int y);       // bit mask of TB_* under a finger
// held: TB_* mask lit; flash: mask drawn with a bright press flash; esc_ms: how long ESC
// has been held (shows the hold-to-exit bar).
void K15_TouchDraw(k15_put_fn put, k15_label_fn label, uint32_t held, uint32_t flash,
                   uint32_t esc_ms);

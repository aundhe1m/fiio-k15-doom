// SPDX-License-Identifier: GPL-2.0-or-later
// doomgeneric backend for the FiiO K15 (Ingenic X2000, ingenicfb), widescreen edition.
//
// The engine renders 1020x200 "Hor+" (DOOM pixels are 1.2x tall), shown 1:1 x 1.2 on the
// 1020x240 landscape panel through k15_plat (fb0 pages 1-2, grabbed input). A translucent
// overlay (HUD from DOOM's own graphics + touch controls) is composited every frame.
//
// Process flow:
//   doomk15                -> DOOM home menu (k15_menu.c), then the game in-process
//   doomk15 -iwad X ...    -> straight into the game (adb/dev use)
//   doomk15 --restore      -> only pan the display back to the stock UI (after a kill -9)
// Quitting a game started from the menu execv()s back into the menu, handing the grabbed
// input fds over, so the stock UI never flashes or receives a tap in between.
// Holding the ESC pill (3 s by default) quits straight back to the stock UI (config is saved).
// Settings come from k15.conf (k15_cfg.c), which the home menu edits.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Capture evdev codes that collide with doomkeys.h names before dropping them.
enum {
    EV_K_ENTER = KEY_ENTER, EV_K_KPENTER = KEY_KPENTER, EV_K_TAB = KEY_TAB,
    EV_K_BACKSPACE = KEY_BACKSPACE, EV_K_MINUS = KEY_MINUS, EV_K_EQUAL = KEY_EQUAL,
    EV_K_F1 = KEY_F1, EV_K_F10 = KEY_F10,
};
#undef KEY_ENTER
#undef KEY_TAB
#undef KEY_BACKSPACE
#undef KEY_MINUS
#undef KEY_EQUALS
#undef KEY_PAUSE
#undef KEY_HOME
#undef KEY_END
#undef KEY_CAPSLOCK
#undef KEY_NUMLOCK
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12

#include "d_event.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "i_system.h"
#include "k15_cfg.h"
#include "k15_hud.h"
#include "k15_menu.h"
#include "k15_plat.h"
#include "k15_sound.h"
#include "k15_touch.h"
#include "k15_wad.h"
#include "m_controls.h"
#include "w_wad.h"

#define SCR_W K15_SCR_W
#define SCR_H K15_SCR_H
#define NPIX K15_NPIX
#define ESC_HOLD_SHOW_MS 300

// Keys we bind for the pad's LB/RB when the config leaves prev/next weapon unbound.
#define DK_PREVWEAPON 0xa4
#define DK_NEXTWEAPON 0xa5
// Pad analog: stick deadzone, ev_mouse turn (angleturn -= x*8; keyboard fast turn = 160)
// and forward (forward += y; MAXPLMOVE = 50) at full deflection.
#define PAD_DEAD 8000
#define PAD_TURN_MAX 200
#define PAD_MOVE_MAX 50

extern int messageToPrint;

static uint16_t xmap[SCR_W], ymap[SCR_H];
static int from_menu;    // game was started by the home menu: quitting returns there
static int exit_to_ui;   // ESC hold / menu Exit: leave to the stock UI
static char err_msg[600];

// Overlay in panel order (same index as the framebuffer): premultiplied colour + alpha.
static uint32_t ov_rgb[NPIX];
static uint8_t ov_a[NPIX];

// ---------------------------------------------------------------- key queue
// Entry: pressed << 16 | typed char << 8 | doom key. A typed char overrides the
// character DOOM derives from the key (WASD move, but still type "iddqd").
#define KQ 64
static uint32_t kq[KQ];
static unsigned kq_w, kq_r;
static int last_typed = -1;

static void push_key_typed(int pressed, unsigned char key, unsigned char typed)
{
    if (!key)
        return;
    kq[kq_w] = (uint32_t)pressed << 16 | typed << 8 | key;
    kq_w = (kq_w + 1) % KQ;
}

static void push_key(int pressed, unsigned char key)
{
    push_key_typed(pressed, key, 0);
}

// Called by the patched I_GetEvent() for the key DG_GetKey() just returned.
int K15_TypedChar(void)
{
    return last_typed;
}

// ---------------------------------------------------------------- exit paths
static void on_signal(int sig)
{
    K15_SoundRestore();
    K15_FbRestore();
    _exit(128 + sig);
}

// Called by the patched I_Error() with the engine's message.
void K15_SetError(const char *msg)
{
    snprintf(err_msg, sizeof err_msg, "DOOM STOPPED WITH AN ERROR\n%s", msg);
}

static void back_to_menu(void)
{
    // Let the finger come up first so the release lands on our grabbed fds.
    uint32_t t0 = K15_NowMs();
    K15_InputPump(NULL, NULL, NULL);
    while (K15_TouchDown() && K15_NowMs() - t0 < 2000) {
        K15_InputWait(50);
        K15_InputPump(NULL, NULL, NULL);
    }
    K15_InputPrepareExec();
    if (err_msg[0])
        setenv("K15_MENU_MSG", err_msg, 1);
    const char *ret = getenv("K15_MENU_RETURN");
    setenv("K15_MENU_SCREEN", ret && *ret ? ret : "play", 1);
    char *args[] = {"doomk15", NULL};
    fflush(NULL);
    execv("/proc/self/exe", args);
    perror("k15: execv menu");
}

static void on_exit_cb(void)
{
    K15_SoundRestore();
    if (from_menu && !exit_to_ui)
        back_to_menu(); // only returns if execv failed
    K15_FbRestore();
}

// ---------------------------------------------------------------- overlay drawing
static inline int pidx(int sx, int sy)
{
    return sx * K15_PANEL_W + (K15_PANEL_W - 1 - sy);
}

static void ov_put(int sx, int sy, uint32_t rgb, int a)
{
    if ((unsigned)sx >= SCR_W || (unsigned)sy >= SCR_H || a <= 0)
        return;
    int i = pidx(sx, sy), inv = 255 - a;
    uint32_t o = ov_rgb[i];
    uint32_t r = (((rgb >> 16) & 0xFF) * a + ((o >> 16) & 0xFF) * inv) / 255;
    uint32_t g = (((rgb >> 8) & 0xFF) * a + ((o >> 8) & 0xFF) * inv) / 255;
    uint32_t b = ((rgb & 0xFF) * a + (o & 0xFF) * inv) / 255;
    ov_rgb[i] = (r << 16) | (g << 8) | b;
    ov_a[i] = a + ov_a[i] * inv / 255;
}

static void ov_rect(int x, int y, int w, int h, uint32_t c, int a)
{
    for (int i = x; i < x + w; i++)
        for (int j = y; j < y + h; j++)
            ov_put(i, j, c, a);
}

static void ov_frame(int x, int y, int w, int h, int t, uint32_t c, int a)
{
    ov_rect(x, y, w, t, c, a);
    ov_rect(x, y + h - t, w, t, c, a);
    ov_rect(x, y + t, t, h - 2 * t, c, a);
    ov_rect(x + w - t, y + t, t, h - 2 * t, c, a);
}

static void ov_label(int cx, int cy, const char *s, int scale, int a)
{
    int w = K15_TextWidth(s, scale), h = K15_TextHeight(scale);
    K15_DrawText(ov_put, cx - w / 2 + 1, cy - h / 2 + 1, s, scale, 0x000000, a / 2);
    K15_DrawText(ov_put, cx - w / 2, cy - h / 2, s, scale, 0xFFFFFF, a);
}

// ---------------------------------------------------------------- touch gamepad
// Layout presets, hit testing and drawing live in k15_touch.c (shared with the menu's
// preview). Tapping the view = USE (doors/switches) unless that is switched off.
#define MAP_HOLD_MS 600 // long-press MAP / hold pad Back: cheat menu
#define FLASH_MS 150

static const unsigned char button_keys[TB_N] = {
    [TB_ESC] = KEY_ESCAPE, [TB_ENTER] = KEY_ENTER, [TB_MAP] = KEY_TAB, [TB_WPN] = 0,
    [TB_USE] = KEY_USE, [TB_FIRE] = KEY_FIRE, [TB_UP] = KEY_UPARROW,
    [TB_DOWN] = KEY_DOWNARROW, [TB_LEFT] = KEY_LEFTARROW, [TB_RIGHT] = KEY_RIGHTARROW,
    [TB_GAME] = KEY_USE,
};

static uint32_t held_mask;
static unsigned char btn_sent[TB_N]; // DOOM key each held touch button sent
static uint32_t flash_at[TB_N];
// Touch controls hide while a keyboard or pad is used and come back on the next touch,
// which only reveals them (it is swallowed until every finger is up).
static int controls_hidden, touch_swallow;
static int weapon_idx;
static int overlay_dirty = 1;
static k15_hud_t hud_last;
static uint32_t esc_since; // when the ESC pill went down (0 = not held)
static uint32_t map_since; // MAP pill down with the cheat menu enabled (0 = not held)
static int map_used;       // that press opened the cheat menu
static int fps, fps_frames;
static uint32_t fps_t0;

static int menu_open(void)
{
    return menuactive;
}

static int in_level(void)
{
    return gamestate == GS_LEVEL && !menu_open();
}

static int cheats_ok(void)
{
    return K15_CfgInt(CFG_CHEATS) && in_level() && !netgame;
}

static void ov_label_hu(int cx, int cy, const char *s, int size, int a)
{
    ov_label(cx, cy, s, size == 2 ? 5 : size == 1 ? 4 : 3, a);
}

// ---------------------------------------------------------------- cheat menu
// Opened by a long press on MAP (touch) or holding the pad's Back button in a level. It
// pauses the game and types the chosen cheat through the key queue (like a keyboard).
enum { C_GOD, C_ALL, C_AMMO, C_CLIP, C_MAP, C_CLOSE, C_PREV, C_WARP, C_NEXT, C_GO, C_N };
static const struct { int x, y, w, h; const char *label; } cbtn[C_N] = {
    [C_GOD] = {276, 52, 150, 44, "IDDQD"},   [C_ALL] = {436, 52, 150, 44, "IDKFA"},
    [C_AMMO] = {596, 52, 150, 44, "IDFA"},   [C_CLIP] = {276, 106, 150, 44, "IDCLIP"},
    [C_MAP] = {436, 106, 150, 44, "IDDT"},   [C_CLOSE] = {596, 106, 150, 44, "CLOSE"},
    [C_PREV] = {276, 160, 56, 44, "<"},      [C_WARP] = {338, 160, 182, 44, ""},
    [C_NEXT] = {526, 160, 56, 44, ">"},      [C_GO] = {596, 160, 150, 44, "WARP"},
};
static const signed char cnav[C_N][4] = { // up, down, left, right
    [C_GOD] = {C_PREV, C_CLIP, C_AMMO, C_ALL},  [C_ALL] = {C_WARP, C_MAP, C_GOD, C_AMMO},
    [C_AMMO] = {C_GO, C_CLOSE, C_ALL, C_GOD},   [C_CLIP] = {C_GOD, C_PREV, C_CLOSE, C_MAP},
    [C_MAP] = {C_ALL, C_WARP, C_CLIP, C_CLOSE}, [C_CLOSE] = {C_AMMO, C_GO, C_MAP, C_CLIP},
    [C_PREV] = {C_CLIP, C_GOD, C_GO, C_WARP},   [C_WARP] = {C_MAP, C_ALL, C_PREV, C_NEXT},
    [C_NEXT] = {C_MAP, C_ALL, C_WARP, C_GO},    [C_GO] = {C_CLOSE, C_AMMO, C_NEXT, C_PREV},
};
static int cheat_open, cheat_focus = -1, cheat_down = -1, cheat_armed, cheat_paused;
static int warp_ep, warp_map;

static int map_exists(int ep, int map)
{
    char name[9];
    if (gamemode == commercial)
        snprintf(name, sizeof name, "MAP%02d", map);
    else
        snprintf(name, sizeof name, "E%dM%d", ep, map);
    return W_CheckNumForName(name) >= 0;
}

// Next/previous existing level within what idclev accepts for this game.
static void warp_step(int dir)
{
    int max_ep = gamemode == commercial ? 1 : gamemode == shareware ? 1
               : gamemode == registered ? 3 : 4;
    int max_map = gamemode == commercial ? 32 : 9;
    int ep = warp_ep, map = warp_map;
    for (int n = 0; n < max_ep * max_map; n++) {
        map += dir;
        if (map > max_map) {
            map = 1;
            ep = ep >= max_ep ? 1 : ep + 1;
        } else if (map < 1) {
            map = max_map;
            ep = ep <= 1 ? max_ep : ep - 1;
        }
        if (map_exists(ep, map)) {
            warp_ep = ep;
            warp_map = map;
            return;
        }
    }
}

static void type_text(const char *s)
{
    for (; *s; s++) {
        push_key_typed(1, (unsigned char)*s, (unsigned char)*s);
        push_key_typed(0, (unsigned char)*s, (unsigned char)*s);
    }
}

static void tap_key(unsigned char k)
{
    push_key(1, k);
    push_key(0, k);
}

static void release_all_keys(void);

static void cheat_menu_open(void)
{
    if (cheat_open)
        return;
    release_all_keys();
    cheat_open = 1;
    cheat_armed = 0; // the finger that opened it has to come up first
    cheat_down = -1;
    cheat_focus = -1;
    warp_ep = gameepisode;
    warp_map = gamemap;
    cheat_paused = !paused;
    if (cheat_paused)
        tap_key(key_pause);
    overlay_dirty = 1;
    fprintf(stderr, "k15: cheat menu\n");
}

static void cheat_menu_close(int unpause)
{
    cheat_open = 0;
    if (unpause && cheat_paused)
        tap_key(key_pause);
    cheat_paused = 0;
    overlay_dirty = 1;
}

static void cheat_activate(int c)
{
    char buf[16];
    switch (c) {
    case C_PREV:
    case C_NEXT:
        warp_step(c == C_NEXT ? 1 : -1);
        overlay_dirty = 1;
        return;
    case C_WARP:
        warp_step(1);
        overlay_dirty = 1;
        return;
    case C_CLOSE:
        cheat_menu_close(1);
        return;
    case C_GO:
        // G_InitNew() unpauses by itself; a pause tap now would re-pause the new level.
        cheat_menu_close(0);
        if (gamemode == commercial)
            snprintf(buf, sizeof buf, "idclev%02d", warp_map);
        else
            snprintf(buf, sizeof buf, "idclev%d%d", warp_ep, warp_map);
        type_text(buf);
        fprintf(stderr, "k15: cheat %s\n", buf);
        return;
    }
    cheat_menu_close(1);
    if (c == C_MAP && !automapactive)
        tap_key(key_map_toggle);
    static const char *const codes[] = {[C_GOD] = "iddqd", [C_ALL] = "idkfa",
                                        [C_AMMO] = "idfa", [C_MAP] = "iddt"};
    const char *code = c == C_CLIP ? (logical_gamemission == doom ? "idspispopd" : "idclip")
                                   : codes[c];
    type_text(code);
    fprintf(stderr, "k15: cheat %s\n", code);
}

static int cheat_hit(int x, int y)
{
    for (int c = 0; c < C_N; c++)
        if (x >= cbtn[c].x - 4 && x < cbtn[c].x + cbtn[c].w + 4 && y >= cbtn[c].y - 4 &&
            y < cbtn[c].y + cbtn[c].h + 4)
            return c;
    return -1;
}

static void draw_cheats(void)
{
    char warp[16];
    if (gamemode == commercial)
        snprintf(warp, sizeof warp, "MAP%02d", warp_map);
    else
        snprintf(warp, sizeof warp, "E%dM%d", warp_ep, warp_map);
    ov_rect(260, 6, 502, 210, 0x180404, 225);
    ov_frame(260, 6, 502, 210, 3, 0xB02412, 255);
    ov_label(511, 30, "CHEATS", 4, 255);
    for (int c = 0; c < C_N; c++) {
        int down = c == cheat_down, foc = c == cheat_focus;
        int x = cbtn[c].x, y = cbtn[c].y, w = cbtn[c].w, h = cbtn[c].h;
        ov_rect(x, y, w, h, c == C_WARP ? 0x100404 : down ? 0x9A2410 : 0x3C0C06, 255);
        ov_frame(x, y, w, h, 2, 0xB02412, 255);
        if (foc)
            ov_frame(x - 3, y - 3, w + 6, h + 6, 3, 0xFFD448, 255);
        ov_label(x + w / 2, y + h / 2, c == C_WARP ? warp : cbtn[c].label, 3, 255);
    }
}

static void cheat_touch(const k15_touches_t *t)
{
    static int outside; // the touch started outside the panel: closes it on release
    if (!cheat_armed) {
        cheat_armed = t->n == 0;
        return;
    }
    if (t->n) {
        int x = t->x[0], y = t->y[0], h = cheat_hit(x, y);
        if (cheat_down == -1) { // finger down
            outside = x < 260 || x >= 762 || y >= 216;
            cheat_down = h >= 0 ? h : -2;
            cheat_focus = -1;
            overlay_dirty = 1;
        } else if (cheat_down >= 0 && h != cheat_down) {
            cheat_down = -2; // slid off: cancel
            overlay_dirty = 1;
        }
        return;
    }
    int c = cheat_down;
    cheat_down = -1;
    overlay_dirty = 1;
    if (c >= 0)
        cheat_activate(c);
    else if (c == -2 && outside)
        cheat_menu_close(1);
}

static void cheat_key(int code)
{
    int dir = -1;
    switch (code) {
    case KEY_UP: case BTN_DPAD_UP: case K15_KEY_LS_UP: dir = 0; break;
    case KEY_DOWN: case BTN_DPAD_DOWN: case K15_KEY_LS_DOWN: dir = 1; break;
    case KEY_LEFT: case BTN_DPAD_LEFT: case K15_KEY_LS_LEFT: dir = 2; break;
    case KEY_RIGHT: case BTN_DPAD_RIGHT: case K15_KEY_LS_RIGHT: dir = 3; break;
    case EV_K_ENTER: case EV_K_KPENTER: case KEY_SPACE: case BTN_A:
        if (cheat_focus >= 0)
            cheat_activate(cheat_focus);
        else
            cheat_focus = C_GOD;
        overlay_dirty = 1;
        return;
    case KEY_ESC: case EV_K_BACKSPACE: case BTN_B: case BTN_START: case BTN_SELECT:
        cheat_menu_close(1);
        return;
    default:
        return;
    }
    cheat_focus = cheat_focus < 0 ? C_GOD : cnav[cheat_focus][dir];
    overlay_dirty = 1;
}

// ---------------------------------------------------------------- overlay
static void rebuild_overlay(const k15_hud_t *h)
{
    memset(ov_rgb, 0, sizeof ov_rgb);
    memset(ov_a, 0, sizeof ov_a);
    K15_HudDraw(h, ov_put, SCR_W);
    if (!controls_hidden) {
        uint32_t now = K15_NowMs(), flash = 0;
        for (int b = 0; b < TB_N; b++)
            if (flash_at[b] && now - flash_at[b] < FLASH_MS)
                flash |= 1u << b;
        K15_TouchDraw(ov_put, ov_label_hu, held_mask, flash, esc_since ? now - esc_since : 0);
    }
    if (K15_CfgInt(CFG_FPS) && h->in_level)
        K15_HudFps(ov_put, SCR_W, SCR_H, fps);
    if (cheat_open)
        draw_cheats();
}

static void apply_mask(uint32_t m)
{
    uint32_t changed = m ^ held_mask;
    if (!changed)
        return;
    for (int b = 0; b < TB_N; b++) {
        if (!(changed & (1u << b)))
            continue;
        int down = !!(m & (1u << b));
        if (down)
            flash_at[b] = K15_NowMs() | 1;
        if (b == TB_ESC)
            esc_since = down ? (K15_NowMs() | 1) : 0;
        if (b == TB_WPN) {
            if (down) {
                weapon_idx = (weapon_idx + 1) % 7;
                tap_key('1' + weapon_idx);
            }
            continue;
        }
        if (b == TB_MAP && (map_since || (down && cheats_ok()))) {
            // MAP acts on release so a long press can open the cheat menu instead.
            if (down) {
                map_since = K15_NowMs() | 1;
                map_used = 0;
            } else {
                if (!map_used)
                    tap_key(KEY_TAB);
                map_since = 0;
            }
            continue;
        }
        if (!down) {
            push_key(0, btn_sent[b]);
            btn_sent[b] = 0;
            continue;
        }
        unsigned char k = button_keys[b];
        // ENTER (and FIRE, for layouts without ENTER, while DOOM's menu is open) also
        // answers "y" to yes/no prompts (quit, end game, nightmare).
        if (b == TB_FIRE && menu_open())
            k = KEY_ENTER;
        if (k == KEY_ENTER)
            tap_key('y');
        push_key(1, k);
        btn_sent[b] = k;
    }
    held_mask = m;
    overlay_dirty = 1;
}

static void hide_controls(int code)
{
    if (controls_hidden || !K15_CfgInt(CFG_AUTOHIDE) ||
        (code >= 249 && code <= 258)) // x2000_key: front panel / knob
        return;
    controls_hidden = 1;
    apply_mask(0);
    overlay_dirty = 1;
}

static void on_touch(const k15_touches_t *t)
{
    if (cheat_open) {
        cheat_touch(t);
        return;
    }
    if (controls_hidden && t->n) {
        controls_hidden = 0;
        touch_swallow = 1;
        overlay_dirty = 1;
    }
    if (touch_swallow) {
        touch_swallow = t->n > 0;
        return;
    }
    uint32_t m = 0;
    for (int i = 0; i < t->n; i++)
        m |= K15_TouchHit(t->x[i], t->y[i]);
    apply_mask(m);
}

// ---------------------------------------------------------------- keys
static unsigned char evdev_to_doom(int code)
{
    if (code >= KEY_1 && code <= KEY_9)
        return '1' + (code - KEY_1);
    // evdev KEY_Q(16)..KEY_M(50), gaps for non-letters
    static const char qwerty[] = "qwertyuiop[]\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
    if (code >= KEY_Q && code < KEY_Q + (int)sizeof qwerty - 1 && qwerty[code - KEY_Q])
        return qwerty[code - KEY_Q];
    switch (code) {
    case KEY_UP: return KEY_UPARROW;
    case KEY_DOWN: return KEY_DOWNARROW;
    case KEY_LEFT: return KEY_LEFTARROW;
    case KEY_RIGHT: return KEY_RIGHTARROW;
    case EV_K_ENTER: case EV_K_KPENTER: case KEY_OK: case KEY_SELECT: return KEY_ENTER;
    case KEY_ESC: case KEY_BACK: case KEY_MENU: case KEY_EXIT: return KEY_ESCAPE;
    case KEY_SPACE: return KEY_USE;
    case KEY_LEFTCTRL: case KEY_RIGHTCTRL: case KEY_PLAYPAUSE: return KEY_FIRE;
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: return KEY_RSHIFT;
    case KEY_LEFTALT: case KEY_RIGHTALT: return KEY_RALT;
    case EV_K_TAB: return KEY_TAB;
    case EV_K_BACKSPACE: return KEY_BACKSPACE;
    // Volume keys (e.g. a keyboard's knob) stay unmapped: they used to resize the view.
    case EV_K_MINUS: return KEY_MINUS;
    case EV_K_EQUAL: return KEY_EQUALS;
    case KEY_COMMA: case KEY_PREVIOUSSONG: return ',';
    case KEY_DOT: case KEY_NEXTSONG: return '.';
    case KEY_SLASH: return '/';
    case KEY_SEMICOLON: return ';';
    case KEY_APOSTROPHE: return '\'';
    case BTN_0: return KEY_ESCAPE;
    case BTN_1: return KEY_ENTER;
    case BTN_2: return KEY_USE;
    }
    if (code >= EV_K_F1 && code <= EV_K_F10)
        return KEY_F1 + (code - EV_K_F1);
    return 0;
}

// DOOM key of a keyboard action (k15.conf K15_KB_*) / pad action (K15_PAD_*).
static unsigned char kb_action_key(int a)
{
    switch (a) {
    case KB_FORWARD: return key_up;
    case KB_BACK: return key_down;
    case KB_TURNLEFT: return key_left;
    case KB_TURNRIGHT: return key_right;
    case KB_STRAFELEFT: return key_strafeleft;
    case KB_STRAFERIGHT: return key_straferight;
    case KB_FIRE: return key_fire;
    case KB_USE: return key_use;
    case KB_RUN: return key_speed;
    case KB_STRAFE: return key_strafe;
    case KB_PREVWPN: return key_prevweapon;
    case KB_NEXTWPN: return key_nextweapon;
    case KB_MAP: return key_map_toggle;
    }
    return 0;
}

static unsigned char pad_action_key(int a)
{
    switch (a) {
    case PA_FIRE: return key_fire;
    case PA_USE: return key_use;
    case PA_RUN: return key_speed;
    case PA_STRAFE: return key_strafe;
    case PA_PREVWPN: return key_prevweapon;
    case PA_NEXTWPN: return key_nextweapon;
    case PA_MAP: return key_map_toggle;
    case PA_MENU: return KEY_ESCAPE;
    }
    return 0;
}

static int pad_button(int code)
{
    switch (code) {
    case BTN_A: return PB_A;
    case BTN_B: return PB_B;
    case BTN_X: return PB_X;
    case BTN_Y: return PB_Y;
    case BTN_TL: return PB_LB;
    case BTN_TR: return PB_RB;
    case BTN_TL2: return PB_LT;
    case BTN_TR2: return PB_RT;
    case BTN_THUMBL: return PB_L3;
    case BTN_THUMBR: return PB_R3;
    case BTN_SELECT: return PB_BACK;
    }
    return -1;
}

// Keyboard and pad keys are mapped at press time, depending on where we are (DOOM menu
// open or not). The DOOM keys a press produced are remembered per evdev code, so the
// release always matches even if the menu opened or closed in between.
static unsigned char held_by[KEY_MAX + 1][2];
static uint32_t key_esc_since; // keyboard Esc / pad Start held (0 = not held)
static uint32_t back_since;    // pad Back held with the cheat menu enabled (0 = not held)
static int back_used;

static void release_all_keys(void)
{
    for (int c = 0; c <= KEY_MAX; c++)
        for (int i = 0; i < 2; i++)
            if (held_by[c][i]) {
                push_key(0, held_by[c][i]);
                held_by[c][i] = 0;
            }
    apply_mask(0);
    key_esc_since = esc_since = 0;
    map_since = back_since = 0;
}

// Pad button -> up to two DOOM keys. A "yes" tap goes out before them (like the ENTER pill).
static void pad_to_doom(int code, unsigned char out[2], int *yes)
{
    switch (code) {
    case BTN_DPAD_UP: out[0] = KEY_UPARROW; return;
    case BTN_DPAD_DOWN: out[0] = KEY_DOWNARROW; return;
    case BTN_DPAD_LEFT: out[0] = KEY_LEFTARROW; return;
    case BTN_DPAD_RIGHT: out[0] = KEY_RIGHTARROW; return;
    case BTN_START: out[0] = KEY_ESCAPE; return;
    }
    if (menu_open()) {
        switch (code) {
        case K15_KEY_LS_UP: out[0] = KEY_UPARROW; break;
        case K15_KEY_LS_DOWN: out[0] = KEY_DOWNARROW; break;
        case K15_KEY_LS_LEFT: out[0] = KEY_LEFTARROW; break;
        case K15_KEY_LS_RIGHT: out[0] = KEY_RIGHTARROW; break;
        case BTN_A: *yes = 1; out[0] = KEY_ENTER; break;
        case BTN_B: out[0] = messageToPrint ? 'n' : KEY_BACKSPACE; break;
        }
        return;
    }
    switch (code) {
    case K15_KEY_LS_LEFT: out[0] = key_strafeleft; return;
    case K15_KEY_LS_RIGHT: out[0] = key_straferight; return;
    // stick up/down and the right stick are analog (post_pad_analog)
    }
    int b = pad_button(code);
    if (b >= 0)
        out[0] = pad_action_key(K15_CfgInt(CFG_PAD_FIRST + b));
}

static void on_key(int pressed, int code)
{
    if (code < 0 || code > KEY_MAX)
        return;
    if (pressed)
        hide_controls(code);
    if (code == KEY_ESC || code == BTN_START)
        key_esc_since = pressed ? (K15_NowMs() | 1) : 0;
    if (!pressed) {
        for (int i = 0; i < 2; i++)
            push_key(0, held_by[code][i]);
        held_by[code][0] = held_by[code][1] = 0;
        if (code == BTN_SELECT && back_since) {
            back_since = 0;
            if (!back_used) { // short press: the button's normal action, as a tap
                unsigned char out[2] = {0, 0};
                int yes = 0;
                pad_to_doom(code, out, &yes);
                tap_key(out[0]);
            }
        }
        return;
    }
    if (cheat_open) {
        cheat_key(code);
        return;
    }
    if (code == BTN_SELECT && cheats_ok()) {
        back_since = K15_NowMs() | 1; // acts on release; held = cheat menu
        back_used = 0;
        return;
    }
    unsigned char out[2] = {0, 0}, typed = 0;
    int yes = 0;
    if ((code >= BTN_JOYSTICK && code <= BTN_THUMBR) ||
        (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT) ||
        (code >= K15_KEY_LS_UP && code <= K15_KEY_LS_RIGHT))
        pad_to_doom(code, out, &yes);
    else {
        out[0] = evdev_to_doom(code);
        // Rebindable keys (WASD, [ ] by default) act in a level with the menu closed; the
        // key's own character still counts as typed (cheats).
        if (in_level()) {
            for (int a = 0; a < KB_N; a++)
                if (K15_CfgInt(CFG_KB_FIRST + a) == code) {
                    unsigned char k = kb_action_key(a);
                    if (!k)
                        break;
                    typed = out[0] >= ' ' && out[0] < 0x7f ? out[0] : 0;
                    out[0] = k;
                    break;
                }
        }
        if (!out[0])
            fprintf(stderr, "k15: unmapped key code %d\n", code);
    }
    if (yes)
        tap_key('y');
    for (int i = 0; i < 2; i++)
        if (out[i])
            push_key_typed(1, out[i], i ? 0 : typed);
    held_by[code][0] = out[0];
    held_by[code][1] = out[1];
}

// Square response outside the deadzone: fine aim near the centre, full speed at the edge.
static int pad_curve(int v, int max, int square)
{
    int s = v < 0 ? -1 : 1, a = v * s - PAD_DEAD;
    if (a <= 0)
        return 0;
    long long n = (long long)a * 1024 / (32767 - PAD_DEAD); // 0..1024
    if (square)
        n = n * n / 1024;
    return s * (int)((n * max + 1023) / 1024);
}

// Right stick X = turn, left stick Y = forward/back, as one mouse-motion event per tic.
static void post_pad_analog(void)
{
    const k15_pad_t *p = K15_Pad();
    if (!p->present || !in_level() || cheat_open)
        return;
    int turn = pad_curve(p->rx, PAD_TURN_MAX, 1);
    int fwd = pad_curve(-p->ly, PAD_MOVE_MAX, 0);
    if (!turn && !fwd)
        return;
    event_t ev = {.type = ev_mouse, .data1 = 0, .data2 = turn, .data3 = fwd};
    D_PostEvent(&ev);
}

static void on_udp(int pressed, unsigned char key)
{
    push_key(pressed, key);
}

// ---------------------------------------------------------------- doomgeneric API
void DG_Init(void)
{
    K15_FbOpen();
    K15_InputOpen();
    for (int x = 0; x < SCR_W; x++)
        xmap[x] = x * DOOMGENERIC_RESX / SCR_W;
    for (int y = 0; y < SCR_H; y++)
        ymap[y] = y * DOOMGENERIC_RESY / SCR_H;
    fprintf(stderr, "k15: render %dx%d\n", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_DrawFrame(void)
{
    // The config is loaded after DG_Init(): bind pad LB/RB if prev/next weapon are free.
    if (!key_prevweapon)
        key_prevweapon = DK_PREVWEAPON;
    if (!key_nextweapon)
        key_nextweapon = DK_NEXTWEAPON;
    uint32_t now = K15_NowMs(), hold = K15_CfgInt(CFG_ESC_HOLD);
    if (key_esc_since && now - key_esc_since >= hold) {
        fprintf(stderr, "k15: Esc/Start held %u ms, quitting to the stock UI\n",
                now - key_esc_since);
        exit_to_ui = 1;
        I_Quit();
    }
    if (esc_since) {
        uint32_t held = now - esc_since;
        if (held >= hold) {
            fprintf(stderr, "k15: ESC held %u ms, quitting to the stock UI\n", held);
            exit_to_ui = 1;
            I_Quit();
        }
        if (held >= ESC_HOLD_SHOW_MS)
            overlay_dirty = 1;
    }
    // Long press on MAP / held pad Back: the cheat menu (only while still in a level).
    if (map_since && !map_used && now - map_since >= MAP_HOLD_MS && cheats_ok()) {
        map_used = 1;
        cheat_menu_open();
    }
    if (back_since && !back_used && now - back_since >= MAP_HOLD_MS && cheats_ok()) {
        back_used = 1;
        cheat_menu_open();
    }
    if (cheat_open && !in_level() && !paused)
        cheat_menu_close(0); // left the level (died, exited...) underneath
    for (int b = 0; b < TB_N; b++)
        if (flash_at[b] && now - flash_at[b] < FLASH_MS + 40)
            overlay_dirty = 1;
    fps_frames++;
    if (now - fps_t0 >= 1000) {
        int f = (int)(fps_frames * 1000 / (now - fps_t0));
        overlay_dirty |= f != fps && K15_CfgInt(CFG_FPS);
        fps = f;
        fps_frames = 0;
        fps_t0 = now;
    }
    k15_hud_t h;
    K15_HudGet(&h);
    if (overlay_dirty || memcmp(&h, &hud_last, sizeof h)) {
        rebuild_overlay(&h);
        hud_last = h;
        overlay_dirty = 0;
    }

    uint32_t *out = K15_FbBack();
    const uint32_t *orgb = ov_rgb;
    const uint8_t *oa = ov_a;
    for (int sx = 0; sx < SCR_W; sx++) {
        const uint32_t *src = DG_ScreenBuffer + xmap[sx];
        // panel row for this column runs sy = 239 .. 0
        for (int sy = SCR_H - 1; sy >= 0; sy--) {
            uint32_t g = src[ymap[sy] * DOOMGENERIC_RESX];
            int a = *oa++;
            uint32_t o = *orgb++;
            if (a) {
                int inv = 255 - a;
                uint32_t rb = ((g & 0xFF00FF) * inv >> 8) & 0xFF00FF;
                uint32_t gg = ((g & 0x00FF00) * inv >> 8) & 0x00FF00;
                g = o + rb + gg;
            }
            *out++ = g;
        }
    }
    K15_FbFlip();
    K15_InputPump(on_touch, on_key, on_udp);
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    static uint32_t t0;
    if (!t0)
        t0 = K15_NowMs() - 1;
    return K15_NowMs() - t0;
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    K15_InputPump(on_touch, on_key, on_udp);
    if (kq_r == kq_w) {
        post_pad_analog(); // I_GetEvent() stops here: once per tic
        return 0;
    }
    uint32_t k = kq[kq_r];
    kq_r = (kq_r + 1) % KQ;
    *pressed = k >> 16;
    *key = k & 0xFF;
    last_typed = (k >> 8) & 0xFF ? (int)((k >> 8) & 0xFF) : -1;
    return 1;
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

// ---------------------------------------------------------------- start-up
// Drop fds inherited from mq_ui via system() (fb0, evdev, mqueues, sockets...), except
// the grabbed input fds our previous incarnation handed over.
static void close_inherited_fds(void)
{
    const char *keep = getenv("K15_EVFDS");
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return;
    int dfd = dirfd(d), fds[256], n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        int fd = atoi(e->d_name);
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || fd <= 2 || fd == dfd)
            continue;
        char tok[16];
        snprintf(tok, sizeof tok, "%d", fd);
        int kept = 0;
        if (keep) {
            const char *p = keep;
            size_t l = strlen(tok);
            while ((p = strstr(p, tok))) {
                if ((p == keep || p[-1] == ',') && (p[l] == ',' || p[l] == 0))
                    kept = 1;
                p += l;
            }
        }
        if (!kept)
            fds[n++] = fd;
    }
    closedir(d);
    for (int i = 0; i < n; i++)
        close(fds[i]);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--restore"))
        return K15_FbRestore() != 0;
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("doomk15 %s\n", K15_VERSION);
        return 0;
    }
    close_inherited_fds();
    mkdir(K15_ConfigDirPath(), 0755);
    if (chdir(K15_ConfigDirPath()) != 0)
        perror("k15: chdir");
    K15_CfgLoad();

    K15_FbOpen();
    K15_InputOpen();
    atexit(on_exit_cb);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGABRT, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int has_iwad = 0;
    for (int i = 1; i < argc; i++)
        has_iwad |= !strcmp(argv[i], "-iwad");
    if (!has_iwad) {
        if (!K15_Menu(&argc, &argv)) {
            exit_to_ui = 1;
            return 0;
        }
        from_menu = 1;
    }
    K15_InputKeyboard(K15_CfgInt(CFG_KBD));
    doomgeneric_Create(argc, argv);
    for (;;)
        doomgeneric_Tick();
}

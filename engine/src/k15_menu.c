// SPDX-License-Identifier: GPL-2.0-or-later
// DOOM home menu for the FiiO K15: PLAY (favourite, the only WAD, or the WAD list), WADS
// (play, favourite star, install/delete), OPTIONS (touch layout + preview, touch/keyboard/gamepad controls,
// features), EXIT. Drawn with k15_gfx (no WAD needed), driven by touch (activate on
// release), front keys / IR, keyboards, pads, or the UDP keyboard (a focus frame moves in
// the pressed direction). Settings live in k15.conf (k15_cfg.c).

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "k15_cfg.h"
#include "k15_gfx.h"
#include "k15_icons.h"
#include "k15_menu.h"
#include "k15_plat.h"
#include "k15_touch.h"
#include "k15_wad.h"

#ifndef K15_VERSION
#define K15_VERSION "dev"
#endif

#define COL_BG_TOP 0x300600
#define COL_BG_BOT 0x000000
#define COL_LOGO_TOP 0xffd448
#define COL_LOGO_BOT 0xb01406
#define COL_BTN 0x3c0c06
#define COL_BTN_DOWN 0x9a2410
#define COL_BTN_OFF 0x1c0c0a
#define COL_BORDER 0xb02412
#define COL_FOCUS 0xffd448
#define COL_ROW 0x1a0907
#define COL_TEXT 0xffffff
#define COL_DIM 0xa89484
#define COL_YELLOW 0xffd448
#define COL_RED 0xff3a24
#define INSTALL_MARGIN (5LL << 20)
#define RESCAN_MS 2000
#define ROWS 3
#define ROW_Y(r) (54 + (r) * 62)
#define FLASH_MS 150

// doomkeys.h values for the UDP keyboard (doomkeys.h clashes with linux/input.h)
#define DK_RIGHT 0xae
#define DK_LEFT 0xac
#define DK_UP 0xad
#define DK_DOWN 0xaf
#define DK_ENTER 13
#define DK_ESCAPE 27

enum { S_HOME, S_WADS, S_NOWAD, S_OPTIONS, S_CONTROLS, S_SET, S_PREVIEW };
enum { SL_LAYOUT, SL_TOUCH, SL_KEYBOARD, SL_GAMEPAD, SL_FEATURES, SL_N };
enum { A_NONE, A_PLAYHOME, A_WADS, A_OPTIONS, A_EXIT, A_BACK, A_UP, A_DOWN, A_PLAY,
       A_INSTALL, A_DELETE, A_STAR, A_OK, A_YES, A_NO, A_CANCEL, A_LIST, A_CONTROLS,
       A_VDEC, A_VINC, A_VALUE, A_TOGGLE, A_BIND, A_UNBIND, A_PREVIEW, A_RESET,
       A_MINSTALL, A_MPLAYUSB };
enum { IT_VALUE, IT_TOGGLE, IT_KEY, IT_ACTION };
enum { M_NONE, M_OK, M_YESNO, M_CAPTURE, M_USBWAD };
// What PLAY on the home screen does (play_state()).
enum { PS_NONE, PS_FAV, PS_ONE, PS_MANY };
#define MODAL_X 150
#define MODAL_Y 22
#define MODAL_W 720
#define MODAL_H 196

typedef struct {
    int x, y, w, h, scale, action, arg, enabled;
    int icon, outline, tile, nofocus, row; // row: list row (0..ROWS-1) or -1
    int sub_icon; // small icon before the sub-label, or -1
    uint32_t icon_rgb;
    const char *label, *sub;
} button_t;

typedef struct {
    int kind, id; // id: cfg id (KB action index for IT_KEY, action for IT_ACTION)
    const char *label;
} item_t;

static button_t btns[48];
static int nbtn;
static int focus = -1;   // index into btns of the focused button (keys), -1 = touch mode
static int focus_act, focus_arg, focus_on; // focus identity, survives redraws
static int pressed = -1; // button under the finger
static int touching;
static int touch_outside; // touch began outside an M_USBWAD box (its release cancels it)
static int screen = S_HOME, scroll, setlist;
static int screen_scroll[2]; // scroll of the WAD list / a settings list, kept per screen
static k15_wad_t wads[K15_MAX_WADS];
static int nwads;
static uint32_t last_scan;
static int dirty = 1;
static int modal;
static char modal_msg[600];
static int modal_action, modal_arg;
static char modal_path[256]; // file a YES/NO modal acts on (indices go stale on rescan)
static int saved_focus_act, saved_focus_arg;
static int result; // 0 running, 1 play, -1 exit
static int play_idx, play_from_home;
static int dev_build;
static int had_kbd, had_pad;
// layout preview
static uint32_t pv_held, pv_flash_at[TB_N], pv_esc_since;
static int pv_start_hit;

static char pool[4096];
static int pool_pos;

static const char *pfmt(const char *fmt, ...)
{
    va_list ap;
    if (pool_pos > (int)sizeof pool - 128)
        pool_pos = 0;
    char *s = pool + pool_pos;
    va_start(ap, fmt);
    int n = vsnprintf(s, sizeof pool - pool_pos, fmt, ap);
    va_end(ap);
    pool_pos += (n < 0 ? 0 : n) + 1;
    if (pool_pos > (int)sizeof pool)
        pool_pos = sizeof pool;
    return s;
}

// ---------------------------------------------------------------- helpers
static button_t *add_btn(int x, int y, int w, int h, int scale, const char *label, int action,
                         int arg, int enabled)
{
    static button_t dummy;
    if (nbtn >= (int)(sizeof btns / sizeof btns[0]))
        return &dummy;
    btns[nbtn] = (button_t){x, y, w, h, scale, action, arg, enabled, -1, 0, 0, 0, -1, -1,
                            COL_TEXT, label, NULL};
    return &btns[nbtn++];
}

static int focusable(int i)
{
    return i >= 0 && i < nbtn && btns[i].enabled && !btns[i].nofocus;
}

static void set_focus(int i)
{
    focus = i;
    if (i >= 0) {
        focus_on = 1;
        focus_act = btns[i].action;
        focus_arg = btns[i].arg;
    }
}

// Map the focus identity onto the freshly built buttons.
static void resolve_focus(void)
{
    focus = -1;
    if (!focus_on)
        return;
    for (int i = 0; i < nbtn; i++)
        if (focusable(i) && btns[i].action == focus_act && btns[i].arg == focus_arg) {
            focus = i;
            return;
        }
    for (int i = 0; i < nbtn; i++)
        if (focusable(i) && btns[i].action != A_BACK) {
            set_focus(i);
            return;
        }
    for (int i = 0; i < nbtn; i++)
        if (focusable(i)) {
            set_focus(i);
            return;
        }
}

static void draw_btn(int i)
{
    const button_t *b = &btns[i];
    uint32_t fill = !b->enabled ? COL_BTN_OFF : i == pressed ? COL_BTN_DOWN : COL_BTN;
    G_Fill(b->x, b->y, b->w, b->h, fill);
    G_Frame(b->x, b->y, b->w, b->h, 2, b->enabled ? COL_BORDER : 0x40201c);
    if (i == focus)
        G_Frame(b->x - 3, b->y - 3, b->w + 6, b->h + 6, 3, COL_FOCUS);
    if (b->action == A_UP || b->action == A_DOWN) {
        G_Triangle(b->x + b->w / 2, b->y + b->h / 2, 18, b->action == A_UP,
                   b->enabled ? COL_TEXT : 0x504040);
        return;
    }
    uint32_t tc = b->enabled ? COL_TEXT : 0x6a5650;
    uint32_t ic = b->enabled ? b->icon_rgb : 0x6a5650;
    int tw = G_TextW(b->label, b->scale), th = G_TextH(b->scale);
    if (b->tile) { // icon above label above sub-label, centred
        int is = b->h >= 140 ? 4 : 3, sh = b->sub ? 22 : 0;
        int total = (b->icon >= 0 ? 16 * is + 10 : 0) + th + sh;
        int y = b->y + (b->h - total) / 2;
        if (b->icon >= 0) {
            G_Icon(b->x + (b->w - 16 * is) / 2, y, b->icon, is, ic, b->outline);
            y += 16 * is + 10;
        }
        if (b->enabled)
            G_TextCentre(b->x + b->w / 2, y, b->label, b->scale, tc);
        else
            G_Text(b->x + (b->w - tw) / 2, y, b->label, b->scale, tc, 0);
        if (b->sub)
            G_Text(b->x + (b->w - G_TextW(b->sub, 2)) / 2, y + th + 6, b->sub, 2,
                   b->enabled ? COL_DIM : 0x6a5650, b->w - 8);
        return;
    }
    int is = b->h >= 90 ? 3 : 2; // icon left of the label
    int iw = b->icon >= 0 ? 16 * is + (*b->label ? 14 : 0) : 0;
    int x = b->x + (b->w - iw - tw) / 2, y = b->y + (b->h - th) / 2, up = b->sub ? 12 : 0;
    if (b->icon >= 0)
        G_Icon(x, b->y + (b->h - 16 * is) / 2 - up, b->icon, is, ic, b->outline);
    x += iw;
    if (b->enabled)
        G_TextShadow(x, y - up, b->label, b->scale, tc);
    else
        G_Text(x, y - up, b->label, b->scale, tc, 0);
    if (b->sub) { // centred under the icon + label, after an optional text-sized icon
        int sx = b->sub_icon >= 0 ? 16 + 8 : 0;
        int sw = G_TextW(b->sub, 2), max = b->w - 12 - sx;
        sw = sw > max ? max : sw;
        int x0 = b->x + (b->w - sx - sw) / 2, sy = b->y + b->h - 26;
        if (b->sub_icon >= 0)
            G_Icon(x0, sy, b->sub_icon, 1, b->enabled ? COL_YELLOW : 0x6a5650, 0);
        G_Text(x0 + sx, sy, b->sub, 2, b->enabled ? COL_DIM : 0x6a5650, max);
    }
}

static void background(void)
{
    G_VGradient(0, 0, K15_SCR_W, K15_SCR_H, COL_BG_TOP, COL_BG_BOT);
}

static void show_modal(int kind, int action, int arg, const char *fmt, const char *a,
                       const char *b)
{
    snprintf(modal_msg, sizeof modal_msg, fmt, a ? a : "", b ? b : "");
    if (!modal) {
        saved_focus_act = focus_act;
        saved_focus_arg = focus_arg;
    }
    modal = kind;
    modal_action = action;
    modal_arg = arg;
    // keyboard default: NO for YES/NO
    focus_act = kind == M_YESNO      ? A_NO
                : kind == M_CAPTURE  ? A_CANCEL
                : kind == M_USBWAD   ? A_MINSTALL
                                     : A_OK;
    focus_arg = 0;
    dirty = 1;
}

static void close_modal(void)
{
    modal = 0;
    focus_act = saved_focus_act;
    focus_arg = saved_focus_arg;
    dirty = 1;
}

static void mb(char *out, size_t n, long long bytes)
{
    snprintf(out, n, "%lld.%lld MB", bytes >> 20, ((bytes & 0xFFFFF) * 10) >> 20);
}

static long long free_bytes(void)
{
    struct statvfs s;
    if (statvfs(K15_ConfigDirPath(), &s) != 0)
        return -1;
    return (long long)s.f_bavail * s.f_frsize;
}

static void rescan(void)
{
    k15_wad_t prev[K15_MAX_WADS];
    int nprev = nwads;
    memcpy(prev, wads, sizeof wads);
    nwads = K15_WadScan(wads, K15_MAX_WADS);
    last_scan = K15_NowMs();
    if (nwads != nprev || memcmp(prev, wads, sizeof wads[0] * nwads))
        dirty = 1;
    // Only an installed WAD can be the favourite: forget one that isn't (any more).
    const char *fav = K15_CfgStr(CFG_DEFAULT_WAD);
    if (*fav) {
        int found = 0;
        for (int i = 0; i < nwads && !found; i++)
            found = !wads[i].usb && !strcasecmp(wads[i].name, fav);
        if (!found) {
            fprintf(stderr, "k15menu: favourite %s is not installed, cleared\n", fav);
            K15_CfgSetStr(CFG_DEFAULT_WAD, "");
            K15_CfgSave();
            dirty = 1;
        }
    }
}

static int playable(int i)
{
    return i >= 0 && i < nwads && (wads[i].iwad || K15_WadBaseFor(wads, nwads, &wads[i]) >= 0);
}

static int is_default(int i)
{
    const char *name = K15_CfgStr(CFG_DEFAULT_WAD);
    return *name && !wads[i].usb && !strcasecmp(wads[i].name, name);
}

// The favourite (an installed WAD, by file name), or -1.
static int default_wad(void)
{
    for (int i = 0; i < nwads; i++)
        if (is_default(i))
            return i;
    return -1;
}

// What PLAY does; *idx: the WAD for PS_FAV / PS_ONE.
static int play_state(int *idx)
{
    *idx = default_wad();
    if (*idx >= 0)
        return PS_FAV;
    *idx = nwads == 1 ? 0 : -1;
    return !nwads ? PS_NONE : nwads == 1 ? PS_ONE : PS_MANY;
}

static int find_path(const char *path)
{
    for (int i = 0; i < nwads; i++)
        if (!strcmp(wads[i].path, path))
            return i;
    return -1;
}

// ---------------------------------------------------------------- settings lists
static int list_items(int sl, item_t *it)
{
    int n = 0;
    switch (sl) {
    case SL_LAYOUT:
        it[n++] = (item_t){IT_VALUE, CFG_LAYOUT, "PRESET"}; // + PREVIEW
        it[n++] = (item_t){IT_VALUE, CFG_TRANSPARENCY, "TRANSPARENCY"};
        break;
    case SL_TOUCH:
        it[n++] = (item_t){IT_TOGGLE, CFG_TAP_USE, "TAP VIEW = USE"};
        it[n++] = (item_t){IT_VALUE, CFG_JOY_DEAD, "STICK DEADZONE"};
        it[n++] = (item_t){IT_VALUE, CFG_ESC_HOLD, "ESC HOLD TO EXIT"};
        it[n++] = (item_t){IT_TOGGLE, CFG_FLASH, "PRESS FLASH"};
        break;
    case SL_KEYBOARD:
        it[n++] = (item_t){IT_TOGGLE, CFG_KBD, "KEYBOARD IN GAME"};
        for (int a = 0; a < KB_N; a++)
            it[n++] = (item_t){IT_KEY, a, K15_KbActionName(a)};
        break;
    case SL_GAMEPAD:
        for (int b = 0; b < PB_N; b++)
            it[n++] = (item_t){IT_VALUE, CFG_PAD_FIRST + b, pfmt("BUTTON %s",
                                                                 K15_PadButtonName(b))};
        break;
    case SL_FEATURES:
        it[n++] = (item_t){IT_TOGGLE, CFG_AUTOHIDE, "HIDE TOUCH ON KEYS/PAD"};
        it[n++] = (item_t){IT_TOGGLE, CFG_CHEATS, "CHEAT MENU (HOLD MAP)"};
        it[n++] = (item_t){IT_VALUE, CFG_MUSIC_GAIN, "MUSIC VOLUME"};
        it[n++] = (item_t){IT_TOGGLE, CFG_FPS, "SHOW FPS"};
        it[n++] = (item_t){IT_TOGGLE, CFG_DEBUG, "DEBUG LOG"};
        break;
    }
    it[n++] = (item_t){IT_ACTION, A_RESET, "RESET TO DEFAULTS"};
    return n;
}

static const char *const list_title[SL_N] = {"LAYOUT", "TOUCH", "KEYBOARD", "GAMEPAD",
                                             "FEATURES"};
static const char *const list_note[SL_N] = {
    "", "", "ARROWS, CTRL, SPACE ETC. ALWAYS WORK",
    "D-PAD = ARROWS, START = MENU", ""};

static const char *fmt_value(int id)
{
    int v = K15_CfgInt(id);
    const char *e = K15_CfgEnumName(id, v);
    if (id >= CFG_PAD_FIRST && id < CFG_PAD_FIRST + PB_N)
        return K15_PadActionName(v);
    if (e)
        return e;
    switch (id) {
    case CFG_TRANSPARENCY: return pfmt("%d%%", v);
    case CFG_JOY_DEAD: return pfmt("%d PX", v);
    case CFG_ESC_HOLD: return pfmt("%d.%d S", v / 1000, v % 1000 / 100);
    case CFG_MUSIC_GAIN: return pfmt("%d%%", v / 10);
    }
    return pfmt("%d", v);
}

static int nlist(void)
{
    item_t it[32];
    return screen == S_SET ? list_items(setlist, it) : nwads;
}

static void scroll_clamp(void)
{
    int n = nlist();
    if (scroll > n - ROWS)
        scroll = n > ROWS ? n - ROWS : 0;
    if (scroll < 0)
        scroll = 0;
}

// ---------------------------------------------------------------- screens
static void draw_logo_block(int alpha)
{
    int h = 138, w = G_LogoWidth(h), x = 44, y = 22;
    G_Logo(x + 4, y + 5, h, 0x000000, 0x000000, alpha / 2);
    G_Logo(x, y, h, COL_LOGO_TOP, COL_LOGO_BOT, alpha);
    G_TextCentre(x + w / 2, 178, "FIIO K15 EDITION", 2, alpha > 128 ? COL_DIM : 0x605048);
    if (dev_build)
        G_TextCentre(x + w / 2, 204, "DEV BUILD", 2, COL_YELLOW);
    else
        G_TextCentre(x + w / 2, 204, "DOOM-FW " K15_VERSION, 2, 0x705a50);
}

static void draw_home(void)
{
    background();
    draw_logo_block(255);
    int d, st = play_state(&d);
    button_t *b;
    b = add_btn(376, 14, 312, 102, 3, "PLAY", A_PLAYHOME, 0, 1);
    b->icon = IC_PLAY;
    b->sub = st == PS_NONE ? "NO WAD FOUND" : st == PS_MANY ? "SELECT WADS" : wads[d].title;
    if (st == PS_FAV)
        b->sub_icon = IC_STAR;
    b = add_btn(700, 14, 312, 102, 3, "WADS", A_WADS, 0, 1);
    b->icon = IC_WADS;
    b = add_btn(376, 126, 312, 102, 3, "OPTIONS", A_OPTIONS, 0, 1);
    b->icon = IC_OPTIONS;
    b = add_btn(700, 126, 312, 102, 3, "EXIT", A_EXIT, 0, 1);
    b->icon = IC_EXIT;
}

static void usb_status(char *out, size_t n)
{
    char roots[K15_MAX_USB][64];
    int nr = K15_UsbRoots(roots, K15_MAX_USB);
    if (!nr) {
        snprintf(out, n, "USB: NO STICK DETECTED");
        return;
    }
    snprintf(out, n, "USB: %s%s", roots[0], nr > 1 ? " +" : "");
}

static void draw_nowad(void)
{
    char st[96];
    background();
    draw_logo_block(80);
    G_TextShadow(390, 18, "NO WAD FOUND", 4, COL_RED);
    G_TextShadow(390, 66, "PUT A DOOM .WAD FILE IN THE ROOT", 2, COL_TEXT);
    G_TextShadow(390, 88, "FOLDER OF A USB STICK, E.G. DOOM1.WAD,", 2, COL_TEXT);
    G_TextShadow(390, 110, "DOOM2.WAD OR FREEDOOM2.WAD.", 2, COL_TEXT);
    usb_status(st, sizeof st);
    G_Text(390, 142, st, 2, COL_DIM, 600);
    add_btn(390, 172, 220, 52, 3, "BACK", A_BACK, 0, 1);
}

static void header(const char *title, const char *right)
{
    background();
    add_btn(8, 6, 136, 40, 3, "BACK", A_BACK, 0, 1);
    G_TextShadow(164, 14, title, 3, COL_YELLOW);
    if (right && *right) {
        int tx = 164 + G_TextW(title, 3) + 24, sw = G_TextW(right, 2);
        int max = K15_SCR_W - 12 - tx;
        G_Text(K15_SCR_W - 12 - (sw > max ? max : sw), 18, right, 2, COL_DIM, max);
    }
}

static void scroll_arrows(int n)
{
    if (n <= ROWS)
        return;
    button_t *b = add_btn(928, 54, 84, 87, 3, "", A_UP, 0, scroll > 0);
    b->nofocus = 1;
    b = add_btn(928, 147, 84, 87, 3, "", A_DOWN, 0, scroll + ROWS < n);
    b->nofocus = 1;
}

static void draw_wads(void)
{
    char line[128], sz[32], st[96];
    long long fb = free_bytes();
    mb(sz, sizeof sz, fb < 0 ? 0 : fb);
    snprintf(st, sizeof st, "K15 FREE: %s", sz);
    header("WADS", st);
    if (!nwads) {
        G_TextCentre(K15_SCR_W / 2, 110, "NO WADS. PUT .WAD FILES ON A USB STICK.", 2,
                     COL_DIM);
        return;
    }
    int rw = nwads > ROWS ? 912 : 1004;
    for (int r = 0; r < ROWS && scroll + r < nwads; r++) {
        int i = scroll + r;
        const k15_wad_t *w = &wads[i];
        int y = ROW_Y(r);
        button_t *b;
        G_Fill(8, y, rw, 56, COL_ROW);
        G_Frame(8, y, rw, 56, 1, 0x55160c);

        // right-aligned buttons, right to left
        int bx = 8 + rw - 8;
        if (w->usb) {
            bx -= 200;
            b = add_btn(bx, y + 8, 200, 40, 3, "INSTALL", A_INSTALL, i, 1);
        } else {
            bx -= 168;
            b = add_btn(bx, y + 8, 168, 40, 3, "DELETE", A_DELETE, i, 1);
        }
        b->row = r;
        bx -= 132;
        b = add_btn(bx, y + 8, 124, 40, 3, "PLAY", A_PLAY, i, playable(i));
        b->row = r;
        bx -= 60;
        if (!w->usb) { // only an installed WAD can be the favourite
            int def = is_default(i);
            b = add_btn(bx, y + 8, 52, 40, 3, "", A_STAR, i, playable(i) || def);
            b->icon = IC_STAR;
            b->outline = !def;
            b->icon_rgb = def ? COL_YELLOW : COL_DIM;
            b->row = r;
        }
        int maxw = bx - 30;
        G_Text(20, y + 6, w->name, 3, COL_TEXT, maxw);
        mb(sz, sizeof sz, w->size);
        const char *note = "";
        if (!w->iwad && K15_WadBaseFor(wads, nwads, w) < 0)
            note = " - NEEDS AN IWAD";
        snprintf(line, sizeof line, "%s - %s - %s%s", w->desc, w->usb ? "USB" : "ON K15", sz,
                 note);
        G_Text(20, y + 36, line, 2, w->usb ? COL_DIM : 0xd0b070, maxw);
    }
    scroll_arrows(nwads);
}

static void draw_grid(const char *title, int n, const int *icons, const char *const *labels,
                      const int *actions, const int *args, const char *const *subs,
                      const int *enabled)
{
    header(title, NULL);
    int w = (1008 - 12 - (n - 1) * 12) / n;
    for (int i = 0; i < n; i++) {
        button_t *b = add_btn(12 + i * (w + 12), 60, w, 168, 3, labels[i], actions[i], args[i],
                              enabled[i]);
        b->icon = icons[i];
        b->tile = 1;
        b->sub = subs[i];
    }
}

static void draw_options(void)
{
    static const int icons[] = {IC_LAYOUT, IC_CONTROLS, IC_FEATURES};
    static const char *const labels[] = {"LAYOUT", "CONTROLS", "FEATURES"};
    static const int actions[] = {A_LIST, A_CONTROLS, A_LIST};
    static const int args[] = {SL_LAYOUT, 0, SL_FEATURES};
    static const int en[] = {1, 1, 1};
    const char *subs[] = {K15_LayoutName(K15_CfgInt(CFG_LAYOUT)), "TOUCH, KEYS, PAD",
                          "CHEATS, FPS, MUSIC"};
    draw_grid("OPTIONS", 3, icons, labels, actions, args, subs, en);
}

static void draw_controls_menu(void)
{
    static const int icons[] = {IC_TOUCH, IC_KEYBOARD, IC_GAMEPAD};
    static const char *const labels[] = {"TOUCH", "KEYBOARD", "GAMEPAD"};
    static const int actions[] = {A_LIST, A_LIST, A_LIST};
    static const int args[] = {SL_TOUCH, SL_KEYBOARD, SL_GAMEPAD};
    int kbd = K15_HasKeyboard(), pad = K15_Pad()->present;
    static const int en[] = {1, 1, 1};
    const char *subs[] = {"STICK, ESC HOLD", kbd ? "CONNECTED" : "NOT CONNECTED",
                          pad ? "CONNECTED" : "NOT CONNECTED"};
    draw_grid("CONTROLS", 3, icons, labels, actions, args, subs, en);
}

static void draw_setlist(void)
{
    item_t it[32];
    int n = list_items(setlist, it);
    header(list_title[setlist], list_note[setlist]);
    scroll_clamp();
    int rw = n > ROWS ? 912 : 1004;
    for (int r = 0; r < ROWS && scroll + r < n; r++) {
        const item_t *t = &it[scroll + r];
        int y = ROW_Y(r), right = 8 + rw - 8;
        button_t *b;
        G_Fill(8, y, rw, 56, COL_ROW);
        G_Frame(8, y, rw, 56, 1, 0x55160c);
        int over = t->kind != IT_ACTION && t->kind != IT_KEY && K15_CfgOverridden(t->id);
        switch (t->kind) {
        case IT_VALUE: {
            int v = K15_CfgInt(t->id), mn, mx, st;
            K15_CfgRange(t->id, &mn, &mx, &st);
            int wrap = K15_CfgEnumName(t->id, v) != NULL;
            b = add_btn(right - 56, y + 8, 56, 40, 3, ">", A_VINC, t->id, wrap || v < mx);
            b->nofocus = 1;
            b->row = r;
            b = add_btn(right - 56 - 8 - 260, y + 8, 260, 40, 3, fmt_value(t->id), A_VALUE,
                        t->id, 1);
            b->row = r;
            b = add_btn(right - 56 - 8 - 260 - 8 - 56, y + 8, 56, 40, 3, "<", A_VDEC, t->id,
                        wrap || v > mn);
            b->nofocus = 1;
            b->row = r;
            right -= 56 + 8 + 260 + 8 + 56;
            if (t->id == CFG_LAYOUT) {
                b = add_btn(right - 8 - 200, y + 8, 200, 40, 3, "PREVIEW", A_PREVIEW, 0, 1);
                b->row = r;
                right -= 8 + 200;
            }
            break;
        }
        case IT_TOGGLE:
            b = add_btn(right - 160, y + 8, 160, 40, 3, K15_CfgInt(t->id) ? "ON" : "OFF",
                        A_TOGGLE, t->id, 1);
            b->row = r;
            right -= 160;
            break;
        case IT_KEY: {
            int code = K15_CfgInt(CFG_KB_FIRST + t->id);
            b = add_btn(right - 120, y + 8, 120, 40, 3, "CLEAR", A_UNBIND, t->id, code > 0);
            b->row = r;
            b = add_btn(right - 120 - 8 - 220, y + 8, 220, 40, 3, K15_KeyName(code), A_BIND,
                        t->id, 1);
            b->label = pfmt("%s", K15_KeyName(code)); // K15_KeyName() reuses its buffer
            b->row = r;
            right -= 120 + 8 + 220;
            break;
        }
        case IT_ACTION:
            b = add_btn(right - 300, y + 8, 300, 40, 3, t->id == A_RESET ? "RESET" : "PREVIEW",
                        t->id, setlist, 1);
            b->row = r;
            right -= 300;
            break;
        }
        G_Text(20, y + (over ? 8 : 18), t->label, 3, COL_TEXT, right - 40);
        if (over)
            G_Text(20, y + 36, "SET BY THE USB STICK K15.CONF THIS RUN", 2, COL_DIM,
                   right - 40);
    }
    scroll_arrows(n);
}

static void label_8x8(int cx, int cy, const char *s, int size, int alpha)
{
    int scale = size == 2 ? 3 : 2;
    int x = cx - G_TextW(s, scale) / 2, y = cy - G_TextH(scale) / 2;
    G_TextA(x + 1, y + 1, s, scale, 0x000000, alpha / 2);
    G_TextA(x, y, s, scale, 0xffffff, alpha);
}

static void draw_preview(void)
{
    // a dim stand-in for the game view
    G_VGradient(0, 0, K15_SCR_W, K15_SCR_H, 0x3a3430, 0x1a1210);
    for (int x = 0; x < K15_SCR_W; x += 60)
        G_Fill(x, 150, 30, 90, 0x2a2420);
    uint32_t now = K15_NowMs(), flash = 0;
    for (int b = 0; b < TB_N; b++)
        if (pv_flash_at[b] && now - pv_flash_at[b] < FLASH_MS)
            flash |= 1u << b;
    K15_TouchDraw(G_Put, label_8x8, pv_held, flash, pv_esc_since ? now - pv_esc_since : 0);
    const k15_layout_t *L = K15_TouchLayout();
    int cx = (L->game_x0 + L->game_x1) / 2;
    G_TextCentre(cx, 70, pfmt("%s PREVIEW", K15_LayoutName(K15_CfgInt(CFG_LAYOUT))), 2,
                 COL_YELLOW);
    G_TextCentre(cx, 96, "TOUCH THE CONTROLS", 2, COL_TEXT);
    G_TextCentre(cx, 118, "TAP HERE TO RETURN", 2, COL_TEXT);
}

static void draw_modal(void)
{
    G_Blend(0, 0, K15_SCR_W, K15_SCR_H, 0x000000, 190);
    int x = MODAL_X, y = MODAL_Y, w = MODAL_W, h = MODAL_H;
    G_Fill(x, y, w, h, 0x1c0806);
    G_Frame(x, y, w, h, 3, COL_BORDER);
    // word-wrap at 42 columns (scale 2), honouring '\n'
    const int cols = (w - 40) / 16;
    char line[64];
    int ly = y + 16, n = 0;
    const char *p = modal_msg;
    while (*p && ly < y + h - 60) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len > cols) {
            len = cols;
            while (len > 0 && p[len] != ' ')
                len--;
            if (!len)
                len = cols;
        }
        snprintf(line, sizeof line, "%.*s", len, p);
        G_TextShadow(x + 20, ly, line, 2, n++ ? COL_TEXT : COL_YELLOW);
        ly += 22;
        p += len;
        while (*p == ' ' || *p == '\n')
            p++;
    }
    nbtn = 0; // only the modal's buttons are live
    if (modal == M_YESNO) {
        add_btn(x + w / 2 - 230, y + h - 58, 200, 46, 3, "YES", A_YES, 0, 1);
        add_btn(x + w / 2 + 30, y + h - 58, 200, 46, 3, "NO", A_NO, 0, 1);
    } else if (modal == M_USBWAD) {
        add_btn(x + w / 2 - 345, y + h - 58, 330, 46, 3, "INSTALL", A_MINSTALL, 0, 1);
        add_btn(x + w / 2 + 15, y + h - 58, 330, 46, 3, "PLAY FROM USB", A_MPLAYUSB, 0, 1);
    } else if (modal == M_CAPTURE) {
        add_btn(x + w / 2 - 100, y + h - 58, 200, 46, 3, "CANCEL", A_CANCEL, 0, 1);
    } else {
        add_btn(x + w / 2 - 100, y + h - 58, 200, 46, 3, "OK", A_OK, 0, 1);
    }
}

static void build(void)
{
    nbtn = 0;
    pool_pos = 0;
    switch (screen) {
    case S_HOME: draw_home(); break;
    case S_WADS: draw_wads(); break;
    case S_NOWAD: draw_nowad(); break;
    case S_OPTIONS: draw_options(); break;
    case S_CONTROLS: draw_controls_menu(); break;
    case S_SET: draw_setlist(); break;
    case S_PREVIEW: draw_preview(); return;
    }
    if (modal) {
        focus = -1;
        int p = pressed;
        pressed = -1; // the screen underneath is inert
        for (int i = 0; i < nbtn; i++)
            draw_btn(i);
        pressed = p;
        draw_modal();
    }
    resolve_focus();
    if (pressed >= nbtn)
        pressed = -1;
    for (int i = 0; i < nbtn; i++)
        draw_btn(i);
}

static void draw(void)
{
    dirty = 0;
    build();
    K15_FbPresent(k15_canvas);
}

static void draw_busy(const char *title, const char *name, long long done, long long total)
{
    char a[32], b[32], line[96];
    background();
    draw_logo_block(80);
    G_TextShadow(390, 30, title, 3, COL_YELLOW);
    G_Text(390, 66, name, 3, COL_TEXT, 600);
    if (total > 0) {
        G_Fill(390, 110, 590, 34, 0x200806);
        G_Frame(390, 110, 590, 34, 2, COL_BORDER);
        G_Fill(394, 114, (int)(582 * done / total), 26, 0xd03018);
        mb(a, sizeof a, done);
        mb(b, sizeof b, total);
        snprintf(line, sizeof line, "%s / %s", a, b);
        G_Text(390, 158, line, 2, COL_DIM, 0);
    }
    K15_FbPresent(k15_canvas);
}

// ---------------------------------------------------------------- actions
static int mkdirs(const char *dir)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

// Copies wads[idx] to the K15. quiet: no "INSTALLED" message. Returns 0 on success.
static int do_install(int idx, int quiet)
{
    k15_wad_t w = wads[idx];
    char need[32], have[32], tmp[300], dst[300];
    long long fb = free_bytes();
    if (fb >= 0 && fb < w.size + INSTALL_MARGIN) {
        mb(need, sizeof need, w.size + INSTALL_MARGIN);
        mb(have, sizeof have, fb);
        show_modal(M_OK, 0, 0, "NOT ENOUGH SPACE ON THE K15\nNEEDS %s (WAD + 5 MB), %s FREE.",
                   need, have);
        return -1;
    }
    if (mkdirs(K15_WadDir()) != 0) {
        show_modal(M_OK, 0, 0, "INSTALL FAILED\nCANNOT CREATE %s: %s", K15_WadDir(),
                   strerror(errno));
        return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.%s.part", K15_WadDir(), w.name);
    snprintf(dst, sizeof dst, "%s%s", K15_WadDir(), w.name);
    int in = open(w.path, O_RDONLY | O_CLOEXEC);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    static char buf[256 * 1024];
    long long done = 0;
    int err = in < 0 || out < 0 ? errno : 0;
    uint32_t last = 0;
    while (!err) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r < 0) {
            err = errno;
            break;
        }
        if (r == 0)
            break;
        for (ssize_t o = 0; o < r && !err;) {
            ssize_t wr = write(out, buf + o, r - o);
            if (wr < 0)
                err = errno;
            else
                o += wr;
        }
        done += r;
        if (K15_NowMs() - last > 100) {
            draw_busy("INSTALLING", w.name, done, w.size);
            last = K15_NowMs();
        }
    }
    if (!err && done != w.size)
        err = EIO;
    if (!err && fsync(out) != 0)
        err = errno;
    if (out >= 0 && close(out) != 0 && !err)
        err = errno;
    if (in >= 0)
        close(in);
    if (!err && rename(tmp, dst) != 0)
        err = errno;
    if (err) {
        unlink(tmp);
        show_modal(M_OK, 0, 0, "INSTALL FAILED\n%s: %s", w.name, strerror(err));
    } else {
        int dfd = open(K15_WadDir(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            fsync(dfd);
            close(dfd);
        }
        fprintf(stderr, "k15menu: installed %s -> %s\n", w.path, dst);
        if (!quiet)
            show_modal(M_OK, 0, 0, "INSTALLED %s\nIT NOW PLAYS WITHOUT THE USB STICK.",
                       w.name, NULL);
    }
    rescan();
    // Drop touches made while copying.
    K15_InputPump(NULL, NULL, NULL);
    touching = K15_TouchDown();
    pressed = -1;
    return err ? -1 : 0;
}

static void do_delete(const char *path)
{
    // Only ever delete inside the installed-WAD directory.
    size_t n = strlen(K15_WadDir());
    if (strncmp(path, K15_WadDir(), n) != 0 || strchr(path + n, '/'))
        return;
    if (unlink(path) != 0)
        show_modal(M_OK, 0, 0, "DELETE FAILED\n%s: %s", path + n, strerror(errno));
    else
        fprintf(stderr, "k15menu: deleted %s\n", path);
    rescan();
}

static void go(int s)
{
    // Leaving a settings screen saves k15.conf (atomically, only if something changed).
    if (screen == S_SET || screen == S_PREVIEW)
        K15_CfgSave();
    if (screen == S_WADS || screen == S_SET)
        screen_scroll[screen == S_SET] = scroll;
    int from = screen;
    screen = s;
    scroll = 0;
    if (s == S_WADS && from != S_HOME && from != S_NOWAD)
        scroll = screen_scroll[0];
    if (s == S_SET && from == S_PREVIEW)
        scroll = screen_scroll[1];
    pressed = -1;
    dirty = 1;
    if (s == S_PREVIEW) {
        pv_held = 0;
        pv_esc_since = 0;
        memset(pv_flash_at, 0, sizeof pv_flash_at);
    }
}

static void open_wads(void)
{
    rescan();
    go(nwads ? S_WADS : S_NOWAD);
}

static void play(int i, int from_home)
{
    if (!playable(i))
        return;
    play_idx = i;
    play_from_home = from_home;
    result = 1;
}

static void change_value(int id, int dir, int wrap)
{
    int v = K15_CfgInt(id), mn, mx, st;
    K15_CfgRange(id, &mn, &mx, &st);
    v += dir * st;
    if (v > mx)
        v = wrap ? mn : mx;
    if (v < mn)
        v = wrap ? mx : mn;
    K15_CfgSetInt(id, v);
    dirty = 1;
}

static void activate(int i)
{
    if (i < 0 || i >= nbtn || !btns[i].enabled)
        return;
    int arg = btns[i].arg, enum_like;
    dirty = 1;
    switch (btns[i].action) {
    case A_PLAYHOME: {
        rescan();
        int d, st = play_state(&d);
        if (st == PS_NONE) {
            go(S_NOWAD);
        } else if (st == PS_MANY || !playable(d)) {
            open_wads();
        } else if (st == PS_ONE && wads[d].usb) {
            snprintf(modal_path, sizeof modal_path, "%s", wads[d].path);
            show_modal(M_USBWAD, A_PLAYHOME, 0,
                       "%s IS ON THE USB STICK\nINSTALL IT ON THE K15 (IT BECOMES YOUR "
                       "FAVOURITE) OR PLAY IT FROM THE STICK?",
                       wads[d].title, NULL);
        } else {
            play(d, 1);
        }
        break;
    }
    case A_WADS:
        open_wads();
        break;
    case A_OPTIONS:
        go(S_OPTIONS);
        break;
    case A_CONTROLS:
        go(S_CONTROLS);
        break;
    case A_LIST:
        setlist = arg;
        go(S_SET);
        break;
    case A_EXIT:
        result = -1;
        break;
    case A_BACK:
        go(screen == S_CONTROLS ? S_OPTIONS
           : screen == S_SET ? (setlist == SL_LAYOUT || setlist == SL_FEATURES ? S_OPTIONS
                                                                               : S_CONTROLS)
           : S_HOME);
        break;
    case A_UP:
        if (scroll > 0)
            scroll--;
        break;
    case A_DOWN:
        if (scroll + ROWS < nlist())
            scroll++;
        break;
    case A_PLAY:
        play(arg, 0);
        break;
    case A_STAR:
        if (arg < nwads)
            K15_CfgSetStr(CFG_DEFAULT_WAD, is_default(arg) ? "" : wads[arg].name);
        K15_CfgSave();
        break;
    case A_INSTALL:
        do_install(arg, 0);
        break;
    case A_DELETE:
        snprintf(modal_path, sizeof modal_path, "%s", wads[arg].path);
        show_modal(M_YESNO, A_DELETE, arg, "DELETE %s?\nIT IS REMOVED FROM THE K15 (%s).",
                   wads[arg].name, K15_WadDir());
        break;
    case A_YES:
        close_modal();
        if (modal_action == A_DELETE)
            do_delete(modal_path);
        else if (modal_action == A_RESET) {
            switch (modal_arg) {
            case SL_LAYOUT: K15_CfgReset(CFG_LAYOUT, CFG_TRANSPARENCY); break;
            case SL_TOUCH: K15_CfgReset(CFG_JOY_DEAD, CFG_FLASH); break;
            case SL_KEYBOARD: K15_CfgReset(CFG_KBD, CFG_KB_FIRST + KB_N - 1); break;
            case SL_GAMEPAD: K15_CfgReset(CFG_PAD_FIRST, CFG_PAD_FIRST + PB_N - 1); break;
            case SL_FEATURES:
                K15_CfgReset(CFG_AUTOHIDE, CFG_FPS);
                K15_CfgReset(CFG_MUSIC_GAIN, CFG_DEBUG);
                break;
            }
        }
        break;
    case A_MINSTALL: {
        close_modal();
        int u = find_path(modal_path);
        if (u < 0 || do_install(u, 1) != 0)
            break;
        char name[64];
        snprintf(name, sizeof name, "%s", wads[u].name); // rescan reorders
        for (int j = 0; j < nwads; j++)
            if (!wads[j].usb && !strcasecmp(wads[j].name, name)) {
                K15_CfgSetStr(CFG_DEFAULT_WAD, wads[j].name);
                K15_CfgSave();
                play(j, 1);
                break;
            }
        break;
    }
    case A_MPLAYUSB: {
        close_modal();
        int u = find_path(modal_path);
        if (u >= 0)
            play(u, 1);
        break;
    }
    case A_OK:
    case A_NO:
    case A_CANCEL:
        close_modal();
        break;
    case A_VDEC:
    case A_VINC:
        enum_like = K15_CfgEnumName(arg, K15_CfgInt(arg)) != NULL;
        change_value(arg, btns[i].action == A_VINC ? 1 : -1, enum_like);
        break;
    case A_VALUE:
        change_value(arg, 1, 1);
        break;
    case A_TOGGLE:
        K15_CfgSetInt(arg, !K15_CfgInt(arg));
        break;
    case A_BIND:
        show_modal(M_CAPTURE, A_BIND, arg,
                   K15_HasKeyboard() ? "PRESS A KEY FOR %s\nESC OR CANCEL KEEPS: %s"
                                     : "PRESS A KEY FOR %s\nNO KEYBOARD IS CONNECTED. PLUG ONE "
                                       "IN, OR CANCEL TO KEEP: %s",
                   K15_KbActionName(arg), K15_KeyName(K15_CfgInt(CFG_KB_FIRST + arg)));
        break;
    case A_UNBIND:
        K15_CfgSetInt(CFG_KB_FIRST + arg, 0);
        break;
    case A_PREVIEW:
        go(S_PREVIEW);
        break;
    case A_RESET:
        show_modal(M_YESNO, A_RESET, arg, "RESET %s TO DEFAULTS?", list_title[arg], NULL);
        break;
    }
}

static void capture(int code)
{
    int a = modal_arg;
    for (int i = 0; i < KB_N; i++)
        if (i != a && K15_CfgInt(CFG_KB_FIRST + i) == code)
            K15_CfgSetInt(CFG_KB_FIRST + i, 0); // a key does one thing
    K15_CfgSetInt(CFG_KB_FIRST + a, code);
    fprintf(stderr, "k15menu: bound %s to %s\n", K15_KbActionName(a), K15_KeyName(code));
    close_modal();
}

static void back(void)
{
    dirty = 1;
    if (modal) {
        close_modal();
        return;
    }
    for (int i = 0; i < nbtn; i++)
        if (btns[i].action == A_BACK) {
            activate(i);
            return;
        }
    if (screen == S_PREVIEW)
        go(S_SET);
    else if (screen != S_HOME)
        go(S_HOME);
}

// Move the focus to the nearest button in direction (dx, dy); scroll lists at the edges.
static void nav(int dx, int dy)
{
    if (dirty)
        draw();
    if (focus < 0) {
        focus_on = 1;
        dirty = 1;
        resolve_focus();
        return;
    }
    const button_t *c = &btns[focus];
    int cx = c->x + c->w / 2, cy = c->y + c->h / 2, best = -1;
    long bs = 0;
    for (int i = 0; i < nbtn; i++) {
        if (i == focus || !focusable(i))
            continue;
        int bx = btns[i].x + btns[i].w / 2, by = btns[i].y + btns[i].h / 2;
        int p = dx ? (bx - cx) * dx : (by - cy) * dy;
        int s = dx ? abs(by - cy) : abs(bx - cx);
        if (p <= 4)
            continue;
        // left/right stay on the same row (the buttons overlap vertically)
        if (dx && abs(by - cy) * 2 >= c->h + btns[i].h)
            continue;
        long score = p + 3L * s;
        if (best < 0 || score < bs) {
            best = i;
            bs = score;
        }
    }
    int n = nlist(), row = c->row;
    int edge = dy > 0 ? row == ROWS - 1 || best < 0 : dy < 0 ? row == 0 : 0;
    if (dy && row >= 0 && edge && ((dy > 0 && scroll + ROWS < n) || (dy < 0 && scroll > 0))) {
        // Scroll one row and keep the column.
        scroll += dy;
        build();
        best = -1;
        bs = 0;
        for (int i = 0; i < nbtn; i++)
            if (focusable(i) && btns[i].row == row) {
                long d = labs((long)btns[i].x + btns[i].w / 2 - cx);
                if (best < 0 || d < bs) {
                    best = i;
                    bs = d;
                }
            }
    }
    if (best >= 0)
        set_focus(best);
    dirty = 1;
}

// 1-D devices (front panel / IR prev-next): step through the buttons in order.
static void nav_linear(int d)
{
    if (dirty)
        draw();
    if (!nbtn)
        return;
    int start = focus;
    focus_on = 1;
    int f = focus;
    for (int k = 0; k < nbtn; k++) {
        f = f < 0 ? (d > 0 ? 0 : nbtn - 1) : (f + d + nbtn) % nbtn;
        if (focusable(f) && f != start)
            break;
    }
    if (focusable(f))
        set_focus(f);
    dirty = 1;
}

// ---------------------------------------------------------------- input
static int hit(int x, int y)
{
    for (int i = nbtn - 1; i >= 0; i--)
        if (x >= btns[i].x - 6 && x < btns[i].x + btns[i].w + 6 && y >= btns[i].y - 6 &&
            y < btns[i].y + btns[i].h + 6)
            return i;
    return -1;
}

static void preview_touch(const k15_touches_t *t)
{
    uint32_t m = 0, now = K15_NowMs();
    for (int i = 0; i < t->n; i++)
        m |= K15_TouchHit(t->x[i], t->y[i]);
    if (t->n && !touching)
        pv_start_hit = m;
    for (int b = 0; b < TB_N; b++)
        if ((m & ~pv_held) & (1u << b))
            pv_flash_at[b] = now | 1;
    if ((m ^ pv_held) & (1u << TB_ESC))
        pv_esc_since = m & (1u << TB_ESC) ? now | 1 : 0;
    if (m != pv_held)
        dirty = 1;
    pv_held = m;
    if (t->n) {
        touching = 1;
        return;
    }
    // A tap that started on the view (or on nothing) returns.
    if (touching && !(pv_start_hit & ~(1u << TB_GAME)))
        go(S_SET);
    touching = 0;
}

static void on_touch(const k15_touches_t *t)
{
    if (screen == S_PREVIEW) {
        preview_touch(t);
        return;
    }
    if (dirty)
        draw();
    if (t->n > 0) {
        if (!touching) {
            pressed = hit(t->x[0], t->y[0]);
            touch_outside = modal == M_USBWAD &&
                            (t->x[0] < MODAL_X || t->x[0] >= MODAL_X + MODAL_W ||
                             t->y[0] < MODAL_Y || t->y[0] >= MODAL_Y + MODAL_H);
            if (focus_on) {
                focus_on = 0;
                focus = -1;
            }
            dirty = 1;
        } else if (pressed >= 0 && hit(t->x[0], t->y[0]) != pressed) {
            pressed = -1; // slid off: cancel
            dirty = 1;
        }
        touching = 1;
        return;
    }
    if (touching) {
        int p = pressed;
        touching = 0;
        pressed = -1;
        dirty = 1;
        if (touch_outside) {
            touch_outside = 0;
            close_modal();
        } else {
            activate(p);
        }
    }
}

static void adjust_focused(int dir)
{
    if (focus >= 0 && btns[focus].action == A_VALUE) {
        int id = btns[focus].arg;
        change_value(id, dir, K15_CfgEnumName(id, K15_CfgInt(id)) != NULL);
    } else if (focus >= 0 && btns[focus].action == A_TOGGLE) {
        activate(focus);
    } else {
        nav(dir, 0);
    }
}

static void on_key(int down, int code)
{
    if (!down)
        return;
    if (modal == M_CAPTURE && code != KEY_ESC && code > 0 && code < BTN_MISC &&
        !(code >= 249 && code <= 258)) { // not the K15's own front-panel keys
        capture(code);
        return;
    }
    if (screen == S_PREVIEW) {
        if (code == KEY_ESC || code == KEY_BACK || code == KEY_BACKSPACE || code == BTN_B ||
            code == KEY_ENTER || code == BTN_A || code == BTN_START)
            go(S_SET);
        return;
    }
    if (dirty)
        draw();
    switch (code) {
    case KEY_UP: case KEY_W: case BTN_DPAD_UP: case K15_KEY_LS_UP:
        nav(0, -1);
        break;
    case KEY_DOWN: case KEY_S: case BTN_DPAD_DOWN: case K15_KEY_LS_DOWN:
        nav(0, 1);
        break;
    case KEY_LEFT: case KEY_A: case BTN_DPAD_LEFT: case K15_KEY_LS_LEFT:
        adjust_focused(-1);
        break;
    case KEY_RIGHT: case KEY_D: case BTN_DPAD_RIGHT: case K15_KEY_LS_RIGHT:
        adjust_focused(1);
        break;
    case KEY_PREVIOUSSONG:
        nav_linear(-1);
        break;
    case KEY_NEXTSONG: case KEY_TAB:
        nav_linear(1);
        break;
    case KEY_ENTER: case KEY_KPENTER: case KEY_OK: case KEY_SELECT: case KEY_PLAYPAUSE:
    case KEY_SPACE: case BTN_A: case BTN_START:
        if (focus < 0)
            nav(0, 1);
        else
            activate(focus);
        break;
    case KEY_ESC: case KEY_BACK: case KEY_EXIT: case KEY_MENU: case KEY_BACKSPACE:
    case BTN_B: case BTN_SELECT:
        back();
        break;
    default:
        if (K15_CfgInt(CFG_DEBUG))
            fprintf(stderr, "k15menu: unmapped key %d\n", code);
    }
}

static void on_udp(int down, unsigned char k)
{
    static const struct { unsigned char dk; int ev; } map[] = {
        {DK_UP, KEY_UP}, {DK_DOWN, KEY_DOWN}, {DK_LEFT, KEY_LEFT}, {DK_RIGHT, KEY_RIGHT},
        {DK_ENTER, KEY_ENTER}, {DK_ESCAPE, KEY_ESC}, {'y', KEY_ENTER},
    };
    if (modal == M_CAPTURE && down && k >= 'a' && k <= 'z') { // dev: bind letters over UDP
        static const char qwerty[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
        for (int i = 0; i < (int)sizeof qwerty - 1; i++)
            if (qwerty[i] == k) {
                capture(KEY_Q + i);
                return;
            }
    }
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (map[i].dk == k)
            on_key(down, map[i].ev);
}

// ---------------------------------------------------------------- entry
int K15_Menu(int *argc, char ***argv)
{
    dev_build = getenv("K15_DEV_BUILD") && *getenv("K15_DEV_BUILD") == '1';
    rescan();
    const char *msg = getenv("K15_MENU_MSG");
    if (msg && *msg) {
        show_modal(M_OK, 0, 0, "%s", msg, NULL);
        unsetenv("K15_MENU_MSG");
    }
    const char *start = getenv("K15_MENU_SCREEN");
    if (start && (!strcmp(start, "wads") || !strcmp(start, "play") || !strcmp(start, "pick")))
        open_wads();
    unsetenv("K15_MENU_SCREEN");
    touching = K15_TouchDown();
    had_kbd = K15_HasKeyboard();
    had_pad = K15_Pad()->present;

    while (!result) {
        if ((screen == S_WADS || screen == S_NOWAD || screen == S_HOME) && !modal &&
            K15_NowMs() - last_scan > RESCAN_MS) {
            rescan();
            if (screen == S_NOWAD && nwads) {
                go(S_WADS);
            }
        }
        if (screen == S_WADS)
            scroll_clamp();
        int kbd = K15_HasKeyboard(), pad = K15_Pad()->present;
        if (kbd != had_kbd || pad != had_pad) {
            had_kbd = kbd;
            had_pad = pad;
            dirty = 1;
        }
        int anim = 0;
        if (screen == S_PREVIEW) {
            uint32_t now = K15_NowMs();
            for (int b = 0; b < TB_N; b++)
                anim |= pv_flash_at[b] && now - pv_flash_at[b] < FLASH_MS + 40;
            anim |= pv_esc_since != 0;
            if (pv_esc_since && now - pv_esc_since >= (uint32_t)K15_CfgInt(CFG_ESC_HOLD)) {
                pv_esc_since = 0;
                go(S_SET); // the ESC hold works here too
            }
            dirty |= anim;
        }
        if (dirty)
            draw();
        K15_InputWait(anim ? 30 : 250);
        K15_InputPump(on_touch, on_key, on_udp);
    }
    K15_CfgSave();
    if (result < 0)
        return 0;

    const k15_wad_t *w = &wads[play_idx];
    int base = w->iwad ? play_idx : K15_WadBaseFor(wads, nwads, w);
    static char *args[16];
    int n = 0;
    args[n++] = strdup("doomk15");
    args[n++] = strdup("-iwad");
    args[n++] = strdup(wads[base].path);
    if (!w->iwad) {
        args[n++] = strdup("-file");
        args[n++] = strdup(w->path);
    }
    char roots[K15_MAX_USB][64], cfg[300];
    int nr = K15_UsbRoots(roots, K15_MAX_USB);
    for (int r = 0; r < nr; r++) {
        snprintf(cfg, sizeof cfg, "%s/doom.cfg", roots[r]);
        if (access(cfg, R_OK) == 0) {
            args[n++] = strdup("-k15overlay");
            args[n++] = strdup(cfg);
            break;
        }
    }
    args[n] = NULL;
    // Where quitting the game comes back to.
    setenv("K15_MENU_RETURN", play_from_home ? "home" : "wads", 1);
    fprintf(stderr, "k15menu: play");
    for (int i = 1; i < n; i++)
        fprintf(stderr, " %s", args[i]);
    fputc('\n', stderr);
    draw_busy("LOADING", w->name, 0, 0);
    *argc = n;
    *argv = args;
    return 1;
}

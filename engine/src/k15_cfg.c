// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "k15_cfg.h"
#include "k15_wad.h"

enum { T_INT, T_BOOL, T_ENUM, T_STR };

typedef struct {
    char key[24];
    int type, def, min, max, step;
    const char *const *names; // T_ENUM
} setting_t;

static const char *const kb_names[KB_N] = {
    "FORWARD", "BACK", "TURN LEFT", "TURN RIGHT", "STRAFE LEFT", "STRAFE RIGHT",
    "FIRE", "USE", "RUN", "STRAFE", "PREV WEAPON", "NEXT WEAPON", "MAP"};
static const char *const kb_keys[KB_N] = {
    "FORWARD", "BACK", "TURNLEFT", "TURNRIGHT", "STRAFELEFT", "STRAFERIGHT",
    "FIRE", "USE", "RUN", "STRAFE", "PREVWPN", "NEXTWPN", "MAP"};
static const int kb_defs[KB_N] = {
    [KB_FORWARD] = KEY_W, [KB_BACK] = KEY_S, [KB_STRAFELEFT] = KEY_A,
    [KB_STRAFERIGHT] = KEY_D, [KB_PREVWPN] = KEY_LEFTBRACE, [KB_NEXTWPN] = KEY_RIGHTBRACE};

static const char *const pb_names[PB_N] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT",
                                           "L3", "R3", "BACK"};
static const char *const pa_names[PA_N] = {"NONE", "FIRE", "USE", "RUN", "STRAFE",
                                           "PREV WEAPON", "NEXT WEAPON", "MAP", "MENU"};
static const char *const pa_keys[PA_N] = {"NONE", "FIRE", "USE", "RUN", "STRAFE",
                                          "PREVWPN", "NEXTWPN", "MAP", "MENU"};
static const int pb_defs[PB_N] = {
    [PB_A] = PA_FIRE, [PB_B] = PA_USE, [PB_X] = PA_USE, [PB_Y] = PA_MAP,
    [PB_LB] = PA_PREVWPN, [PB_RB] = PA_NEXTWPN, [PB_LT] = PA_RUN, [PB_RT] = PA_FIRE,
    [PB_L3] = PA_RUN, [PB_R3] = PA_NONE, [PB_BACK] = PA_MAP};

static const char *const layout_names[LAYOUT_N] = {"CLASSIC", "LEFTY", "MINIMAL", "BIG"};

static setting_t tab[CFG_N] = {
    [CFG_TOUCH] = {"K15_TOUCH", T_INT, 0, 0, 4, 1},
    [CFG_MUSIC_GAIN] = {"K15_MUSIC_GAIN", T_INT, 1000, 0, 4000, 250},
    [CFG_DEBUG] = {"K15_DEBUG", T_BOOL, 0, 0, 1, 1},
    [CFG_SINGLEBUF] = {"K15_SINGLEBUF", T_BOOL, 0, 0, 1, 1},
    [CFG_DEFAULT_WAD] = {"K15_DEFAULT_WAD", T_STR},
    [CFG_LAYOUT] = {"K15_LAYOUT", T_ENUM, LAYOUT_CLASSIC, 0, LAYOUT_N - 1, 1, layout_names},
    [CFG_TRANSPARENCY] = {"K15_TRANSPARENCY", T_INT, 50, 0, 90, 10},
    [CFG_JOY_DEAD] = {"K15_JOY_DEAD", T_INT, 22, 10, 50, 4},
    [CFG_TAP_USE] = {"K15_TAP_USE", T_BOOL, 1, 0, 1, 1},
    [CFG_ESC_HOLD] = {"K15_ESC_HOLD", T_INT, 3000, 1000, 6000, 500},
    [CFG_FLASH] = {"K15_FLASH", T_BOOL, 1, 0, 1, 1},
    [CFG_KBD] = {"K15_KEYBOARD", T_BOOL, 1, 0, 1, 1},
    [CFG_AUTOHIDE] = {"K15_AUTOHIDE", T_BOOL, 1, 0, 1, 1},
    [CFG_CHEATS] = {"K15_CHEATS", T_BOOL, 1, 0, 1, 1},
    [CFG_FPS] = {"K15_SHOW_FPS", T_BOOL, 0, 0, 1, 1},
};

static int saved[CFG_N], eff[CFG_N], over[CFG_N];
static char saved_str[256], eff_str[256];
static int inited, changed;

static void init(void)
{
    if (inited)
        return;
    inited = 1;
    for (int a = 0; a < KB_N; a++)
        tab[CFG_KB_FIRST + a] = (setting_t){"", T_INT, kb_defs[a], 0, KEY_MAX, 1};
    for (int a = 0; a < KB_N; a++)
        snprintf(tab[CFG_KB_FIRST + a].key, sizeof tab[0].key, "K15_KB_%s", kb_keys[a]);
    for (int b = 0; b < PB_N; b++)
        tab[CFG_PAD_FIRST + b] = (setting_t){"", T_ENUM, pb_defs[b], 0, PA_N - 1, 1, pa_keys};
    for (int b = 0; b < PB_N; b++)
        snprintf(tab[CFG_PAD_FIRST + b].key, sizeof tab[0].key, "K15_PAD_%s", pb_names[b]);
    for (int i = 0; i < CFG_N; i++)
        saved[i] = eff[i] = tab[i].def;
}

static int parse(int id, const char *v, int *out)
{
    const setting_t *s = &tab[id];
    if (s->type == T_ENUM) {
        for (int i = s->min; i <= s->max; i++)
            if (!strcasecmp(v, s->names[i])) {
                *out = i;
                return 0;
            }
    }
    char *end;
    long n = strtol(v, &end, 0);
    if (end == v || *end)
        return -1;
    *out = n < s->min ? s->min : n > s->max ? s->max : (int)n;
    return 0;
}

static void load_file(const char *path, int is_saved)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        char *k = line;
        while (*k == ' ' || *k == '\t')
            k++;
        char *eq = strchr(k, '=');
        if (*k == '#' || !eq)
            continue;
        *eq = 0;
        char *v = eq + 1, *end = eq;
        while (end > k && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = 0;
        while (*v == ' ' || *v == '\t' || *v == '"')
            v++;
        end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' ||
                           end[-1] == '"'))
            *--end = 0;
        int id = -1, n = 0;
        for (int i = 0; i < CFG_N && id < 0; i++)
            if (!strcmp(k, tab[i].key))
                id = i;
        if (id < 0) {
            fprintf(stderr, "k15: %s: ignoring %s\n", path, k);
            continue;
        }
        if (tab[id].type == T_STR) {
            if (strchr(v, '/')) // a file name, never a path
                continue;
            snprintf(eff_str, sizeof eff_str, "%s", v);
            if (is_saved)
                snprintf(saved_str, sizeof saved_str, "%s", v);
        } else if (parse(id, v, &n) == 0) {
            eff[id] = n;
            if (is_saved)
                saved[id] = n;
            else
                over[id] = 1;
        } else {
            fprintf(stderr, "k15: %s: bad value %s=%s\n", path, k, v);
            continue;
        }
        fprintf(stderr, "k15: %s: %s=%s\n", path, k, v);
    }
    fclose(f);
}

void K15_CfgLoad(void)
{
    init();
    // Environment first (adb/dev use), then the files.
    static const int env_ids[] = {CFG_TOUCH, CFG_MUSIC_GAIN, CFG_DEBUG, CFG_SINGLEBUF};
    for (size_t i = 0; i < sizeof env_ids / sizeof env_ids[0]; i++) {
        const char *v = getenv(tab[env_ids[i]].key);
        int n;
        if (v && parse(env_ids[i], v, &n) == 0)
            eff[env_ids[i]] = n;
    }
    char path[300], roots[K15_MAX_USB][64];
    snprintf(path, sizeof path, "%sk15.conf", K15_ConfigDirPath());
    load_file(path, 1);
    int n = K15_UsbRoots(roots, K15_MAX_USB);
    for (int i = 0; i < n; i++) {
        snprintf(path, sizeof path, "%s/k15.conf", roots[i]);
        load_file(path, 0);
    }
}

int K15_CfgInt(int id)
{
    init();
    return id >= 0 && id < CFG_N ? eff[id] : 0;
}

const char *K15_CfgStr(int id)
{
    (void)id;
    return eff_str;
}

void K15_CfgSetInt(int id, int v)
{
    init();
    if (id < 0 || id >= CFG_N || tab[id].type == T_STR)
        return;
    v = v < tab[id].min ? tab[id].min : v > tab[id].max ? tab[id].max : v;
    changed |= saved[id] != v;
    saved[id] = eff[id] = v;
    over[id] = 0;
}

void K15_CfgSetStr(int id, const char *s)
{
    (void)id;
    if (strchr(s, '/'))
        return;
    changed |= strcmp(saved_str, s) != 0;
    snprintf(saved_str, sizeof saved_str, "%s", s);
    snprintf(eff_str, sizeof eff_str, "%s", s);
}

int K15_CfgDefault(int id)
{
    init();
    return tab[id].def;
}

void K15_CfgReset(int first, int last)
{
    for (int i = first; i <= last; i++)
        if (tab[i].type != T_STR)
            K15_CfgSetInt(i, tab[i].def);
}

int K15_CfgOverridden(int id)
{
    return over[id];
}

void K15_CfgRange(int id, int *min, int *max, int *step)
{
    init();
    *min = tab[id].min;
    *max = tab[id].max;
    *step = tab[id].step;
}

const char *K15_CfgEnumName(int id, int v)
{
    init();
    return tab[id].type == T_ENUM && v >= tab[id].min && v <= tab[id].max ? tab[id].names[v]
                                                                         : NULL;
}

int K15_CfgSave(void)
{
    if (!changed)
        return 0;
    char path[300], tmp[310];
    snprintf(path, sizeof path, "%sk15.conf", K15_ConfigDirPath());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "k15: %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    fprintf(f, "# DOOM settings, written by the DOOM menu (unknown keys are dropped).\n"
               "# A k15.conf on a USB stick root overrides these for one run.\n");
    for (int i = 0; i < CFG_N; i++) {
        const setting_t *s = &tab[i];
        if (s->type == T_STR)
            fprintf(f, "%s=%s\n", s->key, saved_str);
        else if (s->type == T_ENUM)
            fprintf(f, "%s=%s\n", s->key, s->names[saved[i]]);
        else
            fprintf(f, "%s=%d\n", s->key, saved[i]);
    }
    int err = fflush(f) || fsync(fileno(f));
    err |= fclose(f) != 0;
    if (err || rename(tmp, path) != 0) {
        fprintf(stderr, "k15: saving %s failed: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    int dfd = open(K15_ConfigDirPath(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    changed = 0;
    fprintf(stderr, "k15: saved %s\n", path);
    return 0;
}

const char *K15_KbActionName(int a) { return a >= 0 && a < KB_N ? kb_names[a] : "?"; }
const char *K15_PadButtonName(int b) { return b >= 0 && b < PB_N ? pb_names[b] : "?"; }
const char *K15_PadActionName(int a) { return a >= 0 && a < PA_N ? pa_names[a] : "?"; }
const char *K15_LayoutName(int l) { return l >= 0 && l < LAYOUT_N ? layout_names[l] : "?"; }

const char *K15_KeyName(int c)
{
    static char buf[16];
    static const char row1[] = "1234567890-=", qrow[] = "QWERTYUIOP[]", arow[] = "ASDFGHJKL;'`",
                      zrow[] = "\\ZXCVBNM,./";
    if (c <= 0)
        return "-";
    if (c >= KEY_1 && c <= KEY_EQUAL)
        snprintf(buf, sizeof buf, "%c", row1[c - KEY_1]);
    else if (c >= KEY_Q && c <= KEY_RIGHTBRACE)
        snprintf(buf, sizeof buf, "%c", qrow[c - KEY_Q]);
    else if (c >= KEY_A && c <= KEY_GRAVE)
        snprintf(buf, sizeof buf, "%c", arow[c - KEY_A]);
    else if (c >= KEY_BACKSLASH && c <= KEY_SLASH)
        snprintf(buf, sizeof buf, "%c", zrow[c - KEY_BACKSLASH]);
    else if (c >= KEY_F1 && c <= KEY_F10)
        snprintf(buf, sizeof buf, "F%d", c - KEY_F1 + 1);
    else {
        static const struct { int c; const char *n; } names[] = {
            {KEY_ESC, "ESC"}, {KEY_BACKSPACE, "BKSP"}, {KEY_TAB, "TAB"}, {KEY_ENTER, "ENTER"},
            {KEY_LEFTCTRL, "LCTRL"}, {KEY_RIGHTCTRL, "RCTRL"}, {KEY_LEFTSHIFT, "LSHIFT"},
            {KEY_RIGHTSHIFT, "RSHIFT"}, {KEY_LEFTALT, "LALT"}, {KEY_RIGHTALT, "RALT"},
            {KEY_SPACE, "SPACE"}, {KEY_CAPSLOCK, "CAPS"}, {KEY_F11, "F11"}, {KEY_F12, "F12"},
            {KEY_UP, "UP"}, {KEY_DOWN, "DOWN"}, {KEY_LEFT, "LEFT"}, {KEY_RIGHT, "RIGHT"},
            {KEY_HOME, "HOME"}, {KEY_END, "END"}, {KEY_PAGEUP, "PGUP"}, {KEY_PAGEDOWN, "PGDN"},
            {KEY_INSERT, "INS"}, {KEY_DELETE, "DEL"}, {KEY_KPENTER, "KP ENTER"},
            {KEY_LEFTMETA, "LMETA"}, {KEY_RIGHTMETA, "RMETA"}, {KEY_KP0, "KP 0"},
        };
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
            if (names[i].c == c)
                return names[i].n;
        snprintf(buf, sizeof buf, "KEY %d", c);
    }
    return buf;
}

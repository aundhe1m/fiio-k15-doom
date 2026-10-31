// SPDX-License-Identifier: GPL-2.0-or-later
// Host harness: runs the K15 DOOM home menu (k15_menu.c) on a PC with fake platform
// functions, feeds it a scripted input sequence and writes every presented frame as a PPM.
// No device needed. See run.sh.
//
// Script: whitespace-separated tokens, one per input poll:
//   u d l r      arrow keys          e  Enter        b  Esc (back)       n  next (TAB)
//   k=CODE       evdev key press     t=X:Y  tap (down + up) at landscape (X, Y)
//   p=X:Y        touch down only     R  touch up     .  idle poll
// Env: HARNESS_KBD=1 (a keyboard is "plugged in"), HARNESS_PAD=1, HARNESS_OUT=<dir>,
//      HARNESS_USB=<dir> (a fake USB stick root).
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "k15_plat.h"

static char *script;
static int frame;
static int down, down_x, down_y, release_next;

void K15_FbPresent(const uint32_t *c)
{
    char name[512];
    const char *dir = getenv("HARNESS_OUT");
    snprintf(name, sizeof name, "%s/frame%03d.ppm", dir ? dir : ".", frame++);
    FILE *f = fopen(name, "wb");
    if (!f) {
        perror(name);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", K15_SCR_W, K15_SCR_H);
    for (int i = 0; i < K15_SCR_W * K15_SCR_H; i++) {
        unsigned char px[3] = {c[i] >> 16, c[i] >> 8, c[i]};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}
void K15_FbOpen(void) {}
uint32_t *K15_FbBack(void) { return NULL; }
void K15_FbFlip(void) {}
int K15_FbRestore(void) { return 0; }
void K15_InputOpen(void) {}
int K15_InputWait(int t) { (void)t; return 0; }
int K15_TouchDown(void) { return down; }
void K15_InputPrepareExec(void) {}
void K15_InputKeyboard(int on) { (void)on; }
int K15_Debug(void) { return 0; }
int K15_HasKeyboard(void) { return getenv("HARNESS_KBD") != NULL; }
const k15_pad_t *K15_Pad(void)
{
    static k15_pad_t p;
    p.present = getenv("HARNESS_PAD") != NULL;
    return &p;
}
// Overrides the weak /proc/mounts reader in k15_wad.c.
int K15_UsbRoots(char roots[][64], int max)
{
    const char *d = getenv("HARNESS_USB");
    if (!d || !*d || max < 1)
        return 0;
    snprintf(roots[0], 64, "%s", d);
    return 1;
}

uint32_t K15_NowMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void touch(k15_touch_fn cb, int n, int x, int y)
{
    k15_touches_t t = {0};
    t.n = n;
    t.x[0] = x;
    t.y[0] = y;
    down = n;
    if (cb)
        cb(&t);
}

void K15_InputPump(k15_touch_fn tcb, k15_key_fn key, k15_udp_fn udp)
{
    if (!udp) // the menu's own "drop pending input" call
        return;
    if (release_next) {
        release_next = 0;
        touch(tcb, 0, down_x, down_y);
        return;
    }
    char *tok = strtok(script, " \t\n");
    script = NULL;
    if (!tok)
        exit(0);
    int x, y;
    if (sscanf(tok, "t=%d:%d", &x, &y) == 2) {
        down_x = x;
        down_y = y;
        touch(tcb, 1, x, y);
        release_next = 1;
    } else if (sscanf(tok, "p=%d:%d", &x, &y) == 2) {
        down_x = x;
        down_y = y;
        touch(tcb, 1, x, y);
    } else if (!strcmp(tok, "R")) {
        touch(tcb, 0, down_x, down_y);
    } else if (!strncmp(tok, "k=", 2)) {
        key(1, atoi(tok + 2));
        key(0, atoi(tok + 2));
    } else if (strlen(tok) == 1 && strchr("udlrebn", *tok)) {
        static const int map[128] = {['u'] = KEY_UP, ['d'] = KEY_DOWN, ['l'] = KEY_LEFT,
                                     ['r'] = KEY_RIGHT, ['e'] = KEY_ENTER, ['b'] = KEY_ESC,
                                     ['n'] = KEY_TAB};
        key(1, map[(int)*tok]);
        key(0, map[(int)*tok]);
    }
}

int K15_Menu(int *argc, char ***argv);
void K15_CfgLoad(void);

int main(int argc, char **argv)
{
    static char buf[8192];
    snprintf(buf, sizeof buf, "%s", argc > 1 ? argv[1] : "");
    script = buf;
    char **a;
    int n;
    K15_CfgLoad();
    if (K15_Menu(&n, &a)) {
        printf("PLAY");
        for (int i = 1; i < n; i++)
            printf(" %s", a[i]);
        printf("\nRETURN %s\n", getenv("K15_MENU_RETURN"));
    } else {
        printf("EXIT\n");
    }
    return 0;
}

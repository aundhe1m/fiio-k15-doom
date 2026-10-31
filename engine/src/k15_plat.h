// SPDX-License-Identifier: GPL-2.0-or-later
// FiiO K15 platform layer shared by the game backend and the DOOM home menu:
// fb0 page flipping (pages 1-2, page 0 belongs to the stock UI) and grabbed evdev input.
#pragma once
#include <stdint.h>

#define K15_PANEL_W 240
#define K15_PANEL_H 1020
#define K15_SCR_W K15_PANEL_H // landscape
#define K15_SCR_H K15_PANEL_W
#define K15_NPIX (K15_PANEL_W * K15_PANEL_H)
#define K15_MAX_TOUCH 10

// Framebuffer. K15_FbOpen() exits the process on failure (nothing to show anything on).
void K15_FbOpen(void);
uint32_t *K15_FbBack(void);       // next page, panel order: landscape (x, y) = [x*240 + 239-y]
void K15_FbFlip(void);            // show the page returned by K15_FbBack()
void K15_FbPresent(const uint32_t *canvas); // landscape 0xRRGGBB canvas -> back page -> flip
int K15_FbRestore(void);          // pan back to page 0 (the stock UI)

// Input. Devices are opened O_CLOEXEC and grabbed so the stock UI underneath never sees
// our taps. K15_InputPrepareExec() hands the grabbed fds to an execv()'d copy of us
// (via K15_EVFDS) so there is no window in which a tap could leak to the stock UI.
typedef struct {
    int n;
    int x[K15_MAX_TOUCH], y[K15_MAX_TOUCH]; // landscape screen coords
} k15_touches_t;

typedef void (*k15_touch_fn)(const k15_touches_t *t);
typedef void (*k15_key_fn)(int pressed, int evcode);        // evdev KEY_* code
typedef void (*k15_udp_fn)(int pressed, unsigned char doomkey);

// Gamepad (evdev pad or userspace XInput pad). Buttons arrive through k15_key_fn as evdev
// codes with Xbox labels (BTN_A/B/X/Y, BTN_TL/TR, BTN_START/SELECT, BTN_THUMBL/R,
// BTN_DPAD_*); the left stick also as the K15_KEY_LS_* codes below and the triggers as
// BTN_TL2/BTN_TR2. Analog values: K15_Pad().
#define K15_KEY_LS_UP 0x2f0
#define K15_KEY_LS_DOWN 0x2f1
#define K15_KEY_LS_LEFT 0x2f2
#define K15_KEY_LS_RIGHT 0x2f3
typedef struct {
    int present;
    int lx, ly, rx, ry; // -32767..32767, up/left negative
    int lt, rt;         // 0..255
} k15_pad_t;
const k15_pad_t *K15_Pad(void);

// Input devices are (re)scanned every second while K15_InputPump() runs: hotplugged
// keyboards and pads are grabbed on arrival and dropped when unplugged.
void K15_InputOpen(void);
void K15_InputPump(k15_touch_fn touch, k15_key_fn key, k15_udp_fn udp);
int K15_InputWait(int timeout_ms); // poll() all inputs; >0 if something is readable
int K15_TouchDown(void);           // any finger currently on the screen
int K15_HasKeyboard(void);         // a keyboard (letters + space) is plugged in
void K15_InputKeyboard(int on);    // off: keyboards stay grabbed but their keys are dropped
void K15_InputPrepareExec(void);
int K15_Debug(void);

uint32_t K15_NowMs(void);

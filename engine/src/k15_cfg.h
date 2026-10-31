// SPDX-License-Identifier: GPL-2.0-or-later
// K15 backend settings (k15.conf, KEY=VALUE). Loaded from the config dir (the saved values,
// written back by the home menu with an atomic temp + rename) and then from a USB stick
// root (overrides for this run only, never saved). Only the keys below are accepted.
#pragma once

// Keyboard actions with a rebindable key (an extra key next to the fixed arrows/Ctrl/...).
enum {
    KB_FORWARD, KB_BACK, KB_TURNLEFT, KB_TURNRIGHT, KB_STRAFELEFT, KB_STRAFERIGHT,
    KB_FIRE, KB_USE, KB_RUN, KB_STRAFE, KB_PREVWPN, KB_NEXTWPN, KB_MAP, KB_N
};

// Pad buttons with a rebindable action (D-pad = arrows and Start = Esc are fixed).
enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT, PB_L3, PB_R3, PB_BACK, PB_N };
enum { PA_NONE, PA_FIRE, PA_USE, PA_RUN, PA_STRAFE, PA_PREVWPN, PA_NEXTWPN, PA_MAP, PA_MENU,
       PA_N };

enum { LAYOUT_CLASSIC, LAYOUT_LEFTY, LAYOUT_MINIMAL, LAYOUT_BIG, LAYOUT_N };

enum {
    CFG_TOUCH,       // touch coordinate mapping 0-4
    CFG_MUSIC_GAIN,  // OPL music gain, 1000 = 1.0
    CFG_DEBUG,       // verbose doom.log
    CFG_SINGLEBUF,   // no page flipping
    CFG_DEFAULT_WAD, // file name PLAY starts directly (string)
    CFG_LAYOUT,      // touch layout preset
    CFG_TRANSPARENCY, // touch controls: 0 = most solid, 50 = the original look, 90 = faint
    CFG_JOY_DEAD,    // joystick deadzone, px
    CFG_TAP_USE,     // tapping the view = USE
    CFG_ESC_HOLD,    // ESC hold-to-exit, ms
    CFG_FLASH,       // flash a touch control when it is pressed
    CFG_KBD,         // keyboards work in the game
    CFG_KB_FIRST,
    CFG_PAD_FIRST = CFG_KB_FIRST + KB_N,
    CFG_AUTOHIDE = CFG_PAD_FIRST + PB_N, // hide the touch controls on keyboard/pad use
    CFG_CHEATS,      // cheat overlay (long-press MAP / hold pad Back)
    CFG_FPS,         // FPS counter
    CFG_N
};

void K15_CfgLoad(void);                 // config dir, then USB stick roots
int K15_CfgInt(int id);                 // effective value
const char *K15_CfgStr(int id);
void K15_CfgSetInt(int id, int v);      // sets the saved and the effective value
void K15_CfgSetStr(int id, const char *s);
int K15_CfgDefault(int id);
void K15_CfgReset(int first, int last); // back to defaults (inclusive range)
int K15_CfgOverridden(int id);          // a stick k15.conf overrides it this run
int K15_CfgSave(void);                  // if changed; 0 = ok

// Range/step for numeric settings, names for enums (NULL if not an enum).
void K15_CfgRange(int id, int *min, int *max, int *step);
const char *K15_CfgEnumName(int id, int v);

const char *K15_KbActionName(int a);  // "FORWARD"
const char *K15_PadButtonName(int b); // "A", "LB", ...
const char *K15_PadActionName(int a); // "FIRE"
const char *K15_LayoutName(int l);    // "CLASSIC"
const char *K15_KeyName(int evcode);  // "W", "SPACE", "KEY 183"

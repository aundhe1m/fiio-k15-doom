// SPDX-License-Identifier: GPL-2.0-or-later
// WAD discovery for the K15 DOOM menu: USB stick roots (mounted by mq_player at
// /tmp/udisk or /tmp/udisk<N>) and WADs installed to /usr/data/doom/wads/.
#pragma once

#define K15_MAX_WADS 48
#define K15_MAX_USB 4

enum {
    WAD_UNKNOWN,
    WAD_DOOM_SW,  // shareware (E1 only) - cannot load add-ons
    WAD_DOOM,     // registered (E1-E3)
    WAD_DOOM_U,   // Ultimate DOOM (E4)
    WAD_DOOM2,    // DOOM II / Final DOOM / Freedoom phase 2 (MAPxx)
};

typedef struct {
    char path[256];
    char name[64];      // file name
    long long size;
    long mtime;
    int usb;            // on a USB stick (else installed on the device)
    int iwad;           // IWAD (playable on its own) vs PWAD (add-on)
    int kind;           // WAD_* (for a PWAD: which maps it replaces)
    char desc[40];      // e.g. "DOOM II", "ADD-ON (DOOM II MAPS)"
    char title[40];     // PLAY label: known IWAD name, else the file name without .wad
} k15_wad_t;

const char *K15_ConfigDirPath(void); // "/usr/data/doom/" (K15_HOME overrides)
const char *K15_WadDir(void);        // "<config dir>wads/"

// Mounted USB stick roots. Returns the count.
int K15_UsbRoots(char roots[][64], int max);

// Scan the installed-WAD directory and USB roots. A stick WAD with the same file name as
// an installed one (or one on an earlier stick) is left out. Returns the count, sorted
// (IWADs first, then by name).
int K15_WadScan(k15_wad_t *out, int max);

// Best IWAD to run a PWAD with (index into list), or -1.
int K15_WadBaseFor(const k15_wad_t *list, int n, const k15_wad_t *pwad);

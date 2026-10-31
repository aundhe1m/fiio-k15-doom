// SPDX-License-Identifier: GPL-2.0-or-later
// DOOM home menu for the FiiO K15, shown before any WAD is loaded.
#pragma once

// Runs the menu on the (already open) framebuffer and input devices.
// Returns 1 with a new argc/argv for doomgeneric_Create() to start a game, or 0 to exit.
int K15_Menu(int *argc, char ***argv);

// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef K15_SOUND_H
#define K15_SOUND_H

// Put the DAC-path GPIO back the way we found it (safe to call from a signal handler).
void K15_SoundRestore(void);

#endif

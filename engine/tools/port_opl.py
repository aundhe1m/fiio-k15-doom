#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn Chocolate Doom 2.3.0's opl/opl_sdl.c into opl_k15.c (pthread, no SDL).
Usage: port_opl.py <choco/opl/opl_sdl.c> <out.c>"""
import re
import sys

src = open(sys.argv[1]).read()

src = src.replace('#include "SDL.h"\n#include "SDL_mixer.h"\n', '#include <pthread.h>\n')
src = src.replace("OPL SDL interface.", "OPL software (Nuked OPL3) driver for the K15 mixer thread.")

# Mutexes: SDL mutexes are recursive; keep that behaviour.
src = src.replace("static SDL_mutex *callback_mutex = NULL;",
                  "static pthread_mutex_t callback_mutex;")
src = src.replace("static SDL_mutex *callback_queue_mutex = NULL;",
                  "static pthread_mutex_t callback_queue_mutex;")
src = re.sub(r"SDL_LockMutex\((\w+)\)", r"pthread_mutex_lock(&\1)", src)
src = re.sub(r"SDL_UnlockMutex\((\w+)\)", r"pthread_mutex_unlock(&\1)", src)

# Drop the SDL "is initialised" probe and parameters.
src = re.sub(r"// SDL parameters\.\n.*?static int SDLIsInitialized\(void\)\n\{.*?\n\}\n",
             "static int mixing_freq = 44100, mixing_channels = 2;\n", src, flags=re.S)

# Mix callback -> exported pull function (stereo s16 interleaved, `frames` frames).
src = src.replace(
    "static void OPL_Mix_Callback(void *udata,\n                             Uint8 *byte_buffer,\n"
    "                             int buffer_bytes)",
    "void OPL_K15_Mix(int16_t *byte_buffer, int frames)")
src = src.replace("static uint64_t current_time;", "static uint64_t current_time;\nstatic int k15_opl_up;", 1)
src = src.replace("    unsigned int filled = 0;\n", "    unsigned int filled = 0;\n\n    if (!k15_opl_up)\n        return;\n", 1)
src = src.replace("buffer = (int16_t *) byte_buffer;\n    buffer_len = buffer_bytes / 4;",
                  "buffer = byte_buffer;\n    buffer_len = frames;")

# Shutdown.
src = re.sub(r"static void OPL_SDL_Shutdown\(void\)\n\{.*?\n\}\n",
             "static void OPL_SDL_Shutdown(void)\n{\n"
             "    if (k15_opl_up)\n    {\n"
             "        k15_opl_up = 0;\n"
             "        OPL_Queue_Destroy(callback_queue);\n"
             "        free(mix_buffer);\n"
             "        mix_buffer = NULL;\n"
             "    }\n}\n", src, flags=re.S)

# Init.
src = re.sub(r"static unsigned int GetSliceSize\(void\)\n\{.*?\n\}\n", "", src, flags=re.S)
init = """static int OPL_SDL_Init(unsigned int port_base)
{
    pthread_mutexattr_t attr;

    opl_sdl_paused = 0;
    pause_offset = 0;
    callback_queue = OPL_Queue_Create();
    current_time = 0;
    mix_buffer = malloc(mixing_freq * sizeof(uint32_t) * 2);
    OPL3_Reset(&opl_chip, mixing_freq);
    opl_opl3mode = 0;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&callback_mutex, &attr);
    pthread_mutex_init(&callback_queue_mutex, &attr);
    k15_opl_up = 1;
    return 1;
}
"""
src = re.sub(r"static int OPL_SDL_Init\(unsigned int port_base\)\n\{.*?\n\}\n", init, src,
             flags=re.S)
src = src.replace('"SDL",', '"K15",')

assert "SDL_" not in src.replace("OPL_SDL_", "").replace("opl_sdl_", ""), \
    [l for l in src.splitlines() if "SDL_" in l.replace("OPL_SDL_", "").replace("opl_sdl_", "")]
open(sys.argv[2], "w").write(src)

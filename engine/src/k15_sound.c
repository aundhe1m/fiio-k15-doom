// SPDX-License-Identifier: GPL-2.0-or-later
// K15 sound: DOOM DMX sfx + OPL music (Nuked OPL3 via i_oplmusic.c) mixed to 44.1 kHz 24-bit stereo and streamed to the
// SoC->DAC PCM (hw:0,4) through the device's own aplay.
//
// The DAC path is only live while GPIO PC28 is high (the stock player raises it whenever it
// plays). We raise it for the duration of the game and restore the previous level on exit.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#include "k15_cfg.h"
#include "k15_sound.h"

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

#define RATE 44100
#define NCHAN 16
#define FRAMES 512 // per mix chunk (~11.6 ms)
#define MUSIC_GAIN_DEFAULT 1000 // s16 OPL output -> mix scale (sfx peak is ~255x s16)
#define APLAY "aplay -q -D hw:0,4 -f S24_3LE -r 44100 -c 2 -t raw -B 60000 -F 15000 -"

// GPIO port C, Ingenic X2000: PxPIN @0x200, PxPAT0S @0x244, PxPAT0C @0x248
#define GPIO_BASE 0x10010000
#define PC_PIN 0x200
#define PC_PAT0S 0x244
#define PC_PAT0C 0x248
#define PC28 (1u << 28)

typedef struct {
    const uint8_t *data; // unsigned 8-bit mono samples
    uint32_t len;
    uint32_t step;       // 16.16 source samples per output sample
} k15_sfx_t;

typedef struct {
    const k15_sfx_t *sfx;
    uint32_t pos;        // 16.16
    int left, right;     // 0..255
} k15_chan_t;

static k15_chan_t chans[NCHAN];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static volatile int running;
static FILE *out;
static int out_dead;
static int music_gain = MUSIC_GAIN_DEFAULT;
static volatile uint32_t *gpio;
static int pc28_was_high = -1;
static boolean use_prefix;

// Config variables normally owned by the SDL sound backend (bound in i_sound.c).
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

// ---------------------------------------------------------------- PC28 (DAC path enable)
static void pc28_raise(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("k15snd: /dev/mem");
        return;
    }
    void *m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, GPIO_BASE);
    close(fd);
    if (m == MAP_FAILED) {
        perror("k15snd: mmap gpio");
        return;
    }
    gpio = m;
    pc28_was_high = !!(gpio[PC_PIN / 4] & PC28);
    gpio[PC_PAT0S / 4] = PC28;
    fprintf(stderr, "k15snd: PC28 %d -> 1\n", pc28_was_high);
}

void K15_SoundRestore(void)
{
    if (gpio && pc28_was_high == 0)
        gpio[PC_PAT0C / 4] = PC28;
    pc28_was_high = -1;
}

// ---------------------------------------------------------------- mixer thread
void OPL_K15_Mix(int16_t *buffer, int frames);

static void *mix_thread(void *arg)
{
    (void)arg;
    static int32_t acc[FRAMES * 2];
    static int16_t music[FRAMES * 2];
    static uint8_t buf[FRAMES * 2 * 3];
    unsigned chunk = 0;
    int reasserts = 0;
    while (running) {
        // The stock player may drop PC28 behind our back; keep the DAC path open.
        if (gpio && (++chunk % 43) == 0 && !(gpio[PC_PIN / 4] & PC28)) {
            gpio[PC_PAT0S / 4] = PC28;
            if (reasserts++ < 20)
                fprintf(stderr, "k15snd: PC28 was dropped, re-raised (t=%us)\n",
                        chunk * FRAMES / RATE);
        }
        memset(acc, 0, sizeof acc);
        // Always advance the OPL emulator: its timer callbacks drive the music
        // sequencer and the OPL chip detection at startup.
        memset(music, 0, sizeof music);
        OPL_K15_Mix(music, FRAMES);
        for (int i = 0; i < FRAMES * 2; i++)
            acc[i] = music[i] * music_gain;
        pthread_mutex_lock(&lock);
        for (int c = 0; c < NCHAN; c++) {
            k15_chan_t *ch = &chans[c];
            if (!ch->sfx)
                continue;
            const k15_sfx_t *s = ch->sfx;
            for (int i = 0; i < FRAMES; i++) {
                uint32_t idx = ch->pos >> 16;
                if (idx >= s->len) {
                    ch->sfx = NULL;
                    break;
                }
                int v = ((int)s->data[idx] - 128) << 8; // 16-bit range
                acc[i * 2] += v * ch->left;
                acc[i * 2 + 1] += v * ch->right;
                ch->pos += s->step;
            }
        }
        pthread_mutex_unlock(&lock);
        // acc is 16-bit * 0..255 => ~24-bit; scale down a bit for headroom across channels
        for (int i = 0; i < FRAMES * 2; i++) {
            int32_t v = acc[i] >> 1;
            if (v > 8388607)
                v = 8388607;
            else if (v < -8388608)
                v = -8388608;
            buf[i * 3] = v & 0xFF;
            buf[i * 3 + 1] = (v >> 8) & 0xFF;
            buf[i * 3 + 2] = (v >> 16) & 0xFF;
        }
        if (out && fwrite(buf, sizeof buf, 1, out) != 1) {
            fprintf(stderr, "k15snd: audio output gone (device busy?), continuing silently\n");
            out_dead = 1;
        }
        if (!out || out_dead)
            usleep(FRAMES * 1000000 / RATE);
    }
    return NULL;
}

// ---------------------------------------------------------------- sound module
static boolean snd_init(boolean prefix)
{
    use_prefix = prefix;
    music_gain = K15_CfgInt(CFG_MUSIC_GAIN);
    signal(SIGPIPE, SIG_IGN);
    pc28_raise();
    out = popen(APLAY, "w");
    if (out) {
        fcntl(fileno(out), F_SETPIPE_SZ, 4096);
        setvbuf(out, NULL, _IONBF, 0);
    } else {
        perror("k15snd: popen aplay");
    }
    running = 1;
    if (pthread_create(&mixer, NULL, mix_thread, NULL)) {
        if (out)
            pclose(out);
        K15_SoundRestore();
        return false;
    }
    fprintf(stderr, "k15snd: sfx on hw:0,4 @ %d Hz\n", RATE);
    return true;
}

static void snd_shutdown(void)
{
    if (!running)
        return;
    running = 0;
    pthread_join(mixer, NULL);
    if (out)
        pclose(out);
    K15_SoundRestore();
}

static int snd_lumpnum(sfxinfo_t *sfx)
{
    char name[16];
    if (sfx->link)
        sfx = sfx->link;
    if (use_prefix)
        snprintf(name, sizeof name, "ds%s", sfx->name);
    else
        snprintf(name, sizeof name, "%s", sfx->name);
    return W_GetNumForName(name);
}

// Parse a DMX sound lump into a k15_sfx_t (cached in sfxinfo->driver_data).
static const k15_sfx_t *load_sfx(sfxinfo_t *sfx)
{
    if (sfx->driver_data)
        return sfx->driver_data;
    int lump = sfx->lumpnum;
    const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);
    int lumplen = W_LumpLength(lump);
    if (lumplen < 8 || d[0] != 0x03 || d[1] != 0x00)
        return NULL;
    uint32_t rate = d[2] | (d[3] << 8);
    uint32_t len = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
    if (len > (uint32_t)lumplen - 8 || len <= 48)
        return NULL;
    k15_sfx_t *s = Z_Malloc(sizeof *s, PU_STATIC, NULL);
    // DMX pads 16 bytes at each end of the sample data.
    s->data = d + 8 + 16;
    s->len = len - 32;
    s->step = (uint32_t)(((uint64_t)rate << 16) / RATE);
    sfx->driver_data = s;
    return s;
}

static void set_params(k15_chan_t *ch, int vol, int sep)
{
    // vol 0..127, sep 0..254 (128 = centre), as in Chocolate Doom
    int l = ((254 - sep) * vol) / 127;
    int r = (sep * vol) / 127;
    ch->left = l > 255 ? 255 : (l < 0 ? 0 : l);
    ch->right = r > 255 ? 255 : (r < 0 ? 0 : r);
}

static int snd_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NCHAN)
        return -1;
    const k15_sfx_t *s = load_sfx(sfx);
    if (!s)
        return -1;
    pthread_mutex_lock(&lock);
    chans[channel].sfx = s;
    chans[channel].pos = 0;
    set_params(&chans[channel], vol, sep);
    pthread_mutex_unlock(&lock);
    return channel;
}

static void snd_stop(int channel)
{
    if (channel < 0 || channel >= NCHAN)
        return;
    pthread_mutex_lock(&lock);
    chans[channel].sfx = NULL;
    pthread_mutex_unlock(&lock);
}

static boolean snd_playing(int channel)
{
    if (channel < 0 || channel >= NCHAN)
        return false;
    return chans[channel].sfx != NULL;
}

static void snd_update_params(int channel, int vol, int sep)
{
    if (channel < 0 || channel >= NCHAN)
        return;
    pthread_mutex_lock(&lock);
    set_params(&chans[channel], vol, sep);
    pthread_mutex_unlock(&lock);
}

static void snd_update(void)
{
}

static void snd_cache(sfxinfo_t *sounds, int num)
{
    (void)sounds;
    (void)num;
}

static snddevice_t devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
                                SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
    devices, sizeof devices / sizeof devices[0],
    snd_init, snd_shutdown, snd_lumpnum, snd_update, snd_update_params,
    snd_start, snd_stop, snd_playing, snd_cache,
};

// SPDX-License-Identifier: GPL-2.0-or-later
// FiiO K15 platform layer: framebuffer page flipping + grabbed evdev/XInput/UDP input.
//
// Display: fb0 is a 240x1020 portrait panel mounted 90deg CCW (1020x240 landscape) with
// 3 pages of virtual height. mq_ui only ever draws page 0 and never pans, so we draw
// pages 1-2, page-flip with FBIOPAN_DISPLAY and pan back to 0 to give the screen back.
//
// Touch (axs_ts) reports native portrait coordinates; K15_TOUCH selects another mapping
// in case a unit differs:
//   0 (default, verified): sx = y, sy = 239 - x      1: sx = y, sy = x
//   2: sx = 1019 - y, sy = x    3: sx = 1019 - y, sy = 239 - x    4: already landscape

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/usbdevice_fs.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "k15_cfg.h"
#include "k15_plat.h"

#define FB_DEV "/dev/fb0"
#define FIRST_BUF 1
#define UDP_PORT 6666
#define MAX_DEVS 16
#define RESCAN_MS 1000

static uint32_t *fb_mem;
static int fb_fd = -1;
static int nbuf, cur_buf;

int K15_Debug(void)
{
    return K15_CfgInt(CFG_DEBUG);
}

uint32_t K15_NowMs(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// ---------------------------------------------------------------- framebuffer
static int pan_to(int fd, int b)
{
    struct fb_var_screeninfo var;
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0)
        return -1;
    var.yoffset = b * K15_PANEL_H;
    return ioctl(fd, FBIOPAN_DISPLAY, &var);
}

int K15_FbRestore(void)
{
    if (fb_fd >= 0)
        return pan_to(fb_fd, 0);
    int fd = open(FB_DEV, O_RDWR | O_CLOEXEC);
    int r = pan_to(fd, 0);
    if (fd >= 0)
        close(fd);
    return r;
}

void K15_FbOpen(void)
{
    if (fb_mem)
        return;
    fb_fd = open(FB_DEV, O_RDWR | O_CLOEXEC);
    if (fb_fd < 0) {
        perror("k15: " FB_DEV);
        exit(1);
    }
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    ioctl(fb_fd, FBIOGET_VSCREENINFO, &var);
    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fix);
    fprintf(stderr, "k15: fb0 %ux%u virt %ux%u bpp %u line %u smem %u pan %u\n", var.xres,
            var.yres, var.xres_virtual, var.yres_virtual, var.bits_per_pixel,
            fix.line_length, fix.smem_len, var.yoffset);
    if (var.xres != K15_PANEL_W || var.yres != K15_PANEL_H || var.bits_per_pixel != 32 ||
        fix.line_length != K15_PANEL_W * 4) {
        fprintf(stderr, "k15: unexpected fb geometry\n");
        exit(1);
    }
    nbuf = var.yres_virtual / K15_PANEL_H - FIRST_BUF;
    if (nbuf > 2)
        nbuf = 2;
    if (K15_CfgInt(CFG_SINGLEBUF))
        nbuf = 1;
    size_t len = (size_t)(FIRST_BUF + nbuf) * K15_NPIX * 4;
    if (nbuf < 1 || fix.smem_len < len) {
        fprintf(stderr, "k15: no spare fb page\n");
        exit(1);
    }
    fb_mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("k15: mmap");
        exit(1);
    }
    // After an execv() from the game we may already be showing one of our pages: keep
    // it on screen and draw into the other one.
    int shown = var.yoffset / K15_PANEL_H - FIRST_BUF;
    cur_buf = (shown >= 0 && shown < nbuf) ? shown : 0;
    if (shown < 0 || shown >= nbuf) {
        for (int b = 0; b < nbuf; b++)
            memset(fb_mem + (size_t)(FIRST_BUF + b) * K15_NPIX, 0, K15_NPIX * 4);
    }
}

uint32_t *K15_FbBack(void)
{
    int b = nbuf > 1 ? cur_buf ^ 1 : 0;
    return fb_mem + (size_t)(FIRST_BUF + b) * K15_NPIX;
}

void K15_FbFlip(void)
{
    int b = nbuf > 1 ? cur_buf ^ 1 : 0;
    if (pan_to(fb_fd, FIRST_BUF + b) == 0)
        cur_buf = b;
}

void K15_FbPresent(const uint32_t *canvas)
{
    uint32_t *out = K15_FbBack();
    for (int sx = 0; sx < K15_SCR_W; sx++) {
        const uint32_t *src = canvas + sx;
        for (int sy = K15_SCR_H - 1; sy >= 0; sy--)
            *out++ = src[sy * K15_SCR_W];
    }
    K15_FbFlip();
}

// ---------------------------------------------------------------- input
// Devices are classified by capabilities, not by node number (a keyboard is event3 when
// wired but event4 behind its dongle, where event3 is a mouse). /dev/input is rescanned
// every second while we run, so keyboards and pads can be plugged in mid-game; new nodes
// are grabbed straight away and dropped again on ENODEV.
//
// Gamepads: an evdev pad (HID/D-input mode) or an XInput pad (vendor class ff/5d/01, no
// kernel driver on the K15) driven from userspace through usbfs. Both feed one pad state:
// buttons go out through the key callback as evdev BTN_* codes (Xbox labels, like xpad),
// the left stick additionally as K15_KEY_LS_* and the triggers as BTN_TL2/BTN_TR2;
// analog values are read with K15_Pad().
enum { DEV_OTHER, DEV_TOUCH, DEV_PAD };
static struct dev {
    int fd, kind, kbd;
    uint16_t vid, pid; // USB ids (EVIOCGID)
    dev_t rdev;
    struct input_absinfo tx, ty;  // touch
    struct input_absinfo abs[6];  // pad: ABS_X, Y, Z, RX, RY, RZ
} devs[MAX_DEVS];
static int ndevs;
static struct pollfd pfds[MAX_DEVS + 2];
static int npfds = -1, udp_slot = -1, xi_slot = -1; // npfds < 0: rebuild
static struct { int x, y, active; } slots[K15_MAX_TOUCH];
static int cur_slot;
static int udp_fd = -1;
static int touch_map, kbd_on = 1;
static uint32_t last_scan;

static k15_pad_t pad;
static uint8_t pad_held[(KEY_MAX + 1) / 8];
static int pad_hat[2];

static struct {
    int fd, ep_in, ep_out, pkt, busy, vid, pid;
    uint16_t btn;
    struct usbdevfs_urb urb;
    unsigned char buf[64];
} xi = {.fd = -1};

#define TEST_BIT(b, a) (((a)[(b) / (8 * sizeof(long))] >> ((b) % (8 * sizeof(long)))) & 1)

const k15_pad_t *K15_Pad(void)
{
    return &pad;
}

// Emit a pad "key" only on a state change, and remember it so a pad that disappears
// mid-press can release everything (no stuck fire/run).
static void pad_key(int code, int down, k15_key_fn key)
{
    if (code < 0 || code > KEY_MAX)
        return;
    int was = (pad_held[code / 8] >> (code % 8)) & 1;
    if (was == !!down)
        return;
    pad_held[code / 8] ^= 1 << (code % 8);
    if (K15_Debug())
        fprintf(stderr, "pad key %#x %s\n", code, down ? "down" : "up");
    if (key)
        key(down, code);
}

static void pad_release_all(k15_key_fn key)
{
    for (int c = 0; c <= KEY_MAX; c++)
        pad_key(c, 0, key);
    memset(&pad, 0, sizeof pad);
    pad_hat[0] = pad_hat[1] = 0;
}

// Derived digital events: left-stick directions (with hysteresis) and triggers.
static void pad_derive(k15_key_fn key)
{
    static const struct { int code, *v, sign; } dirs[] = {
        {K15_KEY_LS_UP, &pad.ly, -1}, {K15_KEY_LS_DOWN, &pad.ly, 1},
        {K15_KEY_LS_LEFT, &pad.lx, -1}, {K15_KEY_LS_RIGHT, &pad.lx, 1},
    };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        int v = *dirs[i].v * dirs[i].sign;
        int held = (pad_held[dirs[i].code / 8] >> (dirs[i].code % 8)) & 1;
        pad_key(dirs[i].code, held ? v > 12000 : v > 20000, key);
    }
    int lt = (pad_held[BTN_TL2 / 8] >> (BTN_TL2 % 8)) & 1;
    int rt = (pad_held[BTN_TR2 / 8] >> (BTN_TR2 % 8)) & 1;
    pad_key(BTN_TL2, lt ? pad.lt > 40 : pad.lt > 100, key);
    pad_key(BTN_TR2, rt ? pad.rt > 40 : pad.rt > 100, key);
}

static int norm_axis(int v, const struct input_absinfo *a)
{
    long range = (long)a->maximum - a->minimum;
    if (range <= 0)
        return 0;
    long c = (long)v * 2 - a->maximum - a->minimum; // -range..range
    long r = c * 32767 / range;
    return r > 32767 ? 32767 : r < -32767 ? -32767 : (int)r;
}

static int norm_trigger(int v, const struct input_absinfo *a)
{
    long range = (long)a->maximum - a->minimum;
    return range > 0 ? (int)(((long)v - a->minimum) * 255 / range) : 0;
}

// ---------------------------------------------------------------- XInput over usbfs
static void xi_close(k15_key_fn key)
{
    if (xi.fd < 0)
        return;
    close(xi.fd); // kills the pending URB and releases the interface
    xi.fd = -1;
    xi.busy = 0;
    xi.btn = 0;
    npfds = -1;
    pad_release_all(key);
    for (int d = 0; d < ndevs; d++)
        pad.present |= devs[d].kind == DEV_PAD;
    fprintf(stderr, "k15: XInput pad gone\n");
}

static int xi_submit(void)
{
    memset(&xi.urb, 0, sizeof xi.urb);
    xi.urb.type = USBDEVFS_URB_TYPE_INTERRUPT;
    xi.urb.endpoint = xi.ep_in;
    xi.urb.buffer = xi.buf;
    xi.urb.buffer_length = xi.pkt;
    xi.busy = ioctl(xi.fd, USBDEVFS_SUBMITURB, &xi.urb) == 0;
    return xi.busy;
}

static int read_sysfs(const char *dir, const char *file, char *buf, size_t n)
{
    char p[300];
    snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/%s", dir, file);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t r = read(fd, buf, n - 1);
    close(fd);
    if (r <= 0)
        return -1;
    buf[r] = 0;
    return 0;
}

static int sysfs_hex(const char *dir, const char *file)
{
    char b[16];
    return read_sysfs(dir, file, b, sizeof b) ? -1 : (int)strtol(b, NULL, 16);
}

static int xi_open(const char *intf)
{
    char dev[256], b[16];
    snprintf(dev, sizeof dev, "%s", intf);
    char *colon = strchr(dev, ':');
    if (!colon)
        return -1;
    *colon = 0;
    int ifno = sysfs_hex(intf, "bInterfaceNumber");
    if (ifno < 0 || read_sysfs(dev, "busnum", b, sizeof b))
        return -1;
    int bus = atoi(b);
    if (read_sysfs(dev, "devnum", b, sizeof b))
        return -1;
    char path[40];
    snprintf(path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, atoi(b));
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    // Interrupt endpoints of our interface (alt setting 0) from the raw descriptors.
    unsigned char d[1024];
    int len = read(fd, d, sizeof d), cur = -1, ein = -1, eout = -1, pkt = 32;
    for (int o = 0; o + 2 <= len && d[o] >= 2 && o + d[o] <= len; o += d[o]) {
        if (d[o + 1] == 4 && d[o] >= 4)
            cur = d[o + 3] == 0 ? d[o + 2] : -1;
        else if (d[o + 1] == 5 && d[o] >= 6 && cur == ifno && (d[o + 3] & 3) == 3) {
            if ((d[o + 2] & 0x80) && ein < 0) {
                ein = d[o + 2];
                pkt = d[o + 4] | d[o + 5] << 8;
            } else if (!(d[o + 2] & 0x80) && eout < 0) {
                eout = d[o + 2];
            }
        }
    }
    struct usbdevfs_ioctl dc = {.ifno = ifno, .ioctl_code = USBDEVFS_DISCONNECT};
    unsigned int claim = ifno;
    if (ein < 0 || (ioctl(fd, USBDEVFS_IOCTL, &dc), ioctl(fd, USBDEVFS_CLAIMINTERFACE, &claim))) {
        fprintf(stderr, "k15: %s: XInput claim failed (%s)\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    xi.fd = fd;
    xi.vid = sysfs_hex(dev, "idVendor");
    xi.pid = sysfs_hex(dev, "idProduct");
    xi.ep_in = ein;
    xi.ep_out = eout;
    xi.pkt = pkt < 20 ? 20 : pkt > (int)sizeof xi.buf ? (int)sizeof xi.buf : pkt;
    if (eout >= 0) {
        // Xbox 360 LED pattern "player 1": the pad leaves its "waiting for host" mode.
        unsigned char led[3] = {0x01, 0x03, 0x02};
        struct usbdevfs_bulktransfer bt = {.ep = eout, .len = 3, .timeout = 200, .data = led};
        ioctl(fd, USBDEVFS_BULK, &bt);
    }
    if (!xi_submit()) {
        fprintf(stderr, "k15: %s: XInput submit failed (%s)\n", path, strerror(errno));
        close(fd);
        xi.fd = -1;
        return -1;
    }
    memset(&pad, 0, sizeof pad);
    pad.present = 1;
    npfds = -1;
    fprintf(stderr, "k15: XInput pad %s intf %d IN %#x OUT %#x (%d B)\n", path, ifno, ein, eout,
            xi.pkt);
    return 0;
}

static void xi_scan(void)
{
    if (xi.fd >= 0)
        return;
    DIR *dir = opendir("/sys/bus/usb/devices");
    if (!dir)
        return;
    struct dirent *e;
    while ((e = readdir(dir)) && xi.fd < 0) {
        if (!strchr(e->d_name, ':'))
            continue;
        if (sysfs_hex(e->d_name, "bInterfaceClass") == 0xff &&
            sysfs_hex(e->d_name, "bInterfaceSubClass") == 0x5d &&
            sysfs_hex(e->d_name, "bInterfaceProtocol") == 0x01)
            xi_open(e->d_name);
    }
    closedir(dir);
}

static void xi_report(const unsigned char *b, int n, k15_key_fn key)
{
    if (n < 14 || b[0] != 0x00 || b[1] < 14)
        return;
    static const short bits[16] = {
        BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT, BTN_START, BTN_SELECT,
        BTN_THUMBL, BTN_THUMBR, BTN_TL, BTN_TR, BTN_MODE, -1, BTN_A, BTN_B, BTN_X, BTN_Y,
    };
    uint16_t btn = b[2] | b[3] << 8;
    pad.lt = b[4];
    pad.rt = b[5];
    pad.lx = (int16_t)(b[6] | b[7] << 8);
    pad.ly = -(int16_t)(b[8] | b[9] << 8); // XInput: up = +; ours: up = -
    pad.rx = (int16_t)(b[10] | b[11] << 8);
    pad.ry = -(int16_t)(b[12] | b[13] << 8);
#define CLAMP(v) v = v > 32767 ? 32767 : v < -32767 ? -32767 : v
    CLAMP(pad.lx);
    CLAMP(pad.ly);
    CLAMP(pad.rx);
    CLAMP(pad.ry);
#undef CLAMP
    for (int i = 0; i < 16; i++)
        if (bits[i] >= 0 && ((btn ^ xi.btn) >> i & 1))
            pad_key(bits[i], btn >> i & 1, key);
    xi.btn = btn;
    pad_derive(key);
}

static void xi_pump(k15_key_fn key)
{
    if (xi.fd < 0 || xi_slot < 0 || !pfds[xi_slot].revents)
        return;
    if (pfds[xi_slot].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        xi_close(key);
        return;
    }
    struct usbdevfs_urb *u;
    while (ioctl(xi.fd, USBDEVFS_REAPURBNDELAY, &u) == 0) {
        xi.busy = 0;
        if (u->status == 0)
            xi_report(xi.buf, u->actual_length, key);
        else if (u->status != -EOVERFLOW && u->status != -EPIPE) {
            fprintf(stderr, "k15: XInput URB status %d\n", u->status);
            xi_close(key);
            return;
        }
        if (!xi_submit()) {
            xi_close(key);
            return;
        }
    }
    if (errno == ENODEV)
        xi_close(key);
}

// ---------------------------------------------------------------- evdev devices
static void add_dev(int fd, const char *path, int inherited)
{
    char name[64] = "?";
    unsigned long keyb[KEY_MAX / (8 * sizeof(long)) + 1] = {0};
    unsigned long absb[ABS_MAX / (8 * sizeof(long)) + 1] = {0};
    struct stat st;
    struct dev *v = &devs[ndevs++];
    memset(v, 0, sizeof *v);
    v->fd = fd;
    v->rdev = fstat(fd, &st) == 0 ? st.st_rdev : 0;
    npfds = -1;
    ioctl(fd, EVIOCGNAME(sizeof name), name);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keyb), keyb);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absb), absb);
    unsigned long props[1] = {0};
    ioctl(fd, EVIOCGPROP(sizeof props), props);
    // A touchscreen is a direct-input device or reports BTN_TOUCH (axs_ts does both). Some
    // keyboards expose a generic HID collection with every ABS axis, MT ones included.
    int touchish = TEST_BIT(INPUT_PROP_DIRECT, props) || TEST_BIT(BTN_TOUCH, keyb);
    int grabbed = inherited || ioctl(fd, EVIOCGRAB, 1) == 0;
    if (touchish && TEST_BIT(ABS_MT_POSITION_X, absb) &&
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &v->tx) == 0 &&
        ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &v->ty) == 0 && v->tx.maximum > 0)
        v->kind = DEV_TOUCH;
    else if (TEST_BIT(BTN_GAMEPAD, keyb) || TEST_BIT(BTN_JOYSTICK, keyb))
        v->kind = DEV_PAD;
    else if (touchish && TEST_BIT(ABS_X, absb) &&
             ioctl(fd, EVIOCGABS(ABS_X), &v->tx) == 0 &&
             ioctl(fd, EVIOCGABS(ABS_Y), &v->ty) == 0 && v->tx.maximum > 0)
        v->kind = DEV_TOUCH;
    // A keyboard (letters + space), as opposed to the front panel or a consumer-key node.
    v->kbd = v->kind == DEV_OTHER && TEST_BIT(KEY_A, keyb) && TEST_BIT(KEY_Z, keyb) &&
             TEST_BIT(KEY_SPACE, keyb);
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) == 0) {
        v->vid = id.vendor;
        v->pid = id.product;
    }
    if (v->kind == DEV_PAD) {
        for (int a = ABS_X; a <= ABS_RZ; a++)
            if (TEST_BIT(a, absb))
                ioctl(fd, EVIOCGABS(a), &v->abs[a]);
        pad.present = 1;
    }
    static const char *kinds[] = {"other", "touch", "pad"};
    fprintf(stderr, "k15: %s \"%s\" %s=%d %s%s", path, name, inherited ? "inherited" : "grab",
            grabbed, kinds[v->kind], v->kbd ? " keyboard" : "");
    if (v->kind == DEV_TOUCH)
        fprintf(stderr, " x=[%d,%d] y=[%d,%d]", v->tx.minimum, v->tx.maximum, v->ty.minimum,
                v->ty.maximum);
    fputc('\n', stderr);
}

static void drop_dev(int d, k15_key_fn key)
{
    fprintf(stderr, "k15: input device %d (fd %d) gone\n", d, devs[d].fd);
    int was_pad = devs[d].kind == DEV_PAD;
    close(devs[d].fd);
    memmove(&devs[d], &devs[d + 1], (ndevs - d - 1) * sizeof devs[0]);
    ndevs--;
    npfds = -1;
    if (was_pad && xi.fd < 0) {
        int other = 0;
        for (int i = 0; i < ndevs; i++)
            other |= devs[i].kind == DEV_PAD;
        if (!other)
            pad_release_all(key);
    }
}

static void rescan(void)
{
    last_scan = K15_NowMs();
    DIR *dir = opendir("/dev/input");
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir)) && ndevs < MAX_DEVS) {
            if (strncmp(e->d_name, "event", 5))
                continue;
            char path[300];
            struct stat st;
            snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
            if (stat(path, &st) || !S_ISCHR(st.st_mode))
                continue;
            int known = 0;
            for (int d = 0; d < ndevs; d++)
                known |= devs[d].rdev == st.st_rdev;
            if (known)
                continue;
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0)
                add_dev(fd, path, 0);
        }
        closedir(dir);
    }
    xi_scan();
}

static void open_udp(void)
{
    udp_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(UDP_PORT)};
    int one = 1;
    if (udp_fd >= 0)
        setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (udp_fd < 0 || bind(udp_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("k15: udp");
        if (udp_fd >= 0)
            close(udp_fd);
        udp_fd = -1;
        return;
    }
    fprintf(stderr, "k15: UDP keyboard on port %d\n", UDP_PORT);
}

void K15_InputOpen(void)
{
    static int opened;
    if (opened)
        return;
    opened = 1;
    touch_map = K15_CfgInt(CFG_TOUCH);
    const char *inh = getenv("K15_EVFDS");
    if (inh && *inh) {
        // Grabbed fds handed over by our previous incarnation (see K15_InputPrepareExec).
        char *s = strdup(inh), *save = NULL;
        for (char *t = strtok_r(s, ",", &save); t && ndevs < MAX_DEVS;
             t = strtok_r(NULL, ",", &save)) {
            int fd = atoi(t);
            if (fd > 2 && fcntl(fd, F_GETFD) >= 0) {
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                add_dev(fd, t, 1);
            }
        }
        free(s);
        unsetenv("K15_EVFDS");
    }
    rescan();
    open_udp();
}

void K15_InputPrepareExec(void)
{
    char buf[128] = "";
    for (int d = 0; d < ndevs; d++) {
        fcntl(devs[d].fd, F_SETFD, 0);
        snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "%s%d", d ? "," : "",
                 devs[d].fd);
    }
    setenv("K15_EVFDS", buf, 1);
}

static int scale_axis(int v, struct input_absinfo *a, int out)
{
    int range = a->maximum - a->minimum;
    if (range <= 0)
        return v;
    return (int)((long long)(v - a->minimum) * (out - 1) / range);
}

static void touch_xy(int dev, int rx, int ry, int *sx, int *sy)
{
    struct dev *v = &devs[dev];
    if (touch_map == 4) {
        *sx = scale_axis(rx, &v->tx, K15_SCR_W);
        *sy = scale_axis(ry, &v->ty, K15_SCR_H);
        return;
    }
    int px = scale_axis(rx, &v->tx, K15_PANEL_W);
    int py = scale_axis(ry, &v->ty, K15_PANEL_H);
    switch (touch_map) {
    case 1: *sx = py; *sy = px; break;
    case 2: *sx = K15_PANEL_H - 1 - py; *sy = px; break;
    case 3: *sx = K15_PANEL_H - 1 - py; *sy = K15_PANEL_W - 1 - px; break;
    default: *sx = py; *sy = K15_PANEL_W - 1 - px; break;
    }
}

int K15_TouchDown(void)
{
    for (int s = 0; s < K15_MAX_TOUCH; s++)
        if (slots[s].active)
            return 1;
    return 0;
}

static void touch_sync(int dev, k15_touch_fn cb)
{
    k15_touches_t t = {0};
    for (int s = 0; s < K15_MAX_TOUCH; s++) {
        if (!slots[s].active)
            continue;
        touch_xy(dev, slots[s].x, slots[s].y, &t.x[t.n], &t.y[t.n]);
        if (K15_Debug())
            fprintf(stderr, "touch slot %d raw %d,%d -> screen %d,%d\n", s, slots[s].x,
                    slots[s].y, t.x[t.n], t.y[t.n]);
        t.n++;
    }
    if (cb)
        cb(&t);
}

static void pad_abs(struct dev *v, int code, int value, k15_key_fn key)
{
    switch (code) {
    case ABS_X: pad.lx = norm_axis(value, &v->abs[ABS_X]); break;
    case ABS_Y: pad.ly = norm_axis(value, &v->abs[ABS_Y]); break;
    case ABS_RX: pad.rx = norm_axis(value, &v->abs[ABS_RX]); break;
    case ABS_RY: pad.ry = norm_axis(value, &v->abs[ABS_RY]); break;
    case ABS_Z: pad.lt = norm_trigger(value, &v->abs[ABS_Z]); break;
    case ABS_RZ: pad.rt = norm_trigger(value, &v->abs[ABS_RZ]); break;
    case ABS_HAT0X:
    case ABS_HAT0Y: {
        int y = code == ABS_HAT0Y, n = value < 0 ? -1 : value > 0;
        pad_hat[y] = n;
        pad_key(y ? BTN_DPAD_UP : BTN_DPAD_LEFT, n < 0, key);
        pad_key(y ? BTN_DPAD_DOWN : BTN_DPAD_RIGHT, n > 0, key);
        return;
    }
    default: return;
    }
    pad_derive(key);
}

static void handle_ev(int d, struct input_event *ev, k15_touch_fn touch, k15_key_fn key)
{
    struct dev *v = &devs[d];
    if (K15_Debug() && ev->type != EV_SYN)
        fprintf(stderr, "ev dev%d type %d code %d val %d\n", d, ev->type, ev->code, ev->value);
    if (v->kind == DEV_TOUCH) {
        switch (ev->type) {
        case EV_ABS:
            if (ev->code == ABS_MT_SLOT)
                cur_slot = ev->value >= 0 && ev->value < K15_MAX_TOUCH ? ev->value
                                                                      : K15_MAX_TOUCH - 1;
            else if (ev->code == ABS_MT_TRACKING_ID)
                slots[cur_slot].active = ev->value >= 0;
            else if (ev->code == ABS_MT_POSITION_X || ev->code == ABS_X)
                slots[cur_slot].x = ev->value;
            else if (ev->code == ABS_MT_POSITION_Y || ev->code == ABS_Y)
                slots[cur_slot].y = ev->value;
            break;
        case EV_KEY:
            // single-touch fallback for drivers that only report BTN_TOUCH
            if (ev->code == BTN_TOUCH && ev->value == 0)
                for (int s = 0; s < K15_MAX_TOUCH; s++)
                    slots[s].active = 0;
            else if (ev->code == BTN_TOUCH)
                slots[cur_slot].active = 1;
            break;
        case EV_SYN:
            if (ev->code == SYN_REPORT)
                touch_sync(d, touch);
            break;
        }
        return;
    }
    if (v->kind == DEV_PAD) {
        if (ev->type == EV_ABS)
            pad_abs(v, ev->code, ev->value, key);
        else if (ev->type == EV_KEY && ev->value != 2)
            pad_key(ev->code, ev->value, key);
        return;
    }
    if (ev->type == EV_KEY && ev->value != 2 && key && (kbd_on || !v->kbd))
        key(ev->value, ev->code);
}

// A keyboard node that is part of a gamepad (the 8BitDo receiver exposes one next to its
// XInput interface) doesn't count: same USB VID:PID as a connected pad.
static int part_of_pad(const struct dev *v)
{
    if (!v->vid)
        return 0;
    if (xi.fd >= 0 && xi.vid == v->vid && xi.pid == v->pid)
        return 1;
    for (int d = 0; d < ndevs; d++)
        if (devs[d].kind == DEV_PAD && devs[d].vid == v->vid && devs[d].pid == v->pid)
            return 1;
    return 0;
}

int K15_HasKeyboard(void)
{
    for (int d = 0; d < ndevs; d++)
        if (devs[d].kbd && !part_of_pad(&devs[d]))
            return 1;
    return 0;
}

void K15_InputKeyboard(int on)
{
    kbd_on = on;
}

static void build_pfds(void)
{
    if (npfds >= 0)
        return;
    npfds = 0;
    for (int d = 0; d < ndevs; d++)
        pfds[npfds++] = (struct pollfd){.fd = devs[d].fd, .events = POLLIN};
    udp_slot = xi_slot = -1;
    if (udp_fd >= 0) {
        udp_slot = npfds;
        pfds[npfds++] = (struct pollfd){.fd = udp_fd, .events = POLLIN};
    }
    if (xi.fd >= 0) {
        xi_slot = npfds;
        pfds[npfds++] = (struct pollfd){.fd = xi.fd, .events = POLLOUT};
    }
}

int K15_InputWait(int timeout_ms)
{
    build_pfds();
    return poll(pfds, npfds, timeout_ms);
}

void K15_InputPump(k15_touch_fn touch, k15_key_fn key, k15_udp_fn udp)
{
    if (K15_NowMs() - last_scan >= RESCAN_MS)
        rescan();
    if (K15_InputWait(0) <= 0)
        return;
    xi_pump(key);
    if (udp_fd >= 0 && udp_slot >= 0 && (pfds[udp_slot].revents & POLLIN)) {
        unsigned char pkt[64];
        ssize_t r;
        while ((r = recv(udp_fd, pkt, sizeof pkt, 0)) > 0)
            for (ssize_t i = 0; i + 1 < r; i += 2)
                if (udp)
                    udp(pkt[i] ? 1 : 0, pkt[i + 1]);
    }
    // Last, because dropping a device reshuffles devs[] (pfds[] is rebuilt next time).
    for (int d = ndevs - 1; d >= 0; d--) {
        short re = pfds[d].revents;
        if (!re)
            continue;
        struct input_event ev[32];
        ssize_t r = 0;
        if (re & POLLIN)
            while ((r = read(devs[d].fd, ev, sizeof ev)) > 0)
                for (size_t i = 0; i < r / sizeof ev[0]; i++)
                    handle_ev(d, &ev[i], touch, key);
        if ((re & (POLLERR | POLLHUP | POLLNVAL)) || (r < 0 && errno == ENODEV))
            drop_dev(d, key);
    }
}

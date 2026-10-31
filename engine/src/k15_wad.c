// SPDX-License-Identifier: GPL-2.0-or-later
#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "k15_wad.h"

const char *K15_ConfigDirPath(void)
{
    static char dir[200];
    if (!dir[0]) {
        const char *h = getenv("K15_HOME");
        if (!h || !*h)
            h = "/usr/data/doom";
        snprintf(dir, sizeof dir, "%s%s", h, h[strlen(h) - 1] == '/' ? "" : "/");
    }
    return dir;
}

// Called by the patched d_main.c (M_SetConfigDir).
char *K15_ConfigDir(void)
{
    return strdup(K15_ConfigDirPath());
}

const char *K15_WadDir(void)
{
    static char dir[220];
    if (!dir[0])
        snprintf(dir, sizeof dir, "%swads/", K15_ConfigDirPath());
    return dir;
}

// Weak so the host harness can fake USB sticks.
__attribute__((weak)) int K15_UsbRoots(char roots[][64], int max)
{
    int n = 0;
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char dev[128], mnt[128];
    while (n < max && fscanf(f, "%127s %127s %*[^\n]", dev, mnt) == 2) {
        if (strncmp(mnt, "/tmp/udisk", 10) != 0 || strlen(mnt) >= 64)
            continue;
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= !strcmp(roots[i], mnt);
        if (!dup)
            snprintf(roots[n++], 64, "%s", mnt);
    }
    fclose(f);
    return n;
}

// IWADs we can name with confidence (desc NULL: DOOM or THE ULTIMATE DOOM, from the maps).
static const struct { const char *file, *desc; } known[] = {
    {"doom1.wad", "DOOM SHAREWARE"}, {"doom.wad", NULL}, {"doomu.wad", "THE ULTIMATE DOOM"},
    {"doom2.wad", "DOOM II"}, {"plutonia.wad", "FINAL DOOM: PLUTONIA"},
    {"tnt.wad", "FINAL DOOM: TNT"}, {"freedoom1.wad", "FREEDOOM PHASE 1"},
    {"freedoom2.wad", "FREEDOOM PHASE 2"}, {"freedm.wad", "FREEDM"},
};

static uint32_t le32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// Identify a WAD by its lump directory (like the engine does), name only for labels.
static int identify(k15_wad_t *w)
{
    FILE *f = fopen(w->path, "rb");
    if (!f)
        return 0;
    unsigned char h[12];
    int ok = fread(h, 1, 12, f) == 12 &&
             (!memcmp(h, "IWAD", 4) || !memcmp(h, "PWAD", 4));
    uint32_t num = ok ? le32(h + 4) : 0, ofs = ok ? le32(h + 8) : 0;
    if (!ok || num == 0 || num > 65536 || (long long)ofs + num * 16LL > w->size) {
        fclose(f);
        return 0;
    }
    w->iwad = h[0] == 'I';
    int map01 = 0, e1 = 0, e2 = 0, e4 = 0, freedoom = 0;
    unsigned char ent[16];
    fseek(f, ofs, SEEK_SET);
    for (uint32_t i = 0; i < num && fread(ent, 1, 16, f) == 16; i++) {
        char nm[9];
        memcpy(nm, ent + 8, 8);
        nm[8] = 0;
        if (!strncasecmp(nm, "MAP", 3) && nm[3] >= '0' && nm[3] <= '9')
            map01 = 1;
        else if (nm[0] == 'E' && nm[2] == 'M' && nm[4] == 0) {
            e1 |= nm[1] == '1';
            e2 |= nm[1] == '2' || nm[1] == '3';
            e4 |= nm[1] == '4';
        } else if (!strcmp(nm, "FREEDOOM"))
            freedoom = 1;
    }
    fclose(f);

    if (map01)
        w->kind = WAD_DOOM2;
    else if (e4)
        w->kind = WAD_DOOM_U;
    else if (e2)
        w->kind = WAD_DOOM;
    else if (e1)
        w->kind = w->iwad ? WAD_DOOM_SW : WAD_DOOM;
    else
        w->kind = WAD_UNKNOWN;

    const char *d = NULL;
    int is_known = 0;
    for (size_t i = 0; w->iwad && i < sizeof known / sizeof known[0]; i++)
        if (!strcasecmp(w->name, known[i].file)) {
            d = known[i].desc;
            is_known = 1;
        }
    if (!d && w->iwad) {
        static const char *iw[] = {"IWAD", "DOOM SHAREWARE", "DOOM", "THE ULTIMATE DOOM",
                                   "DOOM II"};
        d = freedoom ? (w->kind == WAD_DOOM2 ? "FREEDOOM PHASE 2" : "FREEDOOM PHASE 1")
                     : iw[w->kind];
    } else if (!d) {
        d = w->kind == WAD_DOOM2 ? "ADD-ON (DOOM II MAPS)"
            : w->kind == WAD_UNKNOWN ? "ADD-ON" : "ADD-ON (DOOM MAPS)";
    }
    snprintf(w->desc, sizeof w->desc, "%s", d);
    if (is_known) {
        snprintf(w->title, sizeof w->title, "%s", d);
    } else { // file name without .wad, upper case
        size_t l = strlen(w->name) - 4;
        if (l >= sizeof w->title)
            l = sizeof w->title - 1;
        for (size_t i = 0; i < l; i++)
            w->title[i] = toupper((unsigned char)w->name[i]);
        w->title[l] = 0;
    }
    return 1;
}

static int is_wad_name(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && n[0] != '.' && !strcasecmp(n + l - 4, ".wad");
}

static int scan_dir(const char *dir, int usb, k15_wad_t *out, int n, int max)
{
    DIR *d = opendir(dir);
    if (!d)
        return n;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (!is_wad_name(e->d_name) || strlen(e->d_name) >= sizeof out->name)
            continue;
        k15_wad_t *w = &out[n];
        memset(w, 0, sizeof *w);
        snprintf(w->path, sizeof w->path, "%s%s%s", dir,
                 dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
        snprintf(w->name, sizeof w->name, "%s", e->d_name);
        struct stat st;
        if (stat(w->path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        w->size = st.st_size;
        w->mtime = st.st_mtime;
        w->usb = usb;
        if (identify(w))
            n++;
    }
    closedir(d);
    return n;
}

static int cmp_wad(const void *a, const void *b)
{
    const k15_wad_t *x = a, *y = b;
    if (x->iwad != y->iwad)
        return y->iwad - x->iwad;
    int c = strcasecmp(x->name, y->name);
    return c ? c : x->usb - y->usb;
}

int K15_WadScan(k15_wad_t *out, int max)
{
    int n = scan_dir(K15_WadDir(), 0, out, 0, max);
    char roots[K15_MAX_USB][64];
    int nr = K15_UsbRoots(roots, K15_MAX_USB);
    for (int r = 0; r < nr; r++) {
        int from = n;
        n = scan_dir(roots[r], 1, out, n, max);
        // Drop stick WADs already installed (same file name) or seen on an earlier stick.
        for (int i = from; i < n;) {
            int dup = 0;
            for (int j = 0; j < from && !dup; j++)
                dup = !strcasecmp(out[i].name, out[j].name);
            if (dup)
                out[i] = out[--n];
            else
                i++;
        }
    }
    qsort(out, n, sizeof *out, cmp_wad);
    return n;
}

int K15_WadBaseFor(const k15_wad_t *list, int n, const k15_wad_t *pwad)
{
    // Preference: exact family, device copy first. Shareware can't load add-ons.
    static const char *d2[] = {"doom2.wad", "freedoom2.wad", "plutonia.wad", "tnt.wad"};
    static const char *d1[] = {"doom.wad", "doomu.wad", "freedoom1.wad"};
    int want2 = pwad->kind == WAD_DOOM2;
    const char **pref = want2 ? d2 : d1;
    int np = want2 ? 4 : 3;
    for (int p = 0; p < np; p++)
        for (int i = 0; i < n; i++)
            if (list[i].iwad && !strcasecmp(list[i].name, pref[p]))
                return i;
    for (int i = 0; i < n; i++) {
        if (!list[i].iwad || list[i].kind == WAD_DOOM_SW)
            continue;
        if (want2 ? list[i].kind == WAD_DOOM2 : list[i].kind != WAD_DOOM2)
            return i;
    }
    return -1;
}

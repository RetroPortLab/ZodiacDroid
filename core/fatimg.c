/* ZodiacDroid - builds and fills the SD card image: an MBR with one FAT16 partition.
 * Enough of FAT to create folders and copy files in (with long file names), so the
 * front-end can put apps and game data on the emulated card. */
#include "fatimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#define SEC 512u
#define PART_START 63u

typedef struct {
    int fd;
    uint32_t part, spc, reserved, nfats, fat_sec, root_ent, total;
    uint32_t fat0, root0, data0, nclusters;
    uint16_t *fat;            /* cached FAT */
} Vol;

static int rd(Vol *v, uint64_t sec, void *buf, uint32_t n) { return pread(v->fd, buf, n * SEC, (off_t)(sec * SEC)) == (ssize_t)(n * SEC); }
static int wr(Vol *v, uint64_t sec, const void *buf, uint32_t n) { return pwrite(v->fd, buf, n * SEC, (off_t)(sec * SEC)) == (ssize_t)(n * SEC); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }

int fat_format(int fd, uint64_t size) {
    uint32_t total = (uint32_t)(size / SEC);
    if (total < 16384) return 0;
    if (ftruncate(fd, (off_t)size) != 0) return 0;
    uint32_t psize = total - PART_START;
    uint32_t spc = 1;
    while (psize / spc > 65000) spc <<= 1;
    if (spc > 64) return 0;                     /* FAT16 tops out around 2GB */
    uint32_t root_ent = 512, reserved = 1, nfats = 2;
    uint32_t clusters = psize / spc;
    uint32_t fat_sec = ((clusters + 2) * 2 + SEC - 1) / SEC;

    uint8_t s[SEC];
    memset(s, 0, SEC);                          /* MBR */
    uint8_t *e = s + 446;
    e[0] = 0x00; e[1] = 1; e[2] = 1; e[3] = 0; e[4] = 0x06; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    put32(e + 8, PART_START); put32(e + 12, psize);
    s[510] = 0x55; s[511] = 0xAA;
    if (pwrite(fd, s, SEC, 0) != SEC) return 0;

    memset(s, 0, SEC);                          /* FAT16 boot sector */
    s[0] = 0xEB; s[1] = 0x3C; s[2] = 0x90;
    memcpy(s + 3, "ZODIACDR", 8);
    put16(s + 11, SEC); s[13] = (uint8_t)spc; put16(s + 14, reserved); s[16] = (uint8_t)nfats;
    put16(s + 17, root_ent); put16(s + 19, psize < 65536 ? psize : 0); s[21] = 0xF8;
    put16(s + 22, fat_sec); put16(s + 24, 63); put16(s + 26, 255); put32(s + 28, PART_START);
    put32(s + 32, psize < 65536 ? 0 : psize);
    s[36] = 0x80; s[38] = 0x29; put32(s + 39, 0x5A0D1AC0);
    memcpy(s + 43, "ZODIAC     ", 11); memcpy(s + 54, "FAT16   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
    if (pwrite(fd, s, SEC, (off_t)PART_START * SEC) != SEC) return 0;

    /* clear the FATs and the root directory */
    uint8_t *z = calloc(1, 64 * SEC);
    uint32_t clear = nfats * fat_sec + root_ent * 32 / SEC;
    for (uint32_t i = 0; i < clear; i += 64) {
        uint32_t n = clear - i < 64 ? clear - i : 64;
        if (pwrite(fd, z, n * SEC, (off_t)(PART_START + reserved + i) * SEC) != (ssize_t)(n * SEC)) { free(z); return 0; }
    }
    free(z);
    memset(s, 0, SEC);
    put16(s, 0xFFF8); put16(s + 2, 0xFFFF);
    for (uint32_t f = 0; f < nfats; f++)
        if (pwrite(fd, s, SEC, (off_t)(PART_START + reserved + f * fat_sec) * SEC) != SEC) return 0;
    memset(s, 0, SEC);                          /* volume label entry */
    memcpy(s, "ZODIAC     ", 11); s[11] = 0x08;
    if (pwrite(fd, s, SEC, (off_t)(PART_START + reserved + nfats * fat_sec) * SEC) != SEC) return 0;
    return 1;
}

static int vol_open(Vol *v, int fd) {
    uint8_t s[SEC];
    memset(v, 0, sizeof *v);
    v->fd = fd;
    if (pread(fd, s, SEC, 0) != SEC) return 0;
    v->part = (s[510] == 0x55 && s[511] == 0xAA && s[446 + 4]) ? get32(s + 446 + 8) : 0;
    if (!rd(v, v->part, s, 1)) return 0;
    if (get16(s + 11) != SEC) return 0;
    v->spc = s[13]; v->reserved = get16(s + 14); v->nfats = s[16]; v->root_ent = get16(s + 17);
    v->total = get16(s + 19) ? get16(s + 19) : get32(s + 32);
    v->fat_sec = get16(s + 22);
    if (!v->spc || !v->fat_sec) return 0;
    v->fat0 = v->part + v->reserved;
    v->root0 = v->fat0 + v->nfats * v->fat_sec;
    v->data0 = v->root0 + v->root_ent * 32 / SEC;
    v->nclusters = (v->total - (v->data0 - v->part)) / v->spc + 2;
    if (v->nclusters > v->fat_sec * SEC / 2) v->nclusters = v->fat_sec * SEC / 2;
    v->fat = malloc(v->fat_sec * SEC);
    if (!v->fat || !rd(v, v->fat0, v->fat, v->fat_sec)) { free(v->fat); return 0; }
    return 1;
}

static int vol_close(Vol *v, int dirty) {
    int ok = 1;
    if (dirty) for (uint32_t f = 0; f < v->nfats; f++) ok &= wr(v, v->fat0 + f * v->fat_sec, v->fat, v->fat_sec);
    free(v->fat);
    return ok;
}

static uint64_t clus_sec(Vol *v, uint32_t c) { return v->data0 + (uint64_t)(c - 2) * v->spc; }

static uint32_t alloc_cluster(Vol *v, uint32_t prev) {
    for (uint32_t c = 2; c < v->nclusters; c++) if (v->fat[c] == 0) {
        v->fat[c] = 0xFFFF;
        if (prev) v->fat[prev] = (uint16_t)c;
        uint8_t *z = calloc(v->spc, SEC);
        wr(v, clus_sec(v, c), z, v->spc);
        free(z);
        return c;
    }
    return 0;
}

/* A directory: cluster 0 = the root. Reads/writes 32-byte entries by index. */
static int dir_entry(Vol *v, uint32_t dir, uint32_t idx, uint8_t ent[32], int write, int grow) {
    uint64_t sec;
    uint32_t per_sec = SEC / 32, off = idx % per_sec;
    if (dir == 0) {
        if (idx >= v->root_ent) return 0;
        sec = v->root0 + idx / per_sec;
    } else {
        uint32_t per_clus = per_sec * v->spc, c = dir, n = idx / per_clus;
        while (n--) {
            uint32_t nx = v->fat[c];
            if (nx >= 0xFFF8) { if (!grow) return 0; nx = alloc_cluster(v, c); if (!nx) return 0; }
            c = nx;
        }
        sec = clus_sec(v, c) + (idx % per_clus) / per_sec;
    }
    uint8_t s[SEC];
    if (!rd(v, sec, s, 1)) return 0;
    if (write) { memcpy(s + off * 32, ent, 32); return wr(v, sec, s, 1); }
    memcpy(ent, s + off * 32, 32);
    return 1;
}

static uint8_t lfn_sum(const uint8_t *short11) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + short11[i]);
    return s;
}

/* Gather the long name (if any) preceding entry idx; compare case-insensitively with name. */
static int name_match(const char *name, const char *got) {
    return strcasecmp(name, got) == 0;
}

/* Find name in dir; returns entry index or -1. Fills ent. */
static int dir_find(Vol *v, uint32_t dir, const char *name, uint8_t ent[32]) {
    char lfn[256]; int have_lfn = 0;
    for (uint32_t i = 0; ; i++) {
        if (!dir_entry(v, dir, i, ent, 0, 0)) return -1;
        if (ent[0] == 0) return -1;
        if (ent[0] == 0xE5) { have_lfn = 0; continue; }
        if (ent[11] == 0x0F) {
            int ord = ent[0] & 0x3F;
            if (ent[0] & 0x40) { memset(lfn, 0, sizeof lfn); have_lfn = 1; }
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) {
                int ci = (ord - 1) * 13 + k;
                uint32_t ch = get16(ent + pos[k]);
                if (ci < 255 && ch && ch != 0xFFFF) lfn[ci] = ch < 128 ? (char)ch : '_';
            }
            continue;
        }
        if (ent[11] & 0x08) { have_lfn = 0; continue; }
        char sn[13]; int n = 0;
        for (int k = 0; k < 8 && ent[k] != ' '; k++) sn[n++] = (char)ent[k];
        if (ent[8] != ' ') { sn[n++] = '.'; for (int k = 8; k < 11 && ent[k] != ' '; k++) sn[n++] = (char)ent[k]; }
        sn[n] = 0;
        if ((have_lfn && name_match(name, lfn)) || name_match(name, sn)) return (int)i;
        have_lfn = 0;
    }
}

static int fits_83(const char *name) {
    const char *dot = strrchr(name, '.');
    size_t base = dot ? (size_t)(dot - name) : strlen(name), ext = dot ? strlen(dot + 1) : 0;
    if (base == 0 || base > 8 || ext > 3) return 0;
    for (const char *p = name; *p; p++) {
        if (p == dot) continue;
        if (*p == '.' || *p == ' ' || islower((unsigned char)*p) || !(isalnum((unsigned char)*p) || strchr("_-~!#$%&'()@^`{}", *p))) return 0;
    }
    return 1;
}

static void make_short(Vol *v, uint32_t dir, const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    if (fits_83(name)) {
        size_t b = dot ? (size_t)(dot - name) : strlen(name);
        memcpy(out, name, b);
        if (dot) memcpy(out + 8, dot + 1, strlen(dot + 1));
        return;
    }
    char base[9] = { 0 }; int n = 0;
    for (const char *p = name; *p && p != dot && n < 6; p++)
        if (isalnum((unsigned char)*p)) base[n++] = (char)toupper((unsigned char)*p);
    if (!n) base[n++] = 'F';
    if (dot) for (int k = 0; k < 3 && dot[1 + k]; k++)
        if (isalnum((unsigned char)dot[1 + k])) out[8 + k] = (uint8_t)toupper((unsigned char)dot[1 + k]);
    for (int t = 1; t < 999999; t++) {
        char tail[8]; int tl = snprintf(tail, sizeof tail, "~%d", t);
        int keep = n + tl > 8 ? 8 - tl : n;
        memset(out, ' ', 8);
        memcpy(out, base, (size_t)keep); memcpy(out + keep, tail, (size_t)tl);
        /* unique? */
        char sn[13]; int m = 0;
        for (int k = 0; k < 8 && out[k] != ' '; k++) sn[m++] = (char)out[k];
        if (out[8] != ' ') { sn[m++] = '.'; for (int k = 8; k < 11 && out[k] != ' '; k++) sn[m++] = (char)out[k]; }
        sn[m] = 0;
        uint8_t e[32];
        if (dir_find(v, dir, sn, e) < 0) return;
    }
}

/* Add an entry (with long name entries when needed); returns 1 on success. */
static int dir_add(Vol *v, uint32_t dir, const char *name, uint8_t attr, uint32_t first, uint32_t size) {
    uint8_t sn[11];
    make_short(v, dir, name, sn);
    int need_lfn = !fits_83(name);
    int nl = need_lfn ? (int)((strlen(name) + 12) / 13) : 0;
    int need = nl + 1;
    /* find a run of free slots */
    uint32_t start = 0; int run = 0;
    for (uint32_t i = 0; ; i++) {
        uint8_t e[32];
        if (!dir_entry(v, dir, i, e, 0, 1)) return 0;
        if (e[0] == 0 || e[0] == 0xE5) { if (!run) start = i; if (++run == need) break; }
        else run = 0;
    }
    uint8_t sum = lfn_sum(sn);
    size_t len = strlen(name);
    for (int k = 0; k < nl; k++) {
        int ord = nl - k;
        uint8_t e[32]; memset(e, 0, 32);
        e[0] = (uint8_t)(ord | (k == 0 ? 0x40 : 0));
        e[11] = 0x0F; e[13] = sum;
        static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int c = 0; c < 13; c++) {
            size_t ci = (size_t)(ord - 1) * 13 + (size_t)c;
            uint32_t ch = ci < len ? (uint8_t)name[ci] : ci == len ? 0 : 0xFFFF;
            put16(e + pos[c], ch);
        }
        if (!dir_entry(v, dir, start + (uint32_t)k, e, 1, 1)) return 0;
    }
    uint8_t e[32]; memset(e, 0, 32);
    memcpy(e, sn, 11); e[11] = attr;
    uint16_t date = (uint16_t)(((2004 - 1980) << 9) | (1 << 5) | 1), time = 12 << 11;
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date); put16(e + 22, time); put16(e + 24, date);
    put16(e + 26, first); put32(e + 28, size);
    return dir_entry(v, dir, start + (uint32_t)nl, e, 1, 1);
}

static void free_chain(Vol *v, uint32_t c) {
    while (c >= 2 && c < v->nclusters) { uint32_t n = v->fat[c]; v->fat[c] = 0; if (n >= 0xFFF8) break; c = n; }
}

/* Walk/create the folders of path (all but its last part); returns the folder's cluster or -1. */
static long walk(Vol *v, const char *path, const char **leaf, int create) {
    uint32_t dir = 0;
    char part[256];
    const char *p = path;
    while (*p == '/' || *p == '\\') p++;
    for (;;) {
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        if (!*e) { *leaf = p; return (long)dir; }
        size_t n = (size_t)(e - p) < sizeof part - 1 ? (size_t)(e - p) : sizeof part - 1;
        memcpy(part, p, n); part[n] = 0;
        uint8_t ent[32];
        if (dir_find(v, dir, part, ent) >= 0) {
            if (!(ent[11] & 0x10)) return -1;
            dir = get16(ent + 26);
        } else {
            if (!create) return -1;
            uint32_t c = alloc_cluster(v, 0);
            if (!c) return -1;
            uint8_t d[32]; memset(d, 0, 32);           /* "." and ".." */
            memcpy(d, ".          ", 11); d[11] = 0x10; put16(d + 26, c);
            dir_entry(v, c, 0, d, 1, 0);
            memcpy(d, "..         ", 11); put16(d + 26, dir);
            dir_entry(v, c, 1, d, 1, 0);
            if (!dir_add(v, dir, part, 0x10, c, 0)) return -1;
            dir = c;
        }
        p = e;
        while (*p == '/' || *p == '\\') p++;
    }
}

int fat_write_file(int fd, const char *path, const uint8_t *data, uint32_t len) {
    Vol v;
    if (!vol_open(&v, fd)) return 0;
    const char *leaf;
    long dir = walk(&v, path, &leaf, 1);
    int ok = 0;
    if (dir >= 0 && *leaf) {
        uint8_t ent[32];
        int idx = dir_find(&v, (uint32_t)dir, leaf, ent);
        if (idx >= 0) {                                   /* replace: free the old data, delete the entries */
            free_chain(&v, get16(ent + 26));
            ent[0] = 0xE5; dir_entry(&v, (uint32_t)dir, (uint32_t)idx, ent, 1, 0);
            for (int i = idx - 1; i >= 0; i--) {
                uint8_t e2[32];
                if (!dir_entry(&v, (uint32_t)dir, (uint32_t)i, e2, 0, 0) || e2[11] != 0x0F || e2[0] == 0xE5) break;
                e2[0] = 0xE5; dir_entry(&v, (uint32_t)dir, (uint32_t)i, e2, 1, 0);
            }
        }
        uint32_t first = 0, prev = 0, csize = v.spc * SEC, done = 0;
        ok = 1;
        uint8_t *buf = malloc(csize);
        while (done < len) {
            uint32_t c = alloc_cluster(&v, prev);
            if (!c) { ok = 0; break; }
            if (!first) first = c;
            uint32_t n = len - done < csize ? len - done : csize;
            memset(buf, 0, csize); memcpy(buf, data + done, n);
            if (!wr(&v, clus_sec(&v, c), buf, v.spc)) { ok = 0; break; }
            done += n; prev = c;
        }
        free(buf);
        if (ok) ok = dir_add(&v, (uint32_t)dir, leaf, 0x20, first, len);
    }
    return vol_close(&v, 1) && ok;
}

int fat_mkdir(int fd, const char *path) {
    Vol v;
    if (!vol_open(&v, fd)) return 0;
    char p[1024];
    snprintf(p, sizeof p, "%s/", path);
    const char *leaf;
    long dir = walk(&v, p, &leaf, 1);
    return vol_close(&v, 1) && dir >= 0;
}

/* Lists a folder ("" = root) into out as "name\tsize\tisdir\n" lines. Returns bytes written. */
int fat_list(int fd, const char *path, char *out, int max) {
    Vol v;
    if (!vol_open(&v, fd)) return 0;
    char p[1024];
    snprintf(p, sizeof p, "%s%s", path, *path ? "/" : "");
    const char *leaf;
    long dir = *path ? walk(&v, p, &leaf, 0) : 0;
    int n = 0;
    if (dir >= 0) {
        char lfn[256]; int have = 0;
        for (uint32_t i = 0; ; i++) {
            uint8_t e[32];
            if (!dir_entry(&v, (uint32_t)dir, i, e, 0, 0) || e[0] == 0) break;
            if (e[0] == 0xE5) { have = 0; continue; }
            if (e[11] == 0x0F) {
                int ord = e[0] & 0x3F;
                if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); have = 1; }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) { int ci = (ord - 1) * 13 + k; uint32_t ch = get16(e + pos[k]); if (ci < 255 && ch && ch != 0xFFFF) lfn[ci] = ch < 128 ? (char)ch : '_'; }
                continue;
            }
            if (e[11] & 0x08 || e[0] == '.') { have = 0; continue; }
            char sn[13]; int m = 0;
            for (int k = 0; k < 8 && e[k] != ' '; k++) sn[m++] = (char)e[k];
            if (e[8] != ' ') { sn[m++] = '.'; for (int k = 8; k < 11 && e[k] != ' '; k++) sn[m++] = (char)e[k]; }
            sn[m] = 0;
            n += snprintf(out + n, (size_t)(max - n), "%s\t%u\t%d\n", have ? lfn : sn, get32(e + 28), (e[11] & 0x10) != 0);
            if (n >= max - 1) break;
            have = 0;
        }
    }
    vol_close(&v, 0);
    return n;
}

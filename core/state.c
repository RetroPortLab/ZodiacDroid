/* ZodiacDroid - save states: the whole machine, including the Palm OS storage heap in SDRAM.
 * Layout: "ZODS" magic, version, uncompressed size, then a zlib stream of the module chunks. */
#include "zodiac.h"
#include "cpu.h"
#include "state.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define STATE_MAGIC   0x53444F5Au   /* "ZODS" */
#define STATE_VERSION 3

void st_bytes(StateIO *s, void *p, size_t n) {
    if (s->err) return;
    if (s->loading) {
        if (s->pos + n > s->len) { s->err = 1; memset(p, 0, n); return; }
        memcpy(p, s->buf + s->pos, n);
    } else {
        if (s->pos + n > s->cap) {
            size_t c = s->cap ? s->cap : (1 << 20);
            while (c < s->pos + n) c *= 2;
            uint8_t *b = realloc(s->buf, c);
            if (!b) { s->err = 1; return; }
            s->buf = b; s->cap = c;
        }
        memcpy(s->buf + s->pos, p, n);
    }
    s->pos += n;
}

/* SDRAM is mostly empty: store a bitmap of non-zero 4KB pages, then those pages */
static void sparse(StateIO *s, uint8_t *mem, size_t size) {
    size_t pages = size >> 12;
    uint8_t *map = calloc(1, pages / 8 + 1);
    if (!s->loading)
        for (size_t i = 0; i < pages; i++) {
            const uint64_t *w = (const uint64_t *)(mem + (i << 12));
            for (int k = 0; k < 512; k++) if (w[k]) { map[i >> 3] |= 1 << (i & 7); break; }
        }
    st_bytes(s, map, pages / 8 + 1);
    for (size_t i = 0; i < pages; i++) {
        if (map[i >> 3] & (1 << (i & 7))) st_bytes(s, mem + (i << 12), 4096);
        else if (s->loading) memset(mem + (i << 12), 0, 4096);
    }
    free(map);
}

static void machine(StateIO *s, uint32_t version) {
    ST(s, zod.model);
    ST(s, zod.cycles);
    st_bytes(s, zod.io, sizeof zod.io);
    st_bytes(s, zod.esram, ESRAM_SIZE);
    sparse(s, zod.sdram, SDRAM_SIZE);
    cpu_state(s);
    io_state(s);
    soc_state(s);
    ext_state(s);
    w4200_state(s);
    w2d_state(s);
    if (version >= 2) sd_state(s, version);
    else if (s->loading) zod.io[0x09004 >> 2] = 0;   /* v1 kept DMA DISR as a plain register */
}

size_t zod_save_state(uint8_t **out) {
    StateIO s = { 0 };
    uint32_t tag = 0x4D454D53;   /* raw chunk start marker */
    ST(&s, tag);
    machine(&s, STATE_VERSION);
    if (s.err) { free(s.buf); return 0; }
    uLongf clen = compressBound(s.pos);
    uint8_t *o = malloc(clen + 16);
    if (!o) { free(s.buf); return 0; }
    if (compress2(o + 16, &clen, s.buf, s.pos, 1) != Z_OK) { free(o); free(s.buf); return 0; }
    uint32_t hdr[4] = { STATE_MAGIC, STATE_VERSION, (uint32_t)s.pos, (uint32_t)time(NULL) };
    memcpy(o, hdr, 16);
    free(s.buf);
    *out = o;
    return clen + 16;
}

int zod_load_state(const uint8_t *data, size_t len) {
    uint32_t hdr[4];
    if (len < 16) return 0;
    memcpy(hdr, data, 16);
    if (hdr[0] != STATE_MAGIC || hdr[1] < 1 || hdr[1] > STATE_VERSION) return 0;
    uLongf rlen = hdr[2];
    uint8_t *raw = malloc(rlen ? rlen : 1);
    if (!raw) return 0;
    if (uncompress(raw, &rlen, data + 16, len - 16) != Z_OK || rlen != hdr[2]) { free(raw); return 0; }
    StateIO s = { .buf = raw, .len = rlen, .loading = 1 };
    uint32_t tag; ST(&s, tag);
    if (tag != 0x4D454D53) { free(raw); return 0; }
    machine(&s, hdr[1]);
    free(raw);
    if (s.err) return 0;
    cpu_tlb_flush();
    extern void rtc_advance(int64_t sec);
    rtc_advance((int64_t)time(NULL) - (int64_t)hdr[3]);   /* the clock keeps running while saved */
    return 1;
}

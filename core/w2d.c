/* ZodiacDroid - W4200 2D engine.
 * W100-family "E2" engine. Register names/layout follow the ATI Imageon W100 as described
 * by the Linux w100fb driver; behaviour beyond that is inferred from what the Zodiac DAL
 * and games program. Supports solid fills, memory-to-memory blits with ROP3, mono->colour
 * expansion from host data, colour-key transparency and clipping, at 8/16 bpp. */
#include "zodiac.h"
#include "w4200.h"
#include <stdio.h>
#include <string.h>

#define R(r) w42.reg[((r) >> 2) & 0x3FFF]

enum {
    DST_OFFSET = 0x1004, DST_PITCH = 0x1008, DST_WIDTH = 0x100C, DST_HEIGHT = 0x1010,
    DST_X = 0x101C, DST_Y = 0x1020, SRC_Y_X = 0x1034, DST_Y_X = 0x1038, DST_HEIGHT_WIDTH = 0x103C,
    DP_GUI_MASTER_CNTL = 0x106C, BRUSH_Y_X = 0x1074, DP_BRUSH_BKGD_CLR = 0x1078, DP_BRUSH_FRGD_CLR = 0x107C,
    BRUSH_OFFSET = 0x108C, DEFAULT_SC_BOTTOM_RIGHT = 0x10A8,
    SRC_X_Y = 0x1190, DST_X_Y = 0x1194, DST_WIDTH_HEIGHT = 0x1198,
    SRC_OFFSET = 0x11AC, SRC_PITCH = 0x11B0, SC_TOP_LEFT = 0x11BC, SC_BOTTOM_RIGHT = 0x11C0,
    SRC_SC_BOTTOM_RIGHT = 0x11C4, DP_CNTL = 0x11C8, DP_CNTL_DST_DIR = 0x11CC,
    CLR_CMP_CNTL = 0x1230, CLR_CMP_CLR_SRC = 0x1234, CLR_CMP_CLR_DST = 0x1238, CLR_CMP_MSK = 0x123C,
    DP_SRC_FRGD_CLR = 0x1240, DP_SRC_BKGD_CLR = 0x1244,
    DP_DATATYPE = 0x12C4, DP_MIX = 0x12C8, DP_WRITE_MSK = 0x12CC,
    HOST_DATA0 = 0x13C0, HOST_DATA_LAST = 0x13E0,
};

static int logs;
static uint8_t seen[0x400];

/* pending host-data blit (mono expansion or colour data) */
static struct {
    int active;
    int x, y, w, h;        /* destination rectangle */
    int cx, cy;            /* current position within it */
    int mono;
    int bpp;
} hb;

void w2d_init(void) { logs = 0; memset(seen, 0, sizeof seen); memset(&hb, 0, sizeof hb); }

uint32_t w2d_read(uint32_t r, uint32_t v) { return v; }

/* ---------------------------------------------------------------- helpers */

static uint32_t rop3(uint32_t rop, uint32_t d, uint32_t s, uint32_t p) {
    switch (rop) {
    case 0xCC: return s;
    case 0xF0: return p;
    case 0x00: return 0;
    case 0xFF: return 0xFFFFFFFF;
    case 0xAA: return d;
    case 0x55: return ~d;
    case 0x33: return ~s;
    case 0x0F: return ~p;
    case 0x66: return d ^ s;
    case 0x5A: return d ^ p;
    case 0x88: return d & s;
    case 0xEE: return d | s;
    case 0x44: return s & ~d;
    case 0x22: return d & ~s;
    case 0xBB: return d | ~s;
    case 0x11: return ~(d | s);
    case 0x99: return ~(d ^ s);
    case 0xA0: return d & p;
    case 0xFA: return d | p;
    case 0xCA: return (s & p) | (d & ~p);
    case 0xE2: return (s & d) | (p & ~d);  /* unlikely */
    case 0xB8: return (p & s) | (d & ~s);
    case 0xE4: return (s & d) | (p & ~d);
    case 0x5F: return ~(d & p);
    case 0x50: return p & ~d;
    case 0x0A: return d & ~p;
    case 0xAF: return d | ~p;
    case 0xF5: return p | ~d;
    case 0x30: return s & ~p;
    case 0xC0: return s & p;
    case 0xFC: return s | p;
    case 0x3C: return s ^ p;
    }
    /* generic: evaluate the truth table bit by bit */
    uint32_t r = 0;
    for (int b = 0; b < 32; b++) {
        int idx = (((p >> b) & 1) << 2) | (((s >> b) & 1) << 1) | ((d >> b) & 1);
        r |= ((rop >> idx) & 1u) << b;
    }
    return r;
}

static int dtype_bpp(uint32_t t) {
    switch (t & 15) {
    case 2: return 8;
    case 3: case 4: return 16;     /* 1555 / 565 */
    case 5: return 24;
    case 6: return 32;
    default: return 16;
    }
}

static inline uint32_t get_px(uint32_t base, uint32_t pitch_px, int bpp, int x, int y, int *ok) {
    uint32_t a = base + ((uint32_t)y * pitch_px + (uint32_t)x) * (bpp / 8);
    uint8_t *p = w42_ptr(a);
    if (!p) { *ok = 0; return 0; }
    *ok = 1;
    if (bpp == 8) return *p;
    if (bpp == 16) return *(uint16_t *)p;
    return *(uint32_t *)p;
}
static inline void put_px(uint32_t base, uint32_t pitch_px, int bpp, int x, int y, uint32_t v) {
    uint32_t a = base + ((uint32_t)y * pitch_px + (uint32_t)x) * (bpp / 8);
    uint8_t *p = w42_ptr(a);
    if (!p) return;
    if (bpp == 8) *p = (uint8_t)v;
    else if (bpp == 16) *(uint16_t *)p = (uint16_t)v;
    else *(uint32_t *)p = v;
}

/* gmc fields (w100fb layout) */
#define GMC            R(DP_GUI_MASTER_CNTL)
#define GMC_DST_CLIP   ((GMC >> 3) & 1)
#define GMC_BRUSH_TYPE ((GMC >> 4) & 15)
#define GMC_DST_TYPE   ((GMC >> 8) & 15)
#define GMC_SRC_TYPE   ((GMC >> 12) & 7)
#define GMC_ROP3       ((GMC >> 17) & 0xFF)
#define GMC_SRC_SOURCE ((GMC >> 25) & 7)
#define GMC_CMP_DIS    ((GMC >> 28) & 1)
#define GMC_WRMSK_DIS  ((GMC >> 30) & 1)

static int clip_ok(int x, int y) {
    if (!GMC_DST_CLIP) {
        uint32_t d = R(DEFAULT_SC_BOTTOM_RIGHT);
        return x <= (int)(d & 0x3FFF) && y <= (int)((d >> 16) & 0x3FFF);
    }
    uint32_t tl = R(SC_TOP_LEFT), br = R(SC_BOTTOM_RIGHT);
    int l = tl & 0x3FFF, t = (tl >> 16) & 0x3FFF, rr = br & 0x3FFF, b = (br >> 16) & 0x3FFF;
    return x >= l && x < rr && y >= t && y < b;
}

static uint32_t pattern_px(int x, int y) {
    uint32_t bt = GMC_BRUSH_TYPE;
    if (bt == 13 || bt == 14 || bt == 15) return R(DP_BRUSH_FRGD_CLR);   /* solid */
    if (bt == 0 || bt == 1) {          /* 8x8 mono pattern at BRUSH_OFFSET (2 dwords) */
        uint8_t *p = w42_ptr(R(BRUSH_OFFSET));
        if (p) {
            int bit = (p[y & 7] >> (x & 7)) & 1;
            if (bit) return R(DP_BRUSH_FRGD_CLR);
            return R(DP_BRUSH_BKGD_CLR);
        }
    }
    return R(DP_BRUSH_FRGD_CLR);
}

static int transparent(uint32_t s, uint32_t d) {
    if (GMC_CMP_DIS) return 0;
    uint32_t cntl = R(CLR_CMP_CNTL), msk = R(CLR_CMP_MSK);
    if (!msk) msk = 0xFFFFFFFF;
    uint32_t fs = cntl & 7, fd = (cntl >> 8) & 7, which = (cntl >> 24) & 3;
    int skip = 0;
    /* functions: 0 false, 1 true, 4 not-equal, 5 equal */
    if (which & 1 || which == 0) {
        int eq = ((s ^ R(CLR_CMP_CLR_SRC)) & msk) == 0;
        if ((fs == 5 && eq) || (fs == 4 && !eq) || fs == 1) skip = 1;
    }
    if (which & 2) {
        int eq = ((d ^ R(CLR_CMP_CLR_DST)) & msk) == 0;
        if ((fd == 5 && eq) || (fd == 4 && !eq) || fd == 1) skip = 1;
    }
    return skip;
}

/* ---------------------------------------------------------------- operations */

static void do_blit(int dx, int dy, int w, int h, int sx, int sy) {
    int dbpp = dtype_bpp(GMC_DST_TYPE);
    uint32_t dbase = R(DST_OFFSET), dpitch = R(DST_PITCH) & 0x3FFF;
    uint32_t sbase = R(SRC_OFFSET), spitch = R(SRC_PITCH) & 0x3FFF;
    uint32_t rop = GMC_ROP3;
    int use_src = ((rop >> 2) ^ rop) & 0x33;          /* rop depends on S */
    int use_dst = ((rop >> 1) ^ rop) & 0x55;          /* rop depends on D */
    uint32_t wmask = GMC_WRMSK_DIS ? 0xFFFFFFFF : R(DP_WRITE_MSK);
    if (dbpp == 16) wmask &= 0xFFFF; else if (dbpp == 8) wmask &= 0xFF;
    int srcmono = GMC_SRC_TYPE == 0 || GMC_SRC_TYPE == 1;
    uint32_t cntl = R(DP_CNTL);
    int xdir = cntl & 1, ydir = (cntl >> 1) & 1;   /* 1 = left-to-right / top-to-bottom */
    if (!dpitch || w <= 0 || h <= 0) return;
    if (w > 4096 || h > 4096) return;
    for (int jj = 0; jj < h; jj++) {
        int j = ydir ? jj : h - 1 - jj;
        for (int ii = 0; ii < w; ii++) {
            int i = xdir ? ii : w - 1 - ii;
            int x = dx + i, y = dy + j;
            if (!clip_ok(x, y)) continue;
            int ok = 1;
            uint32_t s = 0, d = 0;
            if (use_src) {
                if (srcmono) {
                    /* mono source in memory: 1bpp, MSB-first per byte */
                    uint32_t bitx = (uint32_t)(sx + i), a = sbase + (uint32_t)(sy + j) * ((spitch + 7) / 8);
                    uint8_t *p = w42_ptr(a + bitx / 8);
                    int bit = p ? (*p >> (7 - (bitx & 7))) & 1 : 0;
                    if (!bit && GMC_SRC_TYPE == 1) continue;     /* leave background */
                    s = bit ? R(DP_SRC_FRGD_CLR) : R(DP_SRC_BKGD_CLR);
                } else {
                    s = get_px(sbase, spitch, dbpp, sx + i, sy + j, &ok);
                    if (!ok) continue;
                }
            }
            if (use_dst || !GMC_CMP_DIS || wmask != (dbpp == 16 ? 0xFFFFu : dbpp == 8 ? 0xFFu : 0xFFFFFFFFu))
                d = get_px(dbase, dpitch, dbpp, x, y, &ok);
            if (transparent(s, d)) continue;
            uint32_t v = rop3(rop, d, s, pattern_px(x, y));
            v = (v & wmask) | (d & ~wmask);
            put_px(dbase, dpitch, dbpp, x, y, v);
        }
    }
}

static void start_hostdata(int x, int y, int w, int h) {
    hb.active = 1; hb.x = x; hb.y = y; hb.w = w; hb.h = h; hb.cx = 0; hb.cy = 0;
    hb.mono = GMC_SRC_TYPE == 0 || GMC_SRC_TYPE == 1;
    hb.bpp = dtype_bpp(GMC_DST_TYPE);
}

static void host_pixel(uint32_t s, int is_bg_transparent_skip) {
    int dbpp = hb.bpp;
    uint32_t dbase = R(DST_OFFSET), dpitch = R(DST_PITCH) & 0x3FFF;
    int x = hb.x + hb.cx, y = hb.y + hb.cy;
    if (!is_bg_transparent_skip && clip_ok(x, y)) {
        int ok; uint32_t d = get_px(dbase, dpitch, dbpp, x, y, &ok);
        if (ok && !transparent(s, d)) {
            uint32_t v = rop3(GMC_ROP3, d, s, pattern_px(x, y));
            put_px(dbase, dpitch, dbpp, x, y, v);
        }
    }
    if (++hb.cx >= hb.w) { hb.cx = 0; if (++hb.cy >= hb.h) hb.active = 0; }
}

static void host_data(uint32_t v) {
    if (!hb.active) return;
    if (hb.mono) {
        /* 32 pixels per dword, LSB first (bit order from GMC byte_pix_order) */
        int msb = (GMC >> 15) & 1;
        for (int b = 0; b < 32 && hb.active; b++) {
            int bit = msb ? (v >> (31 - b)) & 1 : (v >> b) & 1;
            int skip = !bit && GMC_SRC_TYPE == 1;
            host_pixel(bit ? R(DP_SRC_FRGD_CLR) : R(DP_SRC_BKGD_CLR), skip);
            if (hb.cx == 0) break;   /* each row starts on a new dword */
        }
    } else if (hb.bpp == 16) {
        host_pixel(v & 0xFFFF, 0);
        if (hb.active) host_pixel(v >> 16, 0);
    } else if (hb.bpp == 8) {
        for (int b = 0; b < 4 && hb.active; b++) host_pixel((v >> (b * 8)) & 0xFF, 0);
    } else host_pixel(v, 0);
}

static void trigger(int x, int y, int w, int h) {
    if (zod.log_2d) fprintf(stderr, "[2D] op gmc=%08x rop=%02x src=%d dst=%08x/%u (%d,%d %dx%d) src=%08x/%u (%d,%d) fg=%08x\n",
                            GMC, GMC_ROP3, GMC_SRC_SOURCE, R(DST_OFFSET), R(DST_PITCH) & 0x3FFF, x, y, w, h,
                            R(SRC_OFFSET), R(SRC_PITCH) & 0x3FFF, (int)(R(SRC_Y_X) & 0xFFFF), (int)(R(SRC_Y_X) >> 16), R(DP_BRUSH_FRGD_CLR));
    uint32_t src = GMC_SRC_SOURCE;
    uint32_t rop = GMC_ROP3;
    int use_src = ((rop >> 2) ^ rop) & 0x33;
    if (use_src && src == 3) { start_hostdata(x, y, w, h); return; }
    uint32_t sy = R(SRC_Y_X) >> 16, sx = R(SRC_Y_X) & 0xFFFF;
    do_blit(x, y, w, h, (int)sx, (int)sy);
}

void w2d_write(uint32_t r, uint32_t v) {
    if (zod.log_2d > 1 && logs < 20000) { logs++; fprintf(stderr, "[2D] %04x <- %08x\n", r, v); }
    switch (r) {
    case DST_HEIGHT_WIDTH: {
        uint32_t yx = R(DST_Y_X);
        trigger((int)(yx & 0xFFFF), (int)(yx >> 16), (int)(v & 0xFFFF), (int)(v >> 16));
        break;
    }
    case DST_WIDTH_HEIGHT: {
        uint32_t yx = R(DST_Y_X);
        trigger((int)(yx & 0xFFFF), (int)(yx >> 16), (int)(v >> 16), (int)(v & 0xFFFF));
        break;
    }
    default:
        if (r >= HOST_DATA0 && r <= HOST_DATA_LAST) host_data(v);
        else if (!seen[(r - 0x1000) >> 2]) { seen[(r - 0x1000) >> 2] = 1; if (zod.log_2d) fprintf(stderr, "[2D] first write %04x <- %08x\n", r, v); }
        break;
    }
}

/* Type-3 command processor packets. Opcodes are identified from the DAL's command streams. */
int w2d_packet3(uint32_t op, const uint32_t *d, uint32_t n) {
    switch (op) {
    case 0x1A:   /* PAINT: DST_Y_X, DST_WIDTH_HEIGHT */
        if (n < 2) return 0;
        R(DST_Y_X) = d[0];
        trigger((int)(d[0] & 0xFFFF), (int)(d[0] >> 16), (int)(d[1] >> 16), (int)(d[1] & 0xFFFF));
        return 1;
    }
    if (zod.log_2d) {
        fprintf(stderr, "[2D] packet3 %02x:", op);
        for (uint32_t i = 0; i < n && i < 16; i++) fprintf(stderr, " %08x", d[i]);
        fprintf(stderr, "\n");
    }
    return 0;
}

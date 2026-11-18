/* ZodiacDroid - ATI Imageon W4200 graphics controller (CS5, physical 0x16000000).
 *
 * The W4200 is a member of ATI's Imageon W100 family. Its register layout matches the
 * W100's (as documented by the Linux w100fb driver), with a Radeon-style command
 * processor (CP) ring and 2D engine. Layout seen from the ARM (offsets from CS5):
 *   +0x000000  host interface: 0x10 = interface version (1), 0x18 = control (bit3 reset), 0x1C = bus config
 *   +0x010000  registers; 0x0000 = chip id (vendor 0x1002 = ATI)
 *   +0x030000  internal SRAM (384KB, chip address 0x100000-0x15FFFF)
 *   +0x800000  external SDRAM (8MB, chip address 0x800000-0xFFFFFF)
 */
#include "zodiac.h"
#include "state.h"
#include "cpu.h"
#include "io.h"
#include "w4200.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>

W4200 w42;

#define REG(r) w42.reg[((r) >> 2) & 0x3FFF]

void w4200_init(void) {
    memset(w42.hif, 0, sizeof w42.hif);
    memset(w42.reg, 0, sizeof w42.reg);
    if (!w42.imem) w42.imem = calloc(1, W42_IMEM_SIZE);
    if (!w42.emem) w42.emem = calloc(1, W42_EMEM_SIZE);
    memset(w42.imem, 0, W42_IMEM_SIZE);
    memset(w42.emem, 0, W42_EMEM_SIZE);
    w42.hif[0x10 >> 2] = 1;
    /* device 0x5744: the DAL puts 0x5644/0x5744 (W32xx family) on its newer-chip path; vendor 0x1002 = ATI */
    REG(0x0000) = 0x57441002;
    w2d_init();
}

/* host pointer for a chip address, or NULL */
uint8_t *w42_ptr(uint32_t a) {
    a &= 0xFFFFFF;
    if (a - W42_IMEM_BASE < W42_IMEM_SIZE) return w42.imem + (a - W42_IMEM_BASE);
    if (a - W42_EMEM_BASE < W42_EMEM_SIZE) return w42.emem + (a - W42_EMEM_BASE);
    return NULL;
}

/* host (CS5 offset) -> chip address; 0xFFFFFFFF if not memory.
 * CPU writes inside the WRAP window (WRAP_START_DIR..WRAP_TOP_DIR, in 16-bit units) wrap
 * around inside the buffer described by WRAP_BUF_A; the DAL streams CP packets through it. */
static uint32_t host_to_chip(uint32_t off) {
    off &= 0xFFFFFF;
    uint32_t wlo = (REG(0x14) & 0xFFFFF) * 2, whi = (REG(0x10) & 0xFFFFF) * 2 + 2;
    if (wlo && off >= wlo && off < whi) {
        uint32_t wa = REG(0x08), size = 0x1000u << ((wa >> 24) & 15);
        return (wa & 0xFFFFFF) + ((off - wlo) & (size - 1));
    }
    if (off - 0x30000 < W42_IMEM_SIZE) return off - 0x30000 + W42_IMEM_BASE;   /* window used for the CP ring */
    if (off - W42_IMEM_BASE < W42_IMEM_SIZE) return off;                        /* 1:1 (as on the W100) */
    if (off >= W42_EMEM_BASE) return off;
    return 0xFFFFFFFF;
}

uint8_t *w4200_page(uint32_t pa) {   /* pa = physical ARM address in CS5 */
    uint32_t a = host_to_chip(pa & 0xFFF000);
    return a == 0xFFFFFFFF ? NULL : w42_ptr(a);
}

/* ---------------------------------------------------------------- command processor */

static int cp_logs;

static void cp_run(void) {
    uint32_t cntl = REG(0x210);
    uint32_t size = 8u << (cntl & 0x3F);          /* bytes */
    uint32_t base = REG(0x214) & 0x00FFFFFC;
    uint32_t mask = size / 4 - 1;
    uint8_t *ring = w42_ptr(base);
    if (!ring || size > 0x100000) return;
    uint32_t rp = REG(0x21C) & mask, wp = REG(0x220) & mask;
    if (zod.log_2d > 1) {
        fprintf(stderr, "[CP] run rp=%x wp=%x base=%x size=%x:", rp, wp, base, size);
        for (uint32_t i = rp; i != wp && i != ((rp + 64) & mask); i = (i + 1) & mask) fprintf(stderr, " %08x", ((uint32_t *)ring)[i]);
        fprintf(stderr, "\n");
    }
    int guard = 0;
    while (rp != wp && guard++ < 1000000) {
        uint32_t h = ((uint32_t *)ring)[rp];
        uint32_t type = h >> 30, cnt = ((h >> 16) & 0x3FFF) + 1;
        uint32_t need = type == 2 ? 1 : type == 1 ? 3 : cnt + 1;
        uint32_t avail = (wp - rp) & mask;
        if (need > avail) break;            /* packet still being written: wait for the rest */
        if (getenv("CPWRAP") && rp + need > mask + 1) fprintf(stderr, "[CP] packet %08x crosses ring end at rp=%x need=%x wp=%x  start=%08x %08x  beyond=%08x %08x\n", h, rp, need, wp, ((uint32_t *)ring)[0], ((uint32_t *)ring)[1], ((uint32_t *)ring)[mask + 1], ((uint32_t *)ring)[mask + 2]);
        rp = (rp + 1) & mask;
        if (type == 0) {
            uint32_t reg = (h & 0x1FFF) << 2;
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t v = ((uint32_t *)ring)[rp]; rp = (rp + 1) & mask;
                w4200_reg_write(reg, v);
                if (!(h & 0x8000)) reg += 4;
            }
        } else if (type == 1) {
            uint32_t r0 = (h & 0x7FF) << 2, r1 = ((h >> 11) & 0x7FF) << 2;
            w4200_reg_write(r0, ((uint32_t *)ring)[rp]); rp = (rp + 1) & mask;
            w4200_reg_write(r1, ((uint32_t *)ring)[rp]); rp = (rp + 1) & mask;
        } else if (type == 2) {
            /* filler */
        } else {
            uint32_t op = (h >> 8) & 0xFF;
            static uint32_t buf[0x4000];
            for (uint32_t i = 0; i < cnt; i++) { buf[i] = ((uint32_t *)ring)[rp]; rp = (rp + 1) & mask; }
            if (!w2d_packet3(op, buf, cnt) && cp_logs++ < 50)
                fprintf(stderr, "[W4200] CP type3 op %02x count %u\n", op, cnt);
        }
    }
    REG(0x21C) = rp;
}

/* ---------------------------------------------------------------- registers */

/* Analog controller: the W4200's two-channel ADC samples the stick. A conversion is started
 * by setting bit 10 of 0xD0C; 0xD10 bits 20..24 give the FIFO depth and 0xD14 pops samples
 * tagged with the channel in bits 24..29, 10-bit values, centre 512: channel 1 is the
 * horizontal axis (inverted), channel 2 the vertical. */
static int stick_x, stick_y;          /* -32767..32767, right/down positive */
static int eff_x, eff_y, eff_hold;    /* what the ADC reports: a deflection is kept for a minimum time */
static unsigned adc_seq;

void zod_stick(int x, int y) {
    if (x < -32767) x = -32767;
    if (x > 32767) x = 32767;
    if (y < -32767) y = -32767;
    if (y > 32767) y = 32767;
    stick_x = x; stick_y = y;
    if (x > 9000 || x < -9000 || y > 9000 || y < -9000) { eff_x = x; eff_y = y; eff_hold = 24; }
}

/* The OS notices a deflected stick promptly only when GPIO0 pin 11 (the key-wake line) is
 * pulsed; otherwise its idle poll takes about two seconds to see it and menus feel dead.
 * Pulse the line every few frames while the stick is off centre. */
void w4200_stick_tick(void) {
    static int low, hold;
    if (eff_hold > 0 && --eff_hold == 0) { eff_x = stick_x; eff_y = stick_y; }
    else if (eff_hold == 0) { eff_x = stick_x; eff_y = stick_y; }
    int off = eff_x > 9000 || eff_x < -9000 || eff_y > 9000 || eff_y < -9000;
    if (low > 0) { if (--low == 0) gpio_set_input(0, 1u << 11, 1); return; }
    if (!off) { hold = 0; return; }
    if (hold++ % 6 == 0) { gpio_set_input(0, 1u << 11, 0); low = 3; }
}

static uint32_t adc_pop(void) {
    int ch1 = 512 - eff_x / 64, ch2 = 512 + eff_y / 64;
    if (ch1 > 1023) ch1 = 1023;
    if (ch2 > 1023) ch2 = 1023;
    if (ch1 < 0) ch1 = 0;
    if (ch2 < 0) ch2 = 0;
    return (adc_seq++ & 1) ? (2u << 24) | (uint32_t)ch2 : (1u << 24) | (uint32_t)ch1;
}

static uint32_t reg_read(uint32_t r) {
    uint32_t v = REG(r);
    switch (r) {
    case 0x0D10: return (v & ~(0x1Fu << 20)) | (2u << 20);
    case 0x0D14: return adc_pop();
    case 0x0140: return 0;                         /* RBBM_STATUS: engine idle, FIFO empty */
    case 0x01B8: return (v & ~0x10u) | 1;          /* clock status: PLL locked, not busy */
    case 0x04D8: return v & ~0x3u;                 /* DISP_DB_BUF_CNTL: update latches at vsync */
    case 0x054C: {                                 /* display status: bit10 = vertical blank */
        uint64_t frame = zod.cpu_hz / 60, ph = cpu_now() % frame;
        return ph >= frame - frame / 12 ? (v | 0x400) : (v & ~0x400u);
    }
    }
    if (r >= 0x1000 && r < 0x2000) return w2d_read(r, v);
    return v;
}

static void w42_int_update(void);

void w4200_reg_write(uint32_t r, uint32_t v) {
    r &= 0xFFFC;
    if (r < 8) return;                             /* chip id / revision are read-only */
    uint32_t old204 = REG(0x204);
    REG(r) = v;
    switch (r) {
    case 0x0214: REG(0x21C) = 0; break;            /* CP_RB_BASE: (re)initialising the ring resets the read pointer */
    case 0x0220: cp_run(); break;                  /* CP_RB_WPTR */
    case 0x0D0C: if (v & 0x400) adc_seq = 0; break; /* ADC conversion start */
    case 0x0200: w42_int_update(); break;
    case 0x0204: REG(0x204) = old204 & ~v; w42_int_update(); break;   /* write 1 to clear */
    }
    if (r >= 0x1000 && r < 0x2000) w2d_write(r, v);
}

/* ---------------------------------------------------------------- peripheral blocks at host 0x200000 */

#define AUX(o) w42.aux[(((o) - 0x200000) >> 2) & 0x3FFF]

static uint32_t aux_read(uint32_t off) {
    off |= 0x200000;
    return AUX(off);
}

static void aux_write(uint32_t off, uint32_t v) {
    off |= 0x200000;
    AUX(off) = v;
}

/* ---------------------------------------------------------------- host access */

uint32_t w4200_read(uint32_t off, int size) {
    uint32_t v;
    if (off < 0x10000) v = w42.hif[(off >> 2) & 0x3FFF];
    else if (off < 0x20000) v = reg_read(off & 0xFFFC);
    else if (off - 0x200000 < 0x10000) v = aux_read(off & 0xFFFC);
    else {
        uint32_t a = host_to_chip(off);
        uint8_t *p = a == 0xFFFFFFFF ? NULL : w42_ptr(a);
        if (!p) { io_log("R", 0x16000000 + off, 0, size); return 0; }
        return size == 4 ? *(uint32_t *)p : size == 2 ? *(uint16_t *)p : *p;
    }
    if (size == 2) v = (v >> ((off & 2) * 8)) & 0xFFFF;
    else if (size == 1) v = (v >> ((off & 3) * 8)) & 0xFF;
    io_log("R", 0x16000000 + off, v, size);
    return v;
}

void w4200_write(uint32_t off, uint32_t v, int size) {
    io_log("W", 0x16000000 + off, v, size);
    if (off - 0x200000 < 0x10000) {
        uint32_t *r = &w42.aux[((off - 0x200000) >> 2) & 0x3FFF], nv = *r;
        if (size == 4) nv = v;
        else if (size == 2) { int s = (off & 2) * 8; nv = (nv & ~(0xFFFFu << s)) | ((v & 0xFFFF) << s); }
        else { int s = (off & 3) * 8; nv = (nv & ~(0xFFu << s)) | ((v & 0xFF) << s); }
        aux_write(off & 0xFFFFFC, nv);
        return;
    }
    if (off >= 0x20000) {
        uint32_t a = host_to_chip(off);
        uint8_t *p = a == 0xFFFFFFFF ? NULL : w42_ptr(a);
        if (!p) { static int n; if (n++ < 2000000 && getenv("W42UNMAP")) fprintf(stderr, "[W4200] write to unmapped host offset %06x = %08x pc=%08x\n", off, v, cpu.r[15]); return; }
        if (size == 4) *(uint32_t *)p = v; else if (size == 2) *(uint16_t *)p = (uint16_t)v; else *p = (uint8_t)v;
        return;
    }
    uint32_t *r = off < 0x10000 ? &w42.hif[(off >> 2) & 0x3FFF] : &REG(off & 0xFFFC);
    uint32_t nv = *r;
    if (size == 4) nv = v;
    else if (size == 2) { int s = (off & 2) * 8; nv = (nv & ~(0xFFFFu << s)) | ((v & 0xFFFF) << s); }
    else { int s = (off & 3) * 8; nv = (nv & ~(0xFFu << s)) | ((v & 0xFF) << s); }
    if (off < 0x10000) *r = nv;
    else w4200_reg_write(off & 0xFFFC, nv);
}

/* ---------------------------------------------------------------- display */

/* Render the LCD into zod.fb as the user sees it (landscape 480x320).
 * The panel is 320x480 portrait. GRAPHIC_CTRL.portrait_mode selects how the controller walks
 * memory: mode 0 scans a 320-wide portrait surface directly; modes 1/2 scan a 480-wide
 * landscape surface rotated (GRAPHIC_OFFSET then points at the bottom-left / top-right). */
/* GEN_INT_CNTL (0x200) / GEN_INT_STATUS (0x204): the vertical-blank interrupt, wired to a
 * GPIO input of the i.MX1 (active low). */
static int w42_int_port = -1, w42_int_pin;
static void w42_int_update(void) {
    if (w42_int_port < 0) {
        const char *e = getenv("W42INT");
        w42_int_port = 0; w42_int_pin = 13;
        if (e) sscanf(e, "%d.%d", &w42_int_port, &w42_int_pin);
    }
    /* GEN_INT_CNTL bit0 enables the display interrupts reported in status bits 4/6;
     * bit 25 is enabled and reported in place */
    uint32_t st = REG(0x204), cn = REG(0x200);
    int active = ((cn & 1) && (st & 0x50)) || (st & cn & 0x02000000);
    gpio_set_input(w42_int_port, 1u << w42_int_pin, active ? 0 : 1);
}

void w4200_vblank(void) {
    REG(0x204) |= 0x10 | (REG(0x200) & 0x02000000);
    w42_int_update();
}

void w4200_scanout(void) {
    uint32_t ctrl = REG(0x414), off = REG(0x418) & 0xFFFFFF, pitch = REG(0x41C) & 0x3FFF;
    int mode = (ctrl >> 3) & 3;
    if (!pitch || !(ctrl & 0x800)) { memset(zod.fb, 0, sizeof zod.fb); zod.frame_ready = 1; return; }
    if (mode == 0) {
        /* portrait surface: panel row y (0..479) runs right-to-left across the landscape screen */
        for (int y = 0; y < 480; y++) {
            const uint16_t *src = (const uint16_t *)w42_ptr(off + (uint32_t)y * pitch);
            if (!src) continue;
            for (int x = 0; x < 320; x++) zod.fb[x * SCREEN_W + (479 - y)] = src[x];
        }
    } else {
        uint32_t base = mode == 2 ? off - (pitch - 4) : off - 319 * pitch;
        for (int y = 0; y < 320; y++) {
            const uint16_t *src = (const uint16_t *)w42_ptr(base + (uint32_t)y * pitch);
            if (!src) { memset(&zod.fb[y * SCREEN_W], 0, SCREEN_W * 2); continue; }
            memcpy(&zod.fb[y * SCREEN_W], src, SCREEN_W * 2);
        }
    }
    zod.frame_ready = 1;
}

void w4200_state(StateIO *s) {
    ST(s, w42.hif); ST(s, w42.reg); ST(s, w42.aux);
    st_bytes(s, w42.imem, W42_IMEM_SIZE);
    st_bytes(s, w42.emem, W42_EMEM_SIZE);
    ST(s, adc_seq);
}

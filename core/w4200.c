/* ZodiacDroid - ATI Imageon W4200 graphics controller (CS5, physical 0x16000000).
 * Reverse-engineered from how the Zodiac DAL drives it:
 *   +0x000000  host interface: 0x10 = interface version (1), 0x18 = control (bit3 reset), 0x1C = bus config
 *   +0x010000  chip registers; word 0 = PCI-style id (vendor 0x1002 = ATI) */
#include "zodiac.h"
#include "cpu.h"
#include "io.h"
#include "w4200.h"
#include <stdio.h>
#include <string.h>

W4200 w42;

void w4200_init(void) {
    memset(w42.hif, 0, sizeof w42.hif);
    memset(w42.reg, 0, sizeof w42.reg);
    w42.hif[0x10 >> 2] = 1;
    w42.reg[0] = 0x57441002;     /* device 0x5744: the DAL puts 0x5644/0x5744 (W32xx family) on its newer-chip path; vendor 0x1002 = ATI */
}

static uint32_t *w_reg(uint32_t off) {
    if (off < 0x10000) return &w42.hif[(off >> 2) & 0x3FFF];
    if (off < 0x20000) return &w42.reg[(off >> 2) & 0x3FFF];
    return NULL;
}

/* registers whose reads aren't simply the last value written */
static uint32_t w4200_reg_read(uint32_t r, uint32_t v) {
    switch (r) {
    case 0x01B8: return (v & ~0x10u) | 1;     /* clock status: PLL locked, not busy */
    case 0x04D8: return v & ~0x3u;            /* display update control: latch bits clear at vsync */
    case 0x054C: {                             /* display status: bit10 = vertical blank */
        uint64_t frame = zod.cpu_hz / 60, ph = cpu_now() % frame;
        return ph >= frame - frame / 12 ? (v | 0x400) : (v & ~0x400u);
    }
    }
    return v;
}

uint32_t w4200_read(uint32_t off, int size) {
    uint32_t *r = w_reg(off), v;
    if (!r) { io_log("R", 0x16000000 + off, 0, size); return 0; }
    v = *r;
    if (off >= 0x10000 && off < 0x20000) v = w4200_reg_read(off & 0xFFFC, v);
    if (size == 2) v = (v >> ((off & 2) * 8)) & 0xFFFF;
    else if (size == 1) v = (v >> ((off & 3) * 8)) & 0xFF;
    io_log("R", 0x16000000 + off, v, size);
    return v;
}

void w4200_write(uint32_t off, uint32_t v, int size) {
    uint32_t *r = w_reg(off);
    io_log("W", 0x16000000 + off, v, size);
    if (!r) return;
    if (off >= 0x10000 && (off & 0xFFFF) < 8) return;   /* id is read-only */
    if (size == 4) *r = v;
    else if (size == 2) { int s = (off & 2) * 8; *r = (*r & ~(0xFFFFu << s)) | ((v & 0xFFFF) << s); }
    else { int s = (off & 3) * 8; *r = (*r & ~(0xFFu << s)) | ((v & 0xFF) << s); }
}

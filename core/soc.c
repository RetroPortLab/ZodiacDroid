/* ZodiacDroid - other i.MX1 on-chip blocks: I2C (with the board's I2C devices), and stubs */
#include "zodiac.h"
#include "state.h"
#include "cpu.h"
#include "io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define IOREG(off) zod.io[(off) >> 2]

/* ---------------------------------------------------------------- I2C devices */

typedef struct {
    int addr;                    /* 7-bit */
    uint8_t mem[256];
    int ptr, got_ptr;
} I2cDev;

static I2cDev i2c_devs[8];
static int n_i2c_devs;

static I2cDev *i2c_find(int addr) {
    for (int i = 0; i < n_i2c_devs; i++) if (i2c_devs[i].addr == addr) return &i2c_devs[i];
    return NULL;
}

/* ---------------------------------------------------------------- I2C controller */

#define I2C 0x17000
static int i2c_active, i2c_rw, i2c_first;
static I2cDev *i2c_cur;
static uint8_t i2c_rx;

static void i2c_irq(void) {
    uint32_t cr = IOREG(I2C + 8), sr = IOREG(I2C + 0xC);
    irq_set(39, (cr & 0x40) && (sr & 2));
}

static void i2c_complete(int ack) {
    uint32_t sr = IOREG(I2C + 0xC);
    sr |= 0x80 | 0x02;               /* ICF, IIF */
    sr = ack ? sr & ~1u : sr | 1u;   /* RXAK=1 means no ack */
    IOREG(I2C + 0xC) = sr;
    i2c_irq();
}

static uint8_t i2c_next_read(void) {
    if (!i2c_cur) return 0xFF;
    uint8_t v = i2c_cur->mem[i2c_cur->ptr & 0xFF];
    i2c_cur->ptr++;
    return v;
}

static void i2c_write_cr(uint32_t old, uint32_t v) {
    if (!(v & 0x80)) { IOREG(I2C + 0xC) = 0; i2c_active = 0; return; }
    uint32_t sr = IOREG(I2C + 0xC);
    if (((v & 0x20) && !(old & 0x20)) || (v & 0x04)) {   /* START / repeated START */
        i2c_active = 1; i2c_first = 1; i2c_cur = NULL;
        sr |= 0x20;
        IOREG(I2C + 8) = v & ~0x04u;
    }
    if (!(v & 0x20) && (old & 0x20)) {                     /* STOP */
        i2c_active = 0;
        sr &= ~0x20u;
        if (zod.log_i2c) fprintf(stderr, "[I2C] stop\n");
    }
    IOREG(I2C + 0xC) = sr;
    i2c_irq();
}

static void i2c_write_dr(uint32_t v) {
    if (!i2c_active) return;
    v &= 0xFF;
    if (i2c_first) {
        i2c_first = 0;
        i2c_rw = v & 1;
        i2c_cur = i2c_find(v >> 1);
        if (i2c_cur && !i2c_rw) i2c_cur->got_ptr = 0;
        if (zod.log_i2c) fprintf(stderr, "[I2C] start addr %02x %s%s pc=%08x\n", v >> 1, i2c_rw ? "read" : "write", i2c_cur ? "" : " (nobody)", cpu.r[15]);
        if (i2c_cur && i2c_rw) i2c_rx = i2c_next_read();
        i2c_complete(i2c_cur != NULL);
        return;
    }
    if (zod.log_i2c) fprintf(stderr, "[I2C]   data %02x\n", v);
    if (i2c_cur) {
        if (!i2c_cur->got_ptr) { i2c_cur->ptr = v; i2c_cur->got_ptr = 1; }
        else i2c_cur->mem[i2c_cur->ptr++ & 0xFF] = (uint8_t)v;
    }
    i2c_complete(i2c_cur != NULL);
}

static uint32_t i2c_read_dr(void) {
    uint8_t v = i2c_rx;
    /* in receive mode, reading I2DR starts the next byte */
    if (i2c_active && !(IOREG(I2C + 8) & 0x10)) {
        i2c_rx = i2c_next_read();
        i2c_complete(1);
        if (zod.log_i2c) fprintf(stderr, "[I2C]   read %02x\n", v);
    }
    return v;
}

/* ---------------------------------------------------------------- ASP (touch panel ADC) */
/* Behaviour inferred from the DAL: ICNTLR bit4 enables the pen-down interrupt (source 46),
 * ISTATR bit10 is the pen-down flag (write 1 to clear), register 0x34 holds the FIFO
 * position, and PADFIFO samples come in groups of four (auto-zero, X, Y, aux) with the
 * value in bits 15..7. */
#define ASP 0x15000
static int pen_down, pen_x, pen_y;     /* raw 9-bit panel coordinates */
static unsigned asp_pos;

void zod_touch(int x, int y, int down) {
    /* landscape screen (480x320) -> raw panel coordinates (0..511) */
    if (x < 0) x = 0;
    if (x > 479) x = 479;
    if (y < 0) y = 0;
    if (y > 319) y = 319;
    /* In landscape the DAL passes panel samples through almost unchanged, so feed it
     * screen coordinates; Palm OS's own digitizer calibration then comes out near identity. */
    pen_x = x;
    pen_y = y;
    if (down && !pen_down) {                /* pen-down edge latches ISTATR bit10 */
        IOREG(ASP + 0x1C) |= 0x400;
        if (IOREG(ASP + 0x18) & 0x10) irq_set(46, 1);
    }
    pen_down = down;
}

static uint32_t pen_raw(int v, int range, int axis) {
    static int init, shift = 7, inv = 0, lo = 0, hi = 0;
    if (!init) {
        const char *e;
        init = 1;
        if ((e = getenv("PENSHIFT"))) shift = atoi(e);
        if ((e = getenv("PENINV"))) inv = atoi(e);
        if ((e = getenv("PENLO"))) lo = atoi(e);
        if ((e = getenv("PENHI"))) hi = atoi(e);
    }
    if (inv & (1 << axis)) v = range - 1 - v;
    if (hi > lo) return (uint32_t)(lo + (hi - lo) * v / range) & 0xFFFF;
    return ((uint32_t)v << shift) & 0xFFFF;
}

static uint32_t asp_sample(void) {
    unsigned k = asp_pos++ & 3;
    if (k == 0) return pen_down ? 0 : 0xFF80;               /* auto-zero reference (no contact: high) */
    if (k == 1) return pen_raw(pen_x, 480, 0);
    if (k == 2) return pen_raw(pen_y, 320, 1);
    return 0x2000;                                          /* aux */
}

/* ---------------------------------------------------------------- dispatch */

void soc_init(void) {
    memset(i2c_devs, 0, sizeof i2c_devs);
    n_i2c_devs = 0;
    /* 0x50: serial EEPROM */
    i2c_devs[n_i2c_devs].addr = 0x50; memset(i2c_devs[n_i2c_devs].mem, 0xFF, 256); n_i2c_devs++;
    i2c_active = 0; i2c_cur = NULL;
    pen_down = 0; asp_pos = 0;
}

int soc_read(uint32_t off, uint32_t *v) {
    switch (off) {
    case I2C + 0x10: *v = i2c_read_dr(); return 1;
    case ASP + 0x00: *v = asp_sample(); return 1;
    case ASP + 0x34: *v = 12; return 1;
    case ASP + 0x1C: *v = IOREG(off) | (pen_down ? 0 : 0x400); return 1;   /* bit10 also reads set while the pen is up */           /* samples available (FIFO kept full) */
    }
    return 0;
}

void soc_write(uint32_t off, uint32_t v, uint32_t old) {
    switch (off) {
    case I2C + 0x08: i2c_write_cr(old, v); break;
    case I2C + 0x0C: /* only IAL/IIF are writable (write 0 to clear) */
        IOREG(off) = (old & ~0x12u) | (old & v & 0x12u);
        i2c_irq();
        break;
    case I2C + 0x10: i2c_write_dr(v); break;
    case ASP + 0x10: asp_pos = 0; break;
    case ASP + 0x18: IOREG(off) = v; irq_set(46, (v & 0x10) && (IOREG(ASP + 0x1C) & 0x400)); break;
    case ASP + 0x1C: IOREG(off) = old & ~v; irq_set(46, (IOREG(ASP + 0x18) & 0x10) && (IOREG(off) & 0x400)); break;
    }
}

void soc_tick(int cycles) { (void)cycles; }

void soc_state(StateIO *s) {
    ST(s, n_i2c_devs);
    for (int i = 0; i < 8; i++) { ST(s, i2c_devs[i].addr); ST(s, i2c_devs[i].mem); ST(s, i2c_devs[i].ptr); ST(s, i2c_devs[i].got_ptr); }
    int cur = i2c_cur ? (int)(i2c_cur - i2c_devs) : -1;
    ST(s, i2c_active); ST(s, i2c_rw); ST(s, i2c_first); ST(s, cur); ST(s, i2c_rx);
    if (s->loading) i2c_cur = cur >= 0 && cur < 8 ? &i2c_devs[cur] : NULL;
    ST(s, pen_down); ST(s, pen_x); ST(s, pen_y); ST(s, asp_pos);
}

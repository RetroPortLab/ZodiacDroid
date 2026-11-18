/* ZodiacDroid - other i.MX1 on-chip blocks: I2C (with the board's I2C devices), and stubs */
#include "zodiac.h"
#include "cpu.h"
#include "io.h"
#include <stdio.h>
#include <string.h>

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

/* ---------------------------------------------------------------- dispatch */

void soc_init(void) {
    memset(i2c_devs, 0, sizeof i2c_devs);
    n_i2c_devs = 0;
    /* 0x50: serial EEPROM */
    i2c_devs[n_i2c_devs].addr = 0x50; memset(i2c_devs[n_i2c_devs].mem, 0xFF, 256); n_i2c_devs++;
    i2c_active = 0; i2c_cur = NULL;
}

int soc_read(uint32_t off, uint32_t *v) {
    switch (off) {
    case I2C + 0x10: *v = i2c_read_dr(); return 1;
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
    }
}

void soc_tick(int cycles) { (void)cycles; }

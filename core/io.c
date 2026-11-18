/* ZodiacDroid - i.MX1 (MC9328MX1) on-chip peripherals.
 * AITC interrupt controller, GPT timers, RTC, UARTs, CRM, GPIO; everything else is a
 * plain register file. External chip selects go to ext.c (W4200 etc). */
#include "zodiac.h"
#include "state.h"
#include "cpu.h"
#include "io.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#define IOREG(off) zod.io[(off) >> 2]

static int logs;

static const char *periph_name(uint32_t pa) {
    if (pa - IO_BASE >= IO_SIZE) return "EXT";
    static const char *n[0x30] = {
        "AIPI1","WDT","TIM1","TIM2","RTC","LCDC","UART1","UART2","PWM","DMA","?0A","?0B","?0C","?0D","?0E","?0F",
        "AIPI2","SIM","USBD","CSPI1","MMC","ASP","BTA","I2C","SSI","CSPI2","MSHC","CRM","GPIO","?1D","?1E","?1F",
        "EIM","SDRAMC","MMA","AITC","CSI","?25","?26","?27","?28","?29","?2A","?2B","?2C","?2D","?2E","?2F" };
    return n[(pa - IO_BASE) >> 12];
}

void io_log(const char *what, uint32_t pa, uint32_t v, int size) {
    if (!zod.log_io || logs >= zod.log_limit) return;
    if (pa == 0x00202010 && zod.log_filter != 0x00202000) return;   /* free-running counter polls drown everything */
    if (zod.log_filter) { uint32_t m = ~((zod.log_filter & -zod.log_filter) - 1); if ((pa & m) != zod.log_filter) return; }
    logs++;
    fprintf(stderr, "%-5s %s%d %08x %s %08x  pc=%08x lr=%08x\n", periph_name(pa), what, size * 8, pa,
            what[0] == 'R' ? "->" : "<-", v, cpu.r[15], cpu.r[14]);
}

/* ---------------------------------------------------------------- AITC */

static uint64_t src_level;      /* raw source lines */
static uint64_t src_force;
static uint64_t enabled, fiqtype;
static uint8_t prio[64];
static int nimask = -1;

#define AITC 0x23000
static void aitc_update(void) {
    uint64_t pend = (src_level | src_force) & enabled;
    uint64_t np = pend & ~fiqtype, fp = pend & fiqtype;
    int best = -1, bp = -1;
    for (int i = 63; i >= 0; i--) if ((np >> i) & 1) {
        if (prio[i] > bp && (nimask < 0 || prio[i] > nimask)) { bp = prio[i]; best = i; }
    }
    int fbest = -1;
    for (int i = 63; i >= 0; i--) if ((fp >> i) & 1) { fbest = i; break; }
    IOREG(AITC + 0x40) = best >= 0 ? ((uint32_t)best << 16) | (uint32_t)bp : 0xFFFFFFFF;
    IOREG(AITC + 0x44) = fbest >= 0 ? (uint32_t)fbest : 0xFFFFFFFF;
    IOREG(AITC + 0x48) = (uint32_t)((src_level | src_force) >> 32);
    IOREG(AITC + 0x4C) = (uint32_t)(src_level | src_force);
    IOREG(AITC + 0x58) = (uint32_t)(np >> 32); IOREG(AITC + 0x5C) = (uint32_t)np;
    IOREG(AITC + 0x60) = (uint32_t)(fp >> 32); IOREG(AITC + 0x64) = (uint32_t)fp;
    int nidis = (IOREG(AITC) >> 22) & 1, fidis = (IOREG(AITC) >> 21) & 1;   /* INTCNTL.NIDIS / FIDIS */
    cpu_set_irq(best >= 0 && !nidis);
    cpu_set_fiq(fbest >= 0 && !fidis);
}

void irq_set(int src, int level) {
    uint64_t b = 1ull << src;
    uint64_t old = src_level;
    if (level) src_level |= b; else src_level &= ~b;
    if (old != src_level) aitc_update();
}

static void aitc_write(uint32_t off, uint32_t v) {
    switch (off) {
    case 0x00: IOREG(AITC) = v; break;
    case 0x04: nimask = (v & 0x1F) == 0x1F ? -1 : (int)(v & 0x1F); IOREG(AITC + 4) = v; break;
    case 0x08: enabled |= 1ull << (v & 63); break;
    case 0x0C: enabled &= ~(1ull << (v & 63)); break;
    case 0x10: enabled = (enabled & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case 0x14: enabled = (enabled & ~0xFFFFFFFFull) | v; break;
    case 0x18: fiqtype = (fiqtype & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case 0x1C: fiqtype = (fiqtype & ~0xFFFFFFFFull) | v; break;
    case 0x50: src_force = (src_force & 0xFFFFFFFFull) | ((uint64_t)v << 32); break;
    case 0x54: src_force = (src_force & ~0xFFFFFFFFull) | v; break;
    default:
        if (off >= 0x20 && off < 0x40) {
            int base = (7 - (int)(off - 0x20) / 4) * 8;
            for (int i = 0; i < 8; i++) prio[base + i] = (v >> (i * 4)) & 15;
            IOREG(AITC + off) = v;
        }
        return;
    }
    aitc_update();
}

static uint32_t aitc_read(uint32_t off) {
    switch (off) {
    case 0x10: return (uint32_t)(enabled >> 32);
    case 0x14: return (uint32_t)enabled;
    case 0x18: return (uint32_t)(fiqtype >> 32);
    case 0x1C: return (uint32_t)fiqtype;
    case 0x50: return (uint32_t)(src_force >> 32);
    case 0x54: return (uint32_t)src_force;
    }
    return IOREG(AITC + off);
}

/* ---------------------------------------------------------------- clocks */

uint32_t perclk1_hz(void) {
    uint32_t pcdr = IOREG(0x1B020);
    return 96000000u / ((pcdr & 15) + 1);
}

/* ---------------------------------------------------------------- GPT timers */

typedef struct {
    int irq;
    uint32_t tctl, tprer, tcmp, tstat, tstat_read;
    uint64_t count_fp;    /* counter, 32.32 fixed point, as of last_cycle */
    uint64_t last_cycle;
} Gpt;
static Gpt gpt[2];

static uint64_t gpt_rate(Gpt *t) {   /* counter ticks per CPU cycle, 32.32 */
    if (!(t->tctl & 1)) return 0;
    uint32_t hz;
    switch ((t->tctl >> 1) & 7) {
    case 0: return 0;
    case 1: hz = perclk1_hz(); break;
    case 2: hz = perclk1_hz() / 16; break;
    case 3: return 0;   /* TIN pin: not connected */
    default: hz = 32768; break;
    }
    hz /= (t->tprer & 0xFF) + 1;
    return ((uint64_t)hz << 32) / zod.cpu_hz;
}

static void gpt_sync(Gpt *t, uint64_t now) {
    if (now <= t->last_cycle) return;
    uint64_t dt = now - t->last_cycle;
    t->last_cycle = now;
    uint64_t rate = gpt_rate(t);
    if (!rate) return;
    uint32_t c = (uint32_t)(t->count_fp >> 32);
    uint64_t frac = (t->count_fp & 0xFFFFFFFFull) + dt * rate;
    uint64_t adv = frac >> 32;                     /* whole ticks advanced */
    frac &= 0xFFFFFFFFull;
    uint64_t to_cmp = (uint32_t)(t->tcmp - c);     /* ticks until counter == tcmp */
    if (to_cmp == 0) to_cmp = 0x100000000ull;
    if (adv >= to_cmp) {
        t->tstat |= 1;
        if (t->tctl & 0x10) irq_set(t->irq, 1);
        if (!(t->tctl & 0x100)) {                  /* restart mode: back to 0 after compare */
            uint64_t period = t->tcmp ? (uint64_t)t->tcmp : 0x100000000ull;
            uint64_t rest = (adv - to_cmp) % period;
            c = (uint32_t)rest;
            t->count_fp = ((uint64_t)c << 32) | frac;
            return;
        }
    }
    c += (uint32_t)adv;
    t->count_fp = ((uint64_t)c << 32) | frac;
}

static uint32_t gpt_read(Gpt *t, uint32_t r) {
    gpt_sync(t, cpu_now());
    switch (r) {
    case 0x00: return t->tctl;
    case 0x04: return t->tprer;
    case 0x08: return t->tcmp;
    case 0x0C: return 0;
    case 0x10: return (uint32_t)(t->count_fp >> 32);
    case 0x14: t->tstat_read |= t->tstat; return t->tstat;
    }
    return 0;
}

static void gpt_write(Gpt *t, uint32_t r, uint32_t v) {
    gpt_sync(t, cpu_now());
    switch (r) {
    case 0x00:
        if (v & 0x8000) { t->tctl = 0; t->tprer = 0; t->tcmp = 0xFFFFFFFF; t->tstat = 0; t->count_fp = 0; break; }
        if ((v & 1) && !(t->tctl & 1)) t->count_fp = 0;   /* counter restarts on enable */
        t->tctl = v;
        break;
    case 0x04: t->tprer = v & 0xFF; break;
    case 0x08: t->tcmp = v; if (!(t->tctl & 0x100)) t->count_fp = 0; break;
    case 0x14: /* clear bits that were read as 1, or written as 1 */
        t->tstat &= ~(t->tstat_read | v); t->tstat_read = 0;
        break;
    }
    if (!(t->tstat & 1) || !(t->tctl & 0x10)) irq_set(t->irq, 0);
    else irq_set(t->irq, 1);
}

static int64_t gpt_until(Gpt *t) {   /* cycles until the next compare, or -1 */
    uint64_t rate = gpt_rate(t);
    if (!rate || !(t->tctl & 0x10)) return -1;
    uint32_t c = (uint32_t)(t->count_fp >> 32);
    uint64_t ticks = (uint32_t)(t->tcmp - c);
    if (!ticks) ticks = 0x100000000ull;
    if (ticks > 0x7FFFFFFF) return 0x7FFFFFFF;
    uint64_t need = (ticks << 32) - (t->count_fp & 0xFFFFFFFFull);
    uint64_t cyc = need / rate + 1;
    return cyc > 0x7FFFFFFF ? 0x7FFFFFFF : (int64_t)cyc;
}

/* ---------------------------------------------------------------- RTC */

#define RTC 0x04000
static uint64_t rtc_base_sec;     /* wall time (seconds) at emulated time 0 */
static uint32_t rtc_isr;
static uint64_t rtc_last_ms;

static uint64_t emu_ms(void) { return cpu_now() / (zod.cpu_hz / 1000); }

static void rtc_sync(void) {
    uint64_t ms = emu_ms();
    if (ms != rtc_last_ms) {
        uint64_t a = rtc_last_ms, b = ms;
        if (a / 1000 != b / 1000) rtc_isr |= 0x10;                                   /* 1Hz */
        if (a / 500 != b / 500) rtc_isr |= 0x80;                                     /* 2Hz */
        uint64_t s0 = rtc_base_sec + a / 1000, s1 = rtc_base_sec + b / 1000;
        if (s0 / 60 != s1 / 60) rtc_isr |= 0x02;
        if (s0 / 3600 != s1 / 3600) rtc_isr |= 0x20;
        if (s0 / 86400 != s1 / 86400) rtc_isr |= 0x08;
        {   /* alarm: DAYALARM:ALRM_HM:ALRM_SEC (Palm OS uses it for procedure alarms and timed events) */
            uint32_t hm = IOREG(RTC + 0x08), sec = IOREG(RTC + 0x0C), day = IOREG(RTC + 0x24);
            uint64_t at = (uint64_t)(day & 0x1FF) * 86400 + ((hm >> 8) & 31) * 3600 + (hm & 63) * 60 + (sec & 63);
            uint64_t d0 = s0 % (512ull * 86400), d1 = s1 % (512ull * 86400);
            if (d1 >= d0 ? (at > d0 && at <= d1) : (at > d0 || at <= d1)) rtc_isr |= 0x04;
        }
        for (int k = 0; k < 8; k++) {                                                /* SAM0 4Hz .. SAM7 512Hz */
            uint64_t hz = 4u << k;
            if (a * hz / 1000 != b * hz / 1000) rtc_isr |= 0x100u << k;
        }
        rtc_last_ms = ms;
    }
    uint32_t en = IOREG(RTC + 0x18);
    irq_set(17, (rtc_isr & en & 0x00FF) != 0);
    irq_set(18, (rtc_isr & en & 0xFF00) != 0);
}

static uint32_t rtc_read(uint32_t r) {
    rtc_sync();
    uint64_t t = rtc_base_sec + emu_ms() / 1000;
    switch (r) {
    case 0x00: return (uint32_t)(((t / 3600) % 24) << 8 | ((t / 60) % 60));
    case 0x04: return (uint32_t)(t % 60);
    case 0x14: return rtc_isr;
    case 0x20: return (uint32_t)((t / 86400) & 0x1FF);
    }
    return IOREG(RTC + r);
}

static void rtc_write(uint32_t r, uint32_t v) {
    rtc_sync();
    uint64_t el = emu_ms() / 1000, t = rtc_base_sec + el;
    switch (r) {
    case 0x00: rtc_base_sec = (t / 86400) * 86400 + ((v >> 8) & 31) * 3600 + (v & 63) * 60 + t % 60 - el; break;
    case 0x04: rtc_base_sec = t - t % 60 + (v & 63) - el; break;
    case 0x14: rtc_isr &= ~v; break;
    case 0x20: rtc_base_sec = (uint64_t)(v & 0x1FF) * 86400 + t % 86400 - el; break;
    default: IOREG(RTC + r) = v;
    }
    rtc_sync();
}

/* ---------------------------------------------------------------- UART */

static char uart_line[2][256];
static int uart_len[2];

static uint32_t uart_read(int u, uint32_t r) {
    uint32_t base = 0x06000 + u * 0x1000;
    switch (r) {
    case 0x00: return 0;                     /* no receive data */
    case 0x94: return 0x2000 | 0x0040;       /* USR1: TRDY | RXDS */
    case 0x98: return 0x4008;                /* USR2: TXFE | TXDC */
    case 0xB4: return 0x60;                  /* UTS: TXEMPTY | RXEMPTY */
    }
    return IOREG(base + r);
}

static void uart_write(int u, uint32_t r, uint32_t v) {
    uint32_t base = 0x06000 + u * 0x1000;
    if (r == 0x40) {
        char c = (char)v;
        if (c == '\n' || c == '\r' || uart_len[u] >= 250) {
            if (uart_len[u]) { uart_line[u][uart_len[u]] = 0; if (zod.log_uart) fprintf(stderr, "[UART%d] %s\n", u + 1, uart_line[u]); }
            uart_len[u] = 0;
        } else uart_line[u][uart_len[u]++] = c;
        return;
    }
    IOREG(base + r) = v;
}

/* ---------------------------------------------------------------- GPIO */

uint32_t gpio_in[4];
static uint32_t gpio_isr[4];
static const int gpio_irqn[4] = { 11, 12, 13, 62 };

uint32_t gpio_pins(int p) {
    uint32_t dir = IOREG(0x1C000 + p * 0x100), dr = IOREG(0x1C01C + p * 0x100);
    return (dr & dir) | (gpio_in[p] & ~dir);
}

static void gpio_update_irq(int p) {
    uint32_t imr = IOREG(0x1C030 + p * 0x100);
    uint32_t pins = gpio_pins(p);
    for (int b = 0; b < 32; b++) {
        if (!((imr >> b) & 1)) continue;
        uint32_t icr = (IOREG(0x1C028 + p * 0x100 + (b >> 4) * 4) >> ((b & 15) * 2)) & 3;
        int lv = (pins >> b) & 1;
        if (icr == 2 && lv) gpio_isr[p] |= 1u << b;          /* high level */
        if (icr == 3 && !lv) gpio_isr[p] |= 1u << b;         /* low level */
    }
    irq_set(gpio_irqn[p], (gpio_isr[p] & imr) != 0);
}

void gpio_set_input(int port, uint32_t mask, int level) {
    uint32_t before = gpio_pins(port);
    if (level) gpio_in[port] |= mask; else gpio_in[port] &= ~mask;
    uint32_t after = gpio_pins(port), ch = before ^ after;
    for (int b = 0; b < 32; b++) if ((ch >> b) & 1) {
        uint32_t icr = (IOREG(0x1C028 + port * 0x100 + (b >> 4) * 4) >> ((b & 15) * 2)) & 3;
        int lv = (after >> b) & 1;
        if ((icr == 0 && lv) || (icr == 1 && !lv)) gpio_isr[port] |= 1u << b;   /* rising / falling edge */
    }
    gpio_update_irq(port);
}

/* ---------------------------------------------------------------- dispatch */

static uint32_t io_read32(uint32_t off) {
    uint32_t blk = off & ~0xFFFu, r = off & 0xFFF;
    switch (blk) {
    case 0x02000: return gpt_read(&gpt[0], r);
    case 0x03000: return gpt_read(&gpt[1], r);
    case 0x04000: return rtc_read(r);
    case 0x06000: return uart_read(0, r);
    case 0x07000: return uart_read(1, r);
    case 0x1B000:
        if (r == 0x00) return IOREG(off) & ~0x00600000u;           /* PLL restart bits self-clear */
        if (r == 0x08 || r == 0x10) return IOREG(off) | 0x8000;    /* PLL locked */
        break;
    case 0x1C000:
        if (r < 0x400 && (r & 0xFF) == 0x24) return gpio_pins(r >> 8);
        if (r < 0x400 && (r & 0xFF) == 0x34) return gpio_isr[r >> 8];
        break;
    case 0x23000: return aitc_read(r);
    default:
        if ((blk >= 0x10000 && blk < 0x30000) || blk == 0x09000) { uint32_t v; if (soc_read(off, &v)) return v; }
        break;
    }
    return IOREG(off);
}

static void io_write32(uint32_t off, uint32_t v) {
    uint32_t blk = off & ~0xFFFu, r = off & 0xFFF;
    switch (blk) {
    case 0x02000: gpt_write(&gpt[0], r, v); return;
    case 0x03000: gpt_write(&gpt[1], r, v); return;
    case 0x04000: rtc_write(r, v); return;
    case 0x06000: uart_write(0, r, v); return;
    case 0x07000: uart_write(1, r, v); return;
    case 0x1C000:
        if (r < 0x400 && (r & 0xFF) == 0x34) { gpio_isr[r >> 8] &= ~v; gpio_update_irq(r >> 8); return; }
        IOREG(off) = v;
        if (r < 0x400) gpio_update_irq(r >> 8);
        return;
    case 0x23000: aitc_write(r, v); return;
    }
    uint32_t old = IOREG(off);
    IOREG(off) = v;
    soc_write(off, v, old);
}

uint32_t io_read(uint32_t pa, int size) {
    if (pa - IO_BASE >= IO_SIZE) return ext_read(pa, size);
    uint32_t v = io_read32((pa - IO_BASE) & ~3u);
    if (size == 2) v = (v >> ((pa & 2) * 8)) & 0xFFFF;
    else if (size == 1) v = (v >> ((pa & 3) * 8)) & 0xFF;
    io_log("R", pa, v, size);
    return v;
}

void io_write(uint32_t pa, uint32_t v, int size) {
    if (pa - IO_BASE >= IO_SIZE) { ext_write(pa, v, size); return; }
    uint32_t off = (pa - IO_BASE) & ~3u;
    io_log("W", pa, v, size);
    if (size != 4) {   /* merge partial writes into the current register value */
        uint32_t cur = IOREG(off);
        int s = size == 2 ? (pa & 2) * 8 : (pa & 3) * 8;
        uint32_t m = (size == 2 ? 0xFFFFu : 0xFFu) << s;
        v = (cur & ~m) | ((v << s) & m);
    }
    io_write32(off, v);
}

void io_init(void) {
    memset(zod.io, 0, sizeof zod.io);
    logs = 0;
    src_level = src_force = enabled = fiqtype = 0;
    memset(prio, 0, sizeof prio);
    nimask = -1;
    memset(gpt, 0, sizeof gpt);
    gpt[0].irq = 59; gpt[0].tcmp = 0xFFFFFFFF;
    gpt[1].irq = 58; gpt[1].tcmp = 0xFFFFFFFF;
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    rtc_base_sec = (uint64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    rtc_isr = 0; rtc_last_ms = 0;
    memset(gpio_isr, 0, sizeof gpio_isr);
    for (int i = 0; i < 4; i++) gpio_in[i] = 0xFFFFFFFF;
    if (zod.model == 2) gpio_in[3] &= ~0x40;   /* PD6 low = Zodiac 2 (two SDRAM banks) */
    gpio_in[1] &= ~(1u << 14);                 /* PB14 low = external power present (no battery measurement is emulated) */
    if (getenv("GPIOLOW")) { int port, pin; const char *s = getenv("GPIOLOW"); while (sscanf(s, "%d.%d", &port, &pin) == 2) { gpio_in[port] &= ~(1u << pin); s = strchr(s, ','); if (!s) break; s++; } }
    IOREG(0x1B000) = 0x2F00AC03;    /* CSCR reset value */
    IOREG(0x1B020) = 0x00000051;    /* PCDR */
    IOREG(0x01008) = 0x00000000;    /* WSTR: no watchdog time-out (a set TOUT bit makes Palm OS treat the boot as a crash) */
    soc_init();
    ext_init();
    aitc_update();
}

int io_next_event(int max) {
    int64_t best = max;
    for (int i = 0; i < 2; i++) {
        gpt_sync(&gpt[i], cpu.cycles);
        int64_t u = gpt_until(&gpt[i]);
        if (u >= 0 && u < best) best = u;
    }
    if (best < 16) best = 16;
    return (int)best;
}

void io_tick(int cycles) {
    uint64_t now = cpu.cycles;
    gpt_sync(&gpt[0], now);
    gpt_sync(&gpt[1], now);
    rtc_sync();
    soc_tick(cycles);
    ext_tick(cycles);
}

void rtc_advance(int64_t sec) { if (sec > 0) rtc_base_sec += (uint64_t)sec; }

void io_state(StateIO *s) {
    ST(s, src_level); ST(s, src_force); ST(s, enabled); ST(s, fiqtype); ST(s, prio); ST(s, nimask);
    for (int i = 0; i < 2; i++) {
        ST(s, gpt[i].tctl); ST(s, gpt[i].tprer); ST(s, gpt[i].tcmp); ST(s, gpt[i].tstat); ST(s, gpt[i].tstat_read);
        ST(s, gpt[i].count_fp); ST(s, gpt[i].last_cycle);
    }
    ST(s, rtc_base_sec); ST(s, rtc_isr); ST(s, rtc_last_ms);
    ST(s, gpio_in); ST(s, gpio_isr);
    if (s->loading) aitc_update();
}

/* Zodiac buttons -> GPIO inputs (active low). Home, Function and Power were identified by
 * their effect; the action buttons are the four that set key bits 28-31. */
void zod_buttons(uint32_t mask) {
    static const struct { uint32_t bit; int port, pin; } map[] = {
        { ZB_HOME, 0, 7 }, { ZB_FUNCTION, 3, 12 }, { ZB_POWER, 0, 10 },
        { ZB_A, 3, 9 }, { ZB_B, 3, 8 }, { ZB_C, 3, 14 }, { ZB_D, 3, 10 },
        { ZB_L, 3, 13 }, { ZB_R, 0, 8 }, { ZB_BOOKMARK, 0, 9 }, { ZB_EXTRA, 3, 7 },
    };
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        gpio_set_input(map[i].port, 1u << map[i].pin, (mask & map[i].bit) ? 0 : 1);
}

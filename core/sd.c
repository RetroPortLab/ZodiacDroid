/* ZodiacDroid - SD card: the i.MX1 MMC/SD host controller (SDHC at 0x00214000), the card
 * itself (an SD 1.x standard-capacity card backed by a disk image file), and the i.MX1 DMA
 * channels the Tapwave slot driver uses to move sector data through the SDHC buffer.
 *
 * Card detect is GPIO B20 (low = inserted). Responses are read from RES_FIFO as 16-bit
 * words; for 48-bit responses: [cmd index | bits 39..32], [31..16 of the payload >> 8 ...],
 * exactly as the driver reassembles them. */
#include "zodiac.h"
#include "cpu.h"
#include "io.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IOREG(off) zod.io[(off) >> 2]
#define SDHC 0x14000
#define DMA  0x09000
#define SDHC_BUF_PA 0x00214038u

/* STATUS bits */
#define ST_TIME_OUT_READ   (1u << 0)
#define ST_TIME_OUT_RESP   (1u << 1)
#define ST_BUFF_FE         (1u << 6)
#define ST_BUFF_FF         (1u << 7)
#define ST_CLK_RUN         (1u << 8)
#define ST_DATA_DONE       (1u << 11)
#define ST_WRITE_DONE      (1u << 12)
#define ST_END_CMD_RESP    (1u << 13)

static struct {
    /* host */
    uint32_t status;
    int clk_run;
    int cmd_pending;
    uint16_t resp[8]; int resp_n, resp_pos;
    /* card */
    int inserted;
    int state;              /* 0 idle 1 ready 2 ident 3 stby 4 tran 5 data 6 rcv */
    int app_cmd;
    uint16_t rca;
    uint32_t blklen;
    int bus4;
    /* data phase */
    int dir;                /* 0 none, 1 card->host, 2 host->card */
    int multi;
    uint64_t addr;          /* byte address of the next block */
    uint32_t left;          /* bytes left in this transfer (single block or NOB blocks) */
    uint8_t buf[512];
    uint32_t buf_pos, buf_len;
    int special;            /* data comes from buf only (SCR, SD status) */
} sd;

static int card_fd = -1;
static uint64_t card_size;
int sd_log;

/* ---------------------------------------------------------------- card image */

void sd_attach(int fd, uint64_t size) {
    int was = sd.inserted;
    card_fd = fd;
    card_size = size;
    sd.inserted = fd >= 0;
    if (was != sd.inserted) sd.state = 0;   /* a new card starts idle; the same card keeps its state */
    gpio_set_input(1, 1u << 20, sd.inserted ? 0 : 1);   /* card detect, active low */
}

static void card_read(uint64_t addr, uint8_t *out) {
    memset(out, 0, 512);
    if (card_fd >= 0 && addr + 512 <= card_size && pread(card_fd, out, 512, (off_t)addr) < 0) memset(out, 0, 512);
}

static void card_write(uint64_t addr, const uint8_t *in) {
    if (card_fd >= 0 && addr + 512 <= card_size && pwrite(card_fd, in, 512, (off_t)addr) < 0) { /* ignore */ }
}

/* ---------------------------------------------------------------- card registers */

static void cid(uint8_t c[16]) {
    static const uint8_t v[16] = { 0x03, 'S', 'D', 'Z', 'O', 'D', 'I', 'A', 'C', 0x10, 0x12, 0x34, 0x56, 0x78, 0x00, 0x41 };
    memcpy(c, v, 16);
}

static void csd(uint8_t c[16]) {
    /* CSD v1.0: READ_BL_LEN = 9, C_SIZE_MULT = 7 (x512), C_SIZE from the image size */
    uint32_t blocks = (uint32_t)(card_size / 512);
    uint32_t c_size = blocks / 512; if (c_size) c_size--; if (c_size > 4095) c_size = 4095;
    memset(c, 0, 16);
    c[0] = 0x00;            /* CSD_STRUCTURE 0 */
    c[1] = 0x26;            /* TAAC */
    c[2] = 0x00;            /* NSAC */
    c[3] = 0x32;            /* TRAN_SPEED 25MHz */
    c[4] = 0x5F;            /* CCC (high) */
    c[5] = 0x59;            /* CCC (low) | READ_BL_LEN = 9 */
    c[6] = 0x80 | (uint8_t)((c_size >> 10) & 3);   /* READ_BL_PARTIAL, C_SIZE[11:10] */
    c[7] = (uint8_t)(c_size >> 2);
    c[8] = (uint8_t)((c_size & 3) << 6) | 0x2D;    /* C_SIZE[1:0], VDD currents */
    c[9] = 0xB4 | 0x03;                            /* currents, C_SIZE_MULT[2:1] = 3 */
    c[10] = 0x80 | 0x3F;                           /* C_SIZE_MULT[0] = 1, ERASE_BLK_EN, SECTOR_SIZE */
    c[11] = 0x80;
    c[12] = 0x0A; c[13] = 0x40;                    /* R2W_FACTOR, WRITE_BL_LEN = 9 */
    c[14] = 0x00;
    c[15] = 0x01;
}

static uint32_t card_status(void) {
    uint32_t s = (uint32_t)(sd.state & 15) << 9;
    if (sd.state == 4 || sd.state == 3) s |= 1u << 8;   /* READY_FOR_DATA */
    if (sd.app_cmd) s |= 1u << 5;
    return s;
}

/* ---------------------------------------------------------------- host side */

static void resp48(int cmd, uint32_t r) {
    sd.resp[0] = (uint16_t)(((cmd & 0x3F) << 8) | (r >> 24));
    sd.resp[1] = (uint16_t)(r >> 8);
    sd.resp[2] = (uint16_t)((r & 0xFF) << 8 | 0x01);
    sd.resp_n = 3; sd.resp_pos = 0;
}

static void resp136(const uint8_t b[16]) {
    /* like the 48-bit format, the start/header byte comes first: 0x3F, then bits 127..8 */
    uint8_t r[16];
    r[0] = 0x3F; memcpy(r + 1, b, 15);
    for (int i = 0; i < 8; i++) sd.resp[i] = (uint16_t)(r[i * 2] << 8 | r[i * 2 + 1]);
    sd.resp_n = 8; sd.resp_pos = 0;
}

static void start_read(uint64_t addr, int multi) {
    sd.dir = 1; sd.multi = multi; sd.addr = addr; sd.special = 0;
    uint32_t nob = IOREG(SDHC + 0x1C) & 0xFFFF, bl = IOREG(SDHC + 0x18) & 0xFFF;
    if (!bl) bl = 512;
    sd.left = multi ? (nob ? nob : 1) * bl : bl;
    sd.buf_pos = sd.buf_len = 0;
    sd.state = 5;
}

static void start_write(uint64_t addr, int multi) {
    sd.dir = 2; sd.multi = multi; sd.addr = addr; sd.special = 0;
    uint32_t nob = IOREG(SDHC + 0x1C) & 0xFFFF, bl = IOREG(SDHC + 0x18) & 0xFFF;
    if (!bl) bl = 512;
    sd.left = multi ? (nob ? nob : 1) * bl : bl;
    sd.buf_pos = 0; sd.buf_len = 512;
    sd.state = 6;
}

static void sdhc_event(uint32_t bit);

static void finish_data(void) {
    if (sd.dir == 1) { sd.status |= ST_DATA_DONE; sdhc_event(ST_DATA_DONE); }
    if (sd.dir == 2) { sd.status |= ST_WRITE_DONE; sdhc_event(ST_WRITE_DONE); }
    sd.dir = 0;
    if (sd.state == 5 || sd.state == 6) sd.state = 4;
}

/* next byte of a card->host transfer */
static int data_get(uint8_t *out) {
    if (sd.dir != 1 || !sd.left) return 0;
    if (sd.buf_pos >= sd.buf_len) {
        if (sd.special) return 0;
        card_read(sd.addr, sd.buf);
        sd.addr += 512; sd.buf_pos = 0; sd.buf_len = 512;
    }
    *out = sd.buf[sd.buf_pos++];
    if (--sd.left == 0) finish_data();
    return 1;
}

static int data_put(uint8_t v) {
    if (sd.dir != 2 || !sd.left) return 0;
    sd.buf[sd.buf_pos++] = v;
    if (sd.buf_pos >= 512) { card_write(sd.addr, sd.buf); sd.addr += 512; sd.buf_pos = 0; }
    if (--sd.left == 0) finish_data();
    return 1;
}

static void run_command(void) {
    uint32_t cmd = IOREG(SDHC + 0x28) & 0x3F;
    uint32_t arg = (IOREG(SDHC + 0x2C) & 0xFFFF) << 16 | (IOREG(SDHC + 0x30) & 0xFFFF);
    uint32_t cdc = IOREG(SDHC + 0x0C);
    int fmt = cdc & 3;
    int app = sd.app_cmd;
    sd.app_cmd = 0;
    sd.status &= ~(ST_END_CMD_RESP | ST_TIME_OUT_RESP | ST_DATA_DONE | ST_WRITE_DONE | ST_TIME_OUT_READ);
    sd.resp_n = 0;
    int ok = sd.inserted;
    if (ok) switch (app ? 0x40 | cmd : cmd) {
    case 0: sd.state = 0; sd.rca = 0; sd.dir = 0; break;
    case 8: ok = 0; break;                                   /* SD 1.x card: no response */
    case 55: sd.app_cmd = 1; resp48(cmd, card_status()); break;
    case 0x40 | 41: resp48(0x3F, 0x80FF8000u); if (sd.state == 0) sd.state = 1; break;   /* OCR: ready, 2.7-3.6V */
    case 1: ok = 0; break;                                   /* MMC only */
    case 2: { uint8_t c[16]; cid(c); resp136(c); sd.state = 2; break; }
    case 3: sd.rca = 0x1234; sd.state = 3; resp48(cmd, (uint32_t)sd.rca << 16 | (card_status() & 0x1F00) | 0x0500); break;
    case 9: { uint8_t c[16]; csd(c); resp136(c); break; }
    case 10: { uint8_t c[16]; cid(c); resp136(c); break; }
    case 7: sd.state = (arg >> 16) == sd.rca ? 4 : 3; resp48(cmd, card_status()); break;
    case 13: resp48(cmd, card_status()); break;
    case 0x40 | 13: resp48(cmd, card_status()); sd.dir = 1; sd.special = 1; memset(sd.buf, 0, 64);
        sd.buf_pos = 0; sd.buf_len = sd.left = 64; break;
    case 16: sd.blklen = arg; resp48(cmd, card_status()); break;
    case 0x40 | 6: sd.bus4 = (arg & 3) == 2; resp48(cmd, card_status()); break;
    case 0x40 | 42: resp48(cmd, card_status()); break;
    case 0x40 | 51: {                                        /* SCR: SD 1.10, 1- and 4-bit bus */
        static const uint8_t scr[8] = { 0x01, 0x25, 0, 0, 0, 0, 0, 0 };
        resp48(cmd, card_status());
        memcpy(sd.buf, scr, 8); sd.buf_pos = 0; sd.buf_len = sd.left = 8; sd.dir = 1; sd.special = 1;
        break;
    }
    case 6: resp48(cmd, card_status()); break;
    case 12: resp48(cmd, card_status()); if (sd.dir) finish_data(); sd.state = 4; break;
    case 17: resp48(cmd, card_status()); start_read(arg, 0); break;
    case 18: resp48(cmd, card_status()); start_read(arg, 1); break;
    case 24: resp48(cmd, card_status()); start_write(arg, 0); break;
    case 25: resp48(cmd, card_status()); start_write(arg, 1); break;
    case 32: case 33: case 38: resp48(cmd, card_status()); break;   /* erase: no-op */
    default: resp48(cmd, card_status()); break;
    }
    if (!ok && fmt) sd.status |= ST_TIME_OUT_RESP;
    sd.status |= ST_END_CMD_RESP;
    sdhc_event(ST_END_CMD_RESP);
    if (sd_log) fprintf(stderr, "[SD] %sCMD%u arg=%08x cdc=%03x -> %s\n", app ? "A" : "", cmd, arg, cdc, ok ? "ok" : "timeout");
}

static uint32_t sdhc_status(void);

/* SDHC interrupt (source 35): raised when END_CMD_RES, WRITE_OP_DONE or DATA_TRAN_DONE
 * happens while unmasked; the driver's handler acknowledges it by rewriting INT_MASK. */
static int sdhc_irq_pending;
static void sdhc_event(uint32_t bit) {
    uint32_t m = IOREG(SDHC + 0x24);
    if ((bit == ST_END_CMD_RESP && !(m & 4)) || (bit == ST_WRITE_DONE && !(m & 2)) || (bit == ST_DATA_DONE && !(m & 1)))
        sdhc_irq_pending = 1;
    irq_set(35, sdhc_irq_pending);
}
static void sdhc_irq(void) { irq_set(35, sdhc_irq_pending); }

static uint32_t sdhc_status(void) {
    uint32_t s = sd.status;
    if (sd.clk_run) s |= ST_CLK_RUN;
    if (sd.dir == 1 && sd.left) s |= ST_BUFF_FF;
    if (sd.dir == 2 && sd.left) s |= ST_BUFF_FE;
    return s;
}

/* ---------------------------------------------------------------- DMA */

#define SSI_STX_PA 0x00218000u
static void audio_start(int ch);
static struct { int active; uint32_t src, len; uint64_t acc; } ach[11];

static void dma_irq(void) {
    uint32_t disr = IOREG(DMA + 0x04), dimr = IOREG(DMA + 0x08);
    irq_set(61, (disr & ~dimr & 0x7FF) != 0);
}

/* Memory-to-memory transfers (software-started channels): done at once.
 * SMOD/DMOD: 0 linear, 1 2D (rows of X bytes, W apart, Y rows from the XSR/YSR/WSR set chosen by MSEL), 2 FIFO. */
static uint8_t *dma_ptr(uint32_t pa, int write) {
    uint8_t *page = bus_page(pa & ~0xFFFu, write);
    return page ? page + (pa & 0xFFF) : NULL;
}

static void dma_byte_copy(uint32_t s, uint32_t d) {
    uint8_t *ps = dma_ptr(s, 0), *pd = dma_ptr(d, 1);
    uint8_t v = ps ? *ps : (uint8_t)bus_io_read(s, 1);
    if (pd) { *pd = v; cpu_phys_written(d, 1); } else bus_io_write(d, v, 1);
}

static void dma_mem(int ch) {
    uint32_t base = DMA + 0x80 + ch * 0x40;
    uint32_t ccr = IOREG(base + 0x0C), sar = IOREG(base), dar = IOREG(base + 4), cnt = IOREG(base + 8) & 0xFFFFFF;
    int smod = (ccr >> 10) & 3, dmod = (ccr >> 12) & 3, set = (ccr >> 8) & 1;
    uint32_t xs = IOREG(DMA + 0x50 + set * 0x0C) & 0xFFFF, ys = IOREG(DMA + 0x54 + set * 0x0C) & 0xFFFF, ws = IOREG(DMA + 0x58 + set * 0x0C) & 0xFFFF;
    /* fast path: both linear and both in plain memory */
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t so = i, doff = i;
        if (smod == 1 && xs) so = (i / xs) * ws + i % xs;
        if (dmod == 1 && xs) doff = (i / xs) * ws + i % xs;
        if (smod == 2) so = i & 3;
        if (dmod == 2) doff = i & 3;
        dma_byte_copy(sar + so, dar + doff);
    }
    (void)ys;
    if (sd_log) fprintf(stderr, "[DMA] ch%d mem %08x -> %08x %u bytes ccr=%x\n", ch, sar, dar, cnt, ccr);
    IOREG(base + 0x0C) = ccr & ~1u;
    IOREG(DMA + 0x04) |= 1u << ch;
    dma_irq();
}

/* Run an SD channel to completion at once: the card is always ready. */
static void dma_kick(int ch) {
    uint32_t base = DMA + 0x80 + ch * 0x40;
    uint32_t ccr = IOREG(base + 0x0C);
    if (!(ccr & 1) || !(IOREG(DMA) & 1)) return;
    uint32_t sar = IOREG(base), dar = IOREG(base + 4), cnt = IOREG(base + 8) & 0xFFFFFF;
    if (dar == SSI_STX_PA) { if (!ach[ch].active) audio_start(ch); return; }
    if (!(ccr & 8) && sar != SDHC_BUF_PA && dar != SDHC_BUF_PA) { dma_mem(ch); return; }
    int from_sd = sar == SDHC_BUF_PA, to_sd = dar == SDHC_BUF_PA;
    if (!from_sd && !to_sd) return;
    if (from_sd && sd.dir != 1) return;     /* data not there yet: the command will kick us */
    if (to_sd && sd.dir != 2) return;
    uint32_t done = 0;
    while (done < cnt) {
        uint32_t pa = (from_sd ? dar : sar) + done;
        uint8_t *page = bus_page(pa & ~0xFFFu, from_sd);
        if (!page) break;
        uint8_t *p = page + (pa & 0xFFF);
        uint8_t b;
        if (from_sd) { if (!data_get(&b)) break; *p = b; }
        else { if (!data_put(*p)) break; }
        done++;
    }
    if (from_sd) cpu_phys_written(dar, done);
    if (sd_log) fprintf(stderr, "[SD] DMA ch%d %s %u/%u bytes at %08x\n", ch, from_sd ? "read" : "write", done, cnt, from_sd ? dar : sar);
    IOREG(base + 0x0C) = ccr & ~1u;
    IOREG(DMA + 0x04) |= 1u << ch;
    dma_irq();
}

/* ---------------------------------------------------------------- audio (DMA -> SSI) */

/* Channels feeding the SSI transmit FIFO play PCM at the audio rate: each period of CNTR
 * bytes ends with the channel's interrupt (in repeat mode it starts over, from SAR as the
 * driver has set it by then). The samples are copied to the host audio ring. */
#define AUDIO_RING 32768                      /* stereo frames */
static int16_t audio_ring[AUDIO_RING * 2];
static uint32_t audio_w, audio_r;
uint32_t audio_rate = 44100;

static void audio_push(uint32_t pa, uint32_t len) {
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        uint32_t a = pa + i;
        uint8_t *page = bus_page(a & ~0xFFFu, 0);
        if (!page) return;
        const int16_t *f = (const int16_t *)(page + (a & 0xFFF));
        uint32_t w = __atomic_load_n(&audio_w, __ATOMIC_RELAXED), r = __atomic_load_n(&audio_r, __ATOMIC_ACQUIRE);
        if (w - r >= AUDIO_RING) return;                      /* host isn't draining: drop */
        uint32_t k = w % AUDIO_RING;
        audio_ring[k * 2] = f[0]; audio_ring[k * 2 + 1] = f[1];
        __atomic_store_n(&audio_w, w + 1, __ATOMIC_RELEASE);
    }
}

/* Host side: up to n stereo frames; returns how many were available. */
int zod_audio_read(int16_t *out, int n) {
    int got = 0;
    uint32_t r = __atomic_load_n(&audio_r, __ATOMIC_RELAXED), w = __atomic_load_n(&audio_w, __ATOMIC_ACQUIRE);
    while (got < n && r != w) {
        uint32_t k = r % AUDIO_RING;
        out[got * 2] = audio_ring[k * 2]; out[got * 2 + 1] = audio_ring[k * 2 + 1];
        r++; got++;
    }
    __atomic_store_n(&audio_r, r, __ATOMIC_RELEASE);
    return got;
}

static void audio_start(int ch) {
    uint32_t base = DMA + 0x80 + ch * 0x40;
    ach[ch].active = 1;
    ach[ch].src = IOREG(base);
    ach[ch].len = IOREG(base + 8) & 0xFFFFFF;
    ach[ch].acc = 0;
}

void audio_tick(int cycles) {
    if (!(IOREG(DMA) & 1)) return;
    for (int ch = 0; ch < 11; ch++) {
        uint32_t base = DMA + 0x80 + ch * 0x40, ccr = IOREG(base + 0x0C);
        if (!(ccr & 1) || IOREG(base + 4) != SSI_STX_PA) { ach[ch].active = 0; continue; }
        if (!ach[ch].active) audio_start(ch);
        /* bytes per CPU cycle = rate * 4 / cpu_hz, kept as a 32.32 accumulator */
        ach[ch].acc += (uint64_t)cycles * (((uint64_t)audio_rate * 4 << 32) / zod.cpu_hz);
        if ((ach[ch].acc >> 32) < ach[ch].len || !ach[ch].len) continue;
        if (sd_log) fprintf(stderr, "[AUDIO] ch%d period done src=%08x len=%u ccr=%x\n", ch, ach[ch].src, ach[ch].len, ccr);
        audio_push(ach[ch].src, ach[ch].len);
        IOREG(DMA + 0x04) |= 1u << ch;
        dma_irq();
        if (ccr & 4) audio_start(ch);            /* repeat */
        else { IOREG(base + 0x0C) = ccr & ~1u; ach[ch].active = 0; }
    }
}

static void dma_kick_all(void) {
    for (int ch = 0; ch < 11; ch++) dma_kick(ch);
}

/* ---------------------------------------------------------------- registers */

int sd_read(uint32_t off, uint32_t *v) {
    if ((off & ~0xFFFu) == SDHC) {
        switch (off & 0xFFF) {
        case 0x04: *v = sdhc_status(); return 1;
        case 0x20: *v = 0x0400; return 1;                       /* REV_NO */
        case 0x34: *v = sd.resp_pos < sd.resp_n ? sd.resp[sd.resp_pos++] : 0; return 1;
        case 0x38: {
            uint8_t a = 0, b = 0;
            data_get(&a); data_get(&b);
            *v = (uint32_t)a | (uint32_t)b << 8;
            return 1;
        }
        }
        return 0;
    }
    if ((off & ~0xFFFu) == DMA) {
        if ((off & 0xFFF) == 0x04) { *v = IOREG(off); return 1; }
    }
    return 0;
}

void sd_write(uint32_t off, uint32_t v, uint32_t old) {
    if ((off & ~0xFFFu) == SDHC) {
        switch (off & 0xFFF) {
        case 0x00:
            if (v & 8) { sd.status = 0; sd.dir = 0; sd.clk_run = 0; sd.cmd_pending = 0; }   /* reset */
            if (v & 1) sd.clk_run = 0;
            if (v & 2) {
                sd.clk_run = 1;
                if (sd.cmd_pending) { sd.cmd_pending = 0; run_command(); dma_kick_all(); }
            }
            sdhc_irq();
            break;
        case 0x04: sd.status &= ~v; sdhc_irq(); break;          /* write 1 to clear */
        case 0x24: sdhc_irq_pending = 0; sdhc_irq(); break;     /* INT_MASK: acknowledges */
        case 0x0C: sd.cmd_pending = 1; break;                   /* CMD_DAT_CONT: the command starts with the clock */
        case 0x38: data_put((uint8_t)v); data_put((uint8_t)(v >> 8)); break;
        }
        return;
    }
    if ((off & ~0xFFFu) == DMA) {
        uint32_t r = off & 0xFFF;
        if (r == 0x04) { IOREG(off) = old & ~v; dma_irq(); return; }   /* DISR: write 1 to clear */
        if (r == 0x08) { dma_irq(); return; }
        if (r >= 0x80 && (r - 0x80) % 0x40 == 0x0C && r < 0x80 + 11 * 0x40) dma_kick((int)((r - 0x80) / 0x40));
        if (r == 0x00) dma_kick_all();
    }
}

void sd_state(StateIO *s, uint32_t version) {
    int fd = card_fd; uint64_t size = card_size;
    st_bytes(s, &sd, sizeof sd);
    if (version >= 3) { st_bytes(s, ach, sizeof ach); ST(s, sdhc_irq_pending); }
    if (s->loading) { card_fd = fd; card_size = size; sd.inserted = fd >= 0; }
}

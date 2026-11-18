/* ZodiacDroid - memory map, ROM loading, frame loop */
#include "zodiac.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

Zodiac zod;

/* ---------------------------------------------------------------- bus */

uint8_t *bus_page(uint32_t pa, int write) {
    if (pa - SDRAM_BASE < SDRAM_SIZE) return zod.sdram + ((pa - SDRAM_BASE) & ~0xFFFu);
    if (!write) {
        if (pa - FLASH_BASE < FLASH_SIZE) return zod.flash + ((pa - FLASH_BASE) & ~0xFFFu);
        if (pa < 0x00100000u) return zod.flash + (pa & ~0xFFFu);   /* boot alias of CS0 */
    }
    if (pa - ESRAM_BASE < ESRAM_SIZE) return zod.esram + ((pa - ESRAM_BASE) & ~0xFFFu);
    return NULL;
}

static int unmapped_logs;

uint32_t bus_io_read(uint32_t pa, int size) {
    if (pa - IO_BASE < IO_SIZE) return io_read(pa, size);
    if (pa - FLASH_BASE < FLASH_SIZE || pa < 0x00100000u) {   /* writes to flash land here; reads are mapped */
        const uint8_t *p = zod.flash + (pa & (FLASH_SIZE - 1));
        return size == 4 ? *(uint32_t *)p : size == 2 ? *(uint16_t *)p : *p;
    }
    return io_read(pa, size);   /* external chips (W4200 etc.) are handled by io.c too */
}

void bus_io_write(uint32_t pa, uint32_t v, int size) {
    if (pa - FLASH_BASE < FLASH_SIZE || pa < 0x00100000u) {
        if (zod.log_io && unmapped_logs < 50) { unmapped_logs++; fprintf(stderr, "flash write %08x=%08x pc=%08x\n", pa, v, cpu.r[15]); }
        return;
    }
    io_write(pa, v, size);
}

/* ---------------------------------------------------------------- ROM loading */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static int gunzip(const uint8_t *in, size_t inlen, uint8_t *out, size_t outmax, size_t *outlen) {
    z_stream s; memset(&s, 0, sizeof s);
    if (inflateInit2(&s, 16 + MAX_WBITS) != Z_OK) return 0;
    s.next_in = (Bytef *)in; s.avail_in = (uInt)inlen;
    s.next_out = out; s.avail_out = (uInt)outmax;
    int r = inflate(&s, Z_FINISH);
    *outlen = s.total_out;
    inflateEnd(&s);
    return r == Z_STREAM_END;
}

int zod_load_rom(const uint8_t *data, size_t len, char *err, size_t errlen) {
    if (!zod.flash) {
        zod.flash = calloc(1, FLASH_SIZE);
        zod.sdram = calloc(1, SDRAM_SIZE);
        zod.esram = calloc(1, ESRAM_SIZE);
    }
    memset(zod.flash, 0xFF, FLASH_SIZE);
    /* Palm resource database containing a ROMZ resource (Tapwave ROM updater) */
    if (len > 78 && !memcmp(data + 60, "appl", 4) && (be16(data + 32) & 1)) {
        uint16_t n = be16(data + 76);
        for (int i = 0; i < n; i++) {
            const uint8_t *e = data + 78 + i * 10;
            if (78 + i * 10 + 10 > (long)len) break;
            if (!memcmp(e, "ROMZ", 4)) {
                uint32_t off = be32(e + 6);
                uint32_t end = (i + 1 < n) ? be32(e + 16) : (uint32_t)len;
                size_t out;
                if (off >= len || end > len || !gunzip(data + off, end - off, zod.flash, FLASH_SIZE, &out)) {
                    snprintf(err, errlen, "Couldn't unpack the ROM inside the update file.");
                    return 0;
                }
                zod.rom_size = (uint32_t)out;
                goto check;
            }
        }
        snprintf(err, errlen, "That Palm file has no ROM inside it.");
        return 0;
    }
    if (len > FLASH_SIZE) { snprintf(err, errlen, "ROM file is too big."); return 0; }
    memcpy(zod.flash, data, len);
    zod.rom_size = (uint32_t)len;
check:
    /* card header: branch at 0, 0xFEEDBEEF at 8 */
    if (*(uint32_t *)(zod.flash + 8) != 0xFEEDBEEF) {
        snprintf(err, errlen, "This doesn't look like a Zodiac ROM (no Palm card header).");
        return 0;
    }
    if (memcmp(zod.flash + 0x30, "Tapwave", 7)) {
        snprintf(err, errlen, "This is a Palm OS ROM, but not a Tapwave Zodiac one.");
        return 0;
    }
    return 1;
}

void zod_reset(void) {
    memset(zod.sdram, 0, SDRAM_SIZE);
    memset(zod.esram, 0, ESRAM_SIZE);
    zod.cycles = 0;
    zod.cpu_hz = 200000000;
    io_init();
    cpu_reset();
}

void zod_run_frame(void) {
    int slice = 2000;
    int total = zod.cpu_hz / 60;
    for (int done = 0; done < total; ) {
        int c = cpu_run(io_next_event(slice));
        zod.cycles += c;
        io_tick(c);
        done += c;
        if (cpu.dbg_hit) break;
    }
}

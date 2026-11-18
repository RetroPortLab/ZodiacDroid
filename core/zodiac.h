/* ZodiacDroid - Tapwave Zodiac system: memory map and i.MX1 (MC9328MX1) peripherals */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define FLASH_BASE   0x10000000u
#define FLASH_SIZE   0x01000000u          /* 16MB window, ROM image is 8MB */
#define SDRAM_BASE   0x08000000u
#define SDRAM_SIZE   0x08000000u          /* 128MB (Zodiac 2); CSD0 + CSD1 */
#define ESRAM_BASE   0x00300000u
#define ESRAM_SIZE   0x00020000u
#define IO_BASE      0x00200000u
#define IO_SIZE      0x00030000u

#define SCREEN_W 480
#define SCREEN_H 320

typedef struct {
    uint8_t *flash;       /* FLASH_SIZE */
    uint8_t *sdram;       /* SDRAM_SIZE */
    uint8_t *esram;
    uint32_t io[IO_SIZE / 4];
    uint32_t rom_size;

    uint64_t cycles;      /* CPU cycles since reset */
    uint32_t cpu_hz;
    int log_io;           /* print I/O accesses */
    int log_limit;
    uint32_t log_filter;  /* only log this 4K page (0 = all) */
    int log_uart, log_i2c, log_2d;
    void (*prof)(uint32_t pc);   /* debug: called with the PC after each CPU slice */
    int model;            /* 1 or 2 */
    uint16_t fb[SCREEN_W * SCREEN_H];   /* last frame, RGB565 */
    int frame_ready;
} Zodiac;

extern Zodiac zod;

/* load a ROM: raw 8MB image, or the Tapwave "zodiac_1.1_update.prc" (gzipped ROMZ resource). */
int  zod_load_rom(const uint8_t *data, size_t len, char *err, size_t errlen);
void zod_reset(void);
void zod_soft_reset(void);
void zod_set_cpu_mhz(int mhz);        /* 50..200; the Zodiac runs at 200 */           /* reset button: keeps the RAM (installed apps, data) */
void zod_run_frame(void);            /* one 1/60s slice */
void zod_touch(int x, int y, int down);   /* screen coordinates 0..479 x 0..319 */
/* buttons (active while the bit is set) */
enum {
    ZB_HOME = 1 << 0, ZB_FUNCTION = 1 << 1, ZB_POWER = 1 << 2,
    ZB_A = 1 << 3, ZB_B = 1 << 4, ZB_C = 1 << 5, ZB_D = 1 << 6,
    ZB_L = 1 << 7, ZB_R = 1 << 8, ZB_BOOKMARK = 1 << 9, ZB_EXTRA = 1 << 10,
};
void zod_buttons(uint32_t mask);
/* SD card: a disk image (raw, MBR + FAT) opened read/write; fd < 0 removes the card */
void sd_attach(int fd, uint64_t size);
/* audio: interleaved stereo 16-bit frames at zod_audio_rate Hz; returns frames copied */
int  zod_audio_read(int16_t *out, int n);     /* raw: whatever is queued */
int  zod_audio_stream(int16_t *out, int n);   /* for the audio device: always n frames, jitter-buffered */
extern volatile int zod_vibrate;   /* 1 while the OS has the vibration motor switched on */
extern uint32_t audio_rate;
/* save states (zlib-compressed); zod_save_state returns a malloc'd buffer */
size_t zod_save_state(uint8_t **out);
int    zod_load_state(const uint8_t *data, size_t len);  /* after zod_load_rom; 1 = ok */
void zod_stick(int x, int y);             /* analog controller, -32767..32767, right/down positive */

/* patch.c */
void patches_install(void (*debug_hook)(uint32_t pc));
void patch_arm(uint32_t va);

/* peripherals */
void io_init(void);
uint32_t io_read(uint32_t pa, int size);
void io_write(uint32_t pa, uint32_t v, int size);
void io_tick(int cycles);
int  io_next_event(int max);        /* cycles until the next timer event, capped at max */

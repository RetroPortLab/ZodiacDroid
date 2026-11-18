/* ZodiacDroid - ATI Imageon W4200 */
#pragma once
#include <stdint.h>

#define W42_IMEM_BASE 0x100000u
#define W42_IMEM_SIZE 0x0D0000u     /* internal memory window (host 0x30000-0xFFFFF); 384KB is SRAM, the DAL also uses the rest */
#define W42_EMEM_BASE 0x800000u
#define W42_EMEM_SIZE 0x800000u     /* 8MB external SDRAM */

typedef struct {
    uint32_t hif[0x4000];      /* host interface block */
    uint32_t reg[0x4000];      /* chip registers (byte offset 0..0xFFFF) */
    uint32_t aux[0x4000];      /* host 0x200000-0x20FFFF: peripheral blocks (SD host at 0x202000, ...) */
    uint8_t *imem, *emem;
} W4200;
extern W4200 w42;

void     w4200_init(void);
uint32_t w4200_read(uint32_t off, int size);
void     w4200_write(uint32_t off, uint32_t v, int size);
void     w4200_reg_write(uint32_t r, uint32_t v);
uint8_t *w4200_page(uint32_t pa);
uint8_t *w42_ptr(uint32_t chip_addr);
void     w4200_scanout(void);
void     w4200_vblank(void);

/* w2d.c: 2D engine */
void     w2d_init(void);
uint32_t w2d_read(uint32_t r, uint32_t v);
void     w2d_write(uint32_t r, uint32_t v);
int      w2d_packet3(uint32_t op, const uint32_t *d, uint32_t n);

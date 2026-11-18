#pragma once
#include <stdint.h>
typedef struct {
    uint32_t hif[0x4000];      /* host interface block */
    uint32_t reg[0x4000];      /* chip registers */
} W4200;
extern W4200 w42;
void     w4200_init(void);
uint32_t w4200_read(uint32_t off, int size);
void     w4200_write(uint32_t off, uint32_t v, int size);

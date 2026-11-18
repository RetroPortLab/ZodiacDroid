/* ZodiacDroid - internal interfaces between the peripheral modules */
#pragma once
#include <stdint.h>

void     irq_set(int src, int level);
void     io_log(const char *what, uint32_t pa, uint32_t v, int size);
uint32_t perclk1_hz(void);
uint32_t gpio_pins(int port);
void     gpio_set_input(int port, uint32_t mask, int level);
extern uint32_t gpio_in[4];

/* soc.c: other on-chip blocks (DMA, LCDC, SDHC, ASP, SSI, CSPI...) */
void soc_init(void);
int  soc_read(uint32_t off, uint32_t *v);   /* off from IO_BASE; returns 1 if handled */
void soc_write(uint32_t off, uint32_t v, uint32_t old);   /* called after the register file is updated */
void soc_tick(int cycles);

/* ext.c: devices on the external chip selects (ATI W4200 etc.) */
void     ext_init(void);
uint32_t ext_read(uint32_t pa, int size);
void     ext_write(uint32_t pa, uint32_t v, int size);
void     ext_tick(int cycles);

/* ZodiacDroid - devices on the external chip selects */
#include "zodiac.h"
#include "cpu.h"
#include "io.h"
#include "w4200.h"
#include <stdio.h>
#include <string.h>

static uint32_t scratch[0x40000];
static int logs;

void ext_init(void) { memset(scratch, 0, sizeof scratch); logs = 0; w4200_init(); }

uint32_t ext_read(uint32_t pa, int size) {
    if ((pa >> 24) == 0x16) return w4200_read(pa & 0xFFFFFF, size);
    uint32_t v = scratch[(pa >> 2) & 0x3FFFF];
    if (size == 2) v = (v >> ((pa & 2) * 8)) & 0xFFFF;
    else if (size == 1) v = (v >> ((pa & 3) * 8)) & 0xFF;
    io_log("R", pa, v, size);
    return v;
}

void ext_write(uint32_t pa, uint32_t v, int size) {
    if ((pa >> 24) == 0x16) { w4200_write(pa & 0xFFFFFF, v, size); return; }
    uint32_t *r = &scratch[(pa >> 2) & 0x3FFFF];
    if (size == 4) *r = v;
    else if (size == 2) { int s = (pa & 2) * 8; *r = (*r & ~(0xFFFFu << s)) | ((v & 0xFFFF) << s); }
    else { int s = (pa & 3) * 8; *r = (*r & ~(0xFFu << s)) | ((v & 0xFF) << s); }
    io_log("W", pa, v, size);
}

void ext_tick(int cycles) { (void)cycles; }

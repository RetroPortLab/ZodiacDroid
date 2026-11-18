/* CPU self-test: runs an ARM/Thumb program compiled for armv4t on the emulated CPU (MMU off)
 * and prints the hash it leaves at 0x8000; compare with the same program compiled for the host. */
#include "../../core/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint8_t mem[0x100000] __attribute__((aligned(4096)));
uint8_t *bus_page(uint32_t pa, int w) { return pa < sizeof mem ? mem + (pa & ~0xFFFu) : NULL; }
uint32_t bus_io_read(uint32_t pa, int s) { fprintf(stderr, "io read %08x\n", pa); return 0; }
void bus_io_write(uint32_t pa, uint32_t v, int s) { fprintf(stderr, "io write %08x=%08x\n", pa, v); }
#include "../../core/state.h"
void st_bytes(StateIO *s, void *p, size_t n) { (void)s; (void)p; (void)n; }
int main(int c, char **v) {
    FILE *f = fopen(v[1], "rb"); if (!f) return 1;
    fread(mem + 0x10000, 1, 0x80000, f); fclose(f);
    cpu_reset();
    /* reset vector: branch to 0x10000; SWI vector: loop */
    *(uint32_t *)mem = 0xEA000000 | (((0x10000 - 8) >> 2) & 0xFFFFFF);
    *(uint32_t *)(mem + 8) = 0xEAFFFFFE;
    cpu.r[13] = 0xF0000;
    for (int i = 0; i < 5000; i++) { cpu_run(100000); if (cpu.r[15] == 8) break; }
    printf("%08x  (cycles %llu)\n", *(uint32_t *)(mem + 0x8000), (unsigned long long)cpu.cycles);
    return 0;
}

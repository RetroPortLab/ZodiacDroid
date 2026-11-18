/* ZodiacDroid - targeted fix-ups applied while the ROM runs (the ROM image itself is never modified).
 * Each patch checks the instruction it expects before arming, so a different ROM is left alone. */
#include "zodiac.h"
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t va;          /* address of the instruction to intercept */
    uint32_t expect;      /* instruction word expected there (ROM sanity check) */
    void (*fn)(void);
    const char *what;
} Patch;

/* EEPROM device signature: the DAL compares a 128-byte block from the board EEPROM with a
 * value derived from the ROM's RSA key, and resets with "EEPROM/ROM mismatch" otherwise.
 * We have no per-device EEPROM, so report the comparison as equal. */
static void eeprom_sig_ok(void) { cpu.r[0] = 0; cpu.r[15] += 4; }

static const Patch patches[] = {
    { 0x200A3CCC, 0xEB0097B7, eeprom_sig_ok, "EEPROM signature check" },
};
#define NPATCH (sizeof patches / sizeof patches[0])

static uint8_t *bitmap;
static void (*user_hook)(uint32_t pc);

static void hook(uint32_t pc) {
    for (unsigned i = 0; i < NPATCH; i++) if (patches[i].va == pc) { patches[i].fn(); return; }
    if (user_hook) user_hook(pc);
}

void patch_arm(uint32_t va) {
    if (!bitmap) return;
    uint32_t bi = (va - 0x20000000u) >> 1;
    bitmap[bi >> 3] |= 1 << (bi & 7);
}

void patches_install(void (*debug_hook)(uint32_t)) {
    if (!bitmap) bitmap = calloc(1, 0x20000000 / 16);
    else memset(bitmap, 0, 0x20000000 / 16);
    user_hook = debug_hook;
    cpu.hle_bitmap = bitmap;
    cpu.hle_hook = hook;
    for (unsigned i = 0; i < NPATCH; i++) {
        uint32_t off = patches[i].va - 0x20000000u;   /* ROM is linked at 0x20000000 */
        if (off + 4 > zod.rom_size) continue;
        uint32_t w = *(uint32_t *)(zod.flash + off);
        if (w != patches[i].expect) {
            fprintf(stderr, "patch '%s' not applied: ROM differs\n", patches[i].what);
            continue;
        }
        patch_arm(patches[i].va);
    }
}

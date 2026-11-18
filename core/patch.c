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

/* The ROM's Pen Input Manager is a stub that raises "PINUIDummy.c, Line:25, Unimplemented".
 * Make the stub return 0 instead (Thumb: return to lr). */
static void pins_stub(void) { cpu.r[0] = 0; cpu.r[15] = cpu.r[14] & ~1u; if (!(cpu.r[14] & 1)) cpu.cpsr &= ~0x20u; }

/* HALDelay(us): the DAL busy-waits on GPT1 (1MHz free-running). Instead of spinning
 * through millions of instructions, account the same amount of CPU time and return. */
static void hal_delay(void) {
    uint32_t us = cpu.r[0] ? cpu.r[0] : 1;
    cpu.budget -= (int64_t)us * (zod.cpu_hz / 1000000);
    cpu.r[15] = cpu.r[14] & ~1u; if (!(cpu.r[14] & 1)) cpu.cpsr &= ~0x20u;
}

/* Database signature check (DAL "Signature Check"): Tapwave signs its system extensions
 * ("Tapwave Application Layer" etc). Verification currently fails under emulation, and
 * UIAppShell then retries forever without loading them. Report success. */
static void sig_ok(void) { cpu.r[0] = 0; cpu.r[10] = 0; }   /* continue at the orrs: result 0 = verified */

/* Battery level: the DAL samples the battery through a gauge we don't emulate, then
 * scales it by factory calibration data. Report a full battery (the result is clamped
 * to 50..950 by the DAL itself). */
static void battery_level(void) { cpu.r[0] = 900; cpu.r[15] = cpu.r[14] & ~1u; if (!(cpu.r[14] & 1)) cpu.cpsr &= ~0x20u; }

static const Patch patches[] = {
    { 0x200A4524, 0xE19A0000, sig_ok, "database signature check" },
    { 0x20084912, 0x2800491D, hal_delay, "HALDelay" },
    { 0x204B2BEC, 0xA210B580, pins_stub, "Pen Input Manager stub (UI)" },
    { 0x20055E28, 0xA2A6B580, pins_stub, "Pen Input Manager stub (system)" },
    { 0x200A3CCC, 0xEB0097B7, eeprom_sig_ok, "EEPROM signature check" },
    { 0x20088658, 0xAB00B5FE, battery_level, "battery level" },
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
    if (va - 0x20000000u + 2 > cpu.hle_span) cpu.hle_span = va - 0x20000000u + 2;
}

/* Byte-level fix-ups to 68k application code stored in the ROM. Each one is applied only if
 * the expected bytes are present. */
typedef struct { const char *find, *repl, *what; } BytePatch;
static const BytePatch byte_patches[] = {
    /* Digitizer calibration (Calibration panel): the emulated digitizer already reports
     * exact screen positions, which the uncalibrated pen path maps 1:1. The panel's
     * PenCalibrate call would replace that with a mapping derived from the device's factory
     * EEPROM data, which we do not have. Skip the call (report success) so the 1:1 mapping
     * stays and the panel's own confirmation tap check passes. */
    { "2f2effbe486effb22f2eff92486effa64e4fa2713e00", "2f2effbe486effb22f2eff92486effa670004e713e00", "keep 1:1 digitizer calibration" },
};

static int unhex(const char *h, uint8_t *o) {
    int n = 0;
    while (h[0] && h[1]) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2; }
    return n;
}

static void apply_byte_patches(void) {
    for (unsigned i = 0; i < sizeof byte_patches / sizeof byte_patches[0]; i++) {
        uint8_t f[64], r[64];
        int n = unhex(byte_patches[i].find, f);
        unhex(byte_patches[i].repl, r);
        int hits = 0;
        for (uint32_t o = 0; o + n <= zod.rom_size; o += 2)
            if (zod.flash[o] == f[0] && !memcmp(zod.flash + o, f, n)) { memcpy(zod.flash + o, r, n); hits++; }
        int already = 0;
        for (uint32_t o = 0; !hits && o + n <= zod.rom_size; o += 2) if (!memcmp(zod.flash + o, r, n)) { already = 1; break; }
        if (!hits && !already) fprintf(stderr, "patch '%s' not applied: ROM differs\n", byte_patches[i].what);
    }
}

void patches_install(void (*debug_hook)(uint32_t)) {
    apply_byte_patches();
    if (!bitmap) bitmap = calloc(1, 0x20000000 / 16);
    else memset(bitmap, 0, 0x20000000 / 16);
    user_hook = debug_hook;
    cpu.hle_bitmap = bitmap;
    cpu.hle_span = 0;
    cpu.hle_hook = hook;
    for (unsigned i = 0; i < NPATCH; i++) {
        uint32_t off = patches[i].va - 0x20000000u;   /* ROM is linked at 0x20000000 */
        if (off + 4 > zod.rom_size) continue;
        uint32_t w = (off & 2) ? (uint32_t)*(uint16_t *)(zod.flash + off) | ((uint32_t)*(uint16_t *)(zod.flash + off + 2) << 16)
                               : *(uint32_t *)(zod.flash + off);
        if (w != patches[i].expect) {
            fprintf(stderr, "patch '%s' not applied: ROM differs\n", patches[i].what);
            continue;
        }
        patch_arm(patches[i].va);
    }
}

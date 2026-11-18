#include <fcntl.h>
#include <unistd.h>
/* ZodiacDroid headless test runner:
 *   runzod rom|update.prc [frames] [--io N] [--trace N] [--shot out.ppm] */
#include "../core/io.h"
#include "../core/zodiac.h"
#include "../core/cpu.h"
#include "../core/w4200.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len); if (fread(b, 1, *len, f) != *len) { fclose(f); return NULL; }
    fclose(f); return b;
}

static uint32_t hooks[2048]; static int nhooks; static int hook_hits; static uint32_t rearm;
static void rdstr(uint32_t va, char *out, int max) {
    int i = 0; uint8_t c;
    while (i < max - 1 && cpu_vread8(va + i, &c) && c >= 9 && c < 127) out[i++] = c;
    out[i] = 0;
}
static void hook(uint32_t pc) {
    for (int i = 0; i < nhooks; i++) if (hooks[i] == pc) {
        char s0[80], s1[80], s2[80];
        rdstr(cpu.r[0], s0, 80); rdstr(cpu.r[1], s1, 80); rdstr(cpu.r[2], s2, 80);
        if (getenv("HOOKMEM")) { uint32_t a = cpu.r[strtoul(getenv("HOOKMEM"), 0, 10)], v0 = 0, v1 = 0; cpu_vread32(a, &v0); cpu_vread32(a + 4, &v1); fprintf(stderr, "  mem[r%s=%08x] = %08x %08x\n", getenv("HOOKMEM"), a, v0, v1); }
        if (getenv("HOOKDUMP")) { uint32_t a = cpu.r[atoi(getenv("HOOKDUMP"))]; fprintf(stderr, "  dump %08x:", a); for (int k = 0; k < 8; k++) { uint32_t w = 0; cpu_vread32(a + 4 * k, &w); fprintf(stderr, " %08x", w); } fprintf(stderr, "\n"); }
        if (getenv("TRACEHOOK") && hook_hits == atoi(getenv("TRACEHOOK"))) cpu.trace_n = getenv("TRACELEN") ? atoi(getenv("TRACELEN")) : 80;
        if (hook_hits++ < (getenv("HOOKMAX") ? atoi(getenv("HOOKMAX")) : 400))
        {
            if (getenv("STACK") && hook_hits < (getenv("HOOKMAX") ? atoi(getenv("HOOKMAX")) : 400)) {
                fprintf(stderr, "  stack:");
                for (int k = 0; k < 64; k++) { uint32_t v; if (cpu_vread32(cpu.r[13] + k * 4, &v) && v >= 0x20000000 && v < 0x20800000) fprintf(stderr, " %08x", v); }
                fprintf(stderr, "\n");
            }
        }
            fprintf(stderr, "HOOK %08x r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x  \"%s\" \"%s\" \"%s\"\n", pc, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[14], s0, s1, s2);
    }
}
static uint32_t prof_pc[1 << 16], prof_n[1 << 16];
static void prof(uint32_t pc) {
    uint32_t h = (pc >> 2) & 0xFFFF;
    if (prof_pc[h] != pc) { if (prof_n[h]) return; prof_pc[h] = pc; }
    prof_n[h]++;
}
static void prof_dump(void) {
    for (int k = 0; k < 40; k++) {
        uint32_t best = 0, bi = 0;
        for (int i = 0; i < 1 << 16; i++) if (prof_n[i] > best) { best = prof_n[i]; bi = i; }
        if (!best) break;
        fprintf(stderr, "PROF %08x %u\n", prof_pc[bi], best); prof_n[bi] = 0;
    }
}
static void shot(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6 %d %d 255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint16_t p = zod.fb[i];
        uint8_t rgb[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2, (p & 31) << 3 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: runzod rom [frames] [--io N] [--trace N] [--shot out.ppm]\n"); return 1; }
    int frames = 60; const char *shotp = NULL, *savep = NULL, *loadp = NULL, *cardp = NULL; uint32_t trace = 0, dumpa = 0, dumpn = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--io") && i + 1 < argc) { zod.log_io = 1; zod.log_limit = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--filter") && i + 1 < argc) zod.log_filter = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--uart")) zod.log_uart = 1;
        else if (!strcmp(argv[i], "--i2c")) zod.log_i2c = 1;
        else if (!strcmp(argv[i], "--2d")) zod.log_2d = 1;
        else if (!strcmp(argv[i], "--2d2")) zod.log_2d = 2;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotp = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) savep = argv[++i];
        else if (!strcmp(argv[i], "--load") && i + 1 < argc) loadp = argv[++i];
        else if (!strcmp(argv[i], "--card") && i + 1 < argc) cardp = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 2 < argc) { dumpa = strtoul(argv[++i], 0, 0); dumpn = strtoul(argv[++i], 0, 0); }
        else frames = atoi(argv[i]);
    }
    size_t len; uint8_t *d = slurp(argv[1], &len);
    if (!d) { perror(argv[1]); return 1; }
    char err[256];
    if (!zod_load_rom(d, len, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    zod.model = 2;
    zod_reset();
    cpu.trace_n = trace;
    if (loadp) {
        size_t sl; uint8_t *sd = slurp(loadp, &sl);
        if (!sd || !zod_load_state(sd, sl)) { fprintf(stderr, "cannot load state %s\n", loadp); return 1; }
        free(sd);
    }
    if (getenv("MHZ")) zod_set_cpu_mhz(atoi(getenv("MHZ")));
#if defined(__aarch64__)
    { extern void cpu_jit_debug_signals(void); if (getenv("JITSIG")) cpu_jit_debug_signals(); }
#endif
    { extern int sd_log; sd_log = getenv("SDLOG") != NULL; }
    if (cardp) {
        int fd = open(cardp, O_RDWR);
        if (fd < 0) { perror(cardp); return 1; }
        sd_attach(fd, (uint64_t)lseek(fd, 0, SEEK_END));
    }
    if (getenv("HOOKS")) {
        patches_install(hook);
        char *p = getenv("HOOKS");
        while (*p && nhooks < 2048) {
            uint32_t a = strtoul(p, &p, 16) & ~1u; hooks[nhooks++] = a;
            patch_arm(a);
            if (*p == ',') p++; else break;
        }
    }
    { extern int w2d_break_at; if (getenv("BRK2D")) w2d_break_at = atoi(getenv("BRK2D")); }
    if (getenv("BRK") && !getenv("BRKFROM")) { sscanf(getenv("BRK"), "%x-%x", &cpu.dbg_lo, &cpu.dbg_hi); if (getenv("BRKSKIP")) cpu.dbg_skip = atoi(getenv("BRKSKIP")); }
    /* TAPS="frame:x,y;frame:x,y" taps the screen (held for 6 frames) */
    int tapf[32], tapx[32], tapy[32], ntap = 0;
    if (getenv("TAPS")) { const char *p = getenv("TAPS"); while (ntap < 32 && sscanf(p, "%d:%d,%d", &tapf[ntap], &tapx[ntap], &tapy[ntap]) == 3) { ntap++; p = strchr(p, ';'); if (!p) break; p++; } }
    FILE *wav = getenv("WAV") ? fopen(getenv("WAV"), "wb") : NULL;
    uint32_t wav_frames = 0;
    if (wav) { uint8_t hdr[44] = { 0 }; fwrite(hdr, 1, 44, wav); }
    int iofrom = getenv("IOFROM") ? atoi(getenv("IOFROM")) : -1;
    if (iofrom > 0) zod.log_io = 0;
    for (int f = 0; f < frames && !cpu.dbg_hit; f++) {
        { const char *sr = getenv("SOFTRESET"); if (sr && f == atoi(sr)) { zod_soft_reset(); if (nhooks) { patches_install(hook); for (int h = 0; h < nhooks; h++) patch_arm(hooks[h]); } } }
        if (f == iofrom) { zod.log_io = !getenv("LOG2D"); if (getenv("LOG2D")) zod.log_2d = atoi(getenv("LOG2D")); }
        if (getenv("PROF") && f == atoi(getenv("PROF"))) zod.prof = prof;
        /* REMOUNT=frame ejects the card and puts it back 90 frames later (rescan for new files) */
        if (cardp && getenv("REMOUNT")) {
            int rf = atoi(getenv("REMOUNT"));
            if (f == rf) sd_attach(-1, 0);
            if (f == rf + 90) { int fd2 = open(cardp, O_RDWR); sd_attach(fd2, (uint64_t)lseek(fd2, 0, SEEK_END)); }
        }
        /* MHZSEQ="frame:mhz;..." changes the CPU clock while running */
        if (getenv("MHZSEQ")) { const char *p = getenv("MHZSEQ"); int mf, mm; while (sscanf(p, "%d:%d", &mf, &mm) == 2) { if (f == mf) zod_set_cpu_mhz(mm); p = strchr(p, ';'); if (!p) break; p++; } }
        if (getenv("BRKSP")) sscanf(getenv("BRKSP"), "%x-%x", &cpu.dbg_sp_lo, &cpu.dbg_sp_hi);
        if (getenv("BRKFROM") && f == atoi(getenv("BRKFROM"))) { sscanf(getenv("BRK"), "%x-%x", &cpu.dbg_lo, &cpu.dbg_hi); if (getenv("BRKSKIP")) cpu.dbg_skip = atoi(getenv("BRKSKIP")); }
        /* STICK="frame:x,y:len;..." deflects the analog controller (-32767..32767) */
        if (getenv("STICK")) {
            const char *p = getenv("STICK"); int sf, sx, sy, sl;
            while (sscanf(p, "%d:%d,%d:%d", &sf, &sx, &sy, &sl) == 4) {
                if (f == sf) zod_stick(sx, sy);
                if (f == sf + sl) zod_stick(0, 0);
                p = strchr(p, ';'); if (!p) break; p++;
            }
        }
        /* PINS="frame:port.pin:len;..." holds a GPIO input low (button press) */
        if (getenv("PINS")) {
            const char *p = getenv("PINS"); int pf, pp, pn, pl;
            while (sscanf(p, "%d:%d.%d:%d", &pf, &pp, &pn, &pl) == 4) {
                if (f == pf) gpio_set_input(pp, 1u << pn, 0);
                if (f == pf + pl) gpio_set_input(pp, 1u << pn, 1);
                p = strchr(p, ';'); if (!p) break; p++;
            }
        }
        for (int t = 0; t < ntap; t++) {
            if (f == tapf[t]) zod_touch(tapx[t], tapy[t], 1);
            if (f == tapf[t] + (getenv("TAPLEN") ? atoi(getenv("TAPLEN")) : 6)) zod_touch(tapx[t], tapy[t], 0);
        }
        zod_run_frame();
        if (wav) { int16_t buf[4096 * 2]; int n; while ((n = zod_audio_read(buf, 4096)) > 0) { fwrite(buf, 4, (size_t)n, wav); wav_frames += (uint32_t)n; } }
        if (getenv("FRAMEMARK")) fprintf(stderr, "FRAME %d\n", f);
        if (getenv("SHOTEVERY") && f % atoi(getenv("SHOTEVERY")) == 0) { char pth[256]; snprintf(pth, sizeof pth, "%s/f_%05d.ppm", getenv("SHOTDIR") ? getenv("SHOTDIR") : "/tmp", f); shot(pth); }
    }
    if (cpu.dbg_hit) fprintf(stderr, "BREAK at %08x\n", cpu.r[15]);
    fprintf(stderr, "pc=%08x cpsr=%08x cycles=%llu halted=%d\n", cpu.r[15], cpu_get_cpsr(), (unsigned long long)cpu.cycles, cpu.halted);
    fprintf(stderr, "far=%08x dfsr=%08x ifsr=%08x lr_abt=%08x spsr=%08x\n", cpu.far, cpu.dfsr, cpu.ifsr, cpu.r[14], cpu.spsr);
    for (int i = 0; i < 16; i++) fprintf(stderr, "r%-2d=%08x%s", i, cpu.r[i], (i & 3) == 3 ? "\n" : "  ");
    for (uint32_t i = 0; i < dumpn; i++) {
        uint32_t v = 0;
        if (getenv("VDUMP")) cpu_vread32(dumpa + i * 4, &v);
        else { uint8_t *pg = bus_page(dumpa + i * 4, 0); v = pg ? *(uint32_t *)(pg + ((dumpa + i * 4) & 0xFFF)) : 0; }
        if (i & 7) fprintf(stderr, " %08x", v);
        else fprintf(stderr, "%s%08x: %08x", i ? "\n" : "", dumpa + i * 4, v);
    }
    if (dumpn) fprintf(stderr, "\n");
#if defined(__aarch64__)
    { extern void cpu_jit_stats(void); cpu_jit_stats(); }
#endif
    if (getenv("RING")) { int n = atoi(getenv("RING")); for (int i = n; i > 0; i--) fprintf(stderr, "%08x\n", cpu.ring[(cpu.ring_pos - i) & 65535]); }
    if (getenv("IOREGS")) { uint32_t a, b; sscanf(getenv("IOREGS"), "%x-%x", &a, &b);
        for (uint32_t r = a; r < b; r += 4) fprintf(stderr, "%08x: %08x%s", r, zod.io[(r - IO_BASE) >> 2], ((r - a) & 0xC) == 0xC ? "\n" : "  "); }
    { extern int w2d_ops; fprintf(stderr, "2D ops: %d\n", w2d_ops); }
    if (getenv("IRQS")) fprintf(stderr, "INTENABLE H=%08x L=%08x  INTSRC H=%08x L=%08x  FIQ type H=%08x L=%08x\n",
        io_read(0x223010, 4), io_read(0x223014, 4), io_read(0x223048, 4), io_read(0x22304c, 4), io_read(0x223018, 4), io_read(0x22301c, 4));
    if (getenv("W42REGS")) { uint32_t a, b; sscanf(getenv("W42REGS"), "%x-%x", &a, &b);
        for (uint32_t r = a; r < b; r += 4) fprintf(stderr, "%04x: %08x%s", r, w42.reg[r >> 2], ((r - a) & 0xC) == 0xC ? "\n" : "  "); }
    if (getenv("W42MEM")) { FILE *f = fopen(getenv("W42MEM"), "wb"); fwrite(w42.imem, 1, W42_IMEM_SIZE, f); fwrite(w42.emem, 1, W42_EMEM_SIZE, f); fclose(f); }
    if (wav) {
        uint32_t rate = audio_rate, bytes = wav_frames * 4;
        uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
        uint32_t v; v = bytes + 36; memcpy(h + 4, &v, 4); memcpy(h + 24, &rate, 4); v = rate * 4; memcpy(h + 28, &v, 4); memcpy(h + 40, &bytes, 4);
        fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav); fclose(wav);
        fprintf(stderr, "wav: %u frames\n", wav_frames);
    }
    if (zod.prof) prof_dump();
    if (savep) {
        uint8_t *sd; size_t sl = zod_save_state(&sd);
        FILE *f = sl ? fopen(savep, "wb") : NULL;
        if (f) { fwrite(sd, 1, sl, f); fclose(f); fprintf(stderr, "saved state: %zu bytes\n", sl); free(sd); }
        else fprintf(stderr, "save failed\n");
    }
    if (getenv("RAMDUMP")) { FILE *f = fopen(getenv("RAMDUMP"), "wb"); fwrite(zod.sdram, 1, SDRAM_SIZE, f); fclose(f); }
    if (shotp) shot(shotp);
    return 0;
}

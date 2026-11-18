/* Recompiler fuzzer: runs random ARM/Thumb instruction sequences on the interpreter and on
 * the recompiler from the same state and compares registers, flags and memory.
 * Build for arm64 and run under qemu-aarch64:  fuzz [cases] [seed] [thumb%] */
#include "../../core/cpu.h"
#include "../../core/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM 0x40000
static uint8_t mem[MEM] __attribute__((aligned(4096)));
static uint8_t init[MEM], resa[MEM];
uint8_t *bus_page(uint32_t pa, int w) { return pa < MEM ? mem + (pa & ~0xFFFu) : NULL; }
uint32_t bus_io_read(uint32_t pa, int s) { return pa * 0x9E3779B1u; }
void bus_io_write(uint32_t pa, uint32_t v, int s) { }
void st_bytes(StateIO *s, void *p, size_t n) { (void)s; (void)p; (void)n; }

static uint64_t rs = 88172645463325252ull;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }

static uint32_t gen_arm(void) {
    uint32_t c = (rnd() % 4) ? 0xE : rnd() % 15;
    uint32_t i;
    switch (rnd() % 10) {
    case 0: case 1: case 2: i = rnd() & 0x03FFFFFF; i &= ~(1u << 25 & 0); if (!(i & (1 << 25)) && (i & 0x90) == 0x90) i &= ~0x80; break; /* data processing */
    case 3: i = (rnd() & 0x00EFFF0F) | 0x00000090; if (rnd() & 1) i &= ~0x00800000; break;           /* multiply */
    case 4: i = 0x04000000 | (rnd() & 0x03FFFFFF); i &= ~(1u << 25) | ((rnd() & 1) << 25); if (i & (1 << 25)) i &= ~0x10; break; /* ldr/str */
    case 5: i = (rnd() & 0x01FFFF0F) | 0x90 | ((1 + rnd() % 3) << 5); break;                          /* halfword */
    case 6: i = 0x08000000 | (rnd() & 0x01BFFFFF); if (!(i & 0xFFFF)) i |= 1; break;                   /* ldm/stm */
    case 7: i = 0x0A000000 | (rnd() & 0x01000000) | (rnd() % 8); break;                               /* short branch fwd */
    case 8: i = (rnd() & 1) ? 0x010F0000 | (rnd() % 15) << 12 | (rnd() & (1 << 22)) : 0x012FFF10 | (rnd() % 16) | (rnd() & 0x20); break; /* mrs / bx */
    default: i = 0x03A00000 | (rnd() & 0x1FFFFF); break;                                               /* mov imm */
    }
    /* stores relative to pc would modify the code just ahead (the hardware pipeline and the
     * recompiler both run the old instruction there; the interpreter does not) */
    /* keep control flow inside the generated code unless FREEPC is set */
    if (!getenv("FREEPC")) {
        if ((i >> 26) == 0 && ((i >> 12) & 15) == 15) i &= ~(1u << 12);                  /* dp/ldrh to pc */
        if ((i >> 26) == 1 && (i & (1 << 20)) && ((i >> 12) & 15) == 15) i &= ~(1u << 12); /* ldr pc */
        if ((i >> 25) == 4 && (i & (1 << 20))) i &= ~0x8000u;                             /* ldm pc */
        if ((i & 0x0FFFFFD0) == 0x012FFF10) i = 0x010F0000 | ((i & 15) << 12);            /* bx -> mrs */
        if ((i & 0x0FFFFFF0) == 0x016F0F10) { }
    }
    if (((i >> 16) & 15) == 15 && !(i & (1 << 20)) && ((i >> 26) == 1 || (i >> 25) == 4 || ((i >> 25) == 0 && (i & 0x90) == 0x90))) i &= ~(1u << 16);
    return (c << 28) | i;
}

static uint16_t gen_thumb(void) {
    for (;;) {
        uint16_t i = rnd();
        uint32_t t = i >> 11;
        if (t == 0x1F || t == 0x1E || t == 0x1D) continue;          /* BL pairs: generated below */
        if (t == 0x1C) i = 0xE000 | (rnd() % 8);                    /* short forward branch */
        if (t == 0x1A || t == 0x1B) i = (i & 0xFF00) | (rnd() % 8);
        if ((i & 0xFF00) == 0xDF00 || (i & 0xFF00) == 0xBE00) continue;
        if (!getenv("FREEPC")) {
            if ((i & 0xFC00) == 0x4400 && (((i >> 8) & 3) == 3 || ((i & 0x87) == 0x87 && ((i >> 8) & 3) != 1))) continue;  /* bx / pc writes */
            if ((i & 0xFF00) == 0xBD00) continue;                                                 /* pop pc */
        }
        return i;
    }
}

static void snapshot(Cpu *o) { *o = cpu; }

static int compare(Cpu *a, Cpu *b, uint8_t *ma) {
    int bad = 0;
    for (int k = 0; k < 16; k++) if (a->r[k] != b->r[k]) { printf("  r%d %08x vs %08x\n", k, a->r[k], b->r[k]); bad = 1; }
    if (a->n != b->n || a->z != b->z || a->c != b->c || a->v != b->v) { printf("  flags %d%d%d%d vs %d%d%d%d\n", a->n, a->z, a->c, a->v, b->n, b->z, b->c, b->v); bad = 1; }
    if (a->cpsr != b->cpsr) { printf("  cpsr %x vs %x\n", a->cpsr, b->cpsr); bad = 1; }
    if (a->spsr != b->spsr) { printf("  spsr %x vs %x\n", a->spsr, b->spsr); bad = 1; }
    if (memcmp(a->bank_r13, b->bank_r13, sizeof a->bank_r13) || memcmp(a->bank_r14, b->bank_r14, sizeof a->bank_r14)) { printf("  banks differ\n"); bad = 1; }
    if (memcmp(ma, mem, MEM)) {
        for (int k = 0; k < MEM; k++) if (ma[k] != mem[k]) { printf("  mem %05x %02x vs %02x\n", k, ma[k], mem[k]); bad = 1; break; }
    }
    return bad;
}

int main(int argc, char **argv) {
    int cases = argc > 1 ? atoi(argv[1]) : 1000;
    if (argc > 2) rs ^= strtoull(argv[2], 0, 0) * 0x9E3779B97F4A7C15ull;
    int thumbpct = argc > 3 ? atoi(argv[3]) : 50;
    cpu_reset();
    if (!cpu_jit_available()) { printf("no JIT here\n"); return 1; }
    int fails = 0, skipped = 0;
    for (int n = 0; n < cases; n++) {
        int thumb = (int)(rnd() % 100) < thumbpct;
        /* memory: vectors are "b ." loops, code at 0x1000, data elsewhere random */
        for (int k = 0; k < MEM; k += 4) *(uint32_t *)(init + k) = rnd();
        for (int k = 0; k < 0x20; k += 4) *(uint32_t *)(init + k) = 0xEAFFFFFE;
        uint32_t len = 4 + rnd() % 40, a = 0x1000;
        for (uint32_t k = 0; k < len; k++) {
            if (thumb) {
                if (rnd() % 16 == 0) {                               /* BL pair a short way forward */
                    *(uint16_t *)(init + a) = 0xF000; *(uint16_t *)(init + a + 2) = 0xF800 | (rnd() % 8); a += 4; continue;
                }
                *(uint16_t *)(init + a) = gen_thumb(); a += 2;
            } else { *(uint32_t *)(init + a) = gen_arm(); a += 4; }
        }
        for (int k = 0; k < 64; k += 4) *(uint32_t *)(init + a + k) = thumb ? 0xE7FEE7FE : 0xEAFFFFFE;
        /* state */
        Cpu st;
        cpu_reset();
        uint32_t modes[] = { M_SVC, M_USR, M_SYS, M_IRQ, M_FIQ };
        cpu_set_cpsr((rnd() & 0xF0000000) | modes[rnd() % 5] | 0xC0 | (thumb ? 0x20 : 0));
        for (int k = 0; k < 15; k++) cpu.r[k] = (rnd() % 4) ? 0x8000 + (rnd() % 0x30000) : rnd();
        for (int k = 0; k < 6; k++) { cpu.bank_r13[k] = 0x20000 + rnd() % 0x10000; cpu.bank_r14[k] = rnd(); cpu.bank_spsr[k] = rnd() & 0xF00000FF; }
        cpu.spsr = (rnd() & 0xF0000000) | 0x1F;
        cpu.r[13] = 0x20000 + rnd() % 0x10000;
        cpu.r[15] = 0x1000;
        snapshot(&st);
        if (getenv("ONLY") && n != atoi(getenv("ONLY"))) continue;
        if (getenv("ONLY")) {
            for (int pass = 0; pass < 2; pass++) {
                memcpy(mem, init, MEM); cpu = st; cpu_tlb_flush(); cpu_jit = pass; cpu_code_changed();
                printf("%s\n", pass ? "jit" : "interp");
                for (int k = 0; k < 12; k++) {
                    printf("  pc=%08x r14=%08x r15m=%08x mem1000=%08x cpsr=%x\n", cpu.r[15], cpu.r[14], cpu.r[15], *(uint32_t *)(mem + 0x1000), cpu.cpsr);
                    cpu_run(1);
                }
            }
        }
        /* interpreter */
        memcpy(mem, init, MEM);
        cpu_jit = 0;
        cpu_run(5000);
        Cpu ra; snapshot(&ra); memcpy(resa, mem, MEM);
        if (getenv("VERBOSE")) fprintf(stderr, "case %d\n", n);
        /* recompiler */
        memcpy(mem, init, MEM);
        cpu_code_changed();
        cpu = st;
        cpu_tlb_flush();
        cpu_jit = 1;
        cpu_run(5000);
        /* only compare runs that ended spinning on a known loop (vector or terminator) */
        int spin = (ra.r[15] < 0x20 || (ra.r[15] >= a && ra.r[15] < a + 64)) && ra.r[15] + 4 <= MEM
                   && ((ra.cpsr & 0x20) ? *(uint16_t *)(resa + ra.r[15]) == 0xE7FE : *(uint32_t *)(resa + ra.r[15]) == 0xEAFFFFFE);
        if (memcmp(resa, init, 0x20) || memcmp(resa + 0x1000, init + 0x1000, a + 64 - 0x1000)) spin = 0;   /* code was overwritten */
        if (!spin) { skipped++; continue; }
        Cpu rj = cpu; static uint8_t resj[MEM]; memcpy(resj, mem, MEM);
        int differs = compare(&ra, &rj, resa);
        if (differs) {   /* executed code that was later overwritten (self-modifying)? then it's not comparable */
            memcpy(mem, init, MEM); cpu = st; cpu_tlb_flush(); cpu_jit = 0; cpu_code_changed();
            uint64_t c0 = cpu.cycles; int smc = 0;
            static uint32_t pcs[8000]; int np = 0;
            while (cpu.cycles - c0 < 5000 && np < 8000) { pcs[np++] = cpu.r[15]; cpu_run(1); }
            for (int k = 0; k < np; k++) if (pcs[k] + 4 <= MEM && memcmp(init + pcs[k], resa + pcs[k], 4)) smc = 1;
            memcpy(mem, resj, MEM); cpu = rj;
            if (smc) { skipped++; continue; }
        }
        if (differs) {
            fails++;
            printf("case %d (%s) failed, code:\n", n, thumb ? "thumb" : "arm");
            for (uint32_t k = 0x1000; k < a; k += thumb ? 2 : 4) printf("    %05x: %0*x\n", k, thumb ? 4 : 8, thumb ? *(uint16_t *)(init + k) : *(uint32_t *)(init + k));
            printf("  start: cpsr=%08x", (st.n << 31) | (st.z << 30) | (st.c << 29) | (st.v << 28) | st.cpsr);
            for (int k = 0; k < 16; k++) printf(" r%d=%08x", k, st.r[k]);
            printf("\n");
            if (getenv("TRACE")) {
                memcpy(mem, init, MEM); cpu = st; cpu_tlb_flush(); cpu_jit = 0; cpu_code_changed();
                printf("  interp:");
                for (int k = 0; k < 80; k++) { printf(" %x", cpu.r[15]); cpu_run(1); }
                printf("\n  jit:   ");
                memcpy(mem, init, MEM); cpu = st; cpu_tlb_flush(); cpu_jit = 1; cpu_code_changed();
                for (int k = 0; k < 40; k++) { printf(" %x", cpu.r[15]); cpu_run(1); }
                printf("\n");
            }
            if (getenv("LOCKSTEP")) {   /* find the first instruction where they differ (needs JITMAX=1) */
                static uint32_t tr_pc[6000], tr_h[6000]; static Cpu tr_c[6000]; static uint64_t tr_cy[6000];
                int ns = 0;
                memcpy(mem, init, MEM); cpu = st; cpu_tlb_flush(); cpu_jit = 0; cpu_code_changed();
                uint64_t c0 = cpu.cycles; for (ns = 0; ns < 6000 && cpu.cycles - c0 < 5000; ns++) {
                    tr_pc[ns] = cpu.r[15]; cpu_run(1); tr_c[ns] = cpu; tr_cy[ns] = cpu.cycles - c0;
                    uint32_t h = 0; for (int k = 0; k < MEM; k += 4) h = h * 31 + *(uint32_t *)(mem + k); tr_h[ns] = h;
                }
                memcpy(mem, init, MEM); cpu = st; cpu_tlb_flush(); cpu_jit = 1; cpu_code_changed();
                uint64_t c1 = cpu.cycles;
                for (int k = 0, kk = 0; k < ns; k++) {
                    uint32_t pc = cpu.r[15]; cpu_run(1);
                    while (k < ns - 1 && tr_cy[k] < cpu.cycles - c1) k++;   /* blocks run several instructions */
                    (void)kk;
                    uint32_t h = 0; for (int q = 0; q < MEM; q += 4) h = h * 31 + *(uint32_t *)(mem + q);
                    Cpu c = cpu;
                    if ( h != tr_h[k] || memcmp(c.r, tr_c[k].r, 64) || c.n != tr_c[k].n || c.z != tr_c[k].z || c.c != tr_c[k].c || c.v != tr_c[k].v || c.cpsr != tr_c[k].cpsr) {
                        uint32_t w = *(uint32_t *)(init + (pc & ~1u) % MEM);
                        printf("  cycles %llu vs %llu\n", (unsigned long long)tr_cy[k], (unsigned long long)(cpu.cycles - c1));
                        printf("  first difference at step %d pc=%08x insn=%08x (mem %s)\n", k, pc, (st.cpsr & 0x20) ? (w & 0xFFFF) : w, h != tr_h[k] ? "differs" : "same");
                        Cpu pre = k ? tr_c[k - 1] : st;
                        printf("  before:"); for (int q = 0; q < 16; q++) printf(" r%d=%08x", q, pre.r[q]); printf(" nzcv=%d%d%d%d\n", pre.n, pre.z, pre.c, pre.v);
                        cpu = tr_c[k]; compare(&tr_c[k], &c, mem);
                        break;
                    }
                }
            }
            if (fails >= 5) break;
        }
    }
    printf("%d cases, %d skipped, %d failures\n", cases, skipped, fails);
    return fails != 0;
}

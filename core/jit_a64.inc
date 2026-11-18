/* ZodiacDroid - ARM/Thumb -> AArch64 recompiler.
 *
 * Included at the end of cpu.c on arm64 builds so it shares the interpreter's helpers.
 * Guest code is translated a basic block at a time. Blocks are keyed by physical address,
 * Thumb state and privilege, and never cross a 4K page. Guest registers live in the Cpu
 * struct; the flags are kept packed as NZCV in w20 inside a block. Anything that is not
 * translated natively runs through the interpreter for that one instruction.
 *
 * Register use inside blocks:
 *   x19 = &cpu, w20 = guest NZCV (bits 31..28), x21/x22 = TLB read/write tables for the
 *   block's privilege, x23..x26 = values that live across helper calls,
 *   w0..w13 = temporaries, w14..w17 = flag and budget scratch, x16 = call target.
 *
 * Written from the ARM and Arm A64 architecture manuals; no code from other emulators. */

#include <sys/mman.h>
#include <stdlib.h>

int cpu_jit = 1;
static int jit_ok;                     /* code buffer available */
static int jit_maxlen = 64;

#define RCPU 19
#define RFL  20
#define RTR  21
#define RTW  22
#define WZR  31
#define OFF(f)   ((uint32_t)offsetof(Cpu, f))
#define OFF_R(n) (OFF(r) + 4u * (n))

/* ---------------------------------------------------------------- blocks and pages */

typedef struct Block {
    uint32_t pa, va;          /* translated code embeds virtual addresses: both must match */
    uint8_t thumb, priv, valid;
    void *code;
    struct Block *hnext, *pnext;
} Block;

#define MAXBLK   (1 << 17)
#define HT_N     (1 << 16)
#define CODE_SZ  (32u << 20)
static Block *blocks;
static int nblocks;
static Block *htab[HT_N];

typedef struct { uint32_t mask; Block *list; } PageInfo;   /* mask: 128-byte chunks holding code */
static PageInfo *pdir[1 << 20 >> 10];                     /* 1024 x 1024 pages */

static PageInfo *pinfo(uint32_t pa, int create) {
    uint32_t pg = pa >> 12;
    PageInfo *d = pdir[pg >> 10];
    if (!d) { if (!create) return NULL; d = pdir[pg >> 10] = calloc(1024, sizeof(PageInfo)); }
    return &d[pg & 1023];
}

typedef struct { uint32_t pc, tag; void *code; } VEnt;    /* tag = gen << 2 | thumb << 1 | priv */
#define VC_N (1 << 14)
static VEnt vc[VC_N];
static uint32_t vgen = 1;
static uint32_t jit_stop;                  /* set when code was invalidated: linked jumps must stop (x26 points here) */
static uint32_t *jbuf, *jend, *ep;
static void (*jit_enter)(Cpu *, void *, TlbEnt *, TlbEnt *);
static uint32_t *jdispatch;                /* look up the next block and jump to it, or return to C */

static void jit_tlb_flushed(void) { vgen++; }
static uint32_t jit_trace;
static uint64_t st_compiles, st_invals, st_flushes, st_interp, st_enter;

static int jit_page_code(uint32_t pa) {
    PageInfo *p = pinfo(pa, 0);
    return p && p->mask;
}

/* Write-TLB entries for pages holding code are tagged (bit 31) so stores take the slow path. */
static void tag_tlbw(uint32_t ppage, int on) {
    TlbEnt *t = &cpu.tlb_w[0][0];
    for (int i = 0; i < 2 * TLB_N; i++) {
        if (t[i].tag == 0xFFFFFFFFu || t[i].pa != ppage) continue;
        if (on) t[i].tag |= 0x80000000u; else t[i].tag &= 0x7FFFFFFFu;
    }
}

static void inval_page(PageInfo *p, uint32_t ppage) {
    st_invals++;
    for (Block *b = p->list; b; b = b->pnext) {
        b->valid = 0;
        Block **h = &htab[(b->pa >> 1) & (HT_N - 1)];
        while (*h && *h != b) h = &(*h)->hnext;
        if (*h) *h = b->hnext;
    }
    p->list = NULL;
    p->mask = 0;
    tag_tlbw(ppage, 0);
    vgen++;                                 /* drop the lookup cache */
    jit_stop = 1;                           /* and leave the running chain of linked blocks */
}

static void jit_code_write(uint32_t pa) {
    PageInfo *p = pinfo(pa, 0);
    if (p && (p->mask >> ((pa >> 7) & 31) & 1)) inval_page(p, pa & ~0xFFFu);
}

void cpu_phys_written(uint32_t pa, uint32_t len) {
    if (!jit_ok || !len) return;
    uint32_t end = pa + len - 1;
    for (uint32_t c = pa & ~127u; ; c += 128) {
        jit_code_write(c);
        if (c >= (end & ~127u)) break;
    }
}

static void jit_flush_all(void) {
    if (!jit_ok) return;
    st_flushes++;
    for (int i = 0; i < nblocks; i++) blocks[i].valid = 0;
    nblocks = 0;
    memset(htab, 0, sizeof htab);
    for (int i = 0; i < (int)(sizeof pdir / sizeof pdir[0]); i++) if (pdir[i]) memset(pdir[i], 0, 1024 * sizeof(PageInfo));
    for (int i = 0; i < 2 * TLB_N; i++) { TlbEnt *t = &cpu.tlb_w[0][0] + i; if (t->tag != 0xFFFFFFFFu) t->tag &= 0x7FFFFFFFu; }
    ep = jbuf + 256;   /* trampoline and dispatcher stay */
    vgen++;
}

void cpu_code_changed(void) { jit_flush_all(); }

/* ---------------------------------------------------------------- emitter */

#define E(x) (*ep++ = (uint32_t)(x))

enum { C_EQ, C_NE, C_CS, C_CC, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE };

static void mov32(int rd, uint32_t v) {
    if ((v >> 16) == 0) E(0x52800000 | (v << 5) | rd);
    else if ((v & 0xFFFF) == 0) E(0x52A00000 | ((v >> 16) << 5) | rd);
    else if ((v >> 16) == 0xFFFF) E(0x12800000 | ((~v & 0xFFFF) << 5) | rd);
    else { E(0x52800000 | ((v & 0xFFFF) << 5) | rd); E(0x72A00000 | ((v >> 16) << 5) | rd); }
}
static void mov64(int rd, uint64_t v) {
    E(0xD2800000 | ((uint32_t)(v & 0xFFFF) << 5) | rd);
    for (int h = 1; h < 4; h++) { uint32_t p = (v >> (16 * h)) & 0xFFFF; if (p) E(0xF2800000 | (h << 21) | (p << 5) | rd); }
}
static void call(void *f) { mov64(16, (uint64_t)(uintptr_t)f); E(0xD63F0000 | (16 << 5)); }
static void ldrw(int rt, int rn, uint32_t off) { E(0xB9400000 | ((off >> 2) << 10) | (rn << 5) | rt); }
static void strw(int rt, int rn, uint32_t off) { E(0xB9000000 | ((off >> 2) << 10) | (rn << 5) | rt); }
static void ldrx(int rt, int rn, uint32_t off) { E(0xF9400000 | ((off >> 3) << 10) | (rn << 5) | rt); }
static void strx(int rt, int rn, uint32_t off) { E(0xF9000000 | ((off >> 3) << 10) | (rn << 5) | rt); }
static void ldpw(int a, int b, int rn, uint32_t off) { E(0x29400000 | (((off >> 2) & 0x7F) << 15) | (b << 10) | (rn << 5) | a); }
static void stpw(int a, int b, int rn, uint32_t off) { E(0x29000000 | (((off >> 2) & 0x7F) << 15) | (b << 10) | (rn << 5) | a); }
static void movr(int rd, int rm) { E(0x2A0003E0 | (rm << 16) | rd); }

#define OP_AND  0x0A000000u
#define OP_BIC  0x0A200000u
#define OP_ORR  0x2A000000u
#define OP_ORN  0x2A200000u
#define OP_EOR  0x4A000000u
#define OP_ANDS 0x6A000000u
#define OP_ADD  0x0B000000u
#define OP_ADDS 0x2B000000u
#define OP_SUB  0x4B000000u
#define OP_SUBS 0x6B000000u
#define OP_ADC  0x1A000000u
#define OP_ADCS 0x3A000000u
#define OP_SBC  0x5A000000u
#define OP_SBCS 0x7A000000u
#define OP_RORV 0x1AC02C00u
#define SH_LSL 0
#define SH_LSR 1
#define SH_ASR 2
#define SH_ROR 3

static void dp(uint32_t op, int rd, int rn, int rm, int sh, int amt) { E(op | (sh << 22) | (rm << 16) | ((amt & 63) << 10) | (rn << 5) | rd); }
static void dp3(uint32_t op, int rd, int rn, int rm) { E(op | (rm << 16) | (rn << 5) | rd); }
static void addi(int rd, int rn, uint32_t imm) { E(0x11000000 | (imm << 10) | (rn << 5) | rd); }
static void subi(int rd, int rn, uint32_t imm) { E(0x51000000 | (imm << 10) | (rn << 5) | rd); }
static void cmpr(int rn, int rm) { dp(OP_SUBS, WZR, rn, rm, 0, 0); }
static void cmpi(int rn, uint32_t imm) { E(0x7100001F | (imm << 10) | (rn << 5)); }
static void ubfm(int rd, int rn, int immr, int imms) { E(0x53000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void sbfm(int rd, int rn, int immr, int imms) { E(0x13000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
static void lsri(int rd, int rn, int s) { ubfm(rd, rn, s, 31); }
static void lsli(int rd, int rn, int s) { ubfm(rd, rn, (32 - s) & 31, 31 - s); }
static void asri(int rd, int rn, int s) { sbfm(rd, rn, s, 31); }
static void rori(int rd, int rn, int s) { E(0x13800000 | (rn << 16) | (s << 10) | (rn << 5) | rd); }
static void ubfx(int rd, int rn, int lsb, int w) { ubfm(rd, rn, lsb, lsb + w - 1); }
static void bfi(int rd, int rn, int lsb, int w) { E(0x33000000 | (((32 - lsb) & 31) << 16) | ((w - 1) << 10) | (rn << 5) | rd); }
static void csel(int rd, int rn, int rm, int cond) { E(0x1A800000 | (rm << 16) | (cond << 12) | (rn << 5) | rd); }
static void mrs_nzcv(int rt) { E(0xD53B4200 | rt); }
static void msr_nzcv(int rt) { E(0xD51B4200 | rt); }
static void addx_sh(int rd, int rn, int rm, int sh) { E(0x8B000000 | (rm << 16) | (sh << 10) | (rn << 5) | rd); }

static uint32_t ror32(uint32_t v, int r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }

/* AArch64 logical immediate (32-bit) */
static int logimm(uint32_t v, uint32_t *enc) {
    if (v == 0 || v == 0xFFFFFFFFu) return 0;
    int size = 32;
    while (size > 2) {
        int h = size / 2; uint32_t m = (1u << h) - 1;
        if (((v >> h) & m) != (v & m)) break;
        size = h;
    }
    uint32_t emask = size == 32 ? 0xFFFFFFFFu : (1u << size) - 1;
    uint32_t elem = v & emask;
    int ones = __builtin_popcount(elem);
    uint32_t pat = ones == 32 ? 0xFFFFFFFFu : (1u << ones) - 1;
    for (int r = 0; r < size; r++) {
        uint32_t rot = r ? ((pat >> r) | (pat << (size - r))) & emask : pat;
        if (rot == elem) {
            uint32_t imms = ((~(2u * size - 1)) & 0x3F) | (ones - 1);
            *enc = ((uint32_t)r << 16) | (imms << 10);
            return 1;
        }
    }
    return 0;
}
static void logi(uint32_t opi, uint32_t opr, int rd, int rn, uint32_t v) {
    uint32_t e;
    if (logimm(v, &e)) E(opi | e | (rn << 5) | rd);
    else { mov32(17, v); dp(opr, rd, rn, 17, 0, 0); }
}
#define andi(d, n, v) logi(0x12000000, OP_AND, d, n, v)
#define orri(d, n, v) logi(0x32000000, OP_ORR, d, n, v)

static uint32_t *bc(int cond) { uint32_t *p = ep; E(0x54000000 | cond); return p; }
static uint32_t *bra(void) { uint32_t *p = ep; E(0x14000000); return p; }
static uint32_t *cbzw(int rt) { uint32_t *p = ep; E(0x34000000 | rt); return p; }
static uint32_t *cbnzw(int rt) { uint32_t *p = ep; E(0x35000000 | rt); return p; }
static uint32_t *cbzx(int rt) { uint32_t *p = ep; E(0xB4000000 | rt); return p; }
static uint32_t *tbz(int rt, int bit) { uint32_t *p = ep; E(0x36000000 | ((bit & 32) << 26) | ((bit & 31) << 19) | rt); return p; }
static void fix_to(uint32_t *p, uint32_t *t) {
    int32_t d = (int32_t)(t - p);
    uint32_t i = *p;
    if ((i & 0x7C000000) == 0x14000000) *p = (i & 0xFC000000) | (d & 0x3FFFFFF);
    else if ((i & 0x7E000000) == 0x36000000) *p = i | ((d & 0x3FFF) << 5);
    else *p = i | ((d & 0x7FFFF) << 5);
}
static void fix(uint32_t *p) { fix_to(p, ep); }

/* ---------------------------------------------------------------- per-block compile state */

static struct {
    int thumb, priv;
    int pending;            /* cycles of executed instructions not yet taken off the budget */
    int fl_loaded, fl_dirty;
    int in_cond;
    uint8_t *page;          /* host pointer to the block's page */
    uint32_t vpage;         /* virtual page base of the block */
    uint32_t mask;          /* 128-byte chunks read (code and folded literals) */
    uint32_t *exits[512]; int nexits;
    uint32_t pc0, pa0; uint32_t *start;
    int stored;             /* a store was emitted: literals may have changed, don't fold them */
} J;

static void ld_reg(int w, int n) { ldrw(w, RCPU, OFF_R(n)); }
static void st_reg(int w, int n) { strw(w, RCPU, OFF_R(n)); }
static void ld_reg_pc(int w, int n, uint32_t pcval) { if (n == 15) mov32(w, pcval); else ld_reg(w, n); }

static void fl_load(void) {
    if (J.fl_loaded) return;
    ldpw(14, 15, RCPU, OFF(n));
    ldpw(16, 17, RCPU, OFF(c));
    lsli(RFL, 14, 31);
    dp(OP_ORR, RFL, RFL, 15, SH_LSL, 30);
    dp(OP_ORR, RFL, RFL, 16, SH_LSL, 29);
    dp(OP_ORR, RFL, RFL, 17, SH_LSL, 28);
    J.fl_loaded = 1; J.fl_dirty = 0;
}
static void emit_spill(void) {
    lsri(14, RFL, 31); ubfx(15, RFL, 30, 1); stpw(14, 15, RCPU, OFF(n));
    ubfx(16, RFL, 29, 1); ubfx(17, RFL, 28, 1); stpw(16, 17, RCPU, OFF(c));
}
static void fl_spill(void) { if (J.fl_dirty) { emit_spill(); J.fl_dirty = 0; } }
static void fl_arith(void) { mrs_nzcv(RFL); J.fl_loaded = 1; J.fl_dirty = 1; }

static void budget_sub(int k) {
    if (!k) return;
    ldrx(17, RCPU, OFF(budget));
    if (k > 0) E(0xD1000000 | ((uint32_t)k << 10) | (17 << 5) | 17);
    else E(0x91000000 | ((uint32_t)-k << 10) | (17 << 5) | 17);
    strx(17, RCPU, OFF(budget));
}
static void cost_extra(int k) { if (J.in_cond) budget_sub(k); else J.pending += k; }

static void exit_jump(void) { if (J.nexits < 512) J.exits[J.nexits++] = bra(); }
static void emit_exit(int cost) { if (J.fl_dirty) emit_spill(); budget_sub(J.pending + cost); exit_jump(); }
static int hle_marked(uint32_t va);
/* code of an existing block at npc that this block may jump straight to: same page (so it is
 * dropped together with this one), same state, no HLE hook */
static void *link_target(uint32_t npc) {
    if (((npc ^ J.pc0) & ~0xFFFu) || (npc & (J.thumb ? 1 : 3)) || hle_marked(npc)) return NULL;
    if (npc == J.pc0) return J.start;
    uint32_t pa = (J.pa0 & ~0xFFFu) | (npc & 0xFFF);
    for (Block *b = htab[(pa >> 1) & (HT_N - 1)]; b; b = b->hnext)
        if (b->pa == pa && b->va == npc && b->thumb == J.thumb && b->priv == J.priv && b->valid) return b->code;
    return NULL;
}
static void emit_exit_pc(uint32_t npc, int cost) {
    mov32(9, npc); st_reg(9, 15);
    void *t = link_target(npc);
    if (!t) { emit_exit(cost); return; }
    if (J.fl_dirty) emit_spill();
    budget_sub(J.pending + cost);
    ldrx(17, RCPU, OFF(budget));
    E(0xF100023F);                          /* cmp x17, #0 */
    uint32_t *o1 = bc(C_LE);
    ldrw(17, RCPU, OFF(int_pending));
    uint32_t *o2 = cbnzw(17);
    ldrw(17, 26, 0);
    uint32_t *o3 = cbnzw(17);
    uint32_t *b = bra(); fix_to(b, t);
    fix(o1); fix(o2); fix(o3);              /* conditional branches only reach 1MB: go via a local B */
    exit_jump();
}

/* N,Z from res; C as given (-1 keep, 0/1 constant, 2 = w12); V kept */
static void logic_flags(int res, int carry) {
    fl_load();
    dp(OP_ANDS, WZR, res, res, 0, 0);
    mrs_nzcv(14);
    andi(RFL, RFL, 0x3FFFFFFFu);
    dp(OP_ORR, RFL, RFL, 14, 0, 0);
    if (carry == 0) andi(RFL, RFL, 0xDFFFFFFFu);
    else if (carry == 1) orri(RFL, RFL, 0x20000000u);
    else if (carry == 2) bfi(RFL, 12, 29, 1);
    J.fl_dirty = 1;
}

/* ---------------------------------------------------------------- helpers called from blocks */

static uint32_t jh_ld32(uint32_t a, int p) { return rd_slow(a & ~3u, 4, p); }
static uint32_t jh_ldr(uint32_t a, int p) { uint32_t v = rd_slow(a & ~3u, 4, p); return ror32(v, (a & 3) * 8); }
static uint32_t jh_ld16(uint32_t a, int p) { return rd_slow(a & ~1u, 2, p); }
static uint32_t jh_ld16s(uint32_t a, int p) { return (uint32_t)(int32_t)(int16_t)rd_slow(a & ~1u, 2, p); }
static uint32_t jh_ld8(uint32_t a, int p) { return rd_slow(a, 1, p); }
static uint32_t jh_ld8s(uint32_t a, int p) { return (uint32_t)(int32_t)(int8_t)rd_slow(a, 1, p); }
static void jh_st32(uint32_t a, uint32_t v, int p) { wr_slow(a & ~3u, v, 4, p); }
static void jh_st16(uint32_t a, uint32_t v, int p) { wr_slow(a & ~1u, v, 2, p); }
static void jh_st8(uint32_t a, uint32_t v, int p) { wr_slow(a, v, 1, p); }

static void jh_abort(uint32_t pc) { take_exception(0x10, M_ABT, pc + 8, 0); }

/* LDM/STM-style transfer of the registers in list from/to consecutive words at a (aligned).
 * Loads only change registers if every read succeeded; r15 receives the raw value. */
static void jh_xfer(uint32_t a, uint32_t list, int load, int p, uint32_t pcstore) {
    if (load) {
        uint32_t vals[16];
        for (int i = 0; i < 16; i++) if (list & (1u << i)) { vals[i] = rd32p(a, p); a += 4; }
        if (cpu.abort) return;
        for (int i = 0; i < 16; i++) if (list & (1u << i)) cpu.r[i] = vals[i];
    } else {
        for (int i = 0; i < 16; i++) if (list & (1u << i)) { wr32p(a, i == 15 ? pcstore : cpu.r[i], p); a += 4; }
    }
}

/* One instruction through the interpreter, exactly as the interpreter loop would run it.
 * Returns nonzero if the block must stop (control flow, mode change, interrupt, ...). */
static int jh_fb_arm(uint32_t pc, uint32_t insn) {
    uint32_t c0 = cpu.cpsr;
    branched = 0; cpu.abort = 0;
    if ((insn >> 28) == 0xE || cond_pass(insn >> 28)) {
        if ((insn >> 28) == 0xF) { undefined(pc, insn, 0); branched = 1; }
        else arm_step(pc, insn);
    }
    if (cpu.abort) { take_exception(0x10, M_ABT, pc + 8, 0); cpu.budget -= 1; return 1; }
    if (!branched) cpu.r[15] = pc + 4;
    cpu.budget -= 1;
    return branched || cpu.cpsr != c0 || cpu.halted || cpu.int_pending || ((insn >> 24) & 0xF) == 0xE;
}
static int jh_fb_thumb(uint32_t pc, uint32_t insn) {
    uint32_t c0 = cpu.cpsr;
    branched = 0; cpu.abort = 0;
    thumb_step(pc, insn);
    if (cpu.abort) { take_exception(0x10, M_ABT, pc + 8, 0); cpu.budget -= 1; return 1; }
    if (!branched) cpu.r[15] = pc + 2;
    cpu.budget -= 1;
    return branched || cpu.cpsr != c0 || cpu.halted || cpu.int_pending;
}

/* ---------------------------------------------------------------- memory access */

enum { MK_W, MK_LDR, MK_H, MK_SH, MK_B, MK_SB };
static void *const ld_helper[] = { (void *)jh_ld32, (void *)jh_ldr, (void *)jh_ld16, (void *)jh_ld16s, (void *)jh_ld8, (void *)jh_ld8s };
static const uint32_t ld_op[] = { 0xB8606800, 0xB8606800, 0x78606800, 0x78E06800, 0x38606800, 0x38E06800 };

static void emit_abort_stub(uint32_t pc) {
    if (J.fl_dirty) emit_spill();
    budget_sub(J.pending + 1);
    mov32(0, pc);
    call((void *)jh_abort);
    exit_jump();
}

/* page lookup of the address in ra through the table in rt; leaves x12 = host page and
 * branches to the returned fixups on a miss */
static void tlb_probe(int ra, int rt, uint32_t **m1, uint32_t **m2) {
    lsri(9, ra, 12);
    andi(10, 9, 0xFFF);
    addx_sh(11, rt, 10, 4);
    ldrw(12, 11, 0);
    cmpr(12, 9);
    *m1 = bc(C_NE);
    ldrx(12, 11, 8);
    *m2 = cbzx(12);
}

/* load from the address in ra (kept) into w0 */
static void emit_load(int kind, int ra, uint32_t pc) {
    uint32_t *m1, *m2;
    tlb_probe(ra, RTR, &m1, &m2);
    int sz = kind <= MK_LDR ? 4 : kind <= MK_SH ? 2 : 1;
    andi(10, ra, sz == 4 ? 0xFFC : sz == 2 ? 0xFFE : 0xFFF);
    E(ld_op[kind] | (10 << 16) | (12 << 5) | 0);
    if (kind == MK_LDR) { andi(9, ra, 3); lsli(9, 9, 3); dp3(OP_RORV, 0, 0, 9); }
    uint32_t *done = bra();
    fix(m1); fix(m2);
    budget_sub(J.pending);
    movr(0, ra); mov32(1, J.priv);
    call(ld_helper[kind]);
    budget_sub(-J.pending);
    ldrw(9, RCPU, OFF(abort));
    uint32_t *ok = cbzw(9);
    emit_abort_stub(pc);
    fix(ok); fix(done);
}

static void *const st_helper[] = { NULL, (void *)jh_st8, (void *)jh_st16, NULL, (void *)jh_st32 };
static const uint32_t st_op[] = { 0, 0x38206800, 0x78206800, 0, 0xB8206800 };

/* store rv to the address in ra (both kept across the slow path: use x23..x26) */
static void emit_store(int sz, int ra, int rv, uint32_t pc) {
    uint32_t *m1, *m2;
    J.stored = 1;
    tlb_probe(ra, RTW, &m1, &m2);
    andi(10, ra, sz == 4 ? 0xFFC : sz == 2 ? 0xFFE : 0xFFF);
    E(st_op[sz] | (10 << 16) | (12 << 5) | rv);
    uint32_t *done = bra();
    fix(m1); fix(m2);
    budget_sub(J.pending);
    movr(0, ra); movr(1, rv); mov32(2, J.priv);
    call(st_helper[sz]);
    budget_sub(-J.pending);
    ldrw(9, RCPU, OFF(abort));
    uint32_t *ok = cbzw(9);
    emit_abort_stub(pc);
    fix(ok); fix(done);
}

/* block transfer of list at the aligned address in w23. cnt cycles were charged already
 * (before the accesses, like the interpreter). */
static void emit_xfer(int load, uint32_t list, int cnt, uint32_t pcstore, uint32_t pc) {
    uint32_t *m1, *m2;
    if (!load) J.stored = 1;
    andi(23, 23, ~3u);
    tlb_probe(23, load ? RTR : RTW, &m1, &m2);
    andi(10, 23, 0xFFF);
    cmpi(10, 0x1000 - 4 * cnt);
    uint32_t *m3 = bc(C_HI);
    addx_sh(12, 12, 10, 0);
    int k = 0;
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        int pair = i < 15 && (list & (1u << (i + 1))) && k < 63;
        if (load) {
            if (pair) { ldpw(9, 10, 12, 4 * k); stpw(9, 10, RCPU, OFF_R(i)); k += 2; i++; }
            else { ldrw(9, 12, 4 * k); st_reg(9, i); k++; }
        } else {
            if (pair && i + 1 != 15) { ldpw(9, 10, RCPU, OFF_R(i)); stpw(9, 10, 12, 4 * k); k += 2; i++; }
            else { ld_reg_pc(9, i, pcstore); strw(9, 12, 4 * k); k++; }
        }
    }
    uint32_t *done = bra();
    fix(m1); fix(m2); fix(m3);
    budget_sub(J.pending);
    movr(0, 23); mov32(1, list); mov32(2, load); mov32(3, J.priv); mov32(4, pcstore);
    call((void *)jh_xfer);
    budget_sub(-J.pending);
    ldrw(9, RCPU, OFF(abort));
    uint32_t *ok = cbzw(9);
    emit_abort_stub(pc);
    fix(ok); fix(done);
}

/* ---------------------------------------------------------------- shifter */

/* shift w11 by an immediate; with needc the shifter carry goes to w12 and the return
 * value says how C changes (-1 keep, 2 = w12) */
static int shift_imm(int t, int a, int needc) {
    switch (t) {
    case 0:
        if (a == 0) return -1;
        if (needc) ubfx(12, 11, 32 - a, 1);
        lsli(11, 11, a);
        return needc ? 2 : -1;
    case 1:
        if (a == 0) { if (needc) lsri(12, 11, 31); movr(11, WZR); return needc ? 2 : -1; }
        if (needc) ubfx(12, 11, a - 1, 1);
        lsri(11, 11, a);
        return needc ? 2 : -1;
    case 2:
        if (a == 0) a = 32;
        if (needc) ubfx(12, 11, a - 1, 1);
        asri(11, 11, a == 32 ? 31 : a);
        return needc ? 2 : -1;
    default:
        if (a == 0) {   /* RRX */
            fl_load();
            ubfx(13, RFL, 29, 1);
            if (needc) ubfx(12, 11, 0, 1);
            lsri(11, 11, 1);
            dp(OP_ORR, 11, 11, 13, SH_LSL, 31);
            return needc ? 2 : -1;
        }
        if (needc) ubfx(12, 11, a - 1, 1);
        rori(11, 11, a);
        return needc ? 2 : -1;
    }
}

/* shift w11 by the low byte of w13 (ARM register-specified shift) */
static int shift_reg(int t, int needc) {
    andi(13, 13, 0xFF);
    switch (t) {
    case 0:
        mov32(9, 33); cmpi(13, 33); csel(9, 13, 9, C_CC);
        E(0x9AC02000 | (9 << 16) | (11 << 5) | 10);           /* lslv x10, x11, x9 */
        if (needc) E(0xD3400000 | (32 << 16) | (32 << 10) | (10 << 5) | 12);  /* ubfx x12, x10, #32, #1 */
        movr(11, 10);
        break;
    case 1:
        mov32(9, 33); cmpi(13, 33); csel(9, 13, 9, C_CC);
        E(0xD37FF800 | (11 << 5) | 10);                       /* lsl x10, x11, #1 */
        E(0x9AC02400 | (9 << 16) | (10 << 5) | 10);           /* lsrv x10, x10, x9 */
        if (needc) andi(12, 10, 1);
        E(0xD341FC00 | (10 << 5) | 11);                       /* lsr x11, x10, #1 */
        break;
    case 2:
        mov32(9, 32); cmpi(13, 32); csel(9, 13, 9, C_CC);
        E(0x93407C00 | (11 << 5) | 10);                       /* sxtw x10, w11 */
        E(0xD37FF800 | (10 << 5) | 10);                       /* lsl x10, x10, #1 */
        E(0x9AC02800 | (9 << 16) | (10 << 5) | 10);           /* asrv x10, x10, x9 */
        if (needc) andi(12, 10, 1);
        E(0x9341FC00 | (10 << 5) | 11);                       /* asr x11, x10, #1 */
        break;
    default:
        dp3(OP_RORV, 11, 11, 13);
        if (needc) lsri(12, 11, 31);
        break;
    }
    if (!needc) return -1;
    fl_load();
    ubfx(14, RFL, 29, 1);
    cmpi(13, 0);
    csel(12, 12, 14, C_NE);
    return 2;
}

/* ---------------------------------------------------------------- ARM */

static void fallback(uint32_t pc, uint32_t insn, int thumb) {
    J.stored = 1;
    fl_spill();
    budget_sub(J.pending); J.pending = 0;
    mov32(0, pc); mov32(1, insn);
    call(thumb ? (void *)jh_fb_thumb : (void *)jh_fb_arm);
    J.fl_loaded = 0;
    uint32_t *c = cbzw(0);
    exit_jump();
    fix(c);
}

static uint32_t page_word(uint32_t va) { J.mask |= 1u << ((va >> 7) & 31); return *(uint32_t *)(J.page + (va & 0xFFC)); }

/* data processing; returns 0 (falls through) */
static void nat_dp(uint32_t pc, uint32_t insn) {
    int op = (insn >> 25) & 7, opc = (insn >> 21) & 15, S = (insn >> 20) & 1;
    int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
    int logical = opc <= 1 || opc == 8 || opc == 9 || opc >= 12;
    int needc = S && logical;
    int carry = -1, isimm = 0;
    uint32_t immv = 0, rnpc = pc + 8;
    if (opc == 5 || opc == 6 || opc == 7) fl_load();
    if (op == 1) {
        uint32_t r = ((insn >> 8) & 15) * 2;
        immv = ror32(insn & 0xFF, r);
        if (r && needc) carry = immv >> 31;
        isimm = 1;
    } else if (!(insn & 0x10)) {
        ld_reg_pc(11, insn & 15, pc + 8);
        carry = shift_imm((insn >> 5) & 3, (insn >> 7) & 31, needc);
    } else {
        ld_reg_pc(11, insn & 15, pc + 12);
        ld_reg_pc(13, (insn >> 8) & 15, pc + 8);
        carry = shift_reg((insn >> 5) & 3, needc);
        cost_extra(1);
        rnpc = pc + 12;
    }
    if (opc != 13 && opc != 15) ld_reg_pc(10, rn, rnpc);
    int addsub_imm = isimm && immv < 4096 && (opc == 2 || opc == 4 || opc == 10 || opc == 11);
    if (isimm && !addsub_imm) mov32(11, immv);
    int res = 9, wr = 1;
    switch (opc) {
    case 0: dp(OP_AND, res, 10, 11, 0, 0); break;
    case 1: dp(OP_EOR, res, 10, 11, 0, 0); break;
    case 8: dp(OP_AND, res, 10, 11, 0, 0); wr = 0; break;
    case 9: dp(OP_EOR, res, 10, 11, 0, 0); wr = 0; break;
    case 12: dp(OP_ORR, res, 10, 11, 0, 0); break;
    case 13: res = 11; break;
    case 14: dp(OP_BIC, res, 10, 11, 0, 0); break;
    case 15: dp(OP_ORN, res, WZR, 11, 0, 0); break;
    case 2: case 10:
        if (opc == 10) { res = WZR; wr = 0; }
        if (addsub_imm) E((S ? 0x71000000 : 0x51000000) | (immv << 10) | (10 << 5) | res);
        else dp(S ? OP_SUBS : OP_SUB, res, 10, 11, 0, 0);
        break;
    case 4: case 11:
        if (opc == 11) { res = WZR; wr = 0; }
        if (addsub_imm) E((S ? 0x31000000 : 0x11000000) | (immv << 10) | (10 << 5) | res);
        else dp(S ? OP_ADDS : OP_ADD, res, 10, 11, 0, 0);
        break;
    case 3: dp(S ? OP_SUBS : OP_SUB, res, 11, 10, 0, 0); break;
    case 5: msr_nzcv(RFL); dp3(S ? OP_ADCS : OP_ADC, res, 10, 11); break;
    case 6: msr_nzcv(RFL); dp3(S ? OP_SBCS : OP_SBC, res, 10, 11); break;
    case 7: msr_nzcv(RFL); dp3(S ? OP_SBCS : OP_SBC, res, 11, 10); break;
    }
    if (S) {
        if (logical) logic_flags(res, carry);
        else fl_arith();
    }
    if (wr) st_reg(res, rd);
}

static void nat_mul(uint32_t insn) {
    int S = (insn >> 20) & 1;
    if ((insn & 0x0FC000F0) == 0x00000090) {
        int rd = (insn >> 16) & 15, rn = (insn >> 12) & 15, rs = (insn >> 8) & 15, rm = insn & 15;
        ld_reg(10, rm); ld_reg(11, rs);
        int ra = WZR;
        if (insn & (1 << 21)) { ld_reg(12, rn); ra = 12; }
        E(0x1B000000 | (11 << 16) | (ra << 10) | (10 << 5) | 13);
        st_reg(13, rd);
        if (S) logic_flags(13, -1);
        cost_extra(2);
        return;
    }
    int hi = (insn >> 16) & 15, lo = (insn >> 12) & 15, rs = (insn >> 8) & 15, rm = insn & 15;
    ld_reg(10, rm); ld_reg(11, rs);
    int ra = WZR;
    if (insn & (1 << 21)) { ld_reg(12, lo); ld_reg(9, hi); E(0xAA000000 | (9 << 16) | (32 << 10) | (12 << 5) | 12); ra = 12; }
    E(((insn & (1 << 22)) ? 0x9B200000 : 0x9BA00000) | (11 << 16) | (ra << 10) | (10 << 5) | 13);
    st_reg(13, lo);
    E(0xD3400000 | (32 << 16) | (63 << 10) | (13 << 5) | 9);    /* lsr x9, x13, #32 */
    st_reg(9, hi);
    if (S) {
        fl_load();
        E(0xEA000000 | (13 << 16) | (13 << 5) | WZR);              /* tst x13, x13 */
        mrs_nzcv(14);
        andi(RFL, RFL, 0x3FFFFFFFu);
        dp(OP_ORR, RFL, RFL, 14, 0, 0);
        J.fl_dirty = 1;
    }
    cost_extra(3);
}

/* address for single and halfword transfers: w23 = access address, w24 = written-back base */
static void xfer_addr(int P, int U, int rn, uint32_t pc8, int off_reg, uint32_t off_imm) {
    ld_reg_pc(24, rn, pc8);
    if (off_reg >= 0) dp(U ? OP_ADD : OP_SUB, 24, 24, off_reg, 0, 0);
    else if (off_imm) { if (U) addi(24, 24, off_imm); else subi(24, 24, off_imm); }
    if (P) movr(23, 24); else ld_reg_pc(23, rn, pc8);
}

static int nat_ldst(uint32_t pc, uint32_t insn) {
    int op = (insn >> 25) & 7;
    int P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
    uint32_t pc8 = pc + 8;
    if (op == 2 && rn == 15 && P && !W && L) {    /* literal load from this page: fold it */
        uint32_t off = insn & 0xFFF, a = U ? pc8 + off : pc8 - off;
        if (((a ^ pc) & ~0xFFFu) == 0 && !J.stored) {
            uint32_t v = page_word(a);
            if (B) v = (v >> ((a & 3) * 8)) & 0xFF; else v = ror32(v, (a & 3) * 8);
            mov32(9, v); st_reg(9, rd);
            cost_extra(1);
            return 0;
        }
    }
    if (op == 2) xfer_addr(P, U, rn, pc8, -1, insn & 0xFFF);
    else {
        ld_reg_pc(11, insn & 15, pc8);
        int c = shift_imm((insn >> 5) & 3, (insn >> 7) & 31, 0); (void)c;
        xfer_addr(P, U, rn, pc8, 11, 0);
    }
    int wb = !P || W;
    if (L) {
        emit_load(B ? MK_B : MK_LDR, 23, pc);
        if (wb) st_reg(24, rn);
        st_reg(0, rd);
        cost_extra(1);
    } else {
        ld_reg_pc(25, rd, pc + 12);
        emit_store(B ? 1 : 4, 23, 25, pc);
        if (wb) st_reg(24, rn);
    }
    return 0;
}

static int nat_half(uint32_t pc, uint32_t insn) {
    int P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
    int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15, sh = (insn >> 5) & 3;
    uint32_t pc8 = pc + 8;
    if (I) xfer_addr(P, U, rn, pc8, -1, ((insn >> 4) & 0xF0) | (insn & 15));
    else { ld_reg_pc(11, insn & 15, pc8); xfer_addr(P, U, rn, pc8, 11, 0); }
    int wb = !P || W;
    if (L) {
        emit_load(sh == 1 ? MK_H : sh == 2 ? MK_SB : MK_SH, 23, pc);
        if (wb) st_reg(24, rn);
        st_reg(0, rd);
        cost_extra(1);
    } else {
        ld_reg_pc(25, rd, pc + 12);
        emit_store(2, 23, 25, pc);
        if (wb) st_reg(24, rn);
    }
    return 0;
}

/* LDM/STM (no S bit, rn != 15, non-empty list). Returns 1 if it loads pc. */
static int nat_ldm(uint32_t pc, uint32_t insn) {
    int rn = (insn >> 16) & 15;
    uint32_t list = insn & 0xFFFF;
    int L = (insn >> 20) & 1, W = (insn >> 21) & 1, U = (insn >> 23) & 1, P = (insn >> 24) & 1;
    int cnt = __builtin_popcount(list);
    cost_extra(cnt);
    ld_reg(24, rn);
    if (U) { if (P) addi(23, 24, 4); else movr(23, 24); addi(24, 24, cnt * 4); }
    else { subi(24, 24, cnt * 4); if (P) movr(23, 24); else addi(23, 24, 4); }
    andi(23, 23, ~3u);
    emit_xfer(L, list, cnt, pc + 12, pc);
    if (W && !(L && (list & (1u << rn)))) st_reg(24, rn);
    if (L && (list & 0x8000)) {
        ld_reg(9, 15); andi(9, 9, ~3u); st_reg(9, 15);
        emit_exit(1);
        return 1;
    }
    return 0;
}

static void emit_bx(int rm, int link, uint32_t lrval, int cost, uint32_t pcval) {
    ld_reg_pc(13, rm, pcval);
    if (link) { mov32(9, lrval); st_reg(9, 14); }
    ldrw(9, RCPU, OFF(cpsr));
    uint32_t *arm = tbz(13, 0);
    orri(9, 9, 0x20); andi(13, 13, ~1u);
    uint32_t *j = bra();
    fix(arm);
    andi(9, 9, ~0x20u); andi(13, 13, ~3u);
    fix(j);
    strw(9, RCPU, OFF(cpsr));
    st_reg(13, 15);
    emit_exit(cost);
}

/* returns: 0 = native, continues; 1 = native, transfers control (emitted its exits); -1 = not native */
static int nat_arm(uint32_t pc, uint32_t insn) {
    uint32_t op = (insn >> 25) & 7;
    switch (op) {
    case 0: case 1: {
        if (op == 0 && (insn & 0x90) == 0x90) {
            uint32_t sh = (insn >> 5) & 3;
            if (sh == 0) {
                if (((insn >> 16) & 15) == 15 || ((insn >> 12) & 15) == 15 || ((insn >> 8) & 15) == 15 || (insn & 15) == 15) return -1;
                if ((insn & 0x0FC000F0) == 0x00000090) { nat_mul(insn); return 0; }
                if ((insn & 0x0F8000F0) == 0x00800090) {
                    nat_mul(insn); return 0;
                }
                return -1;
            }
            int L = (insn >> 20) & 1, rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
            if (!L && sh != 1) return -1;                       /* LDRD/STRD */
            if (L && rd == 15) return -1;
            if (rn == 15 && (!((insn >> 24) & 1) || ((insn >> 21) & 1))) return -1;
            return nat_half(pc, insn);
        }
        uint32_t opc = (insn >> 21) & 15;
        int S = (insn >> 20) & 1, rd = (insn >> 12) & 15;
        if (!S && opc >= 8 && opc <= 11) {
            if (op == 0 && (insn & 0x0FFFFFD0) == 0x012FFF10) { emit_bx(insn & 15, insn & 0x20, pc + 4, 1, pc + 8); return 1; }
            if ((insn & 0x0FBF0FFF) == 0x010F0000 && rd != 15) {     /* MRS */
                if (insn & (1 << 22)) ldrw(9, RCPU, OFF(spsr));
                else { fl_load(); ldrw(9, RCPU, OFF(cpsr)); andi(9, 9, 0xFF); dp(OP_ORR, 9, 9, RFL, 0, 0); }
                st_reg(9, rd);
                return 0;
            }
            return -1;
        }
        if (rd == 15 && !(opc >= 8 && opc <= 11)) return -1;
        if (S && rd == 15) return -1;
        nat_dp(pc, insn);
        return 0;
    }
    case 2: case 3: {
        if (op == 3 && (insn & 0x10)) return -1;
        int P = (insn >> 24) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
        if (!P && W) return -1;                                 /* LDRT/STRT */
        if (L && rd == 15) return -1;
        if (rn == 15 && (!P || W)) return -1;
        return nat_ldst(pc, insn);
    }
    case 4: {
        int rn = (insn >> 16) & 15;
        if ((insn & (1 << 22)) || rn == 15 || !(insn & 0xFFFF)) return -1;
        return nat_ldm(pc, insn);
    }
    case 5: {
        int32_t off = ((int32_t)(insn << 8)) >> 6;
        if (insn & (1 << 24)) { mov32(9, pc + 4); st_reg(9, 14); }
        emit_exit_pc(pc + 8 + off, 1);
        return 1;
    }
    default:
        return -1;
    }
}

/* does an unconditional fallback instruction always leave the block? */
static int arm_always_leaves(uint32_t insn) {
    uint32_t op = (insn >> 25) & 7;
    if (op == 7) return 1;                                      /* SWI, coprocessor */
    if (op == 6) return 1;
    if ((insn >> 28) == 0xF) return 1;
    if (op == 4 && (insn & 0x8000) && (insn & (1 << 20))) return 1;
    if ((op == 2 || op == 3) && (insn & (1 << 20)) && ((insn >> 12) & 15) == 15) return 1;
    if (op <= 1 && ((insn >> 12) & 15) == 15 && !(((insn >> 21) & 15) >= 8 && ((insn >> 21) & 15) <= 11)) return 1;
    return 0;
}

static int tr_arm(uint32_t pc, uint32_t insn) {
    uint32_t cond = insn >> 28;
    if (cond == 0xF) { fallback(pc, insn, 0); exit_jump(); return 1; }
    /* try native translation; roll back if it isn't */
    uint32_t *save_ep = ep;
    int save_loaded = J.fl_loaded, save_dirty = J.fl_dirty, save_pending = J.pending, save_nexits = J.nexits;
    uint32_t save_mask = J.mask;
    uint32_t *skip = NULL;
    if (cond != 0xE) { fl_load(); msr_nzcv(RFL); skip = bc(cond ^ 1); J.in_cond = 1; }
    int r = nat_arm(pc, insn);
    J.in_cond = 0;
    if (r < 0) {
        ep = save_ep; J.fl_loaded = save_loaded; J.fl_dirty = save_dirty; J.pending = save_pending;
        J.nexits = save_nexits; J.mask = save_mask;
        fallback(pc, insn, 0);
        if (cond == 0xE && arm_always_leaves(insn)) { exit_jump(); return 1; }   /* helper set r15 either way */
        return 0;
    }
    if (skip) fix(skip);
    J.pending += 1;
    if (r == 1) {
        if (skip) emit_exit_pc(pc + 4, 0);
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- Thumb */

static int tr_thumb(uint32_t pc, uint32_t i) {
    uint32_t pc4 = pc + 4;
    int r = 0;
    switch (i >> 11) {
    case 0x00: case 0x01: case 0x02: {
        ld_reg(11, (i >> 3) & 7);
        int c = shift_imm(i >> 11, (i >> 6) & 31, 1);
        st_reg(11, i & 7);
        logic_flags(11, c);
        break;
    }
    case 0x03:
        ld_reg(10, (i >> 3) & 7);
        if (i & 0x400) E(((i & 0x200) ? 0x71000000 : 0x31000000) | (((i >> 6) & 7) << 10) | (10 << 5) | 9);
        else { ld_reg(11, (i >> 6) & 7); dp((i & 0x200) ? OP_SUBS : OP_ADDS, 9, 10, 11, 0, 0); }
        fl_arith();
        st_reg(9, i & 7);
        break;
    case 0x04: {
        uint32_t v = i & 0xFF;
        mov32(9, v); st_reg(9, (i >> 8) & 7);
        fl_load();
        andi(RFL, RFL, 0x3FFFFFFFu);
        if (!v) orri(RFL, RFL, 0x40000000u);
        J.fl_dirty = 1;
        break;
    }
    case 0x05: ld_reg(10, (i >> 8) & 7); cmpi(10, i & 0xFF); fl_arith(); break;
    case 0x06: case 0x07: {
        int d = (i >> 8) & 7;
        ld_reg(10, d);
        E(((i >> 11) == 6 ? 0x31000000 : 0x71000000) | ((i & 0xFF) << 10) | (10 << 5) | 9);
        fl_arith();
        st_reg(9, d);
        break;
    }
    case 0x08:
        if (!(i & 0x400)) {
            int d = i & 7, s = (i >> 3) & 7, o = (i >> 6) & 15;
            switch (o) {
            case 2: case 3: case 4: case 7: {
                ld_reg(11, d); ld_reg(13, s);
                int c = shift_reg(o == 2 ? 0 : o == 3 ? 1 : o == 4 ? 2 : 3, 1);
                st_reg(11, d);
                logic_flags(11, c);
                return 0;
            }
            }
            ld_reg(10, d); ld_reg(11, s);
            switch (o) {
            case 0: dp(OP_AND, 9, 10, 11, 0, 0); st_reg(9, d); logic_flags(9, -1); break;
            case 1: dp(OP_EOR, 9, 10, 11, 0, 0); st_reg(9, d); logic_flags(9, -1); break;
            case 5: fl_load(); msr_nzcv(RFL); dp3(OP_ADCS, 9, 10, 11); fl_arith(); st_reg(9, d); break;
            case 6: fl_load(); msr_nzcv(RFL); dp3(OP_SBCS, 9, 10, 11); fl_arith(); st_reg(9, d); break;
            case 8: dp(OP_AND, 9, 10, 11, 0, 0); logic_flags(9, -1); break;
            case 9: dp(OP_SUBS, 9, WZR, 11, 0, 0); fl_arith(); st_reg(9, d); break;
            case 10: cmpr(10, 11); fl_arith(); break;
            case 11: dp(OP_ADDS, WZR, 10, 11, 0, 0); fl_arith(); break;
            case 12: dp(OP_ORR, 9, 10, 11, 0, 0); st_reg(9, d); logic_flags(9, -1); break;
            case 13: E(0x1B000000 | (11 << 16) | (WZR << 10) | (10 << 5) | 9); st_reg(9, d); logic_flags(9, -1); cost_extra(2); break;
            case 14: dp(OP_BIC, 9, 10, 11, 0, 0); st_reg(9, d); logic_flags(9, -1); break;
            default: dp(OP_ORN, 9, WZR, 11, 0, 0); st_reg(9, d); logic_flags(9, -1); break;
            }
            return 0;
        } else {
            int d = (i & 7) | ((i >> 4) & 8), m = (i >> 3) & 15;
            switch ((i >> 8) & 3) {
            case 0:
                ld_reg_pc(11, m, pc4);
                ld_reg_pc(10, d, pc4);
                dp(OP_ADD, 9, 10, 11, 0, 0);
                if (d == 15) { andi(9, 9, ~1u); st_reg(9, 15); emit_exit(1); return 1; }
                st_reg(9, d);
                return 0;
            case 1:
                ld_reg_pc(11, m, pc4); ld_reg_pc(10, d, pc4);
                cmpr(10, 11); fl_arith();
                return 0;
            case 2:
                ld_reg_pc(11, m, pc4);
                if (d == 15) { andi(11, 11, ~1u); st_reg(11, 15); emit_exit(1); return 1; }
                st_reg(11, d);
                return 0;
            default:
                emit_bx(m, i & 0x80, (pc + 2) | 1, 1, pc4);
                return 1;
            }
        }
    case 0x09: {
        uint32_t a = (pc4 & ~3u) + ((i & 0xFF) << 2);
        int d = (i >> 8) & 7;
        if (((a ^ pc) & ~0xFFFu) == 0 && !J.stored) { mov32(9, page_word(a)); st_reg(9, d); }
        else { mov32(23, a); emit_load(MK_W, 23, pc); st_reg(0, d); }
        cost_extra(1);
        break;
    }
    case 0x0A: case 0x0B: {
        int d = i & 7, k = (i >> 9) & 7;
        ld_reg(10, (i >> 3) & 7); ld_reg(11, (i >> 6) & 7);
        dp(OP_ADD, 23, 10, 11, 0, 0);
        if (k <= 2) { ld_reg(25, d); emit_store(k == 0 ? 4 : k == 1 ? 2 : 1, 23, 25, pc); break; }
        static const int kinds[] = { 0, 0, 0, MK_SB, MK_LDR, MK_H, MK_B, MK_SH };
        emit_load(kinds[k], 23, pc);
        st_reg(0, d);
        cost_extra(1);
        break;
    }
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: case 0x10: case 0x11: {
        int t = i >> 11, d = i & 7;
        uint32_t off = t <= 0x0D ? (i >> 4) & 0x7C : t <= 0x0F ? (i >> 6) & 31 : (i >> 5) & 0x3E;
        ld_reg(10, (i >> 3) & 7);
        addi(23, 10, off);
        if (t == 0x0C || t == 0x0E || t == 0x10) { ld_reg(25, d); emit_store(t == 0x0C ? 4 : t == 0x0E ? 1 : 2, 23, 25, pc); }
        else { emit_load(t == 0x0D ? MK_LDR : t == 0x0F ? MK_B : MK_H, 23, pc); st_reg(0, d); }
        break;
    }
    case 0x12: case 0x13: {
        int d = (i >> 8) & 7;
        ld_reg(10, 13);
        addi(23, 10, (i & 0xFF) << 2);
        if (i & 0x800) { emit_load(MK_LDR, 23, pc); st_reg(0, d); }
        else { ld_reg(25, d); emit_store(4, 23, 25, pc); }
        break;
    }
    case 0x14: mov32(9, (pc4 & ~3u) + ((i & 0xFF) << 2)); st_reg(9, (i >> 8) & 7); break;
    case 0x15: ld_reg(10, 13); addi(9, 10, (i & 0xFF) << 2); st_reg(9, (i >> 8) & 7); break;
    case 0x16: case 0x17:
        if ((i & 0xFF00) == 0xB000) {
            uint32_t o = (i & 0x7F) << 2;
            ld_reg(10, 13);
            if (i & 0x80) subi(10, 10, o); else addi(10, 10, o);
            st_reg(10, 13);
            break;
        }
        if ((i & 0x0600) == 0x0400) {
            uint32_t list = i & 0xFF;
            int extra = (i >> 8) & 1, cnt = __builtin_popcount(list) + extra;
            cost_extra(cnt);
            ld_reg(24, 13);
            if (i & 0x800) {    /* POP */
                if (extra) list |= 0x8000;
                movr(23, 24); addi(24, 24, cnt * 4);
                emit_xfer(1, list, cnt, 0, pc);
                st_reg(24, 13);
                if (extra) { ld_reg(9, 15); andi(9, 9, ~1u); st_reg(9, 15); emit_exit(1); return 1; }
            } else {
                if (extra) list |= 0x4000;
                subi(24, 24, cnt * 4); movr(23, 24);
                emit_xfer(0, list, cnt, 0, pc);
                st_reg(24, 13);
            }
            break;
        }
        return -1;
    case 0x18: case 0x19: {
        int rn = (i >> 8) & 7;
        uint32_t list = i & 0xFF;
        if (!list) return -1;
        int cnt = __builtin_popcount(list), L = (i >> 11) & 1;
        cost_extra(cnt);
        ld_reg(23, rn);
        addi(24, 23, cnt * 4);
        emit_xfer(L, list, cnt, 0, pc);
        if (!(L && (list & (1u << rn)))) st_reg(24, rn);
        break;
    }
    case 0x1A: case 0x1B: {
        uint32_t c = (i >> 8) & 15;
        if (c >= 14) return -1;
        fl_load();
        msr_nzcv(RFL);
        uint32_t *tk = bc(c);
        emit_exit_pc(pc + 2, 1);
        fix(tk);
        emit_exit_pc(pc4 + ((int32_t)(int8_t)(i & 0xFF) << 1), 1);
        return 1;
    }
    case 0x1C: emit_exit_pc(pc4 + (((int32_t)(i << 21)) >> 20), 1); return 1;
    case 0x1D: return -1;
    case 0x1E: mov32(9, pc4 + (((int32_t)(i << 21)) >> 9)); st_reg(9, 14); break;
    default:
        ld_reg(9, 14);
        addi(9, 9, (i & 0x7FF) << 1);
        mov32(10, (pc + 2) | 1); st_reg(10, 14);
        st_reg(9, 15);
        emit_exit(1);
        return 1;
    }
    return r;
}

static int tr_thumb_top(uint32_t pc, uint32_t i) {
    uint32_t *save_ep = ep;
    int save_loaded = J.fl_loaded, save_dirty = J.fl_dirty, save_pending = J.pending, save_nexits = J.nexits;
    uint32_t save_mask = J.mask;
    int r = tr_thumb(pc, i);
    if (r < 0) {
        ep = save_ep; J.fl_loaded = save_loaded; J.fl_dirty = save_dirty; J.pending = save_pending;
        J.nexits = save_nexits; J.mask = save_mask;
        fallback(pc, i, 1);
        uint32_t t = i >> 11;
        int leaves = t == 0x1D || (t == 0x1B && ((i >> 8) & 15) >= 14) || (t >= 0x16 && t <= 0x17 && (i & 0xFF00) == 0xBE00)
               || (t == 0x08 && (i & 0x400) && ((i >> 8) & 3) == 3);
        if (leaves) exit_jump();
        return leaves;
    }
    if (r == 0) J.pending += 1;
    return r;
}

/* ---------------------------------------------------------------- compile and dispatch */

static int hle_marked(uint32_t va) {
    if (va - 0x20000000u >= cpu.hle_span) return 0;
    uint32_t bi = (va - 0x20000000u) >> 1;
    return (cpu.hle_bitmap[bi >> 3] >> (bi & 7)) & 1;
}

static Block *compile(uint32_t pc, uint32_t pa, int thumb, int priv, uint8_t *page) {
    if (ep + 65536 > jend || nblocks >= MAXBLK) jit_flush_all();
    st_compiles++;
    memset(&J, 0, sizeof J);
    J.thumb = thumb; J.priv = priv; J.page = page; J.vpage = pc & ~0xFFFu;
    uint32_t *start = ep;
    J.pc0 = pc; J.pa0 = pa; J.start = start;
    uint32_t cur = pc;
    int n = 0;
    for (;;) {
        if (n > 0 && hle_marked(cur)) { emit_exit_pc(cur, 0); break; }
        if (n >= jit_maxlen || J.nexits > 400 || ep - start > 12000) { emit_exit_pc(cur, 0); break; }
        int ended;
        J.mask |= 1u << ((cur >> 7) & 31);
        if (thumb) { uint32_t i = *(uint16_t *)(page + (cur & 0xFFF)); ended = tr_thumb_top(cur, i); cur += 2; }
        else { uint32_t i = *(uint32_t *)(page + (cur & 0xFFF)); ended = tr_arm(cur, i); cur += 4; }
        n++;
        if (ended) break;
        if (((cur ^ pc) & ~0xFFFu) || (cur & 0xFFF) > (thumb ? 0xFFEu : 0xFFCu)) { emit_exit_pc(cur, 0); break; }
    }
    for (int k = 0; k < J.nexits; k++) fix_to(J.exits[k], jdispatch);
    if (getenv("JITBLK")) { char fn[64]; snprintf(fn, sizeof fn, "/tmp/blk_%08x_%d.bin", pc, thumb); FILE *f = fopen(fn, "wb"); if (f) { fwrite(start, 4, ep - start, f); fclose(f); } }
    __builtin___clear_cache((char *)start, (char *)ep);

    Block *b = &blocks[nblocks++];
    b->pa = pa; b->va = pc; b->thumb = thumb; b->priv = priv; b->valid = 1; b->code = start;
    Block **h = &htab[(pa >> 1) & (HT_N - 1)];
    b->hnext = *h; *h = b;
    PageInfo *p = pinfo(pa, 1);
    if (!p->mask) tag_tlbw(pa & ~0xFFFu, 1);
    p->mask |= J.mask;
    b->pnext = p->list; p->list = b;
    return b;
}

static void *jit_find(uint32_t pc, int thumb, int priv) {
    if (pc & (thumb ? 1 : 3)) return NULL;          /* odd fetch addresses: leave them to the interpreter */
    VEnt *e = &vc[(pc >> 1) & (VC_N - 1)];
    uint32_t tag = vgen << 2 | thumb << 1 | priv;
    if (e->pc == pc && e->tag == tag) return e->code;
    TlbEnt *t = cpu.tlb_r[priv];
    uint32_t pa; uint8_t *page;
    if (TLB_HIT(t, pc)) { TlbEnt *x = &t[(pc >> 12) & (TLB_N - 1)]; pa = x->pa | (pc & 0xFFF); page = x->host; }
    else {
        uint32_t fsr;
        uint8_t *h = tlb_fill(pc, 0, priv, &pa, &fsr);
        if (getenv("JITWHY") && (fsr || !h)) { static int n; if (n++ < 5) fprintf(stderr, "jit_find %08x: fsr=%x h=%p pa=%08x\n", pc, fsr, (void *)h, pa); }
        if (fsr || !h) return NULL;
        page = h - (pa & 0xFFF);
    }
    Block *b;
    for (b = htab[(pa >> 1) & (HT_N - 1)]; b; b = b->hnext)
        if (b->pa == pa && b->va == pc && b->thumb == thumb && b->priv == priv && b->valid) break;
    if (!b) b = compile(pc, pa, thumb, priv, page);
    e = &vc[(pc >> 1) & (VC_N - 1)];
    e->pc = pc; e->tag = vgen << 2 | thumb << 1 | priv; e->code = b->code;
    return b->code;
}

static void jit_init(void) {
    if (jit_ok) return;
    const char *m = getenv("JITMAX");
    if (m) jit_maxlen = atoi(m) > 0 ? atoi(m) : 1;
    if (getenv("NOJIT")) cpu_jit = 0;
    if (getenv("JITTRACE")) jit_trace = atoi(getenv("JITTRACE"));
    void *mem = mmap(NULL, CODE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) { fprintf(stderr, "jit: no executable memory, using the interpreter\n"); cpu_jit = 0; return; }
    blocks = calloc(MAXBLK, sizeof(Block));
    jbuf = ep = mem; jend = jbuf + CODE_SZ / 4;
    /* trampoline: jit_enter(cpu, code, tlb_r, tlb_w). Blocks run inside its frame and leave
     * through the dispatcher, which returns from here. */
    E(0xA9BA7BFD);                       /* stp x29, x30, [sp, #-96]! */
    E(0x910003FD);                       /* mov x29, sp */
    E(0xA90153F3);                       /* stp x19, x20, [sp, #16] */
    E(0xA9025BF5);                       /* stp x21, x22, [sp, #32] */
    E(0xA90363F7);                       /* stp x23, x24, [sp, #48] */
    E(0xA9046BF9);                       /* stp x25, x26, [sp, #64] */
    E(0xAA0003F3);                       /* mov x19, x0 */
    E(0xAA0203F5);                       /* mov x21, x2 */
    E(0xAA0303F6);                       /* mov x22, x3 */
    mov64(26, (uint64_t)(uintptr_t)&jit_stop);
    E(0xD61F0020);                       /* br x1 */
    uint32_t *out = ep;
    E(0xA9446BF9);                       /* ldp x25, x26, [sp, #64] */
    E(0xA94363F7);                       /* ldp x23, x24, [sp, #48] */
    E(0xA9425BF5);                       /* ldp x21, x22, [sp, #32] */
    E(0xA94153F3);                       /* ldp x19, x20, [sp, #16] */
    E(0xA8C67BFD);                       /* ldp x29, x30, [sp], #96 */
    E(0xD65F03C0);                       /* ret */
    /* dispatcher: r15 holds the next pc */
    jdispatch = ep;
    uint32_t *o[12]; int no = 0;
    ldrx(9, RCPU, OFF(budget));
    E(0xF100013F);                       /* cmp x9, #0 */
    o[no++] = bc(C_LE);
    ldrw(9, RCPU, OFF(int_pending)); o[no++] = cbnzw(9);
    ldrw(9, RCPU, OFF(halted)); o[no++] = cbnzw(9);
    ldrw(9, 26, 0); o[no++] = cbnzw(9);
    ld_reg(9, 15);
    /* HLE hooks: leave if the bitmap marks this pc */
    mov32(12, OFF(hle_hook)); addx_sh(12, RCPU, 12, 0);   /* hle fields lie past the TLBs */
    ldrw(10, 12, OFF(hle_span) - OFF(hle_hook));
    ldrx(12, 12, OFF(hle_bitmap) - OFF(hle_hook));
    mov32(11, 0x20000000u);
    dp(OP_SUB, 11, 9, 11, 0, 0);
    cmpr(11, 10);
    uint32_t *nohle = bc(C_CS);
    lsri(11, 11, 1);
    lsri(13, 11, 3);
    E(0x38606800 | (13 << 16) | (12 << 5) | 13);          /* ldrb w13, [x12, x13] */
    andi(14, 11, 7);
    E(0x1AC02400 | (14 << 16) | (13 << 5) | 13);          /* lsrv w13, w13, w14 */
    o[no++] = tbz(13, 0); { uint32_t *t = o[no - 1]; *t |= 0x01000000; }  /* tbnz: leave if marked */
    fix(nohle);
    /* the MMU-switch window runs on the interpreter */
    mov64(12, (uint64_t)(uintptr_t)&pf_lo); ldrw(13, 12, 0);
    mov64(12, (uint64_t)(uintptr_t)&pf_hi); ldrw(14, 12, 0);
    dp(OP_SUB, 15, 9, 13, 0, 0);
    dp(OP_SUB, 16, 14, 13, 0, 0);
    cmpr(15, 16);
    o[no++] = bc(C_CC);
    /* lookup: tag = vgen << 2 | thumb << 1 | priv */
    ldrw(12, RCPU, OFF(cpsr));
    ubfx(13, 12, 5, 1);
    andi(14, 12, 0x1F);
    cmpi(14, 0x10);
    E(0x1A9F07E0 | 14);                  /* cset w14, ne */
    mov64(15, (uint64_t)(uintptr_t)&vgen); ldrw(15, 15, 0);
    lsli(15, 15, 2);
    dp(OP_ORR, 15, 15, 13, SH_LSL, 1);
    dp(OP_ORR, 15, 15, 14, 0, 0);
    ubfx(16, 9, 1, 14);
    mov64(17, (uint64_t)(uintptr_t)vc);
    addx_sh(17, 17, 16, 4);
    ldpw(10, 11, 17, 0);
    cmpr(10, 9);
    o[no++] = bc(C_NE);
    cmpr(11, 15);
    uint32_t *miss = bc(C_NE);
    ldrx(16, 17, 8);
    mov32(10, OFF(tlb_r)); addx_sh(RTR, RCPU, 10, 0); E(0x8B000000 | (14 << 16) | (16 << 10) | (RTR << 5) | RTR); /* add x21, x21, x14, lsl #16 */
    mov32(10, OFF(tlb_w)); addx_sh(RTW, RCPU, 10, 0); E(0x8B000000 | (14 << 16) | (16 << 10) | (RTW << 5) | RTW);
    E(0xD61F0200);                       /* br x16 */
    for (int k = 0; k < no; k++) fix_to(o[k], out);
    fix_to(miss, out);
    __builtin___clear_cache((char *)jbuf, (char *)ep);
    jit_enter = (void (*)(Cpu *, void *, TlbEnt *, TlbEnt *))(void *)jbuf;
    if (ep > jbuf + 256) { fprintf(stderr, "jit: dispatcher too big\n"); return; }
    if (getenv("JITDUMP")) { FILE *f = fopen(getenv("JITDUMP"), "wb"); if (f) { fwrite(jbuf, 4, ep - jbuf, f); fclose(f); } }
    ep = jbuf + 256;
    jit_ok = 1;
}

int cpu_jit_available(void) { return jit_ok; }        /* set up by cpu_reset */

void cpu_jit_stats(void) {
    fprintf(stderr, "jit: %llu blocks compiled, %llu page invalidations, %llu flushes, %llu entries, %llu interpreted, %d live blocks, %ld KB code\n",
            (unsigned long long)st_compiles, (unsigned long long)st_invals, (unsigned long long)st_flushes,
            (unsigned long long)st_enter, (unsigned long long)st_interp, nblocks, (long)((ep - jbuf) * 4 / 1024));
}



/* debugging: describe the translated block holding host address p */
#include <signal.h>
#include <ucontext.h>
#include <unistd.h>
static void jit_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uint32_t *p = (uint32_t *)uc->uc_mcontext.pc;
    fprintf(stderr, "jit: signal %d at %p (buffer %p..%p) addr %p\n", sig, (void *)p, (void *)jbuf, (void *)ep, si->si_addr);
    for (int i = nblocks - 1; i >= 0; i--) if ((uint32_t *)blocks[i].code <= p && p < ep) {
        fprintf(stderr, "jit: in block %d pa=%08x thumb=%d offset %ld\n", i, blocks[i].pa, blocks[i].thumb, (long)(p - (uint32_t *)blocks[i].code));
        uint32_t *end = i + 1 < nblocks ? (uint32_t *)blocks[i + 1].code : ep;
        FILE *f = fopen("/tmp/crash_blk.bin", "wb"); if (f) { fwrite(blocks[i].code, 4, end - (uint32_t *)blocks[i].code, f); fclose(f); }
        break;
    }
    _exit(99);
}
void cpu_jit_debug_signals(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = jit_crash; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}

static int cpu_run_jit(int cycles) {
    cpu.budget = cycles;
    cpu.run_len = cycles;
    while (cpu.budget > 0) {
        if (cpu.int_pending) {
            uint32_t ret = cpu.r[15] + 4;
            if (cpu.fiq_line && !(cpu.cpsr & F_BIT)) take_exception(0x1C, M_FIQ, ret, 1);
            else take_exception(0x18, M_IRQ, ret, 0);
        }
        if (cpu.halted) { cpu.budget = 0; break; }
        uint32_t pc = cpu.r[15];
        cpu.abort = 0;
        if (pc - 0x20000000u < cpu.hle_span) {
            uint32_t bi = (pc - 0x20000000u) >> 1;
            if ((cpu.hle_bitmap[bi >> 3] >> (bi & 7)) & 1) {
                cpu.hle_hook(pc);
                if (cpu.r[15] != pc) { cpu.budget -= 1; continue; }
            }
        }
        if (!(pc - pf_lo < pf_hi - pf_lo)) {
            int thumb = (cpu.cpsr & T_BIT) != 0, priv = is_priv();
            void *code = jit_find(pc, thumb, priv);
            if (code) {
                st_enter++; jit_stop = 0;
                if (jit_trace) {   /* one block at a time, logged like the interpreter's trace */
                    jit_trace--; jit_stop = 1;
                    uint32_t w = 0; cpu_vread32(pc & ~3u, &w);
                    if (thumb) fprintf(stderr, "T %08x: %04x  r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n", pc, (pc & 2) ? w >> 16 : w & 0xFFFF, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[13], cpu.r[14]);
                    else fprintf(stderr, "A %08x: %08x  r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n", pc, w, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[13], cpu.r[14]);
                }
                jit_enter(&cpu, code, cpu.tlb_r[priv], cpu.tlb_w[priv]);
                continue;
            }
        }
        st_interp++;
        if (getenv("JITWHY") && st_interp % 1000000 == 1) fprintf(stderr, "interp pc=%08x cpsr=%x pf=%08x-%08x ctrl=%x\n", pc, cpu.cpsr, pf_lo, pf_hi, cpu.ctrl);
        interp_step(pc);
    }
    int done = (int)(cpu.run_len - cpu.budget);
    cpu.cycles += done;
    cpu.run_len = cpu.budget = 0;
    return done;
}

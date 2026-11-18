#include <stddef.h>
/* ZodiacDroid - ARM920T CPU core: ARMv4T ARM + Thumb interpreter, CP15, MMU with a soft TLB.
 * Written from the ARM Architecture Reference Manual; no code from other emulators. */
#include "cpu.h"
#include "state.h"
#include <string.h>
#include <stdio.h>

Cpu cpu;

#if defined(__aarch64__) && !defined(NOJIT)
#define ZJIT 1
static void jit_tlb_flushed(void);
static void jit_code_write(uint32_t pa);
static int jit_page_code(uint32_t pa);
static void jit_flush_all(void);
static void jit_init(void);
static int cpu_run_jit(int cycles);
static int jit_ok;
#else
int cpu_jit = 0;
void cpu_code_changed(void) {}
void cpu_phys_written(uint32_t pa, uint32_t len) { (void)pa; (void)len; }
int cpu_jit_available(void) { return 0; }
#endif

#define T_BIT 0x20
#define F_BIT 0x40
#define I_BIT 0x80

static inline int bank_idx(uint32_t m) {
    switch (m & 0x1F) {
    case M_SVC: return 1; case M_ABT: return 2; case M_UND: return 3;
    case M_IRQ: return 4; case M_FIQ: return 5; default: return 0;
    }
}

static void update_pending(void) {
    cpu.int_pending = (cpu.irq_line && !(cpu.cpsr & I_BIT)) || (cpu.fiq_line && !(cpu.cpsr & F_BIT));
}

static void switch_mode(uint32_t nm) {
    uint32_t om = cpu.cpsr & 0x1F;
    nm &= 0x1F;
    if (om == nm) return;
    int oi = bank_idx(om), ni = bank_idx(nm);
    if (oi != ni) {
        cpu.bank_r13[oi] = cpu.r[13]; cpu.bank_r14[oi] = cpu.r[14]; cpu.bank_spsr[oi] = cpu.spsr;
        if ((om == M_FIQ) != (nm == M_FIQ)) {
            if (om == M_FIQ) { memcpy(cpu.bank_fiq, &cpu.r[8], 20); memcpy(&cpu.r[8], cpu.bank_usr, 20); }
            else             { memcpy(cpu.bank_usr, &cpu.r[8], 20); memcpy(&cpu.r[8], cpu.bank_fiq, 20); }
        }
        cpu.r[13] = cpu.bank_r13[ni]; cpu.r[14] = cpu.bank_r14[ni]; cpu.spsr = cpu.bank_spsr[ni];
    }
    cpu.cpsr = (cpu.cpsr & ~0x1F) | nm;
    /* privilege may have changed: TLB is indexed per privilege so nothing to flush */
}

uint32_t cpu_get_cpsr(void) {
    return (cpu.n << 31) | (cpu.z << 30) | (cpu.c << 29) | (cpu.v << 28) | (cpu.cpsr & 0xFF);
}

void cpu_set_cpsr(uint32_t v) {
    cpu.n = v >> 31; cpu.z = (v >> 30) & 1; cpu.c = (v >> 29) & 1; cpu.v = (v >> 28) & 1;
    switch_mode(v & 0x1F);
    cpu.cpsr = (v & 0xFF) | 0x10;
    update_pending();
}

void cpu_set_irq(int l) { cpu.irq_line = l; update_pending(); if (l) cpu.halted = 0; }
void cpu_set_fiq(int l) { cpu.fiq_line = l; update_pending(); if (l) cpu.halted = 0; }

void cpu_tlb_flush(void) {
    memset(cpu.tlb_r, 0xFF, sizeof cpu.tlb_r); memset(cpu.tlb_w, 0xFF, sizeof cpu.tlb_w);
#ifdef ZJIT
    jit_tlb_flushed();
#endif
}

void cpu_reset(void) {
    memset(&cpu.r, 0, sizeof cpu.r);
    cpu.n = cpu.z = cpu.c = cpu.v = 0;
    memset(cpu.bank_r13, 0, sizeof cpu.bank_r13);
    memset(cpu.bank_r14, 0, sizeof cpu.bank_r14);
    cpu.cpsr = M_SVC | I_BIT | F_BIT;
    cpu.ctrl = 0x78; cpu.ttb = 0; cpu.dacr = 0; cpu.dfsr = cpu.ifsr = cpu.far = cpu.pid = 0;
    cpu.halted = 0; cpu.abort = 0;
    cpu.r[15] = 0;
    cpu_tlb_flush();
    update_pending();
#ifdef ZJIT
    jit_init();
    jit_flush_all();
#endif
}

/* ---------------------------------------------------------------- MMU */

static inline int is_priv(void) { return (cpu.cpsr & 0x1F) != M_USR; }

/* Translate. Returns 0 on success (fills *pa) or the FSR value (domain<<4 | status). */
static uint32_t translate(uint32_t va, int write, int priv, uint32_t *pa) {
    if (!(cpu.ctrl & 1)) { *pa = va; return 0; }
    if (va < 0x02000000) va |= cpu.pid;
    uint32_t l1a = (cpu.ttb & 0xFFFFC000) | ((va >> 18) & 0x3FFC);
    uint8_t *p = bus_page(l1a, 0);
    uint32_t d1 = p ? *(uint32_t *)(p + (l1a & 0xFFF)) : bus_io_read(l1a, 4);
    uint32_t dom = (d1 >> 5) & 15, ap, ty = d1 & 3, page = 1, phys;
    if (ty == 0) return 0x5;
    if (ty == 2) {               /* section */
        page = 0;
        ap = (d1 >> 10) & 3;
        phys = (d1 & 0xFFF00000) | (va & 0x000FFFFF);
    } else {
        uint32_t l2a, d2;
        if (ty == 1) l2a = (d1 & 0xFFFFFC00) | ((va >> 10) & 0x3FC);       /* coarse */
        else         l2a = (d1 & 0xFFFFF000) | ((va >> 8) & 0xFFC);        /* fine */
        p = bus_page(l2a, 0);
        d2 = p ? *(uint32_t *)(p + (l2a & 0xFFF)) : bus_io_read(l2a, 4);
        switch (d2 & 3) {
        case 0: return (dom << 4) | 0x7;
        case 1: ap = (d2 >> (4 + ((va >> 13) & 6))) & 3; phys = (d2 & 0xFFFF0000) | (va & 0xFFFF); break; /* large 64K */
        case 2: ap = (d2 >> (4 + ((va >> 9) & 6))) & 3;  phys = (d2 & 0xFFFFF000) | (va & 0xFFF); break;  /* small 4K */
        default: ap = (d2 >> 4) & 3; phys = (d2 & 0xFFFFFC00) | (va & 0x3FF); break;                       /* tiny 1K */
        }
    }
    uint32_t dacc = (cpu.dacr >> (dom * 2)) & 3;
    if (dacc == 0 || dacc == 2) return (dom << 4) | (page ? 0xB : 0x9);
    if (dacc == 1) {
        int ok;
        switch (ap) {
        case 0: {
            int s = (cpu.ctrl >> 8) & 1, r = (cpu.ctrl >> 9) & 1;
            if (write) ok = 0;
            else if (s && priv) ok = 1;
            else ok = r;
            break;
        }
        case 1: ok = priv; break;
        case 2: ok = priv || !write; break;
        default: ok = 1;
        }
        if (!ok) return (dom << 4) | (page ? 0xF : 0xD);
    }
    *pa = phys;
    return 0;
}

/* slow path: walk, fill TLB. returns host pointer for va, or NULL if I/O (then *pa set) or fault (cpu.abort set). */
static uint8_t *tlb_fill(uint32_t va, int write, int priv, uint32_t *pa_out, uint32_t *fsr) {
    uint32_t pa;
    uint32_t f = translate(va, write, priv, &pa);
    if (f) { *fsr = f; return (uint8_t *)1; }
    *fsr = 0;
    uint8_t *h = bus_page(pa, write);
    /* Only cache pages whose translation covers the whole 4K (not tiny pages; 1K is rare). */
    uint32_t idx = (va >> 12) & (TLB_N - 1);
    TlbEnt *e = write ? &cpu.tlb_w[priv][idx] : &cpu.tlb_r[priv][idx];
    e->tag = va >> 12;
    e->pa = pa & ~0xFFFu;
    e->host = h;   /* bus_page returns the page base */
#ifdef ZJIT
    if (write && h && jit_ok && jit_page_code(pa)) e->tag |= 0x80000000u;   /* stores must check for code */
#endif
    *pa_out = pa;
    return h ? h + (pa & 0xFFF) : NULL;
}

static void data_abort(uint32_t va, uint32_t fsr) {
    if (!cpu.abort) { cpu.abort = 1; cpu.dfsr = fsr; cpu.far = va; }
}

static uint32_t rd_slow(uint32_t va, int size, int priv) {
    uint32_t pa, fsr;
    if ((cpu.ctrl & 2) && (va & (size - 1))) { data_abort(va, 1); return 0; }
    uint8_t *h = tlb_fill(va, 0, priv, &pa, &fsr);
    if (fsr) { data_abort(va, fsr); return 0; }
    if (h) {
        if (size == 4) return *(uint32_t *)((uintptr_t)h & ~(uintptr_t)3);
        if (size == 2) return *(uint16_t *)((uintptr_t)h & ~(uintptr_t)1);
        return *h;
    }
    return bus_io_read(pa & ~(uint32_t)(size - 1), size);
}

static void wr_slow(uint32_t va, uint32_t v, int size, int priv) {
    uint32_t pa, fsr;
    if ((cpu.ctrl & 2) && (va & (size - 1))) { data_abort(va, 1); return; }
    uint8_t *h;
#ifdef ZJIT
    TlbEnt *ce = &cpu.tlb_w[priv][(va >> 12) & (TLB_N - 1)];
    if (ce->tag == ((va >> 12) | 0x80000000u) && ce->host) {   /* page holding translated code */
        pa = ce->pa | (va & 0xFFF);
        h = ce->host + (va & 0xFFF);
        jit_code_write(pa);
    } else
#endif
    {
        h = tlb_fill(va, 1, priv, &pa, &fsr);
        if (fsr) { data_abort(va, fsr); return; }
#ifdef ZJIT
        if (h && jit_ok) jit_code_write(pa);
#endif
    }
    if (h) {
        if (size == 4) *(uint32_t *)((uintptr_t)h & ~(uintptr_t)3) = v;
        else if (size == 2) *(uint16_t *)((uintptr_t)h & ~(uintptr_t)1) = (uint16_t)v;
        else *h = (uint8_t)v;
        return;
    }
    bus_io_write(pa & ~(uint32_t)(size - 1), v, size);
}

#define TLB_HIT(tab, va) ((tab)[((va) >> 12) & (TLB_N - 1)].tag == ((va) >> 12) && (tab)[((va) >> 12) & (TLB_N - 1)].host)
#define TLB_HOST(tab, va) ((tab)[((va) >> 12) & (TLB_N - 1)].host + ((va) & 0xFFF))

static inline uint32_t rd32p(uint32_t va, int priv) {
    TlbEnt *t = cpu.tlb_r[priv];
    if (TLB_HIT(t, va) && !(va & 3)) return *(uint32_t *)TLB_HOST(t, va);
    return rd_slow(va, 4, priv);
}
static inline uint32_t rd16(uint32_t va) {
    int p = is_priv(); TlbEnt *t = cpu.tlb_r[p];
    if (TLB_HIT(t, va) && !(va & 1)) return *(uint16_t *)TLB_HOST(t, va);
    return rd_slow(va, 2, p);
}
static inline uint32_t rd8p(uint32_t va, int priv) {
    TlbEnt *t = cpu.tlb_r[priv];
    if (TLB_HIT(t, va)) return *TLB_HOST(t, va);
    return rd_slow(va, 1, priv);
}
static inline void wr32p(uint32_t va, uint32_t v, int priv) {
    TlbEnt *t = cpu.tlb_w[priv];
    if (TLB_HIT(t, va) && !(va & 3)) { *(uint32_t *)TLB_HOST(t, va) = v; return; }
    wr_slow(va, v, 4, priv);
}
static inline void wr16(uint32_t va, uint32_t v) {
    int p = is_priv(); TlbEnt *t = cpu.tlb_w[p];
    if (TLB_HIT(t, va) && !(va & 1)) { *(uint16_t *)TLB_HOST(t, va) = (uint16_t)v; return; }
    wr_slow(va, v, 2, p);
}
static inline void wr8p(uint32_t va, uint32_t v, int priv) {
    TlbEnt *t = cpu.tlb_w[priv];
    if (TLB_HIT(t, va)) { *TLB_HOST(t, va) = (uint8_t)v; return; }
    wr_slow(va, v, 1, priv);
}
#define rd32(a) rd32p((a), is_priv())
#define rd8(a)  rd8p((a), is_priv())
#define wr32(a, v) wr32p((a), (v), is_priv())
#define wr8(a, v)  wr8p((a), (v), is_priv())

/* LDR semantics: unaligned word loads rotate */
static inline uint32_t ldr_word(uint32_t a, int priv) {
    uint32_t v = rd32p(a & ~3u, priv);
    uint32_t s = (a & 3) * 8;
    return s ? (v >> s) | (v << (32 - s)) : v;
}

/* instruction fetch; returns 0 and sets *fault on prefetch abort */
static inline int fetch32(uint32_t va, uint32_t *out) {
    int p = is_priv(); TlbEnt *t = cpu.tlb_r[p];
    if (TLB_HIT(t, va)) { *out = *(uint32_t *)TLB_HOST(t, va); return 1; }
    uint32_t pa, fsr; uint8_t *h = tlb_fill(va, 0, p, &pa, &fsr);
    if (fsr) { cpu.ifsr = fsr; return 0; }
    *out = h ? *(uint32_t *)h : bus_io_read(pa, 4);
    return 1;
}
static inline int fetch16(uint32_t va, uint32_t *out) {
    int p = is_priv(); TlbEnt *t = cpu.tlb_r[p];
    if (TLB_HIT(t, va)) { *out = *(uint16_t *)TLB_HOST(t, va); return 1; }
    uint32_t pa, fsr; uint8_t *h = tlb_fill(va, 0, p, &pa, &fsr);
    if (fsr) { cpu.ifsr = fsr; return 0; }
    *out = h ? *(uint16_t *)h : bus_io_read(pa, 2);
    return 1;
}

int cpu_vread32(uint32_t va, uint32_t *out) {
    uint32_t pa; if (translate(va, 0, 1, &pa)) return 0;
    uint8_t *h = bus_page(pa, 0);
    *out = h ? *(uint32_t *)(h + (pa & 0xFFC)) : bus_io_read(pa & ~3u, 4);
    return 1;
}
int cpu_vread8(uint32_t va, uint8_t *out) {
    uint32_t pa; if (translate(va, 0, 1, &pa)) return 0;
    uint8_t *h = bus_page(pa, 0);
    *out = h ? h[pa & 0xFFF] : (uint8_t)bus_io_read(pa, 1);
    return 1;
}
int cpu_vwrite32(uint32_t va, uint32_t v) {
    uint32_t pa; if (translate(va, 1, 1, &pa)) return 0;
    uint8_t *h = bus_page(pa, 1);
    cpu_phys_written(pa & ~3u, 4);
    if (h) *(uint32_t *)(h + (pa & 0xFFC)) = v; else bus_io_write(pa & ~3u, v, 4);
    return 1;
}
int cpu_vwrite8(uint32_t va, uint8_t v) {
    uint32_t pa; if (translate(va, 1, 1, &pa)) return 0;
    uint8_t *h = bus_page(pa, 1);
    cpu_phys_written(pa, 1);
    if (h) h[pa & 0xFFF] = v; else bus_io_write(pa, v, 1);
    return 1;
}

/* ---------------------------------------------------------------- exceptions */

static void take_exception(uint32_t vec, uint32_t mode, uint32_t lr, int dis_fiq) {
    uint32_t old = cpu_get_cpsr();
    switch_mode(mode);
    cpu.spsr = old;
    cpu.r[14] = lr;
    cpu.cpsr = (cpu.cpsr & ~(T_BIT)) | I_BIT | (dis_fiq ? F_BIT : 0);
    cpu.r[15] = ((cpu.ctrl & 0x2000) ? 0xFFFF0000 : 0) + vec;
    cpu.halted = 0;
    update_pending();
}

/* ---------------------------------------------------------------- CP15 */

static uint32_t pf_lo = 0, pf_hi = 0, pf_ctrl;   /* old-mapping fetch window after toggling the MMU */

static uint32_t cp15_read(uint32_t crn, uint32_t op2, uint32_t crm) {
    switch (crn) {
    case 0: return op2 == 1 ? 0x0D172172 : 0x41129200;   /* cache type / ARM920T id */
    case 1: return cpu.ctrl;
    case 2: return cpu.ttb;
    case 3: return cpu.dacr;
    case 5: return op2 == 1 ? cpu.ifsr : cpu.dfsr;
    case 6: return cpu.far;
    case 13: return cpu.pid;
    default: return cpu.cp15_misc[crn];
    }
}

static void cp15_write(uint32_t crn, uint32_t op2, uint32_t crm, uint32_t v) {
    switch (crn) {
    case 1:
        /* The two instructions after the MCR are already in the pipeline and were
         * fetched with the old mapping; the boot code relies on that. */
        if ((v ^ cpu.ctrl) & 1) { pf_lo = cpu.r[15] - 4; pf_hi = pf_lo + 8; pf_ctrl = cpu.ctrl; }
        if ((v ^ cpu.ctrl) & 0x2307) cpu_tlb_flush();
        cpu.ctrl = v | 0x70;
        break;
    case 2: cpu.ttb = v; cpu_tlb_flush(); break;
    case 3: cpu.dacr = v; cpu_tlb_flush(); break;
    case 5: if (op2 == 1) cpu.ifsr = v; else cpu.dfsr = v; break;
    case 6: cpu.far = v; break;
    case 7:
        if (crm == 0 && op2 == 4) cpu.halted = 1;       /* wait for interrupt */
        break;                                          /* cache ops: nothing to do */
    case 8: cpu_tlb_flush(); break;
    case 13: cpu.pid = v & 0xFE000000; cpu_tlb_flush(); break;
    default: cpu.cp15_misc[crn] = v; break;
    }
}

/* ---------------------------------------------------------------- helpers */

static inline int cond_pass(uint32_t c) {
    switch (c) {
    case 0x0: return cpu.z;
    case 0x1: return !cpu.z;
    case 0x2: return cpu.c;
    case 0x3: return !cpu.c;
    case 0x4: return cpu.n;
    case 0x5: return !cpu.n;
    case 0x6: return cpu.v;
    case 0x7: return !cpu.v;
    case 0x8: return cpu.c && !cpu.z;
    case 0x9: return !cpu.c || cpu.z;
    case 0xA: return cpu.n == cpu.v;
    case 0xB: return cpu.n != cpu.v;
    case 0xC: return !cpu.z && cpu.n == cpu.v;
    case 0xD: return cpu.z || cpu.n != cpu.v;
    default: return 1;
    }
}

static inline void set_nz(uint32_t r) { cpu.n = r >> 31; cpu.z = r == 0; }

static inline uint32_t add_flags(uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t r = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)r;
    cpu.c = (uint32_t)(r >> 32);
    cpu.v = ((~(a ^ b) & (a ^ res)) >> 31) & 1;
    set_nz(res);
    return res;
}
static inline uint32_t sub_flags(uint32_t a, uint32_t b, uint32_t cin) { /* a - b - !cin */
    return add_flags(a, ~b, cin);
}

/* shifter: shift value v by type t, amount amt. imm: amt came from immediate field. updates *co. */
static inline uint32_t shift(uint32_t v, uint32_t t, uint32_t amt, int imm, uint32_t *co) {
    switch (t) {
    case 0: /* LSL */
        if (amt == 0) return v;
        if (amt < 32) { *co = (v >> (32 - amt)) & 1; return v << amt; }
        *co = amt == 32 ? (v & 1) : 0; return 0;
    case 1: /* LSR */
        if (amt == 0) { if (!imm) return v; amt = 32; }
        if (amt < 32) { *co = (v >> (amt - 1)) & 1; return v >> amt; }
        *co = amt == 32 ? v >> 31 : 0; return 0;
    case 2: /* ASR */
        if (amt == 0) { if (!imm) return v; amt = 32; }
        if (amt < 32) { *co = (v >> (amt - 1)) & 1; return (uint32_t)((int32_t)v >> amt); }
        *co = v >> 31; return (uint32_t)((int32_t)v >> 31);
    default: /* ROR / RRX */
        if (amt == 0) {
            if (!imm) return v;
            uint32_t r = (*co << 31) | (v >> 1); *co = v & 1; return r;
        }
        amt &= 31;
        if (amt == 0) { *co = v >> 31; return v; }
        *co = (v >> (amt - 1)) & 1;
        return (v >> amt) | (v << (32 - amt));
    }
}

static int branched;
static void undefined(uint32_t pc, uint32_t insn, int thumb) {
    if (cpu.undef_hook) cpu.undef_hook(pc, insn, thumb);
    take_exception(0x04, M_UND, pc + (thumb ? 2 : 4), 0);
    branched = 1;
}

/* ---------------------------------------------------------------- ARM */

static inline uint32_t reg_r(int n, uint32_t pc8) { return n == 15 ? pc8 : cpu.r[n]; }

static inline void set_pc(uint32_t v) { cpu.r[15] = v; branched = 1; }

static void arm_ldm_stm(uint32_t insn, uint32_t pc) {
    int rn = (insn >> 16) & 15;
    uint32_t list = insn & 0xFFFF;
    int L = (insn >> 20) & 1, W = (insn >> 21) & 1, S = (insn >> 22) & 1, U = (insn >> 23) & 1, P = (insn >> 24) & 1;
    int cnt = __builtin_popcount(list);
    uint32_t base = cpu.r[rn];
    uint32_t addr, wb;
    if (cnt == 0) { /* v4: empty list transfers r15, base +/- 0x40 */
        list = 0x8000; cnt = 16;
    }
    if (U) { addr = base + (P ? 4 : 0); wb = base + cnt * 4; }
    else   { addr = base - cnt * 4 + (P ? 0 : 4); wb = base - cnt * 4; }
    if (cnt == 16 && (insn & 0xFFFF) == 0) { wb = U ? base + 0x40 : base - 0x40; }
    int user_bank = S && !(L && (list & 0x8000));
    uint32_t mode = cpu.cpsr & 0x1F;
    int priv = is_priv();
    cpu.budget -= cnt;
    if (L) {
        uint32_t vals[16];
        uint32_t a = addr;
        for (int i = 0; i < 16; i++) if (list & (1 << i)) { vals[i] = rd32p(a & ~3u, priv); a += 4; }
        if (cpu.abort) return;
        if (W) cpu.r[rn] = wb;
        if (user_bank && mode != M_USR && mode != M_SYS) {
            /* write user-bank registers */
            for (int i = 0; i < 15; i++) if (list & (1 << i)) {
                if (i < 8) cpu.r[i] = vals[i];
                else if (i < 13) { if (mode == M_FIQ) cpu.bank_usr[i - 8] = vals[i]; else cpu.r[i] = vals[i]; }
                else if (i == 13) cpu.bank_r13[0] = vals[i];
                else cpu.bank_r14[0] = vals[i];
            }
        } else {
            for (int i = 0; i < 15; i++) if (list & (1 << i)) cpu.r[i] = vals[i];
        }
        if (list & 0x8000) {
            uint32_t v = vals[15];
            if (S) {
                cpu_set_cpsr(cpu.spsr);
                set_pc(v & ((cpu.cpsr & T_BIT) ? ~1u : ~3u));
            } else set_pc(v & ~3u);   /* ARMv4: no interworking on LDM */
        }
    } else {
        uint32_t a = addr;
        int first = 1;
        for (int i = 0; i < 16; i++) if (list & (1 << i)) {
            uint32_t v;
            if (i == 15) v = pc + 12;
            else if (user_bank && mode != M_USR && mode != M_SYS && i >= 8) {
                if (i < 13) v = mode == M_FIQ ? cpu.bank_usr[i - 8] : cpu.r[i];
                else if (i == 13) v = cpu.bank_r13[0];
                else v = cpu.bank_r14[0];
            } else v = cpu.r[i];
            (void)first;
            wr32p(a & ~3u, v, priv);
            a += 4; first = 0;
        }
        if (W && !cpu.abort) cpu.r[rn] = wb;
    }
}

static void arm_step(uint32_t pc, uint32_t insn) {
    uint32_t pc8 = pc + 8;
    cpu.r[15] = pc8;
    uint32_t op = (insn >> 25) & 7;
    switch (op) {
    case 0: case 1: {
        /* misc / multiply / halfword / data processing */
        if (op == 0 && (insn & 0x90) == 0x90) {
            uint32_t sh = (insn >> 5) & 3;
            if (sh == 0) {
                if ((insn & 0x0FC000F0) == 0x00000090) {  /* MUL/MLA */
                    int rd = (insn >> 16) & 15, rn = (insn >> 12) & 15, rs = (insn >> 8) & 15, rm = insn & 15;
                    uint32_t r = cpu.r[rm] * cpu.r[rs];
                    if (insn & (1 << 21)) r += cpu.r[rn];
                    cpu.r[rd] = r;
                    if (insn & (1 << 20)) set_nz(r);
                    cpu.budget -= 2;
                    return;
                }
                if ((insn & 0x0F8000F0) == 0x00800090) {  /* long multiply */
                    int hi = (insn >> 16) & 15, lo = (insn >> 12) & 15, rs = (insn >> 8) & 15, rm = insn & 15;
                    uint64_t r;
                    if (insn & (1 << 22)) r = (uint64_t)((int64_t)(int32_t)cpu.r[rm] * (int32_t)cpu.r[rs]);
                    else r = (uint64_t)cpu.r[rm] * cpu.r[rs];
                    if (insn & (1 << 21)) r += ((uint64_t)cpu.r[hi] << 32) | cpu.r[lo];
                    cpu.r[lo] = (uint32_t)r; cpu.r[hi] = (uint32_t)(r >> 32);
                    if (insn & (1 << 20)) { cpu.n = (uint32_t)(r >> 63); cpu.z = r == 0; }
                    cpu.budget -= 3;
                    return;
                }
                if ((insn & 0x0FB00FF0) == 0x01000090) {  /* SWP/SWPB */
                    int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15, rm = insn & 15;
                    uint32_t a = cpu.r[rn], t;
                    if (insn & (1 << 22)) { t = rd8(a); if (cpu.abort) return; wr8(a, cpu.r[rm]); }
                    else { t = ldr_word(a, is_priv()); if (cpu.abort) return; wr32(a & ~3u, cpu.r[rm]); }
                    if (cpu.abort) return;
                    cpu.r[rd] = t;
                    cpu.budget -= 2;
                    return;
                }
                undefined(pc, insn, 0); return;
            }
            /* halfword / signed transfers (and v5TE LDRD/STRD) */
            int P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
            int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
            uint32_t off = I ? (((insn >> 4) & 0xF0) | (insn & 15)) : cpu.r[insn & 15];
            uint32_t base = reg_r(rn, pc8);
            uint32_t a = P ? (U ? base + off : base - off) : base;
            uint32_t nb = U ? base + off : base - off;
            uint32_t val = 0;
            if (L) {
                if (sh == 1) val = rd16(a & ~1u);
                else if (sh == 2) val = (uint32_t)(int32_t)(int8_t)rd8(a);
                else val = (uint32_t)(int32_t)(int16_t)rd16(a & ~1u);
                if (cpu.abort) return;
                if (!P || W) cpu.r[rn] = nb;
                if (rd == 15) set_pc(val & ~3u); else cpu.r[rd] = val;
                cpu.budget -= 1;
            } else {
                if (sh == 1) {
                    wr16(a & ~1u, rd == 15 ? pc + 12 : cpu.r[rd]);
                } else if (sh == 2) { /* LDRD */
                    uint32_t v0 = rd32(a & ~3u), v1 = rd32((a + 4) & ~3u);
                    if (cpu.abort) return;
                    if (!P || W) cpu.r[rn] = nb;
                    cpu.r[rd & 14] = v0; cpu.r[(rd & 14) + 1] = v1;
                    return;
                } else { /* STRD */
                    wr32(a & ~3u, cpu.r[rd & 14]); wr32((a + 4) & ~3u, cpu.r[(rd & 14) + 1]);
                }
                if (cpu.abort) return;
                if (!P || W) cpu.r[rn] = nb;
            }
            return;
        }
        uint32_t opc = (insn >> 21) & 15;
        int S = (insn >> 20) & 1;
        if (!S && opc >= 8 && opc <= 11) {
            /* misc: MRS/MSR/BX/CLZ/BLX */
            if (op == 0 && (insn & 0x0FFFFFD0) == 0x012FFF10) { /* BX / BLX reg */
                uint32_t t = cpu.r[insn & 15];
                if (insn & 0x20) cpu.r[14] = pc + 4;
                if (t & 1) { cpu.cpsr |= T_BIT; set_pc(t & ~1u); } else { cpu.cpsr &= ~T_BIT; set_pc(t & ~3u); }
                return;
            }
            if (op == 0 && (insn & 0x0FFF0FF0) == 0x016F0F10) { /* CLZ */
                uint32_t v = cpu.r[insn & 15];
                cpu.r[(insn >> 12) & 15] = v ? __builtin_clz(v) : 32;
                return;
            }
            if ((insn & 0x0FBF0FFF) == 0x010F0000) { /* MRS */
                cpu.r[(insn >> 12) & 15] = (insn & (1 << 22)) ? cpu.spsr : cpu_get_cpsr();
                return;
            }
            if ((insn & 0x0DB0F000) == 0x0120F000) { /* MSR */
                uint32_t v;
                if (insn & (1 << 25)) { uint32_t r = ((insn >> 8) & 15) * 2; v = insn & 0xFF; if (r) v = (v >> r) | (v << (32 - r)); }
                else v = cpu.r[insn & 15];
                uint32_t mask = 0;
                if (insn & (1 << 16)) mask |= 0x000000FF;
                if (insn & (1 << 17)) mask |= 0x0000FF00;
                if (insn & (1 << 18)) mask |= 0x00FF0000;
                if (insn & (1 << 19)) mask |= 0xFF000000;
                if (insn & (1 << 22)) {
                    if (bank_idx(cpu.cpsr) != 0) cpu.spsr = (cpu.spsr & ~mask) | (v & mask);
                } else {
                    if (!is_priv()) mask &= 0xFF000000;
                    uint32_t cur = cpu_get_cpsr();
                    cpu_set_cpsr((cur & ~mask) | (v & mask));
                }
                return;
            }
            undefined(pc, insn, 0); return;
        }
        /* data processing */
        uint32_t op2, co = cpu.c;
        if (op == 1) {
            uint32_t r = ((insn >> 8) & 15) * 2;
            op2 = insn & 0xFF;
            if (r) { op2 = (op2 >> r) | (op2 << (32 - r)); co = op2 >> 31; }
        } else {
            int rm = insn & 15;
            uint32_t t = (insn >> 5) & 3;
            if (insn & 0x10) {
                uint32_t amt = cpu.r[(insn >> 8) & 15] & 0xFF;
                uint32_t v = rm == 15 ? pc + 12 : cpu.r[rm];
                op2 = shift(v, t, amt, 0, &co);
                cpu.budget -= 1;
            } else {
                op2 = shift(reg_r(rm, pc8), t, (insn >> 7) & 31, 1, &co);
            }
        }
        int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
        uint32_t a = (insn & 0x10) && op == 0 && rn == 15 ? pc + 12 : reg_r(rn, pc8);
        uint32_t res = 0;
        int wr = 1;
        if (S && rd != 15) {
            switch (opc) {
            case 0: res = a & op2; set_nz(res); cpu.c = co; break;
            case 1: res = a ^ op2; set_nz(res); cpu.c = co; break;
            case 2: res = sub_flags(a, op2, 1); break;
            case 3: res = sub_flags(op2, a, 1); break;
            case 4: res = add_flags(a, op2, 0); break;
            case 5: res = add_flags(a, op2, cpu.c); break;
            case 6: res = sub_flags(a, op2, cpu.c); break;
            case 7: res = sub_flags(op2, a, cpu.c); break;
            case 8: set_nz(a & op2); cpu.c = co; wr = 0; break;
            case 9: set_nz(a ^ op2); cpu.c = co; wr = 0; break;
            case 10: sub_flags(a, op2, 1); wr = 0; break;
            case 11: add_flags(a, op2, 0); wr = 0; break;
            case 12: res = a | op2; set_nz(res); cpu.c = co; break;
            case 13: res = op2; set_nz(res); cpu.c = co; break;
            case 14: res = a & ~op2; set_nz(res); cpu.c = co; break;
            default: res = ~op2; set_nz(res); cpu.c = co; break;
            }
            if (wr) cpu.r[rd] = res;
            return;
        }
        switch (opc) {
        case 0: res = a & op2; break;
        case 1: res = a ^ op2; break;
        case 2: res = a - op2; break;
        case 3: res = op2 - a; break;
        case 4: res = a + op2; break;
        case 5: res = a + op2 + cpu.c; break;
        case 6: res = a - op2 - !cpu.c; break;
        case 7: res = op2 - a - !cpu.c; break;
        case 8: case 9: case 10: case 11:
            /* S with rd==15 (TEQP etc, v4: just flags from result, then CPSR=SPSR) */
            if (S) {
                switch (opc) {
                case 8: set_nz(a & op2); cpu.c = co; break;
                case 9: set_nz(a ^ op2); cpu.c = co; break;
                case 10: sub_flags(a, op2, 1); break;
                default: add_flags(a, op2, 0); break;
                }
                if (bank_idx(cpu.cpsr)) cpu_set_cpsr(cpu.spsr);
            }
            return;
        case 12: res = a | op2; break;
        case 13: res = op2; break;
        case 14: res = a & ~op2; break;
        default: res = ~op2; break;
        }
        if (rd == 15) {
            if (S) {
                if (bank_idx(cpu.cpsr)) cpu_set_cpsr(cpu.spsr);
                set_pc(res & ((cpu.cpsr & T_BIT) ? ~1u : ~3u));
            } else set_pc(res & ~3u);
        } else cpu.r[rd] = res;
        return;
    }
    case 2: case 3: {
        /* single data transfer */
        if (op == 3 && (insn & 0x10)) { undefined(pc, insn, 0); return; }
        int P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        int rn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
        uint32_t off;
        if (op == 2) off = insn & 0xFFF;
        else { uint32_t co = cpu.c; off = shift(cpu.r[insn & 15], (insn >> 5) & 3, (insn >> 7) & 31, 1, &co); }
        uint32_t base = reg_r(rn, pc8);
        uint32_t nb = U ? base + off : base - off;
        uint32_t a = P ? nb : base;
        int priv = (!P && W) ? 0 : is_priv();   /* LDRT/STRT */
        if (L) {
            uint32_t v = B ? rd8p(a, priv) : ldr_word(a, priv);
            if (cpu.abort) return;
            if (!P || W) cpu.r[rn] = nb;
            if (rd == 15) set_pc(v & ~3u);      /* v4T: no interworking */
            else cpu.r[rd] = v;
            cpu.budget -= 1;
        } else {
            uint32_t v = rd == 15 ? pc + 12 : cpu.r[rd];
            if (B) wr8p(a, v, priv); else wr32p(a & ~3u, v, priv);
            if (cpu.abort) return;
            if (!P || W) cpu.r[rn] = nb;
        }
        return;
    }
    case 4:
        arm_ldm_stm(insn, pc);
        return;
    case 5: {
        int32_t off = ((int32_t)(insn << 8)) >> 6;
        if (insn & (1 << 24)) cpu.r[14] = pc + 4;
        set_pc(pc8 + off);
        return;
    }
    case 6:  /* LDC/STC */
        undefined(pc, insn, 0); return;
    default:
        if (insn & (1 << 24)) {   /* SWI */
            take_exception(0x08, M_SVC, pc + 4, 0);
            branched = 1;
            return;
        }
        if ((insn & 0x10) && ((insn >> 8) & 15) == 15) {
            uint32_t crn = (insn >> 16) & 15, crm = insn & 15, op2 = (insn >> 5) & 7;
            int rd = (insn >> 12) & 15;
            if (!is_priv()) { undefined(pc, insn, 0); return; }
            if (insn & (1 << 20)) {
                uint32_t v = cp15_read(crn, op2, crm);
                if (rd == 15) { cpu.n = v >> 31; cpu.z = (v >> 30) & 1; cpu.c = (v >> 29) & 1; cpu.v = (v >> 28) & 1; }
                else cpu.r[rd] = v;
            } else {
                cp15_write(crn, op2, crm, rd == 15 ? pc + 12 : cpu.r[rd]);
            }
            return;
        }
        undefined(pc, insn, 0);
        return;
    }
}

/* ---------------------------------------------------------------- Thumb */

static void thumb_step(uint32_t pc, uint32_t i) {
    uint32_t pc4 = pc + 4;
    cpu.r[15] = pc4;
    switch (i >> 11) {
    case 0x00: case 0x01: case 0x02: { /* shift imm */
        uint32_t co = cpu.c, rs = cpu.r[(i >> 3) & 7], amt = (i >> 6) & 31;
        uint32_t r = shift(rs, i >> 11, amt, 1, &co);
        cpu.r[i & 7] = r; set_nz(r); cpu.c = co;
        return;
    }
    case 0x03: { /* add/sub */
        uint32_t a = cpu.r[(i >> 3) & 7];
        uint32_t b = (i & 0x400) ? (i >> 6) & 7 : cpu.r[(i >> 6) & 7];
        cpu.r[i & 7] = (i & 0x200) ? sub_flags(a, b, 1) : add_flags(a, b, 0);
        return;
    }
    case 0x04: { uint32_t v = i & 0xFF; cpu.r[(i >> 8) & 7] = v; set_nz(v); return; }
    case 0x05: sub_flags(cpu.r[(i >> 8) & 7], i & 0xFF, 1); return;
    case 0x06: { int d = (i >> 8) & 7; cpu.r[d] = add_flags(cpu.r[d], i & 0xFF, 0); return; }
    case 0x07: { int d = (i >> 8) & 7; cpu.r[d] = sub_flags(cpu.r[d], i & 0xFF, 1); return; }
    case 0x08: {
        if (!(i & 0x400)) { /* ALU */
            int d = i & 7; uint32_t a = cpu.r[d], b = cpu.r[(i >> 3) & 7], r, co = cpu.c;
            switch ((i >> 6) & 15) {
            case 0: r = a & b; set_nz(r); cpu.r[d] = r; break;
            case 1: r = a ^ b; set_nz(r); cpu.r[d] = r; break;
            case 2: r = shift(a, 0, b & 0xFF, 0, &co); cpu.c = co; set_nz(r); cpu.r[d] = r; break;
            case 3: r = shift(a, 1, b & 0xFF, 0, &co); cpu.c = co; set_nz(r); cpu.r[d] = r; break;
            case 4: r = shift(a, 2, b & 0xFF, 0, &co); cpu.c = co; set_nz(r); cpu.r[d] = r; break;
            case 5: cpu.r[d] = add_flags(a, b, cpu.c); break;
            case 6: cpu.r[d] = sub_flags(a, b, cpu.c); break;
            case 7: r = shift(a, 3, b & 0xFF, 0, &co); cpu.c = co; set_nz(r); cpu.r[d] = r; break;
            case 8: set_nz(a & b); break;
            case 9: cpu.r[d] = sub_flags(0, b, 1); break;
            case 10: sub_flags(a, b, 1); break;
            case 11: add_flags(a, b, 0); break;
            case 12: r = a | b; set_nz(r); cpu.r[d] = r; break;
            case 13: r = a * b; set_nz(r); cpu.r[d] = r; cpu.budget -= 2; break;
            case 14: r = a & ~b; set_nz(r); cpu.r[d] = r; break;
            default: r = ~b; set_nz(r); cpu.r[d] = r; break;
            }
            return;
        }
        /* hi register ops / BX */
        int d = (i & 7) | ((i >> 4) & 8), m = (i >> 3) & 15;
        uint32_t b = m == 15 ? pc4 : cpu.r[m];
        switch ((i >> 8) & 3) {
        case 0: if (d == 15) set_pc((pc4 + b) & ~1u); else cpu.r[d] = cpu.r[d] + b; break;
        case 1: sub_flags(d == 15 ? pc4 : cpu.r[d], b, 1); break;
        case 2: if (d == 15) set_pc(b & ~1u); else cpu.r[d] = b; break;
        default:
            if (i & 0x80) cpu.r[14] = (pc + 2) | 1;   /* BLX (v5) */
            if (b & 1) set_pc(b & ~1u);
            else { cpu.cpsr &= ~T_BIT; set_pc(b & ~3u); }
            break;
        }
        return;
    }
    case 0x09: { /* LDR pc-rel */
        uint32_t v = rd32((pc4 & ~3u) + ((i & 0xFF) << 2));
        if (cpu.abort) return;
        cpu.r[(i >> 8) & 7] = v; cpu.budget -= 1;
        return;
    }
    case 0x0A: case 0x0B: { /* load/store reg offset */
        uint32_t a = cpu.r[(i >> 3) & 7] + cpu.r[(i >> 6) & 7];
        int d = i & 7;
        uint32_t v;
        switch ((i >> 9) & 7) {
        case 0: wr32(a & ~3u, cpu.r[d]); return;
        case 1: wr16(a & ~1u, cpu.r[d]); return;
        case 2: wr8(a, cpu.r[d]); return;
        case 3: v = (uint32_t)(int32_t)(int8_t)rd8(a); break;
        case 4: v = ldr_word(a, is_priv()); break;
        case 5: v = rd16(a & ~1u); break;
        case 6: v = rd8(a); break;
        default: v = (uint32_t)(int32_t)(int16_t)rd16(a & ~1u); break;
        }
        if (cpu.abort) return;
        cpu.r[d] = v; cpu.budget -= 1;
        return;
    }
    case 0x0C: wr32((cpu.r[(i >> 3) & 7] + ((i >> 4) & 0x7C)) & ~3u, cpu.r[i & 7]); return;
    case 0x0D: { uint32_t v = ldr_word(cpu.r[(i >> 3) & 7] + ((i >> 4) & 0x7C), is_priv()); if (!cpu.abort) cpu.r[i & 7] = v; return; }
    case 0x0E: wr8(cpu.r[(i >> 3) & 7] + ((i >> 6) & 31), cpu.r[i & 7]); return;
    case 0x0F: { uint32_t v = rd8(cpu.r[(i >> 3) & 7] + ((i >> 6) & 31)); if (!cpu.abort) cpu.r[i & 7] = v; return; }
    case 0x10: wr16((cpu.r[(i >> 3) & 7] + ((i >> 5) & 0x3E)) & ~1u, cpu.r[i & 7]); return;
    case 0x11: { uint32_t v = rd16((cpu.r[(i >> 3) & 7] + ((i >> 5) & 0x3E)) & ~1u); if (!cpu.abort) cpu.r[i & 7] = v; return; }
    case 0x12: wr32((cpu.r[13] + ((i & 0xFF) << 2)) & ~3u, cpu.r[(i >> 8) & 7]); return;
    case 0x13: { uint32_t v = ldr_word(cpu.r[13] + ((i & 0xFF) << 2), is_priv()); if (!cpu.abort) cpu.r[(i >> 8) & 7] = v; return; }
    case 0x14: cpu.r[(i >> 8) & 7] = (pc4 & ~3u) + ((i & 0xFF) << 2); return;
    case 0x15: cpu.r[(i >> 8) & 7] = cpu.r[13] + ((i & 0xFF) << 2); return;
    case 0x16: case 0x17: {
        if ((i & 0xFF00) == 0xB000) {
            uint32_t o = (i & 0x7F) << 2;
            cpu.r[13] = (i & 0x80) ? cpu.r[13] - o : cpu.r[13] + o;
            return;
        }
        if ((i & 0x0600) == 0x0400) { /* PUSH/POP */
            int priv = is_priv();
            uint32_t list = i & 0xFF;
            int extra = (i >> 8) & 1;
            int cnt = __builtin_popcount(list) + extra;
            cpu.budget -= cnt;
            if (i & 0x800) { /* POP */
                uint32_t a = cpu.r[13], vals[9];
                int k = 0;
                for (int b = 0; b < 8; b++) if (list & (1 << b)) { vals[k++] = rd32p(a, priv); a += 4; }
                uint32_t pcv = 0;
                if (extra) { pcv = rd32p(a, priv); a += 4; }
                if (cpu.abort) return;
                k = 0;
                for (int b = 0; b < 8; b++) if (list & (1 << b)) cpu.r[b] = vals[k++];
                cpu.r[13] = a;
                if (extra) set_pc(pcv & ~1u);   /* ARMv4T: POP {pc} stays in Thumb */
            } else {
                uint32_t a = cpu.r[13] - cnt * 4, s = a;
                for (int b = 0; b < 8; b++) if (list & (1 << b)) { wr32p(a, cpu.r[b], priv); a += 4; }
                if (extra) wr32p(a, cpu.r[14], priv);
                if (cpu.abort) return;
                cpu.r[13] = s;
            }
            return;
        }
        if ((i & 0xFF00) == 0xBE00) { /* BKPT */
            take_exception(0x0C, M_ABT, pc + 4, 0); branched = 1; return;
        }
        undefined(pc, i, 1);
        return;
    }
    case 0x18: case 0x19: { /* STMIA/LDMIA */
        int rn = (i >> 8) & 7, priv = is_priv();
        uint32_t list = i & 0xFF, a = cpu.r[rn];
        if (!list) { /* unpredictable; treat like ARM empty list */
            if (i & 0x800) { uint32_t v = rd32p(a, priv); if (cpu.abort) return; set_pc(v & ~1u); }
            else wr32p(a, pc + 6, priv);
            cpu.r[rn] = a + 0x40; return;
        }
        cpu.budget -= __builtin_popcount(list);
        if (i & 0x800) {
            uint32_t vals[8];
            for (int b = 0; b < 8; b++) if (list & (1 << b)) { vals[b] = rd32p(a, priv); a += 4; }
            if (cpu.abort) return;
            cpu.r[rn] = a;
            for (int b = 0; b < 8; b++) if (list & (1 << b)) cpu.r[b] = vals[b];
        } else {
            uint32_t s = a;
            for (int b = 0; b < 8; b++) if (list & (1 << b)) { wr32p(a, cpu.r[b], priv); a += 4; }
            (void)s;
            if (!cpu.abort) cpu.r[rn] = a;
        }
        return;
    }
    case 0x1A: case 0x1B: { /* Bcond / SWI */
        uint32_t c = (i >> 8) & 15;
        if (c == 15) { take_exception(0x08, M_SVC, pc + 2, 0); branched = 1; return; }
        if (c == 14) { undefined(pc, i, 1); return; }
        if (cond_pass(c)) set_pc(pc4 + ((int32_t)(int8_t)(i & 0xFF) << 1));
        return;
    }
    case 0x1C: set_pc(pc4 + (((int32_t)(i << 21)) >> 20)); return;
    case 0x1D: { /* BLX suffix (v5) */
        uint32_t t = cpu.r[14] + ((i & 0x7FF) << 1);
        cpu.r[14] = (pc + 2) | 1;
        cpu.cpsr &= ~T_BIT;
        set_pc(t & ~3u);
        return;
    }
    case 0x1E: cpu.r[14] = pc4 + (((int32_t)(i << 21)) >> 9); return;
    default: { /* 0x1F BL suffix */
        uint32_t t = cpu.r[14] + ((i & 0x7FF) << 1);
        cpu.r[14] = (pc + 2) | 1;
        set_pc(t);
        return;
    }
    }
}

/* ---------------------------------------------------------------- main loop */

void cpu_end_slice(void) { cpu.run_len -= cpu.budget; cpu.budget = 0; }

/* one interpreted instruction at pc (interrupts, idle and HLE hooks already handled) */
static inline __attribute__((always_inline)) void interp_step(uint32_t pc) {
    uint32_t insn;
    branched = 0;
    cpu.abort = 0;
    if (cpu.cpsr & T_BIT) {
        if (!fetch16(pc, &insn)) { take_exception(0x0C, M_ABT, pc + 4, 0); return; }
        if (cpu.trace_n) { cpu.trace_n--; fprintf(stderr, "T %08x: %04x  r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n", pc, insn, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[13], cpu.r[14]); }
        thumb_step(pc, insn);
        if (cpu.abort) { take_exception(0x10, M_ABT, pc + 8, 0); cpu.budget -= 1; return; }
        if (!branched) cpu.r[15] = pc + 2;
        else if (cpu.r[15] != pc) { cpu.ring[cpu.ring_pos++ & 65535] = pc | 1; cpu.ring[cpu.ring_pos++ & 65535] = cpu.r[15] | ((cpu.cpsr >> 5) & 1); }
    } else {
        if (pc - pf_lo < pf_hi - pf_lo) {
            uint32_t keep = cpu.ctrl, pa = pc; cpu.ctrl = pf_ctrl;
            translate(pc, 0, 1, &pa); cpu.ctrl = keep;
            uint8_t *pg = bus_page(pa, 0); insn = pg ? *(uint32_t *)(pg + (pa & 0xFFF)) : bus_io_read(pa, 4);
        } else { pf_lo = pf_hi = 0;
        if (!fetch32(pc, &insn)) { take_exception(0x0C, M_ABT, pc + 4, 0); return; } }
        if (cpu.trace_n) { cpu.trace_n--; fprintf(stderr, "A %08x: %08x  r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x\n", pc, insn, cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], cpu.r[13], cpu.r[14]); }
        if ((insn >> 28) == 0xE || cond_pass(insn >> 28)) {
            if ((insn >> 28) == 0xF) { undefined(pc, insn, 0); branched = 1; }  /* v4: NV space (v5 BLX imm below) */
            else arm_step(pc, insn);
        }
        if (cpu.abort) { take_exception(0x10, M_ABT, pc + 8, 0); cpu.budget -= 1; return; }
        if (!branched) cpu.r[15] = pc + 4;
        else if (cpu.r[15] != pc) { cpu.ring[cpu.ring_pos++ & 65535] = pc; cpu.ring[cpu.ring_pos++ & 65535] = cpu.r[15] | ((cpu.cpsr >> 5) & 1); }
    }
    cpu.budget -= 1;
}

int cpu_run(int cycles) {
#ifdef ZJIT
    if (cpu_jit && jit_ok && cpu.dbg_lo == cpu.dbg_hi && !cpu.trace_n) return cpu_run_jit(cycles);
#endif
    cpu.budget = cycles;
    cpu.run_len = cycles;
    while (cpu.budget > 0) {
        if (cpu.int_pending) {
            uint32_t ret = cpu.r[15] + 4;
            if (cpu.fiq_line && !(cpu.cpsr & F_BIT)) take_exception(0x1C, M_FIQ, ret, 1);
            else take_exception(0x18, M_IRQ, ret, 0);
        }
        if (cpu.halted) { cpu.budget = 0; break; }   /* idle: the rest of the slice passes */
        uint32_t pc = cpu.r[15];
        if (pc - cpu.dbg_lo < cpu.dbg_hi - cpu.dbg_lo && (!cpu.dbg_sp_hi || cpu.r[13] - cpu.dbg_sp_lo < cpu.dbg_sp_hi - cpu.dbg_sp_lo)) { if (cpu.dbg_skip > 0) cpu.dbg_skip--; else { cpu.dbg_hit = 1; break; } }
        if (pc - 0x20000000u < cpu.hle_span) {
            uint32_t bi = (pc - 0x20000000u) >> 1;
            if ((cpu.hle_bitmap[bi >> 3] >> (bi & 7)) & 1) {
                cpu.abort = 0;
                cpu.hle_hook(pc);
                if (cpu.r[15] != pc) { cpu.budget -= 1; continue; }
            }
        }
        interp_step(pc);
    }
    int done = (int)(cpu.run_len - cpu.budget);
    cpu.cycles += done;
    cpu.run_len = cpu.budget = 0;
    return done;
}

void cpu_state(StateIO *s) {
    st_bytes(s, &cpu, offsetof(Cpu, tlb_r));      /* registers, banks, CP15, timing */
    ST(s, pf_lo); ST(s, pf_hi); ST(s, pf_ctrl);
    if (s->loading) cpu_code_changed();
}

#ifdef ZJIT
#include "jit_a64.inc"
#endif

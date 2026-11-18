/* ZodiacDroid - ARM920T (ARMv4T, ARM + Thumb, CP15 MMU) CPU core */
#pragma once
#include <stdint.h>

enum { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };

#define TLB_BITS 12
#define TLB_N    (1 << TLB_BITS)

typedef struct {
    uint32_t tag;        /* (va >> 12) | 0 ; 0xFFFFFFFF = invalid */
    uint32_t pa;         /* physical page base */
    uint8_t *host;       /* host pointer for the page, or NULL for I/O */
} TlbEnt;

typedef struct {
    uint32_t r[16];                    /* r15 = address of the next instruction between steps */
    uint32_t n, z, c, v;               /* flags, each 0 or 1 */
    uint32_t cpsr;                     /* control bits (mode, T, F, I); flags live above */
    uint32_t spsr;
    uint32_t bank_usr[5], bank_fiq[5]; /* r8-r12 */
    uint32_t bank_r13[6], bank_r14[6], bank_spsr[6]; /* usr,svc,abt,und,irq,fiq */

    /* CP15 */
    uint32_t ctrl, ttb, dacr, dfsr, ifsr, far, pid;
    uint32_t cp15_misc[16];

    int irq_line, fiq_line;
    int int_pending;                   /* (irq && !I) || (fiq && !F) */
    int halted;                        /* wait-for-interrupt */
    int abort;                         /* set by a faulting data access */
    int64_t budget, run_len;
    uint64_t cycles;

    TlbEnt tlb_r[2][TLB_N];            /* [priv] */
    TlbEnt tlb_w[2][TLB_N];

    /* debug */
    void (*hle_hook)(uint32_t pc);     /* called when executing an address marked in hle_bitmap */
    uint8_t *hle_bitmap;               /* 1 bit per halfword over the first 512MB, or NULL */
    void (*undef_hook)(uint32_t pc, uint32_t insn, int thumb); /* optional: called on undefined instructions */
    uint32_t trace_n;
    uint32_t dbg_lo, dbg_hi, dbg_sp_lo, dbg_sp_hi; int dbg_hit, dbg_skip;      /* stop when pc enters [lo,hi) */
    uint32_t ring[65536]; uint32_t ring_pos;   /* last executed PCs (bit0 = thumb) */                  /* print this many instructions */
} Cpu;

extern Cpu cpu;

/* bus interface (bus.c) */
uint8_t *bus_page(uint32_t pa, int write);          /* host pointer to the 4KB page containing pa, or NULL (I/O) */
uint32_t bus_io_read(uint32_t pa, int size);
void     bus_io_write(uint32_t pa, uint32_t v, int size);

void     cpu_reset(void);
int      cpu_run(int cycles);
void     cpu_end_slice(void);              /* make cpu_run return soon (e.g. after an I/O write that needs servicing) */
static inline uint64_t cpu_now(void) { return cpu.cycles + (uint64_t)(cpu.run_len - cpu.budget); }
void     cpu_set_irq(int level);
void     cpu_set_fiq(int level);
uint32_t cpu_get_cpsr(void);
void     cpu_set_cpsr(uint32_t v);
void     cpu_tlb_flush(void);

/* virtual memory access for HLE/debug helpers (no exceptions raised; return 0 on fault) */
int      cpu_vread32(uint32_t va, uint32_t *out);
int      cpu_vwrite32(uint32_t va, uint32_t v);
int      cpu_vread8(uint32_t va, uint8_t *out);
int      cpu_vwrite8(uint32_t va, uint8_t v);

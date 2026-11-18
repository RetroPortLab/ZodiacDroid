    .section .text._start
    .arm
    .global _start
_start:
    ldr r0, =run_entry
    mov lr, pc
    bx r0
    ldr r1, =0x8000
    str r0, [r1]
    swi 0
1:  b 1b

/* ZodiacDroid - save states */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *buf;
    size_t len, cap, pos;
    int loading, err;
} StateIO;

void st_bytes(StateIO *s, void *p, size_t n);
#define ST(s, v) st_bytes((s), &(v), sizeof(v))

/* per-module state (same function saves and loads, by s->loading) */
void cpu_state(StateIO *s);
void io_state(StateIO *s);
void soc_state(StateIO *s);
void ext_state(StateIO *s);
void w4200_state(StateIO *s);
void w2d_state(StateIO *s);

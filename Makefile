# ZodiacDroid desktop build
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wno-unused
CORE    := $(wildcard core/*.c)

all: desktop/runzod

desktop/runzod: desktop/runzod.c $(CORE) core/*.h
	$(CC) $(CFLAGS) -o $@ desktop/runzod.c $(CORE) -lz -lm

cputest:
	tools/cputest/build.sh

clean:
	rm -f desktop/runzod

.PHONY: all clean cputest

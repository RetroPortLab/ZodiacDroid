# ZodiacDroid desktop build
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wno-unused
CORE    := $(wildcard core/*.c)

all: desktop/runzod desktop/mkcard

desktop/mkcard: desktop/mkcard.c core/fatimg.c core/fatimg.h
	$(CC) $(CFLAGS) -o $@ desktop/mkcard.c core/fatimg.c

desktop/runzod: desktop/runzod.c $(CORE) core/*.h
	$(CC) $(CFLAGS) -o $@ desktop/runzod.c $(CORE) -lz -lm

cputest:
	tools/cputest/build.sh

clean:
	rm -f desktop/runzod desktop/mkcard

.PHONY: all clean cputest

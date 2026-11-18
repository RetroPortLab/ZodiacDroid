#!/bin/sh
# Builds the CPU self-test for ARM and Thumb and compares against a host run.
set -e
cd "$(dirname "$0")"
gcc -O2 -DHOST -o host t.c
gcc -O2 -o harness harness.c ../../core/cpu.c
clang --target=armv4t-none-eabi -c start.s -o start.o
for mode in arm thumb; do
  fl=""; [ $mode = thumb ] && fl="-mthumb"
  clang --target=armv4t-none-eabi -mcpu=arm920t $fl -mfloat-abi=soft -O2 -ffreestanding -nostdlib -fno-builtin -c t.c -o t_$mode.o
  clang --target=armv4t-none-eabi -mcpu=arm920t $fl -mfloat-abi=soft -O2 -ffreestanding -nostdlib -fno-builtin -c rt.c -o rt_$mode.o
  ld.lld -T link.ld start.o t_$mode.o rt_$mode.o -o t_$mode.elf
  llvm-objcopy -O binary t_$mode.elf t_$mode.bin
done
echo "host : $(./host)"
echo "arm  : $(./harness t_arm.bin)"
echo "thumb: $(./harness t_thumb.bin)"

#!/bin/sh
# Recompiler fuzz test: builds the fuzzer for arm64 with the NDK and runs it under qemu.
#   tools/cputest/fuzz.sh [cases] [seed] [thumb%]
# JITMAX=1 checks single-instruction translation (cycle exact); FREEPC=1 lets the random
# code jump anywhere. Needs qemu-user-static.
set -e
cd "$(dirname "$0")"
NDK=${NDK:-/opt/android-sdk/ndk/27.0.12077973}
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android30-clang
$CC -O2 -static -o /tmp/zodiac_fuzz fuzz.c ../../core/cpu.c
qemu-aarch64-static /tmp/zodiac_fuzz "${1:-2000}" "${2:-1}" "${3:-50}"

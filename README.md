# ZodiacDroid

An experimental Tapwave Zodiac emulator for Android, written from scratch, in the
spirit of JagDroid and 3DODroid. It runs the Zodiac's own Palm OS 5.2T ROM, which you
supply from your own device.

## Emulated hardware

- **CPU:** ARM920T (ARMv4T, ARM and Thumb), CP15 MMU with a software TLB. On arm64
  devices a recompiler (`core/jit_a64.inc`) translates guest code blocks to AArch64;
  the interpreter remains for everything else and can be selected in Settings
- **SoC:** Motorola i.MX1 (MC9328MX1): AITC interrupts, GPT timers, RTC, GPIO buttons,
  I2C EEPROM, ASP touch screen ADC, UART logging
- **Graphics:** ATI Imageon W4200: register file, command-processor ring (PM4 type 0/1/3
  packets through the CIF wrap window), 2D engine (ROP3 fills and blits, colour-key
  transparency, mono and 4-bit coverage host data), landscape and portrait scan-out
- **Analog controller:** the W4200's two-channel ADC
- **Save states:** the whole machine, with sparse SDRAM, zlib-compressed

ROM checks that can't pass without a real device (the EEPROM signature, database
signatures, factory digitizer calibration) are bypassed by small, verified patches in
`core/patch.c`. The ROM file itself is never modified.

## Layout

- `core/`: the emulator (portable C)
- `desktop/runzod.c`: a command-line runner with scripted taps, buttons and stick
  input, screenshots, I/O and 2D logging, save/load
- `android/`: the Android app (library screen, ROM setup, controls, save states,
  screen filters, in-game menu, self-updater)
- `tools/cputest/`: CPU self-test that runs compiled ARM and Thumb code, and
  `fuzz.sh`, which runs random instruction sequences on the interpreter and the
  recompiler (arm64 build under qemu) and compares the results

## Building

Desktop: `make`, then `./desktop/runzod zodiac_1.1_update.prc 3000 --shot out.ppm`.

Android: `cd android && gradle assembleRelease` (needs the Android SDK, NDK 27 and
CMake 3.22). Release builds are signed with the key named in
`android/keystore.properties` (not in the repository).

## Releases

Commit a signed APK as `dist/ZodiacDroid-vX.Y.apk` (with optional
`dist/notes-vX.Y.md`) and the release workflow publishes it. The app checks the
latest release and offers to install it.

Not affiliated with Tapwave or PalmSource.

**New: ARM recompiler**
- The emulated CPU now runs through a recompiler. It translates the Zodiac's ARM and Thumb code into native arm64 code for your device, instead of interpreting it one instruction at a time. It should be several times faster, which is what 3D games like Stuntcar Extreme need to hold full speed.
- It's on by default. Settings > Performance > CPU emulation switches back to the interpreter if a game misbehaves.
- With "Show frame rate" on, the overlay now shows JIT or INT, so you can see which one is running.

**Testing so far**
- Booting the ROM (1,500 frames) and 120 frames of a Stuntcar race end on the same screen with the recompiler as with the interpreter.
- Launching Stuntcar from the SD card and driving a race both work.
- The recompiler is checked against the interpreter on tens of thousands of random instruction sequences.
- All of this testing was on an emulated arm64 CPU. Real speed on the Retroid Pocket Nero is still unmeasured, so please report how it runs.

**Other**
- Removed the outdated "sound is not emulated" line from About.

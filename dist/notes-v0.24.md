## ZodiacDroid v0.24-experimental

**Stick responsiveness (reverse engineered)**

The laggy stick came from Palm OS itself: its key-wake handler ignores wake events for 250 ms after the previous one, and it only polls every ~50 ms. A stick push therefore took ~16 frames (~270 ms) to register.

- The hold-off is now shortened to 30 ms, and the emulated wake line pulses faster (every 4 frames, low for 2).
- In the desktop emulator a push now registers in about 9-11 frames (~160 ms) instead of ~16, and a held push still moves a menu exactly one step (no overshoot).
- Short taps behave as before (use the Stick tap assist setting).

Not verified on a real device. The remaining delay is the OS's own stick filtering (~5 scans) and cannot be cut without risking false pushes.

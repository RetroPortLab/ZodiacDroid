**Stuntcar Extreme runs**
- Fixed: games that copy their frames to the screen with DMA (Tapwave's graphics library does this) stayed on a white screen. Stuntcar Extreme now boots to its menus and races.

**Controls**
- Default buttons updated from testing in Stuntcar: A = select, B = back, X and Y = the other two action buttons, L1/R1 = triggers, Start = Function, Select = Home. Reset them in Settings > Controls > Defaults if you changed them.

**New: CPU clock (Settings > Performance)**
- Auto (default) lowers the emulated 200 MHz CPU when your device can't keep up, so games keep real-time speed and just draw fewer frames. Fixed clocks are there too. With "Show frame rate" on, the overlay shows the current clock.

**Known issues**
- 3D games are heavy: ZodiacDroid still interprets every ARM instruction. A recompiler is the next big job.
- First-start setup still pauses on a white screen for up to ~40 seconds after each calibration.

**Fix: Stuntcar freezing in races**
- The "Auto" CPU clock from v0.3 made things worse: with a slower emulated CPU, Stuntcar's physics falls behind and the race stalls (it can freeze for many seconds at stunts). The clock now stays at the Zodiac's real 200 MHz. When your device can't keep up, the whole game runs a bit slower instead, which keeps it playable.
- Changing the clock between 200, 166 and 133 MHz is still possible under Settings > Performance, but lower clocks can cause the same stalls.
- Emulated time no longer jumps when the clock changes.

**Speed**
- About 8% faster CPU emulation. The big speed-up (a recompiler) is the next job.

**Snappier Stuntcar menus: CPU clock up to 400 MHz**
- The game's menus do a lot of software work every frame (scaling and string handling), so on the emulated 200 MHz CPU a button press takes about half a second to show up. This isn't dropped input: even a 2-frame tap registers, but the game is slow to act on it.
- Settings > Performance > CPU clock now offers 300 and 400 MHz (tap to cycle: 200, 300, 400, 166, 133). At 400 MHz the menu response time halves. The change applies immediately. Game time stays real time at any clock.
- If a game stutters at a high clock, set it back to 200 MHz.

Includes the recompiler (v0.5), Remount SD card (v0.6) and the race-picture fix (v0.7).

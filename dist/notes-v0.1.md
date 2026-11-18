First experimental build of ZodiacDroid, a Tapwave Zodiac emulator written from scratch.

**What works**
- Boots your own Zodiac ROM (raw dump, Tapwave's `zodiac_1.1_update.prc`, or the .zip it came in) as a Zodiac 2.
- Palm OS first-run setup: stylus calibration, analog controller calibration, date and time, then the Tapwave launcher.
- Built-in apps run (tested: launcher, AcidSolitaire).
- Touch screen as the stylus, left stick and D-pad as the analog controller, gamepad buttons for the Zodiac buttons (remappable in Settings > Controls).
- The handheld's memory is saved automatically when you leave, so installed apps, settings and progress carry over.
- 6 save-state slots with thumbnails, from the pause menu or the home screen.
- Screen filters: smooth, sharp pixels, scanlines, LCD grid.
- In-game menu (Select + Start, or Back): save states, filter, Home, reset, quit.
- Checks this page for updates and installs them.

**Known issues**
- No sound yet.
- On the very first start, the screen stays white for up to about 40 seconds after the analog controller calibration and after the stylus calibration. Wait and it carries on.
- The action-button and trigger assignments are best guesses; remap them if they feel wrong.
- No SD card or app installing yet.
- Speed depends on the game; there's no recompiler yet.

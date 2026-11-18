**No more Welcome setup after a reset**
- After the reset button or a Palm error the device used to land back in the "Zodiac Setup" wizard (and then the pen calibration screen). Once the device has been set up, a reset now goes straight to the Launcher. A brand-new device still runs the setup wizard once. The "already set up" flag is stored in save states; older save states count as set up.
- Cause: the OS looks for a saved pen-calibration resource after every reset and doesn't find it, so it re-runs setup. The emulator's touch input needs no calibration, so the setup chain is skipped.

Includes unsigned software support (v0.9), the 400 MHz option (v0.8), recompiler (v0.5) and Remount SD card (v0.6).

## v0.18

- Fixes the stick overshooting: since v0.14 a stick deflection was held for ~24 frames after you let go, so left/right (and menu steps) ran on. A fresh press is now only kept for ~20 frames (enough for the OS to see a quick tap) and releasing the stick after that takes effect immediately.

## v0.23

- Stick response: Palm OS only registers a stick push once it passes about 75% of full travel, so a light push or a diagonal (each axis at ~70%) did nothing. There is now a "Stick sensitivity" setting (Normal / High / Max). The default, High, amplifies the stick by 1.4× so the OS sees a full push at roughly half travel. Max is 2×. Normal is the old behaviour.
- Measured in the emulator: buttons react in 2-3 frames. After a stick push the OS takes about 16 frames (~270 ms) to act on it. That delay is inside the Palm OS stick scanner; I didn't find a safe way to shorten it, so d-pad menus will still feel slower than buttons.

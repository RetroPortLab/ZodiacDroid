**Install feature now puts game data in the right place**
- Installing a store .zip that has no installer file (such as Duke Nukem) copied its data folder to the root of the SD card, so the game reported "Duke data not found". The data now goes under /PALM/Programs (e.g. /PALM/Programs/duke-MWNW) and the .prc into /PALM/Launcher.
- Mac junk files (`._name.prc`, `__MACOSX`) in zips are no longer copied.
- If you already installed Duke Nukem with an earlier version, install the zip again after updating (the old data sits in a `duke-MWNW` folder at the card root, which is harmless).

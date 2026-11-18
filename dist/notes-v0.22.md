## v0.22

- Fixes L1 starting Palm OS Bluetooth discovery. The pin I had been using for the left trigger is actually the one Palm OS answers with "Discoverable for 3 minutes". L1/R1 now drive the two inputs next to Home and Power that Palm OS ignores, which is where a pair of top-edge triggers would sit.
- The old Bluetooth pin is kept as the "Extra" button, unassigned by default.
- The trigger pins are inferred from how Palm OS reacts, not confirmed in a game: if a game's triggers or bookmark feel swapped or dead, tell me which game.

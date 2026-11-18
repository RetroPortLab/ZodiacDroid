## v0.14

- Fixed slow, barely responsive menus when navigating with the d-pad or stick. The OS only noticed a deflected stick after about two seconds because its key-wake line was never raised; the emulator now raises it while the stick is off centre (about 0.3 s response).
- Very short stick taps are now held long enough for the OS to register them.

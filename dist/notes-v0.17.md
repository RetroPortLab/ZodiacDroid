## v0.17

- Audio: fixes the stuttering on menus and splash screens. Sound now goes through a small jitter buffer that rebuffers cleanly and nudges playback speed by a few percent instead of dropping out when the emulator briefly runs behind in heavy scenes. The audio stream uses a larger, steadier buffer.
- Performance: memory-to-memory DMA copies are now done in whole blocks, and the screen conversion uses a lookup table.
- Not verified on a device yet; please report if stutter remains and in which scenes.

# Compatibility and limitations

SaturnRecomp is a research runtime, not a drop-in replacement for a mature Saturn emulator. Compatibility changes quickly and must be demonstrated with actual boot/gameplay frames, input, and audio rather than inferred from a successful process launch.

## Current expectations

- Authentic BIOS animation and automatic disc boot are supported when a valid BIOS and bootable disc are configured.
- Many common VDP1/VDP2 paths, dual-SH-2 synchronization patterns, CD commands, and SCSP features are implemented.
- Correct rendering is title- and scene-dependent. Rotation backgrounds, raster effects, uncommon color modes, and edge-case windows may expose missing behavior.
- Performance is currently below Ymir. The runtime uses an interpreter/fast path; full static recompilation is not yet the public execution path.
- SCSP DSP executes, but effects, reverb, timing, and mixing are not yet guaranteed bit-perfect. CD audio also requires a track-preserving CUE/BIN dump.
- Digital pads work. Full analog Saturn 3D Control Pad behavior, multitap, mouse, light gun, and other specialty peripherals are not implemented.
- CUE/BIN and ISO are supported. CHD is not.

## Reporting a compatibility problem

Do not attach BIOS files, disc images, extracted binaries, generated game code, audio, or copyrighted screenshots to a public issue. Provide:

1. Your SaturnRecomp commit ID.
2. BIOS region and SHA-256 only, not the file.
3. Disc product number and dump format; include hashes rather than media.
4. The last visible stage reached and whether the runtime was using the authentic BIOS path or `nobios`.
5. A diagnostic log containing only runtime state. Review it before posting because memory dumps can contain game data.
6. A minimal reproduction using public test code whenever possible.

Private screenshots may be useful for local frame comparison, but they are intentionally excluded from this repository and its release artifacts.

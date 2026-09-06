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

## Targeted Ymir comparison, September 2026

The September 5 checks used the local Ymir 0.4.0-dev core at
`8667888bd1bfaea0d0cfbde46c458e163ac0224c`, matching BIOS/disc data and scripted
controller actions. SaturnRecomp images came from actual Vulkan readbacks.
Audio comparisons used the final stereo mix from each core. Runs overlapped on
the same machine, so these results are not performance benchmarks.

| Game | Observed path | Result and limit |
| --- | --- | --- |
| Burning Rangers | 24,000 fields: training, camera turns, corridor traversal and jet-assisted jumps | Fixed missing and stretched room walls caused by stale cached projection instructions after the game rewrote its own code. Fresh Vulkan captures confirm complete room geometry through movement and camera changes. Dialogue timing and resulting character/camera positions differ from Ymir at some identical field numbers; complete mission coverage remains unverified. |
| Fighting Vipers | Grace vs. Bahn: movement, jumping, attacks, knockdowns, KO, replay, Round 2; 4,500 fields | Fixed the misplaced arena floor, a sound-command timeout that disabled later combat sounds, and the SCSP timer C interrupt level. A fresh run restored fighter voice/effect events to the recorded mix. Other fighters and stages remain unverified. |
| Daytona USA | 7,200 fields of boot, racing attract mode, rankings and title | Fixed missing SCSP DAC18B handling, which made game audio four times too quiet. Fixed mix RMS was 1,561 versus Ymir's 1,565, with matching peaks. A player-controlled race was not exercised. |
| Gunbird | 2,400 fields of logos and opening animation | Corresponding images and audio closely match. Gameplay was not repeated in this pass. |
| NiGHTS into Dreams | 2,400 fields of logos/opening movie; additional Spring Valley movement captures | Corresponding opening images and aligned audio closely match. Fixed environment flicker in inserted gameplay frames by retaining the displayed frame's complete texture and lighting data. Full stage coverage remains unverified. |
| Sonic 3D Blast | 2,400 fields of logos and opening movie | Corresponding images and aligned audio closely match. Gameplay stages were not repeated in this pass. |
| Sonic R | Launcher boot into a race; captured 120 Hz geometry histories and Vulkan intermediate frames | Bundled MP3 decoder discovery is fixed. Interpolation now holds uncertain connected geometry together and rejects invalid face motion. Twelve native endpoints remained pixel-identical; 24 intermediates retained verified motion without the captured shared-edge cracks or face inversions. This does not establish coverage of every track or camera. |

The Burning Rangers fix updates the writing SH-2's cache line on a local write
hit. It preserves the other CPU's private cached code and bypasses cache updates
for DMA and cache-through stores. A game-independent regression exercises these
cases on both CPUs and both interpreter paths, including all three DMA engines.

After the cache fix, twenty sampled images and the full forty-second stereo mix
remained identical for each of NiGHTS, Sonic 3D Blast and Gunbird. The driven
Fighting Vipers run retained the corrected floor through KO, replay and Round 2.
Sonic R's twelve captured native endpoints also remained pixel-identical, while
its intermediate frames retained verified motion. Its title and race MP3 tracks
were identified in the runtime's final audio mix, with no clipped samples in
that capture.

Fighting Vipers previously exhausted its counted sound-command wait before the
68000 acknowledged the command, then disabled subsequent combat sound requests.
The shared sound bus now accounts for read/write latency across all access
widths, and SCSP timer C uses the correct shared interrupt-level selection bit.
In a fresh 4,500-field run with 120 Hz presentation and 2x internal resolution,
the sound-disable flag remained clear and 38 combat sound events occurred in an
interval that previously had none. The first eight restored sample triggers
matched Ymir's sequence; the recorded final stereo mix contained the restored
voice/effect channels and had no clipped samples. Later fight timing and outcomes
diverge, so this does not establish event-for-event parity for the complete match.

The shared interpolation renderer now snapshots VDP1 material data separately
for each framebuffer. Burning Rangers and NiGHTS rewrite texture, palette and
Gouraud data before their older picture finishes displaying; replaying that
older geometry against the new data caused environmental flicker. Fresh Burning
Rangers captures removed all 36,299 large environment differences in the sampled
inserted frame while retaining character motion. NiGHTS Spring Valley captures
also retained the correct terrain colors. Native endpoints stayed unchanged.
Partial draws with mixed material history use their native framebuffer until
a safe complete redraw is available.

The F1 graphics options apply to the shared Vulkan renderer. Native settings
retain the original rendering path; enhancements are optional. Higher internal
resolution and model-edge supersampling rerasterize safe draw histories, while
uncertain or CPU-uploaded sprite framebuffers retain their native pixels. This
is a rendering enhancement, not replacement model or texture artwork.

These are observed paths and specific fixes, not all-game or all-stage
certification. The private frame, memory and audio captures used for comparison
are excluded from the repository.

## Reporting a compatibility problem

Do not attach BIOS files, disc images, extracted binaries, generated game code, audio, or copyrighted screenshots to a public issue. Provide:

1. Your SaturnRecomp commit ID.
2. BIOS region and SHA-256 only, not the file.
3. Disc product number and dump format; include hashes rather than media.
4. The last visible stage reached and whether the runtime was using the authentic BIOS path or `nobios`.
5. A diagnostic log containing only runtime state. Review it before posting because memory dumps can contain game data.
6. A minimal reproduction using public test code whenever possible.

Private screenshots may be useful for local frame comparison, but they are intentionally excluded from this repository and its release artifacts.

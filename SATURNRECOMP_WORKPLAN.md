# SaturnRecomp Compatibility and Optimization Workplan

## Goal

Make SaturnRecomp a hardware-correct, broadly compatible Sega Saturn runtime and
static-recompilation platform. Individual games are test cases, not targets for
one-off behavior. A change discovered through one title should be expressed as a
Saturn hardware rule and validated against other software.

The intended end state is:

- authentic BIOS boot across Japanese, North American, and PAL configurations;
- reliable gameplay, input, audio, saving, CD access, and timing across a broad
  title matrix;
- deterministic execution suitable for static recompilation and regression tests;
- stable frame pacing at the original title's intended rate;
- optimized native execution without hiding hardware or timing errors.

## Engineering rules

1. Implement hardware behavior, not game-specific PCs, literal addresses, or
   timing exceptions.
2. Keep generated recompilation output read-only. Fix the decoder, recompiler,
   runtime, device model, configuration, or tooling instead.
3. Use mature Saturn implementations and primary hardware documentation as
   behavioral references, then encode the behavior in focused local tests.
4. Treat a successful build, a running process, or one non-black frame as a smoke
   test only. Compatibility requires visible boot and sustained gameplay checks.
5. Optimize only from release-build profiles gathered during a reproducible
   workload. Preserve an exact correctness baseline for every optimization.
6. Do not trade accurate pacing for frame skipping, debt dropping, or catch-up
   behavior that merely improves an average FPS counter.

## Priority 1: shared hardware correctness

### Address bus and open-bus behavior

- Centralize each device's address decoder so byte, word, longword, CPU, and DMA
  accesses use the same mapping.
- Model physical mirrors and holes explicitly instead of adding individual
  address ranges as titles encounter them.
- Give populated, unpopulated, and cartridge regions their correct open-bus
  values and access widths.
- Make trace-region classification call the real decoder. A trace row must never
  say `unmapped` when the access handler actually serviced the address.
- Add table-driven tests for every mapped region, mirror, boundary, and access
  width.

### CD-block mapping: implemented, broader validation pending

`runner/src/bus.c` now uses one CD/CS2 register decoder for CPU reads and writes,
DMA through the bus accessors, unmapped-access accounting, and trace labels.
The 64-byte register file mirrors through the first 4 KiB of each 32 KiB block
in the CD/CS2 address space. DATATRNS and the remaining registers share that
mapping, with byte-access side effects and split 32-bit peripheral-bus transfers.

Automated coverage in `tests/cd_bus.c` checks:

- canonical, 32 KiB, 64-byte, and cache-through register mirrors;
- mapped-block boundaries, unmapped holes, and the final CS2 block;
- FIFO advancement, direct byte reads/writes, odd-byte behavior, and split
  32-bit reads/writes;
- mirrored HIRQ writes, transfer-completion signaling, and periodic reports;
- agreement between actual bus decoding and diagnostic trace labels.

These source changes and regression cases do not establish game compatibility.
Authentic boot, loading transitions, sustained gameplay, visible rendering, and
audible playback still require verification across the title matrix.

### SH-2 and system timing

- Preserve instruction, exception, delay-slot, cache, and on-chip peripheral
  semantics across interpreted, fast, and generated execution paths.
- Keep master SH-2, slave SH-2, SCU, VDP, SCSP, SMPC, and CD events on one
  deterministic machine timeline.
- Retain the current 128-cycle scheduler slice until a smaller or adaptive model
  is proven safe. A previous 512-cycle experiment broke FMV loading.
- Add timing tests around interrupts, DMA completion, H-Blank/V-Blank, FRT, and
  master/slave handshakes.

### Remaining device coverage

- SCU: DMA modes, indirect descriptors, DSP execution, interrupt arbitration,
  and bus contention.
- VDP1: complete command semantics, clipping, mesh, gouraud shading, framebuffer
  modes, erase/write timing, and command-list edge cases.
- VDP2: cell and bitmap modes, rotation backgrounds, windows, priorities, color
  calculation, line effects, interlace, PAL modes, and VDP1 composition.
- SCSP/MC68000: slot behavior, DSP accuracy, timers, interrupts, CD-DA mixing,
  synchronization, and underrun-free host playback.
- SMPC: region/clock configuration, INTBACK protocol, controllers and multitaps,
  reset behavior, RTC, and persistent settings.
- Storage and expansion: backup RAM, cartridge identification, RAM cartridges,
  ROM cartridges, and save persistence.

## Priority 2: broad compatibility validation

Maintain a repeatable test matrix that covers hardware features rather than only
counting titles. Include Japanese, North American, and PAL software and examples
using:

- single and dual SH-2 workloads;
- heavy SCU DMA and SCU DSP use;
- VDP1-heavy 2D, 3D, mesh, clipping, and framebuffer effects;
- VDP2 rotation, transparency, windows, interlace, and high-resolution modes;
- streamed audio, CD-DA, filesystem reads, FMV, and mixed-mode discs;
- backup RAM and expansion cartridges;
- digital, analog, mouse, light-gun, and multiplayer input where available.

For each test title, record these gates:

1. authentic BIOS identifies and automatically boots the disc;
2. intro and title screens render and play audio correctly;
3. menus accept input;
4. gameplay remains stable through a reproducible scenario;
5. loading transitions complete without stalls or corrupted assets;
6. saves survive a clean restart;
7. frame pacing and audio remain stable for an extended run.

Gunbird is one Japanese BIOS/CD/input/gameplay regression case. Sonic 3D Blast's
FMV and bridge transition are timing and performance regression cases. Neither
title should determine the architecture by itself.

## Priority 3: optimization

### Measurement infrastructure

- Build a deterministic headless benchmark mode with scripted input and known
  checkpoints.
- Record frame-time percentiles and worst-frame clusters, not only average FPS.
- Attribute host time to SH-2 execution, bus access, VDP1, VDP2, composition,
  MC68000/SCSP, CD processing, and presentation.
- Keep correctness hashes or state checkpoints beside performance results.
- Ensure profiling and trace instrumentation can be compiled out of release
  binaries.

### Likely optimization areas

- SH-2 dispatch, decoded-block caching, branch handling, memory fast paths, and
  safe direct RAM access.
- Device-aware bus lookup that avoids long conditional chains while preserving
  access side effects and mirrors.
- VDP1 rasterization, especially division-heavy texture-coordinate work,
  clipping, overdraw, and framebuffer conversion.
- VDP2 tile/bitmap fetch caching, priority resolution, window evaluation, and
  scanline reuse.
- SCSP sample generation and DSP execution in bounded batches that remain
  synchronized with the Saturn timeline.
- Avoiding redundant framebuffer conversion, uploads, copies, and presentation
  stalls in the SDL renderer.

Prior experiments are not assumed wins. A polling-loop shortcut regressed the
same workload, PGO was slower in the measured run, and a VDP1 arithmetic rewrite
improved some heavy buckets but was not yet confirmed by continuous subjective
play. Re-test any revival against a clean baseline.

## Priority 4: static recompilation

The current playable path still relies on SH-2 interpretation and optimized fast
execution. Broad static recompilation requires more than declaring modules:

- discover code and data safely across overlays and dynamically loaded modules;
- emit native code for the supported SH-2 instruction set and control flow;
- preserve delay slots, exceptions, self-modifying code detection, cache
  behavior, and memory-mapped I/O side effects;
- support interpreter fallback only for explicitly unsupported dynamic regions,
  with coverage reported honestly;
- produce deterministic generated builds and compare them continuously against
  the interpreter using state checkpoints and randomized instruction tests.

Do not call a title statically recompiled until its gameplay path executes the
generated code with measured coverage and without silently using the interpreter
as the primary engine.

## Immediate execution order

The current release prioritizes the native launcher and play instructions.
Broad compatibility testing has been deferred; Burning Rangers and universal
Saturn compatibility remain unconfirmed. The CD mirror decoder and matching
diagnostic classification are already implemented.

1. Keep the CD/CS2 regression coverage in the full automated suite.
2. When compatibility work resumes, repeat authentic BIOS boot and gameplay
   checks and establish the feature-based multi-title compatibility matrix.
3. Add deterministic workload capture and subsystem/frame-time profiling.
4. Optimize the measured hot paths while preserving state and visual results.
5. Expand AOT generation and report native-code coverage separately from runtime
   compatibility.

## Definition of done for a change

A compatibility or optimization change is complete only when:

- the behavior is expressed as a general hardware/runtime rule;
- focused automated tests cover the discovered case and relevant boundaries;
- the full existing test suite passes;
- authentic BIOS boot and the affected gameplay scenario are visibly checked;
- performance changes include comparable before/after measurements;
- no unrelated title regresses in the compatibility matrix;
- documentation reflects any remaining limitation honestly.

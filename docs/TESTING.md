# Testing and diagnostics

## Standard checks

```powershell
.\build.ps1
.\tests\run_all.ps1
.\tools\audit_release.ps1
```

The suite covers SH-2 instruction semantics, fast-path differential execution, dual-CPU scheduling/FRT, VDP1 rasterization and clipping, VDP2 cell decoding and special priority, MC68000/SCSP behavior, CD-DA, and full-opcode decoder comparison when Python Capstone is available.

Set `SATURN_PYTHON` to a Python executable with the `capstone` package if the default `python` command does not provide it.

## Evidence levels

- A successful build proves that sources compile.
- A unit test proves the isolated mechanism it drives.
- A headless boot proves that the machine reached a state, not that the framebuffer is correct.
- A visible frame/input/audio replay proves the observed path. Renderer fixes should include both a pixel-state trace and a screenshot from the exact scene.

## Useful runner modes

```powershell
# Deterministic headless budget
.\runner\saturnboot.exe .\games\mygame\game.toml 100000000

# Profile the interactive runtime
$env:SATURN_PROF = '1'
.\runner\saturnwin.exe .\games\mygame\game.toml

# Save cycle-spaced screenshots under an ignored output directory
$env:SATURN_SHOTS = 'out\shots\frame:100000000'
.\runner\saturnboot.exe .\games\mygame\game.toml 500000000
```

The runtime exposes additional narrowly scoped `SATURN_*` probes in source comments. They are diagnostics, not compatibility switches. Avoid publishing raw state or framebuffer dumps: they can contain copyrighted game data.

## SH-2 cache regressions

`tests/sh2_cache.c` exercises cached self-modifying instructions with byte,
word and longword stores on both CPUs and both interpreter paths. It also
checks cache-through access, write misses, replacement order, private CPU
lines, and SH-2, SCU and SCU-DSP DMA isolation. These regressions prevent a
local code patch from executing stale instructions without making another
CPU's writes or DMA overwrite a private cached overlay.

## Sound command timing

`tests/sound_bus_timing.c` verifies real SH-2 byte, word and longword accesses
to sound RAM and SCSP registers, including mirrors, both CPUs and both
interpreter paths. Reads take 40 clocks and writes take two; diagnostic
accesses and SH-2, SCU and SCU-DSP DMA do not charge the current CPU. The test
also checks scheduler overshoot carry and polling-loop optimization.
Its separate `--diagnostics` mode checks that in-instruction snapshots, pointer
traces and instruction traces preserve guest clocks and data; real exception
stack/vector accesses remain timed. The focused and standard test scripts run
both modes.

A synthetic 68000 driver acknowledges a shared sound-RAM command while an
SH-2 polls with a 65,536-attempt limit. This checks that the sound CPU receives
enough emulated time to respond before the caller times out. No game media,
renderer or host audio device is required. The standard build and test scripts
include this regression; a focused rebuild is also available:

```powershell
.\tools\test_sound_bus_timing.ps1 -ObjectDir out/runtime-build/obj
```

The focused script recompiles the current bus and SH-2 timing sources against
the supplied core objects. `-UseExistingTimingObjects` permits controlled
before/after checks of an already-built runtime. Timer C overflow and interrupt
level behavior are covered separately by `tests/scsp_effects.c`; together these
checks distinguish missing interrupt service from a command poll that expires
too quickly. Passing them does not replace listening to game-specific voices
and effects.

## Renderer probes

`SATURN_PIXELDBG=cycle:x:y` prints the VDP1 sprite code, each VDP2 layer's base/special/effective priority, color, and final layer order at one pixel. `SATURN_SPDBG=1` adds sprite-control and priority registers. These probes are gated and have no effect unless enabled.

`tools/render_state.c` can render a locally captured raw `saturn` state without advancing CPUs. Such snapshots contain BIOS/game memory and are intentionally ignored; never add them to a release.

For sparse captures from the actual Vulkan presentation path, set
`SATURN_VK_CAPTURE=out/compare/frame`, `SATURN_VK_CAPTURE_FRAME=6000`, and
`SATURN_VK_CAPTURE_EVERY=120`. This writes `frame-006000.png`,
`frame-006120.png`, and subsequent captures without saving every field.
Omitting `SATURN_VK_CAPTURE_EVERY` retains the single-capture behavior, with
`SATURN_VK_CAPTURE` used as the exact output filename. Compare equivalent
gameplay actions and camera views; identical field numbers alone do not
establish that two emulators have reached the same scene.

`tools/audit_geometry_history.c` checks locally captured interpolation histories
for shared-edge cracks, partial face deformation, winding reversal, and invalid
interpolation paths. It also reports retained and moving faces, so a fix that
simply disables all motion is visible:

```powershell
gcc -O2 -std=c11 -Irunner/include tools/audit_geometry_history.c -o out/audit-geometry.exe
.\out\audit-geometry.exe out/local-capture/edge-audit out/local-capture/safe.ops
```

The input prefix names `-before.ops` and `-current.ops` files produced by
`SATURN_INTERP_AUDIT`. Use same-build geometry snapshots with
`tools/replay_interp_capture.c` to inspect native endpoints and actual Vulkan
intermediate images as well. Geometry statistics alone do not establish visual
correctness across a whole game.

Geometry captures also include a `.vram` material snapshot for each displayed
framebuffer. Keep it with the matching `.ops` and machine snapshot when using
`tools/replay_interp_capture.c`; current live VRAM can already contain the next
picture's textures or Gouraud tables. Older captures without material snapshots
cannot establish interpolation color correctness.

## Shared presentation checks

`tests/runtime_settings.c` checks normalization, atomic persistence, unknown-key
preservation and settings round trips. `tests/game_overlay.c` exercises native
F1 input, keyboard navigation, settings actions and unavailable controls.
Both are built and run by the standard scripts.

After a runtime build, run the GPU regressions on an SDL/Vulkan-capable host:

```powershell
.\tools\test_interpolation_material.ps1
.\tools\test_render_quality.ps1
```

Both accept `-ObjectDir` for a different runtime object directory. The material
test checks texture, CLUT and Gouraud reuse across framebuffer swaps, partial
draw fallback and recovery. The quality test renders synthetic geometry and
textures through the production shaders to distinguish real rerasterization
from pixel enlargement, check RGB filtering and indexed codes, and verify
supersampling and FXAA. Both inspect native framebuffer/mesh preservation and
guest-state isolation. They require no game media.

The September 2026 F1 overhead check used a fixed 128-quad scene on a GTX 1660 Ti
at 125% Windows DPI. Three repeated baseline/closed/open runs had overlapping
median frame times of 3.05–3.31 ms. The closed panel submitted no overlay uploads
or render passes. While open, panel construction took about 0.23 ms median and
CPU upload/command recording about 0.04 ms; the native image checksum stayed
identical. These are synthetic renderer measurements, not full-game FPS claims.
Internal resolution, filtering and supersampling have separate rendering costs.

A captured 500-command NiGHTS scene also compared supersampled framebuffer
allocation with 2x internal resolution, model smoothing and interpolation
enabled. Moving those buffers from host-visible memory to device-local memory
reduced the median time for two enhanced pictures from 23.7-28.8 ms to
4.4-4.7 ms in two paired runs. Separate readbacks of the actual 704x448 enhanced
pictures were pixel-identical. This measured rendering only, with no guest CPU
execution; it is not a whole-game FPS multiplier. Detailed local evidence is
in the ignored `out/f1-performance/REPORT.md`.

The integrated runtime was also exercised while paused: F1 changed internal
resolution, interpolation, target rate, volume and mute, F2 preserved a 121 Hz
preference when disabled, and Resume/Esc closed the panel and game in order.
`tests/window_audio.c` verifies actual callback drain, volume, mute and underrun
behavior without changing the source ring's samples or emulated audio timing.

The final paused integration also exercised 640x480 minimum sizing, live resize,
held-key reopening, repeated F1 input, and saving on close/exit without advancing
the guest field. Eight rapid volume changes produced one settings-file update.
A Windows reader holding the file against replacement for 500 ms did not block
F1 input; retries saved the latest values after the reader released it.
GPU lifecycle tests inject partial allocation failures, retry initialization,
and verify overlay output across swapchain format changes and resizing.

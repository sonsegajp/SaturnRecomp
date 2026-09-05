<p align="center">
  <img src="assets/saturnrecomp-logo.png" alt="SaturnRecomp" width="760">
</p>

# SaturnRecomp

SaturnRecomp is an experimental, clean-room Sega Saturn runtime and static-recompilation research toolkit. It contains a Saturn disc/configuration CLI, an SH-2 execution core, and a Windows SDL2 runner that boots a user-supplied game through a user-supplied Saturn BIOS.

The current playable path executes SH-2 code through the interpreter and its optimized fast path. The decoder and module-analysis foundation for ahead-of-time recompilation are present, but a complete public AOT emitter is not. The project name describes the destination; this README describes the current implementation without pretending it is finished.

## No games or firmware are included

This repository intentionally contains no BIOS ROM, game image, extracted executable, generated game code, save data, audio track, or game screenshot. You must dump the BIOS and game disc from hardware/media you own and keep those files local. Do not open an issue asking where to download copyrighted files.

SaturnRecomp is not affiliated with or endorsed by Sega. Sega Saturn and game names are trademarks of their respective owners.

## What works today

- Authentic BIOS reset and IPL/CD boot path, including the boot animation and automatic game boot.
- Dual SH-2 scheduling, cache behavior, interrupts, FRT, SCU DMA, and SCU DSP execution.
- VDP1 command-list rasterization with clipping, mesh, color calculation, and double buffering.
- VDP2 cell/bitmap backgrounds, rotation parameters, windows, special priority, color calculation, and VDP1/VDP2 composition.
- MC68000 sound CPU, SCSP slots/DSP, CD-DA streaming, SMPC input, and persistent console settings.
- CUE/BIN and ISO disc access, ISO9660 inspection/extraction, IP.BIN parsing, and per-game TOML declarations.
- SDL2 window, keyboard/controller input, audio pacing, frame stepping, and diagnostic views.

This remains experimental. Compatibility is title-dependent, performance is below mature emulators such as Ymir, and SCSP DSP/audio behavior is not yet bit-perfect. See [Compatibility and limitations](docs/COMPATIBILITY.md).

## Desktop launcher

The native Windows launcher uses Qt Widgets through PySide6. Browse a cover-art
library, search and sort games, manage imports, and launch games from one window.
It does not require a browser or WebView2.

Start with the [launcher setup and play guide](docs/LAUNCHER.md), including
Windows build dependencies, NiGHTS setup, keyboard/controller controls, and
updating an existing library.

After installing the dependencies listed in that guide, run from PowerShell:

```powershell
python -m pip install -r launcher/requirements.txt
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_launcher.ps1
.\dist\SaturnRecomp\SaturnRecomp.exe
```

Choose **Console settings > Choose BIOS**, then **Add game**. For NiGHTS
and other mixed-mode discs, select the **CUE** with all its track files
available. When import finishes, select its cover and click **Play game** in
the selected-game area. Double-click a cover to open **Game details**.
Use **Enter** for Start, **Z** for Saturn A, and the arrow keys to move.

Imports prepare assets, an XML manifest and a game executable automatically.
Keep the original disc files available. The launcher uses the shared runtime;
import completion does not certify compatibility or full static recompilation.

## Command-line setup (optional)

Requirements: 64-bit Windows, PowerShell 5.1 or newer, MSYS2 MinGW-w64 GCC, and the MSYS2 SDL2 development package.

```powershell
# In an MSYS2 MINGW64 shell:
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2

# In PowerShell, from the repository root:
.\build.ps1
Copy-Item games\_template games\mygame -Recurse
```

Place your own dumps outside version control, edit `games\mygame\game.toml`, validate it, then launch:

```powershell
.\recompiler\saturnrecomp.exe inspect .\discs\mygame.cue
.\recompiler\saturnrecomp.exe modules .\games\mygame\game.toml
.\runner\saturnwin.exe .\games\mygame\game.toml
```

The full, field-by-field setup procedure is in [Setting up a game](docs/GAME_SETUP.md).
For a tested title-specific example, see [Sonic 3D Blast setup](docs/SONIC_3D_BLAST.md).

## Repository map

```text
external/sh2-recomp-core/   SH-2 ISA definitions and table-driven decoder
recompiler/                 Disc, ISO9660, IP.BIN, config, and inspection CLI
launcher/                   Native Qt Widgets library and disc-import workflow
runner/                     Saturn hardware runtime and SDL/headless frontends
tests/                      CPU, scheduler, video, sound, and decoder checks
tools/                      Generic inspection and release-audit utilities
games/_template/            The only game directory shipped by this repository
docs/                       Setup, architecture, compatibility, and legal policy
assets/                     Project-authored SaturnRecomp logo
```

## Command-line tools

```text
saturnrecomp inspect <disc.cue|bin|iso>
saturnrecomp extract <disc> </ISO/PATH> <outfile>
saturnrecomp disasm  <rawfile> <base-address> [instruction-count]
saturnrecomp modules <games/<name>/game.toml>

saturnwin  <games/<name>/game.toml> [nobios]
saturnboot <games/<name>/game.toml> [max-instructions] [nobios]
```

`nobios` selects incomplete HLE firmware stubs and is intended for diagnostics. Normal play should configure a valid BIOS dump so the real IPL establishes the machine and auto-boots the disc.

## Build and test

```powershell
.\build.ps1
.\tests\run_all.ps1
.\tools\audit_release.ps1
```

Set `SATURN_MINGW_BIN` if MinGW is not installed at `C:\msys64\mingw64\bin`. Decoder-oracle comparison additionally needs Python with `capstone` installed; the hardware/runtime tests do not.

The renderer regressions include per-character VDP2 foreground priority, VDP1/user-versus-system clipping, and VDP1/VDP2 tie behavior. The scheduler suite also asserts that a frontend frame ends at V-Blank-IN, matching the frame boundary used by Ymir.

## Documentation

- [Playing games through the launcher](docs/LAUNCHER.md)
- [Setting up a game](docs/GAME_SETUP.md)
- [Sonic 3D Blast setup](docs/SONIC_3D_BLAST.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Compatibility and limitations](docs/COMPATIBILITY.md)
- [Testing and diagnostics](docs/TESTING.md)
- [Firmware, game data, and release policy](docs/LEGAL.md)
- [Saturn hardware notes](docs/HARDWARE.md)

## Shared runtime optimization builds

`build.ps1` builds both `runner/saturnwin.exe` and `runner/saturnboot.exe`
from the same core objects. CPU wait-loop folding, CD polling, and SCSP/DSP
optimizations apply to every game. Frame pacing and audio-device buffering apply
to all games in the windowed frontend. Guest timing is preserved; this does not
force games that render at 30 FPS to generate 60 distinct pictures.

For a runtime-only build, run `tools/build_runtime.ps1`. Optional SH-2 PGO:

```powershell
./tools/build_runtime.ps1 -Profile generate
./tools/benchmark_runtime.ps1 -Exe out/runtime-build/saturnwin.exe -Game games/nights/game.toml
./tools/build_runtime.ps1 -Profile use
```

Keep the object and profile directories unchanged between generation and use.
Regenerate profiles after CPU source changes; missing profiles fail the build.
PGO influences host code layout, not game-specific emulation behavior. The normal
build requires no profile. To use collected profiles through `build.ps1`, set
`SATURN_PGO_MODE=use`, `SATURN_PGO_DIR`, and `SATURN_RUNTIME_OBJECT_DIR` to the
corresponding profile and object directories. Both frontends consume the same
profiled CPU object. Measurements on one game do not establish 60 FPS in all games.

### FM/LFO and high refresh presentation

FM feedback now uses the 64-word SCSP sound stack, including MDL/MDXSL/MDYSL,
STWINH, signed displacement and CPU SOUS access. Pitch and amplitude LFOs use
all four waveforms, all 32 rates, sensitivity levels, deterministic noise and
LFORE reset. These features are shared by both runners and all titles.

**F2** toggles native presentation / 120 Hz interpolation live in every game
using the Vulkan frontend. It preserves game state and clears stale interpolation
history when re-enabled. To choose the initial global presentation setting:

```powershell
$env:SATURN_PRESENT_HZ='120'
./runner/saturnwin.exe out/sonicr-check/game.toml
```

Remove `SATURN_PRESENT_HZ` to return to native presentation. Interpolation leaves
Saturn logic and audio clocks unchanged. A worker computes the next field while
the frontend presents immutable graphics snapshots. The renderer matches stable
primitive attributes and rejects ambiguous or changed-texture matches; HUD
sprites stay at their original positions. Repeated source fields retain their
geometry pair. Rotation-background mappings interpolate between two frozen
sets of parameters and coefficient tables. Live updates for the next picture
cannot move either endpoint halfway through the current pair; texture and
palette sampling still use the current field. Generated pictures restore the canonical GPU buffers
using GPU transfers, without host framebuffer readback or writes to guest RAM. Material buckets share reciprocal matching work while preserving the full
identity and ambiguity checks. Generated pictures reuse the canonical VRAM
upload instead of uploading the same snapshot at every presentation.

The title reports logic FPS separately from interpolation Hz. A high refresh
monitor is required to see all pictures. `SATURN_INTERP_CAPTURE` plus
`SATURN_INTERP_CAPTURE_FRAME` saves the generated pictures for a chosen field;
readback during that diagnostic capture can disturb pacing. Interpolation adds
buffering latency and does not recover original 3D depth or subpixel transforms
that the Saturn game already discarded. Loading stalls can still miss deadlines.

For a local visual-issue capture, set `SATURN_GEOMETRY_CAPTURE` to a file prefix
in an existing folder and `SATURN_GEOMETRY_START` to `18446744073709551615`
before launching. Press **F3** when the issue is visible to save 12 consecutive
graphics snapshots and draw lists. Capturing can briefly disturb pacing. These
files contain local game data and should stay outside source control.

The Vulkan presenter prefers mailbox mode where supported, avoiding tearing
without queuing several old pictures. `SATURN_PRESENT_MODE=immediate` selects
the previous presentation mode for diagnosis. Unsupported mailbox falls back
to immediate, then FIFO. Presentation-completion semaphores are owned by each
swapchain image, following [Khronos synchronization guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
These settings are shared by all games and both F2 states.

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

## Renderer probes

`SATURN_PIXELDBG=cycle:x:y` prints the VDP1 sprite code, each VDP2 layer's base/special/effective priority, color, and final layer order at one pixel. `SATURN_SPDBG=1` adds sprite-control and priority registers. These probes are gated and have no effect unless enabled.

`tools/render_state.c` can render a locally captured raw `saturn` state without advancing CPUs. Such snapshots contain BIOS/game memory and are intentionally ignored; never add them to a release.

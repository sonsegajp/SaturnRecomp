# Build script for SaturnRecomp.
#
# A native 64-bit MinGW-w64 GCC and SDL2 development package are required.
# Set SATURN_MINGW_BIN when they are not installed at the common MSYS2 path.
# NOTE: 'Continue', NOT 'Stop'. Under 'Stop', PowerShell 5.1 turns gcc's stderr
# WARNINGS into terminating NativeCommandError exceptions, aborting the build
# before the explicit $LASTEXITCODE check runs. Every gcc call is followed by
# an exit-code check that throws on real failure, so 'Continue' is both correct
# and warning-tolerant.
$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot

$mingwBin = $env:SATURN_MINGW_BIN
if (-not $mingwBin -and (Test-Path 'C:\msys64\mingw64\bin\gcc.exe')) {
    $mingwBin = 'C:\msys64\mingw64\bin'
}
if (-not $mingwBin) {
    $gccCommand = Get-Command gcc -ErrorAction SilentlyContinue
    if ($gccCommand) { $mingwBin = Split-Path -Parent $gccCommand.Source }
}
if (-not $mingwBin -or -not (Test-Path (Join-Path $mingwBin 'gcc.exe'))) {
    throw 'Native MinGW-w64 gcc.exe not found. Install MSYS2 mingw-w64-x86_64-gcc or set SATURN_MINGW_BIN.'
}
$env:Path = "$mingwBin;$env:Path"

$CFLAGS = @('-O3','-flto','-march=native','-mtune=native','-Wall','-Wextra','-std=c11')
$pgoMode = $env:SATURN_PGO_MODE
$pgoDir = $env:SATURN_PGO_DIR
if (-not $pgoMode) { $pgoMode = 'off' }
if (-not $pgoDir) { $pgoDir = 'out/runtime-build/profile' }
$runtimeObjects = $env:SATURN_RUNTIME_OBJECT_DIR
if (-not $runtimeObjects) { $runtimeObjects = 'out/runtime-build/obj' }
$CORE   = 'external\sh2-recomp-core\common'

New-Item -ItemType Directory -Force -Path 'out' | Out-Null

# ---- recompiler CLI (game-agnostic) ----------------------------------------

# Per-instruction execution tests for the interpreter. The decoder is checked
# against capstone, but capstone cannot execute -- these pin the semantics.
# Forward slashes on purpose: a Windows path like runner\srcus.c carries a
#  that some tooling eats as an escape.
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/sh2_semantics.exe tests/sh2_semantics.c runner/src/sh2_interp.c runner/src/bus.c runner/src/scu_dsp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/png.c `
    runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bios.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw 'sh2_semantics build failed' }

# Differential test: the hand-inlined fast dispatcher vs the reference
# interpreter. Hand-written cases cannot cover operand aliasing (m == n) or
# odd addresses; this generates programs until they disagree.
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/sh2_fastpath_fuzz.exe tests/sh2_fastpath_fuzz.c runner/src/sh2_interp.c runner/src/bus.c runner/src/scu_dsp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/png.c `
    runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bios.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw 'sh2_fastpath_fuzz build failed' }

gcc @CFLAGS -Irecompiler\include -o recompiler\saturnrecomp.exe `
    recompiler\src\main.c `
    recompiler\src\disc.c `
    recompiler\src\game_config.c `
    "$CORE\sh2_decoder.c"
if ($LASTEXITCODE -ne 0) { throw "saturnrecomp build failed" }

# ---- runner: boot harness (interpreter + bus) ------------------------------
# Both frontends are linked from the same core objects below.

gcc @CFLAGS -Irunner/include -Irecompiler/include tests/scsp_modulation.c runner/src/scsp.c runner/src/scsp_dsp.c -o tests/scsp_modulation.exe
if ($LASTEXITCODE -ne 0) { throw 'SCSP modulation build failed' }

gcc @CFLAGS -Irunner/include -Irecompiler/include -o tests/audio_ring.exe tests/audio_ring.c
if ($LASTEXITCODE -ne 0) { throw "audio_ring build failed" }

gcc @CFLAGS -Irunner/include -o tests/frame_pacing.exe tests/frame_pacing.c
if ($LASTEXITCODE -ne 0) { throw "frame_pacing build failed" }

gcc @CFLAGS -Irunner/include -o tests/geometry_interp.exe tests/geometry_interp.c
if ($LASTEXITCODE -ne 0) { throw "geometry_interp build failed" }

# ---- tests: VDP2 cell renderer -------------------------------------------
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/scsp_effects.exe tests/scsp_effects.c runner/src/scsp.c runner/src/scsp_dsp.c
if ($LASTEXITCODE -ne 0) { throw 'scsp_effects build failed' }

gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/vdp2_cell.exe tests/vdp2_cell.c runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bus.c runner/src/scu_dsp.c runner/src/sh2_interp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw "vdp2_cell build failed" }

gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/vdp2_effects.exe tests/vdp2_effects.c runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bus.c runner/src/scu_dsp.c runner/src/sh2_interp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw 'vdp2_effects build failed' }

# ---- tests: dual-CPU scheduler, per-core on-chip banks, FRT ---------------
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/dual_cpu.exe tests/dual_cpu.c runner/src/sh2_interp.c runner/src/bus.c runner/src/scu_dsp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw "dual_cpu build failed" }

# ---- tests: MC68000 sound CPU + SCSP --------------------------------------
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/m68k_core.exe tests/m68k_core.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bus.c runner/src/scu_dsp.c runner/src/sh2_interp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/vdp2.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw "m68k_core build failed" }

# ---- tests: VDP1 rasteriser -----------------------------------------------
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/vdp1_quad.exe tests/vdp1_quad.c runner/src/vdp1.c runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bus.c runner/src/scu_dsp.c runner/src/sh2_interp.c runner/src/cdblock.c runner/src/smpc.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw "vdp1_quad build failed" }

# ---- tests: CD-DA playback path -------------------------------------------
# "The music does not play" can fail in the disc layer, the CD block or the
# SCSP, and they look identical from outside. This drives all three.
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/cdda_play.exe tests/cdda_play.c runner/src/cdblock.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/bus.c runner/src/scu_dsp.c runner/src/sh2_interp.c runner/src/smpc.c runner/src/vdp1.c runner/src/vdp2.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw "cdda_play build failed" }

# ---- tests: shared address bus and CD/CS2 decoder --------------------------
$BUS_TEST_SRCS = @('runner/src/bus.c','runner/src/scu_dsp.c','runner/src/sh2_interp.c',
    'runner/src/cdblock.c','runner/src/smpc.c','runner/src/vdp1.c','runner/src/vdp2.c',
    'runner/src/m68k.c','runner/src/m68k_bus.c','runner/src/scsp.c','runner/src/scsp_dsp.c',
    'runner/src/sound.c','runner/src/bios.c','runner/src/png.c','recompiler/src/disc.c',
    'external/sh2-recomp-core/common/sh2_decoder.c')
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/sh2_waitloop.exe tests/sh2_waitloop.c @BUS_TEST_SRCS
if ($LASTEXITCODE -ne 0) { throw "sh2_waitloop build failed" }

gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/bus_alias.exe tests/bus_alias.c @BUS_TEST_SRCS
if ($LASTEXITCODE -ne 0) { throw "bus_alias build failed" }
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/cd_bus.exe tests/cd_bus.c @BUS_TEST_SRCS
if ($LASTEXITCODE -ne 0) { throw "cd_bus build failed" }
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/smpc_pad.exe tests/smpc_pad.c @BUS_TEST_SRCS
if ($LASTEXITCODE -ne 0) { throw "smpc_pad build failed" }

# ---- runner: SDL2 window ---------------------------------------------------
$glslc = Join-Path $mingwBin 'glslc.exe'
if (-not (Test-Path $glslc)) {
    throw 'glslc.exe not found. Install mingw-w64-x86_64-shaderc.'
}
& $glslc runner\shaders\vdp1.comp -O -o runner\shaders\vdp1.comp.spv
if ($LASTEXITCODE -ne 0) { throw "VDP1 Vulkan shader build failed" }
& $glslc runner\shaders\vdp2.comp -O -o runner\shaders\vdp2.comp.spv
if ($LASTEXITCODE -ne 0) { throw "VDP2 Vulkan shader build failed" }

$env:SATURN_MINGW_BIN = $mingwBin
& powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_runtime.ps1 -Profile $pgoMode -ProfileDir $pgoDir -ObjectDir $runtimeObjects -Output runner/saturnwin.exe -BootOutput runner/saturnboot.exe
if ($LASTEXITCODE -ne 0) { throw 'shared runtime build failed' }
# SDL2.dll must sit beside the executable when its directory is not on the
# runtime PATH. The MSYS2 package places it beside gcc.exe.
$sdlDll = Join-Path $mingwBin 'SDL2.dll'
if (-not (Test-Path $sdlDll)) {
    throw "SDL2.dll not found in $mingwBin. Install mingw-w64-x86_64-SDL2."
}
Copy-Item $sdlDll 'runner\SDL2.dll' -Force

# ---- decoder conformance harness -------------------------------------------
gcc @CFLAGS -o tests\dump_all.exe tests\dump_all.c "$CORE\sh2_decoder.c"
if ($LASTEXITCODE -ne 0) { throw "dump_all build failed" }

Write-Host "build ok" -ForegroundColor Green

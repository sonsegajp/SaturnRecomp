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
if ($pgoMode) {
    if (-not $pgoDir) { $pgoDir = 'out\pgo' }
    New-Item -ItemType Directory -Force -Path $pgoDir | Out-Null
    $pgoAbs = (Resolve-Path $pgoDir).Path -replace '\\','/'
    if ($pgoMode -eq 'generate') {
        $CFLAGS += "-fprofile-generate=$pgoAbs"
    } elseif ($pgoMode -eq 'use') {
        $CFLAGS += "-fprofile-use=$pgoAbs"
        $CFLAGS += '-fprofile-correction'
        $CFLAGS += '-Wno-missing-profile'
    } else {
        throw "SATURN_PGO_MODE must be 'generate' or 'use'"
    }
}
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
gcc @CFLAGS '-Wl,--stack,67108864' -Irunner\include -Irecompiler\include -o runner\saturnboot.exe `
    runner\src\boot.c `
    runner\src\bus.c `
    runner\src\scu_dsp.c `
    runner\src\sh2_interp.c `
    runner\src\bios.c `
    runner\src\cdblock.c `
    runner\src\vdp1.c `
    runner\src\debugview.c `
    runner\src\vdp2.c `
    runner\src\m68k.c `
    runner\src\m68k_bus.c `
    runner\src\scsp.c runner\src\scsp_dsp.c `
    runner\src\sound.c `
    runner\src\png.c `
    runner\src\smpc.c `
    recompiler\src\disc.c `
    recompiler\src\game_config.c `
    "$CORE\sh2_decoder.c"
if ($LASTEXITCODE -ne 0) { throw "saturnboot build failed" }

# ---- tests: VDP2 cell renderer -------------------------------------------
gcc @CFLAGS -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common -o tests/vdp2_cell.exe tests/vdp2_cell.c runner/src/vdp2.c runner/src/m68k.c runner/src/m68k_bus.c runner/src/scsp.c runner/src/scsp_dsp.c runner/src/sound.c runner/src/bus.c runner/src/scu_dsp.c runner/src/sh2_interp.c runner/src/cdblock.c runner/src/smpc.c runner/src/vdp1.c runner/src/bios.c runner/src/png.c recompiler/src/disc.c external/sh2-recomp-core/common/sh2_decoder.c
if ($LASTEXITCODE -ne 0) { throw "vdp2_cell build failed" }

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

# ---- runner: SDL2 window ---------------------------------------------------
gcc @CFLAGS '-Wl,--stack,67108864' -Irunner\include -Irecompiler\include -o runner\saturnwin.exe `
    runner\src\window.c `
    runner\src\bus.c `
    runner\src\scu_dsp.c `
    runner\src\sh2_interp.c `
    runner\src\bios.c `
    runner\src\cdblock.c `
    runner\src\vdp1.c `
    runner\src\debugview.c `
    runner\src\vdp2.c `
    runner\src\m68k.c `
    runner\src\m68k_bus.c `
    runner\src\scsp.c runner\src\scsp_dsp.c `
    runner\src\sound.c `
    runner\src\png.c `
    runner\src\smpc.c `
    recompiler\src\disc.c `
    recompiler\src\game_config.c `
    "$CORE\sh2_decoder.c" `
    -lmingw32 -lSDL2main -lSDL2
if ($LASTEXITCODE -ne 0) { throw "saturnwin build failed" }
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

# run_all.ps1 -- SaturnRecomp test suite.
#
# Two layers, both game-agnostic:
#
#   1. ISA conformance. Our SH-2 decoder vs capstone CS_MODE_SH2 big-endian
#      over the entire 16-bit opcode space. Must be 0 failures.
#
#   2. Corpus sweep. Every games/*/game.toml is loaded and resolved against
#      its disc: header validated, product number checked, every declared
#      module located. Games whose disc image is not present are SKIPped, so
#      the suite stays green on a machine that only has some of the discs.
#
# Add a game by dropping in games/<name>/game.toml -- no C changes, and it is
# picked up here automatically.

$ErrorActionPreference = 'Continue'
Set-Location (Join-Path $PSScriptRoot '..')

# SATURN_PYTHON can select a Python installation that has `capstone`.
$PY = $env:SATURN_PYTHON
if (-not $PY) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) { $PY = $pythonCommand.Source }
}

$fail = 0
$skip = 0
$pass = 0

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# ------------------------------------------------- 0. execution semantics ----
Section 'SH-2 execution semantics (per-instruction, vs the SH-2 manual)'

if (Test-Path 'tests\sh2_semantics.exe') {
    $out = & 'tests\sh2_semantics.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'sh2_semantics.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ---------------------------- 0a2. SH-2 fast path vs reference interpreter ---
Section 'SH-2 fast path vs reference interpreter (randomised differential)'

if (Test-Path 'tests\sh2_fastpath_fuzz.exe') {
    $out = & 'tests\sh2_fastpath_fuzz.exe' 20000 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'sh2_fastpath_fuzz.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ------------------------------------------------ 0b. VDP2 cell renderer ----
Section 'VDP2 cell renderer (known layout, vs the VDP2 manual)'

if (Test-Path 'tests/vdp2_cell.exe') {
    $out = & 'tests/vdp2_cell.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'vdp2_cell.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ------------------------------------------------ 0b2. VDP1 rasteriser ------
Section 'VDP1 rasteriser (hand-built command lists, vs the VDP1 manual)'

if (Test-Path 'tests/vdp1_quad.exe') {
    $out = & 'tests/vdp1_quad.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'vdp1_quad.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ------------------------------------- 0c. dual-CPU scheduler / on-chip -----
Section 'Dual-CPU scheduler, per-core on-chip banks, FRT (vs the SH7604 manual)'

if (Test-Path 'tests/dual_cpu.exe') {
    $out = & 'tests/dual_cpu.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'dual_cpu.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ------------------------------------------ 0d. MC68000 sound CPU + SCSP ----
Section 'MC68000 sound CPU and SCSP (vs the 68000 and SCSP manuals)'

if (Test-Path 'tests/m68k_core.exe') {
    $out = & 'tests/m68k_core.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'm68k_core.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ------------------------------------------ 0e. shared address bus / CD ----
Section 'Shared address bus aliases'

if (Test-Path 'tests/bus_alias.exe') {
    $out = & 'tests/bus_alias.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'bus_alias.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

Section 'CD/CS2 mirrors, widths, FIFO effects, and trace classification'

if (Test-Path 'tests/cd_bus.exe') {
    $out = & 'tests/cd_bus.exe' 2>&1
    $out | Write-Host
    if ($LASTEXITCODE -eq 0) { $pass++ } else { $fail++ }
} else {
    Write-Host 'cd_bus.exe not built - run build.ps1' -ForegroundColor Yellow
    $skip++
}

# ---------------------------------------------------------------- 1. ISA ----
Section 'SH-2 decoder conformance (65,536 opcodes vs capstone)'

if (-not $PY) {
    Write-Host 'SKIP  Python not found; install Python and capstone for oracle comparison' -ForegroundColor Yellow
    $skip++
} else {
    & $PY tests\oracle\capstone_sh2_dump.py tests\oracle_sh2.txt
}
if (-not $PY -or $LASTEXITCODE -ne 0) {
    if ($PY) {
    Write-Host 'SKIP  capstone oracle unavailable' -ForegroundColor Yellow
    $skip++
    }
} else {
    .\tests\dump_all.exe tests\ours_sh2.txt
    & $PY tests\oracle\compare.py
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL  decoder conformance' -ForegroundColor Red; $fail++ }
    else                     { Write-Host 'PASS  decoder conformance' -ForegroundColor Green; $pass++ }
}

# ------------------------------------------------------------- 2. corpus ----
Section 'Game corpus'

$games = Get-ChildItem -Path 'games' -Directory -ErrorAction SilentlyContinue |
         Where-Object { Test-Path (Join-Path $_.FullName 'game.toml') }

if (-not $games) { Write-Host 'no games declared' -ForegroundColor Yellow }

foreach ($g in $games) {
    $toml = Join-Path $g.FullName 'game.toml'
    $name = $g.Name

    # Find the declared disc so we can SKIP cleanly when it is absent.
    $discLine = (Select-String -Path $toml -Pattern '^\s*disc\s*=' | Select-Object -First 1).Line
    $discRel  = if ($discLine -match '"([^"]+)"') { $Matches[1] } else { $null }
    # game.toml may name the disc absolutely; Join-Path would then build a
    # nonsense path and the game SKIPped even though the emulator loads it.
    $discPath = if (-not $discRel) { $null }
                elseif ([System.IO.Path]::IsPathRooted($discRel)) { $discRel }
                else { Join-Path $g.FullName $discRel }

    if (-not $discPath -or -not (Test-Path $discPath)) {
        Write-Host ("SKIP  {0,-14} disc image not present" -f $name) -ForegroundColor Yellow
        $skip++
        continue
    }

    $out = .\recompiler\saturnrecomp.exe modules $toml 2>&1
    if ($LASTEXITCODE -eq 0) {
        $n = ($out | Select-String 'module\(s\) declared').Line
        Write-Host ("PASS  {0,-14} {1}" -f $name, $n.Trim()) -ForegroundColor Green
        $pass++
    } else {
        Write-Host ("FAIL  {0,-14}" -f $name) -ForegroundColor Red
        $out | ForEach-Object { Write-Host "        $_" }
        $fail++
    }
}

# ---------------------------------------------------------------- summary ---
Section 'Summary'
Write-Host ("pass {0}   skip {1}   fail {2}" -f $pass, $skip, $fail)
if ($fail -gt 0) { exit 1 }
exit 0

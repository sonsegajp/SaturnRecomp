# Build the focused timing sources against an existing complete core build.
param(
    [string]$ObjectDir='out/runtime-build/obj',
    [string]$OutputDir='out/sound-bus-timing-test',
    [switch]$UseExistingTimingObjects
)
$ErrorActionPreference='Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$timingMingw=$env:SATURN_MINGW_BIN
if(-not $timingMingw){$timingMingw='C:/msys64/mingw64/bin'}
$env:PATH=$timingMingw+';'+$env:PATH
$timingNames=@('bios','bus','cdblock','disc','m68k','m68k_bus','png','scsp',
    'scsp_dsp','scu_dsp','sh2_decoder','sh2_interp','smpc','sound','vdp1','vdp2')
$timingObjects=@()
foreach($name in $timingNames) {
    $path=Join-Path $ObjectDir ($name+'.o')
    if(-not (Test-Path -LiteralPath $path)){throw "Missing $path; build the runtime first."}
    if($UseExistingTimingObjects -or $name -notin @('bus','sh2_interp')){$timingObjects+=$path}
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$timingFlags=@('-O3','-flto=4','-std=c11','-Irunner/include','-Irecompiler/include','-Iexternal/sh2-recomp-core/common')
if(-not $UseExistingTimingObjects) {
    foreach($name in @('bus','sh2_interp')) {
        $object=Join-Path $OutputDir ($name+'.o')
        & gcc @timingFlags -c ("runner/src/$name.c") -o $object
        if($LASTEXITCODE){throw "Timing source compile failed: $name"}
        $timingObjects+=$object
    }
}
$timingExe=Join-Path $OutputDir 'sound_bus_timing.exe'
& gcc @timingFlags tests/sound_bus_timing.c @timingObjects -lm -o $timingExe
if($LASTEXITCODE){throw 'Sound bus timing regression build failed'}
& $timingExe
$timingResult=$LASTEXITCODE
& $timingExe --diagnostics
if($LASTEXITCODE){$timingResult=$LASTEXITCODE}
exit $timingResult

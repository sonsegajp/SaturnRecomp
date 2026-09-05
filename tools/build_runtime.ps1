# Build the runtime with a reusable object cache and optional SH-2 gameplay PGO.
# Example:
#   ./tools/build_runtime.ps1 -Profile generate
#   ./tools/benchmark_runtime.ps1 -Exe out/runtime-build/saturnwin.exe -Game <game.toml>
#   ./tools/build_runtime.ps1 -Profile use
# Keep ObjectDir stable between generation and use; regenerate after CPU edits.
param(
    [ValidateSet('off','generate','use')][string]$Profile='off',
    [string]$ObjectDir='out/runtime-build/obj',
    [string]$ProfileDir='out/runtime-build/profile',
    [string]$Output='out/runtime-build/saturnwin.exe',
    [string]$BootOutput='out/runtime-build/saturnboot.exe'
)
$ErrorActionPreference='Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$mingw=$env:SATURN_MINGW_BIN
if(-not $mingw){$mingw='C:\msys64\mingw64\bin'}
$env:PATH=$mingw+';'+$env:PATH
$flags=@('-O3','-flto','-march=native','-mtune=native','-std=c11','-Irunner/include','-Irecompiler/include','-Iexternal/sh2-recomp-core/common')
New-Item -ItemType Directory -Force -Path $ObjectDir,$ProfileDir,(Split-Path -Parent $Output),(Split-Path -Parent $BootOutput) | Out-Null
$profileAbs=(Resolve-Path $ProfileDir).Path.Replace('\','/')
if($Profile -eq 'use' -and -not (Get-ChildItem $ProfileDir -Recurse -Filter '*.gcda')){throw 'No gameplay profile found; generate, run a game, then use.'}
$cpuProfile=@()
if($Profile -ne 'off'){$cpuProfile+="-fprofile-$Profile=$profileAbs"}
if($Profile -eq 'use'){$cpuProfile+=@('-fprofile-correction','-Werror=missing-profile')}
$sources=@('bus','scu_dsp','sh2_interp','bios','cdblock','vdp1','debugview','vdp2','m68k','m68k_bus','scsp','scsp_dsp','sound','png','smpc','boot','window','vulkan_renderer') | ForEach-Object {"runner/src/$_.c"}
$sources+=@('recompiler/src/disc.c','recompiler/src/game_config.c','external/sh2-recomp-core/common/sh2_decoder.c')
$objects=@()
foreach($source in $sources){
    $name=[IO.Path]::GetFileNameWithoutExtension($source)
    $object="$ObjectDir/$name.o"
    $extra=@()
    if($name -eq 'sh2_interp'){$extra=$cpuProfile}
    & gcc @flags @extra -c $source -o $object
    if($LASTEXITCODE){throw "compile failed: $source"}
    $objects+=$object
}
$link=@()
if($Profile -eq 'generate'){$link+='-lgcov'}
$windowObjects=@($objects | Where-Object { [IO.Path]::GetFileName($_) -ne 'boot.o' })
$bootObjects=@($objects | Where-Object { [IO.Path]::GetFileName($_) -notin @('window.o','vulkan_renderer.o') })
& gcc @flags '-Wl,--stack,67108864' @bootObjects @link -o $BootOutput
if($LASTEXITCODE){throw 'boot runtime link failed'}
& gcc @flags '-Wl,--stack,67108864' @windowObjects @link -lmingw32 -lSDL2main -lSDL2 -lvulkan-1 -o $Output
if($LASTEXITCODE){throw 'runtime link failed'}
Write-Output "Built $BootOutput and $Output (SH-2 profile: $Profile)"
exit 0

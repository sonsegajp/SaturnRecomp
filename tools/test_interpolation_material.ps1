# Focused actual-Vulkan regression; run build.ps1 first for core objects/shaders.
param([string]$ObjectDir='out/runtime-build/obj')
$ErrorActionPreference='Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$materialMingw=$env:SATURN_MINGW_BIN
if(-not $materialMingw){$materialMingw='C:/msys64/mingw64/bin'}
$env:PATH=$materialMingw+';'+$env:PATH
$materialObjects=@(Get-ChildItem "$ObjectDir/*.o" | Where-Object Name -NotIn @('boot.o','window.o','vulkan_renderer.o') | ForEach-Object FullName)
if(-not $materialObjects){throw "Runtime objects missing in $ObjectDir; run build.ps1 first."}
New-Item -ItemType Directory -Force -Path 'out/interpolation-material-test' | Out-Null
$materialExe='out/interpolation-material-test/test.exe'
& gcc -O3 -flto -std=c11 -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common tests/interpolation_material.c @materialObjects -lSDL2 -lvulkan-1 '-Wl,--stack,67108864' -o $materialExe
if($LASTEXITCODE){throw 'Interpolation material test build failed'}
& $materialExe
$materialResult=$LASTEXITCODE
if($materialResult -eq 77){Write-Host 'SKIP: a working SDL/Vulkan graphics device is required.'}
exit $materialResult

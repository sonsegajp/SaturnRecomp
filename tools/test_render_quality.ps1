# Run after build.ps1 (or point ObjectDir at a complete current runtime build).
param([string]$ObjectDir='out/runtime-build/obj')
$ErrorActionPreference='Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$qualityMingw=$env:SATURN_MINGW_BIN
if(-not $qualityMingw){$qualityMingw='C:/msys64/mingw64/bin'}
$env:PATH=$qualityMingw+';'+$env:PATH
$qualityObjects=@(Get-ChildItem "$ObjectDir/*.o" | Where-Object Name -NotIn @('boot.o','window.o','vulkan_renderer.o') | ForEach-Object FullName)
if(-not $qualityObjects){throw "Runtime objects missing in $ObjectDir; run build.ps1 first."}
New-Item -ItemType Directory -Force -Path 'out/render-quality-test' | Out-Null
$qualityExe='out/render-quality-test/test.exe'
& gcc -O3 -flto -std=c11 -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common tests/render_quality.c @qualityObjects -lSDL2 -lvulkan-1 '-Wl,--stack,67108864' -o $qualityExe
if($LASTEXITCODE){throw 'Render quality test build failed'}
& $qualityExe
$qualityResult=$LASTEXITCODE
if($qualityResult -eq 77){Write-Host 'SKIP: a working SDL/Vulkan graphics device is required.'}
exit $qualityResult

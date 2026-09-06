param([string]$ObjectDir='out/runtime-build/obj')
$ErrorActionPreference='Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
$interlaceMingw=$env:SATURN_MINGW_BIN
if(-not $interlaceMingw){$interlaceMingw='C:/msys64/mingw64/bin'}
$env:PATH=$interlaceMingw+';'+$env:PATH
$interlaceObjects=@(Get-ChildItem "$ObjectDir/*.o" | Where-Object Name -NotIn @('boot.o','window.o','vulkan_renderer.o') | ForEach-Object FullName)
if(-not $interlaceObjects){throw "Runtime objects missing in $ObjectDir"}
New-Item -ItemType Directory -Force -Path 'out/vdp1-interlace-test' | Out-Null
& gcc -O3 -flto -std=c11 -DSATURN_TEST_VULKAN -Irunner/include -Irecompiler/include -Iexternal/sh2-recomp-core/common tests/vdp1_interlace.c @interlaceObjects -lSDL2 -lvulkan-1 '-Wl,--stack,67108864' -o out/vdp1-interlace-test/test.exe
if($LASTEXITCODE){throw 'VDP1 interlace test build failed'}
& out/vdp1-interlace-test/test.exe
$interlaceResult=$LASTEXITCODE
if($interlaceResult -eq 77){Write-Host 'SKIP: a working SDL/Vulkan graphics device is required.'}
exit $interlaceResult

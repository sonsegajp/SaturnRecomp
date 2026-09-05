param([switch]$SkipRuntime, [switch]$ToolsOnly, [string]$Python=$env:SATURN_PYTHON)
$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
if (-not $Python) { $Python = (Get-Command python -ErrorAction Stop).Source }
$mingw = $env:SATURN_MINGW_BIN
if (-not $mingw) { $mingw = 'C:\msys64\mingw64\bin' }
$env:PATH = "$mingw;$env:PATH"
New-Item -ItemType Directory -Force -Path out/launcher-tools | Out-Null
if (-not $SkipRuntime -and -not $ToolsOnly) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/build_runtime.ps1 -Output runner/saturnwin.exe -BootOutput runner/saturnboot.exe
    if ($LASTEXITCODE) { throw 'Runtime build failed' }
    & "$mingw/glslc.exe" runner/shaders/vdp1.comp -O -o runner/shaders/vdp1.comp.spv
    if ($LASTEXITCODE) { throw 'VDP1 shader build failed' }
    & "$mingw/glslc.exe" runner/shaders/vdp2.comp -O -o runner/shaders/vdp2.comp.spv
    if ($LASTEXITCODE) { throw 'VDP2 shader build failed' }
}
& "$mingw/gcc.exe" -O2 -std=c11 -Irecompiler/include launcher/import_disc.c recompiler/src/disc.c -o out/launcher-tools/saturn-import.exe
if ($LASTEXITCODE) { throw 'Disc importer build failed' }
& "$mingw/gcc.exe" -O2 -std=c11 -municode -mwindows launcher/game_entry.c -o out/launcher-tools/saturn-game.exe
if ($LASTEXITCODE) { throw 'Game executable build failed' }
if ($ToolsOnly) { exit 0 }
& $Python -c 'from PySide6 import QtCore, QtGui, QtWidgets; import PyInstaller, PIL'
if ($LASTEXITCODE) { throw 'Install launcher/requirements.txt with Python before packaging.' }
# Derive the Windows icon from the existing project logo; no external assets.
& $Python -c "from PIL import Image; im=Image.open('assets/saturnrecomp-logo.png').convert('RGBA'); im.thumbnail((256,256)); icon=Image.new('RGBA',(256,256)); icon.alpha_composite(im,((256-im.width)//2,(256-im.height)//2)); icon.save('out/launcher-tools/saturnrecomp.ico')"
if ($LASTEXITCODE) { throw 'Application icon generation failed' }
$bundleArgs = @('--noconfirm', '--clean', '--windowed', '--onedir', '--name', 'SaturnRecomp',
    '--distpath', 'out/launcher-package', '--workpath', 'launcher/.build', '--specpath', 'launcher/.build',
    '--icon', 'out/launcher-tools/saturnrecomp.ico',
    '--add-data', 'assets/saturnrecomp-logo.png;assets',
    '--add-data', 'runner/shaders/*.spv;runtime/shaders',
    '--add-binary', 'runner/saturnwin.exe;runtime', '--add-binary', "$mingw/SDL2.dll;runtime",
    '--add-binary', 'out/launcher-tools/saturn-import.exe;runtime',
    '--add-binary', 'out/launcher-tools/saturn-game.exe;runtime',
    '--exclude-module', 'PyQt5', '--exclude-module', 'PyQt6', '--exclude-module', 'PySide2',
    '--exclude-module', 'PySide6.QtWebEngineCore', '--exclude-module', 'PySide6.QtWebEngineWidgets',
    '--exclude-module', 'PySide6.QtWebEngineQuick', '--exclude-module', 'PySide6.QtWebView',
    '--exclude-module', 'webview', '--exclude-module', 'clr', '--exclude-module', 'pythonnet',
    '--exclude-module', 'torch', '--exclude-module', 'tensorflow',
    'launcher/app.py')
# PyInstaller resolves bundled inputs relative to the generated spec file.
for ($i = 1; $i -lt $bundleArgs.Count; $i++) {
    if ($bundleArgs[$i-1] -in @('--add-data', '--add-binary')) {
        $parts = $bundleArgs[$i].Split(';', 2)
        if (-not [IO.Path]::IsPathRooted($parts[0])) { $parts[0] = Join-Path (Get-Location).Path $parts[0] }
        $bundleArgs[$i] = $parts -join ';'
    } elseif ($bundleArgs[$i-1] -eq '--icon') {
        $bundleArgs[$i] = Join-Path (Get-Location).Path $bundleArgs[$i]
    }
}
& $Python -m PyInstaller @bundleArgs
if ($LASTEXITCODE) { throw 'Launcher packaging failed' }
# Replace the application as a unit so obsolete browser DLLs cannot survive.
# The adjacent library (including media, BIOS, saves and settings) is untouched.
$workspace = (Get-Location).Path
$packageRoot = (Resolve-Path -LiteralPath out/launcher-package/SaturnRecomp).Path
$destination = [IO.Path]::GetFullPath((Join-Path $workspace 'dist/SaturnRecomp'))
$expectedDestination = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../dist/SaturnRecomp'))
if ($destination -ne $expectedDestination) { throw 'Unexpected launcher destination; refusing to refresh it.' }
$appNames = @('SaturnRecomp.exe', '_internal')
foreach ($name in $appNames) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $name))) { throw "Package is incomplete: $name" }
}
$running = Get-Process -Name SaturnRecomp -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -eq (Join-Path $destination 'SaturnRecomp.exe')
}
if ($running) { throw 'Close the running SaturnRecomp launcher and rerun this build to refresh the package.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$backupBase = [IO.Path]::GetFullPath((Join-Path $workspace 'out/launcher-previous'))
$backup = Join-Path $backupBase ([Guid]::NewGuid().ToString('N'))
if (-not $backup.StartsWith($backupBase + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Unexpected launcher backup destination.'
}
New-Item -ItemType Directory -Force -Path $backup | Out-Null
$preserved = @()
$installed = @()
try {
    foreach ($name in $appNames) {
        $target = [IO.Path]::GetFullPath((Join-Path $destination $name))
        if ([IO.Path]::GetDirectoryName($target) -ne $expectedDestination) { throw 'Unsafe application refresh path.' }
        if (Test-Path -LiteralPath $target) {
            Move-Item -LiteralPath $target -Destination $backup
            $preserved += $name
        }
    }
    foreach ($name in $appNames) {
        $installed += $name
        Copy-Item -LiteralPath (Join-Path $packageRoot $name) -Destination $destination -Recurse -Force
    }
} catch {
    foreach ($name in $installed) {
        $target = [IO.Path]::GetFullPath((Join-Path $destination $name))
        if ([IO.Path]::GetDirectoryName($target) -ne $expectedDestination) { throw 'Unsafe application rollback path.' }
        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    }
    foreach ($name in $preserved) {
        Move-Item -LiteralPath (Join-Path $backup $name) -Destination $destination
    }
    throw
}
# Only this checked, temporary application backup is removed after a good copy.
if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($backup)) -ne $backupBase) { throw 'Unsafe launcher backup cleanup path.' }
Remove-Item -LiteralPath $backup -Recurse -Force
Write-Output 'Launcher ready: dist/SaturnRecomp/SaturnRecomp.exe'

# ymir_series.ps1 -- launch Ymir on a disc and take periodic memory dumps.
param(
  [string]$Disc = "",
  [Parameter(Mandatory=$true)][string]$YmirDir,
  [int]$Count = 30,
  [int]$IntervalMs = 2000,
  [int]$WarmupMs = 3000,
  [string]$OutDir = (Join-Path $PSScriptRoot "../shots/ymir_series"),
  [switch]$NoLaunch
)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path -LiteralPath (Join-Path $YmirDir 'ymir-sdl3.exe'))) { throw 'YmirDir must contain ymir-sdl3.exe' }
$dumps   = Join-Path $ymirDir 'dumps'
$tools   = $PSScriptRoot

if (-not $NoLaunch) {
  if (-not $Disc -or -not (Test-Path -LiteralPath $Disc)) { throw 'Specify -Disc when launching Ymir.' }
  Get-Process ymir-sdl3 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 800
  Start-Process "$ymirDir\ymir-sdl3.exe" -WindowStyle Hidden -ArgumentList @('-d', "`"$Disc`"")
  Start-Sleep -Milliseconds $WarmupMs
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Count; $i++) {
  $tag = '{0:d3}' -f $i
  $ms  = [int]$sw.ElapsedMilliseconds
  # clear old dumps so we know the new ones landed
  Remove-Item (Join-Path $dumps 'wram-lo.bin')  -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $dumps 'wram-hi.bin')  -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $dumps 'vdp2-vram.bin') -ErrorAction SilentlyContinue
  & powershell -ExecutionPolicy Bypass -File "$tools\ymir_key.ps1" -Keys F11 -Ctrl | Out-Null
  Start-Sleep -Milliseconds 700
  $d = Join-Path $OutDir "t$tag"
  New-Item -ItemType Directory -Force -Path $d | Out-Null
  foreach ($f in @('wram-lo.bin','wram-hi.bin','vdp2-vram.bin','vdp1-vram.bin','cdb-dram.bin')) {
    $src = Join-Path $dumps $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $d $f) -Force }
  }
  & powershell -ExecutionPolicy Bypass -File "$tools\ymir_capture.ps1" -Out (Join-Path $d 'screen.png') | Out-Null
  Set-Content -Path (Join-Path $d 'time.txt') -Value "$ms" -Encoding ascii
  Write-Output "t$tag at ${ms}ms"
  Start-Sleep -Milliseconds $IntervalMs
}
Write-Output "done"

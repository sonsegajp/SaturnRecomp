# ymir_fine.ps1 -- high-rate memory dumps of Ymir across a wall-clock window.
# Everything is in-process (no per-dump powershell spawn) so the sample period
# is ~600ms instead of ~3.6s.
param(
  [string]$Disc = "",
  [Parameter(Mandatory=$true)][string]$YmirDir,
  [int]$WarmupMs = 14000,
  [int]$Count = 60,
  [int]$IntervalMs = 0,
  [string]$OutDir = (Join-Path $PSScriptRoot "../shots/ymir_fine"),
  [switch]$NoLaunch,
  [switch]$NoShot
)
$ErrorActionPreference='Continue'
$sig = @'
using System;
using System.Runtime.InteropServices;
public class YF {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }
}
'@
Add-Type -TypeDefinition $sig -ReferencedAssemblies System.Drawing,System.Drawing.Primitives
Add-Type -AssemblyName System.Drawing

if (-not (Test-Path -LiteralPath (Join-Path $YmirDir 'ymir-sdl3.exe'))) { throw 'YmirDir must contain ymir-sdl3.exe' }
$dumps   = Join-Path $ymirDir 'dumps'

if (-not $NoLaunch) {
  if (-not $Disc -or -not (Test-Path -LiteralPath $Disc)) { throw 'Specify -Disc when launching Ymir.' }
  Get-Process ymir-sdl3 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 900
  Start-Process "$ymirDir\ymir-sdl3.exe" -WindowStyle Hidden -ArgumentList @('-d', "`"$Disc`"")
}
$sw = [System.Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Milliseconds 2500
$p = Get-Process ymir-sdl3 -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "ymir not running"; exit 1 }
$h = $p.MainWindowHandle

function Focus-Ymir {
  if ([YF]::GetForegroundWindow() -eq $h) { return }
  $fg  = [YF]::GetForegroundWindow()
  $fgT = [YF]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
  $myT = [YF]::GetCurrentThreadId()
  [YF]::AttachThreadInput($myT, $fgT, $true) | Out-Null
  [YF]::ShowWindow($h, 9) | Out-Null
  [YF]::BringWindowToTop($h) | Out-Null
  [YF]::SetForegroundWindow($h) | Out-Null
  [YF]::SetFocus($h) | Out-Null
  [YF]::AttachThreadInput($myT, $fgT, $false) | Out-Null
  Start-Sleep -Milliseconds 200
}
function Key([int]$code, [bool]$ctrl=$false, [int]$hold=50) {
  $scan = [byte]([YF]::MapVirtualKey([uint32]$code, 0))
  if ($ctrl) { [YF]::keybd_event(0x11, [byte][YF]::MapVirtualKey(0x11,0), 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 30 }
  [YF]::keybd_event([byte]$code, $scan, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $hold
  [YF]::keybd_event([byte]$code, $scan, 2, [UIntPtr]::Zero)
  if ($ctrl) { Start-Sleep -Milliseconds 30; [YF]::keybd_event(0x11, [byte][YF]::MapVirtualKey(0x11,0), 2, [UIntPtr]::Zero) }
}
function Shot([string]$out) {
  $r = New-Object YF+R
  [YF]::GetClientRect($h, [ref]$r) | Out-Null
  $w = $r.Rr - $r.L; $ht = $r.B - $r.T
  if ($w -le 0 -or $ht -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [YF]::PrintWindow($h, $hdc, 3) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

Focus-Ymir
$wait = $WarmupMs - [int]$sw.ElapsedMilliseconds
if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$files = @{ 'wram-lo.bin' = 1048576; 'wram-hi.bin' = 1048576; 'vdp2-vram.bin' = 524288 }
for ($i = 0; $i -lt $Count; $i++) {
  $tag = '{0:d3}' -f $i
  foreach ($f in $files.Keys) { Remove-Item (Join-Path $dumps $f) -ErrorAction SilentlyContinue }
  Focus-Ymir
  $ms = [int]$sw.ElapsedMilliseconds
  Key 0x7A $true 50
  # wait for all three to appear at full size
  $ok = $false
  for ($w = 0; $w -lt 40; $w++) {
    Start-Sleep -Milliseconds 40
    $all = $true
    foreach ($f in $files.Keys) {
      $s = Join-Path $dumps $f
      if (-not (Test-Path $s)) { $all = $false; break }
      if ((Get-Item $s).Length -lt $files[$f]) { $all = $false; break }
    }
    if ($all) { $ok = $true; break }
  }
  Start-Sleep -Milliseconds 60
  $d = Join-Path $OutDir "f$tag"
  New-Item -ItemType Directory -Force -Path $d | Out-Null
  foreach ($f in $files.Keys) { $s = Join-Path $dumps $f; if (Test-Path $s) { Copy-Item $s (Join-Path $d $f) -Force } }
  if (-not $NoShot) { Shot (Join-Path $d 'screen.png') }
  Set-Content -Path (Join-Path $d 'time.txt') -Value "$ms" -Encoding ascii
  Write-Output "f$tag t=${ms}ms ok=$ok"
  if ($IntervalMs -gt 0) { Start-Sleep -Milliseconds $IntervalMs }
}
Write-Output "done total=$([int]$sw.ElapsedMilliseconds)ms"

# ymir_step.ps1 -- pause Ymir at a wall-clock point, then frame-step and dump.
param(
  [string]$Disc = "",
  [Parameter(Mandatory=$true)][string]$YmirDir,
  [int]$PauseAtMs = 24000,
  [int]$Iters = 32,
  [int]$StepsPerIter = 30,
  [string]$OutDir = (Join-Path $PSScriptRoot "../shots/ymir_step"),
  [switch]$NoLaunch
)
$ErrorActionPreference='Continue'
$sig = @'
using System;
using System.Runtime.InteropServices;
public class YS {
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
Start-Sleep -Milliseconds 2500
$p = Get-Process ymir-sdl3 -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "ymir not running"; exit 1 }
$h = $p.MainWindowHandle

function Focus-Ymir {
  $fg = [YS]::GetForegroundWindow()
  $fgT = [YS]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
  $myT = [YS]::GetCurrentThreadId()
  [YS]::AttachThreadInput($myT, $fgT, $true) | Out-Null
  [YS]::ShowWindow($h, 9) | Out-Null
  [YS]::BringWindowToTop($h) | Out-Null
  [YS]::SetForegroundWindow($h) | Out-Null
  [YS]::SetFocus($h) | Out-Null
  [YS]::AttachThreadInput($myT, $fgT, $false) | Out-Null
}
function Key([int]$code, [bool]$ctrl=$false, [int]$hold=60) {
  $scan = [byte]([YS]::MapVirtualKey([uint32]$code, 0))
  if ($ctrl) { [YS]::keybd_event(0x11, [byte][YS]::MapVirtualKey(0x11,0), 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
  [YS]::keybd_event([byte]$code, $scan, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds $hold
  [YS]::keybd_event([byte]$code, $scan, 2, [UIntPtr]::Zero)
  if ($ctrl) { Start-Sleep -Milliseconds 40; [YS]::keybd_event(0x11, [byte][YS]::MapVirtualKey(0x11,0), 2, [UIntPtr]::Zero) }
}
function Shot([string]$out) {
  $r = New-Object YS+R
  [YS]::GetClientRect($h, [ref]$r) | Out-Null
  $w = $r.Rr - $r.L; $ht = $r.B - $r.T
  if ($w -le 0 -or $ht -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [YS]::PrintWindow($h, $hdc, 3) | Out-Null; $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

Focus-Ymir
Start-Sleep -Milliseconds 400
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$wait = $PauseAtMs - 2900
if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
Key 0x13 $false 80          # VK_PAUSE -> PauseResume
Write-Output "paused at $([int]$sw.ElapsedMilliseconds)ms"
Start-Sleep -Milliseconds 500

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$files = @('wram-lo.bin','wram-hi.bin','vdp2-vram.bin','vdp1-vram.bin','cdb-dram.bin')
for ($i = 0; $i -lt $Iters; $i++) {
  $tag = '{0:d3}' -f $i
  foreach ($f in $files) { Remove-Item (Join-Path $dumps $f) -ErrorAction SilentlyContinue }
  Key 0x7A $true 80         # Ctrl+F11 dump
  Start-Sleep -Milliseconds 600
  $d = Join-Path $OutDir "s$tag"
  New-Item -ItemType Directory -Force -Path $d | Out-Null
  foreach ($f in $files) { $s = Join-Path $dumps $f; if (Test-Path $s) { Copy-Item $s (Join-Path $d $f) -Force } }
  Shot (Join-Path $d 'screen.png')
  Set-Content -Path (Join-Path $d 'frames.txt') -Value "$($i * $StepsPerIter)" -Encoding ascii
  Write-Output "s$tag frame+$($i * $StepsPerIter)"
  for ($k = 0; $k -lt $StepsPerIter; $k++) { Key 0xDD $false 25; Start-Sleep -Milliseconds 15 }   # VK_OEM_6 = ']'
}
Write-Output "done"

# sat_key.ps1 -- send keystrokes to the Ymir window from a background process.
#
# Two problems have to be solved together:
#   1. SDL3 reads keyboard through raw input, so PostMessage'd WM_KEYDOWN is
#      ignored outright. Only SendInput (or keybd_event, which funnels into it)
#      is seen.
#   2. SendInput delivers to whatever has focus, and Windows refuses
#      SetForegroundWindow to a process that is not already foreground. The
#      documented way around it is AttachThreadInput: attach our input queue to
#      the current foreground thread, which makes us "part of" the active
#      window for the duration, set focus, then detach.
#
# Usage:
#   powershell -File tools/sat_key.ps1 -Keys F12
#   powershell -File tools/sat_key.ps1 -Keys F11 -Ctrl
#   powershell -File tools/sat_key.ps1 -Keys J -Hold 400
param(
    [Parameter(Mandatory = $true)][string]$Keys,
    [switch]$Ctrl,
    [int]$Hold = 120
)

$sig = @'
using System;
using System.Runtime.InteropServices;
public class YK {
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
}
'@
Add-Type -TypeDefinition $sig

$p = Get-Process saturnwin -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "ymir not running"; exit 1 }
$h = $p.MainWindowHandle

# --- take focus, the way Windows allows ---
$fg      = [YK]::GetForegroundWindow()
$fgThread = [YK]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$myThread = [YK]::GetCurrentThreadId()
[YK]::AttachThreadInput($myThread, $fgThread, $true) | Out-Null
[YK]::ShowWindow($h, 9) | Out-Null          # SW_RESTORE
[YK]::BringWindowToTop($h) | Out-Null
[YK]::SetForegroundWindow($h) | Out-Null
[YK]::SetFocus($h) | Out-Null
[YK]::AttachThreadInput($myThread, $fgThread, $false) | Out-Null
Start-Sleep -Milliseconds 500

$got = [YK]::GetForegroundWindow()
Write-Output ("focus: " + $(if ($got -eq $h) { "ymir" } else { "FAILED (fg=$got, want=$h)" }))

# --- key codes ---
$vk = @{
    'F1'=0x70;'F2'=0x71;'F3'=0x72;'F4'=0x73;'F5'=0x74;'F6'=0x75;'F7'=0x76;'F8'=0x77
    'F9'=0x78;'F10'=0x79;'F11'=0x7A;'F12'=0x7B;'ENTER'=0x0D;'SPACE'=0x20;'ESC'=0x1B
}
$k = $Keys.ToUpper()
$code = if ($vk.ContainsKey($k)) { $vk[$k] } elseif ($k.Length -eq 1) { [byte][char]$k } else { 0 }
if ($code -eq 0) { Write-Output "unknown key '$Keys'"; exit 1 }

# KEYEVENTF_EXTENDEDKEY(1) / KEYEVENTF_KEYUP(2); scan code helps raw-input consumers
$scan = [byte]([YK]::MapVirtualKey([uint32]$code, 0))
if ($Ctrl) { [YK]::keybd_event(0x11, [byte][YK]::MapVirtualKey(0x11,0), 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60 }
[YK]::keybd_event([byte]$code, $scan, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds $Hold
[YK]::keybd_event([byte]$code, $scan, 2, [UIntPtr]::Zero)
if ($Ctrl) { Start-Sleep -Milliseconds 60; [YK]::keybd_event(0x11, [byte][YK]::MapVirtualKey(0x11,0), 2, [UIntPtr]::Zero) }

Write-Output "sent $(if($Ctrl){'Ctrl+'})$k (vk=0x$('{0:X2}' -f $code) scan=0x$('{0:X2}' -f $scan))"

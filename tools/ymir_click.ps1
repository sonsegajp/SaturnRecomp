# ymir_click.ps1 -- click at a point inside the Ymir window's CLIENT area.
#
# ImGui buttons respond to real mouse input, not to Enter/Escape, so the
# update-check modal that appears on every start has to be clicked away before
# any hotkey (screenshot, memory dump) can reach the emulator.
#
# Coordinates are client-relative, i.e. the same coordinates you read off a
# PrintWindow capture, which is what makes this usable: capture, read the pixel
# position of the button, click it.
#
# Usage: powershell -File tools/ymir_click.ps1 -X 501 -Y 622
param(
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y
)

$sig = @'
using System;
using System.Runtime.InteropServices;
public class YC {
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [StructLayout(LayoutKind.Sequential)] public struct P { public int X, Y; }
}
'@
Add-Type -TypeDefinition $sig

$p = Get-Process ymir-sdl3 -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "ymir not running"; exit 1 }
$h = $p.MainWindowHandle

# focus first, same AttachThreadInput dance as ymir_key.ps1
$fg = [YC]::GetForegroundWindow()
$fgT = [YC]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
$myT = [YC]::GetCurrentThreadId()
[YC]::AttachThreadInput($myT, $fgT, $true) | Out-Null
[YC]::ShowWindow($h, 9) | Out-Null
[YC]::BringWindowToTop($h) | Out-Null
[YC]::SetForegroundWindow($h) | Out-Null
[YC]::AttachThreadInput($myT, $fgT, $false) | Out-Null
Start-Sleep -Milliseconds 400

$pt = New-Object YC+P
$pt.X = $X; $pt.Y = $Y
[YC]::ClientToScreen($h, [ref]$pt) | Out-Null
[YC]::SetCursorPos($pt.X, $pt.Y) | Out-Null
Start-Sleep -Milliseconds 250
[YC]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
Start-Sleep -Milliseconds 120
[YC]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
Write-Output "clicked client ($X,$Y) -> screen ($($pt.X),$($pt.Y))"

# ymir_burst.ps1 -- launch Ymir on a disc and PrintWindow-capture a burst of
# frames, then close it.
#
# PrintWindow reads that window's OWN buffer, so this cannot capture anything
# else on the desktop and never touches focus, the cursor or the keyboard --
# unlike ymir_key.ps1 / ymir_series.ps1, which drive input via AttachThreadInput
# and take the machine away from whoever is sitting at it. Use this one while
# the user is present.
#
# Add-Type runs ONCE here; ymir_capture.ps1 pays it per invocation, which caps
# that script at roughly one frame a second - far too coarse for a boot sequence.
param(
  [Parameter(Mandatory=$true)][string]$YmirDir,
  [string]$Disc = "",
  [string]$OutDir   = (Join-Path $PSScriptRoot "../shots/ymir_boot"),
  [int]$Count       = 44,
  [int]$IntervalMs  = 250,
  [int]$WarmupMs    = 1200,
  [switch]$KeepOpen,
  [switch]$NoDisc,
  # Shove Ymir's window off the visible desktop so a reference capture never
  # appears on screen. SWP_NOACTIVATE means this does NOT steal focus, and
  # PrintWindow reads the window's own back buffer, so an off-screen window
  # still captures normally.
  [switch]$Offscreen
)
$ErrorActionPreference = 'Continue'
if (-not (Test-Path -LiteralPath (Join-Path $YmirDir 'ymir-sdl3.exe'))) { throw 'YmirDir must contain ymir-sdl3.exe' }

$sig = @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class YB {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after,
        int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }
}
'@
Add-Type -TypeDefinition $sig -ReferencedAssemblies System.Drawing,System.Drawing.Primitives

Get-Process ymir-sdl3 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Get-ChildItem "$OutDir\*.png" -ErrorAction SilentlyContinue | Remove-Item -Force

# The quoting matters: a path with spaces passed unquoted silently loads NOTHING
# and the title bar reads "No disc inserted".
# An empty -Disc boots the BIOS with no disc at all, which is the cleanest way
# to compare BIOS rendering: no disc means no game, no region check, no CD
# state -- just the Multi-Player screen and its 3-D elements.
if ($Disc -and -not $NoDisc) { Start-Process "$ymirDir\ymir-sdl3.exe" -WindowStyle Hidden -ArgumentList @('-d', "`"$Disc`"") }
else       { Start-Process "$ymirDir\ymir-sdl3.exe" -WindowStyle Hidden }
$sw = [System.Diagnostics.Stopwatch]::StartNew()
Start-Sleep -Milliseconds $WarmupMs

$p = $null
for ($k = 0; $k -lt 20 -and -not $p; $k++) {
  $p = Get-Process ymir-sdl3 -ErrorAction SilentlyContinue
  if ($p -and $p.MainWindowHandle -eq 0) { $p = $null }
  if (-not $p) { Start-Sleep -Milliseconds 250 }
}
if (-not $p) { Write-Output "ymir did not start"; exit 1 }
$h = $p.MainWindowHandle

if ($Offscreen) {
  # SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE = 0x0001|0x0004|0x0010
  [YB]::SetWindowPos($h, [IntPtr]::Zero, -30000, -30000, 0, 0, 0x0015) | Out-Null
  Start-Sleep -Milliseconds 400
  Write-Output "ymir window moved off-screen"
}

$r = New-Object YB+R
[YB]::GetClientRect($h, [ref]$r) | Out-Null
$w = $r.Rr - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Output "bad client rect"; exit 1 }
Write-Output "capturing $Count frames at ${IntervalMs}ms, client ${w}x${ht}"

for ($i = 0; $i -lt $Count; $i++) {
  $ms  = [int]$sw.ElapsedMilliseconds
  $bmp = New-Object System.Drawing.Bitmap($w, $ht)
  $g   = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [YB]::PrintWindow($h, $hdc, 3) | Out-Null   # PW_RENDERFULLCONTENT|PW_CLIENTONLY
  $g.ReleaseHdc($hdc); $g.Dispose()
  $bmp.Save(("{0}\y{1:d6}.png" -f $OutDir, $ms), [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Start-Sleep -Milliseconds $IntervalMs
}

if (-not $KeepOpen) {
  Get-Process ymir-sdl3 -ErrorAction SilentlyContinue | Stop-Process -Force
}
Write-Output "done: $((Get-ChildItem "$OutDir\*.png").Count) frames in $OutDir"

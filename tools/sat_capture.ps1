# ymir_capture.ps1 -- PrintWindow capture of the Ymir client area to a PNG.
param([Parameter(Mandatory=$true)][string]$Out)
$sig = @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public class YP {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rr,B; }
}
'@
Add-Type -TypeDefinition $sig -ReferencedAssemblies System.Drawing,System.Drawing.Primitives
$p = Get-Process saturnwin -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "saturnwin not running"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object YP+R
[YP]::GetClientRect($h, [ref]$r) | Out-Null
$w = $r.Rr - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { Write-Output "bad rect"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[YP]::PrintWindow($h, $hdc, 3) | Out-Null   # PW_RENDERFULLCONTENT|PW_CLIENTONLY
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output "saved $Out ($w x $ht)"

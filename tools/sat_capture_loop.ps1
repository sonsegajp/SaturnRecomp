# Capture the visible SaturnRecomp client once per interval until it exits.
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [int]$IntervalMs = 1000
)

$ErrorActionPreference = 'Continue'
$capture = Join-Path $PSScriptRoot 'sat_capture.ps1'
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

while (-not (Get-Process saturnwin -ErrorAction SilentlyContinue)) {
    Start-Sleep -Milliseconds 100
}

$index = 0
while (Get-Process saturnwin -ErrorAction SilentlyContinue) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $path = Join-Path $OutDir ('shot-{0:D6}-{1}.png' -f $index, $stamp)
    & $capture -Out $path | Out-Null
    $index++
    Start-Sleep -Milliseconds ([Math]::Max($IntervalMs, 100))
}

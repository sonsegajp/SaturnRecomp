param(
    [string]$Path = (Join-Path $PSScriptRoot '..')
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath($Path)
$fail = New-Object System.Collections.Generic.List[string]

if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "release root does not exist: $root"
}

$forbiddenTop = @(
    '.claude', 'bios', 'discs', 'out', 'shots', 'CodexLite',
    'CodexLite.Smoke'
)
foreach ($name in $forbiddenTop) {
    if (Test-Path -LiteralPath (Join-Path $root $name)) {
        $fail.Add("forbidden top-level path: $name")
    }
}

$forbiddenExact = @(
    'duckram.bin', 'smpc.bin', 'gmon.out', 'HANDOFF.md',
    'external/openai-codex'
)
foreach ($name in $forbiddenExact) {
    if (Test-Path -LiteralPath (Join-Path $root $name)) {
        $fail.Add("forbidden local artifact: $name")
    }
}

$allowedPng = @(
    'assets/saturnrecomp-logo.png',
    'docs/images/launcher-library.png',
    'docs/images/launcher-compact.png',
    'docs/images/launcher-controls.png'
)
$forbiddenExtensions = @(
    '.bin', '.cue', '.iso', '.chd', '.raw', '.wav', '.exe', '.dll',
    '.o', '.obj', '.pdb', '.state', '.dmp', '.zip', '.7z'
)

Get-ChildItem -LiteralPath $root -Recurse -Force -File | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
    $ext = $_.Extension.ToLowerInvariant()
    if ($forbiddenExtensions -contains $ext) {
        $fail.Add("forbidden binary/media extension: $relative")
    }
    if ($ext -eq '.png' -and $allowedPng -notcontains $relative) {
        $fail.Add("unapproved PNG (only named branding and launcher screenshots are allowed): $relative")
    }
    if ($relative -match '^games/(?!_template/)') {
        $fail.Add("game-specific directory: $relative")
    }
}

$textExtensions = @('.c', '.h', '.md', '.ps1', '.py', '.toml')
Get-ChildItem -LiteralPath $root -Recurse -Force -File | Where-Object {
    $textExtensions -contains $_.Extension.ToLowerInvariant() -or $_.Name -eq '.gitignore'
} | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length).TrimStart('\', '/').Replace('\', '/')
    # The audit source necessarily contains the patterns it searches for.
    if ($relative -ne 'tools/audit_release.ps1') {
        $matches = Select-String -LiteralPath $_.FullName -Pattern 'C:\\Users\\|BEGIN (RSA|OPENSSH|EC) PRIVATE KEY|ghp_[A-Za-z0-9]+' -ErrorAction SilentlyContinue
        if ($matches) { $fail.Add("private path or credential-shaped text: $relative") }
    }
}

if ($fail.Count) {
    $unique = @($fail | Sort-Object -Unique)
    $unique | ForEach-Object { Write-Host "FAIL  $_" -ForegroundColor Red }
    Write-Host ("release audit failed: {0} issue(s)" -f $unique.Count) -ForegroundColor Red
    exit 1
}

Write-Host "PASS  source-only release audit: $root" -ForegroundColor Green
exit 0

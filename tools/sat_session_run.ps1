# Launch a visible SaturnRecomp session while timestamping its diagnostic and
# input-change stream. The caller may run this wrapper in a hidden PowerShell;
# saturnwin still owns and displays the game window.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Config,
    [Parameter(Mandatory = $true)][string]$Log
)

$ErrorActionPreference = 'Continue'
$env:SATURN_PADTRACE = '1'
$env:SATURN_PAD_TYPE = '3d'
Remove-Item Env:SATURN_HEADLESS -ErrorAction SilentlyContinue
Remove-Item Env:SATURN_UNCAP -ErrorAction SilentlyContinue
Remove-Item Env:SATURN_RENDERER -ErrorAction SilentlyContinue

& $Exe $Config 2>&1 | ForEach-Object {
    '{0} {1}' -f (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss.fffK'), $_
} | Out-File -LiteralPath $Log -Encoding utf8

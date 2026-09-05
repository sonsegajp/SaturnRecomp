param(
    [Parameter(Mandatory = $true)][string]$Log,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
$events = [System.Collections.Generic.List[string]]::new()

foreach ($line in Get-Content -LiteralPath $Log) {
    if ($line -match '\[padtrace\] pad=([0-9A-Fa-f]{2}) ([0-9A-Fa-f]{2}).*mastercy=([0-9]+)') {
        $events.Add(('{0}:0x{1}:0x{2}' -f $Matches[3], $Matches[1].ToUpperInvariant(),
                     $Matches[2].ToUpperInvariant()))
    }
}

if ($events.Count -eq 0) {
    throw "No padtrace events found in $Log"
}

[System.IO.File]::WriteAllLines($Out, $events, [System.Text.Encoding]::ASCII)
Write-Host "Wrote $($events.Count) events to $Out"

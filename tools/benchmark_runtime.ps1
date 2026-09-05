param(
    [Parameter(Mandatory=$true)][string]$Game,
    [string]$Exe='runner/saturnwin.exe',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag='runtime',
    [int]$Frames=7200,
    [string]$PadSequence='',
    [string]$SmpcFile='',
    [switch]$Paced
)
# Reproducible, silent physical output; the SCSP and its DSP still execute.
$ErrorActionPreference='Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$mingw=$env:SATURN_MINGW_BIN
if(-not $mingw){$mingw='C:\msys64\mingw64\bin'}
$env:PATH=$mingw+';'+$env:PATH
Get-ChildItem Env:SATURN_* | ForEach-Object {Remove-Item -LiteralPath ('Env:'+$_.Name)}
$env:SDL_AUDIODRIVER='dummy'
$env:SATURN_RENDERER='vulkan'
$env:SATURN_VK_HIDDEN='1'
if(-not $Paced){$env:SATURN_UNCAP='1'}
$env:SATURN_PAD='0'
$env:SATURN_MAX_FRAMES=[string]$Frames
$env:SATURN_PROF='1'
$env:SATURN_PROF_INTERVAL='300'
$env:SATURN_FRAMEPROF='1'
$env:SATURN_FRAMEPROF_INTERVAL='300'
if($PadSequence){$env:SATURN_PADSEQ=$PadSequence}
if($SmpcFile){$env:SATURN_SMPCFILE=$SmpcFile}
New-Item -ItemType Directory -Force -Path out/performance | Out-Null
$base="out/performance/$Tag"
$env:SATURN_VK_CAPTURE="$base.png"
$env:SATURN_VK_CAPTURE_FRAME=[string]([Math]::Max(1,$Frames-1))
$watch=[Diagnostics.Stopwatch]::StartNew()
$quotedGame='"'+$Game.Replace('"','')+'"'
$p=Start-Process -FilePath $Exe -ArgumentList $quotedGame -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput "$base.log" -RedirectStandardError "$base.err"
[ordered]@{game=$Game;exe=(Resolve-Path $Exe).Path;sha256=(Get-FileHash $Exe).Hash;fields=$Frames;seconds=$watch.Elapsed.TotalSeconds;exit=$p.ExitCode;uncapped=(-not $Paced);physicalAudio='dummy';dsp='enabled';padSequence=$PadSequence} | ConvertTo-Json | Set-Content "$base.json"
Get-Content "$base.err" | Select-String '^\[prof\]|^\[frame\]'
exit $p.ExitCode

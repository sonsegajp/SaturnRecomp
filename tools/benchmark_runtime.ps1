param(
    [Parameter(Mandatory=$true)][string]$Game,
    [string]$Exe='runner/saturnwin.exe',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag='runtime',
    [ValidateRange(1,1000000)][int]$Frames=7200,
    [string]$PadSequence='',
    [string]$SmpcFile='',
    [ValidateScript({$_ -eq 0 -or ($_ -ge 60 -and $_ -le 240)})][int]$PresentHz=0,
    [ValidateRange(1,4)][int]$InternalScale=1,
    [ValidateRange(0,1)][int]$TextureFilter=0,
    [ValidateRange(0,1)][int]$Antialiasing=0,
    [switch]$ModelSmoothing,
    [switch]$CaptureAudio,
    [switch]$NoProfileCounters,
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
if(-not $NoProfileCounters){
    $env:SATURN_PROF='1'
    $env:SATURN_PROF_INTERVAL='300'
    $env:SATURN_FRAMEPROF='1'
    $env:SATURN_FRAMEPROF_INTERVAL='300'
}
if($PadSequence){$env:SATURN_PADSEQ=$PadSequence}
New-Item -ItemType Directory -Force -Path out/performance | Out-Null
$base="out/performance/$Tag"
# Every run starts from explicit settings and its own console state. User F1
# preferences or a previous benchmark must not silently alter the workload.
$env:SATURN_SETTINGS_FILE="$base.settings.ini"
$env:SATURN_SMPCFILE="$base.smpc.bin"
if($SmpcFile){Copy-Item -LiteralPath $SmpcFile -Destination $env:SATURN_SMPCFILE}
else{[IO.File]::WriteAllBytes($env:SATURN_SMPCFILE,[byte[]](83,0,0,0,0,1,0,0))}
$settingsText=@"
[Video]
WindowWidth=960
WindowHeight=720
Fullscreen=0
InternalScale=$InternalScale
TextureFilter=$TextureFilter
Antialiasing=$Antialiasing
ModelSmoothing=$([int][bool]$ModelSmoothing)
Interpolation=$PresentHz
TargetHz=$(if($PresentHz){$PresentHz}else{120})
[Audio]
Volume=100
Muted=0
"@
[IO.File]::WriteAllText($env:SATURN_SETTINGS_FILE,$settingsText)
if($CaptureAudio){$env:SATURN_WAV="$base.wav"}
$env:SATURN_VK_CAPTURE="$base.png"
$env:SATURN_VK_CAPTURE_FRAME=[string]([Math]::Max(1,$Frames-1))
$watch=[Diagnostics.Stopwatch]::StartNew()
$quotedGame='"'+$Game.Replace('"','')+'"'
$p=Start-Process -FilePath $Exe -ArgumentList $quotedGame -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput "$base.log" -RedirectStandardError "$base.err"
[ordered]@{game=$Game;exe=(Resolve-Path $Exe).Path;sha256=(Get-FileHash $Exe).Hash;fields=$Frames;seconds=$watch.Elapsed.TotalSeconds;exit=$p.ExitCode;uncapped=(-not $Paced);physicalAudio='dummy';dsp='enabled';padSequence=$PadSequence;presentHz=$PresentHz;internalScale=$InternalScale;textureFilter=$TextureFilter;antialiasing=$Antialiasing;modelSmoothing=[bool]$ModelSmoothing;profileCounters=(-not $NoProfileCounters);audioCapture=[bool]$CaptureAudio} | ConvertTo-Json | Set-Content "$base.json"
Get-Content "$base.err" | Select-String '^\[prof\]|^\[frame\]'
exit $p.ExitCode

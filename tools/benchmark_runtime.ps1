param(
    [Parameter(Mandatory=$true)][string]$Game,
    [string]$Exe='runner/saturnwin.exe',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Tag='runtime',
    [ValidateRange(1,1000000)][int]$Frames=7200,
    [string]$PadSequence='',
    [string]$PadSequenceFile='',
    [string]$SmpcFile='',
    [ValidateScript({$_ -eq 0 -or ($_ -ge 60 -and $_ -le 240)})][int]$PresentHz=0,
    [ValidateRange(1,4)][int]$InternalScale=1,
    [ValidateRange(0,1)][int]$TextureFilter=0,
    [ValidateRange(0,1)][int]$Antialiasing=0,
    [switch]$ModelSmoothing,
    [switch]$CaptureAudio,
    [switch]$NoFrameCapture,
    [switch]$NoProfileCounters,
    [switch]$Paced
)
# Reproducible, silent physical output; the SCSP and its DSP still execute.
$ErrorActionPreference='Continue'
Set-Location (Split-Path -Parent $PSScriptRoot)
$resolvedGame=(Resolve-Path -LiteralPath $Game -ErrorAction Stop).Path
$resolvedExe=(Resolve-Path -LiteralPath $Exe -ErrorAction Stop).Path
$exeHash=(Get-FileHash -LiteralPath $resolvedExe -ErrorAction Stop).Hash
$gameHash=(Get-FileHash -LiteralPath $resolvedGame -ErrorAction Stop).Hash
if($SmpcFile){$SmpcFile=(Resolve-Path -LiteralPath $SmpcFile -ErrorAction Stop).Path}
if($PadSequence -and $PadSequenceFile){throw 'Use either PadSequence or PadSequenceFile, not both.'}
if($PadSequenceFile){$PadSequenceFile=(Resolve-Path -LiteralPath $PadSequenceFile -ErrorAction Stop).Path}
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
if($PadSequenceFile){$env:SATURN_PADSEQ_FILE=$PadSequenceFile}
if($PadSequenceFile){$padSequenceHash=(Get-FileHash -LiteralPath $PadSequenceFile).Hash}
else{
    $padHasher=[Security.Cryptography.SHA256]::Create()
    try{$padSequenceHash=[BitConverter]::ToString($padHasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($PadSequence))).Replace('-','')}
    finally{$padHasher.Dispose()}
}
New-Item -ItemType Directory -Force -Path out/performance | Out-Null
$base="out/performance/$Tag"
# Every run starts from explicit settings and its own console state. User F1
# preferences or a previous benchmark must not silently alter the workload.
$env:SATURN_SETTINGS_FILE="$base.settings.ini"
$env:SATURN_SMPCFILE="$base.smpc.bin"
if($SmpcFile){Copy-Item -LiteralPath $SmpcFile -Destination $env:SATURN_SMPCFILE}
else{[IO.File]::WriteAllBytes($env:SATURN_SMPCFILE,[byte[]](83,0,0,0,0,1,0,0))}
$smpcSeedHash=(Get-FileHash -LiteralPath $env:SATURN_SMPCFILE).Hash
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
if(-not $NoFrameCapture){
    $env:SATURN_VK_CAPTURE="$base.png"
    $env:SATURN_VK_CAPTURE_FRAME=[string]([Math]::Max(1,$Frames-1))
}
$watch=[Diagnostics.Stopwatch]::StartNew()
$quotedGame='"'+$resolvedGame+'"'
$p=Start-Process -FilePath $resolvedExe -ArgumentList $quotedGame -WindowStyle Hidden -PassThru -RedirectStandardOutput "$base.log" -RedirectStandardError "$base.err" -ErrorAction Stop
# Retain the handle before waiting so exit code and CPU times remain readable
# even when a short diagnostic process has already exited.
$processHandle=$p.Handle
$p.WaitForExit()
$watch.Stop()
$cpuSeconds=$null
try{$cpuSeconds=$p.TotalProcessorTime.TotalSeconds}catch{}
$exitCode=$p.ExitCode
$hashAfter=(Get-FileHash -LiteralPath $resolvedExe -ErrorAction Stop).Hash
$stdoutText=[IO.File]::ReadAllText("$base.log")
$stderrText=[IO.File]::ReadAllText("$base.err")
$finalPc=$null;$masterCycles=$null;$completedFields=$null;$completedCycles=$null
# The legacy window footer labels master.cycles as "instructions". Preserve
# its actual meaning, and use the explicit completion marker when available.
$stops=[regex]::Matches($stdoutText,'(?m)^stopped at PC=0x([0-9A-Fa-f]+) after (\d+) instructions')
if($stops.Count){
    $lastStop=$stops[$stops.Count-1]
    $finalPc='0x'+$lastStop.Groups[1].Value.ToUpperInvariant()
    $masterCycles=[UInt64]::Parse($lastStop.Groups[2].Value)
}
$completions=[regex]::Matches($stderrText,'(?m)^\[runtime\] fields (\d+) cycles (\d+)')
if($completions.Count){
    $lastCompletion=$completions[$completions.Count-1]
    $completedFields=[UInt64]::Parse($lastCompletion.Groups[1].Value)
    $completedCycles=[UInt64]::Parse($lastCompletion.Groups[2].Value)
}
$halted=($stdoutText -match '\bHALTED\b|\[slave halt\]' -or $stderrText -match '\bHALTED\b|\[slave halt\]')
$renderErrors=@($stderrText -split '\r?\n' | Where-Object {
    $_ -match '^\[video\].*(failed|unavailable)' -or
    $_ -match '^Vulkan.*failed' -or $_ -match '^\[vulkan\].*(failed|overflow)'
})
[ordered]@{game=$resolvedGame;gameSha256=$gameHash;exe=$resolvedExe;sha256=$exeHash;exeChangedDuringRun=($exeHash -ne $hashAfter);smpcSeedSha256=$smpcSeedHash;fields=$Frames;completedFields=$completedFields;completedCycles=$completedCycles;finalPc=$finalPc;masterCycles=$masterCycles;halted=$halted;renderErrors=$renderErrors;seconds=$watch.Elapsed.TotalSeconds;cpuSeconds=$cpuSeconds;exit=$exitCode;uncapped=(-not $Paced);physicalAudio='dummy';dsp='enabled';padSequence=$PadSequence;padSequenceFile=$PadSequenceFile;padSequenceSha256=$padSequenceHash;presentHz=$PresentHz;internalScale=$InternalScale;textureFilter=$TextureFilter;antialiasing=$Antialiasing;modelSmoothing=[bool]$ModelSmoothing;profileCounters=(-not $NoProfileCounters);audioCapture=[bool]$CaptureAudio;frameCapture=(-not $NoFrameCapture)} | ConvertTo-Json | Set-Content -Encoding UTF8 "$base.json"
$stderrText -split '\r?\n' | Select-String '^\[prof\]|^\[frame\]'
exit $exitCode

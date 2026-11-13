# Measures a reference track and writes dsp/ConcussionProfile.h (numbers only, no audio).
#   tools\make_profile.ps1 -Audio "C:\path\to\track.wav" [-Name "Artist - Title"]
param(
    [Parameter(Mandatory = $true)][string]$Audio,
    [string]$Name = [IO.Path]::GetFileNameWithoutExtension($Audio)
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tool = Join-Path $root 'build-tests\Release\profile_ref.exe'
if (-not (Test-Path $tool)) { throw "Build the tools first: cmake --build build-tests --config Release" }

$raw = Join-Path $env:TEMP ("snitch_ref_{0}.f32" -f $PID)
try {
    & ffmpeg -v error -y -i $Audio -f f32le -ac 2 -ar 44100 $raw
    if ($LASTEXITCODE -ne 0) { throw "ffmpeg could not decode $Audio" }
    & $tool $raw 44100 $Name (Join-Path $root 'dsp\ConcussionProfile.h')
    if ($LASTEXITCODE -ne 0) { throw "profile_ref failed" }
} finally {
    Remove-Item $raw -ErrorAction SilentlyContinue
}

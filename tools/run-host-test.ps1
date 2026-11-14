# Checks out\version.dll without the game.
#
# Runs the developer test host twice in a temporary folder. The host's window is never shown.
#   1. Test flag set:   the mod must start, forward version.dll calls and draw its marker.
#   2. Test flag unset: the mod must stay dormant (no marker, no log written).
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1
# Exit code 0 = both checks passed.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'out\version.dll'
$exe = Join-Path $root 'out\testhost.exe'
if (-not (Test-Path $dll) -or -not (Test-Path $exe)) {
    throw 'Build first: powershell -ExecutionPolicy Bypass -File build\build.ps1'
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("racing-master-mod-hosttest-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item $dll, $exe $work
$log = Join-Path $env:LOCALAPPDATA 'RacingMasterMod\mod.log'

try {
    Write-Host '--- 1. mod active ---'
    $env:RACING_MASTER_MOD_TEST_HOST = '1'
    & (Join-Path $work 'testhost.exe') (Join-Path $work 'active.bmp') 4 | Out-Host
    $activeOk = $LASTEXITCODE -eq 0
    Remove-Item Env:\RACING_MASTER_MOD_TEST_HOST

    Write-Host '--- 2. mod dormant ---'
    $stamp = if (Test-Path $log) { (Get-Item $log).LastWriteTimeUtc } else { $null }
    & (Join-Path $work 'testhost.exe') (Join-Path $work 'dormant.bmp') 2 | Out-Host
    $noMarker = $LASTEXITCODE -eq 1
    $stampAfter = if (Test-Path $log) { (Get-Item $log).LastWriteTimeUtc } else { $null }
    $dormantOk = $noMarker -and ($stamp -eq $stampAfter)

    Write-Host ''
    Write-Host ("mod active:  " + $(if ($activeOk) { 'PASS' } else { 'FAIL' }))
    Write-Host ("mod dormant: " + $(if ($dormantOk) { 'PASS' } else { 'FAIL' }))
    $failed = -not ($activeOk -and $dormantOk)
} finally {
    Remove-Item Env:\RACING_MASTER_MOD_TEST_HOST -ErrorAction SilentlyContinue
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
# The dormant run is expected to return 1, so set the script's own result explicitly.
if ($failed) { exit 1 } else { exit 0 }

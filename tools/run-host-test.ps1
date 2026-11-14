# Checks out\version.dll without the game. Nothing is shown on screen: every window the test
# programs create stays hidden.
#
# Every run of the mod gets its own LOCALAPPDATA folder under a temporary work folder, so the real
# %LOCALAPPDATA%\RacingMasterMod\mod.log (the evidence from a game run) is never touched.
#
# Runs, in order:
#   1. game layout, mod active     T1-T6: hook, Present1, PRESENT_TEST, ResizeBuffers, second chain, page protection
#   2. game layout, injected fault T7: the mod logs the fault, releases everything and switches itself off
#   3. old layout                  T8: 640x360 B8G8R8A8 SEQUENTIAL
#   4. test host, flag unset       dormant: no marker, no log folder
#   5. bystander, flag unset       T9: no user32/gdi32/d3d11/dxgi loaded, 17 of 17 forwards, no log folder
#   6. bystander, flag set         T10: a program not named testhost.exe stays dormant even with the flag
#   7. settings.ini UTF-8 + BOM    T13: stage=0 switches the mod off
#   8. settings.ini UTF-16         T13: stage=-1 and marker_seconds=ten are refused with a reason; old log archived
#   9. ini beside the DLL          racing-master-mod.ini overrides settings.ini
#  10. folder with a Chinese name  T14: the "loaded into" line is complete UTF-8
#  11. two hosts at once           T15: the second writes mod.<pid>.log
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1
# Exit code 0 = every check passed.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'out\version.dll'
$hostExe = Join-Path $root 'out\testhost.exe'
$bystanderExe = Join-Path $root 'out\bystander.exe'
foreach ($file in $dll, $hostExe, $bystanderExe) {
    if (-not (Test-Path $file)) {
        throw 'Build first: powershell -ExecutionPolicy Bypass -File build\build.ps1'
    }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("racing-master-mod-hosttest-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item $dll, $hostExe, $bystanderExe $work

$realLocalAppData = $env:LOCALAPPDATA
$results = @()
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Record([string]$Name, [bool]$Ok) {
    $script:results += @{ Name = $Name; Ok = $Ok }
    Write-Host ("  => " + $(if ($Ok) { 'PASS' } else { 'FAIL' }) + "  $Name")
}

# Runs one program with LOCALAPPDATA pointed at its own folder and the given environment variables.
function Invoke-Run {
    param([string]$Title, [string]$Exe, [string[]]$Arguments, [hashtable]$Environment, [string]$LogRoot)
    Write-Host ''
    Write-Host "--- $Title ---"
    New-Item -ItemType Directory -Force $LogRoot | Out-Null
    $env:LOCALAPPDATA = $LogRoot
    foreach ($key in $Environment.Keys) { Set-Item -Path "Env:\$key" -Value $Environment[$key] }
    try {
        $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" })
        $exit = $LASTEXITCODE
    } finally {
        foreach ($key in $Environment.Keys) { Remove-Item -Path "Env:\$key" -ErrorAction SilentlyContinue }
        $env:LOCALAPPDATA = $realLocalAppData
    }
    $lines | ForEach-Object { Write-Host "    $_" }
    return @{ Exit = $exit; Lines = $lines; Log = (Join-Path $LogRoot 'RacingMasterMod\mod.log'); LogRoot = $LogRoot }
}

function Read-Log([string]$Path) {
    if (Test-Path $Path) { [IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8) } else { '' }
}

function Write-Ini([string]$Path, [string[]]$Lines, [string]$Encoding) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Path) | Out-Null
    $text = ($Lines -join "`r`n") + "`r`n"
    switch ($Encoding) {
        'utf8bom' { [IO.File]::WriteAllText($Path, $text, (New-Object System.Text.UTF8Encoding($true))) }
        'utf16'   { [IO.File]::WriteAllText($Path, $text, [Text.Encoding]::Unicode) }
        default   { [IO.File]::WriteAllText($Path, $text, $utf8) }
    }
}

$flag = @{ RACING_MASTER_MOD_TEST_HOST = '1' }
$gameFirstFrame = 'first frame: 2558x1360, format 24, buffers 2, swap effect 4, windowed 1, samples 1'

try {
    # 1. Game layout, mod active.
    $run = Invoke-Run '1. game layout, mod active (T1-T6)' (Join-Path $work 'testhost.exe') @('game', $work, '8') $flag (Join-Path $work 'lad-active')
    $log = Read-Log $run.Log
    Record 'game layout: host checks T1-T6 pass' ($run.Exit -eq 0)
    Record 'game layout: log has the first-frame line the real game wrote' ($log.Contains($gameFirstFrame))
    Record 'game layout: log says the hook attached to Present and Present1' ($log.Contains('attached to the frame presentation functions (Present and Present1)'))

    # 2. Injected fault.
    $run = Invoke-Run '2. game layout, injected fault at frame 300 (T7)' (Join-Path $work 'testhost.exe') @('game', $work, '8') ($flag + @{ RACING_MASTER_MOD_TEST_FAULT = '300' }) (Join-Path $work 'lad-fault')
    $log = Read-Log $run.Log
    Record 'fault: host alive, marker gone, ResizeBuffers succeeds' ($run.Exit -eq 0)
    Record 'fault: one log line names the exception and the module and says the mod switched itself off' ($log -match 'frame code failed \(exception 0x[0-9A-Fa-f]{8} at .+version\.dll\+0x[0-9a-f]+\) and has switched itself off')

    # 3. Old layout.
    $run = Invoke-Run '3. old layout (T8)' (Join-Path $work 'testhost.exe') @('legacy', $work, '8') $flag (Join-Path $work 'lad-legacy')
    Record 'old layout: marker found' ($run.Exit -eq 0)

    # 4. Test host without the flag: dormant.
    $run = Invoke-Run '4. test host, flag unset: mod must stay dormant' (Join-Path $work 'testhost.exe') @('quick', $work, '2') @{} (Join-Path $work 'lad-dormant')
    Record 'dormant host: forwarding works and the stand-in is in use' (($run.Lines -match '^forwarding: OK').Count -eq 1 -and ($run.Lines -match '^stand-in in use: yes').Count -eq 1)
    Record 'dormant host: no marker' (($run.Lines -match '^marker: not found').Count -eq 1)
    Record 'dormant host: no log folder created' (-not (Test-Path (Join-Path $run.LogRoot 'RacingMasterMod')))

    # 5. Bystander without the flag.
    $run = Invoke-Run '5. bystander, flag unset (T9)' (Join-Path $work 'bystander.exe') @() @{} (Join-Path $work 'lad-bystander')
    Record 'bystander: no extra DLL loaded, 17 of 17 forwards, stand-in in use' ($run.Exit -eq 0)
    Record 'bystander: no log folder created' (-not (Test-Path (Join-Path $run.LogRoot 'RacingMasterMod')))

    # 6. Bystander with the flag: the name check keeps it dormant.
    $run = Invoke-Run '6. bystander, flag set (T10)' (Join-Path $work 'bystander.exe') @() $flag (Join-Path $work 'lad-bystander-flag')
    Record 'bystander with flag: still dormant' ($run.Exit -eq 0 -and -not (Test-Path (Join-Path $run.LogRoot 'RacingMasterMod')))

    # 7. settings.ini saved as UTF-8 with a byte order mark, stage=0.
    $iniRoot = Join-Path $work 'lad-ini'
    Write-Ini (Join-Path $iniRoot 'RacingMasterMod\settings.ini') @('[mod]', 'stage=0') 'utf8bom'
    $run = Invoke-Run '7. settings.ini with UTF-8 BOM, stage=0 (T13)' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $iniRoot
    $log = Read-Log $run.Log
    Record 'ini with BOM: stage 0 recognised, mod off' ($log.Contains('settings: stage 0') -and $log.Contains('stage 0: the mod is switched off') -and ($run.Lines -match '^marker: not found').Count -eq 1)

    # 8. settings.ini in UTF-16 with bad values; same folder, so the earlier log gets archived.
    Write-Ini (Join-Path $iniRoot 'RacingMasterMod\settings.ini') @('[mod]', 'stage=-1', 'marker_seconds=ten') 'utf16'
    $run = Invoke-Run '8. settings.ini in UTF-16, stage=-1, marker_seconds=ten (T13)' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $iniRoot
    $log = Read-Log $run.Log
    Record 'bad ini: stage=-1 refused with a reason, mod off' ($log.Contains('"stage=-1" is not 0, 1 or 2') -and ($run.Lines -match '^marker: not found').Count -eq 1)
    Record 'bad ini: marker_seconds=ten refused with a reason' ($log.Contains('"marker_seconds=ten" is not a number'))
    $archived = @(Get-ChildItem (Join-Path $iniRoot 'RacingMasterMod') -Filter 'mod.*.log' | Where-Object { $_.Name -match '^mod\.\d{8}-\d{6}\.log$' })
    Record 'log of the previous run archived under a dated name' ($archived.Count -eq 1)

    # 9. The ini beside the DLL overrides settings.ini.
    $overrideDir = Join-Path $work 'override'
    New-Item -ItemType Directory -Force $overrideDir | Out-Null
    Copy-Item $dll, $hostExe $overrideDir
    $overrideRoot = Join-Path $work 'lad-override'
    Write-Ini (Join-Path $overrideRoot 'RacingMasterMod\settings.ini') @('[mod]', 'marker_seconds=7') 'utf8'
    Write-Ini (Join-Path $overrideDir 'racing-master-mod.ini') @('[mod]', 'marker_seconds = 5 ; beside the DLL') 'utf8'
    $run = Invoke-Run '9. racing-master-mod.ini beside the DLL overrides settings.ini' (Join-Path $overrideDir 'testhost.exe') @('quick', $work, '1') $flag $overrideRoot
    $log = Read-Log $run.Log
    Record 'ini beside the DLL wins: marker for 5 s' ($log.Contains('settings: stage 2, marker for 5 s') -and ($run.Lines -match '^marker: found').Count -eq 1)

    # 10. Folder whose name has Chinese characters (U+904A U+6232).
    $cjkDir = Join-Path $work ([string][char]0x904A + [string][char]0x6232)
    New-Item -ItemType Directory -Force $cjkDir | Out-Null
    Copy-Item $dll, $hostExe $cjkDir
    $run = Invoke-Run '10. folder with a Chinese name (T14)' (Join-Path $cjkDir 'testhost.exe') @('quick', $work, '1') $flag (Join-Path $work 'lad-cjk')
    $log = Read-Log $run.Log
    Record 'Chinese folder: the "loaded into" line carries the full path' ($log.Contains('loaded into ' + (Join-Path $cjkDir 'testhost.exe') + ' '))

    # 11. Two hosts at once share one log folder.
    $twoRoot = Join-Path $work 'lad-two'
    New-Item -ItemType Directory -Force $twoRoot | Out-Null
    Write-Host ''
    Write-Host '--- 11. two hosts at once (T15) ---'
    $env:LOCALAPPDATA = $twoRoot
    $env:RACING_MASTER_MOD_TEST_HOST = '1'
    try {
        $first = Start-Process -FilePath (Join-Path $work 'testhost.exe') -ArgumentList 'quick', "`"$work`"", '3' -NoNewWindow -PassThru
        Start-Sleep -Milliseconds 700
        & (Join-Path $work 'testhost.exe') quick $work 2 | ForEach-Object { Write-Host "    $_" }
        $first.WaitForExit()
    } finally {
        Remove-Item Env:\RACING_MASTER_MOD_TEST_HOST -ErrorAction SilentlyContinue
        $env:LOCALAPPDATA = $realLocalAppData
    }
    $pidLogs = @(Get-ChildItem (Join-Path $twoRoot 'RacingMasterMod') -Filter 'mod.*.log' | Where-Object { $_.Name -match '^mod\.\d+\.log$' })
    Record 'two hosts: mod.log and one mod.<pid>.log' ((Test-Path (Join-Path $twoRoot 'RacingMasterMod\mod.log')) -and $pidLogs.Count -eq 1)
    Record 'two hosts: the process-id log has content' ($pidLogs.Count -eq 1 -and (Read-Log $pidLogs[0].FullName).Contains('loaded into'))

    Write-Host ''
    Write-Host 'Summary'
    foreach ($r in $results) { Write-Host ("  " + $(if ($r.Ok) { 'PASS' } else { 'FAIL' }) + "  $($r.Name)") }
    $failed = @($results | Where-Object { -not $_.Ok }).Count
    Write-Host ''
    Write-Host ("$($results.Count - $failed) of $($results.Count) checks passed" + $(if ($failed) { "; $failed FAILED" } else { '' }))
} finally {
    Remove-Item Env:\RACING_MASTER_MOD_TEST_HOST -ErrorAction SilentlyContinue
    Remove-Item Env:\RACING_MASTER_MOD_TEST_FAULT -ErrorAction SilentlyContinue
    $env:LOCALAPPDATA = $realLocalAppData
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 } else { exit 0 }

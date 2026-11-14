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
#  12. settings.ini UTF-16 BE      T13: stage=0 switches the mod off; the log starts with "loaded into"
#  13. ini without [mod], CR-only  keys before any heading count as [mod]; CR-only line endings split
#  14. ini under a wrong heading   the log says the file changed nothing and names [mod]
# Media remote (src\remote), against the silent fake player in testhost.exe media mode. Each run
# publishes the fake player for a few seconds; while it exists it is Windows' current media
# session (media keys and the Windows flyout point at it). No command can reach any other
# session: the mod refuses every app id but RACING_MASTER_MOD_TEST_MEDIA_APP in the test host.
#  15. basic       every command reaches the fake player in order and is confirmed; clean stop
#  16. ignore-pause the player accepts pause and keeps playing: "no effect"
#  17. no-repeat   the player offers no loop and no next: "not offered", nothing sent
#  18. vanish      the player closes its session on pause: "session gone", then waiting for it
#  19. late        the player appears 2 s after the mod: waiting, then ready without a restart
#  20. guard       commands may only go to another id: refused, nothing reaches the fake player
#  21. lifecycle   the game's Shutdown (sleep, wake), a media thread that stops answering, the
#                  freshness check, and rebuild mode (the fallback when change notices stop)
#  22. media, flag unset: the mod stays dormant, no log folder, nothing sent
#
# Music panel (src\ui), against a stand-in music player and an example "Other app" inside the
# mod (no real media session is read or sent anything). Screenshots are copied to out\screens
# before the temporary folder is removed.
#  23. [panel] and [keys] values   good values accepted, bad ones refused with a reason, mod keeps running
#  24. [keys] chosen by the player the panel uses them; an unknown key name keeps the default and says why
#  25-30. screenshots              English and draft Chinese, small/medium/large text: bar, list,
#                                  settings row, "Other app"; PNG files with pixel checks
#  31-32. "Other app" variants     an app that allows everything, and no app at all
#  33. panel and ResizeBuffers     four sizes with the panel and list drawing
#  34. fault inside the panel      game state put back, nothing leaked, mouse and keys back to the game
#  35. [panel] mode=display        drawn, but nothing taken from the game
#  36. panel input                 clicks, pass-through, hotkeys, Insert, wheel, ClearInputMouse after
#                                  a gap, drop-down closing, text field, focus, "Other app", heartbeat
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
    # Anything the program writes to its error stream is kept as a line, not turned into a script
    # error: under 'Stop', one such line would abort the whole test with no summary.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = @(& $Exe @Arguments 2>&1 | ForEach-Object { "$_" })
        $exit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
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
        'utf16be' { [IO.File]::WriteAllText($Path, $text, [Text.Encoding]::BigEndianUnicode) }
        default   { [IO.File]::WriteAllText($Path, $text, $utf8) }
    }
}

$flag = @{ RACING_MASTER_MOD_TEST_HOST = '1' }

# One music panel run (testhost.exe panel ...). marker_seconds=0 so nothing but the panel is on
# screen; each run is also checked for a Dear ImGui assertion or reported mistake in its log.
function Invoke-PanelRun([string]$Title, [string]$Script, [string]$Name, [hashtable]$Environment, [string[]]$Ini) {
    $logRoot = Join-Path $work ('lad-panel-' + ($Title -replace '[^A-Za-z0-9]+', '-'))
    Write-Ini (Join-Path $logRoot 'RacingMasterMod\settings.ini') (@('[mod]', 'marker_seconds=0') + $Ini) 'utf8'
    $arguments = @('panel', $work, '10', $Script)
    if ($Name) { $arguments += $Name }
    $result = Invoke-Run $Title (Join-Path $work 'testhost.exe') $arguments ($flag + $Environment) $logRoot
    $text = Read-Log $result.Log
    Record "$($Title): host checks pass" ($result.Exit -eq 0)
    Record "$($Title): no Dear ImGui assertion or reported mistake in the log" (-not ($text -match 'IMGUI ASSERT|reported a mistake'))
    return @{ Exit = $result.Exit; Log = $text; Lines = $result.Lines }
}
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

    # 12. settings.ini saved as UTF-16 big endian (Notepad's "UTF-16 BE"), stage=0.
    $beRoot = Join-Path $work 'lad-ini-be'
    Write-Ini (Join-Path $beRoot 'RacingMasterMod\settings.ini') @('[mod]', 'stage=0') 'utf16be'
    $run = Invoke-Run '12. settings.ini in UTF-16 big endian, stage=0 (T13)' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $beRoot
    $log = Read-Log $run.Log
    Record 'ini in UTF-16 big endian: stage 0 recognised, mod off' ($log.Contains('settings: stage 0') -and ($run.Lines -match '^marker: not found').Count -eq 1)
    Record 'log starts with the "loaded into" line' (($log -split "`r`n")[0] -match '^\[[^\]]+\] Racing Master Mod .* loaded into ')

    # 13. No [mod] heading and CR-only line endings: the keys still count as [mod].
    $crRoot = Join-Path $work 'lad-ini-cr'
    New-Item -ItemType Directory -Force (Join-Path $crRoot 'RacingMasterMod') | Out-Null
    [IO.File]::WriteAllText((Join-Path $crRoot 'RacingMasterMod\settings.ini'), "stage=0`rmarker_seconds=3`r", $utf8)
    $run = Invoke-Run '13. settings.ini without a [mod] heading, CR-only line endings' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $crRoot
    $log = Read-Log $run.Log
    Record 'ini without heading, CR-only: stage 0 and marker 3 s applied' ($log.Contains('settings: stage 0, marker for 3 s') -and ($run.Lines -match '^marker: not found').Count -eq 1)

    # 14. Every line under a heading that is not [mod]: the log must say the file changed nothing.
    $otherRoot = Join-Path $work 'lad-ini-other'
    Write-Ini (Join-Path $otherRoot 'RacingMasterMod\settings.ini') @('[other]', 'stage=0') 'utf8'
    $run = Invoke-Run '14. settings.ini whose only heading is not [mod]' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $otherRoot
    $log = Read-Log $run.Log
    Record 'ini under a wrong heading: log says the file changed nothing and names [mod]' ($log.Contains('changed nothing, because none of its lines is a key=value line under a [mod] heading'))

    # --- media remote ---------------------------------------------------------------------------------
    $fakeId = 'RacingMasterMod.TestHost.FakePlayer'
    # Runs one media scenario. The mod's script sets what is sent; the scenario sets how the fake
    # player behaves. Returns the run plus what the fake player received.
    function Invoke-Media([string]$Title, [string]$Scenario, [string]$Script, [string]$Folder, [hashtable]$Extra = @{}, [bool]$WithFlag = $true, [int]$Seconds = 40) {
        $environment = @{ RACING_MASTER_MOD_TEST_MEDIA_APP = $fakeId; RACING_MASTER_MOD_TEST_MEDIA_SCRIPT = $Script }
        if ($WithFlag) { $environment.RACING_MASTER_MOD_TEST_HOST = '1' }
        foreach ($key in $Extra.Keys) { $environment[$key] = $Extra[$key] }
        $run = Invoke-Run $Title (Join-Path $work 'testhost.exe') @('media', $work, "$Seconds", $Scenario) $environment (Join-Path $work $Folder)
        $received = ''
        foreach ($line in $run.Lines) { if ($line -match '^fake player received: (.*)$') { $received = $Matches[1] } }
        $run.Received = $received
        $run.Text = Read-Log $run.Log
        return $run
    }
    # Every command line in the log names the fake player; titles never reach the log.
    function Test-MediaSafety($Run, [string]$Name) {
        $foreign = [regex]::Matches($Run.Text, '" (sent|not sent) to (?!' + [regex]::Escape($fakeId) + '[: ])\S+')
        Record "${Name}: no command went to any app but the fake player" ($foreign.Count -eq 0)
        Record "${Name}: no track title in the log" (-not $Run.Text.Contains('Test track'))
    }
    function Test-CleanStop($Run, [string]$Name) {
        Record "${Name}: quit stopped the media thread cleanly and in time" ($Run.Text.Contains('music apps: stopped cleanly') -and $Run.Text -match 'the media thread returned in time')
    }

    # 15. Everything applied.
    $run = Invoke-Media '15. media remote: basic' 'basic' 'play;pause;toggle;next;previous;repeat-all;repeat-one;repeat-off;stop;quit' 'lad-media-basic'
    Record 'media basic: fake player published and the script finished' ($run.Exit -eq 0)
    Record 'media basic: the fake player received every command in order' ($run.Received -eq 'play, pause, play, next, previous, loop all, loop one, loop off, stop')
    Record 'media basic: all nine steps done' (([regex]::Matches($run.Text, 'step \d+ "[a-z-]+": done; state: ready')).Count -eq 9)
    Test-CleanStop $run 'media basic'
    Test-MediaSafety $run 'media basic'

    # 16. The player ignores pause.
    $run = Invoke-Media '16. media remote: player ignores pause' 'ignore-pause' 'play;pause;quit' 'lad-media-ignore'
    Record 'media ignore-pause: play done, pause "no effect"' ($run.Exit -eq 0 -and $run.Text.Contains('step 1 "play": done') -and $run.Text.Contains('step 2 "pause": no effect') -and $run.Received -eq 'play, pause')
    Record 'media ignore-pause: the message says the app did not respond and what to do' ($run.Text.Contains("$fakeId did not respond. Try again, or use $fakeId itself."))
    Test-MediaSafety $run 'media ignore-pause'

    # 17. The player offers neither loop nor next.
    $run = Invoke-Media '17. media remote: no loop, no next' 'no-repeat' 'repeat-all;next;read;quit' 'lad-media-norepeat'
    Record 'media no-repeat: the snapshot offers neither loop nor next and reports no loop' ($run.Text.Contains('step 3 "read": state: ready; app: "' + $fakeId + '"; status: paused; offers: play stop previous; loop: not reported; timeline: no; title shared: yes'))
    Record 'media no-repeat: both "not offered" and nothing sent' ($run.Exit -eq 0 -and $run.Text.Contains('step 1 "repeat-all": not offered') -and $run.Text.Contains('step 2 "next": not offered') -and $run.Received -eq 'nothing')
    Record 'media no-repeat: the messages name the reason' ($run.Text.Contains("$fakeId does not let other programs change looping. Change it in $fakeId itself.") -and $run.Text.Contains($fakeId + ' does not offer "next track" right now.'))

    # 18. The player closes its session while a command is in flight.
    $run = Invoke-Media '18. media remote: player vanishes' 'vanish' 'play;pause;play;quit' 'lad-media-vanish'
    Record 'media vanish: "session gone", then waiting for the app, nothing re-sent' ($run.Exit -eq 0 -and $run.Text.Contains('step 2 "pause": session gone; state: waiting for the app') -and $run.Text.Contains('step 3 "play": no session; state: waiting for the app') -and $run.Received -eq 'play, pause')
    Record 'media vanish: the message says the picked app closed and what to do' ($run.Text.Contains("$fakeId has closed. Start it again, or pick another app below."))
    Test-CleanStop $run 'media vanish'

    # 19. The player appears after the mod started.
    $run = Invoke-Media '19. media remote: player appears late' 'late' 'play;read;quit' 'lad-media-late'
    Record 'media late: the snapshot reads the player (playing, its controls, loop off, a title, no timeline)' ($run.Text.Contains('step 2 "read": state: ready; app: "' + $fakeId + '"; status: playing; offers: pause stop next previous loop; loop: off; timeline: no; title shared: yes'))
    Record 'media late: waiting for the app, then ready and play done' ($run.Exit -eq 0 -and $run.Text.Contains('the pick took effect; state: waiting for the app') -and $run.Text.Contains('the test player is ready after') -and $run.Text.Contains('step 1 "play": done') -and $run.Received -eq 'play')

    # 20. The guard: commands may only go to an id that is not the fake player.
    $run = Invoke-Media '20. media remote: test guard' 'guard' 'play;next;quit' 'lad-media-guard' @{ RACING_MASTER_MOD_TEST_MEDIA_PICK = $fakeId; RACING_MASTER_MOD_TEST_MEDIA_APP = 'RacingMasterMod.TestHost.NotThisOne' }
    Record 'media guard: both commands refused, the fake player received nothing' ($run.Exit -eq 0 -and ([regex]::Matches($run.Text, 'refused to send')).Count -eq 2 -and $run.Received -eq 'nothing')

    # 21. The game's Shutdown (sleep and wake) and a media thread that stops answering.
    $run = Invoke-Media '21. media remote: sleep, wake, not answering, rebuild mode' 'lifecycle' 'play;sleep;wake;pause;hang;rebuild;play;repeat-all;pause;next;quit' 'lad-media-lifecycle'
    Record 'media lifecycle: sleep lets go but keeps the thread' ($run.Exit -eq 0 -and $run.Text.Contains('step 2 "sleep": state: off; the media thread is still running'))
    Record 'media lifecycle: wake controls the picked app again' ($run.Text -match 'step 3 "wake": state: ready' -and $run.Text.Contains('step 4 "pause": done'))
    Record 'media lifecycle: a stuck media thread shows as "not answering" and recovers' ($run.Text -match 'step 5 "hang": the snapshot said Windows is not answering after \d+ ms; ready again afterwards')
    Record 'media lifecycle: the freshness check ran and agreed' ($run.Text.Contains('music apps: a fresh check agrees with the held session'))
    Record 'media lifecycle: rebuild mode sends and confirms through a fresh manager' (([regex]::Matches($run.Text, 'step (7|8|9|10) "[a-z-]+": done')).Count -eq 4 -and $run.Received -eq 'play, pause, play, loop all, pause, next')
    Test-CleanStop $run 'media lifecycle'
    Test-MediaSafety $run 'media lifecycle'

    # 22. Without the test flag the mod stays dormant: no media code runs, nothing is sent.
    $run = Invoke-Media '22. media mode, flag unset: mod must stay dormant' 'basic' 'play;pause;quit' 'lad-media-dormant' @{} $false 4
    Record 'media dormant: the fake player was published, no log folder, nothing received' ((@($run.Lines -match '^fake player published at start: yes').Count -eq 1) -and -not (Test-Path (Join-Path $run.LogRoot 'RacingMasterMod')) -and $run.Received -eq 'nothing')

    # --- music panel ------------------------------------------------------------------------------
    # 23. [panel] and [keys]: good values are taken, bad ones refused with a reason; the mod runs on.
    $panelIniRoot = Join-Path $work 'lad-ini-panel'
    Write-Ini (Join-Path $panelIniRoot 'RacingMasterMod\settings.ini') @('[mod]', 'marker_seconds=3', '[panel]', 'language=zh-TW', 'text_size=huge', 'mode=sometimes', 'source=other_app', 'colour=red', '[keys]', 'next=F2', 'jump=F3') 'utf8'
    $run = Invoke-Run '23. settings.ini with [panel] and [keys] values' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $panelIniRoot
    $log = Read-Log $run.Log
    Record 'panel settings: bad values refused with a reason, unknown keys named, mod keeps running' ($log.Contains('"text_size=huge" is not small, medium or large') -and $log.Contains('"mode=sometimes" is not off, display or on') -and $log.Contains('unknown key "colour" under [panel]') -and $log.Contains('unknown key "jump" under [keys]') -and $log.Contains('settings: stage 2, marker for 3 s') -and ($run.Lines -match '^marker: found').Count -eq 1)

    # 25-30. Screenshots: English and the draft Chinese at each text size.
    foreach ($language in 'en', 'zh-TW') {
        foreach ($size in 'small', 'medium', 'large') {
            $prefix = "panel-$language-$size"
            Invoke-PanelRun "panel screenshots, $language, $size text" 'shots' $prefix @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = $language; RACING_MASTER_MOD_TEXT_SIZE = $size } @() | Out-Null
        }
    }

    # 31-32. "Other app": an app that allows everything and reports a timeline, and no app at all.
    Invoke-PanelRun 'panel, Other app allowing everything' 'shot1' 'panel-en-medium-other-app-full' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_SOURCE = 'other_app'; RACING_MASTER_MOD_TEST_REMOTE = 'full' } @() | Out-Null
    Invoke-PanelRun 'panel, Other app with nothing playing' 'shot1' 'panel-zh-TW-medium-other-app-none' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'zh-TW'; RACING_MASTER_MOD_SOURCE = 'other_app'; RACING_MASTER_MOD_TEST_REMOTE = 'none' } @() | Out-Null

    # 33. ResizeBuffers while the panel and its list are drawn.
    # The long example list (Japanese, Korean, CJK Extension B, a file that cannot be played, a long title).
    Invoke-PanelRun 'panel and ResizeBuffers' 'resize' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LIST = 'open'; RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_TEST_PLAYLIST = 'long' } @() | Out-Null

    # 34. A fault inside the panel, between its two state swaps.
    $panelRun = Invoke-PanelRun 'panel fault' 'fault' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_TEST_PANEL_FAULT = '60' } @()
    Record 'panel fault: one log line names the exception and the module and says the mod switched itself off' ($panelRun.Log -match 'frame code failed \(exception 0x[0-9A-Fa-f]{8} at .+version\.dll\+0x[0-9a-f]+\) and has switched itself off')

    # 35. [panel] mode=display: drawn, but no window procedure and no keys.
    $panelRun = Invoke-PanelRun 'panel display mode' 'display' '' @{} @('[panel]', 'mode=display')
    Record 'panel display mode: the log says so and the game window is not watched' ($panelRun.Log.Contains('mode display only') -and -not $panelRun.Log.Contains('watching the game window'))

    # 24. Keys chosen in [keys]: taken by the panel; an unknown key name keeps the default and says why.
    $keysRoot = Join-Path $work 'lad-ini-keys'
    Write-Ini (Join-Path $keysRoot 'RacingMasterMod\settings.ini') @('[mod]', 'marker_seconds=0', '[keys]', 'panel=none', 'next=Ctrl+F2', 'stop=Banana', 'media_keys=0') 'utf8'
    $run = Invoke-Run '24. settings.ini with keys chosen by the player' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $keysRoot
    $log = Read-Log $run.Log
    Record 'keys: the chosen keys are used, an unknown name keeps the default with a reason' ($log.Contains('panel: keys none panel, F5 stop, F6 previous, F7 play or pause, Ctrl+F2 next, F9 repeat; media keys off') -and $log.Contains('"stop=Banana" under [keys] is not a key name the mod knows; the default stays'))

    # 36. Input through the window procedure: clicks, pass-through, hotkeys, Insert and the rest.
    $panelRun = Invoke-PanelRun 'panel input' 'input' '' @{ RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_TEST_TEXT_FIELD = '1' } @()
    Record 'panel input: the player and the other app were told what the keys and clicks asked for' ($panelRun.Log.Contains('test app: pause') -and -not $panelRun.Log.Contains('test app: next') -and -not $panelRun.Log.Contains('test app: stop') -and $panelRun.Log.Contains('test player: the game is not in focus') -and $panelRun.Log.Contains('test player: files added') -and $panelRun.Log.Contains('panel: settings saved to'))

    Write-Host ''
    Write-Host 'Summary'
    foreach ($r in $results) { Write-Host ("  " + $(if ($r.Ok) { 'PASS' } else { 'FAIL' }) + "  $($r.Name)") }
    $failed = @($results | Where-Object { -not $_.Ok }).Count
    Write-Host ''
    Write-Host ("$($results.Count - $failed) of $($results.Count) checks passed" + $(if ($failed) { "; $failed FAILED" } else { '' }))
} finally {
    Remove-Item Env:\RACING_MASTER_MOD_TEST_HOST -ErrorAction SilentlyContinue
    Remove-Item Env:\RACING_MASTER_MOD_TEST_FAULT -ErrorAction SilentlyContinue
    foreach ($name in 'APP', 'SCRIPT', 'PICK') {
        Remove-Item "Env:\RACING_MASTER_MOD_TEST_MEDIA_$name" -ErrorAction SilentlyContinue
    }
    $env:LOCALAPPDATA = $realLocalAppData
    # The panel screenshots outlive the temporary folder (out\ is not committed).
    $shots = Join-Path $work 'screens'
    if (Test-Path $shots) {
        $kept = Join-Path $root 'out\screens'
        New-Item -ItemType Directory -Force $kept | Out-Null
        Copy-Item (Join-Path $shots '*.png') $kept -Force
        Write-Host "Panel screenshots: $kept"
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 } else { exit 0 }

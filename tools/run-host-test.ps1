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
# Every run's fake player has an app id of its own, so two runs can never command each other's.
#  15. basic       every command reaches the fake player in order and is confirmed; clean stop
#  16. ignore-pause the player accepts pause and keeps playing: "no effect"
#  17. no-repeat   the player offers no loop and no next: "not offered", nothing sent
#  18. vanish      the player closes its session on pause: "session gone", then waiting for it
#  19. late        the player appears 2 s after the mod: waiting, then ready without a restart
#  20. guard       commands may only go to another id: refused, nothing reaches the fake player
#  21. lifecycle   the game's Shutdown (sleep, wake), a media thread that stops answering, the
#                  freshness check, and rebuild mode (the fallback when change notices stop)
#  22. media, flag unset: the mod stays dormant, no log folder, nothing sent
#  22b. test player hidden     as in the game: a RacingMasterMod.TestHost. session is never listed,
#                  followed or commanded (RACING_MASTER_MOD_TEST_HIDE_TEST_PLAYERS=1)
#
# Music engine (src\music):
#  23. musictest.exe               silent: decoding, playlist logic, gapless, persistence, picker.
#                                  One check per PASS/FAIL line it prints; SKIPPED lines are listed
#                                  with their reason and never count as passes.
#  24. ini with only [music]       the music engine's section is not reported as "changed nothing"
# Music panel (src\ui), with an example "Other app" inside the mod (no real media session is read
# or sent anything). Screenshots are copied to out\screens before the temporary folder is removed.
#  25. [panel] and [keys] values   good values accepted, bad ones refused with a reason, mod keeps running
#  26. [keys] chosen by the player the panel uses them; an unknown key name keeps the default and says why
#  27-32. screenshots              English and draft Chinese, small/medium/large text: bar, list,
#                                  settings row, "Other app" (24:25 of a 1:12:30 video); PNG files
#                                  with pixel checks; the bar at the bottom centre with the list and
#                                  settings row above it; no time cut off, times on the bar's line
#  33-34. "Other app" variants     an app that allows only play and pause, and no app at all
#  34b. top centre from the ini    corner=top-center is read, the bar flush at the top centre
#                                  (each screenshot run also visits all six places, P1-P4)
#  35. panel and ResizeBuffers     five sizes with the panel, list and settings row drawing (large
#                                  text): the bar stays at the bottom centre, all of it on screen
#  36. fault inside the panel      game state put back, nothing leaked, mouse and keys back to the game
#  37. [panel] mode=display        drawn, but nothing taken from the game
#  38. panel input                 clicks, pass-through, hotkeys, Insert, wheel, ClearInputMouse after
#                                  a gap, drop-down closing, text field, focus, "Other app", a drag
#                                  let go outside the game window, heartbeat
# Integration of the panel, the music engine and the media remote:
#  39. [panel] mode=off            the loader alone: no panel, no music player, no other-app control
#  40. end to end, "My files"      songs from musictest.exe --fixtures (FLAC, MP3, tagged Opus and OGG
#                                  Vorbis); Insert, a click plays and the position advances, F7, F8,
#                                  F9, Add files through the picker (no window), focus pause, a
#                                  screenshot with the marker (kept in out\preview\integrated), a
#                                  close the game refuses (saved at once, music plays on), then a
#                                  real close after which the process ends at once (final save on disk)
#  41. end to end, "Other app"     the real media remote and the silent fake player: play, F8, F9,
#                                  F7 reach it and nothing else; screenshot in out\preview\integrated
#  42. settings.ini beside the DLL ignored, and the log says so once, with the two paths it reads
#  43-44. panel switched off       a fault in the window procedure, then a Dear ImGui assertion (both
#                                  at the first F12): the panel closes, the game gets every key and
#                                  click, and the worker pauses the song that was playing
#  45. two threads presenting      the same swap chain from two threads at once (as NVIDIA Smooth
#                                  Motion may): the mod draws only on the first, whose pipeline
#                                  state stays intact; the other is passed on and counted; the
#                                  frames move after the first is quiet for a second;
#                                  the present probe is off: nothing recorded, nothing logged
#  46. present probe on            the same with [debug] present_probe=1: the probe block's values
#                                  (threads, callers, tables, flags, Windows' present count), and
#                                  the Smooth Motion line pointed at a DLL the host loads
#
# Usage:  powershell -ExecutionPolicy Bypass -File tools\run-host-test.ps1
# Exit code 0 = every check passed; 3 = not started because the game is running.

$ErrorActionPreference = 'Stop'

# Never while the game runs. The media runs publish silent fake players, and Windows shares media
# sessions with every program on the PC: on 2026-11-15 the owner's in-game panel ("Other app")
# switched to the test's fake players while a suite ran. The mod now ignores them outside the test
# host as well, but the test is not run beside a game session at all. Only the process names are
# looked at; nothing is opened or read. RACING_MASTER_MOD_TEST_GAME_NAMES (comma-separated) replaces
# the names, so this refusal itself can be checked without the game.
$gameNames = @('g112-Win64-Shipping', 'g112')
if ($env:RACING_MASTER_MOD_TEST_GAME_NAMES) { $gameNames = @($env:RACING_MASTER_MOD_TEST_GAME_NAMES -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
$runningGame = @(Get-Process -Name $gameNames -ErrorAction SilentlyContinue)
if ($runningGame.Count) {
    $unique = @($runningGame | ForEach-Object { "$($_.Name).exe" } | Sort-Object -Unique)
    $verb = if ($unique.Count -gt 1) { 'are' } else { 'is' }
    Write-Host "The test was not started: $($unique -join ' and ') $verb running. The test publishes fake media players that every program on this PC can see, including the mod's panel inside the game. Close the game and run the test again."
    exit 3
}

$root = Split-Path -Parent $PSScriptRoot
$dll = Join-Path $root 'out\version.dll'
$hostExe = Join-Path $root 'out\testhost.exe'
$bystanderExe = Join-Path $root 'out\bystander.exe'
$musicTestExe = Join-Path $root 'out\musictest.exe'
foreach ($file in $dll, $hostExe, $bystanderExe, $musicTestExe) {
    if (-not (Test-Path $file)) {
        throw 'Build first: powershell -ExecutionPolicy Bypass -File build\build.ps1'
    }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("racing-master-mod-hosttest-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $work | Out-Null
Copy-Item $dll, $hostExe, $bystanderExe $work

$realLocalAppData = $env:LOCALAPPDATA
$results = @()
$skipped = @()
$notes = @()   # facts about this PC that did not fail a check but should be read (see the summary)
$readManager = $null            # media session reading (Get-MediaSessionIds), set up on first use
$stuckPlayers = @()             # fake players from other runs that outlasted the wait (reported once)
$sessionsUnreadable = $false
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

# A quiet sine tone as a 16-bit mono WAV file (our own content, generated each run). The
# frequency must divide the sample rate, so one period is a whole number of samples.
function New-ToneWav([string]$Path, [int]$Frequency, [int]$Seconds) {
    $rate = 22050
    $period = [int]($rate / $Frequency)
    $one = New-Object byte[] ($period * 2)
    for ($i = 0; $i -lt $period; $i++) {
        $value = [int][Math]::Round(0.25 * 32767 * [Math]::Sin(2 * [Math]::PI * $i / $period))
        [BitConverter]::GetBytes([int16]$value).CopyTo($one, $i * 2)
    }
    $periods = [int]($rate * $Seconds / $period)
    $dataBytes = $periods * $one.Length
    $stream = New-Object IO.MemoryStream
    $writer = New-Object IO.BinaryWriter($stream)
    $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF')); $writer.Write([int](36 + $dataBytes))
    $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt ')); $writer.Write([int]16); $writer.Write([int16]1)
    $writer.Write([int16]1); $writer.Write([int]$rate); $writer.Write([int]($rate * 2)); $writer.Write([int16]2)
    $writer.Write([int16]16); $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([int]$dataBytes)
    for ($p = 0; $p -lt $periods; $p++) { $writer.Write($one) }
    $writer.Flush()
    [IO.File]::WriteAllBytes($Path, $stream.ToArray())
    $writer.Dispose()
}

# The songs every panel run finds in its music folder, which the real music engine scans at start.
# Neutral names in several scripts (Traditional Chinese, Japanese, Korean, a character outside the
# basic plane) and one long title, so the screenshots show how the panel draws them.
$songs = Join-Path $work 'songs'
New-Item -ItemType Directory -Force $songs | Out-Null
$songNames = @(
    @{ Name = '01 Alpha ' + [string][char]0x6E2C + [string][char]0x8A66 + '.wav'; Hz = 441 },
    @{ Name = '02 Beta ' + [string][char]0x30C6 + [string][char]0x30B9 + [string][char]0x30C8 + '.wav'; Hz = 630 },
    @{ Name = '03 Gamma ' + [string][char]0xD14C + [string][char]0xC2A4 + [string][char]0xD2B8 + '.wav'; Hz = 882 },
    @{ Name = '04 Delta ' + [char]::ConvertFromUtf32(0x2070E) + '.wav'; Hz = 490 },
    @{ Name = '05 A very long song title that keeps going well past the edge of the panel.wav'; Hz = 735 }
)
foreach ($song in $songNames) { New-ToneWav (Join-Path $songs $song.Name) $song.Hz 20 }
# The same songs plus a file named .wav whose content is not audio (64 fixed bytes), for the runs
# that draw the list (screenshots, resize): the engine marks it as unplayable and the panel must
# draw it in that state (the engine finds out on the first try to play it: the screenshot script
# clicks it before the list shot).
$songsWithUnplayable = Join-Path $work 'songs-with-unplayable'
New-Item -ItemType Directory -Force $songsWithUnplayable | Out-Null
Copy-Item (Join-Path $songs '*') $songsWithUnplayable
[IO.File]::WriteAllBytes((Join-Path $songsWithUnplayable '06 Cannot be played.wav'), [byte[]](1..64))

# One music panel run (testhost.exe panel ...). marker_seconds=0 so nothing but the panel is on
# screen; each run is also checked for a Dear ImGui assertion or reported mistake in its log.
function Invoke-PanelRun([string]$Title, [string]$Script, [string]$Name, [hashtable]$Environment, [string[]]$Ini, [string]$Songs = $songs, [bool]$AssertExpected = $false, [bool]$WindowWatched = $true) {
    $logRoot = Join-Path $work ('lad-panel-' + ($Title -replace '[^A-Za-z0-9]+', '-'))
    Write-Ini (Join-Path $logRoot 'RacingMasterMod\settings.ini') (@('[mod]', 'marker_seconds=0') + $Ini) 'utf8'
    New-Item -ItemType Directory -Force (Join-Path $logRoot 'RacingMasterMod\music') | Out-Null
    Copy-Item (Join-Path $Songs '*') (Join-Path $logRoot 'RacingMasterMod\music')
    $arguments = @('panel', $work, '10', $Script)
    if ($Name) { $arguments += $Name }
    $result = Invoke-Run $Title (Join-Path $work 'testhost.exe') $arguments ($flag + $Environment) $logRoot
    $text = Read-Log $result.Log
    Record "$($Title): host checks pass" ($result.Exit -eq 0)
    if ($AssertExpected) {
        # Only the deliberate assertion, logged once.
        Record "$($Title): only the deliberate Dear ImGui assertion in the log, once" (([regex]::Matches($text, 'IMGUI ASSERT')).Count -eq 1 -and $text.Contains('IMGUI ASSERT "test host: a deliberate assertion"') -and -not $text.Contains('reported a mistake'))
    } else {
        Record "$($Title): no Dear ImGui assertion or reported mistake in the log" (-not ($text -match 'IMGUI ASSERT|reported a mistake'))
    }
    if ($WindowWatched) {
        # The host closes its window with WM_CLOSE: the mod's window procedure tells the worker, and
        # waits after WM_DESTROY until the music is stopped and saved (at most half a second).
        Record "$($Title): when the game window closed, the music was stopped and saved before the window was gone" ($text.Contains('music: the game window is closing; stopping the music and saving the list') -and $text.Contains('music: stopped and saved') -and @($result.Lines -match 'music stopped and saved before the window was gone: yes$').Count -eq 1)
    } else {
        # No window procedure (display mode): the worker's window watch is the backstop.
        Record "$($Title): when the game window closed, the music was stopped and saved" ($text.Contains('music: the game window has closed; stopping the music and saving the list') -and $text.Contains('music: stopped and saved'))
    }
    # Frames drawn while the window still existed must not count as "frames still arrive".
    Record "$($Title): no ""frames still arrive"" line at an ordinary close" (-not $text.Contains('frames still arrive'))
    return @{ Exit = $result.Exit; Log = $text; Lines = $result.Lines }
}
$gameFirstFrame = 'first frame: 2558x1360, format 24, buffers 2, swap effect 4, windowed 1, samples 1'

try {
    # 1. Game layout, mod active.
    $run = Invoke-Run '1. game layout, mod active (T1-T6)' (Join-Path $work 'testhost.exe') @('game', $work, '8') $flag (Join-Path $work 'lad-active')
    $log = Read-Log $run.Log
    Record 'game layout: host checks T1-T6 pass' ($run.Exit -eq 0)
    Record 'game layout: log has the first-frame line the real game wrote' ($log.Contains($gameFirstFrame))
    Record 'version 0.3.1: in the DLL''s version resource and in the first log line' ([Diagnostics.FileVersionInfo]::GetVersionInfo($dll).FileVersion -eq '0.3.1' -and $log.Contains('Racing Master Mod 0.3.1 (') -and $log.Contains(' loaded into '))
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
    # Every run's fake player gets an app id of its own (this prefix plus a fresh suffix), and the
    # mod is told to command only that id. Windows tells sessions apart by app id alone: with one
    # fixed id for every run, a fake player published by another test run on this PC at the same
    # time took this run's commands (reproduced: 14 checks failed, the other process received
    # every command). See docs/agent-notes/test-host.md.
    $fakePrefix = 'RacingMasterMod.TestHost.FakePlayer'
    function New-FakeId { $fakePrefix + '.' + [Guid]::NewGuid().ToString('N').Substring(0, 12) }
    # The app ids in Windows' media session list, read only: no title is read and nothing is sent.
    # Returns @{ Ok; Ids } (Ok is false when Windows' media session API cannot be reached from here).
    function Get-MediaSessionIds {
        try {
            if (-not $script:readManager) {
                Add-Type -AssemblyName System.Runtime.WindowsRuntime
                $managerType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
                $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
                    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
                $script:readManager = $asTask.MakeGenericMethod($managerType)
            }
            $task = $script:readManager.Invoke($null, @([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()))
            if (-not $task.Wait(3000)) { return @{ Ok = $false; Ids = @() } }
            return @{ Ok = $true; Ids = @($task.Result.GetSessions() | ForEach-Object { $_.SourceAppUserModelId }) }
        } catch {
            return @{ Ok = $false; Ids = @() }
        }
    }
    # Before a run publishes its fake player: wait (at most 10 s) until no fake player from an
    # earlier run is listed. One that stays is reported in plain words and in the summary, once,
    # and not waited for again; the run still goes ahead, because its own player has an id of its
    # own and cannot be confused with it. Sessions of other apps (a browser, a music player) are
    # ignored and never sent anything.
    function Wait-NoOtherFakePlayers([string]$Title) {
        $deadline = [DateTime]::Now.AddSeconds(10)
        do {
            $list = Get-MediaSessionIds
            if (-not $list.Ok) {
                if (-not $script:sessionsUnreadable) {
                    $script:sessionsUnreadable = $true
                    $script:notes += "${Title}: Windows' media session list could not be read from PowerShell, so the script did not check for fake players left by other runs"
                }
                return
            }
            $left = @($list.Ids | Where-Object { $_ -like "$fakePrefix*" -and $script:stuckPlayers -notcontains $_ })
            if ($left.Count -eq 0) { return }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::Now -lt $deadline)
        $script:stuckPlayers += $left
        $note = "${Title}: Windows still listed a fake test player from another run after 10 s ($($left -join ', ')). Most likely another test run is going on on this PC; only one should run at a time. This run went ahead: its own fake player has an id of its own, so neither run can command the other's player. Later runs do not wait for it again."
        Write-Host "    $note"
        $script:notes += $note
    }
    # Runs one media scenario. The mod's script sets what is sent; the scenario sets how the fake
    # player behaves. Returns the run plus what the fake player received and the id it used.
    function Invoke-Media([string]$Title, [string]$Scenario, [string]$Script, [string]$Folder, [hashtable]$Extra = @{}, [bool]$WithFlag = $true, [int]$Seconds = 40, [string]$FakeId = '') {
        if (-not $FakeId) { $FakeId = New-FakeId }
        $environment = @{ RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID = $FakeId; RACING_MASTER_MOD_TEST_MEDIA_APP = $FakeId; RACING_MASTER_MOD_TEST_MEDIA_SCRIPT = $Script }
        if ($WithFlag) { $environment.RACING_MASTER_MOD_TEST_HOST = '1' }
        foreach ($key in $Extra.Keys) { $environment[$key] = $Extra[$key] }
        Wait-NoOtherFakePlayers $Title
        $run = Invoke-Run $Title (Join-Path $work 'testhost.exe') @('media', $work, "$Seconds", $Scenario) $environment (Join-Path $work $Folder)
        $received = ''
        foreach ($line in $run.Lines) { if ($line -match '^fake player received: (.*)$') { $received = $Matches[1] } }
        $run.Received = $received
        $run.Text = Read-Log $run.Log
        $run.Id = $FakeId
        return $run
    }
    # Every command line in the log names this run's fake player; titles never reach the log.
    function Test-MediaSafety($Run, [string]$Name) {
        $foreign = [regex]::Matches($Run.Text, '" (sent|not sent) to (?!' + [regex]::Escape($Run.Id) + '[: ])\S+')
        Record "${Name}: no command went to any app but the fake player" ($foreign.Count -eq 0)
        Record "${Name}: no track title in the log" (-not $Run.Text.Contains('Test track'))
    }
    function Test-CleanStop($Run, [string]$Name) {
        Record "${Name}: quit stopped the media thread cleanly and in time" ($Run.Text.Contains('music apps: stopped cleanly') -and $Run.Text -match 'the media thread returned in time')
    }

    # 15. Everything applied.
    $run = Invoke-Media '15. media remote: basic' 'basic' 'play;pause;toggle;next;previous;repeat-all;repeat-one;repeat-off;stop;quit' 'lad-media-basic'
    Record 'media basic: fake player published and the script finished' ($run.Exit -eq 0)
    Record 'media basic: the fake player published under this run''s own app id' (@($run.Lines -eq "fake player: app id $($run.Id)").Count -eq 1)
    Record 'media basic: the fake player received every command in order' ($run.Received -eq 'play, pause, play, next, previous, loop all, loop one, loop off, stop')
    Record 'media basic: all nine steps done' (([regex]::Matches($run.Text, 'step \d+ "[a-z-]+": done; state: ready')).Count -eq 9)
    Test-CleanStop $run 'media basic'
    Test-MediaSafety $run 'media basic'

    # 16. The player ignores pause.
    $run = Invoke-Media '16. media remote: player ignores pause' 'ignore-pause' 'play;pause;quit' 'lad-media-ignore'
    Record 'media ignore-pause: play done, pause "no effect"' ($run.Exit -eq 0 -and $run.Text.Contains('step 1 "play": done') -and $run.Text.Contains('step 2 "pause": no effect') -and $run.Received -eq 'play, pause')
    Record 'media ignore-pause: the message says the app did not respond and what to do' ($run.Text.Contains("$($run.Id) did not respond. Try again, or use $($run.Id) itself."))
    Test-MediaSafety $run 'media ignore-pause'

    # 17. The player offers neither loop nor next.
    $run = Invoke-Media '17. media remote: no loop, no next' 'no-repeat' 'repeat-all;next;read;quit' 'lad-media-norepeat'
    Record 'media no-repeat: the snapshot offers neither loop nor next and reports no loop' ($run.Text.Contains('step 3 "read": state: ready; app: "' + $run.Id + '"; status: paused; offers: play stop previous; loop: not reported; timeline: no; title shared: yes'))
    Record 'media no-repeat: both "not offered" and nothing sent' ($run.Exit -eq 0 -and $run.Text.Contains('step 1 "repeat-all": not offered') -and $run.Text.Contains('step 2 "next": not offered') -and $run.Received -eq 'nothing')
    Record 'media no-repeat: the messages name the reason' ($run.Text.Contains("$($run.Id) does not let other programs change looping. Change it in $($run.Id) itself.") -and $run.Text.Contains($run.Id + ' does not offer "next track" right now.'))

    # 18. The player closes its session while a command is in flight.
    $run = Invoke-Media '18. media remote: player vanishes' 'vanish' 'play;pause;play;quit' 'lad-media-vanish'
    Record 'media vanish: "session gone", then waiting for the app, nothing re-sent' ($run.Exit -eq 0 -and $run.Text.Contains('step 2 "pause": session gone; state: waiting for the app') -and $run.Text.Contains('step 3 "play": no session; state: waiting for the app') -and $run.Received -eq 'play, pause')
    Record 'media vanish: the message says the picked app closed and what to do' ($run.Text.Contains("$($run.Id) has closed. Start it again, or pick another app below."))
    Test-CleanStop $run 'media vanish'

    # 19. The player appears after the mod started.
    $run = Invoke-Media '19. media remote: player appears late' 'late' 'play;read;quit' 'lad-media-late'
    Record 'media late: the snapshot reads the player (playing, its controls, loop off, a title, no timeline)' ($run.Text.Contains('step 2 "read": state: ready; app: "' + $run.Id + '"; status: playing; offers: pause stop next previous loop; loop: off; timeline: no; title shared: yes'))
    Record 'media late: waiting for the app, then ready and play done' ($run.Exit -eq 0 -and $run.Text.Contains('the pick took effect; state: waiting for the app') -and $run.Text.Contains('the test player is ready after') -and $run.Text.Contains('step 1 "play": done') -and $run.Received -eq 'play')

    # 20. The guard: commands may only go to an id that is not the fake player.
    $guardId = New-FakeId
    $run = Invoke-Media '20. media remote: test guard' 'guard' 'play;next;quit' 'lad-media-guard' @{ RACING_MASTER_MOD_TEST_MEDIA_PICK = $guardId; RACING_MASTER_MOD_TEST_MEDIA_APP = 'RacingMasterMod.TestHost.NotThisOne' } -FakeId $guardId
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

    # 22b. The game's view of a test player: outside the test host the mod never lists, follows or
    # commands a session whose app id starts with RacingMasterMod.TestHost. (on 2026-11-15 the
    # owner's panel switched to a suite's fake players). RACING_MASTER_MOD_TEST_HIDE_TEST_PLAYERS=1
    # gives the test host that behaviour; the script still picks the fake player and tries to play.
    $run = Invoke-Media '22b. media remote: the test player hidden as in the game' 'basic' 'play;read;quit' 'lad-media-hidden' @{ RACING_MASTER_MOD_TEST_HIDE_TEST_PLAYERS = '1' } $true 30
    $appsLine = [regex]::Match($run.Text, 'music apps: Windows answered in \d+ ms; [^\r\n]*').Value
    Record 'media hidden: the log says once that the test player is ignored, and it is not among the apps found' ($run.Exit -eq 0 -and ([regex]::Matches($run.Text, 'music apps: ignoring ' + [regex]::Escape($run.Id) + ', a silent player of the mod''s own developer test')).Count -eq 1 -and $appsLine -and -not $appsLine.Contains($run.Id))
    Record 'media hidden: the picked test player never appears, play finds no app, nothing reaches it' ($run.Text.Contains('the test player did not appear') -and $run.Text -match 'step 1 "play": no session' -and $run.Text.Contains('step 2 "read": state: waiting for the app;') -and $run.Received -eq 'nothing')

    # --- music engine ---------------------------------------------------------------------------------
    # 23. The engine's own test program: no sound device (manual and null output only), no window,
    # the file dialog configured but never shown. It links the same objects as version.dll.
    $run = Invoke-Run '23. musictest.exe: the music engine, silent' $musicTestExe @((Join-Path $work 'music'), $root) @{} (Join-Path $work 'lad-music')
    foreach ($line in $run.Lines) {
        if ($line -match '^PASS (.+)$') { Record "music: $($Matches[1])" $true }
        elseif ($line -match '^FAIL (.+)$') { Record "music: $($Matches[1])" $false }
        elseif ($line -match '^SKIPPED (.+)$') { $skipped += "music: $($Matches[1])" }
    }
    # The total line proves the program ran to its end (a crash part-way would leave cases unseen).
    Record 'music: the test program ran to the end and reported its total' (@($run.Lines -match '^musictest: \d+ passed').Count -eq 1)
    $log = Read-Log $run.Log
    Record 'music log: tracks start, end and the list finishes, in plain lines' ($log.Contains('music: started ') -and $log.Contains('music: ended ') -and $log.Contains('music: list finished'))
    Record 'music log: a missing file is skipped with its reason' ($log -match 'music: skipped .+ \(file not found\)')
    Record 'music log: the watchdog restarted the output after the device loss' ($log.Contains('restarting the output device'))
    Record 'music log: Chinese file names are written in full (UTF-8)' ($log.Contains('music: started A ' + [string][char]0x6E2C + [string][char]0x8A66 + ' 440.wav'))
    Record 'music log: a settings.ini the engine cannot read is left as it is, and the log says why' ($log.Contains('music: settings.ini is there but could not be read just now (Windows error 223: larger than 1 MB), so it was left as it is'))
    Record 'music: no real audio device was opened (manual and null output only)' ($log.Contains('music: output opened (manual') -and -not $log.Contains('output opened (shared mode'))

    # 24. Only a [music] section (what the music engine writes when the player has no settings
    # file yet): the loader must not report that the file "changed nothing".
    $musicIniRoot = Join-Path $work 'lad-ini-music'
    Write-Ini (Join-Path $musicIniRoot 'RacingMasterMod\settings.ini') @('[music]', 'volume=60', 'loop=all') 'utf8'
    $run = Invoke-Run '24. settings.ini with only a [music] section' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $musicIniRoot
    $log = Read-Log $run.Log
    Record 'ini with only [music]: no "changed nothing" line, mod on' ($log.Contains('settings: read') -and -not $log.Contains('changed nothing') -and ($run.Lines -match '^marker: found').Count -eq 1)

    # --- music panel ------------------------------------------------------------------------------
    # 25. [panel] and [keys]: good values are taken, bad ones refused with a reason; the mod runs on.
    $panelIniRoot = Join-Path $work 'lad-ini-panel'
    Write-Ini (Join-Path $panelIniRoot 'RacingMasterMod\settings.ini') @('[mod]', 'marker_seconds=3', '[panel]', 'language=zh-TW', 'text_size=huge', 'mode=sometimes', 'source=other_app', 'colour=red', 'corner=middle', '[keys]', 'next=F2', 'jump=F3') 'utf8'
    $run = Invoke-Run '25. settings.ini with [panel] and [keys] values' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $panelIniRoot
    $log = Read-Log $run.Log
    Record 'panel settings: bad values refused with a reason, unknown keys named, mod keeps running' ($log.Contains('"text_size=huge" is not small, medium or large') -and $log.Contains('"mode=sometimes" is not off, display or on') -and $log.Contains('unknown key "colour" under [panel]') -and $log.Contains('"corner=middle" is not top-left, top-centre, top-right, bottom-left, bottom-centre or bottom-right') -and $log.Contains('unknown key "jump" under [keys]') -and $log.Contains('settings: stage 2, marker for 3 s') -and ($run.Lines -match '^marker: found').Count -eq 1)

    # 27-32. Screenshots: English and the draft Chinese at each text size.
    foreach ($language in 'en', 'zh-TW') {
        foreach ($size in 'small', 'medium', 'large') {
            $prefix = "panel-$language-$size"
            # One of them also has a settings.ini over the 16 KB the panel reads (a long comment):
            # choosing "Other app" must leave that file alone rather than cut off its end.
            $extra = @()
            if ($language -eq 'en' -and $size -eq 'small') { $extra = @('; ' + ('x' * 17000), '[keys]', 'next=F8') }
            $panelRun = Invoke-PanelRun "panel screenshots, $language, $size text" 'shots' $prefix @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = $language; RACING_MASTER_MOD_TEXT_SIZE = $size; RACING_MASTER_MOD_TEST_REMOTE = 'full' } $extra $songsWithUnplayable
            Record "panel screenshots, $language, $size text: the engine said why the file could not be played" ($panelRun.Log.Contains('music: skipped 06 Cannot be played.wav (the file is damaged or is not what its name says)'))
            if ($extra.Count) {
                $bigIni = [IO.File]::ReadAllText((Join-Path $work 'lad-panel-panel-screenshots-en-small-text\RacingMasterMod\settings.ini'), [Text.Encoding]::UTF8)
                Record 'a settings.ini over 16 KB: the panel leaves it as it is (its end is kept) and the log says why' ($panelRun.Log.Contains('is larger than 16 KB, more than the panel reads; the file was left as it is') -and $bigIni.Contains('[keys]') -and $bigIni.Contains('next=F8') -and -not $bigIni.Contains('source=other_app'))
            }
        }
    }

    # 33-34. "Other app": an app that allows only play and pause and reports no timeline (the
    # screenshot runs above show one that allows everything, 24:25 into a 1:12:30 video), and no
    # app at all.
    Invoke-PanelRun 'panel, Other app allowing only play and pause' 'shot1' 'panel-en-medium-other-app-limited' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_SOURCE = 'other_app'; RACING_MASTER_MOD_TEST_REMOTE = 'limited' } @() | Out-Null
    Invoke-PanelRun 'panel, Other app with nothing playing' 'shot1' 'panel-zh-TW-medium-other-app-none' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'zh-TW'; RACING_MASTER_MOD_SOURCE = 'other_app'; RACING_MASTER_MOD_TEST_REMOTE = 'none' } @() | Out-Null

    # 34b. The sixth place, top centre, written the American way in settings.ini (0.3.2).
    $panelRun = Invoke-PanelRun 'panel, top centre from settings.ini' 'shot1' 'panel-en-medium-top-center-ini' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'en' } @('[panel]', 'corner=top-center')
    Record 'corner=top-center in settings.ini: taken without a complaint, the bar flush at the top centre' (@($panelRun.Lines -eq 'panel corner: 5').Count -eq 1 -and -not $panelRun.Log.Contains('corner=top-center'))

    # 35. ResizeBuffers while the panel and its list are drawn.
    # The generated songs (Chinese, Japanese, Korean, CJK Extension B, a long title) and one file
    # that cannot be played.
    # Large text with the list and the settings row open: at the small sizes the list gives up rows
    # so that the whole panel stays on screen (R3).
    Invoke-PanelRun 'panel and ResizeBuffers' 'resize' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LIST = 'open'; RACING_MASTER_MOD_SETTINGS_ROW = 'open'; RACING_MASTER_MOD_TEXT_SIZE = 'large'; RACING_MASTER_MOD_LANG = 'en' } @() $songsWithUnplayable | Out-Null

    # 36. A fault inside the panel, between its two state swaps.
    $panelRun = Invoke-PanelRun 'panel fault' 'fault' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_TEST_PANEL_FAULT = '60' } @()
    Record 'panel fault: one log line names the exception and the module and says the mod switched itself off' ($panelRun.Log -match 'frame code failed \(exception 0x[0-9A-Fa-f]{8} at .+version\.dll\+0x[0-9a-f]+\) and has switched itself off')
    Record 'panel fault: the music is paused because nothing can control it any more, and the log says so' ($panelRun.Log.Contains('music: paused, because the panel that controls it has switched off with the rest of the mod'))

    # 37. [panel] mode=display: drawn, but no window procedure and no keys.
    $panelRun = Invoke-PanelRun 'panel display mode' 'display' '' @{} @('[panel]', 'mode=display') $songs $false $false
    Record 'panel display mode: the log says so and the game window is not watched' ($panelRun.Log.Contains('mode display only') -and -not $panelRun.Log.Contains('watching the game window'))
    Record 'panel display mode: the key line says no key works in this mode' ($panelRun.Log.Contains('panel: no keys in display mode ([panel] mode=display); with mode=on they would be Insert panel, F5 stop') -and -not $panelRun.Log.Contains('panel: keys Insert'))

    # 26. Keys chosen in [keys]: taken by the panel; an unknown key name keeps the default and says why.
    $keysRoot = Join-Path $work 'lad-ini-keys'
    Write-Ini (Join-Path $keysRoot 'RacingMasterMod\settings.ini') @('[mod]', 'marker_seconds=0', '[keys]', 'panel=none', 'next=Ctrl+F2', 'stop=Banana', 'media_keys=0') 'utf8'
    $run = Invoke-Run '26. settings.ini with keys chosen by the player' (Join-Path $work 'testhost.exe') @('quick', $work, '1') $flag $keysRoot
    $log = Read-Log $run.Log
    Record 'keys: the chosen keys are used, an unknown name keeps the default with a reason' ($log.Contains('panel: keys none panel, F5 stop, F6 previous, F7 play or pause, Ctrl+F2 next, F9 repeat; media keys off') -and $log.Contains('"stop=Banana" under [keys] is not a key name the mod knows; the default stays'))

    # 38. Input through the window procedure: clicks, pass-through, hotkeys, Insert and the rest.
    # Its settings.ini has a [panel] that ends in blank lines and a [keys] after it, so both writers'
    # section joins are checked at the end (one blank line between sections, CR LF, no BOM). The
    # line before [panel] and one inside it hold only spaces: they look blank and count as blank.
    $panelRun = Invoke-PanelRun 'panel input' 'input' '' @{ RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_TEST_TEXT_FIELD = '1' } @('  ', '[panel]', 'mode=on', '', "`t ", '[keys]', 'panel=Insert')
    Record 'panel input: the music engine and the other app were told what the keys and clicks asked for' ($panelRun.Log.Contains('test app: pause') -and -not $panelRun.Log.Contains('test app: next') -and -not $panelRun.Log.Contains('test app: stop') -and $panelRun.Log.Contains('music: started 02 Beta') -and $panelRun.Log.Contains('music: file dialog suppressed (test mode)') -and $panelRun.Log.Contains('panel: settings saved to'))
    Record 'panel input: the music engine played through the null device only (no sound card opened)' ($panelRun.Log.Contains('music: output opened (null') -and -not $panelRun.Log.Contains('output opened (shared mode'))
    # The panel ([panel] values) and the music engine ([music] section) both rewrite settings.ini
    # during this run; neither may undo the other's lines.
    $inputIni = [IO.File]::ReadAllText((Join-Path $work 'lad-panel-panel-input\RacingMasterMod\settings.ini'), [Text.Encoding]::UTF8)
    Record 'panel input: settings.ini keeps the panel''s and the music engine''s values side by side' ($inputIni.Contains('marker_seconds=0') -and $inputIni.Contains('[panel]') -and $inputIni.Contains('language=en') -and $inputIni.Contains('source=my_files') -and $inputIni.Contains('[music]') -and $inputIni -match 'volume=\d+')
    $inputBytes = [IO.File]::ReadAllBytes((Join-Path $work 'lad-panel-panel-input\RacingMasterMod\settings.ini'))
    $joins = '^\[mod\]\r\nmarker_seconds=0\r\n\r\n\[panel\]\r\nmode=on\r\nlanguage=en\r\ntext_size=\w+\r\npause_when_unfocused=\d\r\nsource=my_files\r\ncorner=bottom-centre\r\n\r\n\[keys\]\r\npanel=Insert\r\n\r\n\[music\]\r\n(\w+=[^\r\n]*\r\n)+$'
    Record 'panel input: after both writers, one blank line between sections, the panel''s new keys right after its last line, CR LF only, no BOM' ($inputIni -match $joins -and -not ($inputIni -match '[^\r]\n|\r[^\n]') -and -not ($inputBytes.Length -ge 3 -and $inputBytes[0] -eq 0xEF -and $inputBytes[1] -eq 0xBB -and $inputBytes[2] -eq 0xBF))
    if (-not ($inputIni -match $joins)) { Write-Host "    settings.ini of the input run:"; Write-Host ($inputIni -replace "`r", '\r' -replace "`n", "\n`n") }
    Record 'panel input: the worker logged the music player start with the full path of the music folder' ($panelRun.Log.Contains('music: the player for your own songs has started; songs copied into ' + (Join-Path $work 'lad-panel-panel-input\RacingMasterMod\music') + ' are added at every start'))

    # --- integration ------------------------------------------------------------------------------
    # 39. [panel] mode=off: the loader alone, as in 0.2.0. Nothing of the music parts starts.
    $offRoot = Join-Path $work 'lad-ini-panel-off'
    Write-Ini (Join-Path $offRoot 'RacingMasterMod\settings.ini') @('[mod]', 'marker_seconds=3', '[panel]', 'mode=off', 'source=other_app') 'utf8'
    $run = Invoke-Run '39. [panel] mode=off: no panel, no music player, no other-app control' (Join-Path $work 'testhost.exe') @('quick', $work, '2') $flag $offRoot
    $log = Read-Log $run.Log
    Record 'mode=off: the marker still shows and the log says why nothing of the music started' (($run.Lines -match '^marker: found').Count -eq 1 -and $log.Contains('panel: off ([panel] mode=off)') -and $log.Contains('music: neither your own songs nor other music apps are started, because the panel is off ([panel] mode=off)'))
    Record 'mode=off: no music player, no other-app control, no window procedure, no music folder' (-not $log.Contains('the player for your own songs has started') -and -not $log.Contains('other app:') -and -not $log.Contains('music apps:') -and -not $log.Contains('watching the game window') -and -not (Test-Path (Join-Path $offRoot 'RacingMasterMod\music')))

    # 40. End to end, "My files": the panel driving the real music engine inside version.dll, with
    # songs made by the engine's own fixture writers, and the loader's marker on screen as well.
    $fixtures = Join-Path $work 'integrated-songs'
    $fx = Invoke-Run '40. musictest.exe --fixtures: songs for the end-to-end run' $musicTestExe @('--fixtures', $fixtures) @{} (Join-Path $work 'lad-fixtures')
    Record 'end to end: the songs were written (FLAC, MP3, tagged Opus and OGG Vorbis, a WAV to add)' ($fx.Exit -eq 0)
    $addFile = Join-Path $fixtures ('add\05 Epsilon ' + [string][char]0x6E2C + [string][char]0x8A66 + '.wav')
    # The host ends its process the moment its window is gone (RACING_MASTER_MOD_TEST_EXIT_AFTER_CLOSE),
    # as the game did on 2026-11-15: the final save must already be on disk by then.
    $panelRun = Invoke-PanelRun '40. end to end: the panel driving the music engine' 'integrated' '' @{ RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_TEST_PICKER_FILES = $addFile; RACING_MASTER_MOD_TEST_EXIT_AFTER_CLOSE = '1' } @('[mod]', 'marker_seconds=600') (Join-Path $fixtures 'music')
    $endIni = [IO.File]::ReadAllText((Join-Path $work 'lad-panel-40-end-to-end-the-panel-driving-the-music-engine\RacingMasterMod\settings.ini'), [Text.Encoding]::UTF8)
    $endPosition = if ($endIni -match 'position_ms=(\d+)') { [long]$Matches[1] } else { -1 }
    Record 'end to end: the process ended straight after the close, and the final save was on disk (the song and a place in it)' (@($panelRun.Lines -eq 'ending the process straight after the close').Count -eq 1 -and $endIni -match 'current=.+\.(flac|mp3|opus|ogg|wav)' -and $endPosition -gt 0)
    Record 'end to end: the close request was seen and the music was not stopped by it' ($panelRun.Log.Contains('music: the game was asked to close; the song and its place are saved now') -and $panelRun.Log.Contains('music: the game is still open 3 s after it was asked to close'))
    Record 'end to end: the engine inside version.dll played the MP3 and then the Opus song' ($panelRun.Log.Contains('music: started 02 Beta') -and $panelRun.Log.Contains('music: started 03 gamma.opus'))
    Record 'end to end: the picker handed over the chosen file from a window owned by the game window' ($panelRun.Log.Contains('music: file dialog suppressed (test mode); reporting 1 path(s) from RACING_MASTER_MOD_TEST_PICKER_FILES (owned by a top-level window)'))
    Record 'end to end: only the null device was opened (nothing audible)' ($panelRun.Log.Contains('music: output opened (null device') -and -not $panelRun.Log.Contains('output opened (shared mode'))
    # The host's window is never shown, so it is never minimised or in front, and has a title bar.
    Record 'end to end: the game window line names the picture size, windowed, behind another window' ($panelRun.Log -match 'game window: \d+x\d+, windowed, behind another window')

    # 41. End to end, "Other app": the real media remote, started by the worker because "Other app"
    # was the last choice, controlling the host's silent fake player through Windows. For a few
    # seconds the fake player is Windows' current media session (see media-remote-tests.md).
    $otherId = New-FakeId
    Wait-NoOtherFakePlayers '41. end to end: Other app controlling the silent test player'
    $panelRun = Invoke-PanelRun '41. end to end: Other app controlling the silent test player' 'other-real' '' @{ RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_TEST_REMOTE = 'real'; RACING_MASTER_MOD_TEST_FAKE_PLAYER = '1'; RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID = $otherId; RACING_MASTER_MOD_TEST_MEDIA_APP = $otherId } @('[panel]', 'source=other_app')
    $received = ''
    foreach ($line in $panelRun.Lines) { if ($line -match '^fake player received: (.*)$') { $received = $Matches[1] } }
    Record 'end to end other app: the worker started the control of other apps (last choice) and followed only the test player' ($panelRun.Log.Contains('other app: starting the control of other music apps') -and $panelRun.Log.Contains('other app: test host, so only the test player'))
    Record 'end to end other app: the fake player received play, next, loop one and pause, in that order' ($received -eq 'play, next, loop one, pause')
    Test-MediaSafety @{ Text = $panelRun.Log; Id = $otherId } 'end to end other app'

    # 42. A settings file under the per-user name, but next to the DLL (where the owner first put
    # it). Upper case on purpose. It holds stage=0, which would switch the mod off if it were read.
    $misplacedDir = Join-Path $work 'misplaced'
    New-Item -ItemType Directory -Force $misplacedDir | Out-Null
    Copy-Item $dll, $hostExe $misplacedDir
    Write-Ini (Join-Path $misplacedDir 'SETTINGS.INI') @('[mod]', 'stage=0') 'utf8'
    $misplacedRoot = Join-Path $work 'lad-misplaced'
    $run = Invoke-Run '42. settings.ini next to the DLL is ignored, with one plain log line' (Join-Path $misplacedDir 'testhost.exe') @('quick', $work, '1') $flag $misplacedRoot
    $log = Read-Log $run.Log
    $expected = 'settings: found ' + (Join-Path $misplacedDir 'settings.ini') + ' next to version.dll and ignored it; the mod reads ' + (Join-Path $misplacedDir 'racing-master-mod.ini') + ' or ' + (Join-Path $misplacedRoot 'RacingMasterMod\settings.ini') + ', so rename the file to racing-master-mod.ini or move it into that second folder'
    Record 'misplaced settings.ini: one line names it as ignored and gives the full paths of the two files the mod reads' (([regex]::Matches($log, [regex]::Escape($expected))).Count -eq 1)
    Record 'misplaced settings.ini: its stage=0 was not applied (the mod runs, the marker shows)' (($run.Lines -match '^marker: found').Count -eq 1 -and -not $log.Contains('stage 0: the mod is switched off'))
    Record 'no misplaced-file line when there is none (panel input run)' (-not (Read-Log (Join-Path $work 'lad-panel-panel-input\RacingMasterMod\mod.log')).Contains('next to version.dll and ignored it'))

    # 43-44. The panel switches itself off without a fault in the frame code (the marker and the
    # frame hook carry on): a fault in the mod's part of the window procedure, and a Dear ImGui
    # assertion. The panel closes and the worker pauses the song that was playing.
    $panelRun = Invoke-PanelRun '43. panel off after a fault in the window procedure' 'panel-off' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_TEST_PANEL_OFF = 'wndproc' } @()
    Record 'window procedure fault: the panel says it is closed for the session, the worker paused the music' ($panelRun.Log.Contains('the panel is closed for this session') -and $panelRun.Log.Contains('music: paused, because the music panel has switched itself off after an error'))
    Record 'window procedure fault: the frame code did not switch off (no fault line)' (-not $panelRun.Log.Contains('frame code failed'))
    $panelRun = Invoke-PanelRun '44. panel off after a Dear ImGui assertion' 'panel-off' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_TEST_PANEL_OFF = 'assert' } @() $songs $true
    Record 'ImGui assertion: the worker paused the music, once' (([regex]::Matches($panelRun.Log, 'music: paused, because')).Count -eq 1 -and $panelRun.Log.Contains('music: paused, because the music panel has switched itself off after an error'))

    # 45. Two threads presenting the same swap chain at once (NVIDIA Smooth Motion may do this in
    # the game): the mod draws only on the thread that presented first, whose pipeline state stays
    # intact; the other thread's presents pass straight on; the frames move to the other thread only
    # after the first was quiet for a second. No fault, no ImGui assertion.
    $panelRun = Invoke-PanelRun '45. presents from two threads at once' 'concurrent' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'en' } @()
    Record 'two threads at once: the log names the second thread and the move, the 30 s line is not needed, nothing failed' ($panelRun.Log.Contains('the game''s frames also arrive from a second thread') -and $panelRun.Log.Contains('the game''s frames now come from another thread') -and -not $panelRun.Log.Contains('frames were shown from two threads at the same moment') -and -not $panelRun.Log.Contains('frame code failed'))
    Record 'present probe off (the default): no probe line, nothing recorded, no Smooth Motion line' (@($panelRun.Lines -eq 'probe calls recorded: 0').Count -eq 1 -and -not $panelRun.Log.Contains('present probe') -and -not $panelRun.Log.Contains('Smooth Motion is on'))

    # 46. The same with [debug] present_probe=1, a block every 2 s instead of 30, and the Smooth
    # Motion check pointed at a DLL the host does load (the real NvPresent64.dll is never here).
    $panelRun = Invoke-PanelRun '46. present probe on, presents from two threads' 'concurrent' '' @{ RACING_MASTER_MOD_PANEL = 'open'; RACING_MASTER_MOD_LANG = 'en'; RACING_MASTER_MOD_TEST_PROBE_SECONDS = '2'; RACING_MASTER_MOD_TEST_SMOOTH_MOTION_DLL = 'd3d11.dll' } @('[debug]', 'present_probe=1')
    $probe = $panelRun.Log
    $recorded = 0
    foreach ($line in $panelRun.Lines) { if ($line -match '^probe calls recorded: (\d+)$') { $recorded = [long]$Matches[1] } }
    Record 'present probe on: the log says so, and presents were recorded' ($probe.Contains('present probe: on ([debug] present_probe=1)') -and $recorded -gt 0)
    Record 'present probe: two threads, both inside the hook at once at some point' ($probe -match 'at most [2-9] threads inside the hook at once; threads: \d+ x\d+ (\([^)]*\) )?\d+ x\d+.*\(2 in all\)')
    $mainThread = ''; $secondThread = ''
    foreach ($line in $panelRun.Lines) { if ($line -match '^thread ids: this (\d+), second (\d+)$') { $mainThread = $Matches[1]; $secondThread = $Matches[2] } }
    Record 'present probe: each thread''s line says what the frame work did with its presents (drawn on the first, passed on for the second)' ($mainThread -and $probe -match ('threads:.* ' + $mainThread + ' x\d+ \([^)]*panel drawn \d+') -and $probe -match ('threads:.* ' + $secondThread + ' x\d+ \([^)]*game frame from a thread the mod does not draw on \d+'))
    Record 'present probe: presents called from code in no module (the second thread''s) get their own line with outcomes' ($probe -match 'present probe: called from code in no module: Present \d+, Present1 \d+; .*game frame from a thread the mod does not draw on \d+')
    Record 'present probe: the caller is the host program, with the frame work''s outcomes' ($probe -match 'present probe: called from testhost\.exe: Present \d+, Present1 \d+; .*panel drawn \d+.*game frame from a thread the mod does not draw on \d+')
    Record 'present probe: the original Present is in dxgi.dll and the patched table entry points into the mod' ($probe -match '(?i)the Present the mod passes frames on to is in dxgi\.dll .*the patched table''s Present entry now points into version\.dll')
    Record 'present probe: the swap chain uses the patched table, whose Present entry is the mod''s' ($probe -match '(?i)present probe: swap chain [0-9a-f]+, \d+ presents: its function table is in dxgi\.dll \(the table the mod patched\), and its Present entry points into version\.dll')
    Record 'present probe: Windows'' own present count of the game''s swap chain is reported and moves' ($probe -match 'Windows counted [1-9]\d* presents of the game''s swap chain')
    Record 'present probe: flags and sync intervals are listed' ($probe -match 'present probe: flags: none \d+.*; sync interval: 0 x\d+')
    Record 'Smooth Motion check: one plain line when its DLL is loaded (here a stand-in name)' (([regex]::Matches($probe, 'NVIDIA Smooth Motion is on for this game')).Count -eq 1)

    Write-Host ''
    Write-Host 'Summary'
    foreach ($r in $results) { Write-Host ("  " + $(if ($r.Ok) { 'PASS' } else { 'FAIL' }) + "  $($r.Name)") }
    foreach ($s in $skipped) { Write-Host "  SKIPPED  $s" }
    foreach ($n in $notes) { Write-Host "  NOTE  $n" }
    $failed = @($results | Where-Object { -not $_.Ok }).Count
    Write-Host ''
    Write-Host ("$($results.Count - $failed) of $($results.Count) checks passed" + $(if ($failed) { "; $failed FAILED" } else { '' }) + $(if ($skipped.Count) { "; $($skipped.Count) skipped (listed above with the reason)" } else { '' }))
} finally {
    Remove-Item Env:\RACING_MASTER_MOD_TEST_HOST -ErrorAction SilentlyContinue
    Remove-Item Env:\RACING_MASTER_MOD_TEST_FAULT -ErrorAction SilentlyContinue
    foreach ($name in 'APP', 'SCRIPT', 'PICK') {
        Remove-Item "Env:\RACING_MASTER_MOD_TEST_MEDIA_$name" -ErrorAction SilentlyContinue
    }
    Remove-Item Env:\RACING_MASTER_MOD_TEST_FAKE_PLAYER_ID -ErrorAction SilentlyContinue
    $env:LOCALAPPDATA = $realLocalAppData
    # The panel screenshots outlive the temporary folder (out\ is not committed).
    $shots = Join-Path $work 'screens'
    if (Test-Path $shots) {
        $kept = Join-Path $root 'out\screens'
        New-Item -ItemType Directory -Force $kept | Out-Null
        Copy-Item (Join-Path $shots '*.png') $kept -Force
        Write-Host "Panel screenshots: $kept"
        $integrated = @(Get-ChildItem $shots -Filter 'integrated-*.png')
        if ($integrated.Count) {
            $preview = Join-Path $root 'out\preview\integrated'
            New-Item -ItemType Directory -Force $preview | Out-Null
            $integrated | Copy-Item -Destination $preview -Force
            Write-Host "End-to-end screenshots: $preview"
        }
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) { exit 1 } else { exit 0 }

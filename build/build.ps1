# Builds out\version.dll (the mod), out\testhost.exe, out\bystander.exe and out\musictest.exe
# (developer test programs).
#
# Needs: Visual Studio with the C++ compiler (any edition, 2022 or newer). The Windows SDK and the
# music libraries are fetched by fetch-deps.ps1, so they do not have to be installed.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build\build.ps1
#
# After linking, three gates run and fail the build if they do not hold:
#   1. version.dll imports KERNEL32.dll only. Everything else is delay-loaded, so a bystander
#      program that merely imports version.dll gets no extra DLL loaded. Only the DLLs in
#      $delayLoads may be delay-loaded, and each of them must actually appear: an empty or
#      unreadable dump, or a build that lost the music engine or the panel, fails instead of
#      passing. The media remote (src\remote) resolves combase, shell32 and advapi32 with
#      GetProcAddress at run time for the same reason; calling one of their functions directly
#      would fail this gate.
#   2. No "%ls" under src\: log lines are UTF-8 and wide strings go through LogUtf8().
#   3. No start-up constructor (dynamic initialiser) in the mod's own code: anything like that
#      would run inside every program that loads version.dll, not only inside the game.
#
# The music panel (src\ui) uses Dear ImGui (.deps\imgui-1.93.0, fetched by fetch-deps.ps1) and is
# compiled against the real music engine (src\music\player.h, picker.h) and the real media remote.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

# --- locate the compiler -------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio was not found. Install Visual Studio with "Desktop development with C++".'
}
$vsPath = & $vswhere -latest -products * -property installationPath
$msvcRoot = Join-Path $vsPath 'VC\Tools\MSVC'
if (-not (Test-Path $msvcRoot)) {
    throw "The C++ compiler is missing from $vsPath. Add the `"Desktop development with C++`" workload."
}
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'

# A partial Visual Studio install can have the compiler without its headers and libraries.
# In that case they are fetched into .deps\msvc instead.
$crt = $msvc.FullName
$needCrt = -not (Test-Path (Join-Path $crt 'include\vcruntime.h'))
if ($needCrt) {
    & (Join-Path $PSScriptRoot 'fetch-deps.ps1') -MsvcCrt
    $crt = Join-Path $root '.deps\msvc'
} else {
    & (Join-Path $PSScriptRoot 'fetch-deps.ps1')
}

# --- locate the SDK fetched into .deps ---------------------------------------------------------
$sdkInclude = Get-ChildItem (Join-Path $root '.deps\winsdk\c\Include') -Directory | Select-Object -First 1
$sdkBin = Join-Path $root ".deps\winsdk\c\bin\$($sdkInclude.Name)\x64"
$sdkLib = Join-Path $root '.deps\winsdk-x64\c'

$env:INCLUDE = @(
    (Join-Path $crt 'include'),
    (Join-Path $sdkInclude.FullName 'ucrt'),
    (Join-Path $sdkInclude.FullName 'um'),
    (Join-Path $sdkInclude.FullName 'shared'),
    # Raw WinRT ABI headers (windows.media.control.h) for src\remote. Not cppwinrt: C++/WinRT
    # needs C++ exceptions, and the mod is built without them.
    (Join-Path $sdkInclude.FullName 'winrt')
) -join ';'
$env:LIB = @(
    (Join-Path $crt 'lib\x64'),
    (Join-Path $sdkLib 'ucrt\x64'),
    (Join-Path $sdkLib 'um\x64')
) -join ';'

$out = Join-Path $root 'out'
$obj = Join-Path $root 'out\obj'
New-Item -ItemType Directory -Force $obj | Out-Null

function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$([IO.Path]::GetFileName($Exe)) failed with exit code $LASTEXITCODE"
    }
}

$cl = Join-Path $bin 'cl.exe'
$ml64 = Join-Path $bin 'ml64.exe'
$link = Join-Path $bin 'link.exe'
$lib = Join-Path $bin 'lib.exe'
$rc = Join-Path $sdkBin 'rc.exe'

# --- music libraries in .deps (fetched and hash-checked by fetch-deps.ps1) ------------------------
$deps = Join-Path $root '.deps'
$miniaudioDir = Join-Path $deps 'miniaudio-0.11.26'
$stbDir = Join-Path $deps 'stb-9f755b6'
$oggDir = Join-Path $deps 'libogg-1.3.6'
$opusDir = Join-Path $deps 'opus-1.6.1'
$opusfileDir = Join-Path $deps 'opusfile-0.12'
$vorbisDir = Join-Path $deps 'libvorbis-1.3.7'   # test program only
$musicIncludes = @("/I$miniaudioDir", "/I$stbDir", "/I$(Join-Path $oggDir 'include')", "/I$(Join-Path $opusDir 'include')",
                   "/I$(Join-Path $opusfileDir 'include')")

# /MT links the C runtime statically, so players need no extra runtime installer.
# /Zi with /Fd writes debug information into out\obj\vc.pdb (without /Fd the compiler would drop
# a vc140.pdb wherever the script was started from).
# /D_HAS_EXCEPTIONS=0: the mod is built without C++ exceptions; without this, ordinary STL calls
# such as vector::insert fail the build under /EHs-c- /WX. Running out of memory then ends the
# process instead of throwing, which with exceptions off was never recoverable anyway.
# /DNDEBUG: no assert() may stay live inside the game (a failed one shows a message box and ends
# the process). /utf-8: source files are UTF-8 (Chinese text in comments and test strings).
$cflags = @('/nologo', '/c', '/std:c++20', '/O2', '/W4', '/WX', '/EHs-c-', '/GR-', '/MT', '/GS', '/guard:cf',
            '/Zi', "/Fd$(Join-Path $obj 'vc.pdb')",
            '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
            '/D_HAS_EXCEPTIONS=0', '/DNDEBUG', '/utf-8')

# --- gate 2: no %ls in the mod's sources ------------------------------------------------------------
$wideFormat = Get-ChildItem (Join-Path $root 'src') -Recurse -File | Select-String -Pattern '%ls' -SimpleMatch
if ($wideFormat) {
    throw "src\ must not use %ls (log lines are UTF-8; pass wide strings through LogUtf8 and %s): $($wideFormat | ForEach-Object { "$($_.Path):$($_.LineNumber)" })"
}

# --- third-party C code ------------------------------------------------------------------------------
# Compiled at /W3, never /W4 (miniaudio's implementation fails /W4 with C4456 and C4244, measured in
# the design brief), always with /DNDEBUG: miniaudio maps its asserts to the C assert() and
# stb_vorbis calls it directly; /O2 and /MT do not switch assert() off, only NDEBUG does.
$tpFlags = @('/nologo', '/c', '/TC', '/O2', '/MT', '/GS', '/guard:cf', '/Z7', '/DNDEBUG', '/DWIN32_LEAN_AND_MEAN',
             '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS')
$tpObj = Join-Path $obj 'third_party'
New-Item -ItemType Directory -Force $tpObj | Out-Null

# miniaudio and stb_vorbis: one translation unit each, /W3 /WX (both are clean there).
$miniaudioObject = Join-Path $tpObj 'miniaudio_impl.obj'
Invoke-Tool $cl ($tpFlags + @('/W3', '/WX', "/I$miniaudioDir", "/I$(Join-Path $root 'src\music')", "/Fo$miniaudioObject",
                              (Join-Path $root 'src\third_party\miniaudio_impl.c')))
$stbObject = Join-Path $tpObj 'stb_vorbis_impl.obj'
Invoke-Tool $cl ($tpFlags + @('/W3', '/WX', "/I$stbDir", "/Fo$stbObject", (Join-Path $root 'src\third_party\stb_vorbis_impl.c')))

# libogg, opus and opusfile: about 120 files, built into one static library, out\obj\xiph.lib.
# The file lists come from opus's own *_sources.mk files, so they follow the pinned release.
# Plain C only: no SIMD files, no run-time CPU detection, none of the optional neural-network
# features (deep packet-loss concealment, DRED, OSCE), no HTTP streaming in opusfile. The library
# is rebuilt only when it is missing or this recipe changed (the stamp file holds the recipe).
function Get-MkList([string]$File, [string]$Name) {
    $items = @()
    $inList = $false
    foreach ($line in Get-Content $File) {
        if (-not $inList) {
            if ($line -match "^$Name\s*=") { $inList = $true; $line = $line -replace "^$Name\s*=", '' } else { continue }
        }
        $continued = $line.TrimEnd().EndsWith('\')
        foreach ($word in ($line -replace '\\\s*$', '').Trim() -split '\s+') { if ($word) { $items += $word } }
        if (-not $continued) { break }
    }
    if (-not $items.Count) { throw "$Name was not found in $File; the opus release layout changed" }
    return $items
}
$xiphGroups = @(
    @{ Name = 'ogg'; Dir = $oggDir; Files = @('src\framing.c', 'src\bitwise.c'); Includes = @('include'); Defines = @() },
    @{ Name = 'celt'; Dir = $opusDir; Files = (Get-MkList (Join-Path $opusDir 'celt_sources.mk') 'CELT_SOURCES') },
    @{ Name = 'silk'; Dir = $opusDir; Files = (Get-MkList (Join-Path $opusDir 'silk_sources.mk') 'SILK_SOURCES') },
    @{ Name = 'silk_float'; Dir = $opusDir; Files = (Get-MkList (Join-Path $opusDir 'silk_sources.mk') 'SILK_SOURCES_FLOAT') },
    @{ Name = 'opus'; Dir = $opusDir; Files = ((Get-MkList (Join-Path $opusDir 'opus_sources.mk') 'OPUS_SOURCES') +
                                               (Get-MkList (Join-Path $opusDir 'opus_sources.mk') 'OPUS_SOURCES_FLOAT')) },
    @{ Name = 'opusfile'; Dir = $opusfileDir; Files = @('src\opusfile.c', 'src\info.c', 'src\internal.c', 'src\stream.c');
       Includes = @('include', (Join-Path $opusDir 'include'), (Join-Path $oggDir 'include')); Defines = @('/DOP_DISABLE_HTTP') }
)
$opusIncludes = @('include', 'celt', 'silk', 'silk\float', 'src', 'dnn')
$opusDefines = @('/DOPUS_BUILD', '/DUSE_ALLOCA', '/DHAVE_LRINT', '/DHAVE_LRINTF')
# /W3 without /WX here: these are upstream releases compiled as their authors ship them; a new
# compiler warning in their code must not stop the mod's build. Other warnings are still printed.
# C4244 (double to float, 64 to 32 bit, deliberate in their DSP code; dozens per build) and C4005
# (opusfile redefines WIN32_LEAN_AND_MEAN) are switched off so real news stays readable.
$xiphFlags = $tpFlags + @('/W3', '/MP', '/wd4244', '/wd4005')
$xiphLib = Join-Path $obj 'xiph.lib'
$xiphStamp = Join-Path $obj 'xiph.stamp'
$recipe = ($xiphFlags + $opusDefines + ($xiphGroups | ForEach-Object { $_.Name + ':' + ($_.Files -join ',') })) -join ' '
if (-not (Test-Path $xiphLib) -or -not (Test-Path $xiphStamp) -or (Get-Content $xiphStamp -Raw).Trim() -ne $recipe) {
    $xiphObjects = @()
    foreach ($group in $xiphGroups) {
        $groupObj = Join-Path $tpObj $group.Name
        if (Test-Path $groupObj) { Remove-Item $groupObj -Recurse -Force }
        New-Item -ItemType Directory -Force $groupObj | Out-Null
        $includes = if ($group.Includes) { $group.Includes } else { $opusIncludes }
        $defines = if ($null -ne $group.Defines) { $group.Defines } else { $opusDefines }
        $includeFlags = $includes | ForEach-Object { if ([IO.Path]::IsPathRooted($_)) { "/I$_" } else { "/I$(Join-Path $group.Dir $_)" } }
        $files = $group.Files | ForEach-Object { Join-Path $group.Dir ($_ -replace '/', '\') }
        # The doubled backslash survives PowerShell's quoting of a path with spaces ("...\" would
        # otherwise end in \", which the C runtime reads as an escaped quote).
        Invoke-Tool $cl ($xiphFlags + $includeFlags + $defines + @("/Fo$groupObj\\") + $files)
        $xiphObjects += (Get-ChildItem $groupObj -Filter '*.obj' | ForEach-Object { $_.FullName })
    }
    Invoke-Tool $lib (@('/nologo', "/OUT:$xiphLib") + $xiphObjects)
    Set-Content -Path $xiphStamp -Value $recipe -Encoding utf8
} else {
    Write-Host 'xiph.lib (libogg, opus, opusfile) is up to date'
}

# libvorbis with its encoder, for musictest.exe only: it makes the OGG Vorbis fixture, because
# Windows has no Vorbis encoder. version.dll decodes Vorbis with stb_vorbis and never links this.
$vorbisLib = Join-Path $obj 'vorbisenc.lib'
$vorbisStamp = Join-Path $obj 'vorbisenc.stamp'
$vorbisFiles = @('analysis', 'bitrate', 'block', 'codebook', 'envelope', 'floor0', 'floor1', 'info', 'lookup', 'lpc', 'lsp',
                 'mapping0', 'mdct', 'psy', 'registry', 'res0', 'sharedbook', 'smallft', 'synthesis', 'vorbisenc', 'window')
$vorbisRecipe = ($xiphFlags + $vorbisFiles) -join ' '
if (-not (Test-Path $vorbisLib) -or -not (Test-Path $vorbisStamp) -or (Get-Content $vorbisStamp -Raw).Trim() -ne $vorbisRecipe) {
    $vorbisObj = Join-Path $tpObj 'vorbis'
    if (Test-Path $vorbisObj) { Remove-Item $vorbisObj -Recurse -Force }
    New-Item -ItemType Directory -Force $vorbisObj | Out-Null
    Invoke-Tool $cl ($xiphFlags + @('/wd4267', '/wd4305', "/I$(Join-Path $vorbisDir 'include')", "/I$(Join-Path $vorbisDir 'lib')",
                                    "/I$(Join-Path $oggDir 'include')", "/Fo$vorbisObj\\") +
                     ($vorbisFiles | ForEach-Object { Join-Path $vorbisDir "lib\$_.c" }))
    Invoke-Tool $lib (@('/nologo', "/OUT:$vorbisLib") + (Get-ChildItem $vorbisObj -Filter '*.obj' | ForEach-Object { $_.FullName }))
    Set-Content -Path $vorbisStamp -Value $vorbisRecipe -Encoding utf8
} else {
    Write-Host 'vorbisenc.lib (test fixtures only) is up to date'
}

# --- the mod's own code ---------------------------------------------------------------------------
function Build-Objects([string[]]$Sources, [string[]]$Extra) {
    $built = @()
    foreach ($source in $Sources) {
        $object = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($source) + '.obj')
        Invoke-Tool $cl ($cflags + $Extra + @("/Fo$object", (Join-Path $root $source)))
        $built += $object
    }
    return $built
}

$loaderSources = @(
    'src\loader\dllmain.cpp',
    'src\loader\proxy.cpp',
    'src\core\log.cpp',
    'src\core\settings.cpp',
    'src\core\vtable_patch.cpp',
    'src\core\dxgi_probe.cpp',
    'src\core\present_hook.cpp',
    'src\core\game_close.cpp',
    'src\core\game_music.cpp',
    'src\core\present_probe.cpp',
    'src\core\worker.cpp',
    'src\remote\remote.cpp',
    'src\remote\app_names.cpp'
)
# The music engine (src\music). The worker starts it and the panel calls it.
$musicSources = @(
    'src\music\player.cpp',
    'src\music\audio_output.cpp',
    'src\music\source.cpp',
    'src\music\source_miniaudio.cpp',
    'src\music\source_vorbis.cpp',
    'src\music\source_opus.cpp',
    'src\music\source_mf.cpp',
    'src\music\tag_reader.cpp',
    'src\music\playlist_store.cpp',
    'src\music\atomic_file.cpp',
    'src\music\text.cpp',
    'src\music\picker.cpp'
)
$loaderObjects = Build-Objects $loaderSources @()
$musicObjects = @(Build-Objects $musicSources $musicIncludes)

# The music panel and Dear ImGui. Same flags as the rest of the mod (so the standard library
# types the panel shares with the engine and the remote are compiled the same way on both sides),
# plus Dear ImGui's folders. IMGUI_USER_CONFIG selects src\ui\imconfig_mod.h for every file that
# includes imgui.h (the angle-bracket form needs no quoting through PowerShell). Dear ImGui is
# third-party code, but it compiles cleanly at /W4 /WX and is kept there.
$imgui = Join-Path $deps 'imgui-1.93.0'
if (-not (Test-Path (Join-Path $imgui 'imgui.h'))) {
    throw "Dear ImGui is missing from $imgui. Run build\fetch-deps.ps1."
}
$uiIncludes = @('/DIMGUI_USER_CONFIG=<imconfig_mod.h>', "/I$(Join-Path $root 'src\ui')", "/I$imgui",
                "/I$(Join-Path $imgui 'backends')")
$imguiObjects = @(Build-Objects @('.deps\imgui-1.93.0\imgui.cpp', '.deps\imgui-1.93.0\imgui_draw.cpp',
                                  '.deps\imgui-1.93.0\imgui_tables.cpp', '.deps\imgui-1.93.0\imgui_widgets.cpp',
                                  '.deps\imgui-1.93.0\backends\imgui_impl_dx11.cpp') $uiIncludes)
$uiSources = @(
    'src\ui\imgui_errors.cpp',
    'src\ui\fonts.cpp',
    'src\ui\scale.cpp',
    'src\ui\hotkeys.cpp',
    'src\ui\input_wndproc.cpp',
    'src\ui\global_hotkeys.cpp',
    'src\ui\ui_settings.cpp',
    'src\ui\other_app.cpp',
    'src\ui\panel.cpp',
    'src\ui\overlay.cpp'
)
$uiObjects = @(Build-Objects $uiSources $uiIncludes)

$asmObject = Join-Path $obj 'proxy_exports.obj'
Invoke-Tool $ml64 @('/nologo', '/c', "/Fo$asmObject", (Join-Path $root 'src\loader\proxy_exports.asm'))
$resObject = Join-Path $obj 'version.res'
Invoke-Tool $rc @('/nologo', "/fo$resObject", (Join-Path $root 'src\loader\version.rc'))

# /DEBUG writes out\version.pdb so a fault offset from the log can be turned into a source line.
# /PDBALTPATH keeps the build folder (and the user name in it) out of the published DLL.
# /OPT:REF /OPT:ICF stay explicit because /DEBUG would otherwise switch them off.
# Every system DLL beyond kernel32 is delay-loaded: user32 (the frame hook's window checks and the
# panel's window procedure), d3dcompiler_47 (the panel's two shaders; the panel loads it by full
# path from the worker) and, for the music engine, ole32 (COM), shell32 (file dialog), mfplat and
# mfreadwrite (M4A/AAC through Media Foundation). They load only when first needed inside the
# game; a bystander program that imports version.dll gets none of them. Media Foundation is absent
# on Windows "N" editions: as a normal import it would stop version.dll, and so the game, from
# loading there. Never ship the PDB.
# The DLL names in the spelling the import dump prints them. Gate 1 below insists on exactly this
# list, so a new delay-loaded DLL is a conscious change here.
$delayLoads = @('USER32.dll', 'ole32.dll', 'SHELL32.dll', 'MFPlat.DLL', 'MFReadWrite.dll', 'd3dcompiler_47.dll')
$musicLibs = @('ole32.lib', 'shell32.lib', 'mfplat.lib', 'mfreadwrite.lib', 'mfuuid.lib', 'propsys.lib')
$dllMusic = $musicLibs + $musicObjects + @($miniaudioObject, $stbObject, $xiphLib)
$dllPanel = @('d3dcompiler.lib') + $uiObjects + $imguiObjects
$dll = Join-Path $out 'version.dll'
Invoke-Tool $link (@('/nologo', '/DLL', "/OUT:$dll",
    "/DEF:$(Join-Path $root 'src\loader\version.def')", "/IMPLIB:$(Join-Path $obj 'version_proxy.lib')",
    '/DEBUG', "/PDB:$(Join-Path $out 'version.pdb')", '/PDBALTPATH:version.pdb', "/MAP:$(Join-Path $out 'version.map')",
    '/OPT:REF', '/OPT:ICF', '/guard:cf', '/DYNAMICBASE', '/NXCOMPAT') +
    ($delayLoads | ForEach-Object { "/DELAYLOAD:$_" }) + @('kernel32.lib', 'user32.lib', 'delayimp.lib') +
    $loaderObjects + $dllMusic + $dllPanel + @($asmObject, $resObject))

# --- gate 1: static imports are KERNEL32.dll only; delay-loads are the allowed list -------------------
# link.exe /DUMP is dumpbin; the fetched SDK has no dumpbin.exe of its own.
# The gate must see a usable dump before it can pass: an empty import list (wrong link.exe path,
# changed output format) would otherwise pass for "no extra imports".
$eager = @()
$delayed = @()
$inDelay = $false
$dump = @(& $link /DUMP /NOLOGO /IMPORTS $dll)
$dumpExit = $LASTEXITCODE
foreach ($line in $dump) {
    if ($line -match 'delay load imports') { $inDelay = $true }
    if ($line -match '^\s{4}(\S+\.dll)\s*$') {
        if ($inDelay) { $delayed += $Matches[1] } else { $eager += $Matches[1] }
    }
}
if ($dumpExit -ne 0 -or $eager -notcontains 'KERNEL32.dll') {
    throw "The import dump of version.dll produced no usable import list (exit code $dumpExit, $($dump.Count) lines); the KERNEL32-only gate could not be checked."
}
$extra = @($eager | Where-Object { $_ -ne 'KERNEL32.dll' })
if ($extra.Count) {
    throw "version.dll must import KERNEL32.dll only; also found: $($extra -join ', '). Delay-load it (/DELAYLOAD) or call it through LoadLibraryW from the worker thread."
}
$unexpected = @($delayed | Where-Object { $delayLoads -notcontains $_ })
if ($unexpected.Count) {
    throw "version.dll delay-loads DLLs that are not on the list in build.ps1: $($unexpected -join ', '). Add them to `$delayLoads only after checking they exist on every Windows edition the mod supports."
}
$missing = @($delayLoads | Where-Object { $delayed -notcontains $_ })
if ($missing.Count) {
    throw "version.dll should delay-load $($missing -join ', '), but the import dump lists these delay-loaded DLLs: '$($delayed -join ', ')'. Check the /DELAYLOAD lines and that the music engine and the panel are still linked in."
}
Write-Host "version.dll static imports: $($eager -join ', '); delay-loaded: $($delayed -join ', ')"

# --- gate 3: no start-up constructors in the mod's own code -------------------------------------------
# The compiler names a dynamic initialiser "??__E<name>"; the map file lists the object it came from.
# The panel's own files are checked too; Dear ImGui's are not (third-party code).
$ownObjects = @($loaderObjects + $musicObjects + $uiObjects | ForEach-Object { [IO.Path]::GetFileName($_) })
$initialisers = @(Get-Content (Join-Path $out 'version.map') | Where-Object { $_ -match '\?\?__E' } |
    Where-Object { $line = $_; $ownObjects | Where-Object { $line -match [regex]::Escape($_) } })
if ($initialisers.Count) {
    throw "version.dll has start-up constructors in the mod's own code (they would run in every program that loads it); use function-local statics or constant initialisation instead: $($initialisers -join '; ')"
}

# --- developer test programs ----------------------------------------------------------------------------
$hostFlags = @('/nologo', '/c', '/std:c++20', '/O2', '/W4', '/WX', '/EHsc', '/MT', '/DUNICODE', '/D_UNICODE',
    '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS')
$hostObject = Join-Path $obj 'testhost.obj'
Invoke-Tool $cl ($hostFlags + @("/Fo$hostObject", (Join-Path $root 'tools\testhost\testhost.cpp')))
# The silent fake media player for the remote-control tests (runtimeobject.lib and shell32.lib
# come in through #pragma comment in fake_player.cpp). The panel's screenshots need
# windowscodecs and ole32, its pipeline-state check d3dcompiler.
$fakeObject = Join-Path $obj 'fake_player.obj'
Invoke-Tool $cl ($hostFlags + @("/Fo$fakeObject", (Join-Path $root 'tools\testhost\fake_player.cpp')))
Invoke-Tool $link @('/nologo', "/OUT:$(Join-Path $out 'testhost.exe')", '/SUBSYSTEM:CONSOLE', $hostObject, $fakeObject,
    'd3dcompiler.lib', 'windowscodecs.lib', 'ole32.lib')

$bystanderObject = Join-Path $obj 'bystander.obj'
Invoke-Tool $cl ($hostFlags + @("/Fo$bystanderObject", (Join-Path $root 'tools\testhost\bystander.cpp')))
Invoke-Tool $link @('/nologo', "/OUT:$(Join-Path $out 'bystander.exe')", '/SUBSYSTEM:CONSOLE', $bystanderObject)

# musictest.exe links the very same engine objects that go into version.dll and runs them without
# a sound device (see tools\testhost\musictest.cpp). Same flags as the mod, so the STL is built the
# same way on both sides. Media Foundation is imported normally here: a test program on this PC.
# One file per area of the engine; each registers its own cases (see musictest.h). It also links
# the media remote's app names (src\remote\app_names.cpp: plain functions, no Windows media calls).
$musicTestSources = @(
    'tools\testhost\musictest.cpp',
    'tools\testhost\musictest_fixtures.cpp',
    'tools\testhost\musictest_text.cpp',
    'tools\testhost\musictest_logic.cpp',
    'tools\testhost\musictest_miniaudio.cpp',
    'tools\testhost\musictest_vorbis.cpp',
    'tools\testhost\musictest_mf.cpp',
    'tools\testhost\musictest_opus.cpp',
    'tools\testhost\musictest_store.cpp',
    'tools\testhost\musictest_engine.cpp',
    'tools\testhost\musictest_picker.cpp',
    'tools\testhost\musictest_appnames.cpp'
)
$musicTestObjects = Build-Objects $musicTestSources ($musicIncludes + @("/I$(Join-Path $root 'src')", "/I$(Join-Path $vorbisDir 'include')"))
$engineObjects = $musicObjects
Invoke-Tool $link (@('/nologo', "/OUT:$(Join-Path $out 'musictest.exe')", '/SUBSYSTEM:CONSOLE', '/DEBUG',
    "/PDB:$(Join-Path $obj 'musictest.pdb')", '/OPT:REF', 'kernel32.lib', 'user32.lib') + $musicLibs +
    $musicTestObjects + @((Join-Path $obj 'log.obj'), (Join-Path $obj 'app_names.obj')) + $engineObjects + @($miniaudioObject, $stbObject, $xiphLib, $vorbisLib))

Write-Host ''
Write-Host "Built: $dll"
Write-Host "Built: $(Join-Path $out 'testhost.exe')"
Write-Host "Built: $(Join-Path $out 'bystander.exe')"
Write-Host "Built: $(Join-Path $out 'musictest.exe')"

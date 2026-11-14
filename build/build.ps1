# Builds out\version.dll (the mod), out\testhost.exe and out\bystander.exe (developer test programs).
#
# Needs: Visual Studio with the C++ compiler (any edition, 2022 or newer). The Windows SDK is
# fetched by fetch-deps.ps1, so it does not have to be installed.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build\build.ps1
#
# After linking, two gates run and fail the build if they do not hold:
#   1. version.dll imports KERNEL32.dll only. Everything else (user32 today) is delay-loaded, so
#      a bystander program that merely imports version.dll gets no extra DLL loaded. The gate also
#      insists that the import dump actually listed KERNEL32 and the delay-loaded USER32, so an
#      empty or unreadable dump fails the build instead of passing it. The media remote
#      (src\remote) resolves combase, shell32 and advapi32 with GetProcAddress at run time for
#      the same reason; calling one of their functions directly would fail this gate.
#   2. No "%ls" under src\: log lines are UTF-8 and wide strings go through LogUtf8().

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
$rc = Join-Path $sdkBin 'rc.exe'

# /MT links the C runtime statically, so players need no extra runtime installer.
# /Zi with /Fd writes debug information into out\obj\vc.pdb (without /Fd the compiler would drop
# a vc140.pdb wherever the script was started from).
$cflags = @('/nologo', '/c', '/std:c++20', '/O2', '/W4', '/WX', '/EHs-c-', '/GR-', '/MT', '/GS', '/guard:cf',
            '/Zi', "/Fd$(Join-Path $obj 'vc.pdb')",
            '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS')

# --- gate 2: no %ls in the mod's sources ------------------------------------------------------------
$wideFormat = Get-ChildItem (Join-Path $root 'src') -Recurse -File | Select-String -Pattern '%ls' -SimpleMatch
if ($wideFormat) {
    throw "src\ must not use %ls (log lines are UTF-8; pass wide strings through LogUtf8 and %s): $($wideFormat | ForEach-Object { "$($_.Path):$($_.LineNumber)" })"
}

# --- version.dll ----------------------------------------------------------------------------------
$sources = @(
    'src\loader\dllmain.cpp',
    'src\loader\proxy.cpp',
    'src\core\log.cpp',
    'src\core\settings.cpp',
    'src\core\vtable_patch.cpp',
    'src\core\dxgi_probe.cpp',
    'src\core\present_hook.cpp',
    'src\core\worker.cpp',
    'src\remote\remote.cpp'
)
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($source) + '.obj')
    Invoke-Tool $cl ($cflags + @("/Fo$object", (Join-Path $root $source)))
    $objects += $object
}
$asmObject = Join-Path $obj 'proxy_exports.obj'
Invoke-Tool $ml64 @('/nologo', '/c', "/Fo$asmObject", (Join-Path $root 'src\loader\proxy_exports.asm'))
$resObject = Join-Path $obj 'version.res'
Invoke-Tool $rc @('/nologo', "/fo$resObject", (Join-Path $root 'src\loader\version.rc'))

# /DEBUG writes out\version.pdb so a fault offset from the log can be turned into a source line.
# /PDBALTPATH keeps the build folder (and the user name in it) out of the published DLL.
# /OPT:REF /OPT:ICF stay explicit because /DEBUG would otherwise switch them off.
# user32 is delay-loaded: it is only called from the mod's own thread inside the game, and a
# bystander program that imports version.dll must not get it loaded. Never ship the PDB.
$dll = Join-Path $out 'version.dll'
Invoke-Tool $link (@('/nologo', '/DLL', "/OUT:$dll",
    "/DEF:$(Join-Path $root 'src\loader\version.def')", "/IMPLIB:$(Join-Path $obj 'version_proxy.lib')",
    '/DEBUG', "/PDB:$(Join-Path $out 'version.pdb')", '/PDBALTPATH:version.pdb', "/MAP:$(Join-Path $out 'version.map')",
    '/OPT:REF', '/OPT:ICF', '/guard:cf', '/DYNAMICBASE', '/NXCOMPAT',
    '/DELAYLOAD:user32.dll',
    'kernel32.lib', 'user32.lib', 'delayimp.lib') + $objects + @($asmObject, $resObject))

# --- gate 1: static imports are KERNEL32.dll only -----------------------------------------------------
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
if ($delayed -notcontains 'USER32.dll') {
    throw "version.dll should delay-load USER32.dll, but the import dump lists these delay-loaded DLLs: '$($delayed -join ', ')'. Check the /DELAYLOAD line."
}
Write-Host "version.dll static imports: $($eager -join ', '); delay-loaded: $($delayed -join ', ')"

# --- developer test programs ----------------------------------------------------------------------------
$hostFlags = @('/nologo', '/c', '/std:c++20', '/O2', '/W4', '/WX', '/EHsc', '/MT', '/DUNICODE', '/D_UNICODE',
    '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS')
$hostObject = Join-Path $obj 'testhost.obj'
Invoke-Tool $cl ($hostFlags + @("/Fo$hostObject", (Join-Path $root 'tools\testhost\testhost.cpp')))
# The silent fake media player for the remote-control tests (runtimeobject.lib and shell32.lib
# come in through #pragma comment in fake_player.cpp).
$fakeObject = Join-Path $obj 'fake_player.obj'
Invoke-Tool $cl ($hostFlags + @("/Fo$fakeObject", (Join-Path $root 'tools\testhost\fake_player.cpp')))
Invoke-Tool $link @('/nologo', "/OUT:$(Join-Path $out 'testhost.exe')", '/SUBSYSTEM:CONSOLE', $hostObject, $fakeObject)

$bystanderObject = Join-Path $obj 'bystander.obj'
Invoke-Tool $cl ($hostFlags + @("/Fo$bystanderObject", (Join-Path $root 'tools\testhost\bystander.cpp')))
Invoke-Tool $link @('/nologo', "/OUT:$(Join-Path $out 'bystander.exe')", '/SUBSYSTEM:CONSOLE', $bystanderObject)

Write-Host ''
Write-Host "Built: $dll"
Write-Host "Built: $(Join-Path $out 'testhost.exe')"
Write-Host "Built: $(Join-Path $out 'bystander.exe')"

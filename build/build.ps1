# Builds out\version.dll (the mod) and out\testhost.exe (developer test host).
#
# Needs: Visual Studio with the C++ compiler (any edition, 2022 or newer). The Windows SDK is
# fetched by fetch-deps.ps1, so it does not have to be installed.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build\build.ps1

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
$msvc = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
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
    (Join-Path $sdkInclude.FullName 'shared')
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
$cflags = @('/nologo', '/c', '/std:c++20', '/O2', '/W4', '/WX', '/EHs-c-', '/GR-', '/MT', '/GS', '/guard:cf',
            '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS')

# --- version.dll ----------------------------------------------------------------------------------
$sources = @(
    'src\loader\dllmain.cpp',
    'src\loader\proxy.cpp',
    'src\core\log.cpp',
    'src\core\present_hook.cpp'
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

Invoke-Tool $link (@('/nologo', '/DLL', "/OUT:$(Join-Path $out 'version.dll')",
    "/DEF:$(Join-Path $root 'src\loader\version.def')", "/IMPLIB:$(Join-Path $obj 'version_proxy.lib')",
    '/OPT:REF', '/OPT:ICF', '/guard:cf', '/DYNAMICBASE', '/NXCOMPAT',
    'kernel32.lib', 'user32.lib', 'dxguid.lib') + $objects + @($asmObject, $resObject))

# --- developer test host ----------------------------------------------------------------------------
$hostObject = Join-Path $obj 'testhost.obj'
Invoke-Tool $cl (@('/nologo', '/c', '/std:c++20', '/O2', '/W4', '/EHsc', '/MT', '/DUNICODE', '/D_UNICODE',
    '/DNOMINMAX', "/Fo$hostObject", (Join-Path $root 'tools\testhost\testhost.cpp')))
Invoke-Tool $link @('/nologo', "/OUT:$(Join-Path $out 'testhost.exe')", '/SUBSYSTEM:CONSOLE', $hostObject)

Write-Host ''
Write-Host "Built: $(Join-Path $out 'version.dll')"
Write-Host "Built: $(Join-Path $out 'testhost.exe')"

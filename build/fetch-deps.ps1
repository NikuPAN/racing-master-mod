# Downloads the build dependencies into .deps/ (gitignored). Safe to re-run: it only fetches what is missing.
#
# The Windows SDK headers and libraries come from Microsoft's official NuGet packages, so no
# system-wide Windows SDK install (and no administrator prompt) is needed.
#
# -MsvcCrt also fetches the Visual C++ runtime headers and libraries from Microsoft's Visual
# Studio package feed. build.ps1 asks for this only when the installed Visual Studio has the
# compiler but not its headers (a partial install).

param([switch]$MsvcCrt)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root '.deps'
New-Item -ItemType Directory -Force $deps | Out-Null

$sdkVersion = '10.0.28000.2957'
$packages = @(
    @{ Id = 'microsoft.windows.sdk.cpp';     Dir = 'winsdk' },
    @{ Id = 'microsoft.windows.sdk.cpp.x64'; Dir = 'winsdk-x64' }
)

foreach ($p in $packages) {
    $target = Join-Path $deps $p.Dir
    if (Test-Path (Join-Path $target '.complete')) {
        Write-Host "$($p.Id) $sdkVersion already present"
        continue
    }
    $url = "https://api.nuget.org/v3-flatcontainer/$($p.Id)/$sdkVersion/$($p.Id).$sdkVersion.nupkg"
    $zip = Join-Path $deps "$($p.Dir).zip"
    Write-Host "Downloading $($p.Id) $sdkVersion ..."
    Invoke-WebRequest -Uri $url -OutFile $zip
    if (Test-Path $target) { Remove-Item $target -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $target
    Remove-Item $zip
    Set-Content -Path (Join-Path $target '.complete') -Value $sdkVersion -Encoding utf8
}

if ($MsvcCrt) {
    # Side-by-side 14.50 toolset: older than any Visual Studio 2026 linker, which is the
    # direction Microsoft supports (libraries may be older than the linker, not newer).
    $crtPackages = @('Microsoft.VC.14.50.18.0.CRT.Headers.base', 'Microsoft.VC.14.50.18.0.CRT.x64.Desktop.base',
                     'Microsoft.VC.14.50.18.0.CRT.x64.Store.base')
    $target = Join-Path $deps 'msvc'
    if (Test-Path (Join-Path $target '.complete')) {
        Write-Host 'Visual C++ runtime headers and libraries already present'
    } else {
        Write-Host 'Reading the Visual Studio package list ...'
        Add-Type -AssemblyName System.Web.Extensions
        $json = New-Object System.Web.Script.Serialization.JavaScriptSerializer
        $json.MaxJsonLength = [int]::MaxValue
        $web = New-Object System.Net.WebClient
        $web.Encoding = [Text.Encoding]::UTF8

        $channel = $json.DeserializeObject($web.DownloadString('https://aka.ms/vs/18/stable/channel').TrimStart([char]0xFEFF))
        $manifestItem = $channel['channelItems'] | Where-Object { $_['type'] -eq 'Manifest' } | Select-Object -First 1
        $manifest = $json.DeserializeObject($web.DownloadString($manifestItem['payloads'][0]['url']).TrimStart([char]0xFEFF))

        if (Test-Path $target) { Remove-Item $target -Recurse -Force }
        New-Item -ItemType Directory -Force $target | Out-Null
        foreach ($id in $crtPackages) {
            $package = $manifest['packages'] | Where-Object { $_['id'] -eq $id } | Select-Object -First 1
            if (-not $package) { throw "Package $id is no longer in the Visual Studio feed. Install the 'Desktop development with C++' workload instead." }
            $payload = $package['payloads'][0]
            $vsix = Join-Path $deps "$id.zip"
            Write-Host "Downloading $id ..."
            Invoke-WebRequest -Uri $payload['url'] -OutFile $vsix
            $hash = (Get-FileHash $vsix -Algorithm SHA256).Hash
            if ($hash -ne $payload['sha256']) { throw "Checksum mismatch for $id" }
            $unpacked = Join-Path $deps "$id.unpacked"
            if (Test-Path $unpacked) { Remove-Item $unpacked -Recurse -Force }
            Expand-Archive -Path $vsix -DestinationPath $unpacked
            $toolset = Get-ChildItem (Join-Path $unpacked 'Contents\VC\Tools\MSVC') -Directory | Select-Object -First 1
            Copy-Item -Path (Join-Path $toolset.FullName '*') -Destination $target -Recurse -Force
            Remove-Item $unpacked -Recurse -Force
            Remove-Item $vsix
        }
        Set-Content -Path (Join-Path $target '.complete') -Value ($crtPackages -join ',') -Encoding utf8
    }
}

Write-Host 'Dependencies ready.'

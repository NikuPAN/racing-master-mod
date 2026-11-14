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
# SHA-256 of each .nupkg as published on nuget.org (checked here on 2026-11-14). A new SDK
# version needs new hashes: download the package, run Get-FileHash, paste the result.
$packages = @(
    @{ Id = 'microsoft.windows.sdk.cpp';     Dir = 'winsdk';
       Sha256 = 'F0A200DAE58858F418B30DC9E2F52B4188F97EB7837BD4F73074652A01937DDD' },
    @{ Id = 'microsoft.windows.sdk.cpp.x64'; Dir = 'winsdk-x64';
       Sha256 = '7CA47E7345DCBDF36EC4C3C4CD59AB9CB5C9F620A2E785CC4FA641509647CC5A' }
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
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($hash -ne $p.Sha256) {
        Remove-Item $zip
        throw "Checksum mismatch for $($p.Id) $sdkVersion (got $hash). The download is not the package this script expects; nothing was unpacked."
    }
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

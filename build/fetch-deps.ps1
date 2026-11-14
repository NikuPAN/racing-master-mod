# Downloads the build dependencies into .deps/ (gitignored). Safe to re-run: it only fetches what is missing.
#
# The Windows SDK headers and libraries come from Microsoft's official NuGet packages, so no
# system-wide Windows SDK install (and no administrator prompt) is needed. The music engine's
# libraries (miniaudio, stb_vorbis, libogg, opus, opusfile) and the panel's Dear ImGui come from
# their publishers, each file pinned to one SHA-256. Licences: THIRD_PARTY.md.
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

# --- music engine libraries -------------------------------------------------------------------------
# Each is pinned to one version and one SHA-256, checked here on 2026-11-14. The miniaudio and
# stb_vorbis hashes are the ones in the engine design brief; the three Xiph tarballs match the
# SHA256SUMS files Xiph publishes next to them. A new version needs a new hash: download the file,
# run Get-FileHash, compare with the publisher's checksum list, paste the result.
#   miniaudio 0.11.26     WAV, FLAC, MP3 decoders and the WASAPI/null output device
#   stb_vorbis 1.24       OGG Vorbis (pull API)
#   libogg, opus, opusfile  Opus in Ogg (.opus); opus also carries the encoder the tests use
#   libvorbis 1.3.7       test program only: the Vorbis encoder that makes the OGG test fixture
#                         (Windows has none). Never linked into version.dll.
$music = @(
    @{ Name = 'miniaudio 0.11.26'; Dir = 'miniaudio-0.11.26'; File = 'miniaudio.h'
       Url = 'https://raw.githubusercontent.com/mackron/miniaudio/877ea4a7c2601c7b0f2a44e83dfac42e6470fea6/miniaudio.h'
       Sha256 = '6B376C7FBB1352BA1E0E432AB66069B335EA9405E45CB6C30B97EF0F3C9C3E0E' },
    @{ Name = 'stb_vorbis 1.24'; Dir = 'stb-9f755b6'; File = 'stb_vorbis.c'
       Url = 'https://raw.githubusercontent.com/nothings/stb/9f755b604db3492df2cbeb39bf84b81e3e3932e3/stb_vorbis.c'
       Sha256 = 'B52CF518355F7B2D181F18B97B812F361FFEB26299D935C8CF29E842401D60A6' },
    @{ Name = 'libogg 1.3.6'; Dir = 'libogg-1.3.6'; Tarball = $true
       Url = 'https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.gz'
       Sha256 = '83E6704730683D004D20E21B8F7F55DCB3383CDF84C0DAEDF30BDE175F774638' },
    @{ Name = 'opus 1.6.1'; Dir = 'opus-1.6.1'; Tarball = $true
       Url = 'https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz'
       Sha256 = '6FFCB593207BE92584DF15B32466ED64BBEC99109F007C82205F0194572411A1' },
    @{ Name = 'opusfile 0.12'; Dir = 'opusfile-0.12'; Tarball = $true
       Url = 'https://downloads.xiph.org/releases/opus/opusfile-0.12.tar.gz'
       Sha256 = '118D8601C12DD6A44F52423E68CA9083CC9F2BFE72DA7A8C1ACB22A80AE3550B' },
    @{ Name = 'libvorbis 1.3.7'; Dir = 'libvorbis-1.3.7'; Tarball = $true
       Url = 'https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.gz'
       Sha256 = '0E982409A9C3FC82EE06E08205B1355E5C6AA4C36BCA58146EF399621B0CE5AB' }
)
foreach ($m in $music) {
    $target = Join-Path $deps $m.Dir
    if (Test-Path (Join-Path $target '.complete')) {
        Write-Host "$($m.Name) already present"
        continue
    }
    $download = Join-Path $deps "$($m.Dir).download"
    Write-Host "Downloading $($m.Name) ..."
    Invoke-WebRequest -Uri $m.Url -OutFile $download
    $hash = (Get-FileHash $download -Algorithm SHA256).Hash
    if ($hash -ne $m.Sha256) {
        Remove-Item $download
        throw "Checksum mismatch for $($m.Name) (got $hash). The download is not the file this script expects; nothing was unpacked."
    }
    # Unpack into a fresh folder first, so a half-finished run never leaves a folder marked complete.
    $staging = Join-Path $deps "$($m.Dir).staging"
    if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
    New-Item -ItemType Directory -Force $staging | Out-Null
    if ($m.Tarball) {
        # Windows 10 and 11 ship tar.exe in System32; the archive's top folder is the version folder.
        & (Join-Path $env:SystemRoot 'System32\tar.exe') -xzf $download -C $staging
        if ($LASTEXITCODE -ne 0) { throw "Unpacking $($m.Name) failed (tar exit code $LASTEXITCODE)" }
        $unpacked = Join-Path $staging $m.Dir
        if (-not (Test-Path $unpacked)) { throw "$($m.Name): the archive did not contain the folder $($m.Dir)" }
        if (Test-Path $target) { Remove-Item $target -Recurse -Force }
        Move-Item $unpacked $target
        Remove-Item $staging -Recurse -Force
    } else {
        Move-Item $download (Join-Path $staging $m.File)
        if (Test-Path $target) { Remove-Item $target -Recurse -Force }
        Move-Item $staging $target
    }
    if (Test-Path $download) { Remove-Item $download }
    Set-Content -Path (Join-Path $target '.complete') -Value $m.Sha256 -Encoding utf8
}

# --- Dear ImGui (the in-game panel's drawing library) ---------------------------------------------
# Thirteen files of the v1.93.0 release, fetched one by one from the tagged source tree so that each
# can be checked against a SHA-256 pinned here (an archive download has no stable bytes to pin).
# The library is MIT licensed; releases of the mod must ship its LICENSE.txt. Nothing of it is
# committed to this repository.
$imguiVersion = '1.93.0'
$imguiTarget = Join-Path $deps "imgui-$imguiVersion"
$imguiFiles = @(
    @{ Path = 'imgui.cpp';                    Sha256 = '9542082a1eaeffcab7b3d8069e707839ce1714d167005e9f273ac9f656b415ed' },
    @{ Path = 'imgui.h';                      Sha256 = '314462301948fbd5673f5ead6f9a53acf54767cbee57e186e69f8f8b022c7094' },
    @{ Path = 'imgui_draw.cpp';               Sha256 = 'fff3bb04ca11e32041144e83a37b89ec92f4d5c62d681eb7ef29ee6cd7663e84' },
    @{ Path = 'imgui_internal.h';             Sha256 = '61bc50172d907cc931159cdce48fbff1db616cda33251c9c3469e316056939e6' },
    @{ Path = 'imgui_tables.cpp';             Sha256 = '36635c1cf2ad1b7c88cdced456cceba3a1d02266d78a63d77e9cce92f4d48d61' },
    @{ Path = 'imgui_widgets.cpp';            Sha256 = 'd2d388f711688f6a5ce1ae3b30168c1bf4f954ead40235b0b2b9667452850c93' },
    @{ Path = 'imstb_rectpack.h';             Sha256 = '889b396795202d1457560a797a7242e96f6f132d4b88ca2d69be58bf05e1771f' },
    @{ Path = 'imstb_textedit.h';             Sha256 = 'a985f5fa0ed97353d493b497961e9eef52082edcd045cf6954b69990ec9d0741' },
    @{ Path = 'imstb_truetype.h';             Sha256 = 'c51a0f7e7ea760f2366bd3752635ec58e21fccfec4a832501639990ba6ce0528' },
    @{ Path = 'imconfig.h';                   Sha256 = 'fa91f606da783669ffd485f3f9e0cd0d92cddeac459efce4c2b9327cce7a3233' },
    @{ Path = 'LICENSE.txt';                  Sha256 = '173506a2d6f7fb67990d257fb2507f188690eca39060c39469ae7bef43aae2a3' },
    @{ Path = 'backends/imgui_impl_dx11.cpp'; Sha256 = 'ee561259e75ca08c4fcb109db05ba2ef98979175ec1bb7688078a9dfabf3e977' },
    @{ Path = 'backends/imgui_impl_dx11.h';   Sha256 = '393acaa0fd95acb8c2431a17d6e952fc07ba29344074855aea5679ef3dd5d604' }
)
if (Test-Path (Join-Path $imguiTarget '.complete')) {
    Write-Host "Dear ImGui $imguiVersion already present"
} else {
    $staging = "$imguiTarget.partial"
    if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
    New-Item -ItemType Directory -Force (Join-Path $staging 'backends') | Out-Null
    foreach ($f in $imguiFiles) {
        $url = "https://raw.githubusercontent.com/ocornut/imgui/v$imguiVersion/$($f.Path)"
        $file = Join-Path $staging ($f.Path -replace '/', '\')
        Write-Host "Downloading Dear ImGui $imguiVersion $($f.Path) ..."
        Invoke-WebRequest -Uri $url -OutFile $file
        $hash = (Get-FileHash $file -Algorithm SHA256).Hash
        if ($hash -ne $f.Sha256.ToUpperInvariant()) {
            Remove-Item $staging -Recurse -Force
            throw "Checksum mismatch for Dear ImGui $imguiVersion $($f.Path) (got $hash). The download is not the file this script expects; nothing was kept."
        }
    }
    if (Test-Path $imguiTarget) { Remove-Item $imguiTarget -Recurse -Force }
    Rename-Item $staging $imguiTarget
    Set-Content -Path (Join-Path $imguiTarget '.complete') -Value $imguiVersion -Encoding utf8
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

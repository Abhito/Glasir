# SPDX-License-Identifier: MIT
# Downloads the latest mpv Windows build (zhongfly/mpv-winbuild, x86_64, GPL)
# into the app's engine folder. Glasir does not ship mpv; the installer and
# this developer script fetch it from the official releases on this PC.
#
#   tools\fetch-mpv.ps1                         -> out\Glasir\engine
#   tools\fetch-mpv.ps1 -Dest C:\Games\Glasir\engine
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\out\Glasir\engine')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is very slow with it

$api = 'https://api.github.com/repos/zhongfly/mpv-winbuild/releases/latest'
$release = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'Glasir' }
# Plain x86_64 build: not x86_64-v3 (needs AVX2), not the libmpv dev package.
$asset = $release.assets | Where-Object { $_.name -match '^mpv-x86_64-\d{8}-git-[0-9a-f]+\.7z$' } | Select-Object -First 1
if (-not $asset) { throw "No mpv-x86_64 build in release $($release.tag_name)" }

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("glasir-mpv-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $archive = Join-Path $work $asset.name
    Write-Host "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB)) MB)"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $archive -Headers @{ 'User-Agent' = 'Glasir' }

    if ($asset.digest -match '^sha256:([0-9a-f]{64})$') {
        $hash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
        if ($hash -ne $Matches[1]) { throw "SHA-256 mismatch for $($asset.name): got $hash" }
    } else {
        Write-Warning "The release lists no SHA-256 for $($asset.name); not verified."
    }

    # Windows 11's tar (libarchive) reads 7z.
    $unpacked = Join-Path $work 'unpacked'
    New-Item -ItemType Directory -Path $unpacked | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xf $archive -C $unpacked
    if ($LASTEXITCODE -ne 0) { throw "Could not unpack $($asset.name) (tar exit $LASTEXITCODE)" }
    $exe = Get-ChildItem -Path $unpacked -Recurse -Filter 'mpv.exe' | Select-Object -First 1
    if (-not $exe) { throw "mpv.exe not found in $($asset.name)" }

    New-Item -ItemType Directory -Path $Dest -Force | Out-Null
    Copy-Item $exe.FullName (Join-Path $Dest 'mpv.exe') -Force
    $version = $asset.name -replace '^mpv-x86_64-(.+)\.7z$', '$1'
    Set-Content -Path (Join-Path $Dest 'mpv-version.txt') -Value "$version`r`n$($asset.browser_download_url)" -Encoding ascii
    Write-Host "mpv $version -> $(Resolve-Path $Dest)"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

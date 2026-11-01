# SPDX-License-Identifier: MIT
# Downloads the DLSS 5 components into the app's engine folder, each from
# where its author or NVIDIA publishes it. Glasir does not ship them; the
# installer and this developer script fetch them on this PC.
#
#   DLSS5-Feeder        github.com/jlrouzies-fr/DLSS5-Feeder releases (MIT)
#   RenoDX-DLSS5        github.com/RankFTW/rhi-repo (RHI's mirror; MIT source)
#   nvngx_dlssnr.dll    github.com/RankFTW/rhi-repo (RHI's mirror; NVIDIA)
#   nvngx_dlss.dll      github.com/NVIDIA/DLSS (NVIDIA RTX SDKs licence)
#   LumeniteFX          github.com/umar-afzaal/LumeniteFX (AGNYA licence)
#   ReShade.fxh         github.com/crosire/reshade-shaders (CC0)
#
#   tools\fetch-dlss5.ps1                      -> out\Glasir\engine
#   tools\fetch-dlss5.ps1 -Dest C:\Games\Glasir\engine
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\out\Glasir\engine')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$headers = @{ 'User-Agent' = 'Glasir' }
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("glasir-dlss5-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
New-Item -ItemType Directory -Path $Dest -Force | Out-Null
$shaders = Join-Path $Dest 'reshade-shaders\Shaders'
$textures = Join-Path $Dest 'reshade-shaders\Textures'
$versions = @()

function Get-Json($url) { Invoke-RestMethod -Uri $url -Headers $headers }

function Get-Asset($asset, $file) {
    Write-Host "Downloading $($asset.name) ($([math]::Round($asset.size / 1MB)) MB)"
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $file -Headers $headers -UseBasicParsing
    if ($asset.digest -match '^sha256:([0-9a-f]{64})$') {
        $hash = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLowerInvariant()
        if ($hash -ne $Matches[1]) { throw "SHA-256 mismatch for $($asset.name)" }
    } else {
        Write-Warning "$($asset.name) has no published SHA-256; not verified."
    }
}

function Expand-Into($zip, $folder) {
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    & "$env:SystemRoot\System32\tar.exe" -xf $zip -C $folder
    if ($LASTEXITCODE -ne 0) { throw "Could not unpack $zip" }
}

# The newest release on rhi-repo whose tag is PREFIX-<x.y.z> (no rc or forks).
function Get-RhiRelease($prefix) {
    $releases = Get-Json 'https://api.github.com/repos/RankFTW/rhi-repo/releases?per_page=100'
    $release = $releases | Where-Object { $_.tag_name -match "^$prefix-\d+\.\d+\.\d+$" } |
        Sort-Object { [version]($_.tag_name -replace "^$prefix-", '') } -Descending | Select-Object -First 1
    if (-not $release) { throw "No $prefix release on RankFTW/rhi-repo" }
    $release
}

try {
    # DLSS5-Feeder: the add-on and its ReShade shader.
    $feeder = Get-Json 'https://api.github.com/repos/jlrouzies-fr/DLSS5-Feeder/releases/latest'
    $asset = $feeder.assets | Where-Object { $_.name -match '^DLSS5-Feeder-.*\.zip$' } | Select-Object -First 1
    $zip = Join-Path $work $asset.name
    Get-Asset $asset $zip
    Expand-Into $zip (Join-Path $work 'feeder')
    Copy-Item (Join-Path $work 'feeder\dlss5-feed.addon64') $Dest -Force
    New-Item -ItemType Directory -Path (Join-Path $shaders 'DLSS5Feeder') -Force | Out-Null
    Copy-Item (Join-Path $work 'feeder\reshade-shaders\Shaders\DLSS5_Feed.fx') (Join-Path $shaders 'DLSS5Feeder') -Force
    $versions += "DLSS5-Feeder $($feeder.tag_name)"

    # RenoDX-DLSS5 (the neural rendering add-on) and the DLSS 5 runtime.
    $renodx = Get-RhiRelease 'renodx-dlss5'
    $zip = Join-Path $work $renodx.assets[0].name
    Get-Asset $renodx.assets[0] $zip
    Expand-Into $zip (Join-Path $work 'renodx')
    Copy-Item (Join-Path $work 'renodx\renodx-dlss5.addon64') $Dest -Force
    $versions += "RenoDX-DLSS5 $($renodx.tag_name)"

    $nr = Get-RhiRelease 'dlssnr'
    $zip = Join-Path $work $nr.assets[0].name
    Get-Asset $nr.assets[0] $zip
    Expand-Into $zip (Join-Path $work 'dlssnr')
    $dll = Get-ChildItem -Path (Join-Path $work 'dlssnr') -Recurse -Filter 'nvngx_dlssnr.dll' | Select-Object -First 1
    Copy-Item $dll.FullName $Dest -Force
    $versions += "nvngx_dlssnr $($nr.tag_name)"

    # DLSS (super resolution) from NVIDIA's SDK, as shipped there.
    $dlss = Get-Json 'https://api.github.com/repos/NVIDIA/DLSS/releases/latest'
    $url = "https://github.com/NVIDIA/DLSS/raw/$($dlss.tag_name)/lib/Windows_x86_64/rel/nvngx_dlss.dll"
    Write-Host "Downloading nvngx_dlss.dll $($dlss.tag_name) from NVIDIA/DLSS"
    $file = Join-Path $work 'nvngx_dlss.dll'
    Invoke-WebRequest -Uri $url -OutFile $file -Headers $headers -UseBasicParsing
    $signature = Get-AuthenticodeSignature $file
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'NVIDIA') {
        throw "nvngx_dlss.dll is not signed by NVIDIA ($($signature.Status))"
    }
    Copy-Item $file $Dest -Force
    $versions += "nvngx_dlss $($dlss.tag_name)"

    # LumeniteFX: motion vectors for DLSS5_Feed.fx (DLSS5_MV_PROVIDER=3).
    $zip = Join-Path $work 'lumenite.zip'
    Write-Host 'Downloading LumeniteFX (mainline) from umar-afzaal/LumeniteFX'
    Invoke-WebRequest -Uri 'https://github.com/umar-afzaal/LumeniteFX/archive/refs/heads/mainline.zip' -OutFile $zip `
        -Headers $headers -UseBasicParsing
    Expand-Into $zip (Join-Path $work 'lumenite')
    $root = Get-ChildItem -Path (Join-Path $work 'lumenite') -Directory | Select-Object -First 1
    New-Item -ItemType Directory -Path (Join-Path $shaders 'LumeniteFX') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $textures 'LumeniteFX') -Force | Out-Null
    Copy-Item (Join-Path $root.FullName 'Shaders\*') (Join-Path $shaders 'LumeniteFX') -Recurse -Force
    Copy-Item (Join-Path $root.FullName 'Textures\*') (Join-Path $textures 'LumeniteFX') -Recurse -Force
    Copy-Item (Join-Path $root.FullName 'LICENSE.md') (Join-Path $shaders 'LumeniteFX\LICENSE.md') -Force
    $versions += 'LumeniteFX mainline ' + (Get-Date -Format 'yyyy-MM-dd')

    # ReShade.fxh, which DLSS5_Feed.fx includes.
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/ReShade.fxh' `
        -OutFile (Join-Path $shaders 'ReShade.fxh') -Headers $headers -UseBasicParsing
    $versions += 'ReShade.fxh crosire/reshade-shaders slim'

    Set-Content -Path (Join-Path $Dest 'dlss5-versions.txt') -Value $versions -Encoding ascii
    $versions | ForEach-Object { Write-Host "  $_" }
    Write-Host "DLSS 5 components -> $(Resolve-Path $Dest)"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

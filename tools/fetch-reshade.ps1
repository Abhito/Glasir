# SPDX-License-Identifier: MIT
# Downloads ReShade with full add-on support from reshade.me (crosire) and puts
# its 64-bit DLL into the app's engine folder as dxgi.dll, where mpv loads it.
# Glasir does not ship ReShade: reshade.me asks people not to share the
# binaries, so the installer and this developer script fetch it from there on
# this PC, the way RHI does.
#
#   tools\fetch-reshade.ps1                      -> out\Glasir\engine
#   tools\fetch-reshade.ps1 -Dest C:\Games\Glasir\engine
param(
    [string]$Dest = (Join-Path $PSScriptRoot '..\out\Glasir\engine')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$site = 'https://reshade.me'
$headers = @{ 'User-Agent' = 'Glasir'; 'Referer' = "$site/" }

$page = (Invoke-WebRequest -Uri $site -Headers $headers -UseBasicParsing).Content
$m = [regex]::Match($page, '/downloads/ReShade_Setup_([\d.]+)_Addon\.exe', 'IgnoreCase')
if (-not $m.Success) { throw "No 'ReShade with full add-on support' download link on $site" }
$version = $m.Groups[1].Value

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("glasir-reshade-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null
try {
    $setup = Join-Path $work "ReShade_Setup_${version}_Addon.exe"
    Write-Host "Downloading ReShade $version with add-on support from $site"
    Invoke-WebRequest -Uri ($site + $m.Value) -OutFile $setup -Headers $headers -UseBasicParsing
    $bytes = [System.IO.File]::ReadAllBytes($setup)
    if ($bytes.Length -lt 1MB -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "The download is not the ReShade installer (got $($bytes.Length) bytes)"
    }

    # The installer is an exe with a zip archive appended; the DLLs are in it.
    # Windows' tar (libarchive) reads that; .NET's zip reader does not.
    & "$env:SystemRoot\System32\tar.exe" -xf $setup -C $work ReShade64.dll
    $dll = Join-Path $work 'ReShade64.dll'
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $dll)) { throw 'Could not take ReShade64.dll out of the installer' }
    New-Item -ItemType Directory -Path $Dest -Force | Out-Null
    Copy-Item $dll (Join-Path $Dest 'dxgi.dll') -Force
    $info = (Get-Item (Join-Path $Dest 'dxgi.dll')).VersionInfo
    if ($info.ProductName -notmatch 'ReShade') { throw "dxgi.dll is not ReShade ($($info.ProductName))" }
    Set-Content -Path (Join-Path $Dest 'reshade-version.txt') -Value "$version`r`n$site$($m.Value)" -Encoding ascii
    Write-Host "ReShade $version (add-on support) -> $(Resolve-Path $Dest)\dxgi.dll"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

<#
.SYNOPSIS
Compiles and packages the redistributable Release 0.1.0 ZIP for Resident Evil: Degeneration (N-Gage 2.0 / EKA2L1)
without proprietary firmware, ROMs, original game packages, saves, logs, or ReShade DLLs.
#>
[CmdletBinding()]
param(
    [string]$Eka2l1SourceDir = 'C:\Users\Jeronimo\Documents\Ngage games\EKA2L1',
    [string]$BezelPath       = 'c:\Users\Jeronimo\Documents\Ngage games\resident-emulation\app\assets\bezel.png',
    [string]$DocsSourceDir   = 'c:\Users\Jeronimo\Documents\Ngage games\resident-emulation',
    [string]$ForgeRoot       = 'C:\workspace\LegacyMobileForge',
    [string]$OutputDir       = 'c:\Users\Jeronimo\Documents\Ngage games\resident-emulation\release',
    [string]$Version         = '0.1.0'
)
$ErrorActionPreference = 'Stop'

$integrationDir = $PSScriptRoot
if (!(Test-Path -LiteralPath (Join-Path $Eka2l1SourceDir 'eka2l1_qt.exe'))) {
    throw "EKA2L1 runtime not found at $Eka2l1SourceDir"
}
if (!(Test-Path -LiteralPath $BezelPath)) {
    throw "Bezel asset not found at $BezelPath"
}

$staging = Join-Path $OutputDir 'dist\ResidentEvilLauncher'
$zipPath = Join-Path $OutputDir "ResidentEvilDegeneration-$Version-windows-x64.zip"

if (Test-Path -LiteralPath $staging) {
    Remove-Item -LiteralPath $staging -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}

$appDir     = Join-Path $staging 'app'
$assetsDir  = Join-Path $appDir 'assets'
$runtimeDir = Join-Path $staging 'runtime\eka2l1'
$null = New-Item -ItemType Directory -Path $assetsDir, $runtimeDir -Force

# 1. Build fresh native C17 launcher directly into staging root and update forge-integration copy
$exeDest = Join-Path $staging 'ResidentEvilLauncher.exe'
& (Join-Path $integrationDir 'Build-LauncherNG.ps1') -OutputPath $exeDest | Out-Null
Copy-Item -LiteralPath $exeDest -Destination (Join-Path $integrationDir 'launcher\ResidentEvilLauncher.exe') -Force

# 2. Copy case-specific presentation & portable INI configuration (all relative paths)
Copy-Item -LiteralPath $BezelPath -Destination (Join-Path $assetsDir 'bezel.png') -Force

@"
[launcher-ng]
title=Resident Evil: Degeneration (N-Gage 2.0)
version=$Version
eka2l1_exe=runtime\eka2l1\eka2l1_qt.exe
device=RM-409
game_uid=0x2001AD82
keybind_profile=resident_evil
artwork=assets\bezel.png
bezel_scale_ratio=67
control_port=28182
language=auto
"@ | Set-Content -LiteralPath (Join-Path $appDir 'launcher-ng.ini') -Encoding unicode

# 3. Copy clean, reusable EKA2L1 runtime (explicit whitelist: NO data/, firmware/, updater/, saves, logs, or ReShade)
$rootFiles = @('eka2l1_qt.exe', 'panic.json', 'qt.conf')
foreach ($rf in $rootFiles) {
    $src = Join-Path $Eka2l1SourceDir $rf
    if (Test-Path -LiteralPath $src) {
        Copy-Item -LiteralPath $src -Destination (Join-Path $runtimeDir $rf) -Force
    }
}
Get-ChildItem -LiteralPath $Eka2l1SourceDir -Filter '*.dll' -File | Where-Object {
    $_.Name -ine 'd3d9.dll' -and $_.Name -ine 'dxgi.dll' -and $_.Name -ine 'opengl32.dll'
} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $runtimeDir $_.Name) -Force
}

foreach ($sub in @('qt', 'translations', 'patch', 'scripts', 'resources')) {
    $srcSub = Join-Path $Eka2l1SourceDir $sub
    if (Test-Path -LiteralPath $srcSub) {
        Copy-Item -LiteralPath $srcSub -Destination (Join-Path $runtimeDir $sub) -Recurse -Force
    }
}

$scale2xSrc = Join-Path $integrationDir 'presentation\Scale2x.frag'
if (Test-Path -LiteralPath $scale2xSrc) {
    $upscaleDir = Join-Path $runtimeDir 'resources\upscale'
    $null = New-Item -ItemType Directory -Path $upscaleDir -Force
    Copy-Item -LiteralPath $scale2xSrc -Destination (Join-Path $upscaleDir 'Scale2x.frag') -Force
}

$bindingsDir = Join-Path $runtimeDir 'bindings'
$null = New-Item -ItemType Directory -Path $bindingsDir -Force
Copy-Item -LiteralPath (Join-Path $Eka2l1SourceDir 'bindings\default.yml') -Destination (Join-Path $bindingsDir 'default.yml') -Force
Copy-Item -LiteralPath (Join-Path $integrationDir 'controls\resident_evil.yml') -Destination (Join-Path $bindingsDir 'resident_evil.yml') -Force
Copy-Item -LiteralPath (Join-Path $integrationDir 'controls\resident_evil_rstick.yml') -Destination (Join-Path $bindingsDir 'resident_evil_rstick.yml') -Force

$compatDir = Join-Path $runtimeDir 'compat'
$null = New-Item -ItemType Directory -Path $compatDir -Force
$panicBlackList = Join-Path $Eka2l1SourceDir 'compat\panicBlackList.json'
if (Test-Path -LiteralPath $panicBlackList) {
    Copy-Item -LiteralPath $panicBlackList -Destination (Join-Path $compatDir 'panicBlackList.json') -Force
}

# 4. Convenience batch files with relative paths (no machine-specific directories)
"@echo off`r`nstart `"`" `"%~dp0ResidentEvilLauncher.exe`"`r`n" | Set-Content -LiteralPath (Join-Path $staging 'jogar_resident_evil.bat') -Encoding ascii
"@echo off`r`nstart `"`" `"%~dp0ResidentEvilLauncher.exe`" --direct`r`n" | Set-Content -LiteralPath (Join-Path $staging 'jogar_direto.bat') -Encoding ascii

# 5. Legal notices, license files, documentation, and corresponding source info
foreach ($docName in @('README.md', 'CHANGELOG.md')) {
    $srcDoc = Join-Path $DocsSourceDir $docName
    if (Test-Path -LiteralPath $srcDoc) {
        Copy-Item -LiteralPath $srcDoc -Destination (Join-Path $staging $docName) -Force
    }
}
if (Test-Path -LiteralPath (Join-Path $ForgeRoot 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $ForgeRoot 'LICENSE') -Destination (Join-Path $staging 'LICENSE') -Force
}
if (Test-Path -LiteralPath (Join-Path $ForgeRoot 'THIRD-PARTY.md')) {
    Copy-Item -LiteralPath (Join-Path $ForgeRoot 'THIRD-PARTY.md') -Destination (Join-Path $staging 'THIRD-PARTY.md') -Force
}

$ekaHash = (Get-FileHash -LiteralPath (Join-Path $runtimeDir 'eka2l1_qt.exe') -Algorithm SHA256).Hash
$sdlHash = (Get-FileHash -LiteralPath (Join-Path $runtimeDir 'SDL2.dll') -Algorithm SHA256).Hash
$exeHash = (Get-FileHash -LiteralPath $exeDest -Algorithm SHA256).Hash

@"
Legacy Mobile Forge — N-Gage 2.0 Distribution Notices (Release $Version)
========================================================================

1. Native C17 Launcher (ResidentEvilLauncher.exe)
   - SHA-256: $exeHash
   - License: GPL-3.0-or-later (see LICENSE)
   - Source: https://github.com/piecarlos/legacy-mobile-forge

2. EKA2L1 Symbian / N-Gage Emulator Runtime (runtime/eka2l1/eka2l1_qt.exe)
   - Upstream Version: master-90f46cf91 (x64 Windows)
   - Binary SHA-256: $ekaHash
   - Corresponding Source: https://github.com/EKA2L1/EKA2L1
   - License: GPL-2.0-or-later (bundled runtime libraries subject to their respective licenses below)

3. Bundled Runtime Libraries (runtime/eka2l1/)
   - SDL2.dll (SHA-256: $sdlHash) & gamecontrollerdb.txt: zlib License (https://github.com/libsdl-org/SDL)
   - Qt 6 (Qt6Core, Qt6Gui, Qt6Widgets, Qt6Network, Qt6Svg, Qt6Multimedia + qt/ plugins): LGPLv3 / GPLv3 (https://www.qt.io/)
   - FFmpeg (avcodec-61, avformat-61, avutil-59, swresample-5, swscale-8): LGPLv2.1+ / GPL (https://ffmpeg.org/)
   - OpenSSL (libcrypto-3-x64, libssl-3-x64): Apache-2.0 (https://www.openssl.org/)
   - DirectX Shader Compiler / D3Dcompiler_47: MIT / Microsoft Redistributable

4. Excluded Content Policy
   - This public package includes NO Symbian ROMs, NO RPKG firmware images, NO N-Gage client SIS,
     NO commercial game files (.n-gage/.sis/.exe), NO personal saves, and NO ReShade binaries.
"@ | Set-Content -LiteralPath (Join-Path $staging 'SOURCES.txt') -Encoding utf8

# 6. Hard safety check before zipping
$forbidden = Get-ChildItem -LiteralPath $staging -Recurse -File | Where-Object {
    $_.Extension -in @('.rom', '.rpkg', '.sis', '.sisx', '.n-gage', '.i3d', '.db', '.dat', '.log') -or
    $_.Name -in @('Resident_Evil.exe', 'NgiInstaller.exe', 'd3d9.dll', 'config.yml', 'devices.yml')
}
if ($forbidden) {
    throw "Release staging contains prohibited proprietary/private file(s): $($forbidden.FullName -join ', ')"
}

Compress-Archive -LiteralPath $staging -DestinationPath $zipPath -CompressionLevel Optimal
Write-Output "Release $Version package built: $zipPath"

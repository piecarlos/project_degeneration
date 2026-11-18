[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputPath
)
$ErrorActionPreference = 'Stop'

$programs = [Environment]::GetFolderPath('ProgramFilesX86')
if (!$programs) { $programs = 'C:\Program Files (x86)' }
$vswhere = Join-Path $programs 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (!$visualStudio) {
    throw 'Install Visual Studio C++ Build Tools and the Windows SDK.'
}

$vc = Get-ChildItem (Join-Path $visualStudio 'VC/Tools/MSVC') -Directory |
      Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$sdk = Join-Path $programs 'Windows Kits/10'
$sdkVersion = Get-ChildItem (Join-Path $sdk 'Include') -Directory |
              Where-Object Name -match '^10\.' |
              Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1

$compiler = Join-Path $vc.FullName 'bin/Hostx64/x64/cl.exe'
$launcherDir = Join-Path $PSScriptRoot 'launcher'
$source = Join-Path $launcherDir 'GameLauncherNG.c'
$output = [IO.Path]::GetFullPath($OutputPath)
$null = New-Item -ItemType Directory -Path (Split-Path $output) -Force

$argsList = @(
    '/nologo', '/TC', '/std:c17', '/utf-8', '/O2', '/MT', '/W4', '/WX',
    '/D_CRT_SECURE_NO_WARNINGS',
    ('/I' + $launcherDir),
    ('/I' + (Join-Path $vc.FullName 'include'))
)
foreach ($part in @('ucrt', 'shared', 'um', 'winrt')) {
    $argsList += ('/I' + (Join-Path $sdk ('Include/' + $sdkVersion.Name + '/' + $part)))
}
$argsList += @(
    $source,
    ('/Fo' + [IO.Path]::ChangeExtension($output, '.obj')),
    ('/Fe' + $output),
    '/link', '/SUBSYSTEM:WINDOWS', '/DYNAMICBASE', '/NXCOMPAT', '/MANIFEST:EMBED',
    ('/LIBPATH:' + (Join-Path $vc.FullName 'lib/x64'))
)
foreach ($part in @('ucrt', 'um')) {
    $argsList += ('/LIBPATH:' + (Join-Path $sdk ('Lib/' + $sdkVersion.Name + '/' + $part + '/x64')))
}
$argsList += @(
    'winhttp.lib', 'bcrypt.lib', 'ole32.lib', 'shell32.lib', 'shlwapi.lib', 'uuid.lib',
    'comctl32.lib', 'user32.lib', 'advapi32.lib', 'gdi32.lib',
    'windowscodecs.lib', 'dwmapi.lib'
)

$nativePath = $env:PATH
try {
    $env:PATH = (Split-Path $compiler) + ';' + (Join-Path $sdk ('bin/' + $sdkVersion.Name + '/x64')) + ';' + $nativePath
    & $compiler @argsList
    if ($LASTEXITCODE -ne 0) {
        throw "MSVC compilation failed with exit code $LASTEXITCODE"
    }
} finally {
    $env:PATH = $nativePath
    Remove-Item ([IO.Path]::ChangeExtension($output, '.obj')) -ErrorAction SilentlyContinue
}
Write-Output "Native C17 launcher built: $output"

param([switch]$SkipPackage)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build'
$distDir = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++ and a Windows 10/11 SDK.' }
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'The Visual Studio C++ compiler was not found. Install the Desktop development with C++ workload.' }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkVersion = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'Include') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'cppwinrt\winrt\Windows.Media.Ocr.h') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if (!$sdkVersion) { throw 'A recent Windows SDK with C++/WinRT headers is required.' }
$sdkBin = Join-Path $sdkRoot "bin\$sdkVersion\x64"
$compileScript = Join-Path $buildDir 'compile.cmd'
# Paths are quoted in a fixed batch file; source/config text never becomes shell code.
$batch = @"
@echo off
call "$vcvars" $sdkVersion >nul
if errorlevel 1 exit /b 1
cd /d "$projectRoot\src"
rc /nologo /fo "$buildDir\app.res" app.rc
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /GL /MT /EHsc /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo"$buildDir\main.obj" main.cpp "$buildDir\app.res" /Fe"$distDir\Glyph.exe" /link /LTCG /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib msimg32.lib windowsapp.lib shcore.lib
exit /b %errorlevel%
"@
Set-Content -LiteralPath $compileScript -Value $batch -Encoding ascii
& $env:ComSpec /d /c $compileScript
if ($LASTEXITCODE -ne 0) { throw "Compilation failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $distDir -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'install.ps1'), (Join-Path $projectRoot 'uninstall.ps1') -Destination $distDir -Force
if (!$SkipPackage) {
    $packageDir = Join-Path $buildDir 'package'
    $assetsDir = Join-Path $packageDir 'Assets'
    New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $distDir 'Glyph.exe') -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'packaging\AppxManifest.xml') -Destination $packageDir -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'assets\StoreLogo.png'), (Join-Path $projectRoot 'assets\Logo150.png'), (Join-Path $projectRoot 'assets\Logo44.png') -Destination $assetsDir -Force
    & (Join-Path $sdkBin 'makeappx.exe') pack /d $packageDir /p (Join-Path $distDir 'Glyph.msix') /o
    if ($LASTEXITCODE -ne 0) { throw 'MSIX packaging failed.' }
}
Write-Host "Built: $distDir\Glyph.exe"
if (!$SkipPackage) { Write-Host "Package: $distDir\Glyph.msix (unsigned; install.ps1 signs a local copy)" }

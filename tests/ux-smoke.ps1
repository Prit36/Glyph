$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsPath) { throw 'Visual Studio C++ Build Tools are required.' }
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
$script = Join-Path $buildDir 'ux-smoke.cmd'
$batch = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$projectRoot"
cd src
rc /nologo /fo "$buildDir\ux-app.res" app.rc
if errorlevel 1 exit /b 1
cd /d "$projectRoot"
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /permissive- /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo"$buildDir\ux_smoke.obj" tests\ux_smoke.cpp "$buildDir\ux-app.res" /Fe"$buildDir\ux_smoke.exe" /link /SUBSYSTEM:CONSOLE user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib msimg32.lib windowsapp.lib shcore.lib
if errorlevel 1 exit /b 1
"$buildDir\ux_smoke.exe"
exit /b %errorlevel%
"@
Set-Content -LiteralPath $script -Value $batch -Encoding ascii
& $env:ComSpec /d /c $script
if ($LASTEXITCODE -ne 0) { throw "UX smoke checks failed ($LASTEXITCODE)." }

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC C++ build tools are required.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
New-Item -ItemType Directory -Force build | Out-Null
$batch = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /MD /W4 /I third_party\imgui src\main.cpp third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_win32.cpp third_party\imgui\backends\imgui_impl_dx11.cpp /Fo:build\ /Fe:build\AtomX.exe /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /utf-8 /MD /W4 tests\core_tests.cpp /Fo:build\ /Fe:build\core_tests.exe
if errorlevel 1 exit /b 1
build\core_tests.exe
"@
Set-Content -LiteralPath build/compile.cmd -Value $batch -Encoding utf8
& cmd.exe /d /c build\compile.cmd
if ($LASTEXITCODE -ne 0) { throw "Build or tests failed: $LASTEXITCODE" }

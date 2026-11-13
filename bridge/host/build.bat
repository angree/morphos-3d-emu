@echo off
rem SPDX-License-Identifier: MIT
rem Build tglhost.exe with MSVC (x64). Needs the gen_* files written by bridge\gen\gen.py
rem (bridge/guest/build.sh runs it; see docs\BUILD.md). Output: bin\host\tglhost.exe
setlocal
cd /d "%~dp0"
if not exist gen_dispatch.c (
  echo *** gen_dispatch.c is missing: run bridge\gen\gen.py with your MorphOS SDK first ^(docs\BUILD.md^)
  exit /b 1
)
where cl >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    echo *** cl.exe not in PATH and vswhere.exe not found: run this from a "x64 Native Tools" prompt
    exit /b 1
  )
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist ..\..\bin\host mkdir ..\..\bin\host
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS tglhost.c gen_dispatch.c gen_names.c /Fo..\..\bin\host\ /Fe:..\..\bin\host\tglhost.exe /link /SUBSYSTEM:CONSOLE

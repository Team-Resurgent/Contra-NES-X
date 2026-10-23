@echo off
rem Contra NES-X: one double-click to Contra-NES-X.iso.
rem
rem   run this file (double-click, or "build.bat" in a console)
rem
rem Put your own Contra (US) cartridge in this folder as baserom.nes first.
rem It finds a Python 3.8 or later and hands over to setup.py, which fetches
rem the Contra (US) disassembly and cc65, assembles the cartridge (its
rem graphics and music from your baserom.nes) and builds. Arguments are
rem passed on: build.bat --check, build.bat --config Debug,
rem build.bat --skip-build, build.bat --no-assemble.
setlocal EnableExtensions
set "CNX_SELF=%~f0"
rem A Visual Studio build of an RXDK project runs its Pre-Build Event with no
rem PATH at all; put the Windows folders back so py and where resolve.
if not defined PATH set "PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem"
cd /d "%~dp0"
title Contra NES-X build

echo.
echo  Contra NES-X -- Contra (NES, 1988) for the original Xbox
echo.

set "CNX_PY="
for %%V in (3.13 3.12 3.14 3.15 3.11 3.10 3.9 3.8) do (
  if not defined CNX_PY (
    py -%%V -c "import sys" >nul 2>nul && set "CNX_PY=py -%%V"
  )
)
if not defined CNX_PY (
  python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "CNX_PY=python"
)
if not defined CNX_PY (
  echo  No Python 3.8 or later was found. Install it from
  echo      https://www.python.org/downloads/
  echo  ^(tick "Add python.exe to PATH"^), then run this again.
  goto :fail
)

echo  Python: %CNX_PY%
echo.
%CNX_PY% setup.py %*
set "CNX_RC=%ERRORLEVEL%"
if not "%CNX_RC%"=="0" goto :failrc
echo.
echo  Done.
call :pause
exit /b 0

:failrc
echo.
echo  The build stopped (exit code %CNX_RC%). The messages above say why.
call :pause
exit /b %CNX_RC%

:fail
echo.
call :pause
exit /b 1

rem ---- keep a double-clicked window open --------------------------------
:pause
if defined CNX_NONINTERACTIVE exit /b 0
echo %cmdcmdline% | find /i "%CNX_SELF%" >nul
if not errorlevel 1 pause
exit /b 0

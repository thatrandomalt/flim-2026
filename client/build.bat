@echo off
REM Build flim.exe (launcher) + flim_engine.dll (injected engine).
REM Requires MSVC Build Tools. Output goes to client\native\out\.
setlocal

set VCVARS="C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
  echo vcvars64.bat not found - adjust VCVARS in build.bat
  exit /b 1
)
call %VCVARS% >nul

set ROOT=%~dp0
set OUT=%ROOT%out
if not exist "%OUT%" mkdir "%OUT%"

echo === compiling engine resource ===
rc /nologo /fo "%OUT%\flim_engine.res" "%ROOT%engine\flim_engine.rc"
if errorlevel 1 exit /b 1

echo === compiling engine DLL ===
cl /nologo /O2 /W3 /LD "%ROOT%engine\flim_engine.c" "%OUT%\flim_engine.res" /Fe:"%OUT%\flim_engine.dll" /Fo:"%OUT%\\" winhttp.lib
if errorlevel 1 exit /b 1

echo === compiling launcher resource ===
rc /nologo /fo "%OUT%\flim.res" "%ROOT%launcher\flim.rc"
if errorlevel 1 exit /b 1

echo === compiling launcher exe ===
cl /nologo /O2 /W3 "%ROOT%launcher\flim.c" "%ROOT%launcher\host.c" "%OUT%\flim.res" /Fe:"%OUT%\flim.exe" /Fo:"%OUT%\\" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED user32.lib
if errorlevel 1 exit /b 1

echo === done ===
dir /b "%OUT%"
endlocal

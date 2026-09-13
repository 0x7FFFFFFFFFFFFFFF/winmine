@echo off
rem ====================================================================
rem  build.cmd - builds winmine.exe (a Minesweeper clone) in this folder.
rem
rem  winmine.exe is held to a 119,808 byte budget.  Almost all of that is
rem  artwork and sound, so the code has to fit in what is left.  Two
rem  things make that work:
rem
rem    * the program is linked WITHOUT the C run-time - MinerEntry() in
rem      miner.cpp is the raw PE entry point, and the only library
rem      routines it needs (memset, rand) are in miner.cpp itself;
rem    * it is built 32-bit with a fixed image base, so there is no
rem      .reloc section.
rem
rem  Toolchain preference:
rem    1. MSVC, 32-bit  - located through vswhere; meets the budget.
rem    2. cl.exe already on PATH.
rem    3. MinGW-w64     - works, but a 64-bit build cannot fit in the
rem                       budget; build.cmd says so and fails.
rem
rem  Everything below works on relative paths, so the project directory
rem  may contain spaces and parentheses.
rem ====================================================================
setlocal EnableExtensions DisableDelayedExpansion

cd /d "%~dp0"
if errorlevel 1 goto failed

if not exist "src\miner.cpp" goto no_source
if not exist "build" mkdir "build"
if exist "winmine.exe" del /q "winmine.exe"

set "LIMIT=119808"

rem ------------------------------------------------- MSVC through vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto try_path_cl

set "VSDIR="
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "build\vs.txt" 2>nul
for /f "usebackq tokens=*" %%I in ("build\vs.txt") do set "VSDIR=%%I"
del /q "build\vs.txt" 2>nul
if not defined VSDIR goto try_path_cl
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" goto try_path_cl

rem  (vcvars32 prints a harmless vswhere diagnostic of its own, so its
rem   stderr is swallowed here; a real failure surfaces at rc/cl below.)
echo Setting up the 32-bit MSVC toolchain ...
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 goto try_path_cl
goto build_msvc

:try_path_cl
where cl.exe >nul 2>&1
if not errorlevel 1 goto build_msvc

rem --------------------------------------------------------------- MinGW
where g++.exe >nul 2>&1
if not errorlevel 1 goto build_mingw
if exist "C:\Qt\Tools\mingw1310_64\bin\g++.exe" goto use_qt_default
goto scan_qt

:use_qt_default
set "PATH=C:\Qt\Tools\mingw1310_64\bin;%PATH%"
goto build_mingw

:scan_qt
for /d %%D in ("C:\Qt\Tools\mingw*") do call :try_mingw "%%~D"
where g++.exe >nul 2>&1
if not errorlevel 1 goto build_mingw
goto no_compiler

:try_mingw
if not exist "%~1\bin\g++.exe" exit /b 0
set "PATH=%~1\bin;%PATH%"
exit /b 0

rem ====================================================================
:build_msvc
echo Building winmine.exe with MSVC ...

rc.exe /nologo /I src /fo build\winmine.res src\miner.rc
if errorlevel 1 goto failed

cl.exe /nologo /c /O1 /Os /GS- /Gy /GR- /GF /Gw /W3 ^
    /DUNICODE /D_UNICODE /DMINER_NO_CRT /I src ^
    /Fobuild\winmine.obj src\miner.cpp
if errorlevel 1 goto failed

rem  /NODEFAULTLIB + /ENTRY:MinerEntry  - no C run-time at all
rem  /FIXED /DYNAMICBASE:NO             - drops the .reloc section
link.exe /nologo /SUBSYSTEM:WINDOWS /ENTRY:MinerEntry /NODEFAULTLIB ^
    /OPT:REF /OPT:ICF /MERGE:.rdata=.text /FIXED /DYNAMICBASE:NO ^
    /OUT:winmine.exe build\winmine.obj build\winmine.res ^
    kernel32.lib user32.lib gdi32.lib shell32.lib comctl32.lib ^
    winmm.lib
if errorlevel 1 goto failed
goto check_size

rem ====================================================================
:build_mingw
echo Building winmine.exe with MinGW-w64 ...

windres.exe -I src -i src\miner.rc -O coff -o build\winmine.res.o
if errorlevel 1 goto failed

call :link_mingw MinerEntry
if not exist "winmine.exe" call :link_mingw _MinerEntry
if not exist "winmine.exe" goto failed
goto check_size

:link_mingw
g++.exe -std=c++11 -Os -DUNICODE -D_UNICODE -DMINER_NO_CRT -Wall ^
    -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables ^
    -fno-unwind-tables -fno-ident -fno-stack-protector ^
    -fomit-frame-pointer -falign-functions=1 -fmerge-all-constants ^
    -ffunction-sections -nostartfiles -nodefaultlibs ^
    -Wl,--gc-sections -Wl,--subsystem,windows -Wl,-e%~1 -s ^
    -o winmine.exe src\miner.cpp build\winmine.res.o ^
    -lgdi32 -luser32 -lshell32 -lcomctl32 -lwinmm -lkernel32
exit /b 0

rem ====================================================================
:check_size
set "SZ=0"
for %%F in ("winmine.exe") do set "SZ=%%~zF"
echo.
echo Built "%CD%\winmine.exe"
echo   winmine.exe   %SZ% bytes
echo   budget      %LIMIT% bytes
if %SZ% GTR %LIMIT% goto too_big
echo   OK - within budget.
exit /b 0

:too_big
echo.
echo ERROR: winmine.exe is over the size budget.
echo        This happens with a 64-bit MinGW build: the artwork and
echo        sounds alone take ~100 KB, which leaves too little room for
echo        64-bit code.  Build with 32-bit MSVC (install the "Desktop
echo        development with C++" workload) or a 32-bit MinGW-w64.
exit /b 1

:no_source
echo ERROR: src\miner.cpp not found next to build.cmd.
exit /b 1

:no_compiler
echo ERROR: no C++ compiler found.
echo        Install Visual Studio with the C++ workload, or MinGW-w64.
exit /b 1

:failed
echo.
echo BUILD FAILED
exit /b 1

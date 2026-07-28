@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "VCVARS_DIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"

if exist "!VCVARS_DIR!\vcvars64.bat" goto :vcvars_found
echo [ERROR] Could not find VS2022 BuildTools at:
echo   !VCVARS_DIR!
echo Install Visual Studio 2022 Build Tools with the "Desktop development with C++" workload,
echo or edit VCVARS_DIR at the top of this script to point at your installation.
exit /b 1
:vcvars_found

REM Everything except the final zips is built in a throwaway TMPDIR, removed at the end, so
REM build\ ends up containing only the two packaged plugin archives -- nothing else.
set "OUTDIR=build"
set "TMPDIR=%OUTDIR%\_tmp"
if exist "%TMPDIR%" rmdir /s /q "%TMPDIR%"
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
mkdir "%TMPDIR%\mt" "%TMPDIR%\md"

set SOURCES=src\metadata_parser.cpp src\aimg_decoder.cpp src\aimg.cpp

REM Two runtime-linkage variants, both 32+64 bit:
REM   mt = /MT  (static CRT)  -- self-contained, no VC++ Redistributable needed on target machine
REM   md = /MD  (dynamic CRT) -- smaller, but requires VCRUNTIME140(_1).dll (VC++ Redistributable) on target

echo ========================================
echo Building 64-bit plugin (/MT and /MD)...
echo ========================================
call "!VCVARS_DIR!\vcvars64.bat"
if %errorlevel% neq 0 (
    echo [ERROR] Failed to initialize the x64 build environment.
    exit /b %errorlevel%
)
rc /fo "%TMPDIR%\aimg64.res" /i src src\aimg.rc
if %errorlevel% neq 0 (
    echo Error compiling resource!
    exit /b %errorlevel%
)
cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /MT /LD /I src %SOURCES% /Fo"%TMPDIR%\mt\\" /Fe:"%TMPDIR%\mt\aimg.wdx64" /link /DEF:src\aimg.def "%TMPDIR%\aimg64.res" /MACHINE:X64 /LTCG /OPT:REF /OPT:ICF
if %errorlevel% neq 0 (
    echo Error building 64-bit /MT plugin!
    exit /b %errorlevel%
)
cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /MD /LD /I src %SOURCES% /Fo"%TMPDIR%\md\\" /Fe:"%TMPDIR%\md\aimg.wdx64" /link /DEF:src\aimg.def "%TMPDIR%\aimg64.res" /MACHINE:X64 /LTCG /OPT:REF /OPT:ICF
if %errorlevel% neq 0 (
    echo Error building 64-bit /MD plugin!
    exit /b %errorlevel%
)

echo ========================================
echo Building 32-bit plugin (/MT and /MD)...
echo ========================================
call "!VCVARS_DIR!\vcvars32.bat"
if %errorlevel% neq 0 (
    echo [ERROR] Failed to initialize the x86 build environment.
    exit /b %errorlevel%
)
rc /fo "%TMPDIR%\aimg32.res" /i src src\aimg.rc
if %errorlevel% neq 0 (
    echo Error compiling resource!
    exit /b %errorlevel%
)
cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /MT /LD /I src %SOURCES% /Fo"%TMPDIR%\mt\\" /Fe:"%TMPDIR%\mt\aimg.wdx" /link /DEF:src\aimg.def "%TMPDIR%\aimg32.res" /MACHINE:X86 /LTCG /OPT:REF /OPT:ICF
if %errorlevel% neq 0 (
    echo Error building 32-bit /MT plugin!
    exit /b %errorlevel%
)
cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /MD /LD /I src %SOURCES% /Fo"%TMPDIR%\md\\" /Fe:"%TMPDIR%\md\aimg.wdx" /link /DEF:src\aimg.def "%TMPDIR%\aimg32.res" /MACHINE:X86 /LTCG /OPT:REF /OPT:ICF
if %errorlevel% neq 0 (
    echo Error building 32-bit /MD plugin!
    exit /b %errorlevel%
)

echo ========================================
echo Creating Total Commander Installer Archives...
echo ========================================
tar -a -c -f "%OUTDIR%\aimg.wdx.zip" -C "%TMPDIR%\mt" aimg.wdx aimg.wdx64 -C ..\..\.. pluginst.inf README.md
if %errorlevel% neq 0 (
    echo Error packaging aimg.wdx.zip!
    exit /b %errorlevel%
)
tar -a -c -f "%OUTDIR%\aimg-md.wdx.zip" -C "%TMPDIR%\md" aimg.wdx aimg.wdx64 -C ..\..\.. pluginst.inf README.md
if %errorlevel% neq 0 (
    echo Error packaging aimg-md.wdx.zip!
    exit /b %errorlevel%
)

rmdir /s /q "%TMPDIR%"

echo ========================================
echo BUILD SUCCESSFUL!
echo Output files:
echo   - build\aimg.wdx.zip     (Total Commander Auto-Installer Package, /MT static CRT, self-contained)
echo   - build\aimg-md.wdx.zip  (Total Commander Auto-Installer Package, /MD dynamic CRT, needs VC++ Redistributable)
echo ========================================

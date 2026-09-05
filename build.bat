@echo off
setlocal enabledelayedexpansion
echo Building Total Commander AImg Content Plugin...

mkdir build 2>nul

echo Checking for CMake...
where cmake >nul 2>nul
if %errorlevel%==0 (
    cd build
    cmake .. || (cd .. & echo [ERROR] CMake configure failed. & exit /b 1)
    cmake --build . --config Release
    if errorlevel 1 (
        cd ..
        echo [ERROR] CMake build failed.
        exit /b 1
    )
    cd ..
    echo Build completed via CMake.
    goto end
)

echo Checking for MSVC (cl.exe)...
where cl >nul 2>nul
if %errorlevel%==0 (
    REM Read the target bitness from the developer prompt's own environment rather than
    REM guessing from the host, so a vcvars32 prompt produces aimg.wdx (32-bit), not aimg.wdx64.
    if defined VSCMD_ARG_TGT_ARCH (
        set "TARGET_ARCH=%VSCMD_ARG_TGT_ARCH%"
    ) else if defined Platform (
        set "TARGET_ARCH=%Platform%"
    ) else (
        echo [NOTE] Could not detect target architecture from the environment - assuming x64.
        set "TARGET_ARCH=x64"
    )
    REM Some toolchains spell the 32-bit platform "Win32" in %Platform% rather than "x86".
    if /i "!TARGET_ARCH!"=="x86" (
        set "OUTFILE=build\aimg.wdx"
        set "MACHINE=/MACHINE:X86"
    ) else if /i "!TARGET_ARCH!"=="Win32" (
        set "OUTFILE=build\aimg.wdx"
        set "MACHINE=/MACHINE:X86"
    ) else (
        set "OUTFILE=build\aimg.wdx64"
        set "MACHINE=/MACHINE:X64"
    )
    echo Building for !TARGET_ARCH!...
    rc /fo build\aimg.res /i src src\aimg.rc
    if errorlevel 1 (
        echo [ERROR] Resource compile failed.
        exit /b 1
    )
    cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /DUNICODE /D_UNICODE /DNDEBUG /MT /LD /I src src\metadata_parser.cpp src\aimg_decoder.cpp src\comfyui_decoder.cpp src\aimg.cpp /Fo"build\\" /Fe:!OUTFILE! /link /DEF:src\aimg.def build\aimg.res !MACHINE! /LTCG /OPT:REF /OPT:ICF
    if errorlevel 1 (
        echo [ERROR] MSVC build failed.
        exit /b 1
    )
    echo Build completed via MSVC.
    goto end
)

echo Checking for MinGW / GCC (g++)...
where g++ >nul 2>nul
if %errorlevel%==0 (
    REM Name the output after what this g++ actually targets instead of assuming x64 - a
    REM 32-bit MinGW install must produce aimg.wdx, not a wrongly-named aimg.wdx64.
    for /f "usebackq delims=" %%A in (`g++ -dumpmachine`) do set "GCC_MACHINE=%%A"
    echo(!GCC_MACHINE! | findstr /i "x86_64 aarch64" >nul
    if !errorlevel!==0 (
        set "OUTFILE=build/aimg.wdx64"
        set "TARGET_ARCH=x64"
    ) else (
        set "OUTFILE=build/aimg.wdx"
        set "TARGET_ARCH=x86"
    )
    echo Building for !TARGET_ARCH! ^(!GCC_MACHINE!^)...
    windres -I src src\aimg.rc -O coff -o build\aimg.res.o
    if errorlevel 1 (
        echo [ERROR] Resource compile failed.
        exit /b 1
    )
    g++ -Wall -O3 -flto -fno-rtti -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -DNDEBUG -shared -I src src/metadata_parser.cpp src/aimg_decoder.cpp src/comfyui_decoder.cpp src/aimg.cpp src/aimg.def build/aimg.res.o -o !OUTFILE! -static -s -Wl,--gc-sections
    if errorlevel 1 (
        echo [ERROR] GCC build failed.
        exit /b 1
    )
    echo Build completed via GCC.
    goto end
)

echo [WARNING] No standard C++ compiler found in PATH.
echo Install MSVC, GCC/MinGW, or CMake to compile binary .wdx / .wdx64 files.

:end

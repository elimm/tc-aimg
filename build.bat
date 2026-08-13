@echo off
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
    rc /fo build\aimg.res /i src src\aimg.rc
    if errorlevel 1 (
        echo [ERROR] Resource compile failed.
        exit /b 1
    )
    cl /W4 /WX /O2 /GL /Gy /GR- /EHsc /DUNICODE /D_UNICODE /MT /LD /I src src\metadata_parser.cpp src\aimg_decoder.cpp src\aimg.cpp /Fe:build\aimg.wdx64 /link /DEF:src\aimg.def build\aimg.res /MACHINE:X64 /LTCG /OPT:REF /OPT:ICF
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
    windres -I src src\aimg.rc -O coff -o build\aimg.res.o
    if errorlevel 1 (
        echo [ERROR] Resource compile failed.
        exit /b 1
    )
    g++ -O3 -flto -fno-rtti -ffunction-sections -fdata-sections -DUNICODE -D_UNICODE -shared -I src src/metadata_parser.cpp src/aimg_decoder.cpp src/aimg.cpp src/aimg.def build/aimg.res.o -o build/aimg.wdx64 -static -Wl,--gc-sections
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

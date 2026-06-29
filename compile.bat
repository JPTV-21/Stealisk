@echo off
set PATH=%~dp0mingw32\bin\;%PATH%
echo Compiling Stealisk...

windres resource.rc -o resource.o
if %errorlevel% neq 0 (
    echo windres failed.
    pause
    exit /b %errorlevel%
)

g++ -std=c++17 Stealisk.cpp resource.o -o Stealisk.exe -static -O2 -s -Os -municode ^
    -lshlwapi -luser32 -lshell32 -ladvapi32 -lws2_32 -lgdi32 -lcrypt32 -lpsapi -lole32

if %errorlevel% neq 0 (
    echo g++ failed.
    pause
    exit /b %errorlevel%
)

echo Build successful.
pause
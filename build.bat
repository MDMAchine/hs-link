@echo off
REM HOT-STEP LINK build (AGPL-3.0-or-later). Usage: build.bat [test]
cd /d "%~dp0"
cmake -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
cmake --build build --config Release -- /p:TrackFileAccess=false /m:8 || exit /b 1
if /i "%1"=="test" build\HSLINK_Tests_artefacts\Release\HSLINK_Tests.exe || exit /b 1

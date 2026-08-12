@echo off
REM Builds the interactive viewport.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set VCVARS=%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat
set CMAKE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

call "%VCVARS%" x64 >nul
if errorlevel 1 (
    echo ERROR: vcvarsall failed
    exit /b 1
)

set BUILD_DIR=%~dp0build

"%CMAKE%" -S "%~dp0." -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%BUILD_DIR%" --target makina_viewport
if errorlevel 1 exit /b 1

echo.
echo Built: %BUILD_DIR%\bin\makina_viewport.exe
echo Run:   %BUILD_DIR%\bin\makina_viewport.exe ..\tools\gsf2json\out\hero_flange.makina.json

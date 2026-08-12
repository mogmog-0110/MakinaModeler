@echo off
REM Checks that makina::Scene satisfies MitiruEngine's GameMemory contract.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set VCVARS=%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat
set CMAKE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

if not exist "%VCVARS%" (
    echo ERROR: vcvarsall.bat not found at "%VCVARS%"
    exit /b 1
)

call "%VCVARS%" x64 >nul
if errorlevel 1 (
    echo ERROR: vcvarsall failed
    exit /b 1
)

set BUILD_DIR=%~dp0build

"%CMAKE%" -S "%~dp0." -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo ERROR: cmake configure failed
    exit /b 1
)

"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo ERROR: build failed
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\reflect_check.exe" ^
    "%~dp0..\tools\gsf2json\out\hero_flange.makina.json" ^
    "%~dp0..\tools\gsf2json\out\pettobotoru.makina.json"
if errorlevel 1 (
    echo.
    echo ERROR: GameMemory contract check failed
    exit /b 1
)

endlocal

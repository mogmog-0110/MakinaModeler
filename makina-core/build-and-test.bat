@echo off
REM Builds makina-core's tests and runs the round-trip check over the converted Grasp3D scenes.
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

set J=%~dp0..\tools\gsf2json\out
set S=%~dp0..\tools\sdfdump\out
set M=%~dp0..\tools\measuredump\out
set P=%~dp0..\tools\povdump\out

REM The corpus is discovered rather than listed, so a scene added to tools\makescenes is
REM picked up without anyone having to remember to edit this file.
setlocal enabledelayedexpansion
set ROUND_ARGS=
set PAIR_ARGS=
set MEASURE_ARGS=
set POV_ARGS=
REM %%~nf strips one extension only, so scene.makina.json becomes scene.makina. The inner loop
REM strips the second one, which is what pairs it with scene.sdf.txt.
for %%f in ("%J%\*.makina.json") do (
    set ROUND_ARGS=!ROUND_ARGS! "%%f"
    for %%g in ("%%~nf") do (
        if exist "%S%\%%~ng.sdf.txt" (
            set PAIR_ARGS=!PAIR_ARGS! "%%f" "%S%\%%~ng.sdf.txt"
        ) else (
            echo WARNING: no reference dump for %%~nxf; skipping its SDF comparison
        )
        if exist "%M%\%%~ng.measure.txt" (
            set MEASURE_ARGS=!MEASURE_ARGS! "%%f" "%M%\%%~ng.measure.txt"
        ) else (
            echo WARNING: no measure dump for %%~nxf; skipping its measurement comparison
        )
        if exist "%P%\%%~ng.pov" (
            set POV_ARGS=!POV_ARGS! "%%f" "%P%\%%~ng.pov"
        ) else (
            echo WARNING: no POV dump for %%~nxf; skipping its export comparison
        )
    )
)

echo.
"%BUILD_DIR%\bin\roundtrip.exe" !ROUND_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: round-trip test failed
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\sdf_compare.exe" !PAIR_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: SDF comparison against the Java reference failed
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\measure_compare.exe" !MEASURE_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: measurement comparison against the Java reference failed
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\pov_compare.exe" !POV_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: POV export comparison against the Java reference failed
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\bsp_compare.exe" !ROUND_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: the SDF and the boundary representation disagree
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\edit_check.exe" !ROUND_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: editing broke the tree
    exit /b 1
)

echo.
"%BUILD_DIR%\bin\command_check.exe" !ROUND_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: the command layer misbehaved
    exit /b 1
)
endlocal

endlocal

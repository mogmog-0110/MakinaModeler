@echo off
REM The live generated shader against the baked one, on the same scenes (PLAN.md D-15).
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM A baked shader has every leaf's numbers folded into its code. A live one is specialised to
REM the tree's structure only and reads the numbers from the program buffer, so the engine can
REM upload a freshly sampled program every frame and a joint moves without a recompile. Same
REM program, same buffer, same shading: the only difference allowed is where a number came from,
REM so the pictures must agree to the byte, and the limit below is the interpret-check's.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENES=%HERE%..\makina-core\tests\scenes
set WIDTH=960
set HEIGHT=540

if not exist "%BIN%\render_scene.exe" (
    echo ERROR: render_scene.exe not found. Run build.bat first.
    exit /b 1
)

del /q "%BIN%\shaded_*.bmp" "%BIN%\live_*.bmp" >nul 2>&1

set SCENE_ARGS=
for %%f in ("%SCENES%\*.makina.json") do set SCENE_ARGS=!SCENE_ARGS! "%%f"

"%BIN%\render_scene.exe" --pov-match --width %WIDTH% --height %HEIGHT% !SCENE_ARGS! >nul
if errorlevel 1 (
    echo ERROR: the baked pass failed
    exit /b 1
)
"%BIN%\render_scene.exe" --pov-match --live --width %WIDTH% --height %HEIGHT% !SCENE_ARGS! >nul
if errorlevel 1 (
    echo ERROR: the live pass failed
    exit /b 1
)

REM shaded_<tag>.bmp and live_<tag>.bmp for the same scene: paired by tag, not by a list.
set PAIRS=
pushd "%BIN%"
for %%f in (live_*.bmp) do (
    set NAME=%%~nf
    set TAG=!NAME:live_=!
    if exist "shaded_!TAG!.bmp" (
        set PAIRS=!PAIRS! "%BIN%\shaded_!TAG!.bmp" "%BIN%\%%~nf.bmp"
    )
)
popd

if "%PAIRS%"=="" (
    echo ERROR: no pair of images to compare
    exit /b 1
)

"%BIN%\color_compare.exe" --mean 0.5 --share 0.0005 --title "baked shader vs live shader" --agreed "the two spellings agree pixel for pixel" %PAIRS%
if errorlevel 1 (
    echo.
    echo    THE BAKED AND THE LIVE SHADER DISAGREE
    exit /b 1
)

echo.
echo    the baked shader and the live one draw the same picture
exit /b 0

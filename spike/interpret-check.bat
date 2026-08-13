@echo off
REM The interpreted program against the generated one, on the same scenes.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM There are two ways to evaluate a scene here. The generated shader unrolls the program into
REM straight-line code and compiles it; the interpreter walks the same program out of a buffer with
REM an explicit stack, so the engine can change a model without compiling anything. Two
REM implementations of one set of rules is the risk this project is built to catch, and until now
REM nothing compared their pictures -- scene_interpret.hlsl said they were compared and they were
REM not.
REM
REM The limit is tight on purpose. These are not two renderers: they are the same shading over the
REM same program, and the only thing that differs is the order the arithmetic happens in. A limit
REM loose enough for POV-Ray would pass them however far apart they had drifted.

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

REM Last run's output first, so a scene that stops being rendered cannot leave a stale pair behind
REM for the comparison to agree about.
del /q "%BIN%\shaded_*.bmp" "%BIN%\interp_*.bmp" >nul 2>&1

set SCENE_ARGS=
for %%f in ("%SCENES%\*.makina.json") do set SCENE_ARGS=!SCENE_ARGS! "%%f"

"%BIN%\render_scene.exe" --pov-match --width %WIDTH% --height %HEIGHT% !SCENE_ARGS! >nul
if errorlevel 1 (
    echo ERROR: the generated pass failed
    exit /b 1
)
"%BIN%\render_scene.exe" --pov-match --interpret --width %WIDTH% --height %HEIGHT% !SCENE_ARGS! >nul
if errorlevel 1 (
    echo ERROR: the interpreted pass failed
    exit /b 1
)

REM The two passes write shaded_<tag>.bmp and interp_<tag>.bmp for the same scene, so the pairing
REM is the tag with the prefix swapped rather than a fixed list to keep in step.
set PAIRS=
pushd "%BIN%"
for %%f in (interp_*.bmp) do (
    set NAME=%%~nf
    set TAG=!NAME:interp_=!
    if exist "shaded_!TAG!.bmp" (
        set PAIRS=!PAIRS! "%BIN%\shaded_!TAG!.bmp" "%BIN%\%%~nf.bmp"
    )
)
popd

if "%PAIRS%"=="" (
    echo ERROR: no pair of images to compare
    exit /b 1
)

"%BIN%\color_compare.exe" --mean 0.5 --share 0.0005 --title "generated shader vs interpreted program" --agreed "the two evaluators agree pixel for pixel" %PAIRS%
if errorlevel 1 (
    echo.
    echo    THE TWO EVALUATORS DISAGREE
    exit /b 1
)

echo.
echo    the compiled program and the interpreted one draw the same picture
exit /b 0

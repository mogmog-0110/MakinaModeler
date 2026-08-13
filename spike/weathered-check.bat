@echo off
REM The weathered look, which had no check of any kind until this file.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM Every other comparison in this project points at scene_shading.hlsl, the pass held to POV-Ray.
REM The weathered pass is a second renderer -- its own surface model, its own lighting, its own
REM ground plane -- and nothing looked at it. That is how it came to draw a see-through bottle as a
REM solid lump for four days in the one image the project exists to produce.
REM
REM There is no oracle for a look, so this does not compare it to anything outside. It asks the one
REM question that catches a feature which has stopped reaching the picture: **does the scene still
REM change the image?** glass.makina.json and glass_solid.makina.json are the same scene apart from
REM one alpha, so a pass that ignores alpha renders them identically -- which is exactly what
REM happened, and exactly what this fails on.
REM
REM "The two agree" proves nothing when a renderer has gone blank, so the verdict here is inverted:
REM the run fails if the two pictures are the same.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENES=%HERE%..\makina-core\tests\scenes
set WIDTH=800
set HEIGHT=450

if not exist "%BIN%\render_scene.exe" (
    echo ERROR: render_scene.exe not found. Run build.bat first.
    exit /b 1
)

del /q "%BIN%\weathered_glass.bmp" "%BIN%\weathered_glass_solid.bmp" >nul 2>&1

"%BIN%\render_scene.exe" --weathered --width %WIDTH% --height %HEIGHT% ^
    "%SCENES%\glass.makina.json" "%SCENES%\glass_solid.makina.json" >nul
if errorlevel 1 (
    echo    ERROR: the weathered pass did not finish
    exit /b 1
)

REM A mean of one level is well under what a material change produces and well over the noise
REM between two renders of the same thing, which is zero -- these are the same binary on the same
REM scene, so there is no sampling to disagree about.
"%BIN%\color_compare.exe" --differ --mean 1.0 --share 0.0 ^
    --title "the weathered look, with a material and without it" ^
    --agreed "the scene still reaches the weathered picture" ^
    "%BIN%\weathered_glass.bmp" "%BIN%\weathered_glass_solid.bmp"
if errorlevel 1 (
    echo.
    echo    THE WEATHERED LOOK IGNORES A MATERIAL THE SCENE SETS
    exit /b 1
)

echo.
echo    the weathered look reads what the scene says
exit /b 0

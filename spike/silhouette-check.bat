@echo off
REM Phase 5, third leg: the SDF ray march against POV-Ray, silhouette for silhouette.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENES=%HERE%..\tools\gsf2json\out
set POVRAY=D:\sandbox\Grasp3D\povray\bin\povray.exe
set POVINC=D:\sandbox\Grasp3D\povray\include

set WIDTH=1280
set HEIGHT=720

if not exist "%POVRAY%" (
    echo ERROR: POV-Ray not found at "%POVRAY%"
    exit /b 1
)
if not exist "%BIN%\render_scene.exe" (
    echo ERROR: render_scene.exe not found. Run build.bat first.
    exit /b 1
)

REM One pass writes both the mask and the .pov, from one camera. Deriving the camera twice is how
REM a silhouette comparison ends up measuring a camera mismatch instead of the geometry.
set SCENE_ARGS=
for %%f in ("%SCENES%\*.makina.json") do set SCENE_ARGS=!SCENE_ARGS! "%%f"

"%BIN%\render_scene.exe" --mask --width %WIDTH% --height %HEIGHT% !SCENE_ARGS!
if errorlevel 1 (
    echo ERROR: the mask pass failed
    exit /b 1
)

set PAIRS=
pushd "%BIN%"
for %%f in (mask_*.pov) do (
    REM -A disables antialiasing: a soft edge would blur the disagreement into the tolerance.
    "%POVRAY%" +I"%%f" +O"pov_%%~nf.bmp" +W%WIDTH% +H%HEIGHT% +FS -A -D -GA +L"%POVINC%" >nul 2>&1
    if exist "pov_%%~nf.bmp" (
        set PAIRS=!PAIRS! "%BIN%\%%~nf.bmp" "%BIN%\pov_%%~nf.bmp"
    ) else (
        echo WARNING: POV-Ray produced no image for %%f
    )
)
popd

echo.
"%BIN%\silhouette_compare.exe" !PAIRS!
if errorlevel 1 (
    echo.
    echo ERROR: the two renderers do not draw the same shape
    exit /b 1
)

endlocal

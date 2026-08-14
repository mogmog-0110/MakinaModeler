@echo off
REM The whole pingu model against POV-Ray: blob, sor, two b_spline sweeps, macro-expanded
REM eyes and per-component scales in one frame. The integration measurement -- each shape
REM passed alone, and this is where a transform applied in the wrong order would show.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENE=%HERE%pingu_model.makina.json
set POVRAY=D:\sandbox\Grasp3D\povray\bin\povray.exe
set POVINC=D:\sandbox\Grasp3D\povray\include

REM 320x180: three sor polyline loops per march step put a full-frame draw past the GPU
REM watchdog (~17 s at 1280x720, measured); this stays at ~1 s. Raise it when the
REM program gains per-node bound culling.
set WIDTH=320
set HEIGHT=180

if not exist "%POVRAY%" (
    echo ERROR: POV-Ray not found at "%POVRAY%"
    exit /b 1
)
if not exist "%BIN%\render_scene.exe" (
    echo ERROR: render_scene.exe not found. Run build.bat first.
    exit /b 1
)
if not exist "%SCENE%" (
    echo ERROR: pingu_model.makina.json is missing. Regenerate it:
    echo    makina_povin pingu_model.pov -o pingu_model.makina.json
    exit /b 1
)

"%BIN%\render_scene.exe" --mask --width %WIDTH% --height %HEIGHT% "%SCENE%"
if errorlevel 1 (
    echo ERROR: the mask pass failed
    exit /b 1
)

pushd "%BIN%"
if not exist mask_pingu_model.pov (
    echo ERROR: render_scene wrote no mask_pingu_model.pov
    popd
    exit /b 1
)
REM -A disables antialiasing: a soft edge would blur the disagreement into the tolerance.
"%POVRAY%" +Imask_pingu_model.pov +Opov_mask_pingu_model.bmp +W%WIDTH% +H%HEIGHT% +FS -A -D -GA +L"%POVINC%" >nul 2>&1
if not exist pov_mask_pingu_model.bmp (
    echo ERROR: POV-Ray produced no image
    popd
    exit /b 1
)
popd

echo.
"%BIN%\silhouette_compare.exe" "%BIN%\mask_pingu_model.bmp" "%BIN%\pov_mask_pingu_model.bmp"
if errorlevel 1 (
    echo.
    echo ERROR: the two renderers do not draw the same pingu
    exit /b 1
)

endlocal


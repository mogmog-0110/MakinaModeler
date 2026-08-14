@echo off
REM Sphere_sweep against POV-Ray, silhouette for silhouette: the uniform cubic B-spline basis
REM is public knowledge, and this is the measurement that says it is the basis POV sweeps.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENE=%HERE%sweep_flipper.makina.json
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
if not exist "%SCENE%" (
    echo ERROR: sweep_flipper.makina.json is missing. Regenerate it:
    echo    makina_povin sweep_flipper.pov -o sweep_flipper.makina.json
    exit /b 1
)

"%BIN%\render_scene.exe" --mask --width %WIDTH% --height %HEIGHT% "%SCENE%"
if errorlevel 1 (
    echo ERROR: the mask pass failed
    exit /b 1
)

pushd "%BIN%"
if not exist mask_sweep_flipper.pov (
    echo ERROR: render_scene wrote no mask_sweep_flipper.pov
    popd
    exit /b 1
)
REM -A disables antialiasing: a soft edge would blur the disagreement into the tolerance.
"%POVRAY%" +Imask_sweep_flipper.pov +Opov_mask_sweep_flipper.bmp +W%WIDTH% +H%HEIGHT% +FS -A -D -GA +L"%POVINC%" >nul 2>&1
if not exist pov_mask_sweep_flipper.bmp (
    echo ERROR: POV-Ray produced no image
    popd
    exit /b 1
)
popd

echo.
"%BIN%\silhouette_compare.exe" "%BIN%\mask_sweep_flipper.bmp" "%BIN%\pov_mask_sweep_flipper.bmp"
if errorlevel 1 (
    echo.
    echo ERROR: the two renderers do not draw the same sweep
    exit /b 1
)

endlocal


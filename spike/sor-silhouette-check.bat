@echo off
REM Sor against POV-Ray, silhouette for silhouette: the spline (a cubic in r^2 over h with
REM the neighbours' secant slopes) came from the public reference's description, and this is
REM the measurement that says the in-between shape it produces is POV's.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENE=%HERE%sor_body.makina.json
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
    echo ERROR: sor_body.makina.json is missing. Regenerate it:
    echo    makina_povin sor_body.pov -o sor_body.makina.json
    exit /b 1
)

"%BIN%\render_scene.exe" --mask --width %WIDTH% --height %HEIGHT% "%SCENE%"
if errorlevel 1 (
    echo ERROR: the mask pass failed
    exit /b 1
)

pushd "%BIN%"
if not exist mask_sor_body.pov (
    echo ERROR: render_scene wrote no mask_sor_body.pov
    popd
    exit /b 1
)
REM -A disables antialiasing: a soft edge would blur the disagreement into the tolerance.
"%POVRAY%" +Imask_sor_body.pov +Opov_mask_sor_body.bmp +W%WIDTH% +H%HEIGHT% +FS -A -D -GA +L"%POVINC%" >nul 2>&1
if not exist pov_mask_sor_body.bmp (
    echo ERROR: POV-Ray produced no image
    popd
    exit /b 1
)
popd

echo.
"%BIN%\silhouette_compare.exe" "%BIN%\mask_sor_body.bmp" "%BIN%\pov_mask_sor_body.bmp"
if errorlevel 1 (
    echo.
    echo ERROR: the two renderers do not draw the same sor
    exit /b 1
)

endlocal


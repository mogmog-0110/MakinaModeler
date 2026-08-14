@echo off
REM Blob against POV-Ray, silhouette for silhouette: the falloff formula came from the public
REM reference, and this is the measurement that says the surface it produces is POV's.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENE=%HERE%blob_beak.makina.json
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
    echo ERROR: blob_beak.makina.json is missing. Regenerate it:
    echo    makina_povin blob_beak.pov -o blob_beak.makina.json
    exit /b 1
)

"%BIN%\render_scene.exe" --mask --width %WIDTH% --height %HEIGHT% "%SCENE%"
if errorlevel 1 (
    echo ERROR: the mask pass failed
    exit /b 1
)

pushd "%BIN%"
if not exist mask_blob_beak.pov (
    echo ERROR: render_scene wrote no mask_blob_beak.pov
    popd
    exit /b 1
)
REM -A disables antialiasing: a soft edge would blur the disagreement into the tolerance.
"%POVRAY%" +Imask_blob_beak.pov +Opov_mask_blob_beak.bmp +W%WIDTH% +H%HEIGHT% +FS -A -D -GA +L"%POVINC%" >nul 2>&1
if not exist pov_mask_blob_beak.bmp (
    echo ERROR: POV-Ray produced no image
    popd
    exit /b 1
)
popd

echo.
"%BIN%\silhouette_compare.exe" "%BIN%\mask_blob_beak.bmp" "%BIN%\pov_mask_blob_beak.bmp"
if errorlevel 1 (
    echo.
    echo ERROR: the two renderers do not draw the same blob
    exit /b 1
)

endlocal

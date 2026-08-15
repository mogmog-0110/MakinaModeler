@echo off
REM Domain warps against POV-Ray, silhouette for silhouette (PLAN.md D-14).
REM POV has no twist, bend or taper; the exporter writes each warped subtree as an isosurface
REM whose function is the field spelled in POV's own syntax. POV ray-traces that, we sphere-trace
REM the generated shader, and the outlines have to agree -- the only thing the two share is the
REM algebra of the inverse map, spelled twice.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENES=%HERE%..\makina-core\tests\scenes
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

set FAILED=0
for %%s in (twist bend taper) do (
    if not exist "%SCENES%\%%s.makina.json" (
        echo ERROR: %%s.makina.json is missing
        set FAILED=1
    ) else (
        "%BIN%\render_scene.exe" --mask --width %WIDTH% --height %HEIGHT% "%SCENES%\%%s.makina.json"
        if errorlevel 1 (
            echo ERROR: the mask pass failed for %%s
            set FAILED=1
        ) else (
            pushd "%BIN%"
            REM -A disables antialiasing: a soft edge would blur the disagreement into the tolerance.
            "%POVRAY%" +Imask_%%s.pov +Opov_mask_%%s.bmp +W%WIDTH% +H%HEIGHT% +FS -A -D -GA +L"%POVINC%" >nul 2>&1
            if not exist pov_mask_%%s.bmp (
                echo ERROR: POV-Ray produced no image for %%s
                set FAILED=1
            )
            popd
            echo.
            "%BIN%\silhouette_compare.exe" "%BIN%\mask_%%s.bmp" "%BIN%\pov_mask_%%s.bmp"
            if errorlevel 1 set FAILED=1
        )
    )
)

echo.
if "%FAILED%"=="1" (
    echo ERROR: the two renderers do not draw the same warped shape
    exit /b 1
)
echo the ray march and POV's isosurface draw the same warped shapes
endlocal

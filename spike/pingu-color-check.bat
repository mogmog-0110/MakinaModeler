@echo off
REM The whole pingu model against POV-Ray, pixel for pixel: the shaded integration measurement.
REM Every finish here went through the diffuse rescale, so this is what says the mapping is real.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM This only means anything because the renderer computes POV's finish{} and POV's pigment
REM patterns (scene_finish.hlsl). Comparing colors between two renderers that were free to invent
REM their own shading would measure nothing.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENE=%HERE%pingu_model.makina.json
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

REM One pass writes the shaded image and the .pov, from one camera and one light. Deriving either
REM twice is how this ends up measuring a camera or a lamp that moved.
REM Last run's output goes first. A scene that is skipped this time -- because it gained a
REM see-through material, say -- would otherwise still have its .pov and both .bmp on disk, and
REM the loop below would compare them and report agreement about a pair nothing produced.
del /q "%BIN%\shaded_*.pov" "%BIN%\shaded_*.bmp" "%BIN%\pov_shaded_*.bmp" >nul 2>&1

"%BIN%\render_scene.exe" --pov-match --width %WIDTH% --height %HEIGHT% "%SCENE%"
if errorlevel 1 (
    echo ERROR: the shaded pass failed
    exit /b 1
)

set PAIRS=
pushd "%BIN%"
for %%f in (shaded_*.pov) do (
    REM -A off: antialiasing would blur POV's edges and ours differently, and the difference would
    REM land in the percentile as if it were a shading disagreement.
    "%POVRAY%" +I"%%f" +O"pov_%%~nf.bmp" +W%WIDTH% +H%HEIGHT% +FS -A -D +L"%POVINC%" >nul 2>&1
    if exist "pov_%%~nf.bmp" (
        set PAIRS=!PAIRS! "%BIN%\%%~nf.bmp" "%BIN%\pov_%%~nf.bmp"
    ) else (
        echo WARNING: POV-Ray produced no image for %%f
    )
)
popd

echo.
"%BIN%\color_compare.exe" !PAIRS!
if errorlevel 1 (
    echo.
    echo ERROR: the two renderers do not put the same colors on the same surfaces
    exit /b 1
)

endlocal

@echo off
REM The camera models against POV-Ray, one scene through each.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM Only the ray generator changes between these, which is the point worth checking: a fisheye in
REM a ray marcher is four lines, and four lines can still be four lines of wrong. POV has the same
REM cameras, so each one can be held to the same pixel comparison the perspective camera passes.
REM
REM panoramic is absent on purpose. Two readings of POV's vertical mapping were tried and measured
REM at 159 and 95 against a limit of 6, so this renderer has a panoramic camera and does not claim
REM it matches POV's. Guessing at a third formula would be guessing until something passed.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENE=%HERE%..\makina-core\tests\scenes\lit.makina.json
set POVRAY=D:\sandbox\Grasp3D\povray\bin\povray.exe
set POVINC=D:\sandbox\Grasp3D\povray\include
set WIDTH=1280
set HEIGHT=720
set FAILED=0

if not exist "%POVRAY%" (
    echo ERROR: POV-Ray not found at "%POVRAY%"
    exit /b 1
)

for %%C in (perspective ortho fisheye ultra) do (
    del /q "%BIN%\shaded_lit.pov" "%BIN%\shaded_lit.bmp" "%BIN%\pov_shaded_lit.bmp" >nul 2>&1
    "%BIN%\render_scene.exe" --camera %%C --pov-match --width %WIDTH% --height %HEIGHT% "%SCENE%" >nul 2>&1
    if errorlevel 1 (
        echo    FAILED [%%C]: the render did not finish
        set FAILED=1
    ) else (
        pushd "%BIN%"
        "%POVRAY%" +Ishaded_lit.pov +Opov_shaded_lit.bmp +W%WIDTH% +H%HEIGHT% +FS -A -D +L"%POVINC%" >nul 2>&1
        popd
        echo    %%C
        "%BIN%\color_compare.exe" "%BIN%\shaded_lit.bmp" "%BIN%\pov_shaded_lit.bmp"
        if errorlevel 1 set FAILED=1
    )
)

echo.
if "%FAILED%"=="0" (
    echo    every camera frames it the way POV-Ray does
    exit /b 0
)
echo    A CAMERA DOES NOT AGREE
exit /b 1

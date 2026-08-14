@echo off
REM An imported pingu, driven the way the modeller is driven: the shapes POV brought in (sor,
REM blob, sphere_sweep) must load, draw, obey the same keys as native ones, and survive
REM save/undo byte for byte. viewport-check.bat proves the machinery on a Grasp3D scene; this
REM proves the imported vocabulary rides the same machinery.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal
set HERE=%~dp0
set EXE=%HERE%build\bin\makina_viewport.exe
set SCENE=%HERE%..\spike\pingu_model.makina.json
set OUT=%HERE%build\check
set FAILED=0

if not exist "%EXE%" (
    echo    ERROR: %EXE% is missing. Run app\build-viewport.bat first.
    exit /b 1
)
if not exist "%SCENE%" (
    echo    ERROR: %SCENE% is missing. Regenerate it: makina_povin spike\pingu_model.pov -o ...
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
del /q "%OUT%\pingu_*.json" "%OUT%\pingu_*.ppm" "%OUT%\pingu_*.log" >nul 2>&1

REM Node 5 is the Torso's Sor, 49 the Beak's Blob, 73 the first flipper's SphereSweep -- read
REM from pingu_model.makina.json, which makina_povin writes deterministically.

echo    baseline
"%EXE%" "%SCENE%" --no-shell --frames 3 --save "%OUT%\pingu_base.json" ^
    --screenshot "%OUT%\pingu_base.ppm" >"%OUT%\pingu_base.log" 2>&1
if errorlevel 1 (
    echo    FAILED: the viewport did not start on the imported scene
    exit /b 1
)
REM The picture, not just the exit code: every byte comparison below would pass on a model
REM that loads and draws nothing.
python "%HERE%lit_pixels.py" "%OUT%\pingu_base.ppm"
if errorlevel 1 (
    echo       FAILED: the imported model drew nothing
    set FAILED=1
)

call :run pingu_move   5  "W Y 3 ENTER"
call :run pingu_cancel 5  "W Y 3 ESCAPE"
call :run pingu_undo   5  "W Y 3 ENTER CTRL+Z"
call :run pingu_mute   49 "H"
call :run pingu_unmute 49 "H ALT+A H"
call :run pingu_erase  73 "DELETE"
call :run pingu_undel  73 "DELETE CTRL+Z"
call :run pingu_copy   5  "CTRL+D"

call :differs pingu_move   "moving the Sor must change the tree"
call :same    pingu_cancel "Escape must leave the imported tree untouched"
call :same    pingu_undo   "undo must restore the imported tree exactly"
call :differs pingu_mute   "muting the Blob must change the saved tree"
call :matches pingu_mute   "muted.: true" "the muted Blob must carry the flag"
call :present pingu_mute   49 "a muted Blob must stay in the tree"
call :same    pingu_unmute "muting and unmuting must land back on the original tree"
call :differs pingu_erase  "deleting the SphereSweep must remove the subtree"
call :absent  pingu_erase  73 "the deleted SphereSweep must be gone"
call :same    pingu_undel  "undo after the delete must put the sweep back"
call :differs pingu_copy   "duplicating the Sor must add a subtree"

REM Isolate over an imported shape: a view, not an edit, and a way back.
echo    isolate
call :isoshot pingu_isoBase ""
call :isoshot pingu_isoOn   "view.isolate"
call :isoshot pingu_isoBack "view.isolate view.isolate"
call :diffPic pingu_isoBase pingu_isoOn "isolating the Sor has to change the picture"
call :samePic pingu_isoBase pingu_isoBack "toggling isolate twice has to come back exactly"

echo.
if "%FAILED%"=="0" (
    echo    the imported shapes ride the same rails as the native ones
    exit /b 0
)
echo    PINGU VIEWPORT CHECK FAILED
exit /b 1

:run
echo    %~1
"%EXE%" "%SCENE%" --no-shell --select %~2 --keys "%~3" --frames 40 --save "%OUT%\%~1.json" ^
    >"%OUT%\%~1.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\%~1.log
    set FAILED=1
)
exit /b 0

:isoshot
"%EXE%" "%SCENE%" --no-shell --select 5 --actions "%~2" --frames 40 ^
    --screenshot "%OUT%\%~1.ppm" >"%OUT%\%~1.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\%~1.log
    set FAILED=1
)
exit /b 0

:differs
fc /b "%OUT%\pingu_base.json" "%OUT%\%~1.json" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~2
    set FAILED=1
)
exit /b 0

:same
fc /b "%OUT%\pingu_base.json" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~2
    set FAILED=1
)
exit /b 0

:matches
findstr /R /C:"%~2" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:absent
findstr /C:"\"id\": %~2," "%OUT%\%~1.json" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:present
findstr /C:"\"id\": %~2," "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:diffPic
fc /b "%OUT%\%~1.ppm" "%OUT%\%~2.ppm" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:samePic
fc /b "%OUT%\%~1.ppm" "%OUT%\%~2.ppm" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

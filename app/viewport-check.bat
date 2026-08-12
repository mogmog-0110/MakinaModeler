@echo off
REM Does the viewport actually edit the tree the way the keys say?
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM The viewport is the one part of Makina that ctest cannot reach -- it needs a window and a GPU,
REM so CI runs none of this. Without it, "the build is green" would say nothing about whether
REM pressing W, Y, 3, Enter moves anything.
REM
REM Scripted input (--keys) drives the same state machine the keyboard does, one key per frame,
REM and --save writes the tree out afterwards. The checks are then byte comparisons against a
REM baseline save: an edit that must not happen has to produce an identical file, which is a
REM stronger statement than "the value looks right".

setlocal
set HERE=%~dp0
set EXE=%HERE%build\bin\makina_viewport.exe
set SCENE=%HERE%..\makina-core\tests\scenes\pettobotoru.makina.json
set OUT=%HERE%build\check
set FAILED=0

if not exist "%EXE%" (
    echo    ERROR: %EXE% is missing. Run app\build-viewport.bat first.
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"
REM Last run's saves are deleted first. A run that dies before writing would otherwise be compared
REM against its own previous output, and every check would pass on a program that did nothing.
del /q "%OUT%\*.json" >nul 2>&1

REM Node 4 is the Translate carrying the neck and shoulder, y = 3.0987 in the source scene.
REM Node 15 is a Cylinder with no transform of its own, so moving it has to grow one.

echo    baseline
"%EXE%" "%SCENE%" --frames 3 --save "%OUT%\base.json" >nul 2>&1
if errorlevel 1 (
    echo    FAILED: the viewport did not start
    exit /b 1
)

call :run move   4  "W Y 3 ENTER"
call :run cancel 4  "W Y 3 ESCAPE"
call :run undo   4  "W Y 3 ENTER CTRL+Z"
call :run redo   4  "W Y 3 ENTER CTRL+Z CTRL+Y"
call :run wrap   15 "W X 2 ENTER"

call :differs move   "a committed move must change the tree"
call :same    cancel "Escape must leave the tree untouched"
call :same    undo   "undo must restore the tree exactly"
REM Against the moved tree rather than against a value: "6.0987 is in the file" would still be
REM satisfied by an undo that never undid anything.
call :sameAs  redo move "redo must land back on exactly the moved tree"
call :differs wrap   "moving an untransformed node must change the tree"
REM The dot stands in for the quote: a findstr pattern in a .bat cannot carry one. Neither string
REM appears in the baseline, so a match here is the edit and nothing else.
call :matches wrap   "x.: 2.0"  "the new node must carry the typed X"

echo.
if "%FAILED%"=="0" (
    echo    the viewport edits what the keys say
    exit /b 0
)
echo    VIEWPORT CHECK FAILED
exit /b 1

:run
echo    %~1
"%EXE%" "%SCENE%" --select %~2 --keys "%~3" --frames 40 --save "%OUT%\%~1.json" >"%OUT%\%~1.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\%~1.log
    set FAILED=1
)
exit /b 0

REM The saved tree must be byte-identical to the baseline.
:same
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\base.json" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~2
    set FAILED=1
)
exit /b 0

REM Like :same, but against another run's save instead of the baseline.
:sameAs
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\%~2.json" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:differs
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
REM fc calls a missing file "different", so the guard above has to come first or this check would
REM be satisfied by a run that crashed.
fc /b "%OUT%\base.json" "%OUT%\%~1.json" >nul 2>&1
if not errorlevel 1 (
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

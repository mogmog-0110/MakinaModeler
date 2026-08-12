@echo off
REM Everything that can say whether Makina is right, in one run.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM Order matters. The Java tools write the reference dumps that the C++ tests read, so a stale
REM dump would have the tests comparing today's code against last week's answers -- which passes,
REM and means nothing.

setlocal

set HERE=%~dp0
set FAILED=0

echo ============================================================
echo  1. Grasp3D reference dumps  (Java writes what C++ checks)
echo ============================================================
for %%t in (makescenes gsf2json sdfdump measuredump povdump) do (
    echo.
    echo --- %%t
    call "%HERE%tools\%%t\build-and-run.bat" >nul 2>&1
    if errorlevel 1 (
        echo    FAILED
        set FAILED=1
    ) else (
        echo    ok
    )
)

echo.
echo ============================================================
echo  2. makina-core
echo ============================================================
echo    round trip / SDF vs Java / measurements vs Java /
echo    POV export vs Java / SDF vs boundary / editing / commands
echo.
call "%HERE%makina-core\build-and-test.bat"
if errorlevel 1 set FAILED=1

echo.
echo ============================================================
echo  3. GPU
echo ============================================================
echo    the ray march against POV-Ray, silhouette for silhouette
echo.
call "%HERE%spike\build.bat" >nul 2>&1
if errorlevel 1 (
    echo    ERROR: the spike did not build
    set FAILED=1
) else (
    call "%HERE%spike\silhouette-check.bat"
    if errorlevel 1 set FAILED=1
)

echo.
echo ============================================================
if "%FAILED%"=="0" (
    echo  everything agrees
    exit /b 0
)
echo  SOMETHING FAILED - see above
exit /b 1

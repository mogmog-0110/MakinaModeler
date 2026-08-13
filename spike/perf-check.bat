@echo off
REM What a frame costs, measured rather than remembered.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM docs/SPIKE_PERF.md and PLAN.md R-02 carry numbers from one afternoon in August 2026 and nothing
REM re-measured them afterwards. Between then and now the shader gained materials, pigment patterns
REM with their own transforms, lights, shadows, reflection, five layers of transparency, refraction
REM and a loop that walks off a grazing surface -- every one of them per pixel. A number in a
REM document that nothing re-runs is a claim with no check behind it, which is the thing this
REM project is least willing to ship.
REM
REM This is a ceiling, not a benchmark. PLAN.md's own condition for Phase 4 is that a frame stays
REM under 33 ms while editing, so that is what it holds to. SPIKE_PERF.md measured the same binary
REM four times and saw 39.75 to 43.21 ms: anything inside five percent is not a difference this
REM machine can report, and a gate any tighter would fail on a warm afternoon rather than on a
REM regression.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENES=%HERE%..\makina-core\tests\scenes
set WIDTH=1280
set HEIGHT=720
REM The ceiling PLAN.md sets for an editable frame.
set LIMIT=33

if not exist "%BIN%\render_scene.exe" (
    echo ERROR: render_scene.exe not found. Run build.bat first.
    exit /b 1
)

echo    a frame at %WIDTH%x%HEIGHT%, best of 20 after a warm-up
echo.

REM pettobotoru is the heaviest scene here and the one R-02 quotes: 75 program nodes, and half of
REM them see-through, so it exercises the layer march the others never reach.
"%BIN%\render_scene.exe" --repeat 20 --width %WIDTH% --height %HEIGHT% ^
    "%SCENES%\pettobotoru.makina.json" "%SCENES%\hero_flange.makina.json" ^
    "%SCENES%\penrose.makina.json" > "%BIN%\perf_plain.txt" 2>&1
if errorlevel 1 (
    echo    ERROR: the shading pass did not finish
    exit /b 1
)
"%BIN%\render_scene.exe" --weathered --repeat 20 --width %WIDTH% --height %HEIGHT% ^
    "%SCENES%\pettobotoru.makina.json" > "%BIN%\perf_weathered.txt" 2>&1
if errorlevel 1 (
    echo    ERROR: the weathered pass did not finish
    exit /b 1
)

type "%BIN%\perf_plain.txt" | findstr /c:"drew in"
type "%BIN%\perf_weathered.txt" | findstr /c:"drew in"

REM The gate. findstr cannot compare numbers, so the check is for a two-digit millisecond figure of
REM 33 or more -- which is the ceiling and everything above it.
set OVER=0
for %%F in ("%BIN%\perf_plain.txt" "%BIN%\perf_weathered.txt") do (
    findstr /R /c:"drew in [3-9][3-9]\." /c:"drew in [1-9][0-9][0-9]\." %%F >nul 2>&1
    if not errorlevel 1 set OVER=1
)

echo.
if "%OVER%"=="1" (
    echo    A FRAME IS PAST THE %LIMIT% ms THE PLAN ALLOWS FOR EDITING
    exit /b 1
)
echo    every frame is inside the %LIMIT% ms the plan allows for editing
exit /b 0

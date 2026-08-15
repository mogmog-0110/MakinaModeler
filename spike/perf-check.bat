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

REM The gate lives in perf_gate.py because cmd cannot compare numbers: the findstr regex that
REM stood here matched only millisecond figures whose digits were BOTH 3-9, so 51 ms passed
REM and 73 ms was the first value that ever fired. It also tells a slow machine from a slow
REM shader: the one time it fired, every scene was 2.7x at once and a 12-commit bisect found
REM no regression -- the integrated GPU was throttled. Uniform slowness is reported, not failed.
echo.
python "%HERE%perf_gate.py" --plan %LIMIT% "%BIN%\perf_plain.txt" "%BIN%\perf_weathered.txt"
exit /b %errorlevel%

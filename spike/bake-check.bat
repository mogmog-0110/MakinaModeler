@echo off
REM The baked DXIL has to be the same bytes the modeller compiles.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM Two programs now generate the shader for a scene: render_scene, interactively, and makina_bake,
REM at import. If they ever drift, the engine draws something the modeller never showed -- and the
REM difference would be invisible until someone compared two screenshots side by side.
REM
REM Byte equality is the strongest form this check can take and it costs a file compare, so there
REM is no reason to settle for less.

setlocal enabledelayedexpansion

set HERE=%~dp0
set BIN=%HERE%build\bin
set SCENES=%HERE%..\tools\gsf2json\out
set BAKE=%HERE%build\bake

if not exist "%BIN%\makina_bake.exe" (
    echo ERROR: makina_bake.exe not found. Run build.bat first.
    exit /b 1
)

if exist "%BAKE%" rmdir /s /q "%BAKE%"
mkdir "%BAKE%"

REM The modeller's side. Its intermediate .cso is what the bake has to match, so it runs first.
set SCENE_ARGS=
for %%f in ("%SCENES%\*.makina.json") do set SCENE_ARGS=!SCENE_ARGS! "%%f"
"%BIN%\render_scene.exe" --width 320 --height 180 !SCENE_ARGS! >nul 2>&1
if errorlevel 1 (
    echo ERROR: the modeller pass failed
    exit /b 1
)

set CHECKED=0
set FAILED=0

for %%f in ("%SCENES%\*.makina.json") do (
    for %%g in ("%%~nf") do (
        set TAG=%%~ng
        REM A scene with nothing renderable produces no shader on either side; the bake refuses it
        REM and says so, which is the behaviour we want rather than an empty .cso.
        if exist "%BIN%\scene_!TAG!_ps.cso" (
            "%BIN%\makina_bake.exe" "%%f" -o "%BAKE%" >nul 2>&1
            if errorlevel 1 (
                echo   !TAG!: FAILED to bake
                set FAILED=1
            ) else (
                for %%s in (vs ps) do (
                    fc /b "%BAKE%\!TAG!.%%s.cso" "%BIN%\scene_!TAG!_%%s.cso" >nul 2>&1
                    if errorlevel 1 (
                        echo   !TAG! %%s: BYTES DIFFER
                        set FAILED=1
                    ) else (
                        set /a CHECKED+=1
                    )
                )
            )
        )
    )
)

echo.
if "%FAILED%"=="0" (
    echo   !CHECKED! shader blobs, baked and interactive are byte identical
    exit /b 0
)
echo   the bake and the modeller do not agree
exit /b 1

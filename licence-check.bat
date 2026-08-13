@echo off
REM The one claim in this project whose cost is not a wrong picture.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM PLAN.md Phase R: POV-Ray from 3.7 on is AGPLv3, so bringing its source in would make the whole
REM of Makina AGPL. The line drawn there is that only the *behaviour* is taken -- measured with
REM probes and reimplemented -- and never a line of the source.
REM
REM Every other claim here costs a wrong image when it breaks. This one costs the licence of the
REM project, and it breaks the same quiet way: someone stuck on a formula opens the POV source to
REM see how it does it, keeps a helper because it was easier than rewriting, and nothing in the
REM build says a word. So it is checked rather than only written down.
REM
REM What it looks for, in the tree this repository owns:
REM
REM   AGPL / Affero text        the licence itself, in any file
REM   POV-Ray copyright lines   the header every file of that tree carries
REM   Persistence of Vision     the project's full name, which its sources spell out
REM
REM What it cannot do: recognise a routine that was read and retyped from memory. Nothing can. The
REM defence against that is the method -- pov_filter_probe.py and pov_camera_probe.py exist because
REM the numbers were measured out of the running program rather than read out of its source, and
REM every formula in this renderer arrived that way.

setlocal enabledelayedexpansion

set HERE=%~dp0
set FOUND=0

REM The directories this repository owns. external\nlohmann is vendored JSON (MIT) and is listed
REM because a check that skipped every third-party directory would skip the one place a viral
REM licence would actually arrive.
for %%D in (makina-core spike app tools docs) do (
    for %%K in ("GNU AFFERO" "Affero General Public" "Persistence of Vision") do (
        findstr /s /i /m /c:%%K "%HERE%%%D\*" >nul 2>&1
        if not errorlevel 1 (
            echo    FAIL  %%K appears under %%D:
            findstr /s /i /m /c:%%K "%HERE%%%D\*"
            set FOUND=1
        )
    )
)

echo.
if "%FOUND%"=="1" (
    echo    SOMETHING AGPL HAS ENTERED THE TREE - THE WHOLE PROJECT INHERITS IT
    exit /b 1
)
echo    nothing AGPL in the tree: POV-Ray is an oracle, not a dependency
exit /b 0

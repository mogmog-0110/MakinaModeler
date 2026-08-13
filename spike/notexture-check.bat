@echo off
REM The claim WEATHERING.md opens with, checked mechanically.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM "No textures, no UVs, nothing baked. Every mask is a function of the distance field, so the wear
REM is not painted onto the model -- it is a consequence of the model's shape."
REM
REM That is the whole argument of Phase 4, and it is the kind of claim that stays written down long
REM after it stops being true: the day someone reaches for a lookup table to fix a stubborn mask,
REM the sentence in the document does not change. Nothing else here would notice, because a
REM textured render still renders.
REM
REM So it is checked the only way it can be. A shader that samples anything has to declare a
REM Texture2D, a Texture3D, a TextureCube or a SamplerState -- HLSL has no other way in. None of
REM them appear, and this fails the moment one does.
REM
REM It proves what it says and no more: that nothing is sampled. A permutation table written out as
REM a constant array would pass, and it would still not be a texture -- it would be arithmetic with
REM the numbers written down, which is a different argument to have.

setlocal enabledelayedexpansion

set HERE=%~dp0
set FOUND=0

for %%K in (Texture2D Texture3D TextureCube Texture2DArray SamplerState SamplerComparisonState) do (
    findstr /s /m /c:"%%K" "%HERE%shaders\*.hlsl" >nul 2>&1
    if not errorlevel 1 (
        echo    FAIL  a shader declares %%K:
        findstr /s /m /c:"%%K" "%HERE%shaders\*.hlsl"
        set FOUND=1
    )
)

echo.
if "%FOUND%"=="1" (
    echo    THE WEAR IS NO LONGER ONLY A FUNCTION OF THE SHAPE
    exit /b 1
)
echo    no shader samples anything: the wear is arithmetic on the field
exit /b 0

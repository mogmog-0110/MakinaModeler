@echo off
REM Generates the .gsf verification scenes that Grasp3D's own samples do not cover.
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.

setlocal

set JDK=C:\Program Files\Java\jdk-22.0.1\bin
set GRASP=D:\sandbox\Grasp3D
set HERE=%~dp0
set OUT=%HERE%out

if not exist "%JDK%\javac.exe" (
    echo ERROR: javac not found at "%JDK%"
    exit /b 1
)
if not exist "%GRASP%\bin" (
    echo ERROR: Grasp3D compiled classes not found at "%GRASP%\bin". Build Grasp3D first.
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

REM JOGL is needed at run time even though nothing draws: serialising a Primitive makes
REM ObjectStreamClass reflect over draw(), whose signature names com.jogamp.opengl.GL2.
set CP=%OUT%;%GRASP%\bin;%GRASP%\grasp3d_lib\*

"%JDK%\javac.exe" -encoding UTF-8 -cp "%GRASP%\bin" -d "%OUT%" ^
    "%HERE%MakeVerifyScenes.java" "%HERE%MakeHeroAsset.java"
if errorlevel 1 (
    echo ERROR: javac failed
    exit /b 1
)

"%JDK%\java.exe" -cp "%CP%" MakeVerifyScenes "%OUT%"
if errorlevel 1 (
    echo ERROR: verification scene generation failed
    exit /b 1
)

REM 5 sweep variants alongside the hero asset: the weathering demo needs a sequence where only the
REM model changes, so that what the wear does can only be attributed to the geometry.
"%JDK%\java.exe" -cp "%CP%" MakeHeroAsset "%OUT%" 5
if errorlevel 1 (
    echo ERROR: hero asset generation failed
    exit /b 1
)

echo.
echo Output: %OUT%
endlocal

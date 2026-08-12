@echo off
REM Dumps Grasp3D's SceneSdf over a lattice for every .gsf, as the reference for the C++ port.
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

REM JOGL is needed at run time even though nothing draws: deserialising a Primitive makes
REM ObjectStreamClass reflect over draw(), whose signature names com.jogamp.opengl.GL2.
set CP=%OUT%;%GRASP%\bin;%GRASP%\grasp3d_lib\*

"%JDK%\javac.exe" -encoding UTF-8 -cp "%GRASP%\bin" -d "%OUT%" "%HERE%SdfDump.java"
if errorlevel 1 (
    echo ERROR: javac failed
    exit /b 1
)

set FAILED=0
set VERIFY=%HERE%..\makescenes\out

for %%f in ("%GRASP%\*.gsf" "%VERIFY%\*.gsf") do (
    "%JDK%\java.exe" -cp "%CP%" SdfDump "%%f" "%OUT%\%%~nf.sdf.txt"
    if errorlevel 1 set FAILED=1
)

if "%FAILED%"=="1" (
    echo.
    echo ERROR: at least one dump failed
    exit /b 1
)

echo.
echo Output: %OUT%
endlocal

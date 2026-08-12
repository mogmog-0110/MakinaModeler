@echo off
REM Converts every Grasp3D .gsf scene into Makina scene JSON.
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

REM JOGL must be on the runtime classpath even though nothing here draws: deserialising a
REM Primitive makes ObjectStreamClass reflect over its declared methods, and draw() takes a
REM com.jogamp.opengl.GL2. Without the jars, readObject fails with NoClassDefFoundError.
set CP=%OUT%;%GRASP%\bin;%GRASP%\grasp3d_lib\*

"%JDK%\javac.exe" -encoding UTF-8 -cp "%GRASP%\bin" -d "%OUT%" "%HERE%Gsf2Json.java"
if errorlevel 1 (
    echo ERROR: javac failed
    exit /b 1
)

set FAILED=0
set VERIFY=%HERE%..\makescenes\out

for %%f in ("%GRASP%\*.gsf" "%VERIFY%\*.gsf") do (
    "%JDK%\java.exe" -cp "%CP%" Gsf2Json "%%f" "%OUT%\%%~nf.makina.json"
    if errorlevel 1 set FAILED=1
)

if "%FAILED%"=="1" (
    echo.
    echo ERROR: at least one conversion failed
    exit /b 1
)

echo.
echo Output: %OUT%
endlocal

@echo off
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM The shell is HTML that binds to C++ through data-m-* attributes, and the binder ignores
REM anything it does not recognise without a word. A misspelt attribute, a path written with the
REM prefix a repeat does not want, a value missing its braces -- each of those leaves a window
REM that draws perfectly and updates nothing.
REM
REM tools\shell_audit.py reads the binder's own source for the set of attributes it honours, and
REM Keymap.hpp for the actions the viewport carries out, so this tracks both rather than a copy of
REM either. What it checks and why is at the top of that file.

setlocal
set HERE=%~dp0
python "%HERE%..\tools\shell_audit.py"
if errorlevel 1 exit /b 1

REM And the checks above, checked. Each one runs against a copy of the shell with a single wound
REM in it and has to fail. Three times in this project a check has passed while catching nothing,
REM so the controls run every time rather than once on the day they were written.
python "%HERE%..\tools\shell_audit_control.py"
exit /b %errorlevel%

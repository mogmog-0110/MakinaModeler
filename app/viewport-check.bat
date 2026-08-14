@echo off
REM Does the viewport actually edit the tree the way the keys say?
REM ASCII only: cmd reads .bat as Shift-JIS and UTF-8 comments break parsing.
REM
REM The viewport is the one part of Makina that ctest cannot reach -- it needs a window and a GPU,
REM so CI runs none of this. Without it, "the build is green" would say nothing about whether
REM pressing W, Y, 3, Enter moves anything.
REM
REM Scripted input (--keys) drives the same state machine the keyboard does, one key per frame,
REM and --save writes the tree out afterwards. The checks are then byte comparisons against a
REM baseline save: an edit that must not happen has to produce an identical file, which is a
REM stronger statement than "the value looks right".

setlocal
set HERE=%~dp0
set EXE=%HERE%build\bin\makina_viewport.exe
set SCENE=%HERE%..\makina-core\tests\scenes\pettobotoru.makina.json
set OUT=%HERE%build\check
set FAILED=0

if not exist "%EXE%" (
    echo    ERROR: %EXE% is missing. Run app\build-viewport.bat first.
    exit /b 1
)

REM The keymap before the keys. Every action the build says it knows has to reach the
REM viewport, or a scripted key below could "pass" by doing nothing at all -- which is how
REM Shift and a click came to do nothing for as long as they did.
"%HERE%build\bin\keymap_audit.exe" "%HERE%viewport\main.cpp" "%HERE%..\makina-core\include\makina\Command.hpp" "%HERE%ui\shell.html"
if errorlevel 1 set FAILED=1
echo.
if not exist "%OUT%" mkdir "%OUT%"
REM Last run's saves are deleted first. A run that dies before writing would otherwise be compared
REM against its own previous output, and every check would pass on a program that did nothing.
del /q "%OUT%\*.json" "%OUT%\*.ppm" "%OUT%\*.log" >nul 2>&1

REM Node 4 is the Translate carrying the neck and shoulder, y = 3.0987 in the source scene.
REM Node 15 is a Cylinder with no transform of its own, so moving it has to grow one.

echo    baseline
"%EXE%" "%SCENE%" --no-shell --frames 3 --save "%OUT%\base.json" >nul 2>&1
if errorlevel 1 (
    echo    FAILED: the viewport did not start
    exit /b 1
)

call :run move   4  "W Y 3 ENTER"
call :run cancel 4  "W Y 3 ESCAPE"
call :run undo   4  "W Y 3 ENTER CTRL+Z"
call :run redo   4  "W Y 3 ENTER CTRL+Z CTRL+Y"
call :run wrap   15 "W X 2 ENTER"
call :run erase  13 "DELETE"
call :run copy   13 "CTRL+D"
call :run undel  13 "DELETE CTRL+Z"

REM Several at once. --select takes a list so this path can drive a selection the scripted run
REM has no mouse to build.
REM
REM 13 and 60 are the two Differences, in different branches of the tree and neither inside the
REM other. 14 sits inside 13, which is the case the topLevel rule exists for.
REM
REM Ids above 5 throughout, because the saved file writes a material as {"id": N} too and the
REM search below cannot tell the two apart. This scene has six materials, so 4 reads as present
REM after the node with that id has gone -- which is exactly how this check first "failed".
REM
REM The list is quoted: cmd splits a batch argument on a comma as readily as on a space, so
REM 13,60 unquoted arrives as two arguments and the run selects only the first.
call :run multimove "13,60" "W Y 3 ENTER"
call :run multidel  "13,60" "DELETE"
call :run multicopy "13,60" "CTRL+D"
call :run nested    "13,14" "DELETE"

REM Muting. Not hiding: the node leaves the solid, which is why the saved tree keeps it and
REM the flag rather than losing it the way a delete does. H with nothing selected brings
REM everything back, which is the only way home while there is no outliner to click a node
REM that is no longer drawn.
call :run mute   13 "H"
call :run unmute 13 "H ALT+A H"

call :differs move   "a committed move must change the tree"
call :same    cancel "Escape must leave the tree untouched"
call :same    undo   "undo must restore the tree exactly"
REM Against the moved tree rather than against a value: "6.0987 is in the file" would still be
REM satisfied by an undo that never undid anything.
call :sameAs  redo move "redo must land back on exactly the moved tree"
call :differs wrap   "moving an untransformed node must change the tree"
REM The dot stands in for the quote: a findstr pattern in a .bat cannot carry one. Neither string
REM appears in the baseline, so a match here is the edit and nothing else.
call :matches wrap   "x.: 2.0"  "the new node must carry the typed X"

call :differs erase  "delete must remove the subtree"
call :absent  erase  13 "the deleted node must be gone from the tree"
call :differs copy   "duplicate must add a subtree"

call :differs multimove "moving two nodes at once must change the tree"
call :absent  multidel  13 "deleting two must remove the first"
call :absent  multidel  60 "deleting two must remove the second"
call :differs multicopy "duplicating two must add two subtrees"

call :differs mute   "muting must change the saved tree"
call :matches mute   "muted.: true" "the muted node must carry the flag"
REM Still in the tree, unlike a delete. That is the whole difference between the two.
call :present mute   13 "a muted node must stay in the tree"
call :same    unmute "muting and then unmuting must land back on the original tree"
REM The rule, checked where it is visible: 4 contains 13, so asking to delete both must be the
REM same edit as deleting 4 alone. Without it the second delete refuses -- the first already
REM took the node -- and the whole gesture is abandoned, leaving the tree untouched.
call :absent  nested 14 "deleting a node and something inside it must remove the inner one"
call :absent  nested 13 "deleting a node and something inside it must remove the outer one"
call :same    undel  "undo after a delete must put the subtree back"

REM The live preview draws the pending edit through the interpreted pipeline; committing swaps in
REM a shader generated for the new tree. The two have to produce the same image, or the picture
REM jumps the moment the user lets go. Byte comparison, because they really are the same picture:
REM the interpreter and the generated shader agree exactly (spike\render_scene --interpret).
echo    preview
call :shot during 4 "W Y 3"
call :shot after  4 "W Y 3 ENTER"
call :samePic during after "the drag preview must match the committed picture"

REM The camera. Grasp3D keeps its interactive camera apart from the X/Y/Z-direction views and
REM its toolbar returns to it; here lookAlong() overwrites yaw and pitch, so without somewhere to
REM put the old one a glance down an axis loses the view the user framed.
REM
REM Three pictures rather than two, because "genuine came back" is only worth anything if the
REM axis view went somewhere first. If snapping to Front drew the same picture as the start, the
REM restore below would pass while doing nothing at all -- so `moved` is the control that has to
REM fail for the check to mean something, and it is asserted rather than assumed.
echo    camera
call :actshot camstart ""
call :actshot camfront "view.front"
call :actshot camback  "view.front view.genuine"
call :diffPic camstart camfront "snapping to the front view must move the camera"
call :samePic camstart camback  "Genuine must come back to the camera the axis view took over"

REM The state the shell reads, out of the running application.
REM
REM viewstate_check.cpp already proves the builders are right, which is a different claim: it says
REM the header can produce this, not that the viewport does. Between the two sits the wiring --
REM whether the tree handed over is the edited one, whether the selection is the live one -- and
REM that is exactly the seam a header test cannot see.
REM
REM Nodes 13 and 15 are selected on the way in, so a dump that does not mark them is a dump built
REM from something other than what the viewport is showing.
echo    view state
"%EXE%" "%SCENE%" --no-shell --select "13,15" --frames 6 --dump-state "%OUT%\state.json" ^
    >"%OUT%\state.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\state.log
    set FAILED=1
)
call :inState "view.status.nodes.: .87 nodes" "the node count has to reach the shell"
call :inState "id.:15,.name.:.Cylinder" "the outliner rows have to be the real tree"
call :inState "id.:15,[^{]*selected.:true" "the live selection has to reach the outliner"
call :inState "id.:14,[^{]*selected.:false" "and an unselected node must not be marked"
call :inState "Cylinder +1" "the status bar has to say what is selected"

REM The shell itself, drawn over the frame.
REM
REM Every case above passes --no-shell, and a flag that turns a feature off is also a way to stop
REM checking it. So one run keeps it and has to differ from the same run without it: if the page
REM never reached the back buffer the two pictures would be identical, which is precisely the
REM failure this is here to catch.
REM
REM 120 frames rather than 40 -- a browser process takes a moment to paint, and a picture taken
REM before the first paint agrees with the one that has no shell at all, for the wrong reason.
echo    shell
"%EXE%" "%SCENE%" --no-shell --frames 120 --screenshot "%OUT%\bare.ppm" ^
    >"%OUT%\bare.log" 2>&1
"%EXE%" "%SCENE%" --frames 120 --screenshot "%OUT%\dressed.ppm" ^
    >"%OUT%\dressed.log" 2>&1
call :diffPic bare dressed "the shell has to reach the back buffer"

REM A button on the page, all the way to the tree.
REM
REM This is the claim Phase 3 makes and the only one none of the cases above can reach: --keys
REM goes through the keymap and --actions skips the page entirely, so neither says whether the
REM toolbar is wired to anything. The click is played into the same pointer path a hand uses.
REM
REM (478,15) is the mute button. Mute rather than delete because it asks no question -- delete
REM carries data-m-confirm and would need a second click on a modal -- and because the node stays
REM in the tree afterwards, so a passing run is one where something changed and nothing vanished.
echo    button
"%EXE%" "%SCENE%" --no-shell --select "13,15" --frames 150 --save "%OUT%\noclick.json" ^
    >"%OUT%\noclick.log" 2>&1
"%EXE%" "%SCENE%" --select "13,15" --frames 150 --click "478,15" --save "%OUT%\clickmute.json" ^
    >"%OUT%\clickmute.log" 2>&1
call :diffAs clickmute noclick "the toolbar button has to reach the tree"
call :present clickmute 13 "muting must not remove the node"

REM The outliner, both ways.
REM
REM The tree already shows what is selected -- the "view state" case above proves the highlight
REM travels from C++ to the page. This is the other direction: clicking a row has to select that
REM node, and it cannot go through the viewport's own select.pick, which means "whatever is under
REM the cursor" and would fire a ray from a cursor sitting on the panel.
REM
REM Node 13 goes in selected and (100,245) is the row for node 11, so a passing run is one where
REM the selection moved from one to the other rather than merely changing.
echo    outliner
"%EXE%" "%SCENE%" --select "13" --frames 150 --click "100,245" ^
    --dump-state "%OUT%\rowpick.json" >"%OUT%\rowpick.log" 2>&1
call :inFile rowpick "id.:11,[^{]*selected.:true" "clicking a row has to select that node"
call :inFile rowpick "id.:13,[^{]*selected.:false" "and has to let go of the one before it"

REM A number typed into the property panel, all the way to the tree.
REM
REM The last of the three ways the page can reach C++ -- a button, a row, and a field -- and the
REM only one that needs a keystroke. (1200,128) is the radius of node 11, read off the rendered
REM panel rather than guessed, and the run without the typing is the thing it has to differ from.
echo    field
"%EXE%" "%SCENE%" --no-shell --select "11" --frames 150 --save "%OUT%\untyped.json" ^
    >"%OUT%\untyped.log" 2>&1
"%EXE%" "%SCENE%" --select "11" --frames 200 --click "1200,128" --text "3" ^
    --save "%OUT%\typed.json" >"%OUT%\typed.log" 2>&1
call :diffAs typed untyped "a number typed into the panel has to reach the tree"
call :present typed 11 "setting a parameter must not remove the node"

REM The shapes on the toolbar.
REM
REM Fourteen of Grasp3D's bar are these -- eight primitives, three booleans, three transforms --
REM and every one of them was drawn, labelled and dead. The keymap has no `add.` actions, so
REM shell_audit had nothing to compare them against and nothing had registered a handler.
REM
REM (42,15) is the box. The new node takes the next id, and this scene ends at 87, so 88 is the
REM one the click made.
echo    toolbar
"%EXE%" "%SCENE%" --frames 200 --click "42,15" --save "%OUT%\added.json" ^
    >"%OUT%\added.log" 2>&1
call :present added 88 "the toolbar has to be able to add a shape"

echo.
if "%FAILED%"=="0" (
    echo    the viewport edits what the keys say
    exit /b 0
)
echo    VIEWPORT CHECK FAILED
exit /b 1

:run
echo    %~1
"%EXE%" "%SCENE%" --no-shell --select %~2 --keys "%~3" --frames 40 --save "%OUT%\%~1.json" >"%OUT%\%~1.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\%~1.log
    set FAILED=1
)
exit /b 0

REM Like :run, but keeps the frame instead of the tree.
:shot
"%EXE%" "%SCENE%" --no-shell --select %~2 --keys "%~3" --frames 40 --screenshot "%OUT%\%~1.ppm" ^
    >"%OUT%\%~1.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\%~1.log
    set FAILED=1
)
exit /b 0

REM Like :shot, but driven by action names instead of keys. view.genuine has no binding in
REM either preset -- it is a toolbar control in Grasp3D and neither Maya nor Blender has the
REM concept -- so there is no key to press for it.
:actshot
"%EXE%" "%SCENE%" --no-shell --actions "%~2" --frames 40 --screenshot "%OUT%\%~1.ppm" ^
    >"%OUT%\%~1.log" 2>&1
if errorlevel 1 (
    echo       FAILED: the run did not finish - see %OUT%\%~1.log
    set FAILED=1
)
exit /b 0

REM The other direction: two pictures that must NOT match. Without it a check that compares
REM identical things passes for the wrong reason.
:diffPic
if not exist "%OUT%\%~1.ppm" (
    echo       FAILED [%~1]: no picture was written
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\%~1.ppm" "%OUT%\%~2.ppm" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

REM Like :inState, but naming which dump to look in.
:inFile
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: no view state was written
    set FAILED=1
    exit /b 0
)
findstr /R /C:"%~2" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:inState
if not exist "%OUT%\state.json" (
    echo       FAILED: no view state was written
    set FAILED=1
    exit /b 0
)
findstr /R /C:"%~1" "%OUT%\state.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED: %~2
    set FAILED=1
)
exit /b 0

:samePic
if not exist "%OUT%\%~1.ppm" (
    echo       FAILED [%~1]: no picture was written
    set FAILED=1
    exit /b 0
)
if not exist "%OUT%\%~2.ppm" (
    echo       FAILED [%~2]: no picture was written
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\%~1.ppm" "%OUT%\%~2.ppm" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

REM The saved tree must be byte-identical to the baseline.
:same
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\base.json" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~2
    set FAILED=1
)
exit /b 0

REM The other direction: two saves that must NOT match. A check that compares things which are
REM equal for the wrong reason passes just as quietly as one that works.
:diffAs
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
if not exist "%OUT%\%~2.json" (
    echo       FAILED [%~2]: nothing was saved
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\%~1.json" "%OUT%\%~2.json" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

REM Like :same, but against another run's save instead of the baseline.
:sameAs
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
fc /b "%OUT%\%~2.json" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:differs
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
REM fc calls a missing file "different", so the guard above has to come first or this check would
REM be satisfied by a run that crashed.
fc /b "%OUT%\base.json" "%OUT%\%~1.json" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~2
    set FAILED=1
)
exit /b 0

:matches
findstr /R /C:"%~2" "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

REM No node in the saved tree carries this id any more. The comma is part of the pattern because
REM "id": 13 would otherwise also match "id": 130.
:present
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
findstr /R /C:"id.: %~2," "%OUT%\%~1.json" >nul 2>&1
if errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

:absent
if not exist "%OUT%\%~1.json" (
    echo       FAILED [%~1]: nothing was saved
    set FAILED=1
    exit /b 0
)
findstr /R /C:"id.: %~2," "%OUT%\%~1.json" >nul 2>&1
if not errorlevel 1 (
    echo       FAILED [%~1]: %~3
    set FAILED=1
)
exit /b 0

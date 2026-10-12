@echo off
rem Double-click: sets up povstrip on first run, then opens the Studio in your browser
rem with the strip connected over USB. Keep this window open while you use it.
call "%~dp0povstrip.cmd" serve
pause

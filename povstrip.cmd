@echo off
rem povstrip launcher for Windows: makes a private Python environment on first run,
rem installs the tool into it, then runs the command you gave.
rem   povstrip ports | flash --serial COM5 | serve | push photo.png ...
setlocal
set "HERE=%~dp0"
if not exist "%HERE%.venv\Scripts\python.exe" (
  echo Creating .venv and installing povstrip ^(first run only^)...
  py -3 -m venv "%HERE%.venv" || python -m venv "%HERE%.venv" || goto :nopython
  "%HERE%.venv\Scripts\python.exe" -m pip install --quiet --upgrade pip
  "%HERE%.venv\Scripts\python.exe" -m pip install --quiet -e "%HERE%." || goto :fail
)
"%HERE%.venv\Scripts\python.exe" -m povstrip %*
exit /b %errorlevel%
:nopython
echo Python 3.9 or newer is needed: https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^)
exit /b 1
:fail
echo Installing the tool failed; see the messages above.
exit /b 1

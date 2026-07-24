@echo off
setlocal
cd /d "%~dp0"

if not exist ".venv_build\Scripts\python.exe" (
    echo [Error] .venv_build\Scripts\python.exe not found.
    exit /b 1
)

".venv_build\Scripts\python.exe" -m pip install -r requirements-build.txt
if errorlevel 1 exit /b 1

".venv_build\Scripts\python.exe" scripts\build_nuitka.py
exit /b %errorlevel%

@echo off
title AI Sprite

REM Try to find Python in common locations
set PYTHON_CMD=

REM Method 1: Use PATH only when it resolves to a working Python 3.10+ runtime.
REM Windows Python Manager may exist on PATH without an installed runtime.
python -c "import sys; assert sys.version_info >= (3, 10)" >nul 2>&1 && set PYTHON_CMD=python

REM Method 2: Check common install locations
if "%PYTHON_CMD%"=="" (
    if exist "C:\Users\Administrator\AppData\Local\Programs\Python\Python314\python.exe" (
        set PYTHON_CMD="C:\Users\Administrator\AppData\Local\Programs\Python\Python314\python.exe"
    )
)
if "%PYTHON_CMD%"=="" (
    if exist "C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe" (
        set PYTHON_CMD="C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe"
    )
)
if "%PYTHON_CMD%"=="" (
    if exist "C:\Users\Administrator\AppData\Local\Programs\Python\Python312\python.exe" (
        set PYTHON_CMD="C:\Users\Administrator\AppData\Local\Programs\Python\Python312\python.exe"
    )
)
if "%PYTHON_CMD%"=="" (
    if exist "C:\Python314\python.exe" (
        set PYTHON_CMD="C:\Python314\python.exe"
    )
)
if "%PYTHON_CMD%"=="" (
    if exist "C:\Python313\python.exe" (
        set PYTHON_CMD="C:\Python313\python.exe"
    )
)
if "%PYTHON_CMD%"=="" (
    if exist "C:\Python312\python.exe" (
        set PYTHON_CMD="C:\Python312\python.exe"
    )
)

if "%PYTHON_CMD%"=="" (
    echo [Error] Python not found!
    echo.
    echo Please install Python 3.10+ from https://www.python.org/downloads/
    echo During installation, CHECK the box: "Add Python to PATH"
    echo.
    echo Or, edit this file and change the PYTHON path below.
    echo.
    pause
    exit /b 1
)

echo Found Python at: %PYTHON_CMD%
echo.

REM Check all runtime dependencies used by the application.
%PYTHON_CMD% -c "import PyQt6, win32crypt, PIL, requests, psutil, edge_tts" >nul 2>&1
if %errorlevel% neq 0 (
    echo Installing dependencies...
    %PYTHON_CMD% -m pip install -r "%~dp0requirements.txt"
    if %errorlevel% neq 0 (
        echo [Error] Failed to install dependencies!
        pause
        exit /b 1
    )
)

echo Starting AI Sprite...
echo.
%PYTHON_CMD% "%~dp0main.py"
pause

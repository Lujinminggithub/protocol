@echo off
REM ============================================================
REM  Package PersonalSafer Electron app into a Windows exe (NSIS installer)
REM  Usage:  ! E:\project\safer\build-exe.cmd
REM ============================================================
setlocal
cd /d "E:\project\safer\src\electron"

set CSC_IDENTITY_AUTO_DISCOVERY=false
REM Output to a real C: drive path. The E: drive is a virtual/mapped volume
REM whose copyfile syscall rejects some Electron DLLs with EPERM.
set OUTDIR=%TEMP%\PersonalSafer-dist

REM electron-builder's app-builder.exe unpacks the nsis toolset by calling "7za"
REM from PATH; the bundled 7za lives under node_modules and isn't on PATH, so
REM point SZA_PATH at it (and add its dir to PATH as a fallback). Without this
REM app-builder fails with: exec "7za": executable file not found in %PATH%.
set SZA_DIR=E:\project\safer\src\electron\node_modules\7zip-bin\win\x64
set SZA_PATH=%SZA_DIR%\7za.exe
set PATH=%SZA_DIR%;%PATH%

echo.
echo ==================== STEP 1/4: Sign kernel driver ====================
REM Sign build\kernel\x64\Release\PersonalSafer.sys with the test cert so the
REM packaged driver passes signature checks. Requires elevation (cert lives in
REM LocalMachine store). Never package an unsigned kernel driver.
powershell -ExecutionPolicy Bypass -File "E:\project\safer\sign-driver.ps1"
if errorlevel 1 ( echo [ERROR] driver signing failed & goto :end )

echo.
echo ==================== STEP 2/4: Copy native module ====================
call npm.cmd run copy-native
if errorlevel 1 ( echo [ERROR] copy-native failed & goto :end )

echo.
echo ==================== STEP 3/4: Vite production build ====================
call npx.cmd vite build
if errorlevel 1 ( echo [ERROR] vite build failed & goto :end )

echo.
echo ==================== STEP 4/4: electron-builder (NSIS) ====================
if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
call npx.cmd electron-builder --win nsis --x64 --config electron-builder.yml -c.directories.output="%OUTDIR%"
if errorlevel 1 ( echo [ERROR] electron-builder failed & goto :end )

echo.
echo ==================== EXE BUILD SUCCEEDED ====================
echo Output dir: %OUTDIR%
dir "%OUTDIR%\*.exe" /b 2>nul
if not exist "E:\project\safer\dist" mkdir "E:\project\safer\dist"
copy /y "%OUTDIR%\*.exe" "E:\project\safer\dist\" >nul 2>nul
echo (installer also copied to E:\project\safer\dist)

:end
endlocal

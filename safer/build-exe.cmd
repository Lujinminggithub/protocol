@echo off
REM ============================================================
REM  Package PersonalSafer Electron app into a Windows exe (NSIS installer)
REM  Usage:  build-exe.cmd
REM ============================================================
setlocal EnableExtensions
set "ROOT_DIR=%~dp0"
set "ELECTRON_DIR=%ROOT_DIR%src\electron"
set "DIST_DIR=%ROOT_DIR%dist"
set "EXIT_CODE=1"
set "PUSHD_DONE="

pushd "%ELECTRON_DIR%"
if errorlevel 1 (
  echo [ERROR] Electron directory not found: %ELECTRON_DIR%
  goto :end
)
set "PUSHD_DONE=1"

set CSC_IDENTITY_AUTO_DISCOVERY=false
REM Use the system temp volume because packaging from a mapped volume can make
REM Electron DLL copies fail with EPERM.
set "OUTDIR=%TEMP%\PersonalSafer-dist"
set "PS_PACKAGE_OUTPUT_DIR=%OUTDIR%"

REM electron-builder's app-builder.exe unpacks the nsis toolset by calling "7za"
REM from PATH; the bundled 7za lives under node_modules and isn't on PATH, so
REM point SZA_PATH at it (and add its dir to PATH as a fallback). Without this
REM app-builder fails with: exec "7za": executable file not found in %PATH%.
set "SZA_DIR=%ELECTRON_DIR%\node_modules\7zip-bin\win\x64"
set "SZA_PATH=%SZA_DIR%\7za.exe"
if not exist "%SZA_PATH%" (
  echo [ERROR] 7za.exe not found: %SZA_PATH%
  echo Run npm install in %ELECTRON_DIR% first.
  goto :end
)
set "PATH=%SZA_DIR%;%PATH%"

echo.
echo ==================== STEP 1/6: Sign kernel driver ====================
REM Sign build\kernel\x64\Release\PersonalSafer.sys with the test cert so the
REM packaged driver passes signature checks. Requires elevation (cert lives in
REM LocalMachine store). Never package an unsigned kernel driver.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%ROOT_DIR%sign-driver.ps1"
if errorlevel 1 ( echo [ERROR] driver signing failed & goto :end )

echo.
echo ==================== STEP 2/6: Copy runtime artifacts ====================
call npm.cmd run copy-native
if errorlevel 1 ( echo [ERROR] copy-native failed & goto :end )

echo.
echo ==================== STEP 3/6: Verify staged driver ====================
call node.exe scripts\verify-driver-signature.js
if errorlevel 1 ( echo [ERROR] staged driver verification failed & goto :end )

echo.
echo ==================== STEP 4/6: Vite production build ====================
call npx.cmd vite build
if errorlevel 1 ( echo [ERROR] vite build failed & goto :end )

echo.
echo ==================== STEP 5/6: electron-builder (NSIS) ====================
if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
call npx.cmd electron-builder --win nsis --x64 --config electron-builder.yml -c.directories.output="%OUTDIR%"
if errorlevel 1 ( echo [ERROR] electron-builder failed & goto :end )

echo.
echo ==================== STEP 6/6: Verify packaged driver ====================
call node.exe scripts\verify-packaged-driver.js
if errorlevel 1 ( echo [ERROR] packaged driver verification failed & goto :end )

echo.
echo ==================== EXE BUILD SUCCEEDED ====================
echo Output dir: %OUTDIR%
dir "%OUTDIR%\*.exe" /b 2>nul
if not exist "%DIST_DIR%" mkdir "%DIST_DIR%"
copy /y "%OUTDIR%\*.exe" "%DIST_DIR%\" >nul
if errorlevel 1 ( echo [ERROR] installer copy failed & goto :end )
echo Installer also copied to %DIST_DIR%
set "EXIT_CODE=0"

:end
if defined PUSHD_DONE popd
endlocal & exit /b %EXIT_CODE%

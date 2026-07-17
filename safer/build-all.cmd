@echo off
setlocal
cd /d "%~dp0"

echo.
echo ==================== STEP 1/4: Build kernel driver ====================
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-kernel.ps1"
if errorlevel 1 (
  echo [ERROR] Kernel driver build failed
  goto :end
)

echo.
echo ==================== STEP 2/4: Build N-API native module ====================
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build-native.ps1"
if errorlevel 1 (
  echo [ERROR] Native module build failed
  goto :end
)

echo.
echo ==================== STEP 3/4: Copy runtime resources ====================
pushd "%~dp0src\electron"
call npm.cmd run copy-native
if errorlevel 1 (
  popd
  echo [ERROR] Runtime resource copy failed
  goto :end
)

echo.
echo ==================== STEP 4/4: Build Electron frontend ====================
call npx.cmd vite build
if errorlevel 1 (
  popd
  echo [ERROR] Electron frontend build failed
  goto :end
)
popd

echo.
echo ==================== ALL BUILDS SUCCEEDED ====================
echo Kernel driver:  %~dp0build\kernel\x64\Release\PersonalSafer.sys
echo Native module:  %~dp0src\native\build\Release\personal_safer.node
echo Frontend:       %~dp0src\electron\dist and dist-electron

:end
endlocal

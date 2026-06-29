@echo off
cd /d "%~dp0"

call build_cpp_overlay.bat
if errorlevel 1 exit /b 1

if not exist dist mkdir dist
copy /Y build\F125CppOverlay.exe dist\F125TelemetryOverlay.exe >nul

echo.
echo Created dist\F125TelemetryOverlay.exe
pause

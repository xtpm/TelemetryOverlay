@echo off
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio Build Tools were not found.
  echo Install "Build Tools for Visual Studio" with the C++ workload, then run this again.
  pause
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if "%VSINSTALL%"=="" (
  echo Visual C++ tools were not found.
  echo Open Visual Studio Installer and add "Desktop development with C++".
  pause
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not exist build mkdir build
rc /nologo /fo build\F125CppOverlay.res cpp_overlay\F125CppOverlay.rc
if errorlevel 1 (
  pause
  exit /b 1
)
cl /nologo /EHsc /std:c++17 /O2 /DUNICODE /D_UNICODE cpp_overlay\F125CppOverlay.cpp build\F125CppOverlay.res /Fo:build\F125CppOverlay.obj /Fe:build\F125CppOverlay.exe /link User32.lib Gdi32.lib Msimg32.lib Ws2_32.lib Winmm.lib
if errorlevel 1 (
  pause
  exit /b 1
)

echo Built build\F125CppOverlay.exe
pause

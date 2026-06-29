@echo off
cd /d "%~dp0"
if not exist build\F125CppOverlay.exe (
  call build_cpp_overlay.bat
)
if exist build\F125CppOverlay.exe (
  start "" build\F125CppOverlay.exe
)

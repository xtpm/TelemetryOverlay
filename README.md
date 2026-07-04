# F1 25 Telemetry Overlay

Native telemetry overlay for F1 25, built with the Win32 API.

The app listens for F1 25 UDP telemetry on `127.0.0.1:20777` and provides a small launcher for opening overlay windows while driving.

## Features

- Native Windows app
- Lightweight launcher UI
- Retrial-style HUD
- Timing strip
- Live speed, gear, RPM, rev lights, fuel, ERS, tyre, lap, sector, penalties, throttle, brake, and steering telemetry
- Movable always-on-top overlay windows
- Custom app icon

## Download / Run

Use the prebuilt executable:

```text
dist\F125TelemetryOverlay.exe
```

Or run:

```powershell
.\run_cpp_overlay.bat
```

The launcher lets you choose:

- `2025 Regulations`
- `2026 Regulations`

Close the overlay with `Ctrl + Shift + Q` by default, or change it from `set keybind` in the launcher.

## F1 25 Telemetry Settings

In F1 25, open telemetry settings and use:

```text
UDP Telemetry: On
UDP IP Address: 127.0.0.1
UDP Port: 20777
UDP Send Rate: 20Hz or 60Hz
UDP Format: 2026 Season Pack
```

Borderless windowed mode is recommended for desktop overlays.

## Build

Install Visual Studio Build Tools with:

```text
Desktop development with C++
```

Then build:

```powershell
.\build_cpp_overlay.bat
```

Create a distributable EXE:

```powershell
.\make_cpp_exe.bat
```

Output:

```text
dist\F125TelemetryOverlay.exe
```

## Source

Main source:

```text
cpp_overlay\F125CppOverlay.cpp
```

Resources:

```text
cpp_overlay\F125CppOverlay.rc
cpp_overlay\resource.h
cpp_overlay\assets\retrial_overlay.ico
```

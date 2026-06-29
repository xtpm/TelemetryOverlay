# F1 25 C++ Telemetry Overlay

Native Win32 overlay for F1 25 UDP telemetry.

## Requirements

- Windows
- F1 25 UDP telemetry enabled on `127.0.0.1:20777`
- Visual Studio Build Tools with `Desktop development with C++` if rebuilding

## Run

Use the prebuilt executable:

```powershell
dist\F125TelemetryOverlay.exe
```

The launcher can open:

- full overlay
- retrial HUD only
- timing strip only

## Build

```powershell
.\build_cpp_overlay.bat
```

To create the distributable executable:

```powershell
.\make_cpp_exe.bat
```

The output is:

```text
dist\F125TelemetryOverlay.exe
```

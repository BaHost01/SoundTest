# VirtualMic - Project Context

## Project Overview
**VirtualMic** is a Windows desktop application (C# / .NET 8 / AvaloniaUI) integrated with a custom Virtual Audio Driver (C++ / KMDF). It allows users to capture microphone input, mix it with soundboard audio in real-time, and expose the result as a native Windows recording device via a kernel-mode driver.

### Key Components
- **User-Mode App (AvaloniaUI + C#):** Handles the UI, audio capture (NAudio/WASAPI), and audio mixing.
- **Kernel-Mode Driver (C++ / KMDF):** A WDM/PortCls driver that allocates a WaveRT cyclic buffer for audio injection via IOCTLs.
- **Interop Layer:** C# communicates with the driver using `DeviceIoControl` and P/Invoke.

## ⚠️ CRITICAL SAFETY WARNINGS
- **BSOD Risk:** Any error in the kernel-mode driver (Ring 0) will cause an immediate System Crash (Blue Screen of Death).
- **Testing Mandate:** **NEVER** load a newly built driver directly on your host machine. Always test in a Virtual Machine (VM) with snapshot capabilities.
- **Driver Signing:** Windows requires drivers to be signed. For development, the OS must be in "Test Mode".

## Architecture
```text
[ USER MODE ]
AvaloniaUI -> AudioMixer -> DriverController (C#)
                                |
[ OS BOUNDARY - IOCTLs ] -------|-----------------
                                |
[ KERNEL MODE ]                 v
VirtualMic.sys (WDM Driver) -> Windows Audio Engine
```

## Building and Running
The project is designed to be managed from **Termux (Android)**, with compilation handled by **GitHub Actions**.

### Build Workflow
1.  **Code & Commit:** Modify C# (User-Mode) or C++ (Kernel-Mode) code.
2.  **Push to GitHub:** Trigger the GitHub Actions workflow.
3.  **Dual Compilation:**
    -   **WDK (Windows Driver Kit):** Used to compile `VirtualMic.sys`.
    -   **.NET 8 SDK:** Used to compile the Avalonia application.
4.  **Download Artifact:** The workflow produces a `.zip` containing both the app and the driver.

### Key Commands (Inferred)
- **Local Development:** `dotnet build` (User-mode only, if .NET is available locally).
- **CI/CD:** Handled via `.github/workflows/build.yml`.

## Development Conventions
- **Memory Safety:** In the C++ driver, use `RtlCopyMemory` with extreme caution. Always validate buffer lengths to prevent overflows.
- **Audio Specs:** The pipeline targets **48kHz, 32-bit Float, 2 Channels** (Stereo).
- **Audio Processing:** Use lock-free ring buffers where possible to minimize latency and avoid UI thread blocking.
- **UI:** AvaloniaUI for cross-platform potential, though the driver is strictly Windows-specific.

## Key Files
- `prompt.md`: Comprehensive technical specification and implementation guide.
- `warnings.md`: Critical reminders about driver testing.
- `LICENSE`: Project licensing information.
- `README.md`: Basic project entry point.

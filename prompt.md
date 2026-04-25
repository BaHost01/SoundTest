# VirtualMic — Windows Virtual Microphone & Soundboard (AvaloniaUI + C# + WDM Driver)
> **Goal:** A Windows desktop app built in C# (.NET 8) with AvaloniaUI paired with a custom Virtual Audio Driver (C++ / KMDF). The app captures a real microphone, mixes it with soundboard audio in real-time, and sends the buffers directly to the driver via kernel-mode IOCTLs. The driver exposes this stream as a native recording device selectable in Discord, OBS, and games.
> **Workflow:** Project files are created and managed from **Termux** (Android). Builds are handled entirely by **GitHub Actions**, which compiles both the .NET User-Mode stack and the MSVC/WDK Kernel-Mode driver into a single deployable .zip.
> 
> ⚠️ **CRITICAL SYSTEM WARNING:** ALWAYS ON BUILD TEST THE AUDIO DRIVER TO MAKE SURE THAT IT DOES NOT BSOD AND FUCK THE ENTIRE PC! Kernel-mode driver testing must be strictly performed in a Virtual Machine with snapshot capabilities before loading onto a host OS.
> 
## Table of Contents
 1. Architecture (User-Mode + Kernel-Mode)
 2. Kernel-Mode Driver Implementation (C++ / KMDF)
 3. User-Mode Interop (C#)
 4. Audio Pipeline & Mixer
 5. AvaloniaUI Implementation
 6. GitHub Actions — Dual Compilation (WDK + .NET)
 7. Known Limitations & Next Steps
## 1. Architecture (User-Mode + Kernel-Mode)
This topology crosses the OS boundary to eliminate the latency of third-party loopbacks (like VB-Cable) and provides low-level buffer control.
```text
[ USER MODE - Ring 3 ]
+--------------------------------------------------------------+
|                    AvaloniaUI Layer                          |
+-------+---------------------------------------------+--------+
        |                                             |
+-------v--------------------+          +-------------v------------+
|   SoundboardManager        |          |     DeviceManager        |
+-------+--------------------+          +-------------+------------+
        |                                             |
+-------v---------------------------------------------v----------+
|                        AudioMixer                              |
|          (lock-free ring buffer, gain, tanh limiter)           |
+-------+---------------------------------------------+----------+
        |                                             |
+-------v------------------+                +---------v-----------------------+
|  AudioCapture            |                |   DriverController (C# interop) |
|  (NAudio/WASAPI)         |                |   (DeviceIoControl / Mapped Mem)|
+--------------------------+                +---------+-----------------------+
                                                      |
======================== OS BOUNDARY =================|========================
                                                      |
[ KERNEL MODE - Ring 0 ]                              v
+-----------------------------------------------------------------------------+
|                      VirtualMic.sys (WDM / PortCls Driver)                  |
|                                                                             |
|  +-----------------------+      +----------------------------------------+  |
|  | IMiniportWaveRT       | ---> |  KSAUDIO_DATAFORMAT (48kHz, 32F, 2Ch)  |  |
|  +-----------------------+      +----------------------------------------+  |
|                                                                             |
|  [Hardware Abstraction] Exposes as "VirtualMic Endpoint" to Windows Audio   |
+-----------------------------------------------------------------------------+

```
## 2. Kernel-Mode Driver Implementation (C++ / KMDF)
To remove the dependency on external cables, the system requires a custom *Virtual Audio Adapter* based on the Microsoft PortCls framework. The driver allocates a cyclic buffer (WaveRT) where the C# app injects samples.
### Driver/SysvadMiniport.cpp (Kernel-Mode C++)
```cpp
#include <portcls.h>
#include "SysvadMiniport.h"

// IOCTL for receiving 32-bit Float PCM data from the C# App
#define IOCTL_VIRTUALMIC_WRITE_BUFFER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

NTSTATUS VirtualMicDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
) {
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesWritten = 0;

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_VIRTUALMIC_WRITE_BUFFER: {
            PFLOAT userBuffer = (PFLOAT)Irp->AssociatedIrp.SystemBuffer;
            ULONG bufferLength = irpSp->Parameters.DeviceIoControl.InputBufferLength;

            // Resolve the kernel-allocated RingBuffer pointer (Simulated DMA)
            PFLOAT dmaBuffer = GetWaveRtBuffer(); 
            ULONG dmaBufferSize = GetWaveRtBufferSize();
            ULONG currentWriteOffset = GetCurrentWritePosition();

            if (userBuffer && dmaBuffer) {
                // WARNING: PREVENT OVERFLOWS! 
                // ALWAYS ON BUILD TEST THE AUDIO DRIVER TO MAKE SURE THAT IT DOES NOT BSOD AND FUCK THE ENTIRE PC!
                ULONG copySize = min(bufferLength, dmaBufferSize - currentWriteOffset);
                
                // Unsafe memory copy in Ring 0. If bounds are wrong, the OS instantly crashes.
                RtlCopyMemory((PUCHAR)dmaBuffer + currentWriteOffset, userBuffer, copySize);
                
                AdvanceWritePosition(copySize);
                bytesWritten = copySize;
            } else {
                status = STATUS_INVALID_PARAMETER;
            }
            break;
        }
        default:
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = bytesWritten;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

```
## 3. User-Mode Interop (C#)
The C# application replaces WASAPI rendering (WasapiOut) with direct native P/Invoke calls (DeviceIoControl) to the compiled kernel driver.
### src/Audio/DriverController.cs
```csharp
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace VirtualMic.Audio;

public sealed class DriverController : IDisposable
{
    private const uint IOCTL_VIRTUALMIC_WRITE_BUFFER = 0x222000; // Aligned with C++ macro
    private SafeFileHandle _driverHandle;
    private bool _isOpen;

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern SafeFileHandle CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode, 
        IntPtr lpSecurityAttributes, uint dwCreationDisposition, 
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)]
    private static extern bool DeviceIoControl(
        SafeFileHandle hDevice, uint dwIoControlCode,
        float[] lpInBuffer, uint nInBufferSize,
        IntPtr lpOutBuffer, uint nOutBufferSize,
        out uint lpBytesReturned, IntPtr lpOverlapped);

    public bool Open()
    {
        // Open a handle to the installed virtual kernel driver
        _driverHandle = CreateFile(@"\\.\VirtualMicDevice", 0xC0000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
        
        if (_driverHandle.IsInvalid)
        {
            Logger.Error("[VMIC] Driver not found. Is VirtualMic.sys installed and running?");
            return false;
        }

        _isOpen = true;
        Logger.Info("[VMIC] Connected to Kernel Driver.");
        return true;
    }

    /// <summary>Writes the mixed audio block directly into the driver's kernel memory.</summary>
    public void Write(float[] samples, int frameCount)
    {
        if (!_isOpen || _driverHandle.IsInvalid) return;

        uint bytesLength = (uint)(frameCount * 2 * sizeof(float)); // Stereo calculation
        
        // P/Invoke call directly to Ring 0
        bool success = DeviceIoControl(
            _driverHandle, IOCTL_VIRTUALMIC_WRITE_BUFFER,
            samples, bytesLength,
            IntPtr.Zero, 0,
            out uint bytesReturned, IntPtr.Zero);

        if (!success)
            Logger.Warn("[VMIC] Buffer drop: I/O failure communicating with driver.");
    }

    public void Dispose()
    {
        _isOpen = false;
        _driverHandle?.Dispose();
    }
}

```
## 4. Audio Pipeline & Mixer
### src/Audio/AudioMixer.cs (Processing Snippet)
```csharp
public void Process()
{
    int needed = _blockFrames * _channels;
    Array.Clear(_mixBuf, 0, needed);

    var tmp = new float[needed];

    lock (_lock)
    {
        foreach (var src in _sources)
        {
            if (src.Muted) continue;
            int got = src.Buffer.Read(tmp, 0, needed);
            float g = src.Gain;
            
            // Loop unrolling or SIMD vectorization can be applied here for performance
            for (int i = 0; i < got; i++)
                _mixBuf[i] += tmp[i] * g;
        }
    }

    if (!MasterMuted)
    {
        float mg = MasterGain;
        for (int i = 0; i < needed; i++)
            _mixBuf[i] = MathF.Tanh(_mixBuf[i] * mg); // Tanh Soft-limiter to prevent digital distortion
    }
    else
    {
        Array.Clear(_mixBuf, 0, needed);
    }

    // Trigger event -> DriverController captures this and writes to the kernel buffer
    MixReady?.Invoke(_mixBuf, _blockFrames);
}

```
## 5. AvaloniaUI Implementation
The UI provides the control surface for both the WASAPI mic capture and the kernel driver connection.
*(AvaloniaUI XAML structure remains standard cross-platform implementation. Note: ensure your view models bind strictly to the DriverController connection states instead of previous VB-Cable endpoints).*
## 6. GitHub Actions — Dual Compilation (WDK + .NET)
Because this project compiles both a C# application and a C++ Windows Driver, the CI/CD pipeline on GitHub Actions requires the Windows Driver Kit (WDK) to be installed on the runner.
.github/workflows/build.yml
```yaml
name: Build VirtualMic & WDM Driver

on:
  push:
    branches: [ main ]
  pull_request:
    branches: [ main ]
  workflow_dispatch:

jobs:
  build:
    runs-on: windows-latest

    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Setup .NET 8
        uses: actions/setup-dotnet@v4
        with:
          dotnet-version: '8.0.x'

      - name: Add MSBuild to PATH
        uses: microsoft/setup-msbuild@v2

      - name: Install Windows Driver Kit (WDK)
        # Required to build the .sys kernel driver
        uses: neilenns/setup-wdk@v2 

      - name: Compile Virtual Audio Driver (Kernel Mode)
        run: |
          echo "⚠️ CRITICAL: ALWAYS ON BUILD TEST THE AUDIO DRIVER TO MAKE SURE THAT IT DOES NOT BSOD AND FUCK THE ENTIRE PC! ⚠️"
          
          # Build the C++ Driver Solution
          msbuild Driver/VirtualMicDriver.sln /p:Configuration=Release /p:Platform=x64
          
          # Test-sign the driver locally for development use
          signtool sign /v /fd SHA256 /a /s PrivateCertStore Driver/x64/Release/VirtualMic.sys

      - name: Publish Avalonia App (User Mode)
        run: >
          dotnet publish VirtualMic.csproj
          --configuration Release
          --runtime win-x64
          --self-contained true
          --output publish/App
          -p:PublishSingleFile=false

      - name: Package Release Suite
        shell: pwsh
        run: |
          New-Item -ItemType Directory -Force -Path publish/Release
          
          # Copy App Binaries
          Copy-Item -Recurse publish/App/* publish/Release/
          
          # Copy Kernel Driver Binaries & INF
          Copy-Item Driver/x64/Release/VirtualMic.sys publish/Release/
          Copy-Item Driver/x64/Release/VirtualMic.inf publish/Release/
          
          # Archive Everything
          Compress-Archive -Path publish/Release/* -DestinationPath VirtualMic-Suite.zip

      - name: Upload Artifact
        uses: actions/upload-artifact@v4
        with:
          name: VirtualMic-Suite
          path: VirtualMic-Suite.zip
          retention-days: 30

```
## 7. Known Limitations & Next Steps
| # | Area | Limitation / Risk | Next Step |
|---|---|---|---|
| 1 | **Kernel Stability** | Incorrect ring buffer handling in Ring 0 will cause a Blue Screen of Death. | **ALWAYS ON BUILD TEST THE AUDIO DRIVER TO MAKE SURE THAT IT DOES NOT BSOD AND FUCK THE ENTIRE PC!** Use Driver Verifier in a VM. |
| 2 | **Driver Signing** | Windows blocks unsigned .sys drivers without EV signature and WHQL validation. | Instruct users to boot into "Test Mode" or sign the package via the Microsoft Hardware Developer Portal for production. |
| 3 | **Clock Sync** | Drift between WASAPI mic clock and the virtual driver clock causes buffer underruns. | Implement a software Phase-Locked Loop (PLL) or resampling layer inside AudioMixer to compensate for sample drift. |
| 4 | **UI Threading** | Drag & Drop / RMS meters can block the main thread if not optimized. | Migrate the audio meter to a native Canvas utilizing a raw DrawingContext to decouple from the 10ms audio loop. |


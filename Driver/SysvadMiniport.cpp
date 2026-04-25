#include <portcls.h>
#include "SysvadMiniport.h"

// IOCTL for receiving 32-bit Float PCM data from the C# App
#define IOCTL_VIRTUALMIC_WRITE_BUFFER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Global state simulation for the WaveRT buffer
static FLOAT g_RingBuffer[48000 * 2]; // 1 second of stereo 48kHz
static ULONG g_WriteOffset = 0;

PFLOAT GetWaveRtBuffer() {
    return g_RingBuffer;
}

ULONG GetWaveRtBufferSize() {
    return sizeof(g_RingBuffer);
}

ULONG GetCurrentWritePosition() {
    return g_WriteOffset;
}

void AdvanceWritePosition(ULONG BytesWritten) {
    g_WriteOffset = (g_WriteOffset + BytesWritten) % sizeof(g_RingBuffer);
}

NTSTATUS VirtualMicDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
) {
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG bytesWritten = 0;

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_VIRTUALMIC_WRITE_BUFFER: {
            PFLOAT userBuffer = (PFLOAT)Irp->AssociatedIrp.SystemBuffer;
            ULONG bufferLength = irpSp->Parameters.DeviceIoControl.InputBufferLength;

            PFLOAT dmaBuffer = GetWaveRtBuffer(); 
            ULONG dmaBufferSize = GetWaveRtBufferSize();
            ULONG currentWriteOffset = GetCurrentWritePosition();

            if (userBuffer && dmaBuffer) {
                // Bounds checking
                ULONG copySize = min(bufferLength, dmaBufferSize - currentWriteOffset);
                
                // Copy to kernel buffer
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

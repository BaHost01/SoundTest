#ifndef _SYSVADMINIPORT_H_
#define _SYSVADMINIPORT_H_

#include <portcls.h>

// Function declarations for buffer management
PFLOAT GetWaveRtBuffer();
ULONG GetWaveRtBufferSize();
ULONG GetCurrentWritePosition();
void AdvanceWritePosition(ULONG BytesWritten);

// Device Control handler
NTSTATUS VirtualMicDeviceControl(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
);

#endif // _SYSVADMINIPORT_H_

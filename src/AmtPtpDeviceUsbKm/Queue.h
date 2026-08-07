// Queue definitions. Kernel-mode Driver Framework

EXTERN_C_START

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device
    );

// IoQueue events
// Registered as EvtIoInternalDeviceControl (Queue.c) - internal, not
// external/public IOCTLs - so it must use the INTERNAL_DEVICE_CONTROL
// callback type. Same signature as EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL,
// so the mismatch compiled and ran fine, but the wrong type tag here
// mislabels this for readers and any SAL-based tooling.
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL AmtPtpDeviceUsbKmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP AmtPtpDeviceUsbKmEvtIoStop;

EXTERN_C_END
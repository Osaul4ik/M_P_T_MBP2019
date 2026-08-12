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

// Registered as EvtIoDeviceControl on the SAME default queue as the
// internal-IOCTL callback above. WDF dispatches IRP_MJ_DEVICE_CONTROL
// (external, from CreateFile+DeviceIoControl - i.e. AmtPtpConfigGui) to
// this one and IRP_MJ_INTERNAL_DEVICE_CONTROL (from HIDCLASS.sys, above
// this driver in the stack) to the other; the two coexist on one queue
// with no conflict. This is the entry point for the four
// IOCTL_AMT_PTP_* control codes declared in Public.h.
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AmtPtpDeviceUsbKmEvtIoDeviceControlExternal;

EXTERN_C_END
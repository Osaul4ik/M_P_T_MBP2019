// Queue definitions. Kernel-mode Driver Framework

EXTERN_C_START

NTSTATUS
AmtPtpDeviceUsbKmQueueInitialize(
    _In_ WDFDEVICE Device
    );

// IoQueue events
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL AmtPtpDeviceUsbKmEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_STOP AmtPtpDeviceUsbKmEvtIoStop;

EXTERN_C_END
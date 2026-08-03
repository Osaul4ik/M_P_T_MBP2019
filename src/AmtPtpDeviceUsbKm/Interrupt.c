// Route USB input through PTPCore into HID reports.

#include "Driver.h"
#include "PTPCore.h"
#include "Input.h"
#include "Interrupt.tmh"

#if DBG
static VOID
AmtReportCheckInvariants(_In_ const PTP_REPORT* Report)
{
    NT_ASSERT(Report->ContactCount <= PTP_MAX_CONTACT_POINTS);

    for (UCHAR a = 0; a < Report->ContactCount; a++) {
        NT_ASSERT(Report->Contacts[a].TipSwitch == 0 || Report->Contacts[a].TipSwitch == 1);
        for (UCHAR b = (UCHAR)(a + 1); b < Report->ContactCount; b++) {
            NT_ASSERT(Report->Contacts[a].ContactID != Report->Contacts[b].ContactID);
        }
    }
}
#else
#define AmtReportCheckInvariants(Report) ((VOID)0)
#endif

// Copy core-frame contacts into the final report.
static VOID
AmtSerializeCoreFrameToReport(
    _In_  const PTP_CORE_FRAME* CoreFrame,
    _Out_ PTP_REPORT*           Report
)
{
    UCHAR n = CoreFrame->ContactCount;
    if (n > PTP_MAX_CONTACT_POINTS) n = PTP_MAX_CONTACT_POINTS;

    for (UCHAR i = 0; i < n; i++) {
        const PTP_CORE_CONTACT* c = &CoreFrame->Contacts[i];

        Report->Contacts[i].ContactID  = c->ContactID;
        Report->Contacts[i].X          = c->X;
        Report->Contacts[i].Y          = c->Y;
        Report->Contacts[i].TipSwitch  = (c->Phase == CONTACT_PHASE_UP) ? 0 : 1;
        Report->Contacts[i].Confidence = c->Confident ? 1 : 0;
    }

    Report->ContactCount = n;
}

// Queue a force-touch edge for later delivery.
static VOID
AmtForceTouchEdgeEnqueue(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _In_    BOOLEAN         Button2State
)
{
    if (pCtx->PendingForceTouchEdgeCount == PENDING_FORCE_TOUCH_EDGE_CAPACITY) {
        pCtx->PendingForceTouchEdgeHead = (UCHAR)
            ((pCtx->PendingForceTouchEdgeHead + 1) % PENDING_FORCE_TOUCH_EDGE_CAPACITY);
        pCtx->PendingForceTouchEdgeCount--;
    }

    UCHAR tail = (UCHAR)
        ((pCtx->PendingForceTouchEdgeHead + pCtx->PendingForceTouchEdgeCount)
         % PENDING_FORCE_TOUCH_EDGE_CAPACITY);

    pCtx->PendingForceTouchEdgeQueue[tail] = Button2State;
    pCtx->PendingForceTouchEdgeCount++;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(_In_ PDEVICE_CONTEXT DeviceContext)
{
    WDF_USB_CONTINUOUS_READER_CONFIG contReaderConfig;
    NTSTATUS status;
    size_t   transferLength = 0;

    // Confirm the device uses a supported packet layout.
    switch (DeviceContext->DeviceInfo->tp_type) {
    case TYPE1:
    case TYPE2:
    case TYPE3:
    case TYPE4:
    case TYPE5:
        transferLength = (size_t)DeviceContext->DeviceInfo->tp_datalen;
        break;
    default:
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    if (transferLength == 0) {
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &contReaderConfig,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    contReaderConfig.EvtUsbTargetPipeReadersFailed = AmtPtpEvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe, &contReaderConfig);

exit:
    return status;
}

VOID
AmtPtpEvtUsbInterruptPipeReadComplete(
    _In_ WDFUSBPIPE  Pipe,
    _In_ WDFMEMORY   Buffer,
    _In_ size_t      NumBytesTransferred,
    _In_ WDFCONTEXT  Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    PDEVICE_CONTEXT pCtx       = Context;
    size_t          headerSize = (unsigned int)pCtx->DeviceInfo->tp_header;
    size_t          fingerSize = (unsigned int)pCtx->DeviceInfo->tp_fsize;
    size_t          raw_n;
    UCHAR*          TouchBuffer = NULL;

    LONGLONG      PerfDelta;
    LARGE_INTEGER Now;
    NTSTATUS      Status;
    PTP_REPORT    Report;
    WDFREQUEST    Request;
    WDFMEMORY     RequestMemory;

    // USB read completion

    if (NumBytesTransferred < headerSize ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        return;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &Request);
    if (!NT_SUCCESS(Status))
        return;

    Status = WdfRequestRetrieveOutputMemory(Request, &RequestMemory);
    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
        return;
    }

    // AUDIT FIX: the continuous reader can re-enter on another CPU. Hold
    // StateLock (DISPATCH_LEVEL) across the whole state-mutating body.
    WdfSpinLockAcquire(pCtx->StateLock);

    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    // BUG FIX: KQPC's RETURN VALUE is the tick count; the out-param is the
    // FREQUENCY (constant, latched at D0Entry). Previously Now was being
    // overwritten with the frequency, so time deltas were permanently 0.
    Now = KeQueryPerformanceCounter(NULL);

    // SCANTIME FIX: Windows PTP expects a free-running, ever-increasing
    // 100us clock, and differentiates it across frames itself. Sending
    // the pre-computed inter-frame delta made consecutive values nearly
    // identical, collapsing Windows' velocity/inertia derivation to ~0.
    // Accumulate elapsed time onto a running ULONG and truncate the low
    // 16 bits into the report field (USHORT wraparound is expected).
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;
    if (pCtx->PerfFrequency.QuadPart > 0)
        PerfDelta = PerfDelta * 10000LL / pCtx->PerfFrequency.QuadPart;
    else
        PerfDelta /= 100LL;
    if (PerfDelta < 0) PerfDelta = 0;

    pCtx->ScanTimeAccumulator += (ULONG)PerfDelta;
    Report.ScanTime = (USHORT)(pCtx->ScanTimeAccumulator & 0xFFFF);
    pCtx->LastReportTime = Now;

    BOOLEAN buttonSnapshot =
        pCtx->PtpReportButton && TouchBuffer[pCtx->DeviceInfo->tp_button];

    // RawFrame construction (InputAdapter - no decisions)
    RAW_FRAME rawFrame;
    RtlZeroMemory(&rawFrame, sizeof(rawFrame));
    rawFrame.TimestampQpc = Now.QuadPart;

    if (pCtx->PtpReportTouch) {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        if (raw_n * fingerSize > (NumBytesTransferred - headerSize)) {
            WdfSpinLockRelease(pCtx->StateLock);
            WdfRequestComplete(Request, STATUS_DATA_ERROR);
            return;
        }

        UCHAR* f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;
        AmtInputParseFrame(f_base, fingerSize, raw_n, pCtx->DeviceInfo,
                           Now.QuadPart, &rawFrame);
    }
    // else: empty RawFrame -> PTPCore_ProcessFrame lifts all active contacts.

    // PTPCore orchestration
    PTP_CORE_FRAME coreFrame;
    BOOLEAN forceTouchClick   = FALSE;
    BOOLEAN buttonClickReport = FALSE;
    PTPCore_ProcessFrame(pCtx, &rawFrame, Now.QuadPart, buttonSnapshot,
                         &coreFrame, &forceTouchClick, &buttonClickReport);

    // Serialize to PTP_REPORT
    AmtSerializeCoreFrameToReport(&coreFrame, &Report);

    // Arbitrated by PTPCore, not the raw button bit: withheld while a
    // press is deciding between click and force-touch, and permanently
    // suppressed for force-touch presses (so no click fires underneath).
    if (buttonClickReport) {
        Report.IsButtonClicked = TRUE;
    }

    AmtReportCheckInvariants(&Report);

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
    if (!NT_SUCCESS(Status)) {
        WdfSpinLockRelease(pCtx->StateLock);
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, STATUS_SUCCESS);

    // Force-touch -> synthetic right-click, delivered on the SEPARATE
    // Mouse top-level collection (REPORTID_STANDARDMOUSE) - see
    // AAPL_WELLSPRING_T2_FORCETOUCH_MOUSE_TLC. This never touches the
    // PTP_REPORT/digitizer path above; it opportunistically claims a
    // second pending IOCTL_HID_READ_REPORT request off the SAME manual
    // InputQueue (mouhid.sys keeps its own read continuously queued
    // there, same as the touch/digitizer client does).
    //
    // AUDIT FIX #1: edges were dropped if no mouse request was available
    // that pass - depending on mouhid.sys's read cadence.
    // AUDIT FIX #2: latching only the latest edge lost fast down+up pairs
    // (the up overwrote the down, so Button2 never appeared to move).
    // Edges are now queued (PendingForceTouchEdgeQueue) and delivered in
    // order, one per available mouse request per completion.
    //
    // SIMPLIFICATION (2026-08-03): PTPCore now decides the whole click on
    // the release frame (OutForceTouchClick) - nothing to test here.
    if (forceTouchClick) {
        AmtForceTouchEdgeEnqueue(pCtx, TRUE);
        AmtForceTouchEdgeEnqueue(pCtx, FALSE);
    }

    if (pCtx->PendingForceTouchEdgeCount > 0) {
        BOOLEAN edgeButton2State =
            pCtx->PendingForceTouchEdgeQueue[pCtx->PendingForceTouchEdgeHead];

        WDFREQUEST mouseRequest;
        Status = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &mouseRequest);
        if (NT_SUCCESS(Status)) {
            WDFMEMORY mouseRequestMemory;
            Status = WdfRequestRetrieveOutputMemory(mouseRequest, &mouseRequestMemory);
            if (NT_SUCCESS(Status)) {
                PTP_FORCETOUCH_MOUSE_REPORT mouseReport;
                RtlZeroMemory(&mouseReport, sizeof(mouseReport));
                mouseReport.ReportID = REPORTID_STANDARDMOUSE;
                mouseReport.Button2  = edgeButton2State ? 1 : 0;

                Status = WdfMemoryCopyFromBuffer(
                    mouseRequestMemory, 0, (PVOID)&mouseReport, sizeof(mouseReport));
                if (NT_SUCCESS(Status)) {
                    WdfRequestSetInformation(mouseRequest, sizeof(mouseReport));
                }
            }
            WdfRequestComplete(mouseRequest, Status);

            // Delivered (or handed to Windows): pop it off the head.
            pCtx->PendingForceTouchEdgeHead =
                (UCHAR)((pCtx->PendingForceTouchEdgeHead + 1) % PENDING_FORCE_TOUCH_EDGE_CAPACITY);
            pCtx->PendingForceTouchEdgeCount--;
        }
        // else: no second request yet - retry on the next completion.
    }

    WdfSpinLockRelease(pCtx->StateLock);
}

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(Pipe);
    UNREFERENCED_PARAMETER(Status);
    UNREFERENCED_PARAMETER(UsbdStatus);
    return TRUE;
}
// Interrupt.c: USB completion -> RawFrame -> PTPCore_ProcessFrame -> PTP_REPORT.
// No lifecycle decisions here.

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

// Serialize PTP_CORE_FRAME to PTP_REPORT. Pure formatting.
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

// Push a new force-touch edge onto the tail of the pending-edge FIFO
// (Device.h: PendingForceTouchEdgeQueue). Edges only ever fire on a real
// down/up state change (PTPCore_ProcessFrame), so they arrive strictly
// alternating - the capacity-4 margin means this overflow path is not
// expected to be hit in practice. If it somehow is (an extremely slow
// mouhid.sys read cadence backing up several full press/release cycles),
// the OLDEST queued edge is dropped to make room: a several-cycles-stale
// edge is less useful to the user than the newest one, and dropping from
// the head keeps the queue's ordering-by-recency invariant intact for
// everything already queued.
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

    // transferLength is DeviceInfo->tp_datalen - already computed once by
    // the DATAFORMAT macro in AppleDefinition.h as HEADER_TYPEn +
    // FSIZE_TYPEn*MAX_FINGERS for the table entry's tp_type. Recomputing
    // the same formula by hand here would just be a second place that
    // formula lives, with no independent check behind it (Bcm5974ConfigTable
    // is a static compile-time table, not attacker/hardware controlled) -
    // this switch exists only to confirm tp_type is one of the enumerators
    // WdfUsbTargetPipeConfigContinuousReader's caller expects.
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

    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    // AUDIT FIX (data race): everything from here down either reads or
    // mutates shared DEVICE_CONTEXT state (LastReportTime, the whole
    // PTPCore contact pool via PTPCore_ProcessFrame, and the force-touch
    // edge queue further below) - see the StateLock comment in Device.h
    // for why this is necessary even though completions look sequential
    // on paper. Held across the WDF calls in this region too (manual,
    // non-power-managed queue - safe at DISPATCH_LEVEL).
    WdfSpinLockAcquire(pCtx->StateLock);

    KeQueryPerformanceCounter(&Now);
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;
    if (pCtx->PerfFrequency.QuadPart > 0)
        PerfDelta = PerfDelta * 10000LL / pCtx->PerfFrequency.QuadPart;
    else
        PerfDelta /= 100LL;
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    if (PerfDelta < 0)      PerfDelta = 0;
    Report.ScanTime = (USHORT)PerfDelta;
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

        // NOTE: no "raw_n * fingerSize > NumBytesTransferred - headerSize"
        // check here anymore - it was dead code. The modulo check at the
        // top of this function already guarantees (NumBytesTransferred -
        // headerSize) is an exact multiple of fingerSize, and raw_n is
        // only ever clamped DOWN from the exact quotient, so the product
        // can never exceed the transferred byte count.

        UCHAR* f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;
        AmtInputParseFrame(f_base, fingerSize, raw_n, pCtx->DeviceInfo,
                           Now.QuadPart, &rawFrame);
    }
    // else: empty RawFrame -> PTPCore_ProcessFrame lifts all active contacts.

    // PTPCore orchestration
    PTP_CORE_FRAME coreFrame;
    BOOLEAN forceTouchDownEdge = FALSE;
    BOOLEAN forceTouchUpEdge   = FALSE;
    BOOLEAN buttonClickReport  = FALSE;
    PTPCore_ProcessFrame(pCtx, &rawFrame, Now.QuadPart, buttonSnapshot,
                         &coreFrame, &forceTouchDownEdge, &forceTouchUpEdge,
                         &buttonClickReport);

    // Serialize to PTP_REPORT
    AmtSerializeCoreFrameToReport(&coreFrame, &Report);

    // Arbitrated by PTPCore (Ptpcore.c click arbitration), not the raw
    // button bit directly: withheld while a press is still deciding
    // between an ordinary click and a force touch, and permanently
    // suppressed for presses that resolve to force touch - so a force
    // touch never also reports a regular click underneath it.
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
    // AUDIT FIX #1: previously, if no second request was available this
    // exact pass, the edge was dropped outright - a reproducible way to
    // lose a force-touch click depending on mouhid.sys's read cadence.
    //
    // AUDIT FIX #2: the very first fix latched only the SINGLE most
    // recent undelivered edge ("fresh edge supersedes the stale one").
    // That loses the click just as surely when the finger presses hard
    // and releases fast enough that BOTH the down and the up edge arrive
    // before a mouse request is ever available: the up simply overwrites
    // the down in the latch, and Windows never sees Button2 move at all -
    // the whole force-touch click silently vanishes. Edges are now queued
    // (PendingForceTouchEdgeQueue, Device.h) and delivered strictly in
    // order, one per available mouse request per interrupt completion, so
    // a fast down+up still reaches Windows as two reports - possibly a
    // frame or two late - instead of cancelling out.
    if (forceTouchDownEdge) {
        AmtForceTouchEdgeEnqueue(pCtx, TRUE);
    }
    if (forceTouchUpEdge) {
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

            // Delivered (or at least handed to Windows - matches prior
            // behavior of completing the request either way): pop it off
            // the head of the queue.
            pCtx->PendingForceTouchEdgeHead =
                (UCHAR)((pCtx->PendingForceTouchEdgeHead + 1) % PENDING_FORCE_TOUCH_EDGE_CAPACITY);
            pCtx->PendingForceTouchEdgeCount--;
        }
        // else: still no second request available this pass - leave the
        // queue untouched and retry on the next interrupt completion.
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

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

    // Captured under StateLock below, delivered after it's released.
    WDFREQUEST mouseRequest     = NULL;
    BOOLEAN    haveMouseEdge    = FALSE;
    BOOLEAN    edgeButton2State = FALSE;

    // AUDIT FIX (lock-hold-time reduction, cont'd): pCtx->DeviceInfo,
    // pCtx->PtpReportTouch and pCtx->PtpReportButton are written exactly
    // once - at PrepareHardware/CreateDevice, on PASSIVE_LEVEL, before the
    // interrupt pipe's continuous reader is ever configured/started - and
    // are never mutated again for the life of the device. That means frame
    // parsing (AmtInputParseFrame: up to PTP_MAX_CONTACT_POINTS finger
    // records, clamp/normalize per contact - the single heaviest piece of
    // per-scan work here) doesn't touch anything mutable and doesn't need
    // StateLock. It's hoisted above the lock together with the QPC read
    // (a HW counter, not device state) and the button-bit snapshot. Only
    // the read-modify-write of LastReportTime and PTPCore's contact-pool
    // update below actually need to be serialized.
    KeQueryPerformanceCounter(&Now);

    BOOLEAN buttonSnapshot =
        pCtx->PtpReportButton && TouchBuffer[pCtx->DeviceInfo->tp_button];

    // RawFrame construction (InputAdapter - no decisions)
    RAW_FRAME rawFrame;
    RtlZeroMemory(&rawFrame, sizeof(rawFrame));
    rawFrame.TimestampQpc = Now.QuadPart;

    if (pCtx->PtpReportTouch) {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        // AUDIT FIX (delta not accounted for in the byte budget): raw_n
        // above is derived from (NumBytesTransferred - headerSize), but
        // finger records actually start tp_delta bytes further in
        // (f_base below) - so the real budget for finger data is
        // (NumBytesTransferred - headerSize - tp_delta). Left unguarded,
        // the last parsed finger record could read up to tp_delta bytes
        // past NumBytesTransferred - still inside the WDFMEMORY buffer
        // (sized for the full tp_datalen transfer length), but past the
        // bytes this specific completion actually delivered. Reclamp
        // raw_n against the real, delta-adjusted budget instead.
        size_t availableForFingers = NumBytesTransferred - headerSize;
        size_t delta = (size_t)pCtx->DeviceInfo->tp_delta;
        if (delta > availableForFingers) {
            raw_n = 0;
        } else {
            size_t maxFingers = (availableForFingers - delta) / fingerSize;
            if (raw_n > maxFingers) raw_n = maxFingers;
        }

        UCHAR* f_base = TouchBuffer + headerSize + pCtx->DeviceInfo->tp_delta;
        AmtInputParseFrame(f_base, fingerSize, raw_n, pCtx->DeviceInfo,
                           Now.QuadPart, &rawFrame);
    }
    // else: empty RawFrame -> PTPCore_ProcessFrame lifts all active contacts.

    // AUDIT FIX (data race): everything from here down either reads or
    // mutates shared DEVICE_CONTEXT state (LastReportTime, the whole
    // PTPCore contact pool via PTPCore_ProcessFrame, and the force-touch
    // edge queue further below) - see the StateLock comment in Device.h
    // for why this is necessary even though completions look sequential
    // on paper.
    //
    // AUDIT FIX (lock-hold-time reduction): this used to also stay held
    // across WdfMemoryCopyFromBuffer/WdfRequestComplete for BOTH the touch
    // report and the force-touch mouse report, AND across frame parsing
    // (hoisted above, see comment there). WdfRequestComplete walks the
    // completed IRP back up the whole stack (HIDCLASS, mouhid.sys, any
    // upper filters) - its duration is not bounded by this driver, so
    // running it under a DISPATCH_LEVEL spinlock on every single USB
    // interrupt completion needlessly extended how long this CPU spends at
    // raised IRQL, on the hot path, for every scan. The lock below now
    // covers ONLY the DEVICE_CONTEXT reads/writes: LastReportTime, PTPCore's
    // contact pool, and the force-touch edge FIFO push + gated pop
    // (WdfIoQueueRetrieveNextRequest for the second/mouse request stays
    // inside the lock - it's a bounded, purely-internal queue dequeue, not
    // a completion callout, and whether it succeeds gates the pop, so the
    // two must stay atomic together - see AUDIT FIX #1/#2 below). Once the
    // lock is released, Report/mouseRequest/edgeButton2State are plain
    // local state - delivering them doesn't need pCtx anymore.
    WdfSpinLockAcquire(pCtx->StateLock);

    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;
    if (pCtx->PerfFrequency.QuadPart > 0)
        PerfDelta = PerfDelta * 10000LL / pCtx->PerfFrequency.QuadPart;
    else
        PerfDelta /= 100LL;
    if (PerfDelta > 0xFFFF) PerfDelta = 0xFFFF;
    if (PerfDelta < 0)      PerfDelta = 0;
    Report.ScanTime = (USHORT)PerfDelta;
    pCtx->LastReportTime = Now;

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

    // Gated pop: WdfIoQueueRetrieveNextRequest's success/failure decides
    // whether an edge is actually consumed this pass, so the check, the
    // dequeue attempt, and the pop must stay one atomic step - this is the
    // one WDF call that stays inside the lock (see the lock-hold-time
    // comment above for why). mouseRequest/edgeButton2State are captured
    // here for delivery after the lock is released below; on failure
    // (no second request available yet) the queue is left untouched for a
    // later completion to retry, same as before.
    if (pCtx->PendingForceTouchEdgeCount > 0) {
        NTSTATUS mouseStatus = WdfIoQueueRetrieveNextRequest(pCtx->InputQueue, &mouseRequest);
        if (NT_SUCCESS(mouseStatus)) {
            edgeButton2State = pCtx->PendingForceTouchEdgeQueue[pCtx->PendingForceTouchEdgeHead];

            pCtx->PendingForceTouchEdgeHead =
                (UCHAR)((pCtx->PendingForceTouchEdgeHead + 1) % PENDING_FORCE_TOUCH_EDGE_CAPACITY);
            pCtx->PendingForceTouchEdgeCount--;

            haveMouseEdge = TRUE;
        }
        // else: still no second request available this pass - leave the
        // queue untouched and retry on the next interrupt completion.
    }

    WdfSpinLockRelease(pCtx->StateLock);
    // ---- end of critical section: pCtx is no longer touched below ----

    AmtReportCheckInvariants(&Report);

    // Touch/digitizer report delivery - independent of the mouse edge
    // below (a copy failure here, essentially unreachable in practice
    // since RequestMemory's size is guaranteed by the IOCTL_HID_READ_REPORT
    // contract, must not block delivering an already-dequeued force-touch
    // edge - the two requests are unrelated once off InputQueue).
    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
    if (NT_SUCCESS(Status)) {
        WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    }
    WdfRequestComplete(Request, Status);

    if (haveMouseEdge) {
        NTSTATUS  mouseCompleteStatus;
        WDFMEMORY mouseRequestMemory;

        mouseCompleteStatus = WdfRequestRetrieveOutputMemory(mouseRequest, &mouseRequestMemory);
        if (NT_SUCCESS(mouseCompleteStatus)) {
            PTP_FORCETOUCH_MOUSE_REPORT mouseReport;
            RtlZeroMemory(&mouseReport, sizeof(mouseReport));
            mouseReport.ReportID = REPORTID_STANDARDMOUSE;
            mouseReport.Button2  = edgeButton2State ? 1 : 0;

            mouseCompleteStatus = WdfMemoryCopyFromBuffer(
                mouseRequestMemory, 0, (PVOID)&mouseReport, sizeof(mouseReport));
            if (NT_SUCCESS(mouseCompleteStatus)) {
                WdfRequestSetInformation(mouseRequest, sizeof(mouseReport));
            }
        }
        WdfRequestComplete(mouseRequest, mouseCompleteStatus);
    }
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

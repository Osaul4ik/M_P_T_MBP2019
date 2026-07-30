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
    // BUG FIX: KeQueryPerformanceCounter's RETURN VALUE is the current tick
    // count; the out-param (NULL here - pCtx->PerfFrequency was already
    // latched once in D0Entry, see Device.c) is the counter FREQUENCY,
    // which is constant for the life of the device. The previous code
    // discarded the return value and overwrote Now with the frequency
    // instead, so Now.QuadPart never advanced between interrupt
    // completions - every dtTicks/PerfDelta/elapsedTicks computed from it
    // downstream (velocity classification, the 700ms retap window, the
    // force-touch arbitration grace timer, Match.c's time-domain contact
    // rejection) was permanently 0.
    Now = KeQueryPerformanceCounter(NULL);

    BOOLEAN buttonSnapshot =
        pCtx->PtpReportButton && TouchBuffer[pCtx->DeviceInfo->tp_button];

    // DIAG (spurious buttonClickEdge investigation): buttonSnapshot comes
    // straight from a fixed offset (tp_button) into the raw HID report,
    // completely independent of finger-tracking logic - if it's going
    // TRUE without a real physical/force click (confirmed: user felt no
    // click during the "hold + add second finger" repro), the cause is
    // either (a) genuine firmware behavior - T2 Force Touch trackpads
    // threshold on *summed* force across all fingers, so a second finger
    // landing on an already-resting first finger can cross the click
    // threshold without either finger feeling individually "hard-pressed"
    // - or (b) tp_button pointing at the wrong byte, misread once a
    // second finger's record is present. Printing the exact byte at
    // tp_button plus its immediate neighbors (offset-by-one is the usual
    // symptom of (b)) alongside NumBytesTransferred/raw finger count
    // distinguishes the two: a firmware/threshold cause should show a
    // clean, plausible button-byte value (0/1-ish) that flips exactly
    // when total pressure crosses some level; a wrong-offset cause should
    // show a byte that only "looks like a click" by coincidence (e.g.
    // tracks X/Y/pressure data from the second finger instead) and won't
    // correlate with anything the user is actually pressing.
    // Remove once the spurious-click investigation is resolved.
    {
        LONG btnOff = pCtx->DeviceInfo->tp_button;
        UCHAR bPrev = (btnOff > 0) ? TouchBuffer[btnOff - 1] : 0xFF;
        UCHAR bCur  = TouchBuffer[btnOff];
        UCHAR bNext = (size_t)(btnOff + 1) < NumBytesTransferred
                        ? TouchBuffer[btnOff + 1] : 0xFF;
        DbgPrint("[AmtPtp] BTN raw off=%ld prev=0x%02x cur=0x%02x next=0x%02x "
                 "snapshot=%u xferBytes=%Iu qpc=%I64d\n",
                 btnOff, bPrev, bCur, bNext, buttonSnapshot,
                 NumBytesTransferred, Now.QuadPart);
    }

    // RawFrame construction (InputAdapter - no decisions)
    RAW_FRAME rawFrame;
    RtlZeroMemory(&rawFrame, sizeof(rawFrame));
    rawFrame.TimestampQpc = Now.QuadPart;

    if (pCtx->PtpReportTouch) {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        // REVERTED (regression, confirmed on real MacBookPro16,1/T2
        // PID 0x0340 hardware): a prior "AUDIT FIX" here additionally
        // reclamped raw_n to (NumBytesTransferred - headerSize - tp_delta)
        // / fingerSize, on the theory that tp_delta bytes are extra wire
        // payload that must come out of the finger-data budget. That's
        // wrong for this hardware's actual transfer shape - with
        // USBD_SHORT_TRANSFER_OK (continuous reader config), the device
        // sends exactly headerSize + N*fingerSize bytes for N active
        // contacts (confirmed by the modulo check above: it requires
        // (NumBytesTransferred - headerSize) to be an exact multiple of
        // fingerSize, which only holds if tp_delta contributes ZERO extra
        // transferred bytes). tp_delta is purely a f_base pointer offset
        // (below) into memory already accounted for by headerSize, not
        // additional payload. Subtracting it from an exact multiple of
        // fingerSize before dividing floors the result down by one
        // whenever 0 < tp_delta < fingerSize (true here: delta=2,
        // fingerSize=30) - N=1 -> maxFingers=0 (finger dropped entirely),
        // N=2 -> 1, N=3 -> 2. f_base below does read tp_delta bytes past
        // NumBytesTransferred for the LAST finger record - that part is
        // real, but bounded-safe (WDFMEMORY is allocated for the full
        // tp_datalen transfer length regardless of actual bytes received)
        // and only touches TRACKPAD_FINGER's trailing field, not
        // major/minor/abs_x/abs_y (all read from earlier, always-valid
        // offsets). Reducing the reported finger COUNT is the wrong fix
        // for that - do not reintroduce this reclamp.

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

    // DIAG (soft-tap/double-tap investigation): ScanTime (100ns units,
    // clamped to USHORT) is exactly the field the QPC fix changed - it
    // was permanently 0 before. Windows' PTP recognizer uses it to judge
    // report cadence; if it's now reading absurdly large (clock unit
    // mismatch) or still stuck at 0, that's visible here. Remove once
    // the soft-double-tap report is resolved.
    DbgPrint("[AmtPtp] ScanTime=%u qpc=%I64d\n", Report.ScanTime, Now.QuadPart);

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
    //
    // AUDIT FIX (hot-path copy): WdfMemoryCopyFromBuffer re-validates
    // Offset/Length against the memory object's tracked size on every call,
    // on top of the framework's own object-handle checks - overhead paid
    // per interrupt completion for a size that's fixed at compile time.
    // WdfMemoryGetBuffer hands back the raw pointer + actual size once;
    // the size check below is the same safety guarantee
    // WdfMemoryCopyFromBuffer would have enforced, just done directly.
    {
        size_t bufferSize = 0;
        PVOID  buffer = WdfMemoryGetBuffer(RequestMemory, &bufferSize);
        if (buffer != NULL && bufferSize >= sizeof(PTP_REPORT)) {
            RtlCopyMemory(buffer, &Report, sizeof(PTP_REPORT));
            WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
            Status = STATUS_SUCCESS;
        } else {
            Status = STATUS_BUFFER_TOO_SMALL;
        }
    }
    // AUDIT FIX (scheduling latency): IO_NO_INCREMENT (plain
    // WdfRequestComplete) leaves the thread that's waiting on this IRP
    // (HIDCLASS / whatever ultimately reads the digitizer input) at its
    // normal priority, same as any other completed I/O. mouclass.sys/
    // kbdclass.sys boost their completions for exactly this reason - the
    // consumer of a fresh input sample should get scheduled promptly, not
    // wait its ordinary turn. This changes nothing about the data or the
    // locking above - just how eagerly the waiting thread gets to run
    // once the report is ready.
    WdfRequestCompleteWithPriorityBoost(Request, Status, IO_MOUSE_INCREMENT);

    if (haveMouseEdge) {
        NTSTATUS  mouseCompleteStatus;
        WDFMEMORY mouseRequestMemory;

        mouseCompleteStatus = WdfRequestRetrieveOutputMemory(mouseRequest, &mouseRequestMemory);
        if (NT_SUCCESS(mouseCompleteStatus)) {
            PTP_FORCETOUCH_MOUSE_REPORT mouseReport;
            RtlZeroMemory(&mouseReport, sizeof(mouseReport));
            mouseReport.ReportID = REPORTID_STANDARDMOUSE;
            mouseReport.Button2  = edgeButton2State ? 1 : 0;

            size_t mouseBufferSize = 0;
            PVOID  mouseBuffer = WdfMemoryGetBuffer(mouseRequestMemory, &mouseBufferSize);
            if (mouseBuffer != NULL && mouseBufferSize >= sizeof(mouseReport)) {
                RtlCopyMemory(mouseBuffer, &mouseReport, sizeof(mouseReport));
                WdfRequestSetInformation(mouseRequest, sizeof(mouseReport));
                mouseCompleteStatus = STATUS_SUCCESS;
            } else {
                mouseCompleteStatus = STATUS_BUFFER_TOO_SMALL;
            }
        }
        WdfRequestCompleteWithPriorityBoost(mouseRequest, mouseCompleteStatus, IO_MOUSE_INCREMENT);
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
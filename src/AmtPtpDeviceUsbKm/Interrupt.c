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

// Queue one force-touch click (a full down+up pulse) for later delivery.
//
// Never touches ForceTouchDeliveryState directly - a click that arrives
// while one is already in flight (or queued) just waits its turn. Only
// clicks that haven't started delivering anything are ever dropped on
// overflow, so a delivered DOWN can never be left without its UP (see the
// Device.h field comment for the failure mode this replaced).
static VOID
AmtForceTouchClickEnqueue(_Inout_ PDEVICE_CONTEXT pCtx)
{
    if (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_IDLE &&
        pCtx->PendingForceTouchClickCount == 0) {
        // Nothing in flight and nothing queued - start immediately.
        pCtx->ForceTouchDeliveryState = FORCE_TOUCH_DELIVERY_DOWN_PENDING;
        return;
    }

    if (pCtx->PendingForceTouchClickCount < PENDING_FORCE_TOUCH_CLICK_CAPACITY) {
        pCtx->PendingForceTouchClickCount++;
    }
    // else: click storm far beyond anything a human can produce - drop the
    // newest one. Safe: it never started delivering, so nothing is left
    // half-sent.
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

    // OPTIMIZATION: validate tp_fsize here, once, at PASSIVE_LEVEL setup
    // time - not on every single USB interrupt completion. tp_fsize never
    // changes for the life of this DeviceContext (it's a field copied from
    // the static Bcm5974ConfigTable entry chosen once in
    // AmtPtpGetDeviceConfig), so re-checking "!= 0" on every completion was
    // paying a branch, every ~8-16ms, for a value that can only ever be
    // wrong here - at setup - if a future table entry is misconfigured.
    // Catching that here also fails the bind with a clear status instead
    // of silently no-op'ing every future completion forever.
    if (transferLength == 0 || DeviceContext->DeviceInfo->tp_fsize == 0) {
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

    // fingerSize/headerSize come from the static config entry latched once
    // for this device (see AmtPtpGetDeviceConfig / D0Entry) and can't
    // change during a session. The fingerSize==0 case is now rejected once
    // at setup time (AmtPtpConfigContReaderForInterruptEndPoint) instead of
    // being re-checked on every completion - NT_ASSERT still catches a
    // regression there in debug builds.
    NT_ASSERT(fingerSize != 0);

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

    // OPTIMIZATION: PerfFrequency (and therefore which formula applies) is
    // fixed for the life of a D0 session - see Device.c's D0Entry, which
    // now folds both the normal case and the "no usable clock" fallback
    // into the same precomputed ScanTimeScaleQ16 (field comment in
    // Device.h has the derivation). Branching on PerfFrequency here, every
    // completion, tested a value that can only change across a D0Exit/
    // D0Entry cycle - i.e. never within this routine's lifetime per session.
    PerfDelta = (PerfDelta * pCtx->ScanTimeScaleQ16) >> 16;
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

        // AUDIT: raw_n is floor(D/fingerSize), optionally clamped down to
        // PTP_MAX_CONTACT_POINTS - either way raw_n*fingerSize can never
        // exceed D, so the runtime check that used to sit here could never
        // trigger (dead code, no actual protection). Kept as a debug-only
        // invariant instead of dropping the guarantee's documentation
        // entirely.
        NT_ASSERT(raw_n * fingerSize <= (NumBytesTransferred - headerSize));

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

    //
    // Optional live monitor snapshot. This is deliberately after PTPCore
    // classification/serialization so the GUI sees the same stable contacts
    // that the driver is about to report to Windows. When LiveEnabled is
    // FALSE this whole block is skipped: no snapshot copy and no additional
    // per-contact work is performed in the normal/idle path.
    //
    if (pCtx->LiveEnabled) {
        ULONG i;

        pCtx->LiveSequence++;

        RtlZeroMemory(&pCtx->LiveFrame, sizeof(pCtx->LiveFrame));
        pCtx->LiveFrame.StructVersion = AMT_LIVE_FRAME_VERSION;
        pCtx->LiveFrame.Sequence = pCtx->LiveSequence;
        pCtx->LiveFrame.TimestampQpc = Now.QuadPart;
        pCtx->LiveFrame.ContactCount = coreFrame.ContactCount;
        pCtx->LiveFrame.RawContactCount = rawFrame.ContactCount;
        pCtx->LiveFrame.LargePalmBlanked = coreFrame.LargePalmBlanked ? 1 : 0;
        pCtx->LiveFrame.ButtonDown = buttonSnapshot ? 1 : 0;
        pCtx->LiveFrame.ForceTouchClick = forceTouchClick ? 1 : 0;
        pCtx->LiveFrame.ButtonClickReport = buttonClickReport ? 1 : 0;

        for (i = 0;
             i < coreFrame.ContactCount && i < AMT_LIVE_MAX_CONTACTS;
             ++i) {
            pCtx->LiveFrame.Contacts[i].ContactID =
                coreFrame.Contacts[i].ContactID;
            pCtx->LiveFrame.Contacts[i].X =
                coreFrame.Contacts[i].X;
            pCtx->LiveFrame.Contacts[i].Y =
                coreFrame.Contacts[i].Y;
            pCtx->LiveFrame.Contacts[i].Phase =
                (ULONG)coreFrame.Contacts[i].Phase;
            pCtx->LiveFrame.Contacts[i].Confident =
                coreFrame.Contacts[i].Confident ? 1 : 0;
            pCtx->LiveFrame.Contacts[i].PalmSuspect =
                coreFrame.Contacts[i].PalmSuspect ? 1 : 0;
        }
    }

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
    //
    // REWORK (stuck-button fix): a flat edge ring evicted its OLDEST raw
    // edge on overflow, which could discard a click's UP after its DOWN
    // had already been delivered - Button2 would then latch down forever.
    // Delivery is now a DOWN/UP state machine (ForceTouchDeliveryState)
    // for the one click in flight, backed by a queue of not-yet-started
    // click counts (PendingForceTouchClickCount) that's always safe to
    // trim - see the Device.h field comment.
    //
    // SIMPLIFICATION (2026-08-03): PTPCore now decides the whole click on
    // the release frame (OutForceTouchClick) - nothing to test here.
    if (forceTouchClick) {
        AmtForceTouchClickEnqueue(pCtx);
    }

    if (pCtx->ForceTouchDeliveryState != FORCE_TOUCH_DELIVERY_IDLE) {
        BOOLEAN edgeButton2State =
            (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_DOWN_PENDING);

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

            // DOWN just went out -> now the UP for this SAME click is owed
            // and must be delivered before anything else starts. UP just
            // went out -> this click is fully done; only now may the next
            // queued click (if any) begin its own DOWN.
            if (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_DOWN_PENDING) {
                pCtx->ForceTouchDeliveryState = FORCE_TOUCH_DELIVERY_UP_PENDING;
            } else {
                pCtx->ForceTouchDeliveryState = FORCE_TOUCH_DELIVERY_IDLE;
                if (pCtx->PendingForceTouchClickCount > 0) {
                    pCtx->PendingForceTouchClickCount--;
                    pCtx->ForceTouchDeliveryState = FORCE_TOUCH_DELIVERY_DOWN_PENDING;
                }
            }
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
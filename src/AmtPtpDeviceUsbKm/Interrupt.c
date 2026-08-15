// Route USB input through PTPCore into HID reports.

#include "Driver.h"
#include "PTPCore.h"
#include "Input.h"

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

// Queue ClickCount force-touch clicks (each a full down+up pulse) for later
// delivery. ClickCount is 1 for every AMT_POINTER_ACTION_* except
// DOUBLE_CLICK, which needs two independent down+up pulses (Button1 x2) to
// read as a double-click to Windows - reusing this same one-click-at-a-time
// queue instead of inventing a separate multi-click delivery path.
//
// Never touches ForceTouchDeliveryState directly - a click that arrives
// while one is already in flight (or queued) just waits its turn. Only
// clicks that haven't started delivering anything are ever dropped on
// overflow, so a delivered DOWN can never be left without its UP (see the
// Device.h field comment for the failure mode this replaced).
static VOID
AmtForceTouchClickEnqueue(_Inout_ PDEVICE_CONTEXT pCtx, _In_ UCHAR ClickCount)
{
    for (UCHAR i = 0; i < ClickCount; i++) {
        if (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_IDLE &&
            pCtx->PendingForceTouchClickCount == 0) {
            // Nothing in flight and nothing queued - start immediately.
            pCtx->ForceTouchDeliveryState = FORCE_TOUCH_DELIVERY_DOWN_PENDING;
            continue;
        }

        if (pCtx->PendingForceTouchClickCount < PENDING_FORCE_TOUCH_CLICK_CAPACITY) {
            pCtx->PendingForceTouchClickCount++;
        }
        // else: click storm far beyond anything a human can produce - drop
        // the newest one. Safe: it never started delivering, so nothing is
        // left half-sent.
    }
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
    size_t          tpDelta    = (unsigned int)pCtx->DeviceInfo->tp_delta;
    size_t          raw_n      = 0;
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
        pCtx->DeviceInfo->tp_button >= NumBytesTransferred ||
        tpDelta > (NumBytesTransferred - headerSize) ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        return;
    }

    // Precompute AND fully validate the raw finger-record block here, before
    // the request is dequeued and before StateLock is taken. This keeps
    // every "malformed/short USB packet" rejection in this function a
    // trivial, side-effect-free `return` - there is no path below this
    // point that can bail out while an IRP has been pulled off InputQueue
    // or while StateLock is held.
    //
    // REGRESSION FIX: the previous in-lock check compared tp_delta against
    // (NumBytesTransferred - headerSize - (raw_n-1)*fingerSize - fingerSize),
    // i.e. against the *remainder* left over after raw_n whole finger
    // records. That remainder is 0 whenever the packet divides evenly by
    // fingerSize (the normal case once raw_n is clamped to
    // PTP_MAX_CONTACT_POINTS) - so on every TYPE4/TYPE5 (T2) device, where
    // tp_delta is non-zero, the check was "tp_delta > 0", which is always
    // true and rejected every single valid T2 packet. Worse, that `return`
    // sat between WdfSpinLockAcquire(pCtx->StateLock) and its Release, so it
    // abandoned the lock (and the already-dequeued Request) permanently -
    // the very next completion then deadlocked re-acquiring the same lock.
    // TYPE2/TYPE3 (non-T2, e.g. MacBook Air) devices have tp_delta == 0, so
    // "0 > 0" was always false there and the bug never triggered - which is
    // exactly why this only ever BSOD'd on T2 hardware.
    //
    // The correct check is simply: does the buffer actually contain
    // header + tp_delta + raw_n whole finger records? That is exactly the
    // span AmtInputParseFrame is about to read via f_base below.
    if (pCtx->PtpReportTouch) {
        size_t requiredLength;

        raw_n = (NumBytesTransferred - headerSize) / fingerSize;
        if (raw_n > PTP_MAX_CONTACT_POINTS) raw_n = PTP_MAX_CONTACT_POINTS;

        requiredLength = headerSize + tpDelta + raw_n * fingerSize;
        if (requiredLength > NumBytesTransferred) {
            return;
        }
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
        // raw_n/tpDelta were already computed and bounds-checked against
        // NumBytesTransferred above, before StateLock was taken - nothing
        // in this branch can fail or return.
        UCHAR* f_base = TouchBuffer + headerSize + tpDelta;
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

    // Capture the force-touch queue state while StateLock is held. The report
    // itself is already a local value, so copying it into the HID request can
    // happen after the lock is released.
    BOOLEAN needMouseDelivery =
        forceTouchClick ||
        (pCtx->ForceTouchDeliveryState != FORCE_TOUCH_DELIVERY_IDLE);

    // The main state lock is no longer needed. Request-buffer copying and
    // completion are both outside the spinlock so the DISPATCH_LEVEL
    // critical section contains only driver-owned shared-state work.
    WdfSpinLockRelease(pCtx->StateLock);

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory, 0, (PVOID)&Report, sizeof(PTP_REPORT));
    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(Request, sizeof(PTP_REPORT));
    WdfRequestComplete(Request, STATUS_SUCCESS);

    // Optional live monitor snapshot. This is intentionally outside StateLock:
    // coreFrame/rawFrame are local immutable results at this point, and the
    // dedicated LiveLock protects only the live-monitor state.
    WdfSpinLockAcquire(pCtx->LiveLock);
    if (pCtx->LiveEnabled) {
        ULONG i;
        ULONG liveIndex = ((ULONG)InterlockedCompareExchange(
            &pCtx->LiveFrameIndex, 0, 0)) ^ 1u;
        PAMT_LIVE_FRAME liveFrame = &pCtx->LiveFrame[liveIndex & 1u];

        pCtx->LiveSequence++;

        RtlZeroMemory(liveFrame, sizeof(*liveFrame));
        liveFrame->StructVersion = AMT_LIVE_FRAME_VERSION;
        liveFrame->Sequence = pCtx->LiveSequence;
        liveFrame->TimestampQpc = Now.QuadPart;
        liveFrame->ContactCount = coreFrame.ContactCount;
        liveFrame->RawContactCount = rawFrame.ContactCount;
        liveFrame->LargePalmBlanked = coreFrame.LargePalmBlanked ? 1 : 0;
        liveFrame->ButtonDown = buttonSnapshot ? 1 : 0;
        liveFrame->ForceTouchClick = forceTouchClick ? 1 : 0;
        liveFrame->ButtonClickReport = buttonClickReport ? 1 : 0;

        for (i = 0;
             i < coreFrame.ContactCount && i < AMT_LIVE_MAX_CONTACTS;
             ++i) {
            ULONG j;
            ULONG bestRawIndex = 0;
            ULONGLONG bestDistance = ~0ULL;
            BOOLEAN haveRawMatch = FALSE;

            liveFrame->Contacts[i].ContactID =
                coreFrame.Contacts[i].ContactID;
            liveFrame->Contacts[i].X =
                coreFrame.Contacts[i].X;
            liveFrame->Contacts[i].Y =
                coreFrame.Contacts[i].Y;
            liveFrame->Contacts[i].Phase =
                (ULONG)coreFrame.Contacts[i].Phase;
            liveFrame->Contacts[i].Confident =
                coreFrame.Contacts[i].Confident ? 1 : 0;
            liveFrame->Contacts[i].PalmSuspect =
                coreFrame.Contacts[i].PalmSuspect ? 1 : 0;

            for (j = 0; j < rawFrame.ContactCount; ++j) {
                LONGLONG dx = (LONGLONG)coreFrame.Contacts[i].X -
                               (LONGLONG)rawFrame.Contacts[j].X;
                LONGLONG dy = (LONGLONG)coreFrame.Contacts[i].Y -
                               (LONGLONG)rawFrame.Contacts[j].Y;
                ULONGLONG distance = (ULONGLONG)(dx * dx + dy * dy);

                if (!haveRawMatch || distance < bestDistance) {
                    haveRawMatch = TRUE;
                    bestDistance = distance;
                    bestRawIndex = j;
                }
            }

            if (haveRawMatch) {
                liveFrame->Contacts[i].RawX =
                    rawFrame.Contacts[bestRawIndex].RawX;
                liveFrame->Contacts[i].RawY =
                    rawFrame.Contacts[bestRawIndex].RawY;
                liveFrame->Contacts[i].Major =
                    rawFrame.Contacts[bestRawIndex].Major;
                liveFrame->Contacts[i].Minor =
                    rawFrame.Contacts[bestRawIndex].Minor;
                liveFrame->Contacts[i].Pressure =
                    rawFrame.Contacts[bestRawIndex].Pressure;
                liveFrame->Contacts[i].Orientation =
                    rawFrame.Contacts[bestRawIndex].Orientation;
            } else {
                liveFrame->Contacts[i].RawX =
                    (SHORT)((LONG)coreFrame.Contacts[i].X +
                            pCtx->DeviceInfo->x.min);
                liveFrame->Contacts[i].RawY =
                    (SHORT)(pCtx->DeviceInfo->y.max -
                            (LONG)coreFrame.Contacts[i].Y);
                liveFrame->Contacts[i].Major = 0;
                liveFrame->Contacts[i].Minor = 0;
            }
        }

        // Publish only after the inactive buffer is completely populated.
        KeMemoryBarrier();
        InterlockedExchange(
            &pCtx->LiveFrameIndex,
            (LONG)(liveIndex & 1u));
    }
    WdfSpinLockRelease(pCtx->LiveLock);

    // Mouse delivery is intentionally a separate, very short critical
    // section around the force-touch state machine. Its request comes from
    // MouseInputQueue, never the digitizer queue.
    if (needMouseDelivery) {
        WDFREQUEST mouseRequest = NULL;
        WdfSpinLockAcquire(pCtx->StateLock);

        Status = WdfIoQueueRetrieveNextRequest(pCtx->MouseInputQueue, &mouseRequest);
        if (NT_SUCCESS(Status)) {
            WDFMEMORY mouseRequestMemory;
            Status = WdfRequestRetrieveOutputMemory(mouseRequest, &mouseRequestMemory);
            if (NT_SUCCESS(Status)) {
                PTP_FORCETOUCH_MOUSE_REPORT mouseReport;
                BOOLEAN edgeButtonState =
                    (pCtx->ForceTouchDeliveryState == FORCE_TOUCH_DELIVERY_DOWN_PENDING);

                RtlZeroMemory(&mouseReport, sizeof(mouseReport));
                mouseReport.ReportID = REPORTID_STANDARDMOUSE;

                switch (pCtx->PointerConfig.ForceTapAction) {
                case AMT_POINTER_ACTION_MIDDLE_CLICK:
                    mouseReport.Button3 = edgeButtonState ? 1 : 0;
                    break;
                case AMT_POINTER_ACTION_DOUBLE_CLICK:
                    mouseReport.Button1 = edgeButtonState ? 1 : 0;
                    break;
                case AMT_POINTER_ACTION_CONTEXT_MENU:
                default:
                    mouseReport.Button2 = edgeButtonState ? 1 : 0;
                    break;
                }

                Status = WdfMemoryCopyFromBuffer(
                    mouseRequestMemory, 0, (PVOID)&mouseReport, sizeof(mouseReport));
                if (NT_SUCCESS(Status)) {
                    WdfRequestSetInformation(mouseRequest, sizeof(mouseReport));

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
            }
        }

        WdfSpinLockRelease(pCtx->StateLock);

        if (mouseRequest != NULL) {
            WdfRequestComplete(mouseRequest, Status);
        }
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
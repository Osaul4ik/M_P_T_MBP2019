// Route USB input through PTPCore into HID reports.

#include "Driver.h"
#include "PTPCore.h"
#include "Input.h"

#ifdef ALLOC_PRAGMA
// These two are the only PASSIVE_LEVEL-only, non-hot-path functions in this
// file (the reader-recovery escalation ladder - see READER_RECOVERY_STAGE
// in Device.h); everything else here runs on the USB completion/DPC path
// and must stay non-paged.
#pragma alloc_text (PAGE, AmtPtpCyclePort)
#pragma alloc_text (PAGE, AmtPtpEvtReaderRestartTimer)
#endif

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
    if (n > PTP_MAX_CONTACT_POINTS)
        n = PTP_MAX_CONTACT_POINTS;

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
// overflow, so a delivered DOWN can never be left without its UP.
static VOID
AmtForceTouchClickEnqueue(
    _Inout_ PDEVICE_CONTEXT pCtx,
    _In_ UCHAR             ClickCount
)
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

        // Else: click storm far beyond anything a human can produce - drop
        // the newest one. Safe: it never started delivering, so nothing is
        // left half-sent.
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpConfigContReaderForInterruptEndPoint(
    _In_ PDEVICE_CONTEXT DeviceContext
)
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

    // Validate tp_fsize once at PASSIVE_LEVEL setup time rather than on every
    // USB completion. tp_fsize is fixed for the lifetime of DeviceContext.
    if (transferLength == 0 ||
        DeviceContext->DeviceInfo->tp_fsize == 0) {
        status = STATUS_UNKNOWN_REVISION;
        goto exit;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(
        &contReaderConfig,
        AmtPtpEvtUsbInterruptPipeReadComplete,
        DeviceContext,
        transferLength);

    contReaderConfig.EvtUsbTargetPipeReadersFailed =
        AmtPtpEvtUsbInterruptReadersFailed;

    status = WdfUsbTargetPipeConfigContinuousReader(
        DeviceContext->InterruptPipe,
        &contReaderConfig);

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

    PDEVICE_CONTEXT pCtx = Context;

    size_t headerSize = (unsigned int)pCtx->DeviceInfo->tp_header;
    size_t fingerSize = (unsigned int)pCtx->DeviceInfo->tp_fsize;
    size_t tpDelta    = (unsigned int)pCtx->DeviceInfo->tp_delta;
    size_t raw_n      = 0;

    UCHAR* TouchBuffer = NULL;

    LONGLONG      PerfDelta;
    LARGE_INTEGER Now;
    NTSTATUS      Status;
    PTP_REPORT    Report;
    WDFREQUEST    Request;
    WDFMEMORY     RequestMemory;

    // USB read completion.

    // fingerSize/headerSize come from the static config entry latched once
    // for this device. The fingerSize==0 case is rejected during setup.
    NT_ASSERT(fingerSize != 0);

    // A successful transfer clears the reader-recovery escalation ladder
    // (AmtPtpEvtUsbInterruptReadersFailed / AmtPtpEvtReaderRestartTimer) -
    // a blip early in a power session should not leave the next unrelated
    // failure escalating straight to a port reset or port cycle.
    //
    // MS-RECOMMENDED SYNCHRONIZATION: this write races the read-modify-
    // write AmtPtpEvtReaderRestartTimer does on the same field under
    // D0ExitLock (Interrupt.c), and the unlocked reads
    // AmtPtpEvtUsbInterruptReadersFailed used to do below - both this
    // routine and ReadersFailed run on the same DPC-level completion path,
    // so an unlocked write here is not just a correctness nicety: without
    // the lock's release/acquire barrier there is no guarantee the timer
    // callback (which can be running on a different CPU) ever observes
    // this reset promptly, or that this write and the timer's own
    // ReaderRecoveryStage++ don't tear against each other. D0ExitLock is a
    // spinlock with no blocking calls inside, so taking it here costs
    // nothing correctness- or performance-wise on the hot path.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    pCtx->ReaderRecoveryStage = READER_RECOVERY_RESET_PIPE;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    // Validate packet shape before dequeuing a HID request and before taking
    // StateLock. This ensures malformed/short USB packets cannot leave an
    // already-dequeued request pending or abandon StateLock.
    if (NumBytesTransferred < headerSize ||
        pCtx->DeviceInfo->tp_button >= NumBytesTransferred ||
        (NumBytesTransferred - headerSize) % fingerSize != 0) {
        return;
    }

    if (pCtx->PtpReportTouch) {
        raw_n = (NumBytesTransferred - headerSize) / fingerSize;

        if (raw_n > PTP_MAX_CONTACT_POINTS)
            raw_n = PTP_MAX_CONTACT_POINTS;
    }

    TouchBuffer = WdfMemoryGetBuffer(Buffer, NULL);
    if (TouchBuffer == NULL) {
        return;
    }

    Status = WdfIoQueueRetrieveNextRequest(
        pCtx->InputQueue,
        &Request);

    if (!NT_SUCCESS(Status))
        return;

    Status = WdfRequestRetrieveOutputMemory(
        Request,
        &RequestMemory);

    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
        return;
    }

    // The continuous reader can re-enter on another CPU. Hold StateLock
    // across the complete state-mutating section.
    WdfSpinLockAcquire(pCtx->StateLock);

    RtlZeroMemory(&Report, sizeof(PTP_REPORT));
    Report.ReportID = REPORTID_MULTITOUCH;

    // KeQueryPerformanceCounter() returns the current QPC tick count;
    // its optional out-parameter contains the frequency.
    Now = KeQueryPerformanceCounter(NULL);

    // Windows PTP expects a free-running 100us scan-time counter.
    PerfDelta = Now.QuadPart - pCtx->LastReportTime.QuadPart;

    PerfDelta =
        (PerfDelta * pCtx->ScanTimeScaleQ16) >> 16;

    if (PerfDelta < 0)
        PerfDelta = 0;

    pCtx->ScanTimeAccumulator += (ULONG)PerfDelta;
    Report.ScanTime = (USHORT)(pCtx->ScanTimeAccumulator & 0xFFFF);
    pCtx->LastReportTime = Now;

    BOOLEAN buttonSnapshot =
        pCtx->PtpReportButton &&
        TouchBuffer[pCtx->DeviceInfo->tp_button];

    // RawFrame construction.
    RAW_FRAME rawFrame;
    RtlZeroMemory(&rawFrame, sizeof(rawFrame));
    rawFrame.TimestampQpc = Now.QuadPart;

    if (pCtx->PtpReportTouch) {
        // raw_n was validated and clamped before StateLock was taken.
        // tpDelta selects the start of the finger-record area within the
        // fixed USB transfer buffer.
        UCHAR* f_base =
            TouchBuffer + headerSize + tpDelta;

        AmtInputParseFrame(
            f_base,
            fingerSize,
            raw_n,
            pCtx->DeviceInfo,
            Now.QuadPart,
            &rawFrame);
    }
    // else: empty RawFrame -> PTPCore_ProcessFrame lifts all active contacts.

    // PTPCore orchestration.
    PTP_CORE_FRAME coreFrame;
    BOOLEAN forceTouchClick   = FALSE;
    BOOLEAN buttonClickReport = FALSE;

    PTPCore_ProcessFrame(
        pCtx,
        &rawFrame,
        Now.QuadPart,
        buttonSnapshot,
        &coreFrame,
        &forceTouchClick,
        &buttonClickReport);

    // Serialize to PTP_REPORT.
    AmtSerializeCoreFrameToReport(
        &coreFrame,
        &Report);

    // Arbitrated by PTPCore, not the raw button bit.
    if (buttonClickReport) {
        Report.IsButtonClicked = TRUE;
    }

    AmtReportCheckInvariants(&Report);

    // BUG FIX: forceTouchClick only fires the release-frame edge from
    // PTPCore_ProcessFrame - it does not by itself arm the delivery state
    // machine. Without this call ForceTouchDeliveryState never leaves IDLE,
    // so the mouse-delivery block below always computes edgeButtonState ==
    // FALSE and Force Click silently never sends a button pulse. Still
    // under StateLock, matching every other ForceTouchDeliveryState/
    // PendingForceTouchClickCount mutation in this file.
    if (forceTouchClick) {
        UCHAR clickCount =
            (pCtx->PointerConfig.ForceTapAction == AMT_POINTER_ACTION_DOUBLE_CLICK)
                ? 2 : 1;
        AmtForceTouchClickEnqueue(pCtx, clickCount);
    }

    // Capture force-touch delivery state while StateLock is held.
    BOOLEAN needMouseDelivery =
        forceTouchClick ||
        (pCtx->ForceTouchDeliveryState != FORCE_TOUCH_DELIVERY_IDLE);

    // Main state lock is no longer needed.
    WdfSpinLockRelease(pCtx->StateLock);

    Status = WdfMemoryCopyFromBuffer(
        RequestMemory,
        0,
        (PVOID)&Report,
        sizeof(PTP_REPORT));

    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
        return;
    }

    WdfRequestSetInformation(
        Request,
        sizeof(PTP_REPORT));

    WdfRequestComplete(
        Request,
        STATUS_SUCCESS);

    // Optional live monitor snapshot.
    WdfSpinLockAcquire(pCtx->LiveLock);

    if (pCtx->LiveEnabled) {
        ULONG i;

        ULONG liveIndex =
            ((ULONG)InterlockedCompareExchange(
                &pCtx->LiveFrameIndex,
                0,
                0)) ^ 1u;

        PAMT_LIVE_FRAME liveFrame =
            &pCtx->LiveFrame[liveIndex & 1u];

        pCtx->LiveSequence++;

        RtlZeroMemory(
            liveFrame,
            sizeof(*liveFrame));

        liveFrame->StructVersion =
            AMT_LIVE_FRAME_VERSION;

        liveFrame->Sequence =
            pCtx->LiveSequence;

        liveFrame->TimestampQpc =
            Now.QuadPart;

        liveFrame->ContactCount =
            coreFrame.ContactCount;

        liveFrame->RawContactCount =
            rawFrame.ContactCount;

        liveFrame->LargePalmBlanked =
            coreFrame.LargePalmBlanked ? 1 : 0;

        liveFrame->ButtonDown =
            buttonSnapshot ? 1 : 0;

        liveFrame->ForceTouchClick =
            forceTouchClick ? 1 : 0;

        liveFrame->ButtonClickReport =
            buttonClickReport ? 1 : 0;

        for (i = 0;
             i < coreFrame.ContactCount &&
             i < AMT_LIVE_MAX_CONTACTS;
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

            for (j = 0;
                 j < rawFrame.ContactCount;
                 ++j) {

                LONGLONG dx =
                    (LONGLONG)coreFrame.Contacts[i].X -
                    (LONGLONG)rawFrame.Contacts[j].X;

                LONGLONG dy =
                    (LONGLONG)coreFrame.Contacts[i].Y -
                    (LONGLONG)rawFrame.Contacts[j].Y;

                ULONGLONG distance =
                    (ULONGLONG)(dx * dx + dy * dy);

                if (!haveRawMatch ||
                    distance < bestDistance) {

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

        // Publish only after the inactive buffer is fully populated.
        KeMemoryBarrier();

        InterlockedExchange(
            &pCtx->LiveFrameIndex,
            (LONG)(liveIndex & 1u));
    }

    WdfSpinLockRelease(pCtx->LiveLock);

    // Mouse delivery is a separate critical section around the force-touch
    // delivery state machine.
    //
    // REVERT (force-touch regression fix): opportunistically claim a second
    // pending IOCTL_HID_READ_REPORT request off the SAME manual InputQueue
    // used by the digitizer/touch client above - mouhid.sys keeps its own
    // read continuously queued there too, same as before the "Fullfix"
    // rework split this into a separate MouseInputQueue. That split is the
    // most likely cause of the intermittent (every-other-press) force-touch
    // failures, since nothing guarantees a mouse-collection read is sitting
    // in a dedicated queue at the exact moment this handler needs one.
    if (needMouseDelivery) {
        WDFREQUEST mouseRequest = NULL;

        WdfSpinLockAcquire(pCtx->StateLock);

        Status = WdfIoQueueRetrieveNextRequest(
            pCtx->InputQueue,
            &mouseRequest);

        if (NT_SUCCESS(Status)) {
            WDFMEMORY mouseRequestMemory;

            Status = WdfRequestRetrieveOutputMemory(
                mouseRequest,
                &mouseRequestMemory);

            if (NT_SUCCESS(Status)) {
                PTP_FORCETOUCH_MOUSE_REPORT mouseReport;

                BOOLEAN edgeButtonState =
                    (pCtx->ForceTouchDeliveryState ==
                     FORCE_TOUCH_DELIVERY_DOWN_PENDING);

                RtlZeroMemory(
                    &mouseReport,
                    sizeof(mouseReport));

                mouseReport.ReportID =
                    REPORTID_STANDARDMOUSE;

                switch (pCtx->PointerConfig.ForceTapAction) {
                case AMT_POINTER_ACTION_MIDDLE_CLICK:
                    mouseReport.Button3 =
                        edgeButtonState ? 1 : 0;
                    break;

                case AMT_POINTER_ACTION_DOUBLE_CLICK:
                    mouseReport.Button1 =
                        edgeButtonState ? 1 : 0;
                    break;

                case AMT_POINTER_ACTION_CONTEXT_MENU:
                default:
                    mouseReport.Button2 =
                        edgeButtonState ? 1 : 0;
                    break;
                }

                Status = WdfMemoryCopyFromBuffer(
                    mouseRequestMemory,
                    0,
                    (PVOID)&mouseReport,
                    sizeof(mouseReport));

                if (NT_SUCCESS(Status)) {
                    WdfRequestSetInformation(
                        mouseRequest,
                        sizeof(mouseReport));

                    if (pCtx->ForceTouchDeliveryState ==
                        FORCE_TOUCH_DELIVERY_DOWN_PENDING) {

                        pCtx->ForceTouchDeliveryState =
                            FORCE_TOUCH_DELIVERY_UP_PENDING;

                    } else {
                        pCtx->ForceTouchDeliveryState =
                            FORCE_TOUCH_DELIVERY_IDLE;

                        if (pCtx->PendingForceTouchClickCount > 0) {
                            pCtx->PendingForceTouchClickCount--;

                            pCtx->ForceTouchDeliveryState =
                                FORCE_TOUCH_DELIVERY_DOWN_PENDING;
                        }
                    }
                }
            }
        }

        WdfSpinLockRelease(pCtx->StateLock);

        if (mouseRequest != NULL) {
            WdfRequestComplete(
                mouseRequest,
                Status);
        }
    }
}

// AmtPtpCyclePort
//
// Last rung of the reader-recovery escalation ladder - see
// READER_RECOVERY_STAGE in Device.h. Power-cycles the device's USB port,
// the moral equivalent of an unplug/replug: the device disappears and
// reappears through normal PnP. A WDFUSB wrapper for this does exist -
// WdfUsbTargetDeviceCyclePortSynchronously(WDFUSBDEVICE) - but per its
// documented signature it takes no WDFREQUEST/WDF_REQUEST_SEND_OPTIONS
// parameters, so it cannot be given a time-out. This is the last rung of
// the ladder and, unlike a stuck endpoint or a bad port, has no fallback
// if it never completes, so it deliberately goes down as the raw internal
// IOCTL against the WDFUSBDEVICE's own I/O target instead, purely to keep
// the bounded WDF_REQUEST_SEND_OPTION_TIMEOUT below.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpCyclePort(
    _In_ PDEVICE_CONTEXT DeviceContext)
{
    WDFIOTARGET               ioTarget;
    WDF_REQUEST_SEND_OPTIONS  sendOptions;

    PAGED_CODE();

    ioTarget = WdfUsbTargetDeviceGetIoTarget(DeviceContext->UsbDevice);

    // Bounded like the Wellspring control transfers in Device.c - a cycle
    // request that never completes must not hang the passive-level
    // recovery worker (AmtPtpEvtReaderRestartTimer) forever.
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(
        &sendOptions, WDF_REL_TIMEOUT_IN_SEC(WELLSPRING_CONTROL_TRANSFER_TIMEOUT_SEC));

    return WdfIoTargetSendInternalIoctlSynchronously(
        ioTarget,
        NULL,                        // Request - let WDF allocate one
        IOCTL_INTERNAL_USB_CYCLE_PORT,
        NULL,                        // InputBuffer
        NULL,                        // OutputBuffer
        &sendOptions,
        NULL);                       // BytesReturned
}

// AmtPtpEvtReaderRestartTimer
//
// Performs one rung of the READER_RECOVERY_STAGE escalation ladder, then
// tries to resume the continuous reader. Runs at PASSIVE_LEVEL (the timer
// object was created with ExecutionLevel = WdfExecutionLevelPassive and
// AutomaticSerialization = FALSE - see Device.c - specifically for this:
// WdfUsbTargetPipeResetSynchronously, WdfUsbTargetDeviceResetPortSynchronously,
// AmtPtpCyclePort, and WdfWaitLockAcquire below are all PASSIVE_LEVEL-only
// blocking calls).
//
// SYNCHRONIZATION SHAPE (see the two-level model comment in Device.h):
// a snapshot taken under D0ExitLock alone is not, by itself, a lifetime
// guarantee - D0Exit/ReleaseHardware could still start and finish an entire
// teardown between this function's snapshot and its actual use of that
// snapshot a few calls later. So every use of InterruptPipe/UsbDevice for
// an actual (possibly blocking) recovery step happens only after this
// function itself holds RecoveryLock, with the D0ExitInProgress/
// RecoveryGeneration check *repeated* once RecoveryLock is held - closing
// the window a single up-front check would leave open.
VOID
AmtPtpEvtReaderRestartTimer(
    _In_ WDFTIMER Timer)
{
    WDFDEVICE        device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT  pCtx   = DeviceGetContext(device);
    WDFUSBPIPE       localInterruptPipe;
    ULONG            snapshotGeneration;
    READER_RECOVERY_STAGE stage;

    PAGED_CODE();

    // Phase 1 (DISPATCH-safe): quick check-and-snapshot under D0ExitLock
    // only. No blocking call is made while this lock is held.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    if (pCtx->D0ExitInProgress) {
        WdfSpinLockRelease(pCtx->D0ExitLock);
        AmtTrace(pCtx, "ReaderRestartTimer: D0Exit in progress, backing off");
        return;
    }
    snapshotGeneration  = pCtx->RecoveryGeneration;
    stage               = pCtx->ReaderRecoveryStage;
    localInterruptPipe  = pCtx->InterruptPipe;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    if (stage >= READER_RECOVERY_EXHAUSTED) {
        // Already tried every rung this D0 session. Stay silent - and
        // leave the pipe target untouched - until the next D0Entry
        // (sleep/wake cycle or replug) resets the ladder.
        AmtTrace(pCtx, "ReaderRestartTimer: ladder EXHAUSTED, giving up until next D0Entry");
        return;
    }

    if (localInterruptPipe == NULL) {
        AmtTrace(pCtx, "ReaderRestartTimer: InterruptPipe is NULL, bailing out");
        (VOID)AmtPtpRecoveryMarkExhaustedIfCurrent(pCtx, snapshotGeneration);
        return;
    }

    AmtTrace(pCtx, "ReaderRestartTimer: ENTER, stage=%d", (int)stage);

    // Phase 2 (PASSIVE_LEVEL): acquire RecoveryLock to serialize with any
    // other in-flight recovery/D0Exit/ReleaseHardware cleanup, then
    // re-validate the snapshot from Phase 1 - lifecycle may have moved on
    // (D0Exit, ReleaseHardware, or a full D0Exit->D0Entry cycle) while this
    // callback was waiting for the lock.
    WdfWaitLockAcquire(pCtx->RecoveryLock, NULL);

    {
        BOOLEAN lifecycleStale;

        WdfSpinLockAcquire(pCtx->D0ExitLock);
        lifecycleStale =
            pCtx->D0ExitInProgress ||
            (pCtx->RecoveryGeneration != snapshotGeneration);

        // BUG FIX: re-read ReaderRecoveryStage here too, not just
        // D0ExitInProgress/RecoveryGeneration. AmtPtpEvtUsbInterruptPipeReadComplete
        // resets ReaderRecoveryStage to READER_RECOVERY_RESET_PIPE on every
        // successful transfer WITHOUT bumping RecoveryGeneration (a
        // successful read is not a lifecycle transition), so a stage
        // snapshotted in Phase 1 can go stale purely from ladder progress -
        // no D0Exit/D0Entry/ReleaseHardware involved at all - and the
        // generation check above would not catch it. Without this refresh,
        // the switch below could act on a rung the driver has already
        // moved past (e.g. still executing RESET_PORT after a read already
        // succeeded and reset the ladder to RESET_PIPE), and the
        // escalation step further down - which increments the *current*
        // field value, not this local - would then diverge from the rung
        // actually executed.
        if (!lifecycleStale) {
            stage = pCtx->ReaderRecoveryStage;
        }
        WdfSpinLockRelease(pCtx->D0ExitLock);

        if (lifecycleStale) {
            AmtTrace(pCtx, "ReaderRestartTimer: lifecycle changed since snapshot, aborting");
            WdfWaitLockRelease(pCtx->RecoveryLock);
            return;
        }

        if (stage >= READER_RECOVERY_EXHAUSTED) {
            // The ladder was already reset/exhausted by something else
            // (e.g. a successful read, or another timer fire) between
            // Phase 1 and here - nothing left to do this fire.
            AmtTrace(pCtx, "ReaderRestartTimer: stage changed to EXHAUSTED since snapshot, aborting");
            WdfWaitLockRelease(pCtx->RecoveryLock);
            return;
        }
    }

    // MS-RECOMMENDED CHECK (How to Recover From USB Pipe Errors,
    // learn.microsoft.com/windows-hardware/drivers/usbcon/how-to-recover-from-usb-pipe-errors):
    // "Before issuing any request that resets the pipe or the device, make
    // sure that the device is connected. You can determine the connected
    // state of the device by calling the WdfUsbTargetDeviceIsConnectedSynchronous
    // method." Confirmed via DbgPrint capture across a real sleep/wake
    // cycle: the failure driving this timer was STATUS_NO_SUCH_DEVICE (the
    // physical device gone, not a stalled endpoint), so every rung was
    // guaranteed to fail and burn through the whole ladder for nothing
    // while the *real* recovery (PnP's own ReleaseHardware -> fresh
    // PrepareHardware once the device physically reappears) was already in
    // flight independently. Checking first avoids sending pointless
    // reset/cycle-port requests at a PDO that's already gone.
    if (!NT_SUCCESS(WdfUsbTargetDeviceIsConnectedSynchronous(pCtx->UsbDevice))) {
        AmtTrace(pCtx, "ReaderRestartTimer: device not connected, "
            "skipping reset ladder - waiting for PnP re-enumeration");
        (VOID)AmtPtpRecoveryMarkExhaustedIfCurrent(pCtx, snapshotGeneration);
        WdfWaitLockRelease(pCtx->RecoveryLock);
        return;
    }

    // All three rungs require the interrupt pipe's I/O target to be
    // stopped first - this is not optional bookkeeping, it is a documented
    // precondition of each underlying WDFUSB call:
    //   - WdfUsbTargetPipeResetSynchronously:      "The driver must call
    //     WdfIoTargetStop before it calls WdfUsbTargetPipeResetSynchronously."
    //   - WdfUsbTargetDeviceResetPortSynchronously: "The driver must call
    //     WdfIoTargetStop before it calls WdfUsbTargetDeviceResetPortSynchronously."
    //   - cycle-port (IOCTL_INTERNAL_USB_CYCLE_PORT / the
    //     WdfUsbTargetDeviceCyclePortSynchronously wrapper it stands in
    //     for): same requirement, per WdfUsbTargetDeviceFormatRequestForCyclePort
    //     remarks ("Before the driver calls WdfRequestSend, it must call
    //     WdfIoTargetStop").
    // (See Microsoft Learn / wdfusb.h reference for each function.)
    WdfIoTargetStop(
        WdfUsbTargetPipeGetIoTarget(localInterruptPipe),
        WdfIoTargetCancelSentIo);

    if (stage == READER_RECOVERY_CYCLE_PORT) {
        // CYCLE_PORT is NOT just another rung of the ladder - see the
        // Device.h READER_RECOVERY_STAGE comment and the CYCLE_PORT
        // sections of the lifecycle design notes. IOCTL_INTERNAL_USB_CYCLE_PORT
        // power-cycles the port: the device is expected to disappear and
        // come back through ordinary PnP (surprise removal ->
        // ReleaseHardware -> PrepareHardware -> D0Entry), not to keep
        // living as the same WDFUSBDEVICE/WDFUSBPIPE/IoTarget this
        // function is holding right now. Continuing on afterward to
        // WdfIoTargetStart(localInterruptPipe) - as the other two rungs do
        // - would resubmit reads against a pipe object PnP may be in the
        // middle of tearing down underneath this function, which is
        // exactly the lock-inversion/UAF shape the lifecycle redesign
        // exists to prevent.
        //
        // So: mark the ladder exhausted for this (now-ending) D0 session,
        // release RecoveryLock, and return immediately without touching
        // localInterruptPipe again. PnP owns everything from here; the
        // eventual D0Entry that follows re-enumeration gives the ladder a
        // clean slate on a new RecoveryGeneration.
        NTSTATUS rungStatus = AmtPtpCyclePort(pCtx);
        AmtTrace(pCtx, "ReaderRestartTimer: CYCLE_PORT -> status=0x%08X, "
            "handing lifecycle to PnP", rungStatus);

        (VOID)AmtPtpRecoveryMarkExhaustedIfCurrent(pCtx, snapshotGeneration);

        WdfWaitLockRelease(pCtx->RecoveryLock);
        return;
    }

    switch (stage) {
    case READER_RECOVERY_RESET_PIPE: {
        NTSTATUS rungStatus = WdfUsbTargetPipeResetSynchronously(
            localInterruptPipe,
            WDF_NO_HANDLE,
            NULL);
        AmtTrace(pCtx, "ReaderRestartTimer: RESET_PIPE -> status=0x%08X", rungStatus);
        break;
    }

    case READER_RECOVERY_RESET_PORT:
    default: {
        // The framework reselects the current USB configuration after
        // a successful port reset, so InterruptPipe/UsbInterface stay
        // valid - no need to redo SelectInterruptInterface here.
        // READER_RECOVERY_RESET_PORT is the only remaining value stage
        // can hold here (RESET_PIPE handled above, CYCLE_PORT handled and
        // returned above, EXHAUSTED+ returned in Phase 1); default is kept
        // only as a defensive fallback.
        NTSTATUS rungStatus = WdfUsbTargetDeviceResetPortSynchronously(pCtx->UsbDevice);
        AmtTrace(pCtx, "ReaderRestartTimer: RESET_PORT -> status=0x%08X", rungStatus);
        break;
    }
    }

    // Escalate for next time regardless of this step's own result - if the
    // reader is still stuck after this recovery action, the *next* failure
    // callback should try the next rung, not repeat this one forever. Only
    // apply if this is still the same generation the snapshot was taken
    // from - a concurrent D0Exit/D0Entry may have already reset the ladder
    // for a new session, and this stale callback must not stomp on that.
    {
        READER_RECOVERY_STAGE nextStage = stage;

        WdfSpinLockAcquire(pCtx->D0ExitLock);
        if (pCtx->RecoveryGeneration == snapshotGeneration) {
            pCtx->ReaderRecoveryStage++;
            nextStage = pCtx->ReaderRecoveryStage;
        }
        WdfSpinLockRelease(pCtx->D0ExitLock);

        // Always attempt to resume the reader, even if the recovery call
        // above failed: if reads are still broken, this resubmission will
        // itself fail and drive AmtPtpEvtUsbInterruptReadersFailed again,
        // which is what carries the ladder forward to the next rung. Still
        // safe to do even if generation moved on underneath: WdfIoTargetStart
        // against a pipe whose target has since been stopped/torn down by a
        // concurrent D0Exit/ReleaseHardware simply fails, which every call
        // site already discards.
        NTSTATUS restartStatus = WdfIoTargetStart(
            WdfUsbTargetPipeGetIoTarget(localInterruptPipe));
        AmtTrace(pCtx, "ReaderRestartTimer: EXIT, WdfIoTargetStart -> status=0x%08X, next stage=%d",
            restartStatus, (int)nextStage);
    }

    WdfWaitLockRelease(pCtx->RecoveryLock);
}

BOOLEAN
AmtPtpEvtUsbInterruptReadersFailed(
    _In_ WDFUSBPIPE  Pipe,
    _In_ NTSTATUS    Status,
    _In_ USBD_STATUS UsbdStatus)
{
    WDFIOTARGET      ioTarget = WdfUsbTargetPipeGetIoTarget(Pipe);
    WDFDEVICE        device   = WdfIoTargetGetDevice(ioTarget);
    PDEVICE_CONTEXT  pCtx     = DeviceGetContext(device);
    BOOLEAN                d0ExitInProgress;
    READER_RECOVERY_STAGE  stage;

    // MS-RECOMMENDED SYNCHRONIZATION: this is the second, previously-
    // unguarded path into ReaderRestartTimer - see the D0ExitLock comment
    // in Device.h. WdfIoTargetStop(..., WdfIoTargetCancelSentIo) in
    // AmtPtpEvtDeviceD0Exit cancels any reads still in flight on this pipe,
    // and per the documented continuous-reader contract that cancellation
    // itself is what drives this very callback (EvtUsbTargetPipeReadersFailed
    // fires "after all read requests have been completed", cancellation
    // included) - so this can run synchronously *inside* D0Exit's
    // WdfIoTargetStop call, i.e. after D0ExitInProgress is already set but
    // before D0Exit has finished. Calling WdfTimerStart here unconditionally
    // would re-arm the exact timer D0Exit just tried to stop.
    //
    // D0ExitInProgress and ReaderRecoveryStage are snapshotted together
    // under one D0ExitLock critical section, not as two separate
    // lock/unlock pairs - a previous revision checked D0ExitInProgress,
    // released the lock, and only then read ReaderRecoveryStage
    // unprotected. That left a window where D0Exit could set
    // D0ExitInProgress and the timer callback could concurrently mutate
    // ReaderRecoveryStage between the two reads, and - on top of the
    // torn-snapshot risk - an unlocked read has no ordering guarantee that
    // this callback (which may run on a different CPU than the timer
    // callback that last wrote the field) observes the latest value at
    // all. Take the same D0ExitLock D0Exit/ReaderRestartTimer use - this
    // callback can run at up to DISPATCH_LEVEL (same completion path as
    // EvtUsbTargetPipeReadComplete), which is exactly why this is a
    // WDFSPINLOCK and not a WDFWAITLOCK.
    WdfSpinLockAcquire(pCtx->D0ExitLock);
    d0ExitInProgress = pCtx->D0ExitInProgress;
    stage            = pCtx->ReaderRecoveryStage;
    WdfSpinLockRelease(pCtx->D0ExitLock);

    // DIAG: this is the very first callback to see a broken interrupt
    // read - Status/UsbdStatus here are the actual root cause of any
    // downstream Code 10, so they're worth capturing even though the
    // recovery logic below doesn't otherwise need their exact values.
    // Uses the just-snapshotted local, not a fresh unlocked field read.
    AmtTrace(pCtx, "ReadersFailed: Status=0x%08X, UsbdStatus=0x%08X, stage=%d",
        Status, UsbdStatus, (int)stage);

    if (d0ExitInProgress) {
        AmtTrace(pCtx, "ReadersFailed: D0Exit in progress, not arming restart timer");
        return FALSE;
    }

    // Escalating recovery (reset-pipe -> reset-port -> cycle-port) instead
    // of an immediate unconditional restart. Returning TRUE unconditionally
    // here previously let a persistently failing pipe (most commonly:
    // right after a system resume, before the endpoint has settled) spin
    // as fast as USBD could complete failed URBs - real CPU and
    // USB-controller cost, observable as elevated temperature after wake -
    // and never actually attempted the recovery a stuck endpoint or port
    // typically needs.
    if (stage >= READER_RECOVERY_EXHAUSTED) {
        // Give up until the next D0Entry resets the ladder. The device
        // stays silent rather than burning power in an unbounded loop;
        // AmtPtpEvtDeviceD0Entry (a genuine replug, or the next sleep/wake
        // cycle) gets a clean slate to try again.
        return FALSE;
    }

    WdfTimerStart(
        pCtx->ReaderRestartTimer,
        WDF_REL_TIMEOUT_IN_MS(READER_RECOVERY_STEP_DELAY_MS));

    // We own the recovery/restart via the timer above, so tell the
    // framework not to immediately resubmit the reader itself.
    return FALSE;
}
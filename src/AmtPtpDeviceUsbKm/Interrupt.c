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
    // failure escalating straight to a port reset or port cycle. Not
    // lock-protected: it is a heuristic recovery-progress marker, not
    // correctness-critical, and the worst case of a benign race with that
    // callback is retrying one rung of the ladder an extra time, never a
    // correctness issue.
    pCtx->ReaderRecoveryStage = READER_RECOVERY_RESET_PIPE;

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
// object was created with ExecutionLevel = WdfExecutionLevelPassive
// specifically for this - WdfUsbTargetPipeResetSynchronously,
// WdfUsbTargetDeviceResetPortSynchronously, and AmtPtpCyclePort are all
// PASSIVE_LEVEL-only synchronous calls).
VOID
AmtPtpEvtReaderRestartTimer(
    _In_ WDFTIMER Timer)
{
    WDFDEVICE        device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT  pCtx   = DeviceGetContext(device);

    PAGED_CODE();

    // If this fires in the narrow window after D0Exit already stopped the
    // target but before the next D0Entry restarts it, these recovery calls
    // and the final WdfIoTargetStart simply fail (target/device not in a
    // startable state) and nothing further happens - no special-casing
    // needed. D0Exit additionally cancels this timer synchronously before
    // stopping the target, so in practice that window is not reachable.
    if (pCtx->ReaderRecoveryStage >= READER_RECOVERY_EXHAUSTED) {
        // Already tried every rung this D0 session. Stay silent - and
        // leave the pipe target untouched - until the next D0Entry
        // (sleep/wake cycle or replug) resets the ladder. This check must
        // stay ahead of the WdfIoTargetStop below: unlike the three
        // recovery rungs, this exit path has no matching WdfIoTargetStart,
        // so stopping the target here would leave the reader permanently
        // stopped instead of merely giving up until the next D0Entry.
        DbgPrint("[AmtPtp] ReaderRestartTimer: ladder EXHAUSTED, giving up until next D0Entry\n");
        return;
    }

    DbgPrint("[AmtPtp] ReaderRestartTimer: ENTER, stage=%d\n",
        (int)pCtx->ReaderRecoveryStage);

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
        WdfUsbTargetPipeGetIoTarget(pCtx->InterruptPipe),
        WdfIoTargetCancelSentIo);

    switch (pCtx->ReaderRecoveryStage) {
    case READER_RECOVERY_RESET_PIPE: {
        NTSTATUS rungStatus = WdfUsbTargetPipeResetSynchronously(
            pCtx->InterruptPipe,
            WDF_NO_HANDLE,
            NULL);
        DbgPrint("[AmtPtp] ReaderRestartTimer: RESET_PIPE -> status=0x%08X\n", rungStatus);
        break;
    }

    case READER_RECOVERY_RESET_PORT: {
        // The framework reselects the current USB configuration after
        // a successful port reset, so InterruptPipe/UsbInterface stay
        // valid - no need to redo SelectInterruptInterface here.
        NTSTATUS rungStatus = WdfUsbTargetDeviceResetPortSynchronously(pCtx->UsbDevice);
        DbgPrint("[AmtPtp] ReaderRestartTimer: RESET_PORT -> status=0x%08X\n", rungStatus);
        break;
    }

    case READER_RECOVERY_CYCLE_PORT:
    default: {
        // READER_RECOVERY_CYCLE_PORT is the only remaining value the stage
        // can hold here (the >= READER_RECOVERY_EXHAUSTED case already
        // returned above); default is kept only as a defensive fallback.
        NTSTATUS rungStatus = AmtPtpCyclePort(pCtx);
        DbgPrint("[AmtPtp] ReaderRestartTimer: CYCLE_PORT -> status=0x%08X\n", rungStatus);
        break;
    }
    }

    // Escalate for next time regardless of this step's own result - if the
    // reader is still stuck after this recovery action, the *next* failure
    // callback should try the next rung, not repeat this one forever.
    pCtx->ReaderRecoveryStage++;

    // Always attempt to resume the reader, even if the recovery call above
    // failed: if reads are still broken, this resubmission will itself
    // fail and drive AmtPtpEvtUsbInterruptReadersFailed again, which is
    // what carries the ladder forward to the next rung.
    {
        NTSTATUS restartStatus = WdfIoTargetStart(
            WdfUsbTargetPipeGetIoTarget(pCtx->InterruptPipe));
        DbgPrint("[AmtPtp] ReaderRestartTimer: EXIT, WdfIoTargetStart -> status=0x%08X, next stage=%d\n",
            restartStatus, (int)pCtx->ReaderRecoveryStage);
    }
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

    // DIAG: this is the very first callback to see a broken interrupt
    // read - Status/UsbdStatus here are the actual root cause of any
    // downstream Code 10, so they're worth capturing even though the
    // recovery logic below doesn't otherwise need their exact values.
    DbgPrint("[AmtPtp] ReadersFailed: Status=0x%08X, UsbdStatus=0x%08X, stage=%d\n",
        Status, UsbdStatus, (int)pCtx->ReaderRecoveryStage);

    // Escalating recovery (reset-pipe -> reset-port -> cycle-port) instead
    // of an immediate unconditional restart. Returning TRUE unconditionally
    // here previously let a persistently failing pipe (most commonly:
    // right after a system resume, before the endpoint has settled) spin
    // as fast as USBD could complete failed URBs - real CPU and
    // USB-controller cost, observable as elevated temperature after wake -
    // and never actually attempted the recovery a stuck endpoint or port
    // typically needs.
    if (pCtx->ReaderRecoveryStage >= READER_RECOVERY_EXHAUSTED) {
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
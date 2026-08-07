// HID descriptor and report handling.

#include "Driver.h"
#include <ntintsafe.h>
#include "hid.tmh"

#ifdef ALLOC_PRAGMA
// AmtPtpReportFeatures is the only function in this file that calls
// PAGED_CODE(); without this it was PAGED_CODE()-asserting from a
// non-paged segment (PREfast C28172).
#pragma alloc_text (PAGE, AmtPtpReportFeatures)
#endif

// Centralize HID report-size validation.
static __inline BOOLEAN
HidValidateReportSize(
	_In_ PHID_XFER_PACKET pHidPacket,
	_In_ size_t           requiredSize)
{
	ULONG    requiredSizeUlong;
	NTSTATUS convertStatus;

	// requiredSize is always sizeof(...) of one of our own fixed report
	// structs at every call site (never attacker-controlled), so this can
	// never legitimately overflow ULONG - RtlSizeTToULong is used instead
	// of a raw (ULONG) cast so a future call site that DOES pass a wider
	// value fails safely instead of silently truncating.
	convertStatus = RtlSizeTToULong(requiredSize, &requiredSizeUlong);
	if (!NT_SUCCESS(convertStatus)) {
		return FALSE;
	}

	// Guard against NULL buffers from user input.
	return (pHidPacket->reportBuffer != NULL) &&
	       (pHidPacket->reportBufferLen >= requiredSizeUlong);
}

#ifndef _AAPL_HID_DESCRIPTOR_H_
#define _AAPL_HID_DESCRIPTOR_H_

HID_REPORT_DESCRIPTOR AmtPtpT2ReportDescriptor[] = {
	AAPL_WELLSPRING_T2_PTP_TLC,
	AAPL_PTP_WINDOWS_CONFIGURATION_TLC,
	// Additive: separate Mouse TLC, force-touch right-click delivery
	// only. Does not alter the PTP TLC above in any way.
	AAPL_WELLSPRING_T2_FORCETOUCH_MOUSE_TLC,
};

CONST HID_DESCRIPTOR AmtPtpT2DefaultHidDescriptor = {
	(UCHAR) sizeof(HID_DESCRIPTOR), // bLength - computed, not hardcoded;
	                                 // see C_ASSERT below
	0x21,   // bDescriptorType
	0x0100, // bcdHID
	0x00,   // bCountryCode
	0x01,   // bNumDescriptors
	{
		0x22,                               // bDescriptorType
		sizeof(AmtPtpT2ReportDescriptor)    // bDescriptorLength
	},
};

// HID_DESCRIPTOR (as declared by the WDK) is a packed 9-byte layout for
// exactly one DescriptorList entry: bLength(1) + bDescriptorType(1) +
// bcdHID(2) + bCountryCode(1) + bNumDescriptors(1) + DescriptorList[1]
// {bDescriptorType(1) + wDescriptorLength(2)}. bNumDescriptors above is
// hardcoded to 0x01, matching the single-entry array the type provides.
// If a future WDK/SDK update ever changes that layout, bLength (computed
// from sizeof above) will silently follow it - this assert exists so a
// layout change that changes byte count is caught at compile time
// instead of shipping a semantically-wrong-but-still-in-bounds HID
// descriptor to the host.
C_ASSERT(sizeof(HID_DESCRIPTOR) == 9);

#endif

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetHidDescriptor(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	size_t szCopy = 0;
	WDFMEMORY requestMemory;

	UNREFERENCED_PARAMETER(Device);

	status = WdfRequestRetrieveOutputMemory(
		Request,
		&requestMemory
	);

	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	// All supported products use the same fixed HID descriptor.
	szCopy = AmtPtpT2DefaultHidDescriptor.bLength;

	// Internal invariant: bLength must never claim more bytes than the
	// struct we're about to copy out of - a mismatch here would be an
	// out-of-bounds read baked into the static descriptor definition
	// above, not something request input can trigger.
	NT_ASSERT(szCopy <= sizeof(AmtPtpT2DefaultHidDescriptor));

	status = WdfMemoryCopyFromBuffer(
		requestMemory,
		0,
		(PVOID) &AmtPtpT2DefaultHidDescriptor,
		szCopy
	);

	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	WdfRequestSetInformation(Request, szCopy);

exit:

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetDeviceAttribs(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pContext = DeviceGetContext(Device);
	PHID_DEVICE_ATTRIBUTES pDeviceAttributes = NULL;

	// WDF guarantees a non-NULL context for a device created with this
	// context type; a NULL here would mean the object attributes in
	// AmtPtpDeviceUsbKmCreateDevice regressed.
	NT_ASSERT(pContext != NULL);

	status = WdfRequestRetrieveOutputBuffer(
		Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		&pDeviceAttributes,
		NULL
	);

	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	pDeviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	pDeviceAttributes->ProductID = pContext->DeviceDescriptor.idProduct;
	pDeviceAttributes->VendorID = pContext->DeviceDescriptor.idVendor;
	pDeviceAttributes->VersionNumber = DEVICE_VERSION;

	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

exit:

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetReportDescriptor(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	size_t szCopy = 0;
	WDFMEMORY requestMemory;

	UNREFERENCED_PARAMETER(Device);

	status = WdfRequestRetrieveOutputMemory(
		Request,
		&requestMemory
	);

	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	// All supported products share the same report descriptor.
	szCopy = AmtPtpT2DefaultHidDescriptor.DescriptorList[0].wReportLength;
	if (szCopy == 0) {
		status = STATUS_INVALID_DEVICE_STATE;
		goto exit;
	}

	// Internal invariant: wReportLength is initialized from
	// sizeof(AmtPtpT2ReportDescriptor) above (see the descriptor's static
	// init). If the two ever drift apart, the copy below reads past the
	// end of AmtPtpT2ReportDescriptor - catch that in debug builds rather
	// than silently leaking adjacent kernel memory into the report.
	NT_ASSERT(szCopy == sizeof(AmtPtpT2ReportDescriptor));

	status = WdfMemoryCopyFromBuffer(
		requestMemory,
		0,
		(PVOID) &AmtPtpT2ReportDescriptor,
		szCopy
	);

	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	WdfRequestSetInformation(Request, szCopy);

exit:

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpReportFeatures(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status;
	PHID_XFER_PACKET pHidPacket;
	WDF_REQUEST_PARAMETERS RequestParameters;
	size_t ReportSize;

	PAGED_CODE();

	status = STATUS_SUCCESS;

	// AmtPtpReportFeatures (GET_FEATURE) doesn't need per-device state -
	// unlike AmtPtpSetFeatures, every case below only reads/writes the
	// caller's buffer. Dropping the unused DeviceGetContext() call that
	// used to sit here (dead code / unused local).
	UNREFERENCED_PARAMETER(Device);

	WDF_REQUEST_PARAMETERS_INIT(&RequestParameters);
	WdfRequestGetParameters(Request, &RequestParameters);

	if (RequestParameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		status = STATUS_BUFFER_TOO_SMALL;
		goto exit;
	}

	pHidPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
	if (pHidPacket == NULL)
	{
		status = STATUS_INVALID_DEVICE_REQUEST;
		goto exit;
	}

	switch (pHidPacket->reportId)
	{
		case REPORTID_DEVICE_CAPS:
		{

			// Check buffer size
			ReportSize = sizeof(PTP_DEVICE_CAPS_FEATURE_REPORT);
			if (!HidValidateReportSize(pHidPacket, ReportSize)) {
				status = STATUS_INVALID_BUFFER_SIZE;
				goto exit;
			}

			PPTP_DEVICE_CAPS_FEATURE_REPORT capsReport = (PPTP_DEVICE_CAPS_FEATURE_REPORT) pHidPacket->reportBuffer;
			capsReport->MaximumContactPoints = PTP_MAX_CONTACT_POINTS;
			capsReport->ButtonType = PTP_BUTTON_TYPE_CLICK_PAD;
			capsReport->ReportID = REPORTID_DEVICE_CAPS;

			break;
		}
		case REPORTID_PTPHQA:
		{

			// Check buffer size
			ReportSize = sizeof(PTP_DEVICE_HQA_CERTIFICATION_REPORT);
			if (!HidValidateReportSize(pHidPacket, ReportSize))
			{
				status = STATUS_INVALID_BUFFER_SIZE;
				goto exit;
			}

			PPTP_DEVICE_HQA_CERTIFICATION_REPORT certReport = (PPTP_DEVICE_HQA_CERTIFICATION_REPORT)pHidPacket->reportBuffer;

			// RtlCopyMemory (direct assignment was a comma-expression bug).
			{
				static const UCHAR HqaBlob[256] = { DEFAULT_PTP_HQA_BLOB };
				C_ASSERT(sizeof(HqaBlob) == sizeof(certReport->CertificationBlob));
				RtlCopyMemory(certReport->CertificationBlob, HqaBlob, sizeof(HqaBlob));
			}
			certReport->ReportID = REPORTID_PTPHQA;

			break;
		}
		default:
		{

			status = STATUS_NOT_SUPPORTED;
			goto exit;
		}
	}

exit:

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetFeatures(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS        status;
	PHID_XFER_PACKET pHidPacket;
	WDF_REQUEST_PARAMETERS RequestParameters;
	PDEVICE_CONTEXT pDeviceContext;

	status = STATUS_SUCCESS;
	pDeviceContext = DeviceGetContext(Device);
	NT_ASSERT(pDeviceContext != NULL);

	WDF_REQUEST_PARAMETERS_INIT(&RequestParameters);
	WdfRequestGetParameters(Request, &RequestParameters);

	if (RequestParameters.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{

		status = STATUS_BUFFER_TOO_SMALL;
		goto exit;
	}

	pHidPacket = (PHID_XFER_PACKET) WdfRequestWdmGetIrp(Request)->UserBuffer;
	if (pHidPacket == NULL)
	{

		status = STATUS_INVALID_DEVICE_REQUEST;
		goto exit;
	}

	switch (pHidPacket->reportId)
	{
		case REPORTID_REPORTMODE:
		{

			// AUDIT FIX: validate reportBufferLen before casting/dereferencing -
			// pHidPacket is attacker/HID-class supplied and reportBuffer's real
			// size is not guaranteed to match the struct we're about to read.
			if (!HidValidateReportSize(pHidPacket, sizeof(PTP_DEVICE_INPUT_MODE_REPORT))) {
				status = STATUS_INVALID_BUFFER_SIZE;
				goto exit;
			}

			PPTP_DEVICE_INPUT_MODE_REPORT devInputMode = (PPTP_DEVICE_INPUT_MODE_REPORT) pHidPacket->reportBuffer;
			BOOLEAN bWellspringMode = pDeviceContext->IsWellspringModeOn;

			switch (devInputMode->Mode)
			{
				case PTP_COLLECTION_MOUSE:
				{

					status = STATUS_NOT_SUPPORTED;
					goto exit;
				}
				case PTP_COLLECTION_WINDOWS:
				{

					if (!bWellspringMode) {
						status = AmtPtpSetWellspringMode(pDeviceContext, TRUE);
						if (!NT_SUCCESS(status)) {
							goto exit;
						}
					}
					break;
				}
				default:
				{
					// Unknown Mode: previously fell through silently claiming
					// success (STATUS_SUCCESS left over from function entry).
					// The Mode field is a parameter within an otherwise-
					// supported report, not an unsupported report itself -
					// STATUS_INVALID_PARAMETER reflects that distinction and
					// lets the caller/HID class driver detect the rejection
					// instead of believing the mode switch took effect.
					status = STATUS_INVALID_PARAMETER;
					goto exit;
				}
			}

			break;
		}
		case REPORTID_FUNCSWITCH:
		{

			// AUDIT FIX: same missing-length-check class of bug as
			// REPORTID_REPORTMODE above - validate before dereferencing.
			if (!HidValidateReportSize(pHidPacket, sizeof(PTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT))) {
				status = STATUS_INVALID_BUFFER_SIZE;
				goto exit;
			}

			// REVERTED: honoring ButtonReport/SurfaceReport regressed real
			// hardware (pad stops responding). Validated for size above and
			// otherwise intentionally ignored - no fields are read.
			break;
		}
		default:
		{
			status = STATUS_NOT_SUPPORTED;
			goto exit;
		}
	}

exit:
	return status;
}
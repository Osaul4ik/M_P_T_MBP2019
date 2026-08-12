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

// One compiled report descriptor per distinct trackpad geometry group
// (see the header comment in metadata/WellspringT2.h for what each
// LOGX/PHYSX/LOGY/PHYSY value means and where it came from). Every array
// below shares the exact same structure - only the 8 geometry bytes per
// finger collection differ - so they are all guaranteed to be the same
// length (checked by C_ASSERT below, once, for all of them at once).

#define AAPL_DEFINE_PTP_DESCRIPTOR(NAME, LOGX, PHYSX, LOGY, PHYSY) \
	static const HID_REPORT_DESCRIPTOR NAME[] = { \
		AAPL_WELLSPRING_PTP_TLC(LOGX, PHYSX, LOGY, PHYSY), \
		AAPL_PTP_WINDOWS_CONFIGURATION_TLC, \
		AAPL_WELLSPRING_T2_FORCETOUCH_MOUSE_TLC, \
	}

AAPL_DEFINE_PTP_DESCRIPTOR(AmtPtpReportDescriptor_Fallback,
	AAPL_WS_PTP_LOGX_FALLBACK, AAPL_WS_PTP_PHYSX10_FALLBACK,
	AAPL_WS_PTP_LOGY_FALLBACK, AAPL_WS_PTP_PHYSY10_FALLBACK);

AAPL_DEFINE_PTP_DESCRIPTOR(AmtPtpReportDescriptor_T2_16,
	AAPL_WS_PTP_LOGX_T2_16, AAPL_WS_PTP_PHYSX10_T2_16,
	AAPL_WS_PTP_LOGY_T2_16, AAPL_WS_PTP_PHYSY10_T2_16);

AAPL_DEFINE_PTP_DESCRIPTOR(AmtPtpReportDescriptor_T2_15,
	AAPL_WS_PTP_LOGX_T2_15, AAPL_WS_PTP_PHYSX10_T2_15,
	AAPL_WS_PTP_LOGY_T2_15, AAPL_WS_PTP_PHYSY10_T2_15);

AAPL_DEFINE_PTP_DESCRIPTOR(AmtPtpReportDescriptor_T2_13,
	AAPL_WS_PTP_LOGX_T2_13, AAPL_WS_PTP_PHYSX10_T2_13,
	AAPL_WS_PTP_LOGY_T2_13, AAPL_WS_PTP_PHYSY10_T2_13);

AAPL_DEFINE_PTP_DESCRIPTOR(AmtPtpReportDescriptor_WS9,
	AAPL_WS_PTP_LOGX_WS9, AAPL_WS_PTP_PHYSX10_WS9,
	AAPL_WS_PTP_LOGY_WS9, AAPL_WS_PTP_PHYSY10_WS9);

AAPL_DEFINE_PTP_DESCRIPTOR(AmtPtpReportDescriptor_WS8,
	AAPL_WS_PTP_LOGX_WS8, AAPL_WS_PTP_PHYSX10_WS8,
	AAPL_WS_PTP_LOGY_WS8, AAPL_WS_PTP_PHYSY10_WS8);

#undef AAPL_DEFINE_PTP_DESCRIPTOR

// Same wrapper for every group - only DescriptorList[0].wReportLength
// would ever need to differ, and the C_ASSERTs below guarantee it never
// does (every geometry value is small enough to stay in the *_2, i.e.
// 2-byte, HID item encoding, so no array's byte length can drift from the
// others just because its numbers are bigger).
CONST HID_DESCRIPTOR AmtPtpT2DefaultHidDescriptor = {
	(UCHAR) sizeof(HID_DESCRIPTOR), // bLength - computed, not hardcoded;
	                                 // see C_ASSERT below
	0x21,   // bDescriptorType
	0x0100, // bcdHID
	0x00,   // bCountryCode
	0x01,   // bNumDescriptors
	{
		0x22,                                        // bDescriptorType
		sizeof(AmtPtpReportDescriptor_T2_16)          // bDescriptorLength
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

// All per-model descriptors MUST be byte-identical in length - Queue.c /
// AmtPtpGetReportDescriptor serve whichever one matches this device's PID,
// but AmtPtpT2DefaultHidDescriptor above (returned unconditionally by
// AmtPtpGetHidDescriptor before the report descriptor itself is ever
// requested) advertises one fixed wReportLength for all of them. If a
// future geometry value ever needed a *_3 (4-byte) item instead of *_2,
// this would silently break every model except the one that grew - catch
// it here instead.
C_ASSERT(sizeof(AmtPtpReportDescriptor_Fallback) == sizeof(AmtPtpReportDescriptor_T2_16));
C_ASSERT(sizeof(AmtPtpReportDescriptor_T2_15)    == sizeof(AmtPtpReportDescriptor_T2_16));
C_ASSERT(sizeof(AmtPtpReportDescriptor_T2_13)    == sizeof(AmtPtpReportDescriptor_T2_16));
C_ASSERT(sizeof(AmtPtpReportDescriptor_WS9)      == sizeof(AmtPtpReportDescriptor_T2_16));
C_ASSERT(sizeof(AmtPtpReportDescriptor_WS8)      == sizeof(AmtPtpReportDescriptor_T2_16));

// PID -> report descriptor. Deliberately mirrors AmtPtpGetDeviceConfig's
// grouping in Device.c (same USB_DEVICE_ID_* constants, same fallback
// semantics: an unrecognized PID still binds, using the generic
// oversampled geometry instead of failing) - if a PID is ever added to
// Bcm5974ConfigTable in AppleDefinition.h, add the matching row here too.
typedef struct _PTP_DESCRIPTOR_ENTRY {
	USHORT                       ProductId;
	const HID_REPORT_DESCRIPTOR* Descriptor;
	size_t                       DescriptorLength;
} PTP_DESCRIPTOR_ENTRY;

#define PTP_DESCRIPTOR_ROW(PID, ARR) { (PID), (ARR), sizeof(ARR) }

static const PTP_DESCRIPTOR_ENTRY AmtPtpDescriptorTable[] = {
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J152F,         AmtPtpReportDescriptor_T2_16),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J680,          AmtPtpReportDescriptor_T2_15),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J140K,         AmtPtpReportDescriptor_T2_13),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J132,          AmtPtpReportDescriptor_T2_13),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J213,          AmtPtpReportDescriptor_T2_13),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J214K,         AmtPtpReportDescriptor_T2_13),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J223,          AmtPtpReportDescriptor_T2_13),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_T2_J230K,         AmtPtpReportDescriptor_T2_13),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_WELLSPRING9_ANSI, AmtPtpReportDescriptor_WS9),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_WELLSPRING9_ISO,  AmtPtpReportDescriptor_WS9),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_WELLSPRING9_JIS,  AmtPtpReportDescriptor_WS9),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_WELLSPRING8_ANSI, AmtPtpReportDescriptor_WS8),
	PTP_DESCRIPTOR_ROW(USB_DEVICE_ID_APPLE_WELLSPRING8_ISO,  AmtPtpReportDescriptor_WS8),
};

#undef PTP_DESCRIPTOR_ROW

// Resolve this device's report descriptor by its USB Product ID. Unknown
// PID -> the same generic oversampled fallback AmtPtpGetDeviceConfig()
// (Device.c) uses for Bcm5974ConfigTable, so an unlisted-but-compatible
// device still binds instead of refusing to load.
static const PTP_DESCRIPTOR_ENTRY*
AmtPtpResolveReportDescriptor(_In_ USHORT ProductId)
{
	for (size_t i = 0; i < sizeof(AmtPtpDescriptorTable) / sizeof(AmtPtpDescriptorTable[0]); i++) {
		if (AmtPtpDescriptorTable[i].ProductId == ProductId) {
			return &AmtPtpDescriptorTable[i];
		}
	}

	static const PTP_DESCRIPTOR_ENTRY fallback = {
		USB_DEVICE_ID_DEFAULT_FALLBACK,
		AmtPtpReportDescriptor_Fallback,
		sizeof(AmtPtpReportDescriptor_Fallback)
	};
	return &fallback;
}

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

	// The outer HID_DESCRIPTOR wrapper (bLength/bcdHID/wReportLength) is
	// identical across all models - only the report descriptor it points
	// to (served by AmtPtpGetReportDescriptor below) varies per model.
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
	PDEVICE_CONTEXT pContext = DeviceGetContext(Device);
	const PTP_DESCRIPTOR_ENTRY* pDescEntry;

	NT_ASSERT(pContext != NULL);

	status = WdfRequestRetrieveOutputMemory(
		Request,
		&requestMemory
	);

	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	// Per-model report descriptor, keyed by this device's USB Product ID -
	// see AmtPtpResolveReportDescriptor and metadata/WellspringT2.h. This
	// is what previously always served MacBookPro16,1's geometry
	// (LOGICAL_MAXIMUM/PHYSICAL_MAXIMUM) to every model.
	pDescEntry = AmtPtpResolveReportDescriptor(pContext->DeviceDescriptor.idProduct);
	szCopy = pDescEntry->DescriptorLength;
	if (szCopy == 0) {
		status = STATUS_INVALID_DEVICE_STATE;
		goto exit;
	}

	// Internal invariant: every per-model array is asserted equal length
	// at compile time (see the C_ASSERTs above AmtPtpDescriptorTable), and
	// AmtPtpT2DefaultHidDescriptor's wReportLength is that same shared
	// length - so this must always hold regardless of which model matched.
	NT_ASSERT(szCopy == AmtPtpT2DefaultHidDescriptor.DescriptorList[0].wReportLength);

	status = WdfMemoryCopyFromBuffer(
		requestMemory,
		0,
		(PVOID) pDescEntry->Descriptor,
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
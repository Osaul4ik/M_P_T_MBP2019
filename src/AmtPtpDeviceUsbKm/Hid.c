// HID descriptor and report handling.

#include "Driver.h"
#include "hid.tmh"

// Centralize HID report-size validation.
static __inline BOOLEAN
HidValidateReportSize(
	_In_ PHID_XFER_PACKET pHidPacket,
	_In_ size_t           requiredSize)
{
	// Guard against NULL buffers from user input.
	return (pHidPacket->reportBuffer != NULL) &&
	       (pHidPacket->reportBufferLen >= (ULONG)requiredSize);
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
	0x09,   // bLength
	0x21,   // bDescriptorType
	0x0100, // bcdHID
	0x00,   // bCountryCode
	0x01,   // bNumDescriptors
	{
		0x22,                               // bDescriptorType
		sizeof(AmtPtpT2ReportDescriptor)    // bDescriptorLength
	},
};

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
	PDEVICE_CONTEXT pDeviceContext;
	PHID_XFER_PACKET pHidPacket;
	WDF_REQUEST_PARAMETERS RequestParameters;
	size_t ReportSize;

	PAGED_CODE();

	status = STATUS_SUCCESS;
	pDeviceContext = DeviceGetContext(Device);

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
					// Unknown Mode: previously fell through silently claiming success.
					break;
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
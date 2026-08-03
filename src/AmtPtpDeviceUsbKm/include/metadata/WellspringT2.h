#pragma once

#include <hid/HidCommon.h>

// Match the descriptor's physical dimensions for the 16-inch trackpad.

#define AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1 \
	BEGIN_COLLECTION, 0x02, /* Begin Collection: Logical */ \
		/* Begin a byte */ \
		LOGICAL_MAXIMUM, 0x01, /* Logical Maximum: 1 */ \
		USAGE, 0x47, /* Usage: Confidence */ \
		USAGE, 0x42, /* Usage: Tip switch */ \
		REPORT_COUNT, 0x02, /* Report Count: 2 */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		REPORT_COUNT, 0x06, /* Report Count: 6 */ \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* End of a byte */ \
		/* Begin of 4 bytes */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		REPORT_SIZE, 0x20, /* Report Size: 0x10 (2 bytes) */ \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0xff, 0xff, /* Logical Maximum: 0xffffffff */ \
		USAGE, 0x51, /* Usage: Contract Identifier */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
		/* Begin of 4 bytes */ \
		/* Size is hard-coded at this moment */ \
		USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
		LOGICAL_MAXIMUM_2, 0x20, 0x4e, /* Logical Maximum: 20000 (See defintion) */ \
		REPORT_SIZE, 0x10, /* Report Size: 0x10 (2 bytes) */ \
		UNIT_EXPONENT, 0x0e, /* Unit exponent: -2 */ \
		UNIT, 0x11, /* Unit: SI Length (cm) */ \
		USAGE, 0x30, /* Usage: X */ \
		PHYSICAL_MAXIMUM_2, 0x2a, 0x06, /* Physical Maximum: 1578 = 157.8mm — measured, MacBookPro16,1 sensor surface width */ \
		REPORT_COUNT, 0x01, /* Report count: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM_2, 0xd2, 0x03, /* Physical Maximum: 978 = 97.8mm — measured, MacBookPro16,1 sensor surface height */ \
		LOGICAL_MAXIMUM_2, 0xe0, 0x2e, /* Logical Maximum: 12000 (See definition) */ \
		USAGE, 0x31, /* Usage: Y */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM, 0x00, /* Physical Maximum: 0 */ \
		UNIT_EXPONENT, 0x00, /* Unit exponent: 0 */ \
		UNIT, 0x00, /* Unit: None */ \
		/* End of 4 bytes */ \
	END_COLLECTION /* End Collection */ \

#define AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_2 \
	BEGIN_COLLECTION, 0x02, /* Begin Collection: Logical */ \
		/* Begin a byte */ \
		LOGICAL_MAXIMUM, 0x01, /* Logical Maximum: 1 */ \
		USAGE, 0x47, /* Usage: Confidence */ \
		USAGE, 0x42, /* Usage: Tip switch */ \
		REPORT_COUNT, 0x02, /* Report Count: 2 */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_SIZE, 0x01, /* Report Size: 1 */ \
		REPORT_COUNT, 0x06, /* Report Count: 6 */ \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		/* End of a byte */ \
		/* Begin of 4 bytes */ \
		REPORT_COUNT, 0x01, /* Report Count: 1 */ \
		REPORT_SIZE, 0x20, /* Report Size: 0x10 (2 bytes) */ \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0xff, 0xff, /* Logical Maximum: 0xffffffff */ \
		USAGE, 0x51, /* Usage: Contract Identifier */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
		/* Begin of 4 bytes */ \
		/* Size is hard-coded at this moment */ \
		USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
		LOGICAL_MAXIMUM_2, 0x20, 0x4e, /* Logical Maximum: 20000 (See defintion) */ \
		REPORT_SIZE, 0x10, /* Report Size: 0x10 (2 bytes) */ \
		UNIT_EXPONENT, 0x0e, /* Unit exponent: -2 */ \
		UNIT, 0x11, /* Unit: SI Length (cm) */ \
		USAGE, 0x30, /* Usage: X */ \
		PHYSICAL_MAXIMUM_2, 0x2a, 0x06, /* Physical Maximum: 1578 = 157.8mm — measured, MacBookPro16,1 sensor surface width (was 1045/15" value; now matches Collection 1, see header comment) */ \
		REPORT_COUNT, 0x01, /* Report count: 1 */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		PHYSICAL_MAXIMUM_2, 0xd2, 0x03, /* Physical Maximum: 978 = 97.8mm — measured, MacBookPro16,1 sensor surface height (was 750/15" value; now matches Collection 1) */ \
		LOGICAL_MAXIMUM_2, 0xe0, 0x2e, /* Logical Maximum: 12000 (See definition) */ \
		USAGE, 0x31, /* Usage: Y */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		/* End of 4 bytes */ \
	END_COLLECTION /* End Collection */ \

// Separate mouse collection for synthetic right-click delivery.
#define AAPL_WELLSPRING_T2_FORCETOUCH_MOUSE_TLC \
	USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
	USAGE, 0x02, /* Usage: Mouse */ \
	BEGIN_COLLECTION, 0x01, /* Begin Collection: Application */ \
		REPORT_ID, REPORTID_STANDARDMOUSE, \
		USAGE, 0x01, /* Usage: Pointer */ \
		BEGIN_COLLECTION, 0x00, /* Begin Collection: Physical */ \
			USAGE_PAGE, 0x09, /* Usage Page: Button */ \
			USAGE_MINIMUM, 0x01, /* Button 1 */ \
			USAGE_MAXIMUM, 0x03, /* Button 3 */ \
			LOGICAL_MINIMUM, 0x00, \
			LOGICAL_MAXIMUM, 0x01, \
			REPORT_COUNT, 0x03, \
			REPORT_SIZE, 0x01, \
			INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
			REPORT_COUNT, 0x01, \
			REPORT_SIZE, 0x05, \
			INPUT, 0x03, /* Input: (Const, Var, Abs) - padding */ \
			USAGE_PAGE, 0x01, /* Usage Page: Generic Desktop */ \
			USAGE, 0x30, /* Usage: X */ \
			USAGE, 0x31, /* Usage: Y */ \
			LOGICAL_MINIMUM, 0x81, /* -127 */ \
			LOGICAL_MAXIMUM, 0x7f, /* 127 */ \
			/* AUDIT FIX (cursor teleport to bottom-right on force */ \
			/* touch): Unit/Unit Exponent/Physical Min/Max are HID */ \
			/* GLOBAL items - they persist across top-level */ \
			/* collection boundaries, not just within one. The PTP */ \
			/* TLC above leaves Unit = Time(Second), Unit Exponent */ \
			/* = -4, and Physical Maximum = 0xffffffff in effect */ \
			/* from its Scan Time field and never resets them before */ \
			/* this (later) collection. Without an explicit reset */ \
			/* here, X/Y silently inherited that huge bogus physical */ \
			/* range, which is what drove the cursor to the extreme */ \
			/* (bottom-right) logical position. Same reset already */ \
			/* used at the end of AAPL_WELLSPRING_T2_PTP_FINGER_ */ \
			/* COLLECTION_1/2 above, for the identical reason. */ \
			PHYSICAL_MINIMUM, 0x00, \
			PHYSICAL_MAXIMUM, 0x00, \
			UNIT_EXPONENT, 0x00, \
			UNIT, 0x00, \
			REPORT_SIZE, 0x08, \
			REPORT_COUNT, 0x02, \
			INPUT, 0x06, /* Input: (Data, Var, Rel) - always sent as 0 */ \
		END_COLLECTION, /* End Collection */ \
	END_COLLECTION /* End Collection */

#define AAPL_WELLSPRING_T2_PTP_TLC \
	USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
	USAGE, 0x05, /* Usage: Touch Pad */ \
	BEGIN_COLLECTION, 0x01, /* Begin Collection: Application */ \
		REPORT_ID, REPORTID_MULTITOUCH, /* Report ID: Multi-touch */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1, /* 1 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1, /* 2 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_2, /* 3 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_1, /* 4 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		USAGE, 0x22, /* Usage: Finger */ \
		AAPL_WELLSPRING_T2_PTP_FINGER_COLLECTION_2, /* 5 */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		UNIT_EXPONENT, 0x0c, /* Unit exponent: -4 */ \
		UNIT_2, 0x01, 0x10, /* Time: Second */ \
		PHYSICAL_MAXIMUM_3, 0xff, 0xff, 0x00, 0x00, \
		LOGICAL_MAXIMUM_3, 0xff, 0xff, 0x00, 0x00, \
		USAGE, 0x56, /* Usage: Scan Time */ \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		USAGE, 0x54, /* Usage: Contact Count */ \
		LOGICAL_MAXIMUM, 0x7f, \
		REPORT_SIZE, 0x08, \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		USAGE_PAGE, 0x09, /* Usage Page: Button */ \
		USAGE, 0x01, /* Button 1 */ \
		LOGICAL_MAXIMUM, 0x01, \
		REPORT_SIZE, 0x01, \
		INPUT, 0x02, /* Input: (Data, Var, Abs) */ \
		REPORT_COUNT, 0x07, \
		INPUT, 0x03, /* Input: (Const, Var, Abs) */ \
		USAGE_PAGE, 0x0d, /* Usage Page: Digitizer */ \
		REPORT_ID, REPORTID_DEVICE_CAPS, \
		USAGE, 0x55, /* Usage: Maximum Contacts */ \
		USAGE, 0x59, /* Usage: Touchpad Button Type*/ \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM_2, 0xff, 0x00, \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT, 0x02, \
		FEATURE, 0x02, \
		USAGE_PAGE_1, 0x00, 0xff, \
		REPORT_ID, REPORTID_PTPHQA, \
		USAGE, 0xc5, \
		LOGICAL_MINIMUM, 0x00, \
		LOGICAL_MAXIMUM_2, 0xff, 0x00, \
		REPORT_SIZE, 0x08, \
		REPORT_COUNT_2, 0x00, 0x01, \
		FEATURE, 0x02, \
	END_COLLECTION /* End Collection */

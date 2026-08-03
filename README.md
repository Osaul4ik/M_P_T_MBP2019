# WellspringPTP

**Windows Precision Touchpad driver for Apple MacBook T2 trackpads.**

WellspringPTP is a Windows Precision Touchpad driver focused on delivering a native, responsive, and reliable experience on Apple MacBook T2 hardware.

The project started as a fork of **mac-precision-touchpad** and has since evolved into a significantly reworked implementation for Apple T2 MacBooks.

---

# Current Status

The USB implementation for **MacBook Pro 16,1 (2019)** is considered feature-complete and is now primarily in the tuning and stabilization phase.

The driver has been extensively tested on this device and is optimized specifically for its trackpad.

---

# Features

* Windows Precision Touchpad support
* Native multi-touch gestures
* Reworked contact matching pipeline
* Stable Contact ID management
* Improved cursor stability
* Enhanced palm rejection
* Overflow packet handling
* Force Touch for open context menu
* Low-latency input processing
* Optimized scrolling behavior
* Production-oriented architecture focused on correctness and maintainability

---

# Supported Devices

### Fully supported

* Apple MacBook Pro 16,1 (2019, T2)

### Testing

MacBook Pro:
- MacBook Pro 2015 13" (12,1) — Force Touch, no Touch Bar
- MacBook Pro 2018 15" (15,1)
- MacBook Pro 2018 13", 4× TB3 (15,2)
- MacBook Pro 2019 15" (15,3) — shares the same trackpad/PID as 15,1
- MacBook Pro 2019 13", 2× TB3 (15,4)
- MacBook Pro 2019 16" (16,1)
- MacBook Pro 2020 13", 4× TB3 (16,2)
- MacBook Pro 2020 13", 2× TB3 (16,3)
- MacBook Pro 2020 16" (16,4) — shares the same trackpad/PID as 16,1

MacBook Air:
- MacBook Air 2013 11" (6,1)
- MacBook Air 2013 13" (6,2)
- MacBook Air 2015 11" (7,1) — shares the same trackpad/PID as 6,1
- MacBook Air 2015 13" (7,2) — shares the same trackpad/PID as 6,2
- MacBook Air 2018 (8,1)
- MacBook Air 2019 (8,2) — shares the same trackpad/PID as 8,1 (not verified on real hardware)
- MacBook Air 2020 (9,1)

### Partial

MacBookPro13,x / 14,x (2016-2017, Touch Bar, T1) — PID 0277. Falls back to an imprecise generic entry

### Other MacBooks SPI

MacBookPro13,1 / 14,1 (2016-2017, no Touch Bar) — SPI, not USB
MacBook 12" (2015-2017) — SPI, not USB
- Use https://github.com/imbushuo/mac-precision-touchpad

### Magic TrackPad 2

* Use https://github.com/vitoplantamura/MagicTrackpad2ForWindows

---

# 📦 Installation
0. Extract the archive.
1. Run TestMode.bat, enter 1, and press Enter to enable Test Mode.
2. **Restart your computer.** (Really)

3. Run InstallSert.bat to install the certificate.
4. Right-click AmtPtpDeviceUsbKm.inf and select "Install".

> **Note**
> The driver currently requires Windows Test Mode because it is not digitally signed.

---

# Driver Removal
This step is important before installing another Apple touchpad driver (Trackpad++, Magic Utilities, etc.).

1. Open **Device Manager**.
2. Find touchpad driver "Wellspring Precision Touchpad" in "HID" section, open and select "Delete"
3. Enable **Delete the driver software for this device**.
4. Scan for hardware changes or reboot. (Use Alt key in Device Manager and arrows or Win key and choose reboot)

---

# Development Status

* ✅ Contact lifecycle redesign
* ✅ Stable contact matching
* ✅ Palm rejection
* ✅ Gesture stability improvements
* ✅ Scroll improvements
* ✅ Cursor jump fixes
* ✅ Force Touch implementation
* ✅  Driver optimization
* ✅  Code audit
  

# Credits

This project is based on the excellent work of the original **mac-precision-touchpad** project by imbushuo.

While the original project provided the foundation, the USB implementation has undergone substantial architectural redesign and behavior changes focused on MacBook Pro 16,1.

---

# License
* USB driver — GPL v2

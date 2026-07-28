# WellspringPTP

**Windows Precision Touchpad driver for Apple MacBook T2 trackpads.**

WellspringPTP is a Windows Precision Touchpad driver focused on delivering a native, responsive, and reliable experience on Apple MacBook T2 hardware.

The project started as a fork of **mac-precision-touchpad** and has since evolved into a significantly reworked implementation for Apple T2 MacBooks. Support for non-T2 MacBook models remains based on the original implementation.

---

# Current Status

The USB implementation for **MacBook Pro 16,1 (2019)** is considered feature-complete and is now primarily in the tuning and stabilization phase.

The driver has been extensively tested on this device and is optimized specifically for its trackpad.

Support for other Apple T2 devices may be incomplete or require additional tuning.

---

# Features

* Windows Precision Touchpad support
* Native multi-touch gestures
* Reworked contact matching pipeline
* Stable Contact ID management
* Improved cursor stability
* Enhanced palm rejection
* Overflow packet handling
* Force Touch for open context menu (Next release)
* Low-latency input processing
* Optimized scrolling behavior
* Production-oriented architecture focused on correctness and maintainability

---

# Supported Devices

### Fully supported

* Apple MacBook Pro 16,1 (2019, T2)

### Experimental

* Other USB Apple trackpads supported by the original project work, but are not actively maintained or tested.

---

# Installation

1. Install the WellspringPTP driver.
2. Reboot if required.

> **Note**
>
> The driver currently requires Windows Test Mode because it is not digitally signed.

---

# Driver Removal
This step is important before installing another Apple touchpad driver (Trackpad++, Magic Utilities, etc.).

1. Open **Device Manager**.
2. Find touchpad driver in HID section
3. Enable **Delete the driver software for this device**.
4. Scan for hardware changes or reboot. (Use Alt key in Device Manager and arrows)

---

# Development Status

## USBKM Driver (MacBook Pro 16,1)

* ✅ Contact lifecycle redesign
* ✅ Stable contact matching
* ✅ Palm rejection
* ✅ Gesture stability improvements
* ✅ Scroll improvements
* ✅ Cursor jump fixes
* ✅ Force Touch implementation
* ❌  Driver optimization
* ❌  Code audit 

---

# Other Drivers

The repository also contains additional driver implementations inherited from the original project.

These components are currently included largely unchanged and have not received the same level of redesign, testing, or optimization as the USBKM implementation for the MacBook Pro 16,1.

Only the USBKM driver is actively developed and maintained within this project.

# Credits

This project is based on the excellent work of the original **mac-precision-touchpad** project by imbushuo.

While the original project provided the foundation, the USB implementation has undergone substantial architectural redesign and behavior changes focused on MacBook Pro 16,1.

---

# License

* USB driver — GPL v2
* SPI driver — MIT

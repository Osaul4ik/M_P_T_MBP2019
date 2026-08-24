# WellspringPTP

WellspringPTP is a reworked Windows Precision Touchpad driver for Apple MacBook trackpads. The project originated from mac-precision-touchpad and has been redesigned and have a dedicated configuration GUI.

---

# Current Status

Stable 

The driver has been tested on Apple MacBook hardware, including:
* MacBook Pro 16,1 (2019, T2)
* MacBook Air 2015

> **Recommended:** Set the Windows pointer speed to **6**.

---

# Main features

## Windows Precision Touchpad

* Native Windows Precision Touchpad input.
* Native multi-touch gestures (pinch, zoom, scroll, swipe)
* Improved contact matching and lifecycle.

## Palm rejection

Palm rejection is runtime configurable instead of being limited to compile-time constants.

The GUI exposes:
* edge rejection zones;
* major/minor contact thresholds;
* palm shape ratio;
* palm score threshold;
* minimum contact dimensions.
The configuration is shared through a stable public configuration structure and IOCTL interface.

## Force Touch

For trackpads with a real pressure channel, the driver supports configurable Force Touch click arbitration.

Available controls include:
* Force Touch enable/disable;
* pressure threshold;
* optional pressure gate;
* optional continuous-pressure requirement;

## Force Touch emulation

For trackpads without a hardware pressure channel, the driver can emulate Force Touch from a mechanical hard tap.

The emulation path:
* detects a hard tap;
* starts a pending click arbitration state;
* waits for the configured hold duration while the press remains active;
* converts the press into the configured Force Touch action when the hold time expires.

The emulation hold duration is configurable from 200 ms to 2000 ms in 50 ms steps.
The emulation path also has its own drag-cancel distance so that movement can cancel the pending Force Touch operation.

Force Touch/Emu action:
* context menu;
* middle mouse button;
* double click;
* independent drag-cancel distance.
---

# Wellspring Control Center

The application is a .NET 8 WPF desktop application with Windows Forms support for the tray UI.

The GUI provides:
* Device connection/status information;
* Palm settings;
* Pointer settings;
* Scroll settings;
* Force Touch controls;
* Force Touch emulation controls;
* User profiles;
* Live touch/contact monitoring;
* Touchpad geometry information;
* Driver debug logging control;
* Tray controls for commonly used settings;

Force Touch controls are exposed only when the connected device reports actual Force Touch support. On non-Force-Touch devices, the GUI can expose the software emulation controls instead.

Two GUI versions are included:

- **Min** — requires **.NET 8 Desktop Runtime (x64)** to be installed separately.
- **Full** — includes the required .NET Runtime and does not require a separate .NET installation.

---
# Known Limitations

- Only USB Apple touchpads are supported.
---

# Supported Devices

### Fully supported

* Apple MacBook Pro 16,1 (2019, T2)
* MacBook Air 2015 13" (7,2)

### Expected to work (not yet tested)

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
- MacBook Air 2018 (8,1)
- MacBook Air 2019 (8,2) — shares the same trackpad/PID as 8,1 (not verified on real hardware)
- MacBook Air 2020 (9,1)

### Partial

MacBookPro13,x / 14,x (2016-2017, Touch Bar, T1) — PID 0277. Falls back to an imprecise generic entry

### Other MacBooks SPI
MacBookPro13,1 / 14,1 (2016-2017, no Touch Bar) 
MacBook 12" (2015-2017)

- Use https://github.com/imbushuo/mac-precision-touchpad

### Magic TrackPad 2

* Use https://github.com/vitoplantamura/MagicTrackpad2ForWindows

---

# 📦 Installation

> **Note**
> The driver requires Windows Test Mode;

0. Extract the archive.
1. Run **TestMode.bat**, enter 1, and press Enter to enable Test Mode.
2. **Restart your computer.** (Really)
3. Run **InstallSert.bat** to install the certificate.
4. Right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
5. Reboot
---

# 🚀 Update

### Clean Update

1. Open Device Manager.
2. Under **HID**, find **Wellspring Precision Touchpad**, right-click it, and select **Uninstall device**.
3. Check **Delete the driver software for this device**, then click **Uninstall**.
4. Extract the archive, then right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
> Use keys (Arrows, Tab, Alt or mouse), when touchpad driver not installed.

### Normal Update

1. Extract the archive, then right-click **AmtPtpDeviceUsbKm.inf** and select **Install**.
---

# Driver Removal

This step is important before installing another Apple touchpad driver (Trackpad++, Magic Utilities, etc.).

1. Open **Device Manager**.
2. Find touchpad driver "Wellspring Precision Touchpad" in "HID" section, open and select "Delete"
3. Enable **Delete the driver software for this device**.
4. Scan for hardware changes or reboot. (Use Alt key in Device Manager and arrows or Win key and choose reboot)

---

# Tested with:

* Driver Verifier
* PREfast
* CodeQL
* Static Analysis

---

### ⚠️ Known Issues

- Fast Force Touch → Hard Tap: may occasionally be missed before reaching the input queue.
 
---

# Credits

This project is based on the excellent work of the original **mac-precision-touchpad** project by imbushuo.

---

# License
* USB driver — GPL v2

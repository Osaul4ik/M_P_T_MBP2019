using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// Talks to AmtPtpDeviceUsbKm.sys through its KMDF control-device
    /// symbolic link (\\.\AmtPtpDeviceUsbKm) using DeviceIoControl.
    /// The USB/HID filter itself is intentionally not opened by the GUI.
    /// </summary>
    public sealed class DeviceIo : IDisposable
    {
        // AMT_PTP_IOCTL_INDEX (0x900) + offset, mirrored from Public.h.
        private const uint FileDeviceUnknown = 0x00000022;
        private const uint MethodBuffered = 0;
        private const uint FileAnyAccess = 0;
        private const uint AmtPtpIoctlIndex = 0x900;

        private static uint CtlCode(
            uint deviceType,
            uint function,
            uint method,
            uint access) =>
            (deviceType << 16) |
            (access << 14) |
            (function << 2) |
            method;

        public static readonly uint IoctlGetPalmConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 0,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlSetPalmConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 1,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlGetPadGeometry =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 2,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlResetPalmConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 3,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlSetLiveEnabled =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 4,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlGetLiveFrame =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 5,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlGetPointerConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 6,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlSetPointerConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 7,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlResetPointerConfig =
            CtlCode(
                FileDeviceUnknown,
                AmtPtpIoctlIndex + 8,
                MethodBuffered,
                FileAnyAccess);

        public static readonly uint IoctlGetScrollConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 9, MethodBuffered, FileAnyAccess);
        public static readonly uint IoctlSetScrollConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 10, MethodBuffered, FileAnyAccess);
        public static readonly uint IoctlResetScrollConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 11, MethodBuffered, FileAnyAccess);

        private const string ControlDevicePath = @"\\.\AmtPtpDeviceUsbKm";

        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;
        private const uint FileShareRead = 0x1;
        private const uint FileShareWrite = 0x2;
        private const uint OpenExisting = 3;

        private SafeFileHandle? _handle;

        public bool IsConnected =>
            _handle != null && !_handle.IsInvalid;

        /// <summary>
        /// Human-readable reason the last TryConnect() call failed.
        /// </summary>
        public string LastErrorMessage { get; private set; } =
            string.Empty;

        private bool Fail(string step)
        {
            int err = Marshal.GetLastWin32Error();

            string text =
                new Win32Exception(err).Message.Trim();

            LastErrorMessage =
                $"{step}: {text} (Win32 {err})";

            return false;
        }

        /// <summary>
        /// Opens the driver's dedicated KMDF control device.
        /// </summary>
        public bool TryConnect()
        {
            Disconnect();

            LastErrorMessage = string.Empty;

            var handle = CreateFile(
                ControlDevicePath,
                GenericRead | GenericWrite,
                FileShareRead | FileShareWrite,
                IntPtr.Zero,
                OpenExisting,
                0,
                IntPtr.Zero);

            if (handle.IsInvalid)
            {
                return Fail($"CreateFile('{ControlDevicePath}')");
            }

            _handle = handle;
            return true;
        }

        public void Disconnect()
        {
            _handle?.Dispose();
            _handle = null;
        }

        public bool TryGetPalmConfig(
            out PalmConfig config)
        {
            config = PalmConfig.Default;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlGetPalmConfig,
                null,
                ref config);
        }

        public bool TrySetPalmConfig(
            PalmConfig request,
            out PalmConfig applied)
        {
            applied = request;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlSetPalmConfig,
                (PalmConfig?)request,
                ref applied);
        }

        public bool TryResetPalmConfig(
            out PalmConfig applied)
        {
            applied = PalmConfig.Default;

            if (!IsConnected)
                return false;

            bool ok = DeviceIoControl(
                _handle!,
                IoctlResetPalmConfig,
                IntPtr.Zero,
                0,
                IntPtr.Zero,
                0,
                out _,
                IntPtr.Zero);

            if (!ok)
                return false;

            // Driver doesn't echo a buffer back for RESET - just re-fetch.
            return TryGetPalmConfig(out applied);
        }

        public bool TryGetPointerConfig(
            out PointerConfig config)
        {
            config = PointerConfig.Default;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlGetPointerConfig,
                null,
                ref config);
        }

        public bool TrySetPointerConfig(
            PointerConfig request,
            out PointerConfig applied)
        {
            applied = request;

            if (!IsConnected)
                return false;

            return TryIoctl(
                IoctlSetPointerConfig,
                (PointerConfig?)request,
                ref applied);
        }

        public bool TryResetPointerConfig(
            out PointerConfig applied)
        {
            applied = PointerConfig.Default;

            if (!IsConnected)
                return false;

            bool ok = DeviceIoControl(
                _handle!,
                IoctlResetPointerConfig,
                IntPtr.Zero,
                0,
                IntPtr.Zero,
                0,
                out _,
                IntPtr.Zero);

            if (!ok)
                return false;

            // Driver doesn't echo a buffer back for RESET - just re-fetch.
            return TryGetPointerConfig(out applied);
        }

        public bool TryGetScrollConfig(out ScrollConfig config)
        {
            config = ScrollConfig.Default;
            if (!IsConnected) return false;
            return TryIoctl(IoctlGetScrollConfig, null, ref config);
        }

        public bool TrySetScrollConfig(ScrollConfig request, out ScrollConfig applied)
        {
            applied = request;
            if (!IsConnected) return false;
            return TryIoctl(IoctlSetScrollConfig, (ScrollConfig?)request, ref applied);
        }

        public bool TryResetScrollConfig(out ScrollConfig applied)
        {
            applied = ScrollConfig.Default;
            if (!IsConnected) return false;
            bool ok = DeviceIoControl(_handle!, IoctlResetScrollConfig, IntPtr.Zero, 0, IntPtr.Zero, 0, out _, IntPtr.Zero);
            return ok && TryGetScrollConfig(out applied);
        }

        public bool TryGetPadGeometry(
            out PadGeometry geometry)
        {
            geometry = PadGeometry.Fallback;

            if (!IsConnected)
                return false;

            int size =
                Marshal.SizeOf<PadGeometry>();

            IntPtr outBuf =
                Marshal.AllocHGlobal(size);

            try
            {
                bool ok = DeviceIoControl(
                    _handle!,
                    IoctlGetPadGeometry,
                    IntPtr.Zero,
                    0,
                    outBuf,
                    (uint)size,
                    out uint bytesReturned,
                    IntPtr.Zero);

                if (!ok || bytesReturned < size)
                    return false;

                var result =
                    Marshal.PtrToStructure<PadGeometry>(
                        outBuf);

                if (!result.IsValid)
                    return false;

                geometry = result;

                return true;
            }
            finally
            {
                Marshal.FreeHGlobal(outBuf);
            }
        }

        private bool TryIoctl(
            uint code,
            PalmConfig? input,
            ref PalmConfig output)
        {
            int size =
                Marshal.SizeOf<PalmConfig>();

            IntPtr inBuf = IntPtr.Zero;

            IntPtr outBuf =
                Marshal.AllocHGlobal(size);

            try
            {
                uint inSize = 0;

                if (input.HasValue)
                {
                    inBuf =
                        Marshal.AllocHGlobal(size);

                    Marshal.StructureToPtr(
                        input.Value,
                        inBuf,
                        false);

                    inSize =
                        (uint)size;
                }

                bool ok = DeviceIoControl(
                    _handle!,
                    code,
                    inBuf,
                    inSize,
                    outBuf,
                    (uint)size,
                    out uint bytesReturned,
                    IntPtr.Zero);

                if (!ok || bytesReturned < size)
                    return false;

                output =
                    Marshal.PtrToStructure<PalmConfig>(
                        outBuf);

                return true;
            }
            finally
            {
                if (inBuf != IntPtr.Zero)
                    Marshal.FreeHGlobal(inBuf);

                Marshal.FreeHGlobal(outBuf);
            }
        }

        private bool TryIoctl(
            uint code,
            PointerConfig? input,
            ref PointerConfig output)
        {
            int size =
                Marshal.SizeOf<PointerConfig>();

            IntPtr inBuf = IntPtr.Zero;

            IntPtr outBuf =
                Marshal.AllocHGlobal(size);

            try
            {
                uint inSize = 0;

                if (input.HasValue)
                {
                    inBuf =
                        Marshal.AllocHGlobal(size);

                    Marshal.StructureToPtr(
                        input.Value,
                        inBuf,
                        false);

                    inSize =
                        (uint)size;
                }

                bool ok = DeviceIoControl(
                    _handle!,
                    code,
                    inBuf,
                    inSize,
                    outBuf,
                    (uint)size,
                    out uint bytesReturned,
                    IntPtr.Zero);

                if (!ok || bytesReturned < size)
                    return false;

                output =
                    Marshal.PtrToStructure<PointerConfig>(
                        outBuf);

                return true;
            }
            finally
            {
                if (inBuf != IntPtr.Zero)
                    Marshal.FreeHGlobal(inBuf);

                Marshal.FreeHGlobal(outBuf);
            }
        }

        public bool SetLiveEnabled(bool enabled)
        {
            if (!IsConnected)
                return false;

            IntPtr inBuf = Marshal.AllocHGlobal(sizeof(int));
            try
            {
                Marshal.WriteInt32(inBuf, enabled ? 1 : 0);

                return DeviceIoControl(
                    _handle!,
                    IoctlSetLiveEnabled,
                    inBuf,
                    sizeof(int),
                    IntPtr.Zero,
                    0,
                    out _,
                    IntPtr.Zero);
            }
            finally
            {
                Marshal.FreeHGlobal(inBuf);
            }
        }

        public bool TryGetLiveFrame(out LiveFrame frame)
        {
            frame = default;

            if (!IsConnected)
                return false;

            int size = Marshal.SizeOf<LiveFrame>();
            IntPtr outBuf = Marshal.AllocHGlobal(size);

            try
            {
                bool ok = DeviceIoControl(
                    _handle!,
                    IoctlGetLiveFrame,
                    IntPtr.Zero,
                    0,
                    outBuf,
                    (uint)size,
                    out uint bytesReturned,
                    IntPtr.Zero);

                if (!ok || bytesReturned < size)
                    return false;

                frame = Marshal.PtrToStructure<LiveFrame>(outBuf);
                // StructVersion 2 added Major/Minor geometry fields to
                // LiveContact; a v1 driver will report a smaller
                // bytesReturned and fail the size check above already,
                // but keep an explicit version gate too.
                return frame.StructVersion == 2;
            }
            finally
            {
                Marshal.FreeHGlobal(outBuf);
            }
        }

        public void Dispose() =>
            Disconnect();

        // ---- P/Invoke plumbing ---------------------------------------------

        [DllImport(
            "kernel32.dll",
            SetLastError = true,
            CharSet = CharSet.Auto)]
        private static extern SafeFileHandle CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            IntPtr securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport(
            "kernel32.dll",
            SetLastError = true)]
        private static extern bool DeviceIoControl(
            SafeFileHandle device,
            uint ioControlCode,
            IntPtr inBuffer,
            uint inBufferSize,
            IntPtr outBuffer,
            uint outBufferSize,
            out uint bytesReturned,
            IntPtr overlapped);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LiveContact
    {
        public uint ContactID;
        public ushort X;       // normalized X
        public ushort Y;       // normalized Y
        public uint Phase;
        public byte Confident;
        public byte PalmSuspect;
        public ushort Reserved;
        public short RawX;     // exact raw USB abs_x
        public short RawY;     // exact raw USB abs_y
        public ushort Major;   // touch_major, raw sensor units
        public ushort Minor;   // touch_minor, raw sensor units
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct LiveFrame
    {
        public uint StructVersion;
        public uint Sequence;
        public long TimestampQpc;
        public byte ContactCount;
        public byte RawContactCount;
        public byte LargePalmBlanked;
        public byte ButtonDown;
        public byte ForceTouchClick;
        public byte ButtonClickReport;
        public ushort Reserved0;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 5)]
        public LiveContact[] Contacts;
    }

    /// <summary>
    /// Minimal SafeHandle for a Win32 file handle from CreateFile.
    /// </summary>
    internal sealed class SafeFileHandle :
        Microsoft.Win32.SafeHandles.SafeHandleZeroOrMinusOneIsInvalid
    {
        public SafeFileHandle()
            : base(true)
        {
        }

        protected override bool ReleaseHandle() =>
            CloseHandleNative(handle);

        [DllImport("kernel32.dll")]
        private static extern bool CloseHandleNative(
            IntPtr handle);
    }
}
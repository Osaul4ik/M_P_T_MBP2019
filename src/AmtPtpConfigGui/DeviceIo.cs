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
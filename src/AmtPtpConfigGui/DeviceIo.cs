using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// Talks to AmtPtpDeviceUsbKm.sys over its custom device interface
    /// (GUID_DEVINTERFACE_AmtPtpDeviceUsbKm, Public.h) using SetupAPI to
    /// find the device path and DeviceIoControl for the IOCTL_AMT_PTP_*
    /// calls implemented in ConfigIoctl.c.
    /// </summary>
    public sealed class DeviceIo : IDisposable
    {
        // {4aa332cc-5777-4afd-aa4e-95387330612a} - must match
        // GUID_DEVINTERFACE_AmtPtpDeviceUsbKm in Public.h exactly.
        private static readonly Guid InterfaceGuid =
            new Guid("4aa332cc-5777-4afd-aa4e-95387330612a");

        // AMT_PTP_IOCTL_INDEX (0x900) + offset, mirrored from Public.h.
        private const uint FileDeviceUnknown = 0x00000022;
        private const uint MethodBuffered = 0;
        private const uint FileAnyAccess = 0;
        private const uint AmtPtpIoctlIndex = 0x900;

        private static uint CtlCode(uint deviceType, uint function, uint method, uint access) =>
            (deviceType << 16) | (access << 14) | (function << 2) | method;

        public static readonly uint IoctlGetPalmConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 0, MethodBuffered, FileAnyAccess);

        public static readonly uint IoctlSetPalmConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 1, MethodBuffered, FileAnyAccess);

        public static readonly uint IoctlGetPadGeometry =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 2, MethodBuffered, FileAnyAccess);

        public static readonly uint IoctlResetPalmConfig =
            CtlCode(FileDeviceUnknown, AmtPtpIoctlIndex + 3, MethodBuffered, FileAnyAccess);

        private SafeFileHandle? _handle;

        public bool IsConnected => _handle != null && !_handle.IsInvalid;

        /// <summary>
        /// Human-readable reason the last TryConnect() call failed.
        /// </summary>
        public string LastErrorMessage { get; private set; } = string.Empty;

        private bool Fail(string step)
        {
            int err = Marshal.GetLastWin32Error();
            string text = new Win32Exception(err).Message.Trim();
            LastErrorMessage = $"{step}: {text} (Win32 {err})";
            return false;
        }

        /// <summary>
        /// Finds the first live AmtPtpDeviceUsbKm device interface
        /// and opens a handle to it.
        /// </summary>
        public bool TryConnect()
        {
            Disconnect();
            LastErrorMessage = string.Empty;

            IntPtr deviceInfoSet = SetupDiGetClassDevs(
                ref InterfaceGuidLocal,
                IntPtr.Zero,
                IntPtr.Zero,
                DigcfPresent | DigcfDeviceinterface);

            if (deviceInfoSet == IntPtr.Zero || deviceInfoSet.ToInt64() == -1)
            {
                return Fail("SetupDiGetClassDevs");
            }

            try
            {
                var ifData = new SP_DEVICE_INTERFACE_DATA();
                ifData.cbSize = Marshal.SizeOf(ifData);

                if (!SetupDiEnumDeviceInterfaces(
                        deviceInfoSet,
                        IntPtr.Zero,
                        ref InterfaceGuidLocal,
                        0,
                        ref ifData))
                {
                    return Fail(
                        "SetupDiEnumDeviceInterfaces (no device interface present)");
                }

                uint requiredSize = 0;

                SetupDiGetDeviceInterfaceDetail(
                    deviceInfoSet,
                    ref ifData,
                    IntPtr.Zero,
                    0,
                    ref requiredSize,
                    IntPtr.Zero);

                if (requiredSize == 0)
                {
                    return Fail(
                        "SetupDiGetDeviceInterfaceDetail (size query)");
                }

                IntPtr detailBuffer =
                    Marshal.AllocHGlobal((int)requiredSize);

                try
                {
                    /*
                     * SP_DEVICE_INTERFACE_DETAIL_DATA:
                     *
                     * On x64 Windows, cbSize must be 8.
                     * On x86 Windows, cbSize must be 6.
                     */
                    Marshal.WriteInt32(
                        detailBuffer,
                        IntPtr.Size == 8 ? 8 : 6);

                    if (!SetupDiGetDeviceInterfaceDetail(
                            deviceInfoSet,
                            ref ifData,
                            detailBuffer,
                            requiredSize,
                            ref requiredSize,
                            IntPtr.Zero))
                    {
                        return Fail(
                            "SetupDiGetDeviceInterfaceDetail (fetch)");
                    }

                    /*
                     * IMPORTANT:
                     *
                     * The DevicePath WCHAR buffer starts immediately
                     * after the 4-byte cbSize field for this manually
                     * allocated SetupAPI structure.
                     *
                     * Do NOT use +8 here.
                     */
                    string devicePath =
                        Marshal.PtrToStringUni(detailBuffer + 4)!;

                    var handle = CreateFile(
                        devicePath,
                        GenericRead | GenericWrite,
                        FileShareRead | FileShareWrite,
                        IntPtr.Zero,
                        OpenExisting,
                        0,
                        IntPtr.Zero);

                    if (handle.IsInvalid)
                    {
                        return Fail($"CreateFile('{devicePath}')");
                    }

                    _handle = handle;
                    return true;
                }
                finally
                {
                    Marshal.FreeHGlobal(detailBuffer);
                }
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(deviceInfoSet);
            }
        }

        public void Disconnect()
        {
            _handle?.Dispose();
            _handle = null;
        }

        public bool TryGetPalmConfig(out PalmConfig config)
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

        public bool TryResetPalmConfig(out PalmConfig applied)
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

        public bool TryGetPadGeometry(out PadGeometry geometry)
        {
            geometry = PadGeometry.Fallback;

            if (!IsConnected)
                return false;

            int size = Marshal.SizeOf<PadGeometry>();

            IntPtr outBuf = Marshal.AllocHGlobal(size);

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
                    Marshal.PtrToStructure<PadGeometry>(outBuf);

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
            int size = Marshal.SizeOf<PalmConfig>();

            IntPtr inBuf = IntPtr.Zero;
            IntPtr outBuf = Marshal.AllocHGlobal(size);

            try
            {
                uint inSize = 0;

                if (input.HasValue)
                {
                    inBuf = Marshal.AllocHGlobal(size);

                    Marshal.StructureToPtr(
                        input.Value,
                        inBuf,
                        false);

                    inSize = (uint)size;
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
                    Marshal.PtrToStructure<PalmConfig>(outBuf);

                return true;
            }
            finally
            {
                if (inBuf != IntPtr.Zero)
                    Marshal.FreeHGlobal(inBuf);

                Marshal.FreeHGlobal(outBuf);
            }
        }

        public void Dispose() => Disconnect();

        // ---- P/Invoke plumbing -------------------------------------------------

        private Guid InterfaceGuidLocal = InterfaceGuid;

        private const uint DigcfPresent = 0x02;
        private const uint DigcfDeviceinterface = 0x10;

        private const uint GenericRead = 0x80000000;
        private const uint GenericWrite = 0x40000000;

        private const uint FileShareRead = 0x1;
        private const uint FileShareWrite = 0x2;

        private const uint OpenExisting = 3;

        [StructLayout(LayoutKind.Sequential)]
        private struct SP_DEVICE_INTERFACE_DATA
        {
            public int cbSize;
            public Guid InterfaceClassGuid;
            public int Flags;
            public IntPtr Reserved;
        }

        [DllImport(
            "setupapi.dll",
            SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevs(
            ref Guid classGuid,
            IntPtr enumerator,
            IntPtr hwndParent,
            uint flags);

        [DllImport(
            "setupapi.dll",
            SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInterfaces(
            IntPtr deviceInfoSet,
            IntPtr deviceInfoData,
            ref Guid interfaceClassGuid,
            uint memberIndex,
            ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

        [DllImport(
            "setupapi.dll",
            SetLastError = true,
            CharSet = CharSet.Auto)]
        private static extern bool SetupDiGetDeviceInterfaceDetail(
            IntPtr deviceInfoSet,
            ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData,
            IntPtr deviceInterfaceDetailData,
            uint deviceInterfaceDetailDataSize,
            ref uint requiredSize,
            IntPtr deviceInfoData);

        [DllImport(
            "setupapi.dll",
            SetLastError = true)]
        private static extern bool SetupDiDestroyDeviceInfoList(
            IntPtr deviceInfoSet);

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
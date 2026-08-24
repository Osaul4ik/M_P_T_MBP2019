using System;
using System.Diagnostics;
using System.Runtime;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace AmtPtpConfigGui
{
    /// <summary>
    /// Trims the process's physical working set once the main window is
    /// hidden to the tray.
    ///
    /// Hiding a WPF window does not shrink what Task Manager shows: the CLR
    /// only returns *managed* garbage to its own heap on collection, and even
    /// then Windows keeps those freed pages mapped into the process's working
    /// set until something else needs the physical memory. That is why an
    /// idle tray-parked instance previously read the same "Memory" figure as
    /// the visible window - nothing had ever told the OS those pages were
    /// free to reclaim.
    ///
    /// SetProcessWorkingSetSize(hProcess, -1, -1) is the documented way to
    /// ask the OS to page out everything not immediately resident. It is
    /// non-destructive: touching any of that memory later (e.g. ShowFromTray
    /// rebuilding the live overlay) simply pages it back in on demand, so
    /// this has no effect on correctness - only on the number shown while
    /// parked in tray. A full, compacting, blocking GC runs first so there is
    /// actually dead managed memory for the OS to reclaim; trimming against a
    /// live, fragmented heap mostly no-ops.
    /// </summary>
    internal static class TrayMemoryTrimmer
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetProcessWorkingSetSize(
            IntPtr hProcess, IntPtr dwMinimumWorkingSetSize, IntPtr dwMaximumWorkingSetSize);

        private static readonly IntPtr TrimToMinimum = new IntPtr(-1);

        /// <summary>
        /// Call once the window has been Hide()-n and any per-frame work
        /// (live overlay polling/render timer) has already been stopped.
        /// Runs off the UI thread on a short delay so Hide() itself is never
        /// blocked by the GC pass, and so short show/hide flicker (e.g. the
        /// activation pipe re-showing the window almost immediately) doesn't
        /// pay for a trim that's about to be undone anyway.
        /// </summary>
        public static void TrimAfterHide()
        {
            _ = Task.Run(async () =>
            {
                await Task.Delay(TimeSpan.FromSeconds(2)).ConfigureAwait(false);

                try
                {
                    GCSettings.LargeObjectHeapCompactionMode = GCLargeObjectHeapCompactionMode.CompactOnce;
                    GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
                    GC.WaitForPendingFinalizers();
                    GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);

                    using Process process = Process.GetCurrentProcess();
                    SetProcessWorkingSetSize(process.Handle, TrimToMinimum, TrimToMinimum);
                }
                catch (Exception)
                {
                    // Best-effort. A failed collect/trim just leaves the
                    // working set where it was - never worth surfacing to
                    // the user over a tray-parked background process.
                }
            });
        }
    }
}

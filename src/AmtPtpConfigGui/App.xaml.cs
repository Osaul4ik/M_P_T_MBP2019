using System;
using System.IO.Pipes;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using Application = System.Windows.Application;

namespace AmtPtpConfigGui
{
    public partial class App : Application
    {
        // Single-instance gate. "Local\" (not "Global\") is correct here -
        // this app is per-user by design (per-user profile/settings store),
        // so we only need to de-duplicate within the current session, not
        // across all sessions on the box.
        private const string SingleInstanceMutexName = @"Local\WellspringPTP_ConfigGui_SingleInstance";
        private const string ActivationPipeName = "WellspringPTP_ConfigGui_ActivationPipe";

        // Held for the app's entire lifetime - a Mutex is released the
        // instant it's disposed/GC'd or the owning thread exits, and we
        // need "second launch sees this as taken" to hold from OnStartup
        // all the way to process exit.
        private Mutex? _singleInstanceMutex;
        private CancellationTokenSource? _pipeServerCts;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            _singleInstanceMutex = new Mutex(initiallyOwned: true, SingleInstanceMutexName, out bool createdNew);

            if (!createdNew)
            {
                // Another instance already owns the mutex. Hand off an
                // "activate" ping over the named pipe and exit immediately -
                // deliberately never construct MainWindow/NotifyIcon here,
                // so a second launch never doubles up tray icons or opens a
                // second DeviceIo handle to the driver.
                TryPingRunningInstance();
                Shutdown();
                return;
            }

            // app.manifest declares dpiAwareness=PerMonitorV2 for the whole
            // process, but that only controls the Win32/OS-level DPI
            // virtualization context - it does NOT flip WinForms' own
            // internal HighDpiMode flag, which is a separate opt-in that
            // WinForms normally gets from the SDK-generated Program.cs
            // (ApplicationConfiguration.Initialize()) in a pure WinForms
            // project. This project is UseWPF+UseWindowsForms together, so
            // WPF's own generated entry point is used instead and that call
            // never happens. Net effect without it: the OS correctly tells
            // Windows not to bitmap-stretch our windows (manifest works),
            // but WinForms' internal DeviceDpi/Font/Padding auto-scale
            // machinery (what RoundedContextMenuStrip.CurrentRadius below
            // relies on, and what ResolveTrayFont's
            // hand-measured pixel offsets implicitly assume) still computes
            // everything against the system/primary-monitor DPI. On a
            // 96 DPI internal panel (MacBook Air 2015 via Boot Camp/OpenCore)
            // that happens to match reality, so it looked "normal" there by
            // coincidence - on a scaled Retina external display it doesn't,
            // which is what read as the tray menu being squished. Must be
            // set before any WinForms control (NotifyIcon, ContextMenuStrip)
            // gets its window handle - first thing after the mutex/pipe
            // gate, before MainWindow (which creates _trayIcon) exists.
            System.Windows.Forms.Application.SetHighDpiMode(System.Windows.Forms.HighDpiMode.PerMonitorV2);
            System.Windows.Forms.Application.EnableVisualStyles();
            System.Windows.Forms.Application.SetCompatibleTextRenderingDefault(false);

            StartActivationPipeServer();

            var mainWindow = new MainWindow();
            MainWindow = mainWindow;
            mainWindow.Show();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            _pipeServerCts?.Cancel();
            _singleInstanceMutex?.ReleaseMutex();
            _singleInstanceMutex?.Dispose();
            base.OnExit(e);
        }

        // Best-effort: if the first instance is mid-startup/shutdown and
        // isn't listening yet, this silently no-ops (short connect timeout)
        // rather than hanging the second launch's process exit.
        private static void TryPingRunningInstance()
        {
            try
            {
                using var client = new NamedPipeClientStream(".", ActivationPipeName, PipeDirection.Out);
                client.Connect(500);
                client.WriteByte(1);
                client.Flush();
            }
            catch (Exception)
            {
                // No listener, timeout, or pipe busy - nothing else this
                // second launch can usefully do. It exits either way.
            }
        }

        // One NamedPipeServerStream, reused in a loop for the app's whole
        // lifetime (each client connection is a single one-byte ping from a
        // subsequent launch attempt, not a persistent channel).
        private void StartActivationPipeServer()
        {
            _pipeServerCts = new CancellationTokenSource();
            var token = _pipeServerCts.Token;

            Task.Run(async () =>
            {
                while (!token.IsCancellationRequested)
                {
                    try
                    {
                        using var server = new NamedPipeServerStream(
                            ActivationPipeName,
                            PipeDirection.In,
                            maxNumberOfServerInstances: 1,
                            PipeTransmissionMode.Byte,
                            PipeOptions.Asynchronous);

                        await server.WaitForConnectionAsync(token).ConfigureAwait(false);
                        _ = server.ReadByte();

                        Dispatcher.BeginInvoke(new Action(() =>
                        {
                            if (MainWindow is MainWindow mw)
                                mw.ActivateFromExternalLaunch();
                        }));
                    }
                    catch (OperationCanceledException)
                    {
                        // Normal path on shutdown (token cancelled while
                        // waiting for a connection).
                    }
                    catch (Exception)
                    {
                        // Transient pipe error - brief backoff, then keep
                        // serving; a broken listener here would silently
                        // regress every future launch back to "opens
                        // nothing," which is worse than one retry.
                        await Task.Delay(250, token).ConfigureAwait(false);
                    }
                }
            }, token);
        }
    }
}
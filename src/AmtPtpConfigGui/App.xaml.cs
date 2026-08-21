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
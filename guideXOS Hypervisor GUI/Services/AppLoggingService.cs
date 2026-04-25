using System.Diagnostics;
using System.IO;
using System.Text;

namespace guideXOS_Hypervisor_GUI.Services
{
    internal static class AppLoggingService
    {
        private static readonly object SyncRoot = new();
        private static TextWriter? _originalOut;
        private static TextWriter? _originalError;
        private static TextWriter? _logWriter;
        private static TraceListener? _traceListener;
        private static bool _initialized;

        public static string? LogDirectory { get; private set; }
        public static string? CurrentLogPath { get; private set; }

        public static void Initialize()
        {
            lock (SyncRoot)
            {
                if (_initialized)
                {
                    return;
                }

                LogDirectory = Path.Combine(AppContext.BaseDirectory, "logs");
                Directory.CreateDirectory(LogDirectory);

                string timestamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
                CurrentLogPath = Path.Combine(LogDirectory, $"app-{timestamp}.log");

                _originalOut = Console.Out;
                _originalError = Console.Error;

                var fileStream = new FileStream(CurrentLogPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite);
                _logWriter = TextWriter.Synchronized(new StreamWriter(fileStream, new UTF8Encoding(false))
                {
                    AutoFlush = true
                });

                Console.SetOut(new TeeTextWriter(_originalOut, _logWriter));
                Console.SetError(new TeeTextWriter(_originalError, _logWriter));

                _traceListener = new TextWriterTraceListener(_logWriter, "guideXOS file logger");
                Trace.Listeners.Add(_traceListener);
                Trace.AutoFlush = true;

                Console.WriteLine($"[{DateTime.Now:O}] guideXOS Hypervisor GUI logging started");
                Console.WriteLine($"Log file: {CurrentLogPath}");

                AppDomain.CurrentDomain.UnhandledException += OnUnhandledException;
                TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;

                _initialized = true;
            }
        }

        public static void Shutdown()
        {
            lock (SyncRoot)
            {
                if (!_initialized)
                {
                    return;
                }

                Console.WriteLine($"[{DateTime.Now:O}] guideXOS Hypervisor GUI logging stopped");

                AppDomain.CurrentDomain.UnhandledException -= OnUnhandledException;
                TaskScheduler.UnobservedTaskException -= OnUnobservedTaskException;

                if (_traceListener != null)
                {
                    Trace.Listeners.Remove(_traceListener);
                    _traceListener.Flush();
                    _traceListener.Dispose();
                    _traceListener = null;
                }

                if (_originalOut != null)
                {
                    Console.SetOut(_originalOut);
                }

                if (_originalError != null)
                {
                    Console.SetError(_originalError);
                }

                // _logWriter is already flushed and disposed by _traceListener.Dispose() above
                _logWriter = null;
                _initialized = false;
            }
        }

        private static void OnUnhandledException(object sender, UnhandledExceptionEventArgs e)
        {
            Console.Error.WriteLine($"[{DateTime.Now:O}] Unhandled exception: {e.ExceptionObject}");
        }

        private static void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
        {
            Console.Error.WriteLine($"[{DateTime.Now:O}] Unobserved task exception: {e.Exception}");
        }

        private sealed class TeeTextWriter : TextWriter
        {
            private readonly TextWriter _first;
            private readonly TextWriter _second;

            public TeeTextWriter(TextWriter first, TextWriter second)
            {
                _first = first;
                _second = second;
            }

            public override Encoding Encoding => _first.Encoding;

            public override void Write(char value)
            {
                _first.Write(value);
                _second.Write(value);
            }

            public override void Write(string? value)
            {
                _first.Write(value);
                _second.Write(value);
            }

            public override void WriteLine(string? value)
            {
                _first.WriteLine(value);
                _second.WriteLine(value);
            }

            public override void Flush()
            {
                _first.Flush();
                _second.Flush();
            }
        }
    }
}

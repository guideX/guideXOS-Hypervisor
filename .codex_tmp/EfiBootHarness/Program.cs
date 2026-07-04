using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

internal static class NativeApi
{
    private const string DllName = "guideXOS_Hypervisor.dll";

    private static readonly string RepoRoot =
        Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));

    private static readonly string NativeDllPath = Path.Combine(
        RepoRoot,
        "guideXOS Hypervisor GUI",
        "bin",
        "Debug",
        "net9.0-windows",
        DllName);

    static NativeApi()
    {
        NativeLibrary.SetDllImportResolver(typeof(NativeApi).Assembly, ResolveDllImport);
    }

    private static IntPtr ResolveDllImport(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, DllName, StringComparison.OrdinalIgnoreCase))
        {
            return IntPtr.Zero;
        }

        if (!File.Exists(NativeDllPath))
        {
            throw new FileNotFoundException($"Native DLL not found: {NativeDllPath}", NativeDllPath);
        }

        return NativeLibrary.Load(NativeDllPath);
    }

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr VMManager_Create();

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void VMManager_Destroy(IntPtr manager);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern IntPtr VMManager_CreateVM(IntPtr manager, [MarshalAs(UnmanagedType.LPStr)] string configJson);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)]
    internal static extern bool VMManager_StartVM(IntPtr manager, [MarshalAs(UnmanagedType.LPStr)] string vmId);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.I1)]
    internal static extern bool VMManager_StopVM(IntPtr manager, [MarshalAs(UnmanagedType.LPStr)] string vmId);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern ulong VMManager_RunVM(IntPtr manager, [MarshalAs(UnmanagedType.LPStr)] string vmId, ulong maxCycles);

    [DllImport(DllName, CallingConvention = CallingConvention.Cdecl)]
    internal static extern void VMManager_FreeString(IntPtr str);
}

internal static class Program
{
    private const string SeedEnvVar = "GUIDEXOS_EFI_DIAG_SEED_LOADER_LOCAL_5E018";

    private static int Main(string[] args)
    {
        bool seed = false;
        ulong cyclesPerBatch = 500_000;
        int batches = 20;

        foreach (string arg in args)
        {
            if (string.Equals(arg, "--seed", StringComparison.OrdinalIgnoreCase))
            {
                seed = true;
            }
            else if (arg.StartsWith("--cycles=", StringComparison.OrdinalIgnoreCase) &&
                     ulong.TryParse(arg.Substring("--cycles=".Length), out ulong parsedCycles))
            {
                cyclesPerBatch = parsedCycles;
            }
            else if (arg.StartsWith("--batches=", StringComparison.OrdinalIgnoreCase) &&
                     int.TryParse(arg.Substring("--batches=".Length), out int parsedBatches))
            {
                batches = parsedBatches;
            }
        }

        if (seed)
        {
            Environment.SetEnvironmentVariable(SeedEnvVar, "1");
        }

        string isoPath = @"D:\bkup\unsorted\debian-7.11.0-ia64-netinst.iso";
        string configJson = $@"{{
  ""name"":""diag-ia64-{(seed ? "seed" : "noseed")}"",
  ""cpu"":{{""isaType"":""IA-64"",""cpuCount"":1,""clockFrequency"":0,""enableProfiling"":true}},
  ""memory"":{{""memorySize"":536870912,""enableMMU"":true,""pageSize"":16384}},
  ""boot"":{{""bootDevice"":""disk0"",""kernelPath"":"""",""initrdPath"":"""",""bootArgs"":"""",""entryPoint"":0,""directBoot"":false}},
  ""storageDevices"":[{{""deviceId"":""disk0"",""deviceType"":""raw"",""imagePath"":""{EscapeJson(isoPath)}"",""readOnly"":true,""sizeBytes"":0,""blockSize"":2048}}],
  ""enableDebugger"":true,
  ""enableSnapshots"":true
}}";

        Console.WriteLine($"RepoRoot={Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."))}");
        Console.WriteLine($"NativeDllPath={Path.Combine(Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..")), "guideXOS Hypervisor GUI", "bin", "Debug", "net9.0-windows", "guideXOS_Hypervisor.dll")}");
        Console.WriteLine($"seed={seed}");
        Console.WriteLine($"cyclesPerBatch={cyclesPerBatch}");
        Console.WriteLine($"batches={batches}");
        Console.WriteLine($"configJson={configJson}");

        IntPtr manager = IntPtr.Zero;
        IntPtr vmIdPtr = IntPtr.Zero;

        try
        {
            manager = NativeApi.VMManager_Create();
            Console.WriteLine($"manager=0x{manager.ToInt64():X}");
            if (manager == IntPtr.Zero)
            {
                Console.WriteLine("VMManager_Create returned null");
                return 2;
            }

            vmIdPtr = NativeApi.VMManager_CreateVM(manager, configJson);
            Console.WriteLine($"vmIdPtr=0x{vmIdPtr.ToInt64():X}");
            if (vmIdPtr == IntPtr.Zero)
            {
                Console.WriteLine("VMManager_CreateVM returned null");
                return 3;
            }

            string vmId = Marshal.PtrToStringAnsi(vmIdPtr) ?? string.Empty;
            Console.WriteLine($"vmId={vmId}");
            NativeApi.VMManager_FreeString(vmIdPtr);
            vmIdPtr = IntPtr.Zero;

            bool started = NativeApi.VMManager_StartVM(manager, vmId);
            Console.WriteLine($"started={started}");
            if (!started)
            {
                return 4;
            }

            for (int i = 0; i < batches; i++)
            {
                ulong executed = NativeApi.VMManager_RunVM(manager, vmId, cyclesPerBatch);
                Console.WriteLine($"batch={i} cycles={executed}");
                if (executed == 0)
                {
                    break;
                }
            }

            bool stopped = NativeApi.VMManager_StopVM(manager, vmId);
            Console.WriteLine($"stopped={stopped}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"EXCEPTION: {ex.GetType().Name}: {ex.Message}");
            Console.WriteLine(ex);
            return 1;
        }
        finally
        {
            if (vmIdPtr != IntPtr.Zero)
            {
                NativeApi.VMManager_FreeString(vmIdPtr);
            }

            if (manager != IntPtr.Zero)
            {
                NativeApi.VMManager_Destroy(manager);
            }
        }
    }

    private static string EscapeJson(string value)
    {
        return value.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }
}

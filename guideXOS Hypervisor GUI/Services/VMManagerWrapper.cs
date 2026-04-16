using System;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using guideXOS_Hypervisor_GUI.Models;

namespace guideXOS_Hypervisor_GUI.Services
{
    /// <summary>
    /// Wrapper for C++ VMManager - provides managed interface to native hypervisor
    /// </summary>
    public class VMManagerWrapper
    {
        // TODO: P/Invoke declarations for C++ VMManager
        // These will need to be implemented when integrating with the actual C++ backend
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr VMManager_Create();
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern void VMManager_Destroy(IntPtr manager);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr VMManager_CreateVM(IntPtr manager, string configPath);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_StartVM(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_StopVM(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_PauseVM(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_ResetVM(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_DeleteVM(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr VMManager_GetVMs(IntPtr manager, out int count);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern ulong VMManager_RunVM(IntPtr manager, string vmId, ulong maxCycles);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_GetFramebuffer(IntPtr manager, string vmId, byte[] buffer, int bufferSize, out int width, out int height);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_GetFramebufferDimensions(IntPtr manager, string vmId, out int width, out int height);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern void VMManager_FreeString(IntPtr str);

        private static VMManagerWrapper? _instance;
        private readonly object _lock = new();
        private readonly Dictionary<string, CancellationTokenSource> _vmExecutionThreads = new();
        private readonly Dictionary<string, byte[]> _mockFramebuffers = new();
        private IntPtr _nativeManager = IntPtr.Zero;
        private bool _useMockExecution = true; // Set to false when DLL is available

        private VMManagerWrapper()
        {
            // Try to initialize native VMManager
            try
            {
                System.Diagnostics.Debug.WriteLine("Attempting to load native VMManager DLL...");
                _nativeManager = VMManager_Create();
                if (_nativeManager != IntPtr.Zero)
                {
                    _useMockExecution = false; // DLL is available!
                    System.Diagnostics.Debug.WriteLine("? Native VMManager DLL loaded successfully!");
                    Console.WriteLine("? Native VMManager DLL loaded successfully!");
                }
                else
                {
                    System.Diagnostics.Debug.WriteLine("? VMManager_Create returned null - using mock execution");
                    Console.WriteLine("? VMManager_Create returned null - using mock execution");
                }
            }
            catch (DllNotFoundException ex)
            {
                // DLL not found - use mock execution
                _useMockExecution = true;
                System.Diagnostics.Debug.WriteLine($"? DLL not found: {ex.Message}");
                Console.WriteLine($"? DLL not found: {ex.Message}");
                Console.WriteLine("Using mock execution mode");
            }
            catch (Exception ex)
            {
                // Other error - use mock execution
                _useMockExecution = true;
                System.Diagnostics.Debug.WriteLine($"? Error loading DLL: {ex.Message}");
                Console.WriteLine($"? Error loading native DLL: {ex.Message}");
                Console.WriteLine($"   Type: {ex.GetType().Name}");
                Console.WriteLine("Using mock execution mode");
            }
        }
        
        ~VMManagerWrapper()
        {
            if (_nativeManager != IntPtr.Zero)
            {
                VMManager_Destroy(_nativeManager);
                _nativeManager = IntPtr.Zero;
            }
        }

        public static VMManagerWrapper Instance
        {
            get
            {
                _instance ??= new VMManagerWrapper();
                return _instance;
            }
        }

        /// <summary>
        /// Get all virtual machines
        /// </summary>
        public List<VMStateModel> GetVirtualMachines()
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_GetVMs
                // For now, return empty list - VMs are created via MainViewModel
                return new List<VMStateModel>();
            }
        }

        /// <summary>
        /// Create a new virtual machine
        /// </summary>
        public string CreateVM(VMConfiguration config)
        {
            lock (_lock)
            {
                Console.WriteLine($"CreateVM called. Mock mode: {_useMockExecution}");
                Console.WriteLine($"Native manager handle: {_nativeManager}");
                
                // Write diagnostic log
                try
                {
                    string diagLog = $@"
=== VM Creation Diagnostic ===
Time: {DateTime.Now}
Mock mode: {_useMockExecution}
Native manager: {_nativeManager}
Config name: {config.Name}
Storage count: {config.Storage.Count}
";
                    if (config.Storage.Count > 0)
                    {
                        diagLog += $"First storage: {config.Storage[0].ImagePath}\n";
                    }
                    System.IO.File.AppendAllText("vm_creation_log.txt", diagLog);
                }
                catch { }
                
                try
                {
                    if (!_useMockExecution && _nativeManager != IntPtr.Zero)
                    {
                        // Convert config to JSON and call native function
                        string configJson = config.ToJson();
                        Console.WriteLine("Calling native VMManager_CreateVM with JSON:");
                        Console.WriteLine(configJson);
                        Console.WriteLine("(JSON length: " + configJson.Length + " characters)");
                        
                        // Write JSON to file for inspection
                        try
                        {
                            System.IO.File.WriteAllText("last_vm_config.json", configJson);
                            Console.WriteLine("JSON saved to: last_vm_config.json");
                        }
                        catch (Exception ex)
                        {
                            Console.WriteLine($"Could not save JSON: {ex.Message}");
                        }
                        
                        IntPtr result = VMManager_CreateVM(_nativeManager, configJson);
                        
                        if (result != IntPtr.Zero)
                        {
                            string vmId = Marshal.PtrToStringAnsi(result) ?? string.Empty;
                            VMManager_FreeString(result);
                            Console.WriteLine($"? VM created with ID: {vmId}");
                            return vmId;
                        }
                        else
                        {
                            Console.WriteLine("? VMManager_CreateVM returned null");
                            Console.WriteLine("ERROR: Native VM creation failed!");
                            Console.WriteLine("This could be due to:");
                            Console.WriteLine("  - Invalid configuration");
                            Console.WriteLine("  - Storage device file not found");
                            Console.WriteLine("  - Memory allocation failure");
                            Console.WriteLine("  - C++ exception during creation");
                            Console.WriteLine("");
                            Console.WriteLine("The C++ DLL should have printed detailed error messages above.");
                            Console.WriteLine("If you don't see C++ error messages, the DLL might not be");
                            Console.WriteLine("printing to this console (check Output window in VS).");
                            throw new Exception("Native VM creation failed. Check C++ logs for details.");
                        }
                    }
                    else
                    {
                        Console.WriteLine("Using mock VM creation (DLL not loaded)");
                        // Mock: Return a new VM ID
                        Console.WriteLine("Returning mock VM ID");
                        return Guid.NewGuid().ToString();
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"? Error in CreateVM: {ex.Message}");
                    Console.WriteLine($"   Exception type: {ex.GetType().Name}");
                    Console.WriteLine($"   Stack trace: {ex.StackTrace}");
                    throw; // Re-throw so UI can show error
                }
            }
        }

        /// <summary>
        /// Start a virtual machine
        /// </summary>
        public bool StartVM(string vmId)
        {
            lock (_lock)
            {
                Console.WriteLine($"StartVM called for {vmId}. Mock mode: {_useMockExecution}");
                
                bool started = false;
                
                // Call native StartVM if available
                if (!_useMockExecution && _nativeManager != IntPtr.Zero)
                {
                    try
                    {
                        Console.WriteLine("Calling native VMManager_StartVM...");
                        started = VMManager_StartVM(_nativeManager, vmId);
                        Console.WriteLine($"Native StartVM returned: {started}");
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"? Error calling native StartVM: {ex.Message}");
                    }
                }
                
                // Start execution thread (for both native and mock)
                var cts = new CancellationTokenSource();
                _vmExecutionThreads[vmId] = cts;
                
                Task.Run(() => VMExecutionLoop(vmId, cts.Token), cts.Token);
                
                return started || _useMockExecution; // Return true for mock mode
            }
        }

        /// <summary>
        /// Stop a virtual machine
        /// </summary>
        public bool StopVM(string vmId)
        {
            lock (_lock)
            {
                Console.WriteLine($"StopVM called for {vmId}");
                
                // Stop execution thread
                if (_vmExecutionThreads.TryGetValue(vmId, out var cts))
                {
                    cts.Cancel();
                    _vmExecutionThreads.Remove(vmId);
                }
                
                // Clean up mock framebuffer
                _mockFramebuffers.Remove(vmId);
                
                // Call native StopVM if available
                if (!_useMockExecution && _nativeManager != IntPtr.Zero)
                {
                    try
                    {
                        Console.WriteLine("Calling native VMManager_StopVM...");
                        return VMManager_StopVM(_nativeManager, vmId);
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"? Error calling native StopVM: {ex.Message}");
                    }
                }
                
                return true; // Mock always succeeds
            }
        }

        /// <summary>
        /// Pause a virtual machine
        /// </summary>
        public bool PauseVM(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_PauseVM
                return true;
            }
        }

        /// <summary>
        /// Reset a virtual machine
        /// </summary>
        public bool ResetVM(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_ResetVM
                return true;
            }
        }

        /// <summary>
        /// Delete a virtual machine
        /// </summary>
        public bool DeleteVM(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_DeleteVM
                return true;
            }
        }

        /// <summary>
        /// Create a snapshot of a virtual machine
        /// </summary>
        public bool CreateSnapshot(string vmId, string snapshotName)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_CreateSnapshot
                return true;
            }
        }

        /// <summary>
        /// Create a snapshot with description (enhanced version)
        /// </summary>
        public string CreateSnapshotEx(string vmId, string snapshotName, string description)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_CreateSnapshotEx
                // Return the snapshot ID
                return Guid.NewGuid().ToString();
            }
        }

        /// <summary>
        /// Restore a VM to a specific snapshot
        /// </summary>
        public bool RestoreSnapshot(string vmId, string snapshotId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_RestoreSnapshot
                return true;
            }
        }

        /// <summary>
        /// Delete a snapshot
        /// </summary>
        public bool DeleteSnapshot(string vmId, string snapshotId, bool deleteChildren)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_DeleteSnapshot
                return true;
            }
        }

        /// <summary>
        /// Get all snapshots for a VM
        /// </summary>
        public List<VMSnapshot> GetSnapshots(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_GetSnapshots
                // For now, return sample snapshots for demonstration
                return GenerateSampleSnapshots();
            }
        }

        /// <summary>
        /// Generate sample snapshots for UI demonstration
        /// </summary>
        private List<VMSnapshot> GenerateSampleSnapshots()
        {
            var random = new Random();
            var snapshots = new List<VMSnapshot>();

            // Create a few sample snapshots with different timestamps
            var baseDate = DateTime.Now.AddDays(-7);
            
            for (int i = 1; i <= 3; i++)
            {
                snapshots.Add(new VMSnapshot
                {
                    Id = Guid.NewGuid().ToString(),
                    Name = $"Snapshot {i}",
                    Description = $"Test snapshot {i}",
                    CreatedDate = baseDate.AddDays(i),
                    SizeMB = (ulong)random.Next(100, 1000),
                    State = new VMSnapshotState
                    {
                        CpuCount = 2,
                        MemoryMB = 1024,
                        State = VMState.Running,
                        InstructionPointer = 0x400000,
                        TotalCycles = (ulong)random.Next(1000000, 10000000)
                    }
                });
            }

            return snapshots;
        }

        /// <summary>
        /// Get VM state
        /// </summary>
        public VMStateModel? GetVMState(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager to get VM state
                return null;
            }
        }

        /// <summary>
        /// Get profiler data for a VM
        /// </summary>
        public ProfilerDataModel? GetProfilerData(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager.GetVM(vmId).GetProfiler()
                // For now, return sample data to demonstrate the UI
                return GenerateSampleProfilerData();
            }
        }
        
        /// <summary>
        /// Export profiler data as JSON
        /// </summary>
        public string ExportProfilerJson(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native Profiler::exportToJSON()
                return "{}";
            }
        }
        
        /// <summary>
        /// Export control flow graph as DOT (Graphviz)
        /// </summary>
        public string ExportProfilerDot(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native Profiler::exportControlFlowGraphDOT()
                return "digraph CFG {\n}\n";
            }
        }
        
        /// <summary>
        /// Export profiler statistics as CSV
        /// </summary>
        public string ExportProfilerCsv(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native Profiler::exportInstructionFrequencyCSV()
                return "Address,Instruction,Count\n";
            }
        }
        
        #region Debugger API
        
        // Debugger P/Invoke declarations (optional - can be added later)
        /*
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_AttachDebugger(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_DetachDebugger(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_StepInstruction(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_ContinueExecution(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_PauseExecution(IntPtr manager, string vmId);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_SetBreakpoint(IntPtr manager, string vmId, ulong address);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_RemoveBreakpoint(IntPtr manager, string vmId, ulong address);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_SetWatchpoint(IntPtr manager, string vmId, ulong address, int size, int type);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_RemoveWatchpoint(IntPtr manager, string vmId, ulong address);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_GetDebuggerState(IntPtr manager, string vmId, out DebuggerStateNative state);
        
        [DllImport("guideXOS_Hypervisor.dll", CallingConvention = CallingConvention.Cdecl)]
        private static extern bool VMManager_ReadMemory(IntPtr manager, string vmId, ulong address, byte[] buffer, int size);
        */
        
        /// <summary>
        /// Attach debugger to a VM
        /// </summary>
        public void AttachDebugger(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_AttachDebugger
                // For now, just a stub
            }
        }
        
        /// <summary>
        /// Detach debugger from a VM
        /// </summary>
        public void DetachDebugger(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_DetachDebugger
            }
        }
        
        /// <summary>
        /// Execute a single instruction
        /// </summary>
        public void StepInstruction(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_StepInstruction
            }
        }
        
        /// <summary>
        /// Continue execution until next breakpoint
        /// </summary>
        public void ContinueExecution(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_ContinueExecution
            }
        }
        
        /// <summary>
        /// Pause execution
        /// </summary>
        public void PauseExecution(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_PauseExecution
            }
        }
        
        /// <summary>
        /// Set a breakpoint at the specified address
        /// </summary>
        public void SetBreakpoint(string vmId, ulong address)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_SetBreakpoint
            }
        }
        
        /// <summary>
        /// Remove a breakpoint
        /// </summary>
        public void RemoveBreakpoint(string vmId, ulong address)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_RemoveBreakpoint
            }
        }
        
        /// <summary>
        /// Toggle breakpoint enabled/disabled state
        /// </summary>
        public void ToggleBreakpoint(string vmId, ulong address, bool enabled)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager to toggle breakpoint
            }
        }
        
        /// <summary>
        /// Set a watchpoint (memory watch)
        /// </summary>
        public void SetWatchpoint(string vmId, ulong address, int size, int type)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_SetWatchpoint
            }
        }
        
        /// <summary>
        /// Remove a watchpoint
        /// </summary>
        public void RemoveWatchpoint(string vmId, ulong address)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_RemoveWatchpoint
            }
        }
        
        /// <summary>
        /// Toggle watchpoint enabled/disabled state
        /// </summary>
        public void ToggleWatchpoint(string vmId, ulong address, bool enabled)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager to toggle watchpoint
            }
        }
        
        /// <summary>
        /// Get current debugger state (registers, IP, etc.)
        /// </summary>
        public VMStateModel GetDebuggerState(string vmId)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_GetDebuggerState
                // For now, return mock data
                var state = new VMStateModel
                {
                    Id = vmId,
                    CurrentIP = 0x400000,
                    CFM = 0,
                    PSR = 0,
                    GeneralRegisters = new string[8],
                    FloatRegisters = new string[8],
                    PredicateRegisters = new string[16],
                    BranchRegisters = new string[8]
                };
                
                // Initialize with sample values
                for (int i = 0; i < 8; i++)
                {
                    state.GeneralRegisters[i] = "0x0000000000000000";
                    state.FloatRegisters[i] = "0x0000000000000000";
                    state.BranchRegisters[i] = "0x0000000000000000";
                }
                
                for (int i = 0; i < 16; i++)
                {
                    state.PredicateRegisters[i] = "0";
                }
                
                return state;
            }
        }
        
        /// <summary>
        /// Read memory from VM
        /// </summary>
        public byte[] ReadMemory(string vmId, ulong address, int size)
        {
            lock (_lock)
            {
                // TODO: Call native VMManager_ReadMemory
                // For now, return mock data
                var buffer = new byte[size];
                var random = new Random((int)address);
                random.NextBytes(buffer);
                return buffer;
            }
        }
        
        /// <summary>
        /// Get framebuffer data from VM
        /// </summary>
        /// <param name="vmId">VM identifier</param>
        /// <param name="buffer">Destination buffer for framebuffer data (BGRA32 format)</param>
        /// <param name="width">Output: framebuffer width</param>
        /// <param name="height">Output: framebuffer height</param>
        /// <returns>True if successful</returns>
        public bool GetFramebuffer(string vmId, byte[] buffer, out int width, out int height)
        {
            lock (_lock)
            {
                // Try real DLL first
                if (!_useMockExecution && _nativeManager != IntPtr.Zero)
                {
                    try
                    {
                        return VMManager_GetFramebuffer(_nativeManager, vmId, buffer, buffer.Length, out width, out height);
                    }
                    catch
                    {
                        // Fall back to mock
                    }
                }
                
                // Use mock framebuffer
                width = 640;
                height = 480;
                
                if (_mockFramebuffers.TryGetValue(vmId, out byte[]? framebuffer))
                {
                    Array.Copy(framebuffer, buffer, Math.Min(buffer.Length, framebuffer.Length));
                    return true;
                }
                
                return false;
            }
        }
        
        /// <summary>
        /// VM execution loop - calls real VMManager_RunVM or uses mock
        /// </summary>
        private void VMExecutionLoop(string vmId, CancellationToken cancellationToken)
        {
            const ulong cyclesPerFrame = 10000; // ~10K cycles per frame
            
            if (_useMockExecution)
            {
                // Mock execution for testing
                VMExecutionLoopMock(vmId, cancellationToken);
                return;
            }
            
            // Real execution using native DLL
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    if (_nativeManager != IntPtr.Zero)
                    {
                        // Call native VMManager_RunVM to execute VM cycles
                        ulong cyclesExecuted = VMManager_RunVM(_nativeManager, vmId, cyclesPerFrame);
                        
                        if (cyclesExecuted == 0)
                        {
                            // VM halted or error
                            break;
                        }
                    }
                    
                    // Sleep for ~16ms (60 FPS)
                    Thread.Sleep(16);
                }
                catch (Exception)
                {
                    // Stop on error
                    break;
                }
            }
        }
        
        /// <summary>
        /// Mock VM execution loop for testing without DLL
        /// </summary>
        private void VMExecutionLoopMock(string vmId, CancellationToken cancellationToken)
        {
            // Create mock framebuffer (640x480, BGRA32 = 4 bytes per pixel)
            byte[] framebuffer = new byte[640 * 480 * 4];
            
            lock (_lock)
            {
                _mockFramebuffers[vmId] = framebuffer;
            }
            
            int frame = 0;
            
            while (!cancellationToken.IsCancellationRequested)
            {
                try
                {
                    // Simulate execution by drawing animated pattern
                    UpdateMockFramebuffer(framebuffer, frame++);
                    
                    // Sleep for ~16ms (60 FPS)
                    Thread.Sleep(16);
                }
                catch (Exception)
                {
                    // Stop on error
                    break;
                }
            }
        }
        
        /// <summary>
        /// Update mock framebuffer with animated pattern
        /// This proves the framebuffer rendering pipeline works
        /// </summary>
        private void UpdateMockFramebuffer(byte[] framebuffer, int frame)
        {
            const int width = 640;
            const int height = 480;
            
            // Draw animated pattern (moving vertical bars)
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int offset = (y * width + x) * 4;
                    
                    // Create moving wave pattern
                    int waveX = (x + frame * 2) % 256;
                    int waveY = y % 256;
                    
                    byte r = (byte)((Math.Sin(waveX * 0.05) + 1.0) * 127);
                    byte g = (byte)((Math.Sin(waveY * 0.05) + 1.0) * 127);
                    byte b = (byte)((Math.Sin((waveX + waveY) * 0.05) + 1.0) * 127);
                    
                    framebuffer[offset + 0] = b;  // Blue
                    framebuffer[offset + 1] = g;  // Green
                    framebuffer[offset + 2] = r;  // Red
                    framebuffer[offset + 3] = 255; // Alpha
                }
            }
        }
        
        /// <summary>
        /// Get framebuffer dimensions
        /// </summary>
        /// <param name="vmId">VM identifier</param>
        /// <param name="width">Output: framebuffer width</param>
        /// <param name="height">Output: framebuffer height</param>
        /// <returns>True if successful</returns>
        public bool GetFramebufferDimensions(string vmId, out int width, out int height)
        {
            lock (_lock)
            {
                // Try real DLL first
                if (!_useMockExecution && _nativeManager != IntPtr.Zero)
                {
                    try
                    {
                        return VMManager_GetFramebufferDimensions(_nativeManager, vmId, out width, out height);
                    }
                    catch
                    {
                        // Fall back to mock
                    }
                }
                
                // Default dimensions
                width = 640;
                height = 480;
                return true;
            }
        }
        
        #endregion
        
        /// <summary>
        /// Generate sample profiler data for UI demonstration
        /// </summary>
        private ProfilerDataModel GenerateSampleProfilerData()
        {
            var random = new Random();
            var data = new ProfilerDataModel
            {
                TotalInstructions = (ulong)(random.Next(1_000_000, 10_000_000)),
                TotalCycles = (ulong)(random.Next(2_000_000, 15_000_000)),
                AverageIPC = 1.5 + random.NextDouble() * 0.8,
                ExecutionTime = TimeSpan.FromSeconds(random.Next(1, 30)),
                LastUpdateTime = DateTime.Now,
                IsLive = true
            };
            
            // Generate top 10 instructions
            var instructionMnemonics = new[] { "ld8", "st8", "add", "mov", "br.cond", "cmp.eq", "sub", "shl", "and", "or", "xor", "nop" };
            ulong totalCount = 0;
            var instructions = new List<InstructionFrequencyInfo>();
            
            for (int i = 0; i < 10; i++)
            {
                var count = (ulong)random.Next(50000, 500000);
                totalCount += count;
                instructions.Add(new InstructionFrequencyInfo
                {
                    Address = (ulong)(0x400000 + i * 16),
                    Disassembly = $"{instructionMnemonics[random.Next(instructionMnemonics.Length)]} r{random.Next(32)}, r{random.Next(32)}",
                    ExecutionCount = count,
                    Percentage = 0
                });
            }
            
            // Calculate percentages
            foreach (var inst in instructions)
            {
                inst.Percentage = (double)inst.ExecutionCount / totalCount * 100.0;
            }
            data.TopInstructions = instructions.OrderByDescending(i => i.ExecutionCount).ToList();
            
            // Generate hot paths
            data.HotPaths = new List<HotPathInfo>
            {
                new HotPathInfo
                {
                    PathDescription = "main_loop ? process_data ? validate",
                    ExecutionCount = (ulong)random.Next(10000, 100000),
                    TotalInstructions = (ulong)random.Next(50, 200),
                    Percentage = random.Next(10, 30)
                },
                new HotPathInfo
                {
                    PathDescription = "kernel_entry ? syscall_handler ? dispatch",
                    ExecutionCount = (ulong)random.Next(5000, 50000),
                    TotalInstructions = (ulong)random.Next(30, 150),
                    Percentage = random.Next(5, 15)
                },
                new HotPathInfo
                {
                    PathDescription = "interrupt_handler ? save_context ? handle_irq",
                    ExecutionCount = (ulong)random.Next(3000, 30000),
                    TotalInstructions = (ulong)random.Next(20, 100),
                    Percentage = random.Next(3, 10)
                }
            };
            
            // Generate register pressure data
            data.RegisterPressure.GeneralRegisters = GenerateRegisterUsage("r", 32, random, 10);
            data.RegisterPressure.FloatRegisters = GenerateRegisterUsage("f", 128, random, 10);
            data.RegisterPressure.PredicateRegisters = GenerateRegisterUsage("p", 64, random, 10);
            data.RegisterPressure.BranchRegisters = GenerateRegisterUsage("b", 8, random, 8);
            
            // Generate memory access breakdown
            var totalReads = (ulong)random.Next(100000, 1000000);
            var totalWrites = (ulong)random.Next(50000, 500000);
            
            data.MemoryBreakdown.Stack = new MemoryRegionInfo
            {
                RegionName = "Stack",
                ReadCount = totalReads * 30 / 100,
                WriteCount = totalWrites * 40 / 100,
                TotalBytes = (ulong)random.Next(50000, 200000),
                UniqueAddresses = (ulong)random.Next(1000, 5000),
                ReadPercentage = 30.0,
                WritePercentage = 40.0,
                BytesPercentage = 25.0
            };
            
            data.MemoryBreakdown.Heap = new MemoryRegionInfo
            {
                RegionName = "Heap",
                ReadCount = totalReads * 40 / 100,
                WriteCount = totalWrites * 35 / 100,
                TotalBytes = (ulong)random.Next(100000, 500000),
                UniqueAddresses = (ulong)random.Next(2000, 10000),
                ReadPercentage = 40.0,
                WritePercentage = 35.0,
                BytesPercentage = 45.0
            };
            
            data.MemoryBreakdown.Code = new MemoryRegionInfo
            {
                RegionName = "Code",
                ReadCount = totalReads * 25 / 100,
                WriteCount = 0,
                TotalBytes = (ulong)random.Next(20000, 100000),
                UniqueAddresses = (ulong)random.Next(500, 3000),
                ReadPercentage = 25.0,
                WritePercentage = 0.0,
                BytesPercentage = 20.0
            };
            
            data.MemoryBreakdown.Data = new MemoryRegionInfo
            {
                RegionName = "Data",
                ReadCount = totalReads * 5 / 100,
                WriteCount = totalWrites * 25 / 100,
                TotalBytes = (ulong)random.Next(10000, 50000),
                UniqueAddresses = (ulong)random.Next(100, 1000),
                ReadPercentage = 5.0,
                WritePercentage = 25.0,
                BytesPercentage = 10.0
            };
            
            data.MemoryBreakdown.TotalReads = totalReads;
            data.MemoryBreakdown.TotalWrites = totalWrites;
            data.MemoryBreakdown.TotalBytes = data.MemoryBreakdown.Stack.TotalBytes +
                                               data.MemoryBreakdown.Heap.TotalBytes +
                                               data.MemoryBreakdown.Code.TotalBytes +
                                               data.MemoryBreakdown.Data.TotalBytes;
            
            return data;
        }
        
        private List<RegisterUsageInfo> GenerateRegisterUsage(string prefix, int count, Random random, int topN)
        {
            var registers = new List<RegisterUsageInfo>();
            ulong totalAccess = 0;
            
            for (int i = 0; i < count; i++)
            {
                var reads = (ulong)random.Next(1000, 100000);
                var writes = (ulong)random.Next(500, 50000);
                var total = reads + writes;
                totalAccess += total;
                
                registers.Add(new RegisterUsageInfo
                {
                    RegisterName = $"{prefix}{i}",
                    RegisterIndex = i,
                    ReadCount = reads,
                    WriteCount = writes,
                    TotalAccess = total,
                    Percentage = 0
                });
            }
            
            // Calculate percentages and return top N
            foreach (var reg in registers)
            {
                reg.Percentage = (double)reg.TotalAccess / totalAccess * 100.0;
            }
            
            return registers.OrderByDescending(r => r.TotalAccess).Take(topN).ToList();
        }
    }
}

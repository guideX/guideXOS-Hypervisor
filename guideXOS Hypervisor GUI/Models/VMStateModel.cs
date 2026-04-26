using System;
using System.Collections.Generic;

namespace guideXOS_Hypervisor_GUI.Models
{
    /// <summary>
    /// Represents the state of a Virtual Machine
    /// </summary>
    public enum VMState
    {
        PoweredOff,
        Running,
        Paused,
        Saved,
        Aborted
    }

    /// <summary>
    /// Model representing a Virtual Machine's current state and configuration
    /// </summary>
    public class VMStateModel
    {
        // General Information
        public string Id { get; set; } = string.Empty;
        public string UUID { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;
        public VMState State { get; set; }
        public string OperatingSystem { get; set; } = "IA-64 System";
        public string Architecture { get; set; } = "IA-64";
        public string ISAType { get; set; } = "IA-64 (Itanium)";
        public DateTime CreatedDate { get; set; }
        public DateTime? LastRunDate { get; set; }
        public string ConfigurationPath { get; set; } = string.Empty;
        
        // System Configuration
        public int CpuCount { get; set; }
        public ulong MemoryMB { get; set; }
        public bool MMUEnabled { get; set; } = true;
        public ulong PageSizeKB { get; set; } = 16; // IA-64 supports 4KB-256MB pages, 16KB is common

        // Boot Configuration
        public string BootFromISO { get; set; } = string.Empty; // Path to ISO file for CD-ROM boot
        
        // CPU State
        public ulong CurrentIP { get; set; }
        public ulong CyclesPerSecond { get; set; }
        public ulong TotalCycles { get; set; }
        
        // Register Snapshots (summaries)
        public string[] GeneralRegisters { get; set; } = new string[8]; // First 8 GR registers for summary
        public string[] FloatRegisters { get; set; } = new string[8]; // First 8 FR registers for summary
        public string[] PredicateRegisters { get; set; } = new string[16]; // First 16 PR registers for summary
        public string[] BranchRegisters { get; set; } = new string[8]; // All 8 BR registers
        public ulong CFM { get; set; } // Current Frame Marker
        public ulong PSR { get; set; } // Processor Status Register
        
        // Storage
        public List<DiskImageInfo> DiskImages { get; set; } = new List<DiskImageInfo>();
        
        // Debug State
        public List<BreakpointInfo> Breakpoints { get; set; } = new List<BreakpointInfo>();
        public List<WatchpointInfo> Watchpoints { get; set; } = new List<WatchpointInfo>();
        
        public string StateDescription => State switch
        {
            VMState.PoweredOff => "Powered Off",
            VMState.Running => "Running",
            VMState.Paused => "Paused",
            VMState.Saved => "Saved",
            VMState.Aborted => "Aborted",
            _ => "Unknown"
        };
    }
    
    /// <summary>
    /// Disk image attachment information
    /// </summary>
    public class DiskImageInfo
    {
        public string Path { get; set; } = string.Empty;
        public string Type { get; set; } = "HDD"; // HDD, DVD, Floppy
        public bool ReadOnly { get; set; } = false;
        public ulong SizeMB { get; set; }
        public string Controller { get; set; } = "SATA";
        public int Port { get; set; } = 0;
    }
    
    /// <summary>
    /// Breakpoint information
    /// </summary>
    public class BreakpointInfo
    {
        public ulong Address { get; set; }
        public bool Enabled { get; set; } = true;
        public string Condition { get; set; } = string.Empty;
        public int HitCount { get; set; } = 0;
    }
    
    /// <summary>
    /// Memory watchpoint information
    /// </summary>
    public class WatchpointInfo
    {
        public ulong AddressStart { get; set; }
        public ulong AddressEnd { get; set; }
        public string Type { get; set; } = "Read/Write"; // Read, Write, Read/Write, Execute
        public bool Enabled { get; set; } = true;
        public int HitCount { get; set; } = 0;
    }

    /// <summary>
    /// Profiler data model for performance monitoring
    /// </summary>
    public class ProfilerDataModel
    {
        public ulong TotalInstructions { get; set; }
        public ulong TotalCycles { get; set; }
        public double AverageIPC { get; set; }
        public ulong CacheHits { get; set; }
        public ulong CacheMisses { get; set; }
        public double CacheHitRate { get; set; }
        public TimeSpan ExecutionTime { get; set; }
        public string HottestFunction { get; set; } = "N/A";
        public ulong HottestFunctionCount { get; set; }
        
        // Instruction frequency data (top N instructions)
        public List<InstructionFrequencyInfo> TopInstructions { get; set; } = new List<InstructionFrequencyInfo>();
        
        // Hot execution paths
        public List<HotPathInfo> HotPaths { get; set; } = new List<HotPathInfo>();
        
        // Register pressure data
        public RegisterPressureInfo RegisterPressure { get; set; } = new RegisterPressureInfo();
        
        // Memory access breakdown
        public MemoryAccessBreakdown MemoryBreakdown { get; set; } = new MemoryAccessBreakdown();
        
        // Export metadata
        public DateTime LastUpdateTime { get; set; } = DateTime.Now;
        public bool IsLive { get; set; } = false;
    }
    
    /// <summary>
    /// Instruction frequency information
    /// </summary>
    public class InstructionFrequencyInfo
    {
        public ulong Address { get; set; }
        public string Disassembly { get; set; } = string.Empty;
        public ulong ExecutionCount { get; set; }
        public double Percentage { get; set; }
    }
    
    /// <summary>
    /// Hot execution path information
    /// </summary>
    public class HotPathInfo
    {
        public string PathDescription { get; set; } = string.Empty;
        public List<ulong> Addresses { get; set; } = new List<ulong>();
        public ulong ExecutionCount { get; set; }
        public ulong TotalInstructions { get; set; }
        public double Percentage { get; set; }
    }
    
    /// <summary>
    /// Register pressure information
    /// </summary>
    public class RegisterPressureInfo
    {
        // General registers (top N most used)
        public List<RegisterUsageInfo> GeneralRegisters { get; set; } = new List<RegisterUsageInfo>();
        
        // Floating-point registers (top N most used)
        public List<RegisterUsageInfo> FloatRegisters { get; set; } = new List<RegisterUsageInfo>();
        
        // Predicate registers (top N most used)
        public List<RegisterUsageInfo> PredicateRegisters { get; set; } = new List<RegisterUsageInfo>();
        
        // Branch registers
        public List<RegisterUsageInfo> BranchRegisters { get; set; } = new List<RegisterUsageInfo>();
    }
    
    /// <summary>
    /// Individual register usage information
    /// </summary>
    public class RegisterUsageInfo
    {
        public string RegisterName { get; set; } = string.Empty;
        public int RegisterIndex { get; set; }
        public ulong ReadCount { get; set; }
        public ulong WriteCount { get; set; }
        public ulong TotalAccess { get; set; }
        public double Percentage { get; set; }
    }
    
    /// <summary>
    /// Memory access breakdown by region type
    /// </summary>
    public class MemoryAccessBreakdown
    {
        public MemoryRegionInfo Stack { get; set; } = new MemoryRegionInfo { RegionName = "Stack" };
        public MemoryRegionInfo Heap { get; set; } = new MemoryRegionInfo { RegionName = "Heap" };
        public MemoryRegionInfo Code { get; set; } = new MemoryRegionInfo { RegionName = "Code" };
        public MemoryRegionInfo Data { get; set; } = new MemoryRegionInfo { RegionName = "Data" };
        public MemoryRegionInfo Unknown { get; set; } = new MemoryRegionInfo { RegionName = "Unknown" };
        
        public ulong TotalReads { get; set; }
        public ulong TotalWrites { get; set; }
        public ulong TotalBytes { get; set; }
    }
    
    /// <summary>
    /// Memory region access information
    /// </summary>
    public class MemoryRegionInfo
    {
        public string RegionName { get; set; } = string.Empty;
        public ulong ReadCount { get; set; }
        public ulong WriteCount { get; set; }
        public ulong TotalBytes { get; set; }
        public ulong UniqueAddresses { get; set; }
        public double ReadPercentage { get; set; }
        public double WritePercentage { get; set; }
        public double BytesPercentage { get; set; }
    }

    /// <summary>
    /// Storage device configuration matching C++ StorageConfiguration
    /// </summary>
    public class StorageConfiguration
    {
        public string DeviceId { get; set; } = "disk0";
        public string DeviceType { get; set; } = "raw"; // "raw", "qcow2", "vhd"
        public string ImagePath { get; set; } = string.Empty;
        public bool ReadOnly { get; set; } = false;
        public ulong SizeBytes { get; set; } = 0;
        public uint BlockSize { get; set; } = 512;
    }

    /// <summary>
    /// VM Configuration settings - enhanced with storage support
    /// </summary>
    public class VMConfiguration
    {
        public string Name { get; set; } = "New Virtual Machine";
        public int CpuCount { get; set; } = 1;
        public ulong MemoryMB { get; set; } = 512;
        public string BootDevice { get; set; } = "disk0";
        public string DiskPath { get; set; } = string.Empty;
        public bool EnableProfiler { get; set; } = true;
        public bool EnableDebugger { get; set; } = false;
        public string OperatingSystem { get; set; } = "IA-64 System";
        public string Architecture { get; set; } = "IA-64";
        
        // Storage devices collection
        public List<StorageConfiguration> Storage { get; set; } = new List<StorageConfiguration>();

        /// <summary>
        /// Create VM configuration with ISO/IMG file attached
        /// </summary>
        public static VMConfiguration CreateWithISO(string name, string isoPath, ulong memoryMB = 512, int cpuCount = 1)
        {
            var config = new VMConfiguration
            {
                Name = name,
                CpuCount = cpuCount,
                MemoryMB = memoryMB,
                DiskPath = isoPath,
                EnableProfiler = true,
                EnableDebugger = true
            };

            if (!string.IsNullOrEmpty(isoPath))
            {
                config.Storage.Add(new StorageConfiguration
                {
                    DeviceId = "disk0",
                    DeviceType = "raw",
                    ImagePath = isoPath,
                    ReadOnly = true, // ISOs are read-only
                    BlockSize = 2048 // CD-ROM standard block size
                });
                config.BootDevice = "disk0";
            }

            return config;
        }

        /// <summary>
        /// Convert configuration to JSON for C++ backend
        /// </summary>
        public string ToJson()
        {
            var json = new System.Text.StringBuilder();
            json.Append("{");
            
            // Basic info
            json.Append($"\"name\":\"{Name}\",");
            
            // CPU configuration
            json.Append("\"cpu\":{");
            json.Append($"\"isaType\":\"{Architecture}\",");
            json.Append($"\"cpuCount\":{CpuCount},");
            json.Append($"\"clockFrequency\":0,");
            json.Append($"\"enableProfiling\":{(EnableProfiler ? "true" : "false")}");
            json.Append("},");
            
            // Memory configuration
            json.Append("\"memory\":{");
            json.Append($"\"memorySize\":{MemoryMB * 1024 * 1024},");
            json.Append("\"enableMMU\":true,");
            json.Append("\"pageSize\":16384");  // 16KB default
            json.Append("},");
            
            // Boot configuration
            json.Append("\"boot\":{");
            json.Append($"\"bootDevice\":\"{BootDevice}\",");
            json.Append("\"kernelPath\":\"\",");
            json.Append("\"initrdPath\":\"\",");
            json.Append("\"bootArgs\":\"\",");
            json.Append("\"entryPoint\":0,");
            json.Append("\"directBoot\":false");
            json.Append("},");
            
            // Storage devices
            json.Append("\"storageDevices\":[");
            for (int i = 0; i < Storage.Count; i++)
            {
                var storage = Storage[i];
                json.Append("{");
                json.Append($"\"deviceId\":\"{storage.DeviceId}\",");
                json.Append($"\"deviceType\":\"{storage.DeviceType}\",");
                // Escape backslashes for Windows paths
                json.Append($"\"imagePath\":\"{storage.ImagePath.Replace("\\", "\\\\")}\",");
                json.Append($"\"readOnly\":{(storage.ReadOnly ? "true" : "false")},");
                json.Append($"\"sizeBytes\":{storage.SizeBytes},");
                json.Append($"\"blockSize\":{storage.BlockSize}");
                json.Append("}");
                if (i < Storage.Count - 1) json.Append(",");
            }
            json.Append("],");
            
            // Features (as top-level fields, not nested object)
            json.Append($"\"enableDebugger\":{(EnableDebugger ? "true" : "false")},");
            json.Append("\"enableSnapshots\":true");
            
            json.Append("}");
            return json.ToString();
        }
    }
}

using System.Collections.ObjectModel;

namespace guideXOS_Hypervisor_GUI.Models
{
    /// <summary>
    /// Model for VM configuration settings
    /// </summary>
    public class VMSettingsModel
    {
        // General Settings
        public string Id { get; set; } = string.Empty;
        public string Name { get; set; } = string.Empty;
        public string Description { get; set; } = string.Empty;
        public string OperatingSystem { get; set; } = "IA-64 System";
        public string Architecture { get; set; } = "IA-64";
        public string ISAType { get; set; } = "IA-64 (Itanium)";

        // System Settings
        public int CpuCount { get; set; } = 1;
        public ulong MemoryMB { get; set; } = 512;
        public bool MMUEnabled { get; set; } = true;
        public ulong PageSizeKB { get; set; } = 16;

        // Boot Settings
        public string BootDevice { get; set; } = "disk";
        public int BootOrder { get; set; } = 1;
        public string BootFromISO { get; set; } = string.Empty; // Path to ISO file for CD-ROM boot

        // Display Settings
        public int VideoMemoryMB { get; set; } = 32;
        public bool Enable3DAcceleration { get; set; } = false;

        // Storage Settings
        public ObservableCollection<DiskImageInfo> DiskImages { get; set; } = new ObservableCollection<DiskImageInfo>();

        // Network Settings
        public bool NetworkEnabled { get; set; } = false;
        public string NetworkAdapter { get; set; } = "Intel PRO/1000 MT Desktop";
        public string NetworkMode { get; set; } = "NAT";
        public string MacAddress { get; set; } = string.Empty;

        // Advanced Settings
        public bool EnableProfiler { get; set; } = true;
        public bool EnableDebugger { get; set; } = true;
        public bool EnableSnapshot { get; set; } = true;
        public bool PauseOnStart { get; set; } = false;

        // Performance Settings
        public bool EnableCPUHotplug { get; set; } = false;
        public int ExecutionCap { get; set; } = 100; // Percentage
        public bool EnablePAE { get; set; } = false;
        public bool EnableNestedVT { get; set; } = false;

        /// <summary>
        /// Create a VMSettingsModel from a VMStateModel
        /// </summary>
        public static VMSettingsModel FromVMState(VMStateModel state)
        {
            var settings = new VMSettingsModel
            {
                Id = state.Id,
                Name = state.Name,
                OperatingSystem = state.OperatingSystem,
                Architecture = state.Architecture,
                ISAType = state.ISAType,
                CpuCount = state.CpuCount,
                MemoryMB = state.MemoryMB,
                MMUEnabled = state.MMUEnabled,
                PageSizeKB = state.PageSizeKB
            };

            // Copy disk images
            foreach (var disk in state.DiskImages)
            {
                settings.DiskImages.Add(new DiskImageInfo
                {
                    Path = disk.Path,
                    Type = disk.Type,
                    ReadOnly = disk.ReadOnly,
                    SizeMB = disk.SizeMB,
                    Controller = disk.Controller,
                    Port = disk.Port
                });
            }

            return settings;
        }

        /// <summary>
        /// Apply these settings to a VMStateModel
        /// </summary>
        public void ApplyToVMState(VMStateModel state)
        {
            state.Name = Name;
            state.OperatingSystem = OperatingSystem;
            state.Architecture = Architecture;
            state.ISAType = ISAType;
            state.CpuCount = CpuCount;
            state.MemoryMB = MemoryMB;
            state.MMUEnabled = MMUEnabled;
            state.PageSizeKB = PageSizeKB;

            // Update disk images
            state.DiskImages.Clear();
            foreach (var disk in DiskImages)
            {
                state.DiskImages.Add(new DiskImageInfo
                {
                    Path = disk.Path,
                    Type = disk.Type,
                    ReadOnly = disk.ReadOnly,
                    SizeMB = disk.SizeMB,
                    Controller = disk.Controller,
                    Port = disk.Port
                });
            }
        }
    }
}

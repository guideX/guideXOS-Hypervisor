using System;
using System;
using System.Collections.Generic;
using System.Linq;
using guideXOS_Hypervisor_GUI.Models;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for the detailed VM information panel
    /// </summary>
    public class VMDetailsViewModel : ViewModelBase
    {
        private VMStateModel? _vmState;
        private ProfilerDataModel? _profilerData;
        private string _selectedTab = "Summary";

        public VMStateModel? VMState
        {
            get => _vmState;
            set
            {
                if (SetProperty(ref _vmState, value))
                {
                    OnPropertyChanged(nameof(Name));
                    OnPropertyChanged(nameof(UUID));
                    OnPropertyChanged(nameof(ISAType));
                    OnPropertyChanged(nameof(State));
                    OnPropertyChanged(nameof(StateDescription));
                    OnPropertyChanged(nameof(CpuCount));
                    OnPropertyChanged(nameof(MemoryMB));
                    OnPropertyChanged(nameof(MemoryDisplay));
                    OnPropertyChanged(nameof(Architecture));
                    OnPropertyChanged(nameof(OperatingSystem));
                    OnPropertyChanged(nameof(ConfigurationPath));
                    OnPropertyChanged(nameof(CreatedDate));
                    OnPropertyChanged(nameof(LastRunDate));
                    OnPropertyChanged(nameof(TotalCycles));
                    OnPropertyChanged(nameof(CyclesPerSecond));
                    OnPropertyChanged(nameof(HasVM));
                    OnPropertyChanged(nameof(MMUEnabled));
                    OnPropertyChanged(nameof(MMUStatus));
                    OnPropertyChanged(nameof(PageSizeKB));
                    OnPropertyChanged(nameof(PageSizeDisplay));
                    OnPropertyChanged(nameof(CurrentIP));
                    OnPropertyChanged(nameof(CurrentIPDisplay));
                    OnPropertyChanged(nameof(CFM));
                    OnPropertyChanged(nameof(CFMDisplay));
                    OnPropertyChanged(nameof(PSR));
                    OnPropertyChanged(nameof(PSRDisplay));
                    OnPropertyChanged(nameof(GeneralRegistersDisplay));
                    OnPropertyChanged(nameof(FloatRegistersDisplay));
                    OnPropertyChanged(nameof(PredicateRegistersDisplay));
                    OnPropertyChanged(nameof(BranchRegistersDisplay));
                    OnPropertyChanged(nameof(DiskImages));
                    OnPropertyChanged(nameof(HasDiskImages));
                    OnPropertyChanged(nameof(Breakpoints));
                    OnPropertyChanged(nameof(HasBreakpoints));
                    OnPropertyChanged(nameof(Watchpoints));
                    OnPropertyChanged(nameof(HasWatchpoints));
                }
            }
        }

        public ProfilerDataModel? ProfilerData
        {
            get => _profilerData;
            set
            {
                if (SetProperty(ref _profilerData, value))
                {
                    OnPropertyChanged(nameof(TotalInstructions));
                    OnPropertyChanged(nameof(AverageIPC));
                    OnPropertyChanged(nameof(CacheHitRate));
                    OnPropertyChanged(nameof(ExecutionTime));
                    OnPropertyChanged(nameof(HottestFunction));
                }
            }
        }

        public string SelectedTab
        {
            get => _selectedTab;
            set => SetProperty(ref _selectedTab, value);
        }

        // General Properties
        public bool HasVM => _vmState != null;
        public string Name => _vmState?.Name ?? "No VM Selected";
        public string UUID => _vmState?.UUID ?? "N/A";
        public string ISAType => _vmState?.ISAType ?? "N/A";
        public Models.VMState State => _vmState?.State ?? Models.VMState.PoweredOff;
        public string StateDescription => _vmState?.StateDescription ?? "N/A";
        public string Architecture => _vmState?.Architecture ?? "N/A";
        public string OperatingSystem => _vmState?.OperatingSystem ?? "N/A";
        public string ConfigurationPath => _vmState?.ConfigurationPath ?? "N/A";
        public DateTime? CreatedDate => _vmState?.CreatedDate;
        public DateTime? LastRunDate => _vmState?.LastRunDate;
        
        // System Properties
        public int CpuCount => _vmState?.CpuCount ?? 0;
        public ulong MemoryMB => _vmState?.MemoryMB ?? 0;
        public string MemoryDisplay => _vmState != null ? $"{_vmState.MemoryMB} MB" : "N/A";
        public bool MMUEnabled => _vmState?.MMUEnabled ?? false;
        public string MMUStatus => _vmState?.MMUEnabled == true ? "Enabled" : "Disabled";
        public ulong PageSizeKB => _vmState?.PageSizeKB ?? 0;
        public string PageSizeDisplay => _vmState != null ? $"{_vmState.PageSizeKB} KB" : "N/A";
        
        // CPU Properties
        public ulong CurrentIP => _vmState?.CurrentIP ?? 0;
        public string CurrentIPDisplay => _vmState != null ? $"0x{_vmState.CurrentIP:X16}" : "N/A";
        public ulong TotalCycles => _vmState?.TotalCycles ?? 0;
        public ulong CyclesPerSecond => _vmState?.CyclesPerSecond ?? 0;
        public string CyclesPerSecondDisplay => CyclesPerSecond > 0 
            ? $"{CyclesPerSecond / 1_000_000.0:F2} MHz" 
            : "0 Hz";
        public ulong CFM => _vmState?.CFM ?? 0;
        public string CFMDisplay => _vmState != null ? $"0x{_vmState.CFM:X16}" : "N/A";
        public ulong PSR => _vmState?.PSR ?? 0;
        public string PSRDisplay => _vmState != null ? $"0x{_vmState.PSR:X16}" : "N/A";
        
        // Register Snapshot Properties
        public List<RegisterDisplayItem> GeneralRegistersDisplay
        {
            get
            {
                if (_vmState?.GeneralRegisters == null) return new List<RegisterDisplayItem>();
                return _vmState.GeneralRegisters
                    .Select((val, idx) => new RegisterDisplayItem 
                    { 
                        Name = $"GR{idx}", 
                        Value = val ?? "0x0000000000000000" 
                    })
                    .ToList();
            }
        }
        
        public List<RegisterDisplayItem> FloatRegistersDisplay
        {
            get
            {
                if (_vmState?.FloatRegisters == null) return new List<RegisterDisplayItem>();
                return _vmState.FloatRegisters
                    .Select((val, idx) => new RegisterDisplayItem 
                    { 
                        Name = $"FR{idx}", 
                        Value = val ?? "0.0" 
                    })
                    .ToList();
            }
        }
        
        public List<RegisterDisplayItem> PredicateRegistersDisplay
        {
            get
            {
                if (_vmState?.PredicateRegisters == null) return new List<RegisterDisplayItem>();
                return _vmState.PredicateRegisters
                    .Select((val, idx) => new RegisterDisplayItem 
                    { 
                        Name = $"PR{idx}", 
                        Value = val ?? "0" 
                    })
                    .ToList();
            }
        }
        
        public List<RegisterDisplayItem> BranchRegistersDisplay
        {
            get
            {
                if (_vmState?.BranchRegisters == null) return new List<RegisterDisplayItem>();
                return _vmState.BranchRegisters
                    .Select((val, idx) => new RegisterDisplayItem 
                    { 
                        Name = $"BR{idx}", 
                        Value = val ?? "0x0000000000000000" 
                    })
                    .ToList();
            }
        }
        
        // Storage Properties
        public List<DiskImageInfo> DiskImages => _vmState?.DiskImages ?? new List<DiskImageInfo>();
        public bool HasDiskImages => DiskImages.Count > 0;
        
        // Debug Properties
        public List<BreakpointInfo> Breakpoints => _vmState?.Breakpoints ?? new List<BreakpointInfo>();
        public bool HasBreakpoints => Breakpoints.Count > 0;
        public List<WatchpointInfo> Watchpoints => _vmState?.Watchpoints ?? new List<WatchpointInfo>();
        public bool HasWatchpoints => Watchpoints.Count > 0;
        
        // Performance Properties
        public ulong TotalInstructions => _profilerData?.TotalInstructions ?? 0;
        public double AverageIPC => _profilerData?.AverageIPC ?? 0.0;
        public double CacheHitRate => _profilerData?.CacheHitRate ?? 0.0;
        public TimeSpan ExecutionTime => _profilerData?.ExecutionTime ?? TimeSpan.Zero;
        public string HottestFunction => _profilerData?.HottestFunction ?? "N/A";
        public string IPCDisplay => $"{AverageIPC:F2}";
        public string CacheHitRateDisplay => $"{CacheHitRate:F1}%";
        public string ExecutionTimeDisplay => ExecutionTime.TotalSeconds > 0 
            ? $"{ExecutionTime.TotalSeconds:F2}s" 
            : "N/A";
    }
    
    /// <summary>
    /// Helper class for displaying register name/value pairs
    /// </summary>
    public class RegisterDisplayItem
    {
        public string Name { get; set; } = string.Empty;
        public string Value { get; set; } = string.Empty;
    }
}

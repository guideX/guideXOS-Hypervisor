using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.Services;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for the IA-64 VM Debugger
    /// </summary>
    public class DebuggerViewModel : ViewModelBase
    {
        private readonly DispatcherTimer _updateTimer;
        private string _vmId = string.Empty;
        private bool _isAttached;
        private bool _isPaused;
        private IA64CPUState _cpuState;
        private ulong _memoryViewAddress;
        private int _memoryViewSize = 256;
        private string _statusMessage = "Not attached";
        private int _selectedRegisterTab;
        private string _breakpointAddress = string.Empty;
        private string _watchpointAddress = string.Empty;
        private int _nextBreakpointId = 1;
        private int _nextWatchpointId = 1;

        public DebuggerViewModel()
        {
            _cpuState = new IA64CPUState();
            MemoryRegions = new ObservableCollection<MemoryRegion>();
            Breakpoints = new ObservableCollection<Breakpoint>();
            Watchpoints = new ObservableCollection<Watchpoint>();

            // Commands
            AttachCommand = new RelayCommand(OnAttach, CanAttach);
            DetachCommand = new RelayCommand(OnDetach, CanDetach);
            StepInstructionCommand = new RelayCommand(OnStepInstruction, CanStep);
            ContinueCommand = new RelayCommand(OnContinue, CanContinue);
            PauseCommand = new RelayCommand(OnPause, CanPause);
            AddBreakpointCommand = new RelayCommand(OnAddBreakpoint, CanAddBreakpoint);
            RemoveBreakpointCommand = new RelayCommand<Breakpoint>(OnRemoveBreakpoint);
            ToggleBreakpointCommand = new RelayCommand<Breakpoint>(OnToggleBreakpoint);
            AddWatchpointCommand = new RelayCommand(OnAddWatchpoint, CanAddWatchpoint);
            RemoveWatchpointCommand = new RelayCommand<Watchpoint>(OnRemoveWatchpoint);
            ToggleWatchpointCommand = new RelayCommand<Watchpoint>(OnToggleWatchpoint);
            RefreshMemoryCommand = new RelayCommand(OnRefreshMemory, CanRefreshMemory);
            GoToAddressCommand = new RelayCommand(OnGoToAddress, CanGoToAddress);

            // Update timer for live updates when debugging
            _updateTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(100)
            };
            _updateTimer.Tick += UpdateTimer_Tick;
        }

        #region Properties

        public string VMId
        {
            get => _vmId;
            set => SetProperty(ref _vmId, value);
        }

        public bool IsAttached
        {
            get => _isAttached;
            set
            {
                if (SetProperty(ref _isAttached, value))
                {
                    CommandManager.InvalidateRequerySuggested();
                }
            }
        }

        public bool IsPaused
        {
            get => _isPaused;
            set
            {
                if (SetProperty(ref _isPaused, value))
                {
                    CommandManager.InvalidateRequerySuggested();
                }
            }
        }

        public IA64CPUState CPUState
        {
            get => _cpuState;
            set => SetProperty(ref _cpuState, value);
        }

        public ulong MemoryViewAddress
        {
            get => _memoryViewAddress;
            set => SetProperty(ref _memoryViewAddress, value);
        }

        public int MemoryViewSize
        {
            get => _memoryViewSize;
            set => SetProperty(ref _memoryViewSize, value);
        }

        public string StatusMessage
        {
            get => _statusMessage;
            set => SetProperty(ref _statusMessage, value);
        }

        public int SelectedRegisterTab
        {
            get => _selectedRegisterTab;
            set => SetProperty(ref _selectedRegisterTab, value);
        }

        public string BreakpointAddress
        {
            get => _breakpointAddress;
            set => SetProperty(ref _breakpointAddress, value);
        }

        public string WatchpointAddress
        {
            get => _watchpointAddress;
            set => SetProperty(ref _watchpointAddress, value);
        }

        public ObservableCollection<MemoryRegion> MemoryRegions { get; }
        public ObservableCollection<Breakpoint> Breakpoints { get; }
        public ObservableCollection<Watchpoint> Watchpoints { get; }

        // Computed properties
        public string InstructionPointerHex => $"0x{CPUState.InstructionPointer:X16}";
        public string CFMHex => $"0x{CPUState.CurrentFrameMarker:X16}";
        public string PSRHex => $"0x{CPUState.ProcessorStatusRegister:X16}";

        #endregion

        #region Commands

        public ICommand AttachCommand { get; }
        public ICommand DetachCommand { get; }
        public ICommand StepInstructionCommand { get; }
        public ICommand ContinueCommand { get; }
        public ICommand PauseCommand { get; }
        public ICommand AddBreakpointCommand { get; }
        public ICommand RemoveBreakpointCommand { get; }
        public ICommand ToggleBreakpointCommand { get; }
        public ICommand AddWatchpointCommand { get; }
        public ICommand RemoveWatchpointCommand { get; }
        public ICommand ToggleWatchpointCommand { get; }
        public ICommand RefreshMemoryCommand { get; }
        public ICommand GoToAddressCommand { get; }

        #endregion

        #region Command Handlers

        private bool CanAttach() => !IsAttached && !string.IsNullOrEmpty(VMId);
        private bool CanDetach() => IsAttached;
        private bool CanStep() => IsAttached && IsPaused;
        private bool CanContinue() => IsAttached && IsPaused;
        private bool CanPause() => IsAttached && !IsPaused;
        private bool CanAddBreakpoint() => IsAttached && !string.IsNullOrWhiteSpace(BreakpointAddress);
        private bool CanAddWatchpoint() => IsAttached && !string.IsNullOrWhiteSpace(WatchpointAddress);
        private bool CanRefreshMemory() => IsAttached;
        private bool CanGoToAddress() => IsAttached;

        private void OnAttach()
        {
            try
            {
                // Call native debugger attach
                VMManagerWrapper.Instance.AttachDebugger(VMId);
                IsAttached = true;
                IsPaused = true; // Attaching pauses the VM
                StatusMessage = $"Attached to VM {VMId}";
                
                // Load initial state
                RefreshCPUState();
                RefreshMemory();
                
                // Start update timer
                _updateTimer.Start();
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to attach: {ex.Message}";
                MessageBox.Show($"Failed to attach debugger: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnDetach()
        {
            try
            {
                _updateTimer.Stop();
                VMManagerWrapper.Instance.DetachDebugger(VMId);
                IsAttached = false;
                IsPaused = false;
                StatusMessage = "Detached";
                
                // Clear breakpoints and watchpoints
                Breakpoints.Clear();
                Watchpoints.Clear();
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to detach: {ex.Message}";
                MessageBox.Show($"Failed to detach debugger: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnStepInstruction()
        {
            try
            {
                VMManagerWrapper.Instance.StepInstruction(VMId);
                RefreshCPUState();
                RefreshMemory();
                StatusMessage = $"Stepped to IP: {InstructionPointerHex}";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Step failed: {ex.Message}";
                MessageBox.Show($"Failed to step: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnContinue()
        {
            try
            {
                VMManagerWrapper.Instance.ContinueExecution(VMId);
                IsPaused = false;
                StatusMessage = "Running...";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Continue failed: {ex.Message}";
                MessageBox.Show($"Failed to continue: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnPause()
        {
            try
            {
                VMManagerWrapper.Instance.PauseExecution(VMId);
                IsPaused = true;
                RefreshCPUState();
                StatusMessage = "Paused";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Pause failed: {ex.Message}";
                MessageBox.Show($"Failed to pause: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnAddBreakpoint()
        {
            try
            {
                if (ulong.TryParse(BreakpointAddress.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out ulong address))
                {
                    var breakpoint = new Breakpoint
                    {
                        Id = _nextBreakpointId++,
                        Address = address,
                        IsEnabled = true
                    };
                    
                    VMManagerWrapper.Instance.SetBreakpoint(VMId, address);
                    Breakpoints.Add(breakpoint);
                    BreakpointAddress = string.Empty;
                    StatusMessage = $"Breakpoint added at {breakpoint.HexAddress}";
                }
                else
                {
                    MessageBox.Show("Invalid address format. Use hexadecimal (e.g., 0x1000 or 1000)", 
                        "Invalid Address", MessageBoxButton.OK, MessageBoxImage.Warning);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to add breakpoint: {ex.Message}";
                MessageBox.Show($"Failed to add breakpoint: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnRemoveBreakpoint(Breakpoint? breakpoint)
        {
            if (breakpoint != null)
            {
                try
                {
                    VMManagerWrapper.Instance.RemoveBreakpoint(VMId, breakpoint.Address);
                    Breakpoints.Remove(breakpoint);
                    StatusMessage = $"Breakpoint removed from {breakpoint.HexAddress}";
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to remove breakpoint: {ex.Message}";
                }
            }
        }

        private void OnToggleBreakpoint(Breakpoint? breakpoint)
        {
            if (breakpoint != null)
            {
                try
                {
                    breakpoint.IsEnabled = !breakpoint.IsEnabled;
                    VMManagerWrapper.Instance.ToggleBreakpoint(VMId, breakpoint.Address, breakpoint.IsEnabled);
                    StatusMessage = $"Breakpoint {breakpoint.HexAddress} {(breakpoint.IsEnabled ? "enabled" : "disabled")}";
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to toggle breakpoint: {ex.Message}";
                }
            }
        }

        private void OnAddWatchpoint()
        {
            try
            {
                if (ulong.TryParse(WatchpointAddress.Replace("0x", ""), System.Globalization.NumberStyles.HexNumber, null, out ulong address))
                {
                    var watchpoint = new Watchpoint
                    {
                        Id = _nextWatchpointId++,
                        Address = address,
                        Size = 8,
                        Type = WatchpointType.ReadWrite,
                        IsEnabled = true
                    };
                    
                    VMManagerWrapper.Instance.SetWatchpoint(VMId, address, watchpoint.Size, (int)watchpoint.Type);
                    Watchpoints.Add(watchpoint);
                    WatchpointAddress = string.Empty;
                    StatusMessage = $"Watchpoint added at {watchpoint.HexAddress}";
                }
                else
                {
                    MessageBox.Show("Invalid address format. Use hexadecimal (e.g., 0x1000 or 1000)", 
                        "Invalid Address", MessageBoxButton.OK, MessageBoxImage.Warning);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to add watchpoint: {ex.Message}";
                MessageBox.Show($"Failed to add watchpoint: {ex.Message}", "Debugger Error", 
                    MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnRemoveWatchpoint(Watchpoint? watchpoint)
        {
            if (watchpoint != null)
            {
                try
                {
                    VMManagerWrapper.Instance.RemoveWatchpoint(VMId, watchpoint.Address);
                    Watchpoints.Remove(watchpoint);
                    StatusMessage = $"Watchpoint removed from {watchpoint.HexAddress}";
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to remove watchpoint: {ex.Message}";
                }
            }
        }

        private void OnToggleWatchpoint(Watchpoint? watchpoint)
        {
            if (watchpoint != null)
            {
                try
                {
                    watchpoint.IsEnabled = !watchpoint.IsEnabled;
                    VMManagerWrapper.Instance.ToggleWatchpoint(VMId, watchpoint.Address, watchpoint.IsEnabled);
                    StatusMessage = $"Watchpoint {watchpoint.HexAddress} {(watchpoint.IsEnabled ? "enabled" : "disabled")}";
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to toggle watchpoint: {ex.Message}";
                }
            }
        }

        private void OnRefreshMemory()
        {
            RefreshMemory();
        }

        private void OnGoToAddress()
        {
            // Memory view address is bound to UI, refresh automatically
            RefreshMemory();
        }

        #endregion

        #region Helper Methods

        private void RefreshCPUState()
        {
            try
            {
                var state = VMManagerWrapper.Instance.GetDebuggerState(VMId);
                
                // Update instruction pointer
                CPUState.InstructionPointer = state.CurrentIP;
                CPUState.CurrentFrameMarker = state.CFM;
                CPUState.ProcessorStatusRegister = state.PSR;
                
                // Update registers - parse from string values
                for (int i = 0; i < 128; i++)
                {
                    if (i < state.GeneralRegisters.Length && !string.IsNullOrEmpty(state.GeneralRegisters[i]))
                    {
                        if (ulong.TryParse(state.GeneralRegisters[i].Replace("0x", ""), 
                            System.Globalization.NumberStyles.HexNumber, null, out ulong grValue))
                        {
                            CPUState.GeneralRegisters[i].Value = grValue;
                        }
                    }
                        
                    if (i < state.FloatRegisters.Length && !string.IsNullOrEmpty(state.FloatRegisters[i]))
                    {
                        if (ulong.TryParse(state.FloatRegisters[i].Replace("0x", ""), 
                            System.Globalization.NumberStyles.HexNumber, null, out ulong frValue))
                        {
                            CPUState.FloatingPointRegisters[i].RawBits = frValue;
                        }
                    }
                }
                
                for (int i = 0; i < 64; i++)
                {
                    if (i < state.PredicateRegisters.Length && !string.IsNullOrEmpty(state.PredicateRegisters[i]))
                    {
                        CPUState.PredicateRegisters[i].Value = state.PredicateRegisters[i] == "1";
                    }
                }
                
                for (int i = 0; i < 8; i++)
                {
                    if (i < state.BranchRegisters.Length && !string.IsNullOrEmpty(state.BranchRegisters[i]))
                    {
                        if (ulong.TryParse(state.BranchRegisters[i].Replace("0x", ""), 
                            System.Globalization.NumberStyles.HexNumber, null, out ulong brValue))
                        {
                            CPUState.BranchRegisters[i].Value = brValue;
                        }
                    }
                }
                
                // Notify UI of changes
                OnPropertyChanged(nameof(InstructionPointerHex));
                OnPropertyChanged(nameof(CFMHex));
                OnPropertyChanged(nameof(PSRHex));
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to refresh CPU state: {ex.Message}";
            }
        }

        private void RefreshMemory()
        {
            try
            {
                var memory = VMManagerWrapper.Instance.ReadMemory(VMId, MemoryViewAddress, MemoryViewSize);
                
                MemoryRegions.Clear();
                
                // Display memory in 16-byte rows
                for (int i = 0; i < memory.Length; i += 16)
                {
                    int rowSize = Math.Min(16, memory.Length - i);
                    byte[] rowData = new byte[rowSize];
                    Array.Copy(memory, i, rowData, 0, rowSize);
                    
                    MemoryRegions.Add(new MemoryRegion
                    {
                        Address = MemoryViewAddress + (ulong)i,
                        Data = rowData
                    });
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to read memory: {ex.Message}";
            }
        }

        private void UpdateTimer_Tick(object? sender, EventArgs e)
        {
            if (!IsAttached)
            {
                _updateTimer.Stop();
                return;
            }
            
            // Only update if paused (when running, we don't want to spam updates)
            if (IsPaused)
            {
                RefreshCPUState();
            }
        }

        /// <summary>
        /// Attach debugger to a specific VM
        /// </summary>
        public void AttachToVM(string vmId)
        {
            VMId = vmId;
            if (CanAttach())
            {
                OnAttach();
            }
        }

        #endregion
    }
}

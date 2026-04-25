using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Windows;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Threading;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.Services;
using guideXOS_Hypervisor_GUI.Views;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// Main ViewModel for the VirtualBox-style VM Manager
    /// </summary>
    public class MainViewModel : ViewModelBase
    {
        private readonly DispatcherTimer _updateTimer;
        private VMListItemViewModel? _selectedVM;
        private string _statusMessage = "Ready";
        private bool _isLoading;
        private string _searchText = string.Empty;
        private readonly Dictionary<string, VMScreenWindow> _screenWindows = new();

        public MainViewModel()
        {
            VirtualMachines = new ObservableCollection<VMListItemViewModel>();
            FilteredVirtualMachines = CollectionViewSource.GetDefaultView(VirtualMachines);
            FilteredVirtualMachines.Filter = FilterVM;
            DetailsViewModel = new VMDetailsViewModel();
            PerformanceMonitorViewModel = new PerformanceMonitorViewModel();

            // Commands
            NewVMCommand = new RelayCommand(OnNewVM);
            StartVMCommand = new RelayCommand(OnStartVM, CanStartVM);
            StopVMCommand = new RelayCommand(OnStopVM, CanStopVM);
            PauseVMCommand = new RelayCommand(OnPauseVM, CanPauseVM);
            ResetVMCommand = new RelayCommand(OnResetVM, CanResetVM);
            SnapshotVMCommand = new RelayCommand(OnSnapshotVM, CanSnapshotVM);
            SettingsVMCommand = new RelayCommand(OnSettingsVM, CanExecuteVMCommand);
            ShowScreenCommand = new RelayCommand(OnShowScreen, CanExecuteVMCommand);
            DebugVMCommand = new RelayCommand(OnDebugVM, CanExecuteVMCommand);
            DeleteVMCommand = new RelayCommand(OnDeleteVM, CanExecuteVMCommand);
            RefreshCommand = new RelayCommand(OnRefresh);

            // Setup update timer for real-time stats
            _updateTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(500)
            };
            _updateTimer.Tick += UpdateTimer_Tick;
            _updateTimer.Start();

            // Load initial VMs
            LoadVirtualMachines();
        }

        #region Properties

        public ObservableCollection<VMListItemViewModel> VirtualMachines { get; }

        public ICollectionView FilteredVirtualMachines { get; }

        public VMDetailsViewModel DetailsViewModel { get; }
        
        public PerformanceMonitorViewModel PerformanceMonitorViewModel { get; }

        public string SearchText
        {
            get => _searchText;
            set
            {
                if (SetProperty(ref _searchText, value))
                {
                    FilteredVirtualMachines.Refresh();
                }
            }
        }

        public VMListItemViewModel? SelectedVM
        {
            get => _selectedVM;
            set
            {
                if (SetProperty(ref _selectedVM, value))
                {
                    if (_selectedVM != null)
                    {
                        _selectedVM.IsSelected = true;
                        DetailsViewModel.VMState = _selectedVM.GetModel();
                        LoadProfilerData(_selectedVM.Id);
                        
                        // Start monitoring for the selected VM
                        PerformanceMonitorViewModel.StartMonitoring(_selectedVM.Id, _selectedVM.State);
                    }
                    else
                    {
                        DetailsViewModel.VMState = null;
                        DetailsViewModel.ProfilerData = null;
                        
                        // Stop monitoring when no VM is selected
                        PerformanceMonitorViewModel.StopMonitoring();
                    }

                    // Update command states
                    CommandManager.InvalidateRequerySuggested();
                }
            }
        }

        public string StatusMessage
        {
            get => _statusMessage;
            set => SetProperty(ref _statusMessage, value);
        }

        public bool IsLoading
        {
            get => _isLoading;
            set => SetProperty(ref _isLoading, value);
        }

        public int TotalVMs => VirtualMachines.Count;
        public int RunningVMs => VirtualMachines.Count(vm => vm.State == VMState.Running);
        public int ActiveCPUs => VirtualMachines.Where(vm => vm.State == VMState.Running).Sum(vm => vm.CpuCount);
        public ulong TotalMemoryAllocated => VirtualMachines.Aggregate(0UL, (sum, vm) => sum + vm.MemoryMB);

        #endregion

        #region Commands

        public ICommand NewVMCommand { get; }
        public ICommand StartVMCommand { get; }
        public ICommand StopVMCommand { get; }
        public ICommand PauseVMCommand { get; }
        public ICommand ResetVMCommand { get; }
        public ICommand SnapshotVMCommand { get; }
        public ICommand SettingsVMCommand { get; }
        public ICommand ShowScreenCommand { get; }
        public ICommand DebugVMCommand { get; }
        public ICommand DeleteVMCommand { get; }
        public ICommand RefreshCommand { get; }

        #endregion

        #region Command Implementations

        private bool CanExecuteVMCommand() => SelectedVM != null;
        private bool CanStartVM() => SelectedVM != null && SelectedVM.State == VMState.PoweredOff;
        private bool CanStopVM() => SelectedVM != null && SelectedVM.State == VMState.Running;
        private bool CanPauseVM() => SelectedVM != null && SelectedVM.State == VMState.Running;
        private bool CanResetVM() => SelectedVM != null && SelectedVM.State == VMState.Running;
        private bool CanSnapshotVM() => SelectedVM != null && (SelectedVM.State == VMState.Running || SelectedVM.State == VMState.Paused);

        private void OnNewVM()
        {
            StatusMessage = "Creating new virtual machine...";
            
            try
            {
                // Prompt user to select ISO/IMG file
                var openDialog = new Microsoft.Win32.OpenFileDialog
                {
                    Filter = "Disk Images|*.iso;*.img;*.ima|ISO Images (*.iso)|*.iso|IMG Files (*.img;*.ima)|*.img;*.ima|All Files (*.*)|*.*",
                    Title = "Select Bootable Disk Image (Optional)",
                    CheckFileExists = true
                };
                
                string? isoPath = openDialog.ShowDialog() == true ? openDialog.FileName : null;
                
                // Create VM configuration
                VMConfiguration config;
                if (!string.IsNullOrEmpty(isoPath))
                {
                    // Create VM with ISO attached
                    config = VMConfiguration.CreateWithISO(
                        name: $"Virtual Machine {VirtualMachines.Count + 1}",
                        isoPath: isoPath,
                        memoryMB: 512,
                        cpuCount: 1
                    );
                }
                else
                {
                    // Create VM without storage
                    config = new VMConfiguration
                    {
                        Name = $"Virtual Machine {VirtualMachines.Count + 1}",
                        CpuCount = 1,
                        MemoryMB = 512,
                        BootDevice = "disk0",
                        EnableProfiler = true
                    };
                }

                // Call VMManager.CreateVM
                var vmId = VMManagerWrapper.Instance.CreateVM(config);

                var newVM = new VMStateModel
                {
                    Id = vmId,
                    Name = config.Name,
                    State = VMState.PoweredOff,
                    CpuCount = config.CpuCount,
                    MemoryMB = config.MemoryMB,
                    Architecture = "IA-64",
                    OperatingSystem = "IA-64 System",
                    CreatedDate = DateTime.Now,
                    ConfigurationPath = $"VMs/{vmId}.json"
                };
                
                // Store disk path if provided
                if (!string.IsNullOrEmpty(isoPath))
                {
                    newVM.DiskImages.Add(new DiskImageInfo
                    {
                        Path = isoPath,
                        Type = "ISO",
                        ReadOnly = true,
                        Controller = "SATA",
                        Port = 0
                    });

                    // Store the boot ISO path in the VM state
                    newVM.BootFromISO = isoPath;
                }

                var vmViewModel = new VMListItemViewModel(newVM);
                VirtualMachines.Add(vmViewModel);
                SelectedVM = vmViewModel;

                // Save the new VM to disk
                VMPersistenceService.Instance.SaveVM(newVM);

                StatusMessage = $"Created virtual machine: {newVM.Name}" + 
                    (isoPath != null ? $" with disk image: {System.IO.Path.GetFileName(isoPath)}" : "");
                UpdateStatistics();
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error creating VM: {ex.Message}";
                MessageBox.Show($"Failed to create virtual machine:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnStartVM()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Starting {SelectedVM.Name}...";
            
            try
            {
                Console.WriteLine($"OnStartVM: Attempting to start VM {SelectedVM.Id}");
                
                // Call VMManager.Start(vmId)
                bool success = VMManagerWrapper.Instance.StartVM(SelectedVM.Id);
                
                Console.WriteLine($"OnStartVM: StartVM returned {success}");
                
                if (success)
                {
                    SelectedVM.State = VMState.Running;
                    SelectedVM.GetModel().LastRunDate = DateTime.Now;
                    StatusMessage = $"{SelectedVM.Name} is running";
                    UpdateStatistics();
                    
                    // Start performance monitoring
                    PerformanceMonitorViewModel.StartMonitoring(SelectedVM.Id, VMState.Running);
                    
                    // Update screen window if open
                    if (_screenWindows.TryGetValue(SelectedVM.Id, out var screenWindow))
                    {
                        screenWindow.UpdateRunningState(true);
                    }
                }
                else
                {
                    string errorMsg = $"Failed to start {SelectedVM.Name}. Check console output for details.";
                    StatusMessage = errorMsg;
                    Console.WriteLine($"ERROR: {errorMsg}");
                    Console.WriteLine("This usually means:");
                    Console.WriteLine("1. The C++ VMManager_StartVM returned false");
                    Console.WriteLine("2. The VM doesn't exist in the C++ backend");
                    Console.WriteLine("3. The VM is in an invalid state");
                    MessageBox.Show($"Failed to start virtual machine: {SelectedVM.Name}\n\nCheck the console window for detailed error information.", 
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error starting VM: {ex.Message}";
                Console.WriteLine($"EXCEPTION in OnStartVM: {ex}");
                MessageBox.Show($"Failed to start virtual machine:\n{ex.Message}\n\nStack trace:\n{ex.StackTrace}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnStopVM()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Stopping {SelectedVM.Name}...";
            
            try
            {
                // Call VMManager.Stop(vmId)
                bool success = VMManagerWrapper.Instance.StopVM(SelectedVM.Id);
                
                if (success)
                {
                    SelectedVM.State = VMState.PoweredOff;
                    StatusMessage = $"{SelectedVM.Name} stopped";
                    UpdateStatistics();
                    
                    // Stop performance monitoring
                    PerformanceMonitorViewModel.StopMonitoring();
                    
                    // Update screen window if open
                    if (_screenWindows.TryGetValue(SelectedVM.Id, out var screenWindow))
                    {
                        screenWindow.UpdateRunningState(false);
                    }
                }
                else
                {
                    StatusMessage = $"Failed to stop {SelectedVM.Name}";
                    MessageBox.Show($"Failed to stop virtual machine: {SelectedVM.Name}", 
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error stopping VM: {ex.Message}";
                MessageBox.Show($"Failed to stop virtual machine:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnPauseVM()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Pausing {SelectedVM.Name}...";
            
            try
            {
                // TODO: Call VMManager.PauseVM(SelectedVM.Id)
                SelectedVM.State = VMState.Paused;
                StatusMessage = $"{SelectedVM.Name} paused";
                UpdateStatistics();
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error pausing VM: {ex.Message}";
            }
        }

        private void OnResetVM()
        {
            if (SelectedVM == null) return;

            var result = MessageBox.Show(
                $"Are you sure you want to reset {SelectedVM.Name}?\nAll unsaved data will be lost.",
                "Confirm Reset",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning);

            if (result == MessageBoxResult.Yes)
            {
                StatusMessage = $"Resetting {SelectedVM.Name}...";
                
                try
                {
                    // Call VMManager.Reset(vmId)
                    bool success = VMManagerWrapper.Instance.ResetVM(SelectedVM.Id);
                    
                    if (success)
                    {
                        StatusMessage = $"{SelectedVM.Name} reset";
                    }
                    else
                    {
                        StatusMessage = $"Failed to reset {SelectedVM.Name}";
                        MessageBox.Show($"Failed to reset virtual machine: {SelectedVM.Name}", 
                            "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                    }
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Error resetting VM: {ex.Message}";
                    MessageBox.Show($"Failed to reset virtual machine:\n{ex.Message}", 
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        private void OnSnapshotVM()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Opening snapshot manager for {SelectedVM.Name}...";
            
            try
            {
                // Open the snapshot manager window
                var snapshotWindow = new Views.SnapshotManagerView(SelectedVM.Id, SelectedVM.Name);
                snapshotWindow.Show();
                StatusMessage = $"Snapshot manager opened for {SelectedVM.Name}";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error opening snapshot manager: {ex.Message}";
                MessageBox.Show($"Failed to open snapshot manager:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnSettingsVM()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Opening settings for {SelectedVM.Name}...";
            
            try
            {
                // Get the VM state model
                var vmState = SelectedVM.GetModel();
                
                // Open the settings dialog
                var settingsWindow = new Views.VMSettingsView(vmState);
                var result = settingsWindow.ShowDialog();
                
                if (result == true)
                {
                    // Settings were saved, update the VM
                    SelectedVM.UpdateFromModel(vmState);
                    
                    // Save changes to disk
                    VMPersistenceService.Instance.SaveVM(vmState);
                    
                    StatusMessage = $"Settings saved for {SelectedVM.Name}";
                    UpdateStatistics();
                }
                else
                {
                    StatusMessage = $"Settings cancelled for {SelectedVM.Name}";
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error opening settings: {ex.Message}";
                MessageBox.Show($"Failed to open settings:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnShowScreen()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Opening screen for {SelectedVM.Name}...";
            
            try
            {
                // Check if screen window already exists for this VM
                if (_screenWindows.TryGetValue(SelectedVM.Id, out var existingWindow))
                {
                    // Bring existing window to front
                    existingWindow.Activate();
                    existingWindow.Focus();
                    StatusMessage = $"Screen window for {SelectedVM.Name} is already open";
                    return;
                }
                
                // Create new screen window
                var screenWindow = new VMScreenWindow(
                    SelectedVM.Id, 
                    SelectedVM.Name, 
                    SelectedVM.State == VMState.Running);
                
                // Track the window
                _screenWindows[SelectedVM.Id] = screenWindow;
                
                // Remove from tracking when closed
                screenWindow.Closed += (s, e) => _screenWindows.Remove(SelectedVM.Id);
                
                screenWindow.Show();
                StatusMessage = $"Screen opened for {SelectedVM.Name}";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error opening screen: {ex.Message}";
                MessageBox.Show($"Failed to open VM screen:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnDebugVM()
        {
            if (SelectedVM == null) return;

            StatusMessage = $"Opening debugger for {SelectedVM.Name}...";
            
            try
            {
                // Open the debugger window
                var debuggerWindow = new Views.DebuggerView(SelectedVM.Id);
                debuggerWindow.Show();
                StatusMessage = $"Debugger opened for {SelectedVM.Name}";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error opening debugger: {ex.Message}";
                MessageBox.Show($"Failed to open debugger:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnDeleteVM()
        {
            if (SelectedVM == null) return;

            var result = MessageBox.Show(
                $"Are you sure you want to delete {SelectedVM.Name}?\nThis action cannot be undone.",
                "Confirm Deletion",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning);

            if (result == MessageBoxResult.Yes)
            {
                StatusMessage = $"Deleting {SelectedVM.Name}...";
                
                try
                {
                    var vmId = SelectedVM.Id;
                    
                    // Remove from disk
                    VMPersistenceService.Instance.DeleteVM(vmId);
                    
                    // Remove from collection
                    VirtualMachines.Remove(SelectedVM);
                    SelectedVM = VirtualMachines.FirstOrDefault();
                    StatusMessage = "Virtual machine deleted";
                    UpdateStatistics();
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Error deleting VM: {ex.Message}";
                    MessageBox.Show($"Failed to delete virtual machine:\n{ex.Message}", 
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        private void OnRefresh()
        {
            StatusMessage = "Refreshing virtual machines...";
            LoadVirtualMachines();
            StatusMessage = "Refreshed";
        }

        #endregion

        #region Private Methods

        private void LoadVirtualMachines()
        {
            IsLoading = true;
            
            try
            {
                // Clear any existing VMs
                VirtualMachines.Clear();
                
                // TODO: Load VMs from persistence and recreate them in C++ backend
                // For now, start with empty list - VMs are only created via "New" button
                // This ensures C# and C++ VM lists are in sync
                
                Console.WriteLine("VM list initialized (empty). Create VMs using the 'New' button.");

                UpdateStatistics();
                StatusMessage = $"Loaded {VirtualMachines.Count} virtual machines";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error loading VMs: {ex.Message}";
                MessageBox.Show($"Failed to load virtual machines:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                IsLoading = false;
            }
        }

        private void LoadProfilerData(string vmId)
        {
            try
            {
                // TODO: Load profiler data from VMManager
                // For now, create sample data
                DetailsViewModel.ProfilerData = new ProfilerDataModel
                {
                    TotalInstructions = 1_234_567,
                    TotalCycles = 2_345_678,
                    AverageIPC = 1.9,
                    CacheHits = 950_000,
                    CacheMisses = 50_000,
                    CacheHitRate = 95.0,
                    ExecutionTime = TimeSpan.FromSeconds(2.5),
                    HottestFunction = "kernel_main",
                    HottestFunctionCount = 45_678
                };
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error loading profiler data: {ex.Message}";
            }
        }

        private void UpdateStatistics()
        {
            OnPropertyChanged(nameof(TotalVMs));
            OnPropertyChanged(nameof(RunningVMs));
            OnPropertyChanged(nameof(ActiveCPUs));
            OnPropertyChanged(nameof(TotalMemoryAllocated));
        }

        private void UpdateTimer_Tick(object? sender, EventArgs e)
        {
            // Update running VMs with real-time stats
            foreach (var vm in VirtualMachines.Where(v => v.State == VMState.Running))
            {
                // TODO: Update from VMManager
                // Simulate some activity for now
                var model = vm.GetModel();
                model.CyclesPerSecond = (ulong)(100_000_000 + Random.Shared.Next(-1_000_000, 1_000_000));
                model.TotalCycles += model.CyclesPerSecond / 2;
                
                vm.UpdateFromModel(model);
            }

            // Update details if a running VM is selected
            if (SelectedVM?.State == VMState.Running)
            {
                DetailsViewModel.VMState = SelectedVM.GetModel();
                LoadProfilerData(SelectedVM.Id);
            }
        }

        private bool FilterVM(object obj)
        {
            if (string.IsNullOrWhiteSpace(SearchText))
                return true;

            if (obj is VMListItemViewModel vm)
            {
                return vm.Name.Contains(SearchText, StringComparison.OrdinalIgnoreCase) ||
                       vm.StateDescription.Contains(SearchText, StringComparison.OrdinalIgnoreCase) ||
                       vm.OperatingSystem.Contains(SearchText, StringComparison.OrdinalIgnoreCase);
            }

            return false;
        }

        /// <summary>
        /// Save all VMs and cleanup resources
        /// </summary>
        public void Cleanup()
        {
            try
            {
                // Stop the update timer
                _updateTimer?.Stop();
                
                // Save all VMs to disk
                var vmsToSave = VirtualMachines.Select(vm => vm.GetModel()).ToList();
                VMPersistenceService.Instance.SaveAllVMs(vmsToSave);
                
                // Close all screen windows
                foreach (var window in _screenWindows.Values.ToList())
                {
                    window.Close();
                }
                _screenWindows.Clear();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error during cleanup: {ex.Message}");
            }
        }

        #endregion
    }
}

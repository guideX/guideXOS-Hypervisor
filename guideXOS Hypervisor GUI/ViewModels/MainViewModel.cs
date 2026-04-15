using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Linq;
using System.Windows;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Threading;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.Services;

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
                // TODO: Show VM creation wizard
                // For now, create a sample VM configuration
                var config = new VMConfiguration
                {
                    Name = $"Virtual Machine {VirtualMachines.Count + 1}",
                    CpuCount = 1,
                    MemoryMB = 512,
                    BootDevice = "disk",
                    EnableProfiler = true
                };

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

                var vmViewModel = new VMListItemViewModel(newVM);
                VirtualMachines.Add(vmViewModel);
                SelectedVM = vmViewModel;

                StatusMessage = $"Created virtual machine: {newVM.Name}";
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
                // Call VMManager.Start(vmId)
                bool success = VMManagerWrapper.Instance.StartVM(SelectedVM.Id);
                
                if (success)
                {
                    SelectedVM.State = VMState.Running;
                    SelectedVM.GetModel().LastRunDate = DateTime.Now;
                    StatusMessage = $"{SelectedVM.Name} is running";
                    UpdateStatistics();
                    
                    // Start performance monitoring
                    PerformanceMonitorViewModel.StartMonitoring(SelectedVM.Id, VMState.Running);
                }
                else
                {
                    StatusMessage = $"Failed to start {SelectedVM.Name}";
                    MessageBox.Show($"Failed to start virtual machine: {SelectedVM.Name}", 
                        "Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error starting VM: {ex.Message}";
                MessageBox.Show($"Failed to start virtual machine:\n{ex.Message}", 
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
                    
                    // TODO: Persist changes to VMManager
                    // VMManagerWrapper.Instance.UpdateVMConfiguration(vmState.Id, settingsWindow.ViewModel.GetSettings());
                    
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
                    // TODO: Call VMManager.DeleteVM(SelectedVM.Id)
                    VirtualMachines.Remove(SelectedVM);
                    SelectedVM = VirtualMachines.FirstOrDefault();
                    StatusMessage = "Virtual machine deleted";
                    UpdateStatistics();
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Error deleting VM: {ex.Message}";
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
                // TODO: Load VMs from VMManager
                // For now, create sample VMs for demonstration
                VirtualMachines.Clear();

                var sampleVMs = new[]
                {
                    new VMStateModel
                    {
                        Id = Guid.NewGuid().ToString(),
                        Name = "IA-64 Development",
                        State = VMState.PoweredOff,
                        CpuCount = 2,
                        MemoryMB = 1024,
                        Architecture = "IA-64",
                        OperatingSystem = "IA-64 Linux",
                        CreatedDate = DateTime.Now.AddDays(-7),
                        ConfigurationPath = "VMs/dev.json"
                    },
                    new VMStateModel
                    {
                        Id = Guid.NewGuid().ToString(),
                        Name = "Test Environment",
                        State = VMState.PoweredOff,
                        CpuCount = 1,
                        MemoryMB = 512,
                        Architecture = "IA-64",
                        OperatingSystem = "IA-64 System",
                        CreatedDate = DateTime.Now.AddDays(-3),
                        ConfigurationPath = "VMs/test.json"
                    }
                };

                foreach (var vm in sampleVMs)
                {
                    VirtualMachines.Add(new VMListItemViewModel(vm));
                }

                if (VirtualMachines.Any())
                {
                    SelectedVM = VirtualMachines[0];
                }

                UpdateStatistics();
                StatusMessage = $"Loaded {VirtualMachines.Count} virtual machines";
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error loading VMs: {ex.Message}";
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

        #endregion
    }
}

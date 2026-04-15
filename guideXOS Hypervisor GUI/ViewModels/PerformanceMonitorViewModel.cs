using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.Services;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for the Performance Monitor panel
    /// Displays live profiler data with 500ms refresh rate
    /// </summary>
    public class PerformanceMonitorViewModel : ViewModelBase
    {
        private readonly DispatcherTimer _updateTimer;
        private string? _currentVMId;
        private ProfilerDataModel? _profilerData;
        private bool _isMonitoring;
        private string _statusMessage = "No VM selected";
        
        public PerformanceMonitorViewModel()
        {
            // Commands
            ExportJsonCommand = new RelayCommand(OnExportJson, CanExport);
            ExportDotCommand = new RelayCommand(OnExportDot, CanExport);
            ExportCsvCommand = new RelayCommand(OnExportCsv, CanExport);
            StartMonitoringCommand = new RelayCommand(OnStartMonitoring, CanStartMonitoring);
            StopMonitoringCommand = new RelayCommand(OnStopMonitoring, CanStopMonitoring);
            
            // Setup 500ms update timer
            _updateTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(500)
            };
            _updateTimer.Tick += UpdateTimer_Tick;
        }
        
        #region Properties
        
        public ProfilerDataModel? ProfilerData
        {
            get => _profilerData;
            set
            {
                if (SetProperty(ref _profilerData, value))
                {
                    OnPropertyChanged(nameof(HasData));
                    OnPropertyChanged(nameof(TotalInstructions));
                    OnPropertyChanged(nameof(AverageIPC));
                    OnPropertyChanged(nameof(ExecutionTime));
                    OnPropertyChanged(nameof(TopInstructions));
                    OnPropertyChanged(nameof(HotPaths));
                    OnPropertyChanged(nameof(GeneralRegisterPressure));
                    OnPropertyChanged(nameof(FloatRegisterPressure));
                    OnPropertyChanged(nameof(PredicateRegisterPressure));
                    OnPropertyChanged(nameof(BranchRegisterPressure));
                    OnPropertyChanged(nameof(MemoryBreakdown));
                    OnPropertyChanged(nameof(LastUpdateTime));
                }
            }
        }
        
        public bool IsMonitoring
        {
            get => _isMonitoring;
            set
            {
                if (SetProperty(ref _isMonitoring, value))
                {
                    CommandManager.InvalidateRequerySuggested();
                }
            }
        }
        
        public string StatusMessage
        {
            get => _statusMessage;
            set => SetProperty(ref _statusMessage, value);
        }
        
        // Data Properties
        public bool HasData => _profilerData != null;
        public string TotalInstructions => _profilerData?.TotalInstructions.ToString("N0") ?? "0";
        public string AverageIPC => _profilerData?.AverageIPC.ToString("F2") ?? "0.00";
        public string ExecutionTime => _profilerData?.ExecutionTime.ToString(@"hh\:mm\:ss\.fff") ?? "00:00:00.000";
        public string LastUpdateTime => _profilerData?.LastUpdateTime.ToString("HH:mm:ss.fff") ?? "N/A";
        
        // Collections
        public ObservableCollection<InstructionFrequencyInfo> TopInstructions { get; } = new ObservableCollection<InstructionFrequencyInfo>();
        public ObservableCollection<HotPathInfo> HotPaths { get; } = new ObservableCollection<HotPathInfo>();
        public ObservableCollection<RegisterUsageInfo> GeneralRegisterPressure { get; } = new ObservableCollection<RegisterUsageInfo>();
        public ObservableCollection<RegisterUsageInfo> FloatRegisterPressure { get; } = new ObservableCollection<RegisterUsageInfo>();
        public ObservableCollection<RegisterUsageInfo> PredicateRegisterPressure { get; } = new ObservableCollection<RegisterUsageInfo>();
        public ObservableCollection<RegisterUsageInfo> BranchRegisterPressure { get; } = new ObservableCollection<RegisterUsageInfo>();
        
        public MemoryAccessBreakdown? MemoryBreakdown => _profilerData?.MemoryBreakdown;
        
        #endregion
        
        #region Commands
        
        public ICommand ExportJsonCommand { get; }
        public ICommand ExportDotCommand { get; }
        public ICommand ExportCsvCommand { get; }
        public ICommand StartMonitoringCommand { get; }
        public ICommand StopMonitoringCommand { get; }
        
        #endregion
        
        #region Public Methods
        
        /// <summary>
        /// Start monitoring a specific VM
        /// </summary>
        public void StartMonitoring(string vmId, VMState vmState)
        {
            _currentVMId = vmId;
            
            if (vmState == VMState.Running)
            {
                IsMonitoring = true;
                _updateTimer.Start();
                StatusMessage = "Monitoring active - updating every 500ms";
            }
            else
            {
                IsMonitoring = false;
                _updateTimer.Stop();
                StatusMessage = "VM is not running";
            }
        }
        
        /// <summary>
        /// Stop monitoring
        /// </summary>
        public void StopMonitoring()
        {
            IsMonitoring = false;
            _updateTimer.Stop();
            StatusMessage = "Monitoring stopped";
        }
        
        /// <summary>
        /// Clear all profiler data
        /// </summary>
        public void ClearData()
        {
            ProfilerData = null;
            TopInstructions.Clear();
            HotPaths.Clear();
            GeneralRegisterPressure.Clear();
            FloatRegisterPressure.Clear();
            PredicateRegisterPressure.Clear();
            BranchRegisterPressure.Clear();
            StatusMessage = "No VM selected";
        }
        
        #endregion
        
        #region Private Methods
        
        private void UpdateTimer_Tick(object? sender, EventArgs e)
        {
            if (!IsMonitoring || string.IsNullOrEmpty(_currentVMId))
                return;
                
            try
            {
                // Get profiler data from VMManager
                var profilerData = VMManagerWrapper.Instance.GetProfilerData(_currentVMId);
                
                if (profilerData != null)
                {
                    profilerData.IsLive = true;
                    profilerData.LastUpdateTime = DateTime.Now;
                    UpdateProfilerData(profilerData);
                    StatusMessage = $"Last update: {profilerData.LastUpdateTime:HH:mm:ss.fff}";
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error updating profiler data: {ex.Message}";
            }
        }
        
        private void UpdateProfilerData(ProfilerDataModel data)
        {
            ProfilerData = data;
            
            // Update top instructions
            TopInstructions.Clear();
            foreach (var instruction in data.TopInstructions.Take(10))
            {
                TopInstructions.Add(instruction);
            }
            
            // Update hot paths
            HotPaths.Clear();
            foreach (var path in data.HotPaths.Take(10))
            {
                HotPaths.Add(path);
            }
            
            // Update register pressure
            GeneralRegisterPressure.Clear();
            foreach (var reg in data.RegisterPressure.GeneralRegisters.Take(10))
            {
                GeneralRegisterPressure.Add(reg);
            }
            
            FloatRegisterPressure.Clear();
            foreach (var reg in data.RegisterPressure.FloatRegisters.Take(10))
            {
                FloatRegisterPressure.Add(reg);
            }
            
            PredicateRegisterPressure.Clear();
            foreach (var reg in data.RegisterPressure.PredicateRegisters.Take(10))
            {
                PredicateRegisterPressure.Add(reg);
            }
            
            BranchRegisterPressure.Clear();
            foreach (var reg in data.RegisterPressure.BranchRegisters)
            {
                BranchRegisterPressure.Add(reg);
            }
            
            OnPropertyChanged(nameof(MemoryBreakdown));
        }
        
        #endregion
        
        #region Command Implementations
        
        private bool CanExport() => HasData && !string.IsNullOrEmpty(_currentVMId);
        private bool CanStartMonitoring() => !IsMonitoring && !string.IsNullOrEmpty(_currentVMId);
        private bool CanStopMonitoring() => IsMonitoring;
        
        private void OnStartMonitoring()
        {
            if (!string.IsNullOrEmpty(_currentVMId))
            {
                IsMonitoring = true;
                _updateTimer.Start();
                StatusMessage = "Monitoring active - updating every 500ms";
            }
        }
        
        private void OnStopMonitoring()
        {
            StopMonitoring();
        }
        
        private void OnExportJson()
        {
            if (_profilerData == null || string.IsNullOrEmpty(_currentVMId))
                return;
                
            try
            {
                // Call VMManager to export JSON
                string json = VMManagerWrapper.Instance.ExportProfilerJson(_currentVMId);
                
                // Save to file
                var dialog = new Microsoft.Win32.SaveFileDialog
                {
                    FileName = $"profiler_{_currentVMId}_{DateTime.Now:yyyyMMdd_HHmmss}.json",
                    DefaultExt = ".json",
                    Filter = "JSON files (*.json)|*.json|All files (*.*)|*.*"
                };
                
                if (dialog.ShowDialog() == true)
                {
                    File.WriteAllText(dialog.FileName, json);
                    StatusMessage = $"Exported JSON report to {Path.GetFileName(dialog.FileName)}";
                    MessageBox.Show($"Profiler data exported successfully to:\n{dialog.FileName}", 
                        "Export Complete", MessageBoxButton.OK, MessageBoxImage.Information);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error exporting JSON: {ex.Message}";
                MessageBox.Show($"Failed to export JSON report:\n{ex.Message}", 
                    "Export Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        
        private void OnExportDot()
        {
            if (_profilerData == null || string.IsNullOrEmpty(_currentVMId))
                return;
                
            try
            {
                // Call VMManager to export DOT graph
                string dot = VMManagerWrapper.Instance.ExportProfilerDot(_currentVMId);
                
                // Save to file
                var dialog = new Microsoft.Win32.SaveFileDialog
                {
                    FileName = $"cfg_{_currentVMId}_{DateTime.Now:yyyyMMdd_HHmmss}.dot",
                    DefaultExt = ".dot",
                    Filter = "DOT files (*.dot)|*.dot|All files (*.*)|*.*"
                };
                
                if (dialog.ShowDialog() == true)
                {
                    File.WriteAllText(dialog.FileName, dot);
                    StatusMessage = $"Exported DOT graph to {Path.GetFileName(dialog.FileName)}";
                    MessageBox.Show($"Control flow graph exported successfully to:\n{dialog.FileName}\n\nVisualize with Graphviz.", 
                        "Export Complete", MessageBoxButton.OK, MessageBoxImage.Information);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error exporting DOT: {ex.Message}";
                MessageBox.Show($"Failed to export DOT graph:\n{ex.Message}", 
                    "Export Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        
        private void OnExportCsv()
        {
            if (_profilerData == null || string.IsNullOrEmpty(_currentVMId))
                return;
                
            try
            {
                // Call VMManager to export CSV
                string csv = VMManagerWrapper.Instance.ExportProfilerCsv(_currentVMId);
                
                // Save to file
                var dialog = new Microsoft.Win32.SaveFileDialog
                {
                    FileName = $"profiler_{_currentVMId}_{DateTime.Now:yyyyMMdd_HHmmss}.csv",
                    DefaultExt = ".csv",
                    Filter = "CSV files (*.csv)|*.csv|All files (*.*)|*.*"
                };
                
                if (dialog.ShowDialog() == true)
                {
                    File.WriteAllText(dialog.FileName, csv);
                    StatusMessage = $"Exported CSV stats to {Path.GetFileName(dialog.FileName)}";
                    MessageBox.Show($"Profiler statistics exported successfully to:\n{dialog.FileName}", 
                        "Export Complete", MessageBoxButton.OK, MessageBoxImage.Information);
                }
            }
            catch (Exception ex)
            {
                StatusMessage = $"Error exporting CSV: {ex.Message}";
                MessageBox.Show($"Failed to export CSV stats:\n{ex.Message}", 
                    "Export Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }
        
        #endregion
    }
}

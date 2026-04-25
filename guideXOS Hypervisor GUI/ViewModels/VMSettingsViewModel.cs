using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using guideXOS_Hypervisor_GUI.Models;
using Microsoft.Win32;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for VM Settings dialog
    /// </summary>
    public class VMSettingsViewModel : ViewModelBase
    {
        private VMSettingsModel _settings;
        private bool _hasChanges;
        private string _selectedCategory = "General";

        public VMSettingsViewModel(VMStateModel vmState)
        {
            _settings = VMSettingsModel.FromVMState(vmState);
            OriginalState = vmState;

            // Initialize commands
            SaveCommand = new RelayCommand(OnSave, CanSave);
            CancelCommand = new RelayCommand(OnCancel);
            AddDiskCommand = new RelayCommand(OnAddDisk);
            AddCDRomCommand = new RelayCommand(OnAddCDRom);
            RemoveDiskCommand = new RelayCommand<DiskImageInfo>(OnRemoveDisk, CanRemoveDisk);
            BrowseDiskCommand = new RelayCommand<DiskImageInfo>(OnBrowseDisk);
            GenerateMacCommand = new RelayCommand(OnGenerateMac);
            BrowseBootISOCommand = new RelayCommand(OnBrowseBootISO);
            ClearBootISOCommand = new RelayCommand(OnClearBootISO, CanClearBootISO);
        }

        #region Properties

        public VMStateModel OriginalState { get; }

        public string SelectedCategory
        {
            get => _selectedCategory;
            set => SetProperty(ref _selectedCategory, value);
        }

        public bool HasChanges
        {
            get => _hasChanges;
            set => SetProperty(ref _hasChanges, value);
        }

        // General Settings
        public string Name
        {
            get => _settings.Name;
            set
            {
                if (_settings.Name != value)
                {
                    _settings.Name = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public string Description
        {
            get => _settings.Description;
            set
            {
                if (_settings.Description != value)
                {
                    _settings.Description = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public string OperatingSystem
        {
            get => _settings.OperatingSystem;
            set
            {
                if (_settings.OperatingSystem != value)
                {
                    _settings.OperatingSystem = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        // System Settings
        public int CpuCount
        {
            get => _settings.CpuCount;
            set
            {
                if (_settings.CpuCount != value)
                {
                    _settings.CpuCount = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public ulong MemoryMB
        {
            get => _settings.MemoryMB;
            set
            {
                if (_settings.MemoryMB != value)
                {
                    _settings.MemoryMB = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(MemoryDisplay));
                    HasChanges = true;
                }
            }
        }

        public string MemoryDisplay => $"{MemoryMB} MB";

        public bool MMUEnabled
        {
            get => _settings.MMUEnabled;
            set
            {
                if (_settings.MMUEnabled != value)
                {
                    _settings.MMUEnabled = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public ulong PageSizeKB
        {
            get => _settings.PageSizeKB;
            set
            {
                if (_settings.PageSizeKB != value)
                {
                    _settings.PageSizeKB = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(PageSizeDisplay));
                    HasChanges = true;
                }
            }
        }

        public string PageSizeDisplay => $"{PageSizeKB} KB";

        // Boot Settings
        public string BootFromISO
        {
            get => _settings.BootFromISO;
            set
            {
                if (_settings.BootFromISO != value)
                {
                    _settings.BootFromISO = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(BootFromISODisplay));
                    HasChanges = true;
                }
            }
        }

        public string BootFromISODisplay => string.IsNullOrEmpty(BootFromISO) 
            ? "No ISO selected" 
            : System.IO.Path.GetFileName(BootFromISO);

        // Display Settings
        public int VideoMemoryMB
        {
            get => _settings.VideoMemoryMB;
            set
            {
                if (_settings.VideoMemoryMB != value)
                {
                    _settings.VideoMemoryMB = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public bool Enable3DAcceleration
        {
            get => _settings.Enable3DAcceleration;
            set
            {
                if (_settings.Enable3DAcceleration != value)
                {
                    _settings.Enable3DAcceleration = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        // Storage Settings
        public ObservableCollection<DiskImageInfo> DiskImages => _settings.DiskImages;

        // Network Settings
        public bool NetworkEnabled
        {
            get => _settings.NetworkEnabled;
            set
            {
                if (_settings.NetworkEnabled != value)
                {
                    _settings.NetworkEnabled = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public string NetworkAdapter
        {
            get => _settings.NetworkAdapter;
            set
            {
                if (_settings.NetworkAdapter != value)
                {
                    _settings.NetworkAdapter = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public string NetworkMode
        {
            get => _settings.NetworkMode;
            set
            {
                if (_settings.NetworkMode != value)
                {
                    _settings.NetworkMode = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public string MacAddress
        {
            get => _settings.MacAddress;
            set
            {
                if (_settings.MacAddress != value)
                {
                    _settings.MacAddress = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        // Advanced Settings
        public bool EnableProfiler
        {
            get => _settings.EnableProfiler;
            set
            {
                if (_settings.EnableProfiler != value)
                {
                    _settings.EnableProfiler = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public bool EnableDebugger
        {
            get => _settings.EnableDebugger;
            set
            {
                if (_settings.EnableDebugger != value)
                {
                    _settings.EnableDebugger = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public bool EnableSnapshot
        {
            get => _settings.EnableSnapshot;
            set
            {
                if (_settings.EnableSnapshot != value)
                {
                    _settings.EnableSnapshot = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public bool PauseOnStart
        {
            get => _settings.PauseOnStart;
            set
            {
                if (_settings.PauseOnStart != value)
                {
                    _settings.PauseOnStart = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        // Performance Settings
        public bool EnableCPUHotplug
        {
            get => _settings.EnableCPUHotplug;
            set
            {
                if (_settings.EnableCPUHotplug != value)
                {
                    _settings.EnableCPUHotplug = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public int ExecutionCap
        {
            get => _settings.ExecutionCap;
            set
            {
                if (_settings.ExecutionCap != value)
                {
                    _settings.ExecutionCap = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public bool EnablePAE
        {
            get => _settings.EnablePAE;
            set
            {
                if (_settings.EnablePAE != value)
                {
                    _settings.EnablePAE = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        public bool EnableNestedVT
        {
            get => _settings.EnableNestedVT;
            set
            {
                if (_settings.EnableNestedVT != value)
                {
                    _settings.EnableNestedVT = value;
                    OnPropertyChanged();
                    HasChanges = true;
                }
            }
        }

        #endregion

        #region Commands

        public ICommand SaveCommand { get; }
        public ICommand CancelCommand { get; }
        public ICommand AddDiskCommand { get; }
        public ICommand AddCDRomCommand { get; }
        public ICommand RemoveDiskCommand { get; }
        public ICommand BrowseDiskCommand { get; }
        public ICommand GenerateMacCommand { get; }
        public ICommand BrowseBootISOCommand { get; }
        public ICommand ClearBootISOCommand { get; }

        public event EventHandler? SaveRequested;
        public event EventHandler? CancelRequested;

        #endregion

        #region Command Implementations

        private bool CanSave() => HasChanges && !string.IsNullOrWhiteSpace(Name);

        private void OnSave()
        {
            try
            {
                // Apply settings to the original VM state
                _settings.ApplyToVMState(OriginalState);
                
                // Notify that save was requested
                SaveRequested?.Invoke(this, EventArgs.Empty);
            }
            catch (Exception ex)
            {
                MessageBox.Show($"Failed to save settings:\n{ex.Message}", 
                    "Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnCancel()
        {
            if (HasChanges)
            {
                var result = MessageBox.Show(
                    "You have unsaved changes. Are you sure you want to cancel?",
                    "Confirm Cancel",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Question);

                if (result != MessageBoxResult.Yes)
                    return;
            }

            CancelRequested?.Invoke(this, EventArgs.Empty);
        }

        private void OnAddDisk()
        {
            var openDialog = new OpenFileDialog
            {
                Title = "Select Disk Image",
                Filter = "Disk Images (*.img;*.vdi;*.vmdk;*.vhd)|*.img;*.vdi;*.vmdk;*.vhd|All Files (*.*)|*.*",
                CheckFileExists = true
            };

            if (openDialog.ShowDialog() == true)
            {
                var newDisk = new DiskImageInfo
                {
                    Path = openDialog.FileName,
                    Type = "HDD",
                    ReadOnly = false,
                    SizeMB = 0, // TODO: Get actual file size
                    Controller = "SATA",
                    Port = DiskImages.Count(d => d.Type == "HDD")
                };

                DiskImages.Add(newDisk);
                HasChanges = true;
            }
        }

        private void OnAddCDRom()
        {
            var openDialog = new OpenFileDialog
            {
                Title = "Select ISO Image",
                Filter = "ISO Images (*.iso)|*.iso|All Files (*.*)|*.*",
                CheckFileExists = true
            };

            if (openDialog.ShowDialog() == true)
            {
                var newDisk = new DiskImageInfo
                {
                    Path = openDialog.FileName,
                    Type = "CD-ROM",
                    ReadOnly = true,
                    SizeMB = 0, // ISO size
                    Controller = "IDE",
                    Port = DiskImages.Count(d => d.Type == "CD-ROM")
                };

                DiskImages.Add(newDisk);
                HasChanges = true;
            }
        }

        private bool CanRemoveDisk(DiskImageInfo? disk) => disk != null;

        private void OnRemoveDisk(DiskImageInfo? disk)
        {
            if (disk == null) return;

            var result = MessageBox.Show(
                $"Remove disk image:\n{disk.Path}?",
                "Confirm Removal",
                MessageBoxButton.YesNo,
                MessageBoxImage.Question);

            if (result == MessageBoxResult.Yes)
            {
                DiskImages.Remove(disk);
                HasChanges = true;

                // Update port numbers
                for (int i = 0; i < DiskImages.Count; i++)
                {
                    DiskImages[i].Port = i;
                }
            }
        }

        private void OnBrowseDisk(DiskImageInfo? disk)
        {
            if (disk == null) return;

            var filter = disk.Type == "CD-ROM" 
                ? "ISO Images (*.iso)|*.iso|All Files (*.*)|*.*"
                : "Disk Images (*.img;*.vdi;*.vmdk;*.vhd)|*.img;*.vdi;*.vmdk;*.vhd|All Files (*.*)|*.*";

            var openDialog = new OpenFileDialog
            {
                Title = disk.Type == "CD-ROM" ? "Select ISO Image" : "Select Disk Image",
                Filter = filter,
                CheckFileExists = true,
                FileName = disk.Path
            };

            if (openDialog.ShowDialog() == true)
            {
                disk.Path = openDialog.FileName;
                HasChanges = true;
                OnPropertyChanged(nameof(DiskImages));
            }
        }

        private void OnGenerateMac()
        {
            // Generate a random MAC address
            var random = new Random();
            var mac = $"52:54:00:{random.Next(0, 256):X2}:{random.Next(0, 256):X2}:{random.Next(0, 256):X2}";
            MacAddress = mac;
        }

        private void OnBrowseBootISO()
        {
            var openDialog = new OpenFileDialog
            {
                Title = "Select Boot ISO Image",
                Filter = "ISO Images (*.iso)|*.iso|All Files (*.*)|*.*",
                CheckFileExists = true,
                FileName = BootFromISO
            };

            if (openDialog.ShowDialog() == true)
            {
                BootFromISO = openDialog.FileName;
            }
        }

        private bool CanClearBootISO() => !string.IsNullOrEmpty(BootFromISO);

        private void OnClearBootISO()
        {
            BootFromISO = string.Empty;
        }

        #endregion

        /// <summary>
        /// Get the settings model
        /// </summary>
        public VMSettingsModel GetSettings() => _settings;
    }
}

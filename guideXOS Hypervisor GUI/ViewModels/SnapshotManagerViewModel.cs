using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.Services;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for the VirtualBox-style Snapshot Manager
    /// </summary>
    public class SnapshotManagerViewModel : ViewModelBase
    {
        private string _vmId = string.Empty;
        private string _vmName = string.Empty;
        private VMSnapshot? _selectedSnapshot;
        private string _newSnapshotName = string.Empty;
        private string _newSnapshotDescription = string.Empty;
        private string _statusMessage = "Ready";
        private bool _showCreateDialog;

        public SnapshotManagerViewModel()
        {
            SnapshotTree = new SnapshotTree();
            
            // Initialize root collection for TreeView binding
            SnapshotTreeRoot = new ObservableCollection<VMSnapshot>();
            
            // Commands
            TakeSnapshotCommand = new RelayCommand(OnTakeSnapshot, CanTakeSnapshot);
            RestoreSnapshotCommand = new RelayCommand(OnRestoreSnapshot, CanRestoreSnapshot);
            DeleteSnapshotCommand = new RelayCommand(OnDeleteSnapshot, CanDeleteSnapshot);
            DeleteSnapshotAndChildrenCommand = new RelayCommand(OnDeleteSnapshotAndChildren, CanDeleteSnapshot);
            ShowSnapshotDetailsCommand = new RelayCommand(OnShowSnapshotDetails, CanShowSnapshotDetails);
            CloneSnapshotCommand = new RelayCommand(OnCloneSnapshot, CanRestoreSnapshot);
            RefreshCommand = new RelayCommand(OnRefresh);
            CreateSnapshotDialogCommand = new RelayCommand(OnShowCreateDialog, CanTakeSnapshot);
            ConfirmCreateSnapshotCommand = new RelayCommand(OnConfirmCreateSnapshot, CanConfirmCreateSnapshot);
            CancelCreateSnapshotCommand = new RelayCommand(OnCancelCreateSnapshot);
        }

        #region Properties

        public string VMId
        {
            get => _vmId;
            set => SetProperty(ref _vmId, value);
        }

        public string VMName
        {
            get => _vmName;
            set => SetProperty(ref _vmName, value);
        }

        public SnapshotTree SnapshotTree { get; private set; }

        /// <summary>
        /// Root collection for TreeView binding
        /// </summary>
        public ObservableCollection<VMSnapshot> SnapshotTreeRoot { get; }

        public VMSnapshot? SelectedSnapshot
        {
            get => _selectedSnapshot;
            set
            {
                if (SetProperty(ref _selectedSnapshot, value))
                {
                    CommandManager.InvalidateRequerySuggested();
                    OnPropertyChanged(nameof(SelectedSnapshotDetails));
                    OnPropertyChanged(nameof(CanRestore));
                    OnPropertyChanged(nameof(CanDelete));
                }
            }
        }

        public string NewSnapshotName
        {
            get => _newSnapshotName;
            set => SetProperty(ref _newSnapshotName, value);
        }

        public string NewSnapshotDescription
        {
            get => _newSnapshotDescription;
            set => SetProperty(ref _newSnapshotDescription, value);
        }

        public string StatusMessage
        {
            get => _statusMessage;
            set => SetProperty(ref _statusMessage, value);
        }

        public bool ShowCreateDialog
        {
            get => _showCreateDialog;
            set => SetProperty(ref _showCreateDialog, value);
        }

        // Computed properties
        public string SelectedSnapshotDetails
        {
            get
            {
                if (SelectedSnapshot == null)
                    return "No snapshot selected";

                if (SelectedSnapshot.IsCurrentState)
                    return "Current State - This is the live running state of the VM";

                var details = $"Name: {SelectedSnapshot.Name}\n";
                details += $"Created: {SelectedSnapshot.FormattedDate}\n";
                details += $"Size: {SelectedSnapshot.FormattedSize}\n";
                
                if (!string.IsNullOrEmpty(SelectedSnapshot.Description))
                    details += $"Description: {SelectedSnapshot.Description}\n";

                if (SelectedSnapshot.State != null)
                    details += $"VM State: {SelectedSnapshot.State.Summary}";

                return details;
            }
        }

        public bool CanRestore => SelectedSnapshot != null && !SelectedSnapshot.IsCurrentState;
        public bool CanDelete => SelectedSnapshot != null && !SelectedSnapshot.IsCurrentState;

        public int TotalSnapshots => SnapshotTree.TotalSnapshots;
        public string TotalSize => FormatSize(SnapshotTree.TotalSizeMB);

        #endregion

        #region Commands

        public ICommand TakeSnapshotCommand { get; }
        public ICommand RestoreSnapshotCommand { get; }
        public ICommand DeleteSnapshotCommand { get; }
        public ICommand DeleteSnapshotAndChildrenCommand { get; }
        public ICommand ShowSnapshotDetailsCommand { get; }
        public ICommand CloneSnapshotCommand { get; }
        public ICommand RefreshCommand { get; }
        public ICommand CreateSnapshotDialogCommand { get; }
        public ICommand ConfirmCreateSnapshotCommand { get; }
        public ICommand CancelCreateSnapshotCommand { get; }

        #endregion

        #region Command Handlers

        private bool CanTakeSnapshot() => !string.IsNullOrEmpty(VMId);
        private bool CanRestoreSnapshot() => SelectedSnapshot != null && !SelectedSnapshot.IsCurrentState;
        private bool CanDeleteSnapshot() => SelectedSnapshot != null && !SelectedSnapshot.IsCurrentState;
        private bool CanShowSnapshotDetails() => SelectedSnapshot != null;
        private bool CanConfirmCreateSnapshot() => !string.IsNullOrWhiteSpace(NewSnapshotName);

        private void OnShowCreateDialog()
        {
            NewSnapshotName = $"Snapshot {DateTime.Now:yyyy-MM-dd HH:mm:ss}";
            NewSnapshotDescription = string.Empty;
            ShowCreateDialog = true;
        }

        private void OnCancelCreateSnapshot()
        {
            ShowCreateDialog = false;
            NewSnapshotName = string.Empty;
            NewSnapshotDescription = string.Empty;
        }

        private void OnConfirmCreateSnapshot()
        {
            OnTakeSnapshot();
            ShowCreateDialog = false;
        }

        private void OnTakeSnapshot()
        {
            if (string.IsNullOrWhiteSpace(NewSnapshotName))
            {
                NewSnapshotName = $"Snapshot {DateTime.Now:yyyy-MM-dd HH:mm:ss}";
            }

            try
            {
                StatusMessage = $"Creating snapshot '{NewSnapshotName}'...";

                // Call backend to create snapshot
                var snapshotId = VMManagerWrapper.Instance.CreateSnapshotEx(VMId, NewSnapshotName, NewSnapshotDescription);

                // Create snapshot model
                var snapshot = new VMSnapshot
                {
                    Id = snapshotId,
                    Name = NewSnapshotName,
                    Description = NewSnapshotDescription,
                    CreatedDate = DateTime.Now,
                    SizeMB = (ulong)new Random().Next(50, 500) // TODO: Get real size from backend
                };

                // Get current VM state and save it
                var vmState = VMManagerWrapper.Instance.GetVMState(VMId);
                if (vmState != null)
                {
                    snapshot.State = new VMSnapshotState
                    {
                        CpuCount = vmState.CpuCount,
                        MemoryMB = vmState.MemoryMB,
                        InstructionPointer = vmState.CurrentIP,
                        State = vmState.State,
                        TotalCycles = vmState.TotalCycles,
                        DiskImages = vmState.DiskImages
                    };
                }

                // Add to tree
                SnapshotTree.AddSnapshot(snapshot);
                
                // Refresh the tree view
                RefreshTreeView();

                StatusMessage = $"Snapshot '{NewSnapshotName}' created successfully";
                OnPropertyChanged(nameof(TotalSnapshots));
                OnPropertyChanged(nameof(TotalSize));

                // Clear the form
                NewSnapshotName = string.Empty;
                NewSnapshotDescription = string.Empty;
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to create snapshot: {ex.Message}";
                MessageBox.Show($"Failed to create snapshot:\n{ex.Message}", 
                    "Snapshot Error", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void OnRestoreSnapshot()
        {
            if (SelectedSnapshot == null || SelectedSnapshot.IsCurrentState)
                return;

            var result = MessageBox.Show(
                $"Are you sure you want to restore the snapshot '{SelectedSnapshot.Name}'?\n\n" +
                "The current state will be lost unless you create a snapshot first.",
                "Confirm Restore Snapshot",
                MessageBoxButton.YesNo,
                MessageBoxImage.Warning);

            if (result == MessageBoxResult.Yes)
            {
                try
                {
                    StatusMessage = $"Restoring snapshot '{SelectedSnapshot.Name}'...";

                    // Call backend to restore snapshot
                    VMManagerWrapper.Instance.RestoreSnapshot(VMId, SelectedSnapshot.Id);

                    // Update active snapshot
                    SnapshotTree.ActiveSnapshot = SelectedSnapshot;

                    StatusMessage = $"Snapshot '{SelectedSnapshot.Name}' restored successfully";
                    
                    MessageBox.Show($"Snapshot '{SelectedSnapshot.Name}' has been restored.", 
                        "Snapshot Restored", MessageBoxButton.OK, MessageBoxImage.Information);
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to restore snapshot: {ex.Message}";
                    MessageBox.Show($"Failed to restore snapshot:\n{ex.Message}", 
                        "Snapshot Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        private void OnDeleteSnapshot()
        {
            if (SelectedSnapshot == null || SelectedSnapshot.IsCurrentState)
                return;

            string message;
            if (SelectedSnapshot.HasChildren)
            {
                message = $"Delete snapshot '{SelectedSnapshot.Name}'?\n\n" +
                         "This snapshot has children. The children will be moved to the parent snapshot.\n\n" +
                         "To delete this snapshot and all its children, use 'Delete Snapshot and Children'.";
            }
            else
            {
                message = $"Are you sure you want to delete the snapshot '{SelectedSnapshot.Name}'?";
            }

            var result = MessageBox.Show(message, "Confirm Delete Snapshot",
                MessageBoxButton.YesNo, MessageBoxImage.Warning);

            if (result == MessageBoxResult.Yes)
            {
                try
                {
                    StatusMessage = $"Deleting snapshot '{SelectedSnapshot.Name}'...";

                    // Call backend to delete snapshot (keeping children)
                    VMManagerWrapper.Instance.DeleteSnapshot(VMId, SelectedSnapshot.Id, false);

                    var snapshotName = SelectedSnapshot.Name;
                    
                    // Remove from tree (keeping children)
                    SnapshotTree.RemoveSnapshot(SelectedSnapshot, false);
                    
                    // Refresh the tree view
                    RefreshTreeView();

                    SelectedSnapshot = null;
                    StatusMessage = $"Snapshot '{snapshotName}' deleted successfully";
                    OnPropertyChanged(nameof(TotalSnapshots));
                    OnPropertyChanged(nameof(TotalSize));
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to delete snapshot: {ex.Message}";
                    MessageBox.Show($"Failed to delete snapshot:\n{ex.Message}", 
                        "Snapshot Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        private void OnDeleteSnapshotAndChildren()
        {
            if (SelectedSnapshot == null || SelectedSnapshot.IsCurrentState)
                return;

            var childCount = CountDescendants(SelectedSnapshot);
            var message = childCount > 0
                ? $"Delete snapshot '{SelectedSnapshot.Name}' and {childCount} child snapshot(s)?\n\nThis action cannot be undone."
                : $"Are you sure you want to delete the snapshot '{SelectedSnapshot.Name}'?";

            var result = MessageBox.Show(message, "Confirm Delete Snapshot and Children",
                MessageBoxButton.YesNo, MessageBoxImage.Warning);

            if (result == MessageBoxResult.Yes)
            {
                try
                {
                    StatusMessage = $"Deleting snapshot '{SelectedSnapshot.Name}' and children...";

                    // Call backend to delete snapshot and children
                    VMManagerWrapper.Instance.DeleteSnapshot(VMId, SelectedSnapshot.Id, true);

                    var snapshotName = SelectedSnapshot.Name;
                    
                    // Remove from tree (including children)
                    SnapshotTree.RemoveSnapshot(SelectedSnapshot, true);
                    
                    // Refresh the tree view
                    RefreshTreeView();

                    SelectedSnapshot = null;
                    StatusMessage = $"Snapshot '{snapshotName}' and children deleted successfully";
                    OnPropertyChanged(nameof(TotalSnapshots));
                    OnPropertyChanged(nameof(TotalSize));
                }
                catch (Exception ex)
                {
                    StatusMessage = $"Failed to delete snapshot: {ex.Message}";
                    MessageBox.Show($"Failed to delete snapshot:\n{ex.Message}", 
                        "Snapshot Error", MessageBoxButton.OK, MessageBoxImage.Error);
                }
            }
        }

        private void OnShowSnapshotDetails()
        {
            if (SelectedSnapshot == null)
                return;

            MessageBox.Show(SelectedSnapshotDetails, 
                $"Snapshot Details - {SelectedSnapshot.DisplayName}", 
                MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void OnCloneSnapshot()
        {
            if (SelectedSnapshot == null || SelectedSnapshot.IsCurrentState)
                return;

            // TODO: Implement VM cloning from snapshot
            MessageBox.Show($"Clone VM from snapshot '{SelectedSnapshot.Name}' - Not yet implemented", 
                "Clone VM", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        private void OnRefresh()
        {
            LoadSnapshots();
        }

        #endregion

        #region Helper Methods

        /// <summary>
        /// Load snapshot tree for a specific VM
        /// </summary>
        public void LoadSnapshotsForVM(string vmId, string vmName)
        {
            VMId = vmId;
            VMName = vmName;
            LoadSnapshots();
        }

        private void LoadSnapshots()
        {
            try
            {
                StatusMessage = "Loading snapshots...";

                // Get snapshots from backend
                var snapshots = VMManagerWrapper.Instance.GetSnapshots(VMId);

                // Rebuild tree
                SnapshotTree = new SnapshotTree();
                
                foreach (var snapshotData in snapshots)
                {
                    var snapshot = new VMSnapshot
                    {
                        Id = snapshotData.Id,
                        Name = snapshotData.Name,
                        Description = snapshotData.Description,
                        CreatedDate = snapshotData.CreatedDate,
                        SizeMB = snapshotData.SizeMB,
                        State = snapshotData.State
                    };

                    SnapshotTree.AddSnapshot(snapshot);
                }

                RefreshTreeView();

                StatusMessage = $"Loaded {SnapshotTree.TotalSnapshots} snapshot(s)";
                OnPropertyChanged(nameof(TotalSnapshots));
                OnPropertyChanged(nameof(TotalSize));
            }
            catch (Exception ex)
            {
                StatusMessage = $"Failed to load snapshots: {ex.Message}";
            }
        }

        private void RefreshTreeView()
        {
            SnapshotTreeRoot.Clear();
            
            // Add current state and its children to root
            SnapshotTreeRoot.Add(SnapshotTree.Root);
        }

        private int CountDescendants(VMSnapshot snapshot)
        {
            int count = snapshot.Children.Count;
            foreach (var child in snapshot.Children)
            {
                count += CountDescendants(child);
            }
            return count;
        }

        private string FormatSize(ulong sizeMB)
        {
            if (sizeMB < 1024)
                return $"{sizeMB} MB";
            else
                return $"{sizeMB / 1024.0:F2} GB";
        }

        #endregion
    }
}

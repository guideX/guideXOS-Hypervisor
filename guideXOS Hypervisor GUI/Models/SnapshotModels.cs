using System;
using System.Collections.ObjectModel;
using System.ComponentModel;

namespace guideXOS_Hypervisor_GUI.Models
{
    /// <summary>
    /// Represents a VM snapshot in the snapshot tree
    /// </summary>
    public class VMSnapshot : INotifyPropertyChanged
    {
        private string _name = string.Empty;
        private string _description = string.Empty;
        private bool _isCurrentState;
        private bool _isSelected;

        /// <summary>
        /// Unique identifier for the snapshot
        /// </summary>
        public string Id { get; set; } = Guid.NewGuid().ToString();

        /// <summary>
        /// Snapshot name
        /// </summary>
        public string Name
        {
            get => _name;
            set
            {
                if (_name != value)
                {
                    _name = value;
                    OnPropertyChanged(nameof(Name));
                }
            }
        }

        /// <summary>
        /// User-provided description
        /// </summary>
        public string Description
        {
            get => _description;
            set
            {
                if (_description != value)
                {
                    _description = value;
                    OnPropertyChanged(nameof(Description));
                }
            }
        }

        /// <summary>
        /// Timestamp when snapshot was created
        /// </summary>
        public DateTime CreatedDate { get; set; }

        /// <summary>
        /// Formatted timestamp for display
        /// </summary>
        public string FormattedDate => CreatedDate.ToString("yyyy-MM-dd HH:mm:ss");

        /// <summary>
        /// Whether this represents the current running state (not an actual snapshot)
        /// </summary>
        public bool IsCurrentState
        {
            get => _isCurrentState;
            set
            {
                if (_isCurrentState != value)
                {
                    _isCurrentState = value;
                    OnPropertyChanged(nameof(IsCurrentState));
                    OnPropertyChanged(nameof(DisplayName));
                    OnPropertyChanged(nameof(Icon));
                }
            }
        }

        /// <summary>
        /// Whether this snapshot is currently selected in the UI
        /// </summary>
        public bool IsSelected
        {
            get => _isSelected;
            set
            {
                if (_isSelected != value)
                {
                    _isSelected = value;
                    OnPropertyChanged(nameof(IsSelected));
                }
            }
        }

        /// <summary>
        /// Display name with special formatting for current state
        /// </summary>
        public string DisplayName => IsCurrentState ? "Current State" : Name;

        /// <summary>
        /// Icon to display for the snapshot
        /// </summary>
        public string Icon => IsCurrentState ? "?" : "??";

        /// <summary>
        /// Parent snapshot (null for root snapshots)
        /// </summary>
        public VMSnapshot? Parent { get; set; }

        /// <summary>
        /// Child snapshots
        /// </summary>
        public ObservableCollection<VMSnapshot> Children { get; set; } = new ObservableCollection<VMSnapshot>();

        /// <summary>
        /// Saved VM state at the time of snapshot
        /// </summary>
        public VMSnapshotState? State { get; set; }

        /// <summary>
        /// Size of snapshot data in MB
        /// </summary>
        public ulong SizeMB { get; set; }

        /// <summary>
        /// Formatted size for display
        /// </summary>
        public string FormattedSize
        {
            get
            {
                if (SizeMB < 1024)
                    return $"{SizeMB} MB";
                else
                    return $"{SizeMB / 1024.0:F2} GB";
            }
        }

        /// <summary>
        /// Whether this snapshot has children
        /// </summary>
        public bool HasChildren => Children.Count > 0;

        /// <summary>
        /// Path from root to this snapshot
        /// </summary>
        public string Path
        {
            get
            {
                if (Parent == null)
                    return Name;
                return $"{Parent.Path} ? {Name}";
            }
        }

        public event PropertyChangedEventHandler? PropertyChanged;

        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }

        /// <summary>
        /// Add a child snapshot
        /// </summary>
        public void AddChild(VMSnapshot child)
        {
            child.Parent = this;
            Children.Add(child);
            OnPropertyChanged(nameof(HasChildren));
        }

        /// <summary>
        /// Remove a child snapshot
        /// </summary>
        public void RemoveChild(VMSnapshot child)
        {
            child.Parent = null;
            Children.Remove(child);
            OnPropertyChanged(nameof(HasChildren));
        }
    }

    /// <summary>
    /// VM state saved in a snapshot
    /// </summary>
    public class VMSnapshotState
    {
        /// <summary>
        /// CPU count at snapshot time
        /// </summary>
        public int CpuCount { get; set; }

        /// <summary>
        /// Memory size in MB at snapshot time
        /// </summary>
        public ulong MemoryMB { get; set; }

        /// <summary>
        /// Instruction pointer at snapshot time
        /// </summary>
        public ulong InstructionPointer { get; set; }

        /// <summary>
        /// VM state (Running, Paused, etc.)
        /// </summary>
        public VMState State { get; set; }

        /// <summary>
        /// Total execution cycles at snapshot time
        /// </summary>
        public ulong TotalCycles { get; set; }

        /// <summary>
        /// Disk images attached at snapshot time
        /// </summary>
        public List<DiskImageInfo> DiskImages { get; set; } = new List<DiskImageInfo>();

        /// <summary>
        /// Summary information for display
        /// </summary>
        public string Summary => $"{CpuCount} CPU, {MemoryMB} MB RAM, {State}";
    }

    /// <summary>
    /// Snapshot tree structure for a VM
    /// </summary>
    public class SnapshotTree
    {
        /// <summary>
        /// Root snapshot (represents the initial state)
        /// </summary>
        public VMSnapshot Root { get; set; }

        /// <summary>
        /// Current state node (always present, represents running state)
        /// </summary>
        public VMSnapshot CurrentState { get; set; }

        /// <summary>
        /// Currently active snapshot (the one we're running from)
        /// </summary>
        public VMSnapshot? ActiveSnapshot { get; set; }

        /// <summary>
        /// Total number of snapshots (excluding current state)
        /// </summary>
        public int TotalSnapshots { get; private set; }

        /// <summary>
        /// Total size of all snapshots in MB
        /// </summary>
        public ulong TotalSizeMB { get; private set; }

        public SnapshotTree()
        {
            // Initialize with current state
            CurrentState = new VMSnapshot
            {
                Id = "current",
                Name = "Current State",
                IsCurrentState = true,
                CreatedDate = DateTime.Now
            };

            Root = CurrentState;
        }

        /// <summary>
        /// Add a snapshot as a child of the current state
        /// </summary>
        public void AddSnapshot(VMSnapshot snapshot)
        {
            // New snapshots are always children of current state
            var parent = ActiveSnapshot ?? Root;
            
            // If we're adding from current state, make it a child of the active snapshot
            if (!CurrentState.IsCurrentState || ActiveSnapshot != null)
            {
                parent.AddChild(snapshot);
            }
            else
            {
                Root.AddChild(snapshot);
            }

            TotalSnapshots++;
            TotalSizeMB += snapshot.SizeMB;
        }

        /// <summary>
        /// Remove a snapshot and optionally its children
        /// </summary>
        public void RemoveSnapshot(VMSnapshot snapshot, bool removeChildren)
        {
            if (snapshot.Parent == null)
                return;

            if (removeChildren)
            {
                // Remove all descendants
                RemoveSnapshotRecursive(snapshot);
            }
            else
            {
                // Move children to parent
                foreach (var child in snapshot.Children)
                {
                    child.Parent = snapshot.Parent;
                    snapshot.Parent.Children.Add(child);
                }
            }

            snapshot.Parent.RemoveChild(snapshot);
            TotalSnapshots--;
            TotalSizeMB -= snapshot.SizeMB;
        }

        private void RemoveSnapshotRecursive(VMSnapshot snapshot)
        {
            // Remove all children first
            while (snapshot.Children.Count > 0)
            {
                var child = snapshot.Children[0];
                RemoveSnapshotRecursive(child);
            }

            TotalSnapshots--;
            TotalSizeMB -= snapshot.SizeMB;
        }

        /// <summary>
        /// Find a snapshot by ID
        /// </summary>
        public VMSnapshot? FindSnapshot(string id)
        {
            return FindSnapshotRecursive(Root, id);
        }

        private VMSnapshot? FindSnapshotRecursive(VMSnapshot node, string id)
        {
            if (node.Id == id)
                return node;

            foreach (var child in node.Children)
            {
                var found = FindSnapshotRecursive(child, id);
                if (found != null)
                    return found;
            }

            return null;
        }

        /// <summary>
        /// Get all snapshots as a flat list
        /// </summary>
        public List<VMSnapshot> GetAllSnapshots()
        {
            var snapshots = new List<VMSnapshot>();
            GetAllSnapshotsRecursive(Root, snapshots);
            return snapshots;
        }

        private void GetAllSnapshotsRecursive(VMSnapshot node, List<VMSnapshot> snapshots)
        {
            if (!node.IsCurrentState)
                snapshots.Add(node);

            foreach (var child in node.Children)
            {
                GetAllSnapshotsRecursive(child, snapshots);
            }
        }
    }
}

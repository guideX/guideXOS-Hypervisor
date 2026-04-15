using System;
using guideXOS_Hypervisor_GUI.Models;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for individual VM list items in the sidebar
    /// </summary>
    public class VMListItemViewModel : ViewModelBase
    {
        private VMStateModel _vmState;
        private bool _isSelected;

        public VMListItemViewModel(VMStateModel vmState)
        {
            _vmState = vmState ?? throw new ArgumentNullException(nameof(vmState));
        }

        public string Id => _vmState.Id;
        
        public string Name
        {
            get => _vmState.Name;
            set
            {
                if (_vmState.Name != value)
                {
                    _vmState.Name = value;
                    OnPropertyChanged();
                }
            }
        }

        public VMState State
        {
            get => _vmState.State;
            set
            {
                if (_vmState.State != value)
                {
                    _vmState.State = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(StateDescription));
                    OnPropertyChanged(nameof(StateColor));
                    OnPropertyChanged(nameof(IsRunning));
                }
            }
        }

        public string StateDescription => _vmState.StateDescription;

        public string StateColor => State switch
        {
            VMState.Running => "#4CAF50",
            VMState.Paused => "#FF9800",
            VMState.PoweredOff => "#757575",
            VMState.Saved => "#2196F3",
            VMState.Aborted => "#F44336",
            _ => "#757575"
        };

        public bool IsRunning => State == VMState.Running;

        public bool IsSelected
        {
            get => _isSelected;
            set => SetProperty(ref _isSelected, value);
        }

        public int CpuCount
        {
            get => _vmState.CpuCount;
            set
            {
                if (_vmState.CpuCount != value)
                {
                    _vmState.CpuCount = value;
                    OnPropertyChanged();
                }
            }
        }

        public ulong MemoryMB
        {
            get => _vmState.MemoryMB;
            set
            {
                if (_vmState.MemoryMB != value)
                {
                    _vmState.MemoryMB = value;
                    OnPropertyChanged();
                    OnPropertyChanged(nameof(MemoryDisplay));
                }
            }
        }

        public string MemoryDisplay => $"{MemoryMB} MB";

        public string OperatingSystem
        {
            get => _vmState.OperatingSystem;
            set
            {
                if (_vmState.OperatingSystem != value)
                {
                    _vmState.OperatingSystem = value;
                    OnPropertyChanged();
                }
            }
        }

        public string Architecture
        {
            get => _vmState.Architecture;
            set
            {
                if (_vmState.Architecture != value)
                {
                    _vmState.Architecture = value;
                    OnPropertyChanged();
                }
            }
        }

        public VMStateModel GetModel() => _vmState;

        public void UpdateFromModel(VMStateModel model)
        {
            _vmState = model;
            OnPropertyChanged(nameof(Name));
            OnPropertyChanged(nameof(State));
            OnPropertyChanged(nameof(StateDescription));
            OnPropertyChanged(nameof(StateColor));
            OnPropertyChanged(nameof(IsRunning));
            OnPropertyChanged(nameof(CpuCount));
            OnPropertyChanged(nameof(MemoryMB));
            OnPropertyChanged(nameof(MemoryDisplay));
            OnPropertyChanged(nameof(OperatingSystem));
            OnPropertyChanged(nameof(Architecture));
        }
    }
}

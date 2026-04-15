using System.Windows;
using System.Windows.Controls;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.ViewModels;

namespace guideXOS_Hypervisor_GUI.Views
{
    /// <summary>
    /// Interaction logic for SnapshotManagerView.xaml
    /// </summary>
    public partial class SnapshotManagerView : Window
    {
        public SnapshotManagerView()
        {
            InitializeComponent();
            DataContext = new SnapshotManagerViewModel();
        }

        /// <summary>
        /// Constructor for opening snapshot manager for a specific VM
        /// </summary>
        public SnapshotManagerView(string vmId, string vmName) : this()
        {
            if (DataContext is SnapshotManagerViewModel viewModel)
            {
                viewModel.LoadSnapshotsForVM(vmId, vmName);
            }
        }

        /// <summary>
        /// Handle TreeView selection changed (workaround for binding limitations)
        /// </summary>
        private void TreeView_SelectedItemChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
        {
            if (DataContext is SnapshotManagerViewModel viewModel && e.NewValue is VMSnapshot snapshot)
            {
                viewModel.SelectedSnapshot = snapshot;
            }
        }
    }
}

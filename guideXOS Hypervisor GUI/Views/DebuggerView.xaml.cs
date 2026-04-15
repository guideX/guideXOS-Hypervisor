using System.Windows;
using guideXOS_Hypervisor_GUI.ViewModels;

namespace guideXOS_Hypervisor_GUI.Views
{
    /// <summary>
    /// Interaction logic for DebuggerView.xaml
    /// </summary>
    public partial class DebuggerView : Window
    {
        public DebuggerView()
        {
            InitializeComponent();
            DataContext = new DebuggerViewModel();
        }

        /// <summary>
        /// Constructor for attaching to a specific VM
        /// </summary>
        public DebuggerView(string vmId) : this()
        {
            if (DataContext is DebuggerViewModel viewModel)
            {
                viewModel.AttachToVM(vmId);
            }
        }
    }
}

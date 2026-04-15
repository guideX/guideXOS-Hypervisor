using System;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace guideXOS_Hypervisor_GUI.ViewModels
{
    /// <summary>
    /// ViewModel for VM Screen Display Window
    /// Manages framebuffer rendering and screen updates
    /// </summary>
    public class VMScreenViewModel : ViewModelBase
    {
        private readonly DispatcherTimer _renderTimer;
        private string _vmId;
        private string _vmName;
        private WriteableBitmap? _screenBitmap;
        private int _screenWidth = 640;
        private int _screenHeight = 480;
        private double _scale = 1.0;
        private bool _isRunning;

        public VMScreenViewModel(string vmId, string vmName)
        {
            _vmId = vmId;
            _vmName = vmName;
            
            // Initialize screen bitmap (640x480 default resolution)
            InitializeScreenBitmap();
            
            // Setup render timer (60 FPS)
            _renderTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(16.67) // ~60 FPS
            };
            _renderTimer.Tick += RenderTimer_Tick;
            
            // Commands
            CloseCommand = new RelayCommand(OnClose);
            ResetScaleCommand = new RelayCommand(OnResetScale);
            FullscreenCommand = new RelayCommand(OnFullscreen);
        }

        #region Properties

        public string VmId
        {
            get => _vmId;
            set => SetProperty(ref _vmId, value);
        }

        public string VmName
        {
            get => _vmName;
            set
            {
                if (SetProperty(ref _vmName, value))
                {
                    OnPropertyChanged(nameof(WindowTitle));
                }
            }
        }

        public string WindowTitle => $"{VmName} - VM Screen";

        public WriteableBitmap? ScreenBitmap
        {
            get => _screenBitmap;
            private set => SetProperty(ref _screenBitmap, value);
        }

        public int ScreenWidth
        {
            get => _screenWidth;
            set => SetProperty(ref _screenWidth, value);
        }

        public int ScreenHeight
        {
            get => _screenHeight;
            set => SetProperty(ref _screenHeight, value);
        }

        public double Scale
        {
            get => _scale;
            set => SetProperty(ref _scale, value);
        }

        public bool IsRunning
        {
            get => _isRunning;
            set
            {
                if (SetProperty(ref _isRunning, value))
                {
                    if (_isRunning)
                        StartRendering();
                    else
                        StopRendering();
                }
            }
        }

        #endregion

        #region Commands

        public ICommand CloseCommand { get; }
        public ICommand ResetScaleCommand { get; }
        public ICommand FullscreenCommand { get; }

        #endregion

        #region Public Methods

        /// <summary>
        /// Start rendering the VM screen
        /// </summary>
        public void StartRendering()
        {
            _renderTimer.Start();
        }

        /// <summary>
        /// Stop rendering the VM screen
        /// </summary>
        public void StopRendering()
        {
            _renderTimer.Stop();
        }

        /// <summary>
        /// Update screen resolution
        /// </summary>
        public void UpdateResolution(int width, int height)
        {
            if (width != _screenWidth || height != _screenHeight)
            {
                _screenWidth = width;
                _screenHeight = height;
                InitializeScreenBitmap();
                OnPropertyChanged(nameof(ScreenWidth));
                OnPropertyChanged(nameof(ScreenHeight));
            }
        }

        #endregion

        #region Private Methods

        private void InitializeScreenBitmap()
        {
            // Create a WriteableBitmap for the screen
            ScreenBitmap = new WriteableBitmap(
                _screenWidth,
                _screenHeight,
                96, // DPI X
                96, // DPI Y
                PixelFormats.Bgr32, // 32-bit color
                null);
            
            // Fill with black initially
            ClearScreen();
        }

        private void ClearScreen()
        {
            if (ScreenBitmap == null) return;

            ScreenBitmap.Lock();
            try
            {
                unsafe
                {
                    int* pBackBuffer = (int*)ScreenBitmap.BackBuffer;
                    int pixels = _screenWidth * _screenHeight;
                    
                    for (int i = 0; i < pixels; i++)
                    {
                        pBackBuffer[i] = unchecked((int)0xFF000000); // Black
                    }
                }
                
                ScreenBitmap.AddDirtyRect(new Int32Rect(0, 0, _screenWidth, _screenHeight));
            }
            finally
            {
                ScreenBitmap.Unlock();
            }
        }

        private void RenderTimer_Tick(object? sender, EventArgs e)
        {
            UpdateScreen();
        }

        private void UpdateScreen()
        {
            if (ScreenBitmap == null) return;

            // TODO: Get framebuffer data from VMManager via P/Invoke
            // For now, render a test pattern to show it's working
            RenderTestPattern();
        }

        private void RenderTestPattern()
        {
            if (ScreenBitmap == null) return;

            ScreenBitmap.Lock();
            try
            {
                unsafe
                {
                    int* pBackBuffer = (int*)ScreenBitmap.BackBuffer;
                    int stride = ScreenBitmap.BackBufferStride / 4;
                    
                    // Create a simple gradient pattern to show the screen is working
                    for (int y = 0; y < _screenHeight; y++)
                    {
                        for (int x = 0; x < _screenWidth; x++)
                        {
                            int r = (x * 255) / _screenWidth;
                            int g = (y * 255) / _screenHeight;
                            int b = 128;
                            
                            int color = unchecked((int)0xFF000000) | (r << 16) | (g << 8) | b;
                            pBackBuffer[y * stride + x] = color;
                        }
                    }
                }
                
                ScreenBitmap.AddDirtyRect(new Int32Rect(0, 0, _screenWidth, _screenHeight));
            }
            finally
            {
                ScreenBitmap.Unlock();
            }
        }

        private void OnClose()
        {
            StopRendering();
            // Window will handle actual closing
        }

        private void OnResetScale()
        {
            Scale = 1.0;
        }

        private void OnFullscreen()
        {
            // This will be handled by the view
        }

        #endregion
    }
}

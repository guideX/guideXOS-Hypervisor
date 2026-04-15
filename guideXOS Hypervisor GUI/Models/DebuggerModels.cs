using System;
using System.ComponentModel;

namespace guideXOS_Hypervisor_GUI.Models
{
    /// <summary>
    /// Represents a general-purpose register (GR[0..127])
    /// </summary>
    public class GeneralRegister : INotifyPropertyChanged
    {
        private ulong _value;
        
        public int Index { get; set; }
        public string Name => $"GR[{Index}]";
        
        public ulong Value
        {
            get => _value;
            set
            {
                if (_value != value)
                {
                    _value = value;
                    OnPropertyChanged(nameof(Value));
                    OnPropertyChanged(nameof(HexValue));
                }
            }
        }
        
        public string HexValue => $"0x{Value:X16}";
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Represents a floating-point register (FR[0..127])
    /// </summary>
    public class FloatingPointRegister : INotifyPropertyChanged
    {
        private double _value;
        private ulong _rawBits;
        
        public int Index { get; set; }
        public string Name => $"FR[{Index}]";
        
        public double Value
        {
            get => _value;
            set
            {
                if (Math.Abs(_value - value) > double.Epsilon)
                {
                    _value = value;
                    _rawBits = BitConverter.ToUInt64(BitConverter.GetBytes(value), 0);
                    OnPropertyChanged(nameof(Value));
                    OnPropertyChanged(nameof(HexValue));
                }
            }
        }
        
        public ulong RawBits
        {
            get => _rawBits;
            set
            {
                if (_rawBits != value)
                {
                    _rawBits = value;
                    _value = BitConverter.ToDouble(BitConverter.GetBytes(value), 0);
                    OnPropertyChanged(nameof(RawBits));
                    OnPropertyChanged(nameof(Value));
                    OnPropertyChanged(nameof(HexValue));
                }
            }
        }
        
        public string HexValue => $"0x{RawBits:X16} ({Value:G})";
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Represents a predicate register (PR[0..63])
    /// </summary>
    public class PredicateRegister : INotifyPropertyChanged
    {
        private bool _value;
        
        public int Index { get; set; }
        public string Name => $"PR[{Index}]";
        
        public bool Value
        {
            get => _value;
            set
            {
                if (_value != value)
                {
                    _value = value;
                    OnPropertyChanged(nameof(Value));
                    OnPropertyChanged(nameof(DisplayValue));
                }
            }
        }
        
        public string DisplayValue => Value ? "1" : "0";
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Represents a branch register (BR[0..7])
    /// </summary>
    public class BranchRegister : INotifyPropertyChanged
    {
        private ulong _value;
        
        public int Index { get; set; }
        public string Name => $"BR[{Index}]";
        
        public ulong Value
        {
            get => _value;
            set
            {
                if (_value != value)
                {
                    _value = value;
                    OnPropertyChanged(nameof(Value));
                    OnPropertyChanged(nameof(HexValue));
                }
            }
        }
        
        public string HexValue => $"0x{Value:X16}";
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Represents a breakpoint in the debugger
    /// </summary>
    public class Breakpoint : INotifyPropertyChanged
    {
        private bool _isEnabled = true;
        private string _condition = string.Empty;
        
        public int Id { get; set; }
        public ulong Address { get; set; }
        public string HexAddress => $"0x{Address:X16}";
        
        public bool IsEnabled
        {
            get => _isEnabled;
            set
            {
                if (_isEnabled != value)
                {
                    _isEnabled = value;
                    OnPropertyChanged(nameof(IsEnabled));
                    OnPropertyChanged(nameof(Status));
                }
            }
        }
        
        public string Condition
        {
            get => _condition;
            set
            {
                if (_condition != value)
                {
                    _condition = value;
                    OnPropertyChanged(nameof(Condition));
                    OnPropertyChanged(nameof(HasCondition));
                }
            }
        }
        
        public bool HasCondition => !string.IsNullOrWhiteSpace(Condition);
        public string Status => IsEnabled ? "Enabled" : "Disabled";
        public int HitCount { get; set; }
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Represents a watchpoint (memory watch) in the debugger
    /// </summary>
    public class Watchpoint : INotifyPropertyChanged
    {
        private bool _isEnabled = true;
        private WatchpointType _type = WatchpointType.ReadWrite;
        
        public int Id { get; set; }
        public ulong Address { get; set; }
        public string HexAddress => $"0x{Address:X16}";
        public int Size { get; set; } = 8; // Default 8 bytes
        
        public bool IsEnabled
        {
            get => _isEnabled;
            set
            {
                if (_isEnabled != value)
                {
                    _isEnabled = value;
                    OnPropertyChanged(nameof(IsEnabled));
                    OnPropertyChanged(nameof(Status));
                }
            }
        }
        
        public WatchpointType Type
        {
            get => _type;
            set
            {
                if (_type != value)
                {
                    _type = value;
                    OnPropertyChanged(nameof(Type));
                    OnPropertyChanged(nameof(TypeString));
                }
            }
        }
        
        public string TypeString => Type.ToString();
        public string Status => IsEnabled ? "Enabled" : "Disabled";
        public int HitCount { get; set; }
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Types of watchpoints
    /// </summary>
    public enum WatchpointType
    {
        Read,
        Write,
        ReadWrite
    }
    
    /// <summary>
    /// Represents a memory region for display in the memory viewer
    /// </summary>
    public class MemoryRegion : INotifyPropertyChanged
    {
        private ulong _address;
        private byte[] _data = Array.Empty<byte>();
        
        public ulong Address
        {
            get => _address;
            set
            {
                if (_address != value)
                {
                    _address = value;
                    OnPropertyChanged(nameof(Address));
                    OnPropertyChanged(nameof(HexAddress));
                }
            }
        }
        
        public string HexAddress => $"{Address:X16}";
        
        public byte[] Data
        {
            get => _data;
            set
            {
                if (_data != value)
                {
                    _data = value;
                    OnPropertyChanged(nameof(Data));
                    OnPropertyChanged(nameof(HexDump));
                    OnPropertyChanged(nameof(AsciiDump));
                }
            }
        }
        
        public string HexDump
        {
            get
            {
                if (Data == null || Data.Length == 0)
                    return string.Empty;
                    
                return string.Join(" ", Array.ConvertAll(Data, b => $"{b:X2}"));
            }
        }
        
        public string AsciiDump
        {
            get
            {
                if (Data == null || Data.Length == 0)
                    return string.Empty;
                    
                var chars = new char[Data.Length];
                for (int i = 0; i < Data.Length; i++)
                {
                    chars[i] = Data[i] >= 32 && Data[i] <= 126 ? (char)Data[i] : '.';
                }
                return new string(chars);
            }
        }
        
        public event PropertyChangedEventHandler? PropertyChanged;
        
        protected void OnPropertyChanged(string propertyName)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        }
    }
    
    /// <summary>
    /// Complete IA-64 CPU state for debugger
    /// </summary>
    public class IA64CPUState
    {
        public GeneralRegister[] GeneralRegisters { get; set; } = new GeneralRegister[128];
        public FloatingPointRegister[] FloatingPointRegisters { get; set; } = new FloatingPointRegister[128];
        public PredicateRegister[] PredicateRegisters { get; set; } = new PredicateRegister[64];
        public BranchRegister[] BranchRegisters { get; set; } = new BranchRegister[8];
        
        public ulong InstructionPointer { get; set; }
        public ulong CurrentFrameMarker { get; set; }
        public ulong ProcessorStatusRegister { get; set; }
        
        public IA64CPUState()
        {
            // Initialize all registers
            for (int i = 0; i < 128; i++)
            {
                GeneralRegisters[i] = new GeneralRegister { Index = i };
                FloatingPointRegisters[i] = new FloatingPointRegister { Index = i };
            }
            
            for (int i = 0; i < 64; i++)
            {
                PredicateRegisters[i] = new PredicateRegister { Index = i };
            }
            
            for (int i = 0; i < 8; i++)
            {
                BranchRegisters[i] = new BranchRegister { Index = i };
            }
        }
    }
}

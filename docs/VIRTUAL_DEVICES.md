# Virtual Devices Implementation

This document describes the implementation and usage of the three core virtual devices in the IA-64 Hypervisor:
1. **Virtual Console** - Guest I/O output
2. **Virtual Timer** - Programmable interval timer
3. **Interrupt Controller** - Interrupt routing and delivery

## Table of Contents
- [Overview](#overview)
- [Virtual Console](#virtual-console)
- [Virtual Timer](#virtual-timer)
- [Interrupt Controller](#interrupt-controller)
- [Integration with CPU](#integration-with-cpu)
- [Usage Examples](#usage-examples)
- [Memory Map](#memory-map)

---

## Overview

The virtual devices provide essential I/O functionality for guest operating systems and applications running in the hypervisor. They are implemented as memory-mapped devices that can be accessed through standard memory read/write operations.

### Key Features

- **Memory-Mapped I/O**: All devices accessible via memory addresses
- **Interrupt Support**: Timer and devices can trigger CPU interrupts
- **Host Integration**: Console output appears in host UI or stdout
- **IA-64 Compatible**: Designed for IA-64 CPU exception handling

---

## Virtual Console

The Virtual Console (`VirtualConsole` class) provides a memory-mapped character device for guest output that displays in the host environment.

### Memory Layout

| Offset | Register | Access | Description |
|--------|----------|--------|-------------|
| 0x0    | DATA     | W      | Write character to output |
| 0x4    | STATUS   | R      | Buffer size (bytes pending) |
| 0x5    | FLUSH    | W      | Manual flush (write non-zero) |

**Default Base Address**: `0xFFFF0000`  
**Size**: 8 bytes

### Features

- **Automatic Flushing**: Flushes on newline (`\n`) character
- **Manual Flushing**: Write to FLUSH register
- **Output Buffering**: Internal buffer with configurable scrollback
- **Host Display**: Output directed to `std::ostream` (default: `std::cout`)
- **History Access**: API to retrieve past output lines

### API Reference

```cpp
class VirtualConsole : public IMemoryMappedDevice {
public:
    // Constructor
    explicit VirtualConsole(
        uint64_t baseAddress = kDefaultBaseAddress,
        std::ostream& output = std::cout,
        size_t scrollbackLines = kDefaultScrollbackLines
    );

    // Memory-mapped I/O (inherited from IMemoryMappedDevice)
    bool Read(uint64_t address, uint8_t* data, size_t size) const override;
    bool Write(uint64_t address, const uint8_t* data, size_t size) override;

    // Direct console access
    void WriteChar(char value);
    void Flush();

    // Output history access
    std::vector<std::string> getAllOutputLines() const;
    std::vector<std::string> getOutputLines(size_t startLine, size_t count) const;
    std::string getRecentOutput(size_t maxBytes) const;
    std::vector<std::string> getOutputSince(size_t lineNumber) const;
    size_t getOutputLineCount() const;
    uint64_t getTotalBytesWritten() const;

    // State management
    void clearOutput();
    void Reset();
};
```

### Usage Example

```cpp
// Create console
VirtualConsole console(0xFFFF0000, std::cout);

// Guest writes "Hello\n"
const char* msg = "Hello\n";
for (size_t i = 0; msg[i] != '\0'; ++i) {
    uint8_t ch = static_cast<uint8_t>(msg[i]);
    uint64_t addr = 0xFFFF0000; // DATA register
    console.Write(addr, &ch, 1);
}
// Output automatically flushed on '\n'

// Read status
uint8_t status;
console.Read(0xFFFF0004, &status, 1); // STATUS register

// Get output history
auto lines = console.getAllOutputLines();
```

### Guest OS Integration

From guest kernel/userland code:

```c
// IA-64 assembly to write character
void console_putchar(char c) {
    volatile char* console_data = (volatile char*)0xFFFF0000;
    *console_data = c;
}

void console_puts(const char* str) {
    while (*str) {
        console_putchar(*str++);
    }
}
```

---

## Virtual Timer

The Virtual Timer (`VirtualTimer` class) provides a programmable interval timer that generates periodic interrupts for scheduling and system time.

### Memory Layout

| Offset | Register     | Access | Size | Description |
|--------|--------------|--------|------|-------------|
| 0x0    | CONTROL      | R/W    | 1    | Control flags (bit 0: enable, bit 1: periodic) |
| 0x8    | INTERVAL     | R/W    | 8    | Interval in CPU cycles (uint64_t) |
| 0x10   | VECTOR       | R/W    | 1    | Interrupt vector number |
| 0x11   | STATUS       | R/W    | 1    | Status (bit 0: interrupt pending) |

**Default Base Address**: `0xFFFF0100`  
**Size**: 24 bytes

### Control Register Bits

- **Bit 0**: Enable timer (1 = enabled, 0 = disabled)
- **Bit 1**: Periodic mode (1 = periodic, 0 = one-shot)

### Features

- **Programmable Interval**: Set interval in CPU cycles
- **Periodic/One-Shot**: Flexible timer modes
- **Interrupt Generation**: Raises interrupts via attached controller
- **Cycle-Accurate**: Tick-based simulation
- **Interrupt Acknowledgment**: Clear pending interrupt via STATUS write

### API Reference

```cpp
class VirtualTimer : public IMemoryMappedDevice, public ITickableDevice {
public:
    // Constructor
    explicit VirtualTimer(uint64_t baseAddress = kDefaultBaseAddress);

    // Memory-mapped I/O
    bool Read(uint64_t address, uint8_t* data, size_t size) const override;
    bool Write(uint64_t address, const uint8_t* data, size_t size) override;

    // Tick interface (called by CPU scheduler)
    void Tick(uint64_t cycles) override;

    // Configuration
    void Configure(uint64_t intervalCycles, uint8_t vector, 
                   bool periodic, bool enabled);
    void AttachInterruptController(IInterruptController* controller);
    void SetCallback(std::function<void()> callback);

    // State management
    void Acknowledge();
    void Reset();

    // Status query
    bool IsEnabled() const;
    bool IsPeriodic() const;
    bool HasPendingInterrupt() const;
    uint64_t GetIntervalCycles() const;
    uint8_t GetInterruptVector() const;
};
```

### Usage Example

```cpp
// Create interrupt controller
BasicInterruptController intCtrl;
intCtrl.RegisterDeliveryCallback([](uint8_t vector) {
    // Handle interrupt in CPU
    cpu.queueInterrupt(vector);
});

// Create and configure timer
VirtualTimer timer(0xFFFF0100);
timer.AttachInterruptController(&intCtrl);
timer.Configure(
    1000,    // Interval: 1000 cycles
    0x20,    // Vector: 0x20
    true,    // Periodic
    true     // Enabled
);

// Simulation loop
while (running) {
    // Execute CPU instructions...
    
    // Tick the timer (e.g., after 100 cycles)
    timer.Tick(100);
    
    // Timer will raise interrupt when interval expires
}

// Acknowledge interrupt in ISR
if (timer.HasPendingInterrupt()) {
    timer.Acknowledge();
}
```

### Guest OS Integration

```c
// Configure timer from guest kernel
void timer_init(uint64_t interval_cycles) {
    volatile uint8_t* timer_ctrl = (volatile uint8_t*)0xFFFF0100;
    volatile uint64_t* timer_interval = (volatile uint64_t*)0xFFFF0108;
    volatile uint8_t* timer_vector = (volatile uint8_t*)0xFFFF0110;
    
    // Set interval
    *timer_interval = interval_cycles;
    
    // Set interrupt vector
    *timer_vector = 0x20;
    
    // Enable periodic timer
    *timer_ctrl = 0x03; // Enable | Periodic
}

// Acknowledge timer interrupt in ISR
void timer_isr() {
    volatile uint8_t* timer_status = (volatile uint8_t*)0xFFFF0111;
    *timer_status = 1; // Acknowledge
    
    // Handle timer interrupt (scheduling, etc.)
}
```

---

## Interrupt Controller

The Interrupt Controller (`BasicInterruptController` class) routes interrupts from devices to the CPU exception handling system.

### Features

- **Multiple Sources**: Register named interrupt sources with vectors
- **Source Management**: Enable/disable individual sources
- **Queue Management**: FIFO queue for pending interrupts
- **Delivery Callback**: Direct integration with CPU interrupt system
- **Vector Routing**: Map source IDs to interrupt vectors

### Architecture

```
  Device 1 ???
  Device 2 ???
  Timer    ?????> Interrupt Controller ??> CPU Exception Handler
  IPI      ???         (queuing)              (vector delivery)
  Device N ???
```

### API Reference

```cpp
class IInterruptController {
public:
    // Callback type: delivers vector to CPU
    using InterruptDeliveryCallback = std::function<void(uint8_t vector)>;

    // Setup
    virtual void RegisterDeliveryCallback(InterruptDeliveryCallback callback) = 0;
    
    // Source management
    virtual size_t RegisterSource(const std::string& name, uint8_t vector) = 0;
    virtual bool SetSourceEnabled(size_t sourceId, bool enabled) = 0;
    
    // Interrupt raising
    virtual bool RaiseInterrupt(uint8_t vector) = 0;
    virtual bool RaiseInterrupt(size_t sourceId) = 0;
    
    // Interrupt delivery
    virtual bool HasPendingInterrupt() const = 0;
    virtual bool GetNextPendingInterrupt(uint8_t& vector) = 0;
    virtual void DeliverPendingInterrupts() = 0;
    
    // State management
    virtual void Reset() = 0;
};

class BasicInterruptController : public IInterruptController {
    // Implements all IInterruptController methods
};
```

### Usage Example

```cpp
// Create controller
BasicInterruptController intCtrl;

// Register delivery to CPU
intCtrl.RegisterDeliveryCallback([&cpu](uint8_t vector) {
    cpu.queueInterrupt(vector);
    cpu.setState(CPUState::RUNNING); // Wake from WAIT
});

// Register interrupt sources
size_t timerId = intCtrl.RegisterSource("System Timer", 0x20);
size_t diskId = intCtrl.RegisterSource("Disk Controller", 0x21);
size_t nicId = intCtrl.RegisterSource("Network Card", 0x22);

// Device raises interrupt
intCtrl.RaiseInterrupt(timerId);  // By source ID
// OR
intCtrl.RaiseInterrupt(0x20);     // By vector

// Interrupts are automatically delivered via callback
```

### Integration with Devices

```cpp
// Timer integration
VirtualTimer timer;
timer.AttachInterruptController(&intCtrl);

// When timer fires, it calls:
intCtrl.RaiseInterrupt(timer.GetInterruptVector());
// Which triggers the delivery callback
```

---

## Integration with CPU

The virtual devices integrate with the IA-64 CPU exception handling system through the interrupt controller.

### Exception Flow

1. **Device Event**: Timer expires, console buffer full, etc.
2. **Raise Interrupt**: Device calls `intCtrl.RaiseInterrupt(vector)`
3. **Queue**: Interrupt controller queues the interrupt
4. **Deliver**: Callback invokes `cpu.queueInterrupt(vector)`
5. **Exception**: CPU exception handler processes the interrupt
6. **ISR**: Guest interrupt service routine executes
7. **Acknowledge**: Guest writes to device STATUS register

### CPU Integration Example

```cpp
// Setup in VirtualMachine constructor
interruptController_->RegisterDeliveryCallback([this](uint8_t vector) {
    // Determine target CPU (for SMP)
    const int targetCPU = activeCPUIndex_ >= 0 ? activeCPUIndex_ : 0;
    
    if (isValidCPUIndex(targetCPU) && cpus_[targetCPU].cpu) {
        // Queue interrupt in CPU
        cpus_[targetCPU].cpu->queueInterrupt(vector);
        
        // Wake CPU from WAIT state
        if (cpus_[targetCPU].state == CPUExecutionState::WAITING) {
            cpus_[targetCPU].state = CPUExecutionState::RUNNING;
        }
    }
});
```

### IA-64 Exception Handling

In IA-64 architecture:
- External interrupts use vectors 0x20-0x2F
- CPU saves state and jumps to IVT (Interrupt Vector Table)
- PSR.ic (interrupt collection) must be enabled
- RFI instruction returns from interrupt

---

## Usage Examples

### Example 1: Simple Console Output

```cpp
#include "Console.h"

VirtualConsole console;
console.WriteChar('H');
console.WriteChar('i');
console.WriteChar('\n'); // Auto-flushes
```

### Example 2: Scheduler Timer

```cpp
#include "Timer.h"
#include "InterruptController.h"

BasicInterruptController intCtrl;
intCtrl.RegisterDeliveryCallback([](uint8_t vec) {
    handleTimerInterrupt();
});

VirtualTimer schedTimer;
schedTimer.AttachInterruptController(&intCtrl);
schedTimer.Configure(10000, 0x20, true, true); // 10K cycle intervals

// In main loop
while (running) {
    executeCPUInstructions(100);
    schedTimer.Tick(100);
}
```

### Example 3: Multi-Device System

```cpp
// Setup
BasicInterruptController intCtrl;
VirtualConsole console;
VirtualTimer timer1, timer2;
Memory memory(16 * 1024 * 1024);

// Register devices with memory
memory.RegisterDevice(&console);
memory.RegisterDevice(&timer1);
memory.RegisterDevice(&timer2);

// Configure interrupt routing
timer1.AttachInterruptController(&intCtrl);
timer2.AttachInterruptController(&intCtrl);

// Configure timers
timer1.Configure(1000, 0x20, true, true);  // System tick
timer2.Configure(5000, 0x21, true, true);  // Scheduler

// Connect to CPU
intCtrl.RegisterDeliveryCallback([&cpu](uint8_t vector) {
    cpu.queueInterrupt(vector);
});
```

---

## Memory Map

### Default Device Layout

| Address Range          | Device            | Size  | Description |
|------------------------|-------------------|-------|-------------|
| `0xFFFF0000-0xFFFF0007` | Virtual Console   | 8 B   | Console I/O registers |
| `0xFFFF0100-0xFFFF0117` | Virtual Timer     | 24 B  | Timer control registers |
| `0xFFFF0200-0xFFFF0217` | Virtual Timer #2  | 24 B  | Additional timer (optional) |

### Custom Memory Map

Devices can be placed at any address:

```cpp
// Place console at top of 16MB memory
VirtualConsole console(16 * 1024 * 1024 - 0x1000);

// Place timer nearby
VirtualTimer timer(16 * 1024 * 1024 - 0x1000 + 0x100);
```

---

## Performance Considerations

### Timer Tick Granularity

- Tick granularity affects timer accuracy
- Smaller tick intervals = more overhead
- Recommended: 100-1000 cycles per tick

```cpp
// Fine-grained (higher accuracy, more overhead)
timer.Tick(10);

// Coarse-grained (lower accuracy, less overhead)
timer.Tick(1000);
```

### Console Buffering

- Automatic flush on `\n` reduces manual flushes
- Large scrollback buffers use more memory
- Recommended: 10,000 lines (default)

```cpp
// Minimal scrollback
VirtualConsole console(baseAddr, std::cout, 1000);

// Large scrollback for debugging
VirtualConsole console(baseAddr, std::cout, 100000);
```

### Interrupt Latency

- Interrupts delivered immediately via callback
- No polling required
- Minimal overhead

---

## Testing

Run the comprehensive example:

```bash
./examples/virtual_devices_example
```

This demonstrates:
- Console output to host
- Timer interrupt generation
- Interrupt controller routing
- Memory-mapped I/O access
- Complete integration

---

## See Also

- [Virtual Machine Implementation](VMMANAGER.md)
- [CPU Implementation](CPU_IMPLEMENTATION.md)
- [Memory Management](MMU_IMPLEMENTATION.md)
- [API Documentation](API_DOCUMENTATION.md)

---

## References

- IA-64 Architecture Manual, Volume 2 (Interrupt System)
- Device I/O and Memory-Mapped I/O
- Timer/Counter Hardware Specifications

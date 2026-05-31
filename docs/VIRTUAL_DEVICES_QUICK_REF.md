# Virtual Devices Quick Reference

## Quick Start

### Include Headers
```cpp
#include "Console.h"
#include "Timer.h"
#include "InterruptController.h"
#include "memory.h"
```

### Basic Setup
```cpp
// Create devices
VirtualConsole console;
VirtualTimer timer;
BasicInterruptController intCtrl;

// Connect interrupt controller to CPU
intCtrl.RegisterDeliveryCallback([&cpu](uint8_t vector) {
    cpu.queueInterrupt(vector);
});

// Attach timer to interrupt controller
timer.AttachInterruptController(&intCtrl);

// Register with memory system
Memory memory(16 * 1024 * 1024);
memory.RegisterDevice(&console);
memory.RegisterDevice(&timer);
```

---

## Virtual Console

### Memory Map
| Offset | Register | Size | R/W | Description |
|--------|----------|------|-----|-------------|
| +0x0   | DATA     | 1    | W   | Write character |
| +0x4   | STATUS   | 1    | R   | Buffer size |
| +0x5   | FLUSH    | 1    | W   | Manual flush |

### Write Character
```cpp
uint8_t ch = 'A';
console.Write(baseAddr + 0, &ch, 1);
```

### Read Status
```cpp
uint8_t status;
console.Read(baseAddr + 4, &status, 1);
```

### Manual Flush
```cpp
uint8_t flush = 1;
console.Write(baseAddr + 5, &flush, 1);
```

### Get Output History
```cpp
auto lines = console.getAllOutputLines();
auto recent = console.getRecentOutput(1024);
```

---

## Virtual Timer

### Memory Map
| Offset | Register | Size | R/W | Description |
|--------|----------|------|-----|-------------|
| +0x0   | CONTROL  | 1    | R/W | Enable & mode flags |
| +0x8   | INTERVAL | 8    | R/W | Cycle interval (uint64_t) |
| +0x10  | VECTOR   | 1    | R/W | Interrupt vector |
| +0x11  | STATUS   | 1    | R/W | Interrupt pending flag |

### Configure Timer
```cpp
timer.Configure(
    1000,   // Interval (cycles)
    0x20,   // Interrupt vector
    true,   // Periodic mode
    true    // Enabled
);
```

### Enable/Disable
```cpp
// Enable periodic timer
uint8_t ctrl = 0x03; // Enable | Periodic
timer.Write(baseAddr + 0, &ctrl, 1);

// Disable timer
ctrl = 0x00;
timer.Write(baseAddr + 0, &ctrl, 1);
```

### Set Interval
```cpp
uint64_t interval = 5000; // cycles
timer.Write(baseAddr + 8, reinterpret_cast<uint8_t*>(&interval), 8);
```

### Tick Timer
```cpp
// In execution loop
timer.Tick(100); // Advance 100 cycles
```

### Acknowledge Interrupt
```cpp
if (timer.HasPendingInterrupt()) {
    timer.Acknowledge();
    // or via memory-mapped I/O:
    uint8_t ack = 1;
    timer.Write(baseAddr + 0x11, &ack, 1);
}
```

---

## Interrupt Controller

### Register Sources
```cpp
size_t timerId = intCtrl.RegisterSource("Timer", 0x20);
size_t diskId = intCtrl.RegisterSource("Disk", 0x21);
```

### Raise Interrupts
```cpp
// By source ID
intCtrl.RaiseInterrupt(timerId);

// By vector
intCtrl.RaiseInterrupt(static_cast<uint8_t>(0x20));
```

### Enable/Disable Sources
```cpp
intCtrl.SetSourceEnabled(timerId, false); // Disable
intCtrl.SetSourceEnabled(timerId, true);  // Enable
```

### Poll Interrupts
```cpp
if (intCtrl.HasPendingInterrupt()) {
    uint8_t vector;
    if (intCtrl.GetNextPendingInterrupt(vector)) {
        // Handle interrupt
    }
}
```

---

## Common Patterns

### Scheduler Timer
```cpp
VirtualTimer schedTimer;
schedTimer.AttachInterruptController(&intCtrl);
schedTimer.Configure(10000, 0x20, true, true); // 10K cycles

while (running) {
    // Execute instructions...
    executeCPU(1000);
    
    // Tick timer
    schedTimer.Tick(1000);
    
    // Timer fires interrupt automatically via intCtrl
}
```

### System Time Tracking
```cpp
uint64_t systemTime = 0;
timer.SetCallback([&systemTime]() {
    systemTime += 1; // Increment on each tick
});
```

### Debug Console
```cpp
void debug_print(const std::string& msg) {
    for (char c : msg) {
        uint8_t ch = static_cast<uint8_t>(c);
        console.Write(0xFFFF0000, &ch, 1);
    }
}
```

### Multi-CPU Interrupt Routing
```cpp
intCtrl.RegisterDeliveryCallback([&cpus, &activeCPU](uint8_t vector) {
    int target = activeCPU >= 0 ? activeCPU : 0;
    cpus[target].queueInterrupt(vector);
});
```

---

## Guest OS Integration

### Console Driver (C)
```c
#define CONSOLE_BASE 0xFFFF0000ULL
#define CONSOLE_DATA (CONSOLE_BASE + 0)
#define CONSOLE_STATUS (CONSOLE_BASE + 4)
#define CONSOLE_FLUSH (CONSOLE_BASE + 5)

void putchar(char c) {
    *(volatile char*)CONSOLE_DATA = c;
}

void puts(const char* s) {
    while (*s) putchar(*s++);
}
```

### Timer Driver (C)
```c
#define TIMER_BASE 0xFFFF0100ULL
#define TIMER_CTRL (TIMER_BASE + 0)
#define TIMER_INTERVAL (TIMER_BASE + 8)
#define TIMER_VECTOR (TIMER_BASE + 16)
#define TIMER_STATUS (TIMER_BASE + 17)

void timer_init(uint64_t interval) {
    *(volatile uint64_t*)TIMER_INTERVAL = interval;
    *(volatile uint8_t*)TIMER_VECTOR = 0x20;
    *(volatile uint8_t*)TIMER_CTRL = 0x03; // Enable | Periodic
}

void timer_isr(void) {
    // Acknowledge interrupt
    *(volatile uint8_t*)TIMER_STATUS = 1;
    
    // Handle timer tick
    schedule_next_task();
}
```

### IA-64 Assembly Example
```asm
; Write to console
mov r8 = 0xFFFF0000    ; Console base
mov r9 = 'H'           ; Character
st1 [r8] = r9          ; Store byte

; Configure timer
mov r8 = 0xFFFF0100    ; Timer base
mov r9 = 1000          ; Interval
st8 [r8] = r9, 8       ; Store interval, advance r8
mov r9 = 0x20          ; Vector
st1 [r8] = r9, 1       ; Store vector
mov r9 = 0x03          ; Enable | Periodic
mov r8 = 0xFFFF0100    ; Reset to base
st1 [r8] = r9          ; Store control
```

---

## Memory Addresses

### Default Layout
```
0xFFFF0000  Console Device (8 bytes)
  +0x0      DATA register
  +0x4      STATUS register
  +0x5      FLUSH register

0xFFFF0100  Timer Device (24 bytes)
  +0x0      CONTROL register
  +0x8      INTERVAL register (8 bytes)
  +0x10     VECTOR register
  +0x11     STATUS register
```

### Custom Addresses
```cpp
// Place at top of memory (16MB system)
const uint64_t ioBase = 16 * 1024 * 1024 - 0x1000;
VirtualConsole console(ioBase);
VirtualTimer timer(ioBase + 0x100);
```

---

## Error Handling

### Check Device Range
```cpp
if (console.HandlesRange(address, size)) {
    console.Write(address, data, size);
}
```

### Validate Timer State
```cpp
if (!timer.IsEnabled()) {
    std::cerr << "Timer is disabled\n";
}

if (timer.GetIntervalCycles() == 0) {
    std::cerr << "Invalid timer interval\n";
}
```

### Check Interrupt Queue
```cpp
if (intCtrl.HasPendingInterrupt()) {
    uint8_t vector;
    while (intCtrl.GetNextPendingInterrupt(vector)) {
        std::cout << "Pending: 0x" << std::hex << (int)vector << "\n";
    }
}
```

---

## Performance Tips

1. **Timer Granularity**: Use coarse ticks (100-1000 cycles) to reduce overhead
2. **Console Buffering**: Auto-flush on `\n` is efficient
3. **Interrupt Delivery**: Direct callback is zero-overhead
4. **Memory-Mapped I/O**: Device checks are fast (range comparison)

---

## See Also

- [Virtual Devices Documentation](VIRTUAL_DEVICES.md) - Full documentation
- [CPU Implementation](CPU_IMPLEMENTATION.md) - CPU integration
- [Memory Management](MMU_IMPLEMENTATION.md) - Memory system
- Example: `examples/virtual_devices_example.cpp`

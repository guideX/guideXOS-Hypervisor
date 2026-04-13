/**
 * @file virtual_devices_example.cpp
 * @brief Comprehensive example demonstrating Virtual Console, Timer, and Interrupt Controller
 * 
 * This example shows how to:
 * 1. Use the Virtual Console for guest I/O output
 * 2. Configure and use the programmable Virtual Timer
 * 3. Route interrupts through the Interrupt Controller to CPU exception handling
 */

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include "Console.h"
#include "Timer.h"
#include "InterruptController.h"
#include "memory.h"
#include "cpu_state.h"

using namespace ia64;

// Simulated interrupt handler
void handleInterrupt(uint8_t vector) {
    std::cout << "[INTERRUPT] Vector 0x" << std::hex << static_cast<int>(vector) 
              << std::dec << " delivered to CPU\n";
}

int main() {
    std::cout << "==============================================\n";
    std::cout << "IA-64 Virtual Devices Integration Example\n";
    std::cout << "==============================================\n\n";

    try {
        // ========================================================================
        // Part 1: Virtual Console Device
        // ========================================================================
        std::cout << "--- Part 1: Virtual Console Device ---\n\n";

        // Create a virtual console mapped to guest I/O
        // Base address: 0xFFFF0000 (default)
        // Output stream: std::cout
        VirtualConsole console(VirtualConsole::kDefaultBaseAddress, std::cout);

        std::cout << "Virtual Console created at address 0x" 
                  << std::hex << console.GetBaseAddress() << std::dec << "\n";
        std::cout << "Memory region: 0x" << std::hex << console.GetBaseAddress() 
                  << " - 0x" << (console.GetBaseAddress() + console.GetSize()) 
                  << std::dec << "\n\n";

        // Simulate guest kernel writing to console
        std::cout << "Guest kernel writing: \"Hello from IA-64!\\n\"\n";
        const char* message = "Hello from IA-64!\n";
        for (size_t i = 0; message[i] != '\0'; ++i) {
            uint8_t data = static_cast<uint8_t>(message[i]);
            uint64_t addr = console.GetBaseAddress() + VirtualConsole::kDataRegisterOffset;
            console.Write(addr, &data, 1);
        }

        // Read status register (shows buffer size)
        uint8_t status = 0;
        console.Read(console.GetBaseAddress() + VirtualConsole::kStatusRegisterOffset, &status, 1);
        std::cout << "Console status: " << static_cast<int>(status) << " bytes buffered\n\n";

        // Manual flush
        std::cout << "Manually flushing console...\n";
        uint8_t flushCmd = 1;
        console.Write(console.GetBaseAddress() + VirtualConsole::kFlushRegisterOffset, &flushCmd, 1);

        // Get console output history
        auto lines = console.getAllOutputLines();
        std::cout << "\nConsole output history (" << lines.size() << " lines):\n";
        for (const auto& line : lines) {
            std::cout << "  > " << line << "\n";
        }

        std::cout << "\n";

        // ========================================================================
        // Part 2: Interrupt Controller
        // ========================================================================
        std::cout << "--- Part 2: Interrupt Controller ---\n\n";

        // Create interrupt controller
        BasicInterruptController intController;

        // Register delivery callback (connects to CPU exception handling)
        intController.RegisterDeliveryCallback(handleInterrupt);
        std::cout << "Interrupt delivery callback registered\n";

        // Register interrupt sources
        size_t timerSource = intController.RegisterSource("System Timer", 0x20);
        size_t deviceSource = intController.RegisterSource("I/O Device", 0x21);
        size_t ipiSource = intController.RegisterSource("IPI", 0x22);

        std::cout << "Registered interrupt sources:\n";
        std::cout << "  - Timer (ID: " << timerSource << ", Vector: 0x20)\n";
        std::cout << "  - I/O Device (ID: " << deviceSource << ", Vector: 0x21)\n";
        std::cout << "  - IPI (ID: " << ipiSource << ", Vector: 0x22)\n\n";

        // Test interrupt routing
        std::cout << "Raising interrupts by source ID:\n";
        intController.RaiseInterrupt(timerSource);
        intController.RaiseInterrupt(deviceSource);

        std::cout << "\nRaising interrupt by vector:\n";
        intController.RaiseInterrupt(static_cast<uint8_t>(0x22));

        std::cout << "\n";

        // ========================================================================
        // Part 3: Programmable Virtual Timer
        // ========================================================================
        std::cout << "--- Part 3: Programmable Virtual Timer ---\n\n";

        // Create a new interrupt controller for the timer demo
        BasicInterruptController timerIntController;
        timerIntController.RegisterDeliveryCallback(handleInterrupt);

        // Create virtual timer
        VirtualTimer timer(VirtualTimer::kDefaultBaseAddress);
        timer.AttachInterruptController(&timerIntController);

        std::cout << "Virtual Timer created at address 0x" 
                  << std::hex << timer.GetBaseAddress() << std::dec << "\n";
        std::cout << "Memory region: 0x" << std::hex << timer.GetBaseAddress() 
                  << " - 0x" << (timer.GetBaseAddress() + timer.GetSize()) 
                  << std::dec << "\n\n";

        // Configure timer: 1000 cycles interval, vector 0x20, periodic, enabled
        std::cout << "Configuring timer:\n";
        std::cout << "  - Interval: 1000 cycles\n";
        std::cout << "  - Vector: 0x20\n";
        std::cout << "  - Mode: Periodic\n";
        std::cout << "  - Enabled: Yes\n\n";
        
        timer.Configure(1000, 0x20, true, true);

        // Add a callback for demonstration
        int timerFireCount = 0;
        timer.SetCallback([&timerFireCount]() {
            timerFireCount++;
        });

        // Simulate CPU cycles and timer ticks
        std::cout << "Simulating CPU execution (5000 cycles):\n";
        std::cout << "----------------------------------------\n";

        uint64_t totalCycles = 0;
        const uint64_t cyclesPerTick = 250;
        const uint64_t maxCycles = 5000;

        while (totalCycles < maxCycles) {
            timer.Tick(cyclesPerTick);
            totalCycles += cyclesPerTick;

            if (totalCycles % 1000 == 0) {
                std::cout << "Cycles: " << totalCycles 
                          << " | Timer fires: " << timerFireCount << "\n";
            }
        }

        std::cout << "\nFinal state:\n";
        std::cout << "  - Total cycles: " << totalCycles << "\n";
        std::cout << "  - Timer fire count: " << timerFireCount << "\n";
        std::cout << "  - Timer enabled: " << (timer.IsEnabled() ? "Yes" : "No") << "\n";
        std::cout << "  - Timer pending: " << (timer.HasPendingInterrupt() ? "Yes" : "No") << "\n\n";

        // ========================================================================
        // Part 4: Memory-Mapped I/O Integration
        // ========================================================================
        std::cout << "--- Part 4: Memory-Mapped I/O Integration ---\n\n";

        // Create memory system
        const size_t memorySize = 16 * 1024 * 1024; // 16MB
        Memory memory(memorySize);

        // Register devices with memory system
        memory.RegisterDevice(&console);
        memory.RegisterDevice(&timer);

        std::cout << "Devices registered with memory system\n";
        std::cout << "Memory size: " << (memorySize / 1024 / 1024) << " MB\n\n";

        // Test memory-mapped access to console
        std::cout << "Testing memory-mapped console access:\n";
        const char* mmioMessage = "MMIO Test\n";
        for (size_t i = 0; mmioMessage[i] != '\0'; ++i) {
            uint64_t addr = console.GetBaseAddress() + VirtualConsole::kDataRegisterOffset;
            uint8_t data = static_cast<uint8_t>(mmioMessage[i]);
            memory.Write(addr, &data, 1);
        }

        // Test memory-mapped access to timer configuration
        std::cout << "\nTesting memory-mapped timer access:\n";
        
        // Read current control register
        uint8_t controlReg = 0;
        memory.Read(timer.GetBaseAddress() + VirtualTimer::kControlOffset, &controlReg, 1);
        std::cout << "Timer control register: 0x" << std::hex << static_cast<int>(controlReg) 
                  << std::dec << "\n";

        // Modify control register (disable timer)
        controlReg = 0;
        memory.Write(timer.GetBaseAddress() + VirtualTimer::kControlOffset, &controlReg, 1);
        std::cout << "Timer disabled via memory-mapped I/O\n";
        std::cout << "Timer enabled: " << (timer.IsEnabled() ? "Yes" : "No") << "\n\n";

        // ========================================================================
        // Part 5: Complete Integration Example
        // ========================================================================
        std::cout << "--- Part 5: Complete Integration Example ---\n\n";

        // Create a new timer with interrupt controller
        VirtualTimer schedTimer(0xFFFF0200);
        BasicInterruptController schedIntController;
        
        int interruptCount = 0;
        schedIntController.RegisterDeliveryCallback([&interruptCount](uint8_t vector) {
            interruptCount++;
            std::cout << "  [IRQ #" << interruptCount << "] Scheduler interrupt (vector 0x" 
                      << std::hex << static_cast<int>(vector) << std::dec << ")\n";
        });

        schedTimer.AttachInterruptController(&schedIntController);
        schedTimer.Configure(100, 0x30, true, true);

        std::cout << "Simulating scheduler timer for task switching:\n";
        std::cout << "Timer interval: 100 cycles (simulated time slices)\n\n";

        // Simulate 10 time slices
        for (int slice = 1; slice <= 10; ++slice) {
            std::cout << "Time slice " << slice << ":\n";
            
            // Execute for 100 cycles
            schedTimer.Tick(100);
            
            // Acknowledge the interrupt
            if (schedTimer.HasPendingInterrupt()) {
                schedTimer.Acknowledge();
            }
            
            std::cout << "\n";
        }

        std::cout << "Total scheduler interrupts: " << interruptCount << "\n\n";

        // ========================================================================
        // Summary
        // ========================================================================
        std::cout << "==============================================\n";
        std::cout << "Summary\n";
        std::cout << "==============================================\n\n";

        std::cout << "Successfully demonstrated:\n";
        std::cout << "  ? Virtual Console - Guest I/O output to host\n";
        std::cout << "  ? Virtual Timer - Programmable interval timer\n";
        std::cout << "  ? Interrupt Controller - Interrupt routing to CPU\n";
        std::cout << "  ? Memory-Mapped I/O - Device access via memory\n";
        std::cout << "  ? Integration - All components working together\n\n";

        std::cout << "These components enable:\n";
        std::cout << "  - Kernel and userland output display\n";
        std::cout << "  - Regular timer interrupts for scheduling\n";
        std::cout << "  - Device interrupt handling\n";
        std::cout << "  - System time management\n\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

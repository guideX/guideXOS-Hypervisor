#pragma once

#include "IVirtualMachine.h"
#include "ICPU.h"
#include "IMemory.h"
#include "IDecoder.h"
#include <memory>
#include <set>
#include <cstdint>

namespace ia64 {

// Forward declarations
class CPU;
class Memory;
class InstructionDecoder;

/**
 * VirtualMachine - Complete IA-64 virtual machine implementation
 * 
 * This class encapsulates:
 * - CPU core (IA-64 processor)
 * - Memory system (paged virtual memory)
 * - Instruction decoder (bundle decoder)
 * - Execution state management
 * - Debugging infrastructure
 * 
 * The VM owns all subsystems and manages their lifecycle.
 * CPU state is fully encapsulated - external code interacts
 * through the VM interface, not directly with CPU.
 * 
 * Architecture:
 * 
 *   VirtualMachine
 *   +------------------+
 *   | - CPU (owned)    |
 *   | - Memory (owned) |
 *   | - Decoder (owned)|
 *   | - VMState        |
 *   | - Breakpoints    |
 *   +------------------+
 *          |
 *          v
 *   CPU executes instructions
 *   using Memory and Decoder
 * 
 * Ownership Model:
 * - VM owns CPU, Memory, Decoder (unique_ptr)
 * - CPU holds references to Memory and Decoder (injected)
 * - Clean destruction order guaranteed
 * 
 * Thread Safety:
 * - Single-threaded execution model
 * - No concurrent access to CPU state
 * - Future: Add mutex for multi-threaded debugging
 */
class VirtualMachine : public IVirtualMachine {
public:
    /**
     * Constructor
     * 
     * @param memorySize Size of virtual memory in bytes (default 16MB)
     */
    explicit VirtualMachine(size_t memorySize = 16 * 1024 * 1024);

    /**
     * Destructor - cleans up all subsystems
     */
    ~VirtualMachine() override;

    // Disable copy/move (VM is not copyable)
    VirtualMachine(const VirtualMachine&) = delete;
    VirtualMachine& operator=(const VirtualMachine&) = delete;
    VirtualMachine(VirtualMachine&&) = delete;
    VirtualMachine& operator=(VirtualMachine&&) = delete;

    // ========================================================================
    // IVirtualMachine Implementation - Lifecycle
    // ========================================================================

    bool init() override;
    bool reset() override;
    void halt() override;

    // ========================================================================
    // IVirtualMachine Implementation - Execution
    // ========================================================================

    bool step() override;
    uint64_t run(uint64_t maxCycles = 0) override;

    // ========================================================================
    // IVirtualMachine Implementation - Program Loading
    // ========================================================================

    bool loadProgram(const uint8_t* data, size_t size, uint64_t loadAddress) override;
    void setEntryPoint(uint64_t address) override;

    // ========================================================================
    // IVirtualMachine Implementation - State Query
    // ========================================================================

    VMState getState() const override { return state_; }
    uint64_t getIP() const override;
    const CPUState& getCPUState() const override;

    // ========================================================================
    // IVirtualMachine Implementation - Debugging
    // ========================================================================

    bool attach_debugger() override;
    void detach_debugger() override;
    bool isDebuggerAttached() const override { return debuggerAttached_; }
    bool setBreakpoint(uint64_t address) override;
    bool clearBreakpoint(uint64_t address) override;

    // ========================================================================
    // IVirtualMachine Implementation - Register Access
    // ========================================================================

    uint64_t readGR(size_t index) const override;
    void writeGR(size_t index, uint64_t value) override;

    // ========================================================================
    // Debug Support
    // ========================================================================

    void dump() const override;

    // ========================================================================
    // Extended API (beyond interface)
    // ========================================================================

    /**
     * Get memory system (for advanced access)
     * Use with caution - bypasses VM encapsulation
     */
    IMemory& getMemory();
    const IMemory& getMemory() const;

    /**
     * Get CPU (for advanced access)
     * Use with caution - bypasses VM encapsulation
     */
    ICPU& getCPU();
    const ICPU& getCPU() const;

private:
    // ========================================================================
    // Subsystems (owned by VM)
    // ========================================================================

    std::unique_ptr<Memory> memory_;                // Memory system
    std::unique_ptr<InstructionDecoder> decoder_;   // Instruction decoder
    std::unique_ptr<CPU> cpu_;                      // CPU core

    // ========================================================================
    // VM State
    // ========================================================================

    VMState state_;                                 // Current execution state
    bool debuggerAttached_;                         // Debugger attached?
    std::set<uint64_t> breakpoints_;               // Breakpoint addresses
    uint64_t cyclesExecuted_;                       // Total cycles executed

    // ========================================================================
    // Internal Helpers
    // ========================================================================

    /**
     * Check if we hit a breakpoint
     * @param address Current IP
     * @return true if breakpoint hit
     */
    bool checkBreakpoint(uint64_t address) const;

    /**
     * Initialize all subsystems
     * @return true if initialization succeeded
     */
    bool initializeSubsystems();
};

} // namespace ia64

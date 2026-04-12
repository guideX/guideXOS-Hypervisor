# ISA Plugin Architecture

## Overview

The ISA Plugin Architecture enables the IA-64 emulator to support multiple instruction set architectures (ISAs) within the same virtual machine framework. This design allows:

- **Multiple ISA Support**: Run different ISAs (IA-64, x86-64, ARM64, etc.) in the same VM
- **Shared Resources**: All ISAs share memory and device models
- **Clean Separation**: ISA-specific code is isolated in plugins
- **Easy Extension**: Add new ISAs without modifying core VM code
- **Hot-Swapping**: Switch between ISAs at runtime (future feature)

## Architecture Diagram

```
???????????????????????????????????????????????????????????????
?                      VirtualMachine                          ?
?  ??????????????  ??????????????  ??????????????            ?
?  ?   CPU 0    ?  ?   CPU 1    ?  ?   CPU N    ?            ?
?  ?  (IA-64)   ?  ?  (x86-64)  ?  ?  (ARM64)   ?            ?
?  ?            ?  ?            ?  ?            ?            ?
?  ?  ????????  ?  ?  ????????  ?  ?  ????????  ?            ?
?  ?  ? ISA  ?  ?  ?  ? ISA  ?  ?  ?  ? ISA  ?  ?            ?
?  ?  ?Plugin?  ?  ?  ?Plugin?  ?  ?  ?Plugin?  ?            ?
?  ?  ????????  ?  ?  ????????  ?  ?  ????????  ?            ?
?  ??????????????  ??????????????  ??????????????            ?
?        ?               ?               ?                    ?
?        ?????????????????????????????????                    ?
?                        ?                                    ?
?  ??????????????????????????????????????????????             ?
?  ?        Shared Memory & Devices              ?             ?
?  ?  ????????????  ????????????  ???????????? ?             ?
?  ?  ?  Memory  ?  ? Console  ?  ?  Timer   ? ?             ?
?  ?  ????????????  ????????????  ???????????? ?             ?
?  ???????????????????????????????????????????????             ?
?                                                              ?
?  ????????????????????????????????????????????????            ?
?  ?         ISA Plugin Registry                  ?            ?
?  ?  ??????????  ??????????  ??????????         ?            ?
?  ?  ? IA-64  ?  ? x86-64 ?  ? ARM64  ?  ...    ?            ?
?  ?  ?Factory ?  ?Factory ?  ?Factory ?         ?            ?
?  ?  ??????????  ??????????  ??????????         ?            ?
?  ????????????????????????????????????????????????            ?
???????????????????????????????????????????????????????????????
```

## Core Interfaces

### IISA Interface

The `IISA` interface defines the contract for all ISA plugins:

```cpp
class IISA {
public:
    // ISA identification
    virtual std::string getName() const = 0;
    virtual std::string getVersion() const = 0;
    
    // Core operations (required by all ISAs)
    virtual void reset() = 0;
    virtual ISADecodeResult decode(IMemory& memory) = 0;
    virtual ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& result) = 0;
    
    // State management (required for checkpointing)
    virtual ISAState& getState() = 0;
    virtual std::vector<uint8_t> serialize_state() const = 0;
    virtual bool deserialize_state(const std::vector<uint8_t>& data) = 0;
    
    // Debugging support
    virtual std::string dumpState() const = 0;
    virtual std::string disassemble(IMemory& memory, uint64_t address) = 0;
    
    // ISA capabilities
    virtual size_t getWordSize() const = 0;
    virtual bool isLittleEndian() const = 0;
    virtual bool hasFeature(const std::string& feature) const = 0;
};
```

### ISAState Interface

The `ISAState` interface provides type-erased access to ISA-specific architectural state:

```cpp
class ISAState {
public:
    virtual std::unique_ptr<ISAState> clone() const = 0;
    virtual void serialize(uint8_t* buffer) const = 0;
    virtual void deserialize(const uint8_t* buffer) = 0;
    virtual size_t getStateSize() const = 0;
    virtual std::string toString() const = 0;
    virtual void reset() = 0;
};
```

## Creating a New ISA Plugin

### Step 1: Define ISA State

Create a class that inherits from `ISAState` and contains all architectural registers and state for your ISA:

```cpp
class MyISAState : public ISAState {
public:
    // Your ISA's registers
    uint64_t generalRegisters[32];
    uint64_t programCounter;
    uint32_t statusFlags;
    
    // Implement ISAState interface
    std::unique_ptr<ISAState> clone() const override;
    void serialize(uint8_t* buffer) const override;
    void deserialize(const uint8_t* buffer) override;
    size_t getStateSize() const override;
    std::string toString() const override;
    void reset() override;
};
```

### Step 2: Implement ISA Plugin

Create a class that inherits from `IISA` and implements all required methods:

```cpp
class MyISAPlugin : public IISA {
public:
    std::string getName() const override { return "MyISA"; }
    std::string getVersion() const override { return "1.0"; }
    
    void reset() override {
        state_.reset();
    }
    
    ISADecodeResult decode(IMemory& memory) override {
        // Fetch instruction bytes from memory
        // Decode into your ISA's instruction format
        // Return decode result
    }
    
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& result) override {
        // Execute the decoded instruction
        // Update architectural state
        // Perform memory accesses via memory parameter
        // Return execution result
    }
    
    // Implement remaining interface methods...
    
private:
    MyISAState state_;
};
```

### Step 3: Create Factory Function

Provide a factory function that creates instances of your ISA plugin:

```cpp
std::unique_ptr<IISA> createMyISA() {
    return std::make_unique<MyISAPlugin>();
}
```

### Step 4: Register with Plugin Registry

Register your ISA with the plugin registry (typically in a static initializer):

```cpp
namespace {
    struct MyISAAutoRegistrar {
        MyISAAutoRegistrar() {
            ISAPluginRegistry::instance().registerISA("MyISA", 
                [](void* data) -> std::unique_ptr<IISA> {
                    return createMyISA();
                });
        }
    } myISAAutoRegistrar;
}
```

## Using ISA Plugins

### Creating a VM with Specific ISA

```cpp
// Create VM with IA-64 (default)
VirtualMachine vm1(16 * 1024 * 1024, 1, "IA-64");

// Create VM with alternative ISA
VirtualMachine vm2(16 * 1024 * 1024, 1, "x86-64");

// All VMs share the same memory and device infrastructure
```

### Listing Available ISAs

```cpp
auto isas = ISAPluginRegistry::instance().getRegisteredISAs();
for (const auto& isaName : isas) {
    std::cout << "Available ISA: " << isaName << "\n";
}
```

### Creating ISA Instances Directly

```cpp
// Using registry
auto isa = ISAPluginRegistry::instance().createISA("IA-64", &factoryData);

// Using factory function directly
auto isa = createIA64ISA(decoder, syscallDispatcher, profiler);
```

## ISA Plugin Lifecycle

1. **Registration**: ISA plugin registers with `ISAPluginRegistry` on startup
2. **Creation**: VM creates ISA instance via registry factory function
3. **Initialization**: `reset()` called to initialize architectural state
4. **Execution Loop**: 
   - `decode()` fetches and decodes instruction
   - `execute()` performs instruction execution
   - State updated, PC advanced
5. **Checkpointing**: `serialize_state()` captures state for VM snapshots
6. **Destruction**: ISA instance cleaned up when VM is destroyed

## Shared Resources

### Memory System

All ISAs share the same `IMemory` interface:

```cpp
ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& result) {
    // Read from memory
    uint64_t value = memory.read<uint64_t>(address);
    
    // Write to memory
    memory.write<uint32_t>(address, data);
    
    // Bulk operations
    memory.Read(address, buffer, size);
    memory.Write(address, buffer, size);
}
```

### Device Models

Devices (console, timer, interrupts) are memory-mapped and accessible to all ISAs:

```cpp
// Console at 0xFFFFF000
// Timer at 0xFFFFF100
// Interrupt controller (via VM callback)
```

## Design Patterns

### Decode-Execute Separation

ISA plugins separate decoding and execution for flexibility:

```cpp
// Decode phase (can be used for disassembly, analysis)
ISADecodeResult decodeResult = isa->decode(memory);

// Execute phase (performs actual state changes)
ISAExecutionResult execResult = isa->execute(memory, decodeResult);
```

### State Serialization

ISA state can be serialized for checkpointing and migration:

```cpp
// Capture state
std::vector<uint8_t> snapshot = isa->serialize_state();

// Restore state
isa->deserialize_state(snapshot);
```

### Feature Detection

ISAs can advertise capabilities via `hasFeature()`:

```cpp
if (isa->hasFeature("predication")) {
    // ISA supports predicated execution
}

if (isa->hasFeature("simd")) {
    // ISA has SIMD instructions
}
```

## Example: IA-64 Plugin

The IA-64 ISA plugin demonstrates the architecture:

```cpp
class IA64ISAPlugin : public IISA {
public:
    IA64ISAPlugin(IDecoder& decoder);
    
    std::string getName() const override { return "IA-64"; }
    
    ISADecodeResult decode(IMemory& memory) override {
        // Fetch 128-bit bundle from memory
        // Decode 3 instruction slots
        // Return decoded instruction
    }
    
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& result) override {
        // Execute IA-64 instruction
        // Handle predication, register rotation
        // Advance IP by slot or bundle
    }
    
    // IA-64 specific features
    bool hasFeature(const std::string& feature) const override {
        if (feature == "predication") return true;
        if (feature == "register_rotation") return true;
        if (feature == "bundles") return true;
        return false;
    }
};
```

## Example: Simple RISC Plugin

The Example ISA demonstrates a minimal plugin:

```cpp
class ExampleISAPlugin : public IISA {
    // 32-bit fixed-width instructions
    // Load/store architecture
    // 32 general-purpose registers
    
    ISADecodeResult decode(IMemory& memory) override {
        // Fetch 32-bit instruction
        uint32_t instr = memory.read<uint32_t>(state_.programCounter);
        // Decode opcode, registers
        // Return result
    }
    
    ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& result) override {
        // Execute ADD, SUB, LOAD, STORE, HALT
        // Update registers
        // Advance PC by 4 bytes
    }
};
```

## Best Practices

### 1. Stateless Decoding

Keep decode logic stateless - only parse instruction bytes, don't modify architectural state:

```cpp
ISADecodeResult decode(IMemory& memory) {
    // Good: Parse instruction without side effects
    uint32_t instr = memory.read<uint32_t>(pc);
    return parseInstruction(instr);
    
    // Bad: Don't modify state during decode
    // pc += 4;  // NO!
}
```

### 2. Exception Safety

Handle exceptions gracefully to prevent VM crashes:

```cpp
ISAExecutionResult execute(IMemory& memory, const ISADecodeResult& result) {
    try {
        executeInstruction(result);
        return ISAExecutionResult::CONTINUE;
    } catch (const std::exception& e) {
        std::cerr << "Execution error: " << e.what() << "\n";
        return ISAExecutionResult::EXCEPTION;
    }
}
```

### 3. Efficient State Serialization

Implement efficient serialization for large states:

```cpp
void serialize(uint8_t* buffer) const {
    size_t offset = 0;
    
    // Serialize in chunks
    std::memcpy(buffer + offset, registers, sizeof(registers));
    offset += sizeof(registers);
    
    std::memcpy(buffer + offset, &pc, sizeof(pc));
    offset += sizeof(pc);
}
```

### 4. Descriptive Disassembly

Provide clear, readable disassembly:

```cpp
std::string disassemble(IMemory& memory, uint64_t address) {
    auto instr = decodeAtAddress(memory, address);
    
    // Good: "add r1, r2, r3"
    // Better: "add r1 = r2 + r3  ; r1=0x1234"
    
    return formatInstruction(instr);
}
```

## Advanced Features

### Multi-ISA VMs (Future)

Support running multiple ISAs simultaneously:

```cpp
VirtualMachine vm(memSize, 4);  // 4 CPUs
vm.setCPUIsa(0, "IA-64");       // CPU 0: IA-64
vm.setCPUIsa(1, "IA-64");       // CPU 1: IA-64
vm.setCPUIsa(2, "x86-64");      // CPU 2: x86-64
vm.setCPUIsa(3, "ARM64");       // CPU 3: ARM64
```

### JIT Compilation Hooks

ISA plugins can provide JIT compilation hints:

```cpp
class JITEnabledISA : public IISA {
    virtual void* getJITContext() { return jitContext_; }
    virtual bool canJIT(const ISADecodeResult& result) { return true; }
};
```

### Hardware Acceleration

ISA plugins can delegate to hardware-assisted virtualization:

```cpp
class HardwareAcceleratedISA : public IISA {
    bool supportsHardwareAcceleration() const { return true; }
    ISAExecutionResult executeNative(IMemory& memory, ...) { ... }
};
```

## Troubleshooting

### ISA Not Found

```
Error: ISA 'MyISA' is not registered
```

**Solution**: Ensure your ISA is registered with the plugin registry before VM creation.

### State Serialization Mismatch

```
Error: Deserialization failed - size mismatch
```

**Solution**: Verify `getStateSize()` returns the correct size and serialization is consistent.

### Memory Access Violations

```
Exception: Memory access out of bounds at 0x12345678
```

**Solution**: Validate addresses before memory access, handle exceptions in execute().

## Performance Considerations

- **Decode Caching**: Cache decoded instructions to avoid re-decoding
- **Hot Path Optimization**: Optimize common instruction execution paths
- **State Layout**: Organize state for cache-friendly access
- **Minimal Indirection**: Reduce virtual function calls in hot loops

## Summary

The ISA Plugin Architecture provides a flexible, extensible framework for supporting multiple instruction set architectures in the same VM. Key benefits:

? **Modularity**: ISA-specific code isolated in plugins  
? **Reusability**: Shared memory and devices across ISAs  
? **Extensibility**: Add new ISAs without modifying core VM  
? **Flexibility**: Mix different ISAs in multi-CPU VMs  
? **Maintainability**: Clean separation of concerns  

The architecture demonstrates how to build a plugin system for complex emulation while maintaining performance and code quality.

# VM Boot State Machine

## Overview

The VM Boot State Machine provides a structured, validated approach to managing virtual machine boot lifecycle. It implements explicit boot phases with controlled transitions, ensuring that the VM progresses through well-defined states from power-on through shutdown.

## Architecture

### Boot States

The boot state machine defines 17 explicit states organized into five categories:

#### Pre-Boot States
- **POWERED_OFF**: VM is powered off (initial state)
- **POWER_ON**: Power button pressed, initializing hardware

#### Boot Sequence
- **FIRMWARE_INIT**: Firmware/BIOS initialization (POST, device enumeration)
- **BOOTLOADER_EXEC**: Bootloader executing (GRUB, U-Boot, etc.)
- **KERNEL_LOAD**: Kernel being loaded into memory
- **KERNEL_ENTRY**: Kernel entry point reached, early initialization
- **KERNEL_INIT**: Kernel subsystems initializing
- **INIT_PROCESS**: Init process (systemd, init) starting
- **USERSPACE_RUNNING**: Fully booted, userspace running

#### Shutdown Sequence
- **SHUTDOWN_INIT**: Shutdown initiated
- **STOPPING_SERVICES**: Stopping services and processes
- **KERNEL_SHUTDOWN**: Kernel cleanup
- **FIRMWARE_SHUTDOWN**: Firmware cleanup
- **POWER_OFF**: Final power off

#### Error States
- **BOOT_FAILED**: Boot process failed
- **KERNEL_PANIC**: Kernel panic during boot or runtime
- **EMERGENCY_HALT**: Emergency halt (critical hardware failure)

### State Transition Graph

```
POWERED_OFF
    ?
POWER_ON
    ?
FIRMWARE_INIT ??? BOOT_FAILED
    ?                  ?
BOOTLOADER_EXEC ????????
    ?
KERNEL_LOAD
    ?
KERNEL_ENTRY ??? KERNEL_PANIC
    ?                  ?
KERNEL_INIT ????????????
    ?                  ?
INIT_PROCESS ???????????
    ?                  ?
USERSPACE_RUNNING      ?
    ?                  ?
SHUTDOWN_INIT          ?
    ?                  ?
STOPPING_SERVICES      ?
    ?                  ?
KERNEL_SHUTDOWN ????????
    ?
FIRMWARE_SHUTDOWN
    ?
POWER_OFF
    ?
POWERED_OFF

Note: EMERGENCY_HALT can be reached from any state
```

## Features

### 1. Validated Transitions

Every state transition is validated against:
- **State Graph**: Only transitions defined in the state graph are allowed
- **Conditions**: Custom validation conditions can be attached to transitions
- **Callbacks**: Pre/post transition callbacks for custom logic

### 2. Transition Conditions

You can add validation conditions to specific transitions:

```cpp
bootSM.addTransitionCondition(
    VMBootState::POWER_ON, 
    VMBootState::FIRMWARE_INIT,
    "hardware_ready",
    "All hardware devices initialized",
    []() {
        return allDevicesInitialized();
    }
);
```

Multiple conditions can be added to a single transition - all must pass for the transition to succeed.

### 3. Event System

#### State Change Callbacks
```cpp
bootSM.onStateChanged([](VMBootState from, VMBootState to, const std::string& reason) {
    LOG_INFO("Boot state: " + bootStateToString(from) + " -> " + bootStateToString(to));
});
```

#### Validation Failure Callbacks
```cpp
bootSM.onValidationFailure([](VMBootState from, VMBootState to, const std::string& condition) {
    LOG_ERROR("Transition failed: " + condition);
});
```

#### Enter/Exit State Callbacks
```cpp
bootSM.onEnterState(VMBootState::KERNEL_ENTRY, []() {
    LOG_INFO("Kernel entry point reached");
});

bootSM.onExitState(VMBootState::FIRMWARE_INIT, []() {
    LOG_INFO("Firmware initialization complete");
});
```

### 4. History and Diagnostics

- **Transition History**: Records all state transitions with timestamps
- **Time Tracking**: Tracks time spent in each state
- **Boot Time Metrics**: Measures total boot time from POWER_ON to USERSPACE_RUNNING
- **Diagnostic Reports**: Comprehensive diagnostic information

## Usage

### Basic Boot Sequence

```cpp
VMBootStateMachine bootSM;

// Power on the VM
if (!bootSM.powerOn()) {
    // Handle power-on failure
    return;
}

// Progress through boot phases
bootSM.transition(VMBootState::FIRMWARE_INIT, "Starting POST");
bootSM.transition(VMBootState::BOOTLOADER_EXEC, "Bootloader starting");
bootSM.transition(VMBootState::KERNEL_LOAD, "Loading kernel");
bootSM.transition(VMBootState::KERNEL_ENTRY, "Jumping to kernel");
bootSM.transition(VMBootState::KERNEL_INIT, "Kernel initializing");
bootSM.transition(VMBootState::INIT_PROCESS, "Starting init");
bootSM.transition(VMBootState::USERSPACE_RUNNING, "Boot complete");

// Check if boot is complete
if (bootSM.isBootComplete()) {
    std::cout << "Boot time: " << bootSM.getTotalBootTime() << " ms\n";
}
```

### With Validation Conditions

```cpp
VMBootStateMachine bootSM;

// Add hardware initialization condition
bool hardwareReady = false;
bootSM.addTransitionCondition(
    VMBootState::POWER_ON,
    VMBootState::FIRMWARE_INIT,
    "hardware_init",
    "Hardware must be initialized",
    [&hardwareReady]() { return hardwareReady; }
);

// Power on
bootSM.powerOn();

// This will fail until hardware is ready
if (!bootSM.transition(VMBootState::FIRMWARE_INIT)) {
    std::cout << "Waiting for hardware...\n";
}

// Initialize hardware
initializeHardware();
hardwareReady = true;

// Now transition succeeds
bootSM.transition(VMBootState::FIRMWARE_INIT);
```

### Shutdown Sequence

```cpp
// Initiate shutdown from running state
if (bootSM.shutdown()) {
    bootSM.transition(VMBootState::STOPPING_SERVICES);
    bootSM.transition(VMBootState::KERNEL_SHUTDOWN);
    bootSM.transition(VMBootState::FIRMWARE_SHUTDOWN);
    bootSM.transition(VMBootState::POWER_OFF);
    bootSM.transition(VMBootState::POWERED_OFF);
}
```

### Error Handling

```cpp
// Boot failure
if (bootloaderFailed()) {
    bootSM.transition(VMBootState::BOOT_FAILED, "Bootloader error");
}

// Kernel panic
if (kernelPanic()) {
    bootSM.transition(VMBootState::KERNEL_PANIC, "Kernel panic");
}

// Emergency halt
if (criticalHardwareFailure()) {
    bootSM.forceTransition(VMBootState::EMERGENCY_HALT, "Critical failure");
}
```

## Integration with VirtualMachine

The boot state machine is integrated into the `VirtualMachine` class:

### Automatic Boot State Transitions

```cpp
VirtualMachine vm(memorySize, cpuCount);

// init() advances through POWER_ON -> FIRMWARE_INIT
vm.init();

// loadProgram() advances to BOOTLOADER_EXEC -> KERNEL_LOAD
vm.loadProgram(kernelData, kernelSize, loadAddr);

// setEntryPoint() advances to KERNEL_ENTRY
vm.setEntryPoint(entryPoint);

// Manual transitions for kernel phases
vm.getBootStateMachine().transition(VMBootState::KERNEL_INIT);
vm.getBootStateMachine().transition(VMBootState::INIT_PROCESS);
vm.getBootStateMachine().transition(VMBootState::USERSPACE_RUNNING);

// Check boot state
if (vm.isBootComplete()) {
    std::cout << "VM boot time: " << vm.getBootTime() << " ms\n";
}
```

### Boot Diagnostics

```cpp
// Get diagnostic information
std::string diagnostics = vm.getBootDiagnostics();
std::cout << diagnostics;

// Output:
// === VM Boot State Machine Diagnostics ===
// Current State: USERSPACE_RUNNING
// Previous State: INIT_PROCESS
// Time in Current State: 1523 ms
// Power On Time: 1234567890 ms
// Boot Complete Time: 1234568123 ms
// Total Boot Time: 233 ms
//
// === State Time Summary ===
// POWER_ON: 12 ms
// FIRMWARE_INIT: 45 ms
// BOOTLOADER_EXEC: 23 ms
// ...
```

## Advanced Features

### Conditional Transitions

Add complex validation logic:

```cpp
bootSM.addTransitionCondition(
    VMBootState::KERNEL_LOAD,
    VMBootState::KERNEL_ENTRY,
    "kernel_valid",
    "Kernel signature must be valid",
    [&]() {
        return validateKernelSignature(kernelImage);
    }
);

bootSM.addTransitionCondition(
    VMBootState::KERNEL_LOAD,
    VMBootState::KERNEL_ENTRY,
    "memory_sufficient",
    "Sufficient memory for kernel",
    [&]() {
        return availableMemory >= requiredMemory;
    }
);
```

### Monitoring Boot Progress

```cpp
VMBootStateMachine bootSM;

// Track boot progress
std::map<VMBootState, std::string> stateDescriptions = {
    {VMBootState::POWER_ON, "Powering on..."},
    {VMBootState::FIRMWARE_INIT, "Initializing firmware..."},
    {VMBootState::BOOTLOADER_EXEC, "Running bootloader..."},
    // ... more states
};

bootSM.onStateChanged([&](VMBootState from, VMBootState to, const std::string& reason) {
    auto it = stateDescriptions.find(to);
    if (it != stateDescriptions.end()) {
        updateBootProgress(it->second);
    }
});
```

### Boot Time Optimization

```cpp
// Measure time in each boot phase
for (auto state : {
    VMBootState::FIRMWARE_INIT,
    VMBootState::BOOTLOADER_EXEC,
    VMBootState::KERNEL_LOAD,
    VMBootState::KERNEL_ENTRY,
    VMBootState::KERNEL_INIT
}) {
    uint64_t timeInState = bootSM.getTimeInState(state);
    std::cout << bootStateToString(state) << ": " << timeInState << " ms\n";
}

// Identify slowest boot phase
```

## API Reference

### Core Methods

| Method | Description |
|--------|-------------|
| `powerOn()` | Initiate power-on sequence |
| `shutdown()` | Initiate shutdown sequence |
| `transition(state, reason)` | Attempt validated transition |
| `forceTransition(state, reason)` | Force transition (bypasses validation) |
| `reset()` | Reset to POWERED_OFF state |

### State Query

| Method | Description |
|--------|-------------|
| `getCurrentState()` | Get current boot state |
| `getPreviousState()` | Get previous boot state |
| `isBootComplete()` | Check if fully booted |
| `isPoweredOff()` | Check if powered off |
| `isInErrorState()` | Check if in error state |
| `getTimeInCurrentState()` | Time in current state (ms) |

### Validation

| Method | Description |
|--------|-------------|
| `addTransitionCondition()` | Add validation condition |
| `removeTransitionCondition()` | Remove validation condition |
| `canTransition()` | Check if transition is valid |

### Events

| Method | Description |
|--------|-------------|
| `onStateChanged(callback)` | Register state change callback |
| `onValidationFailure(callback)` | Register validation failure callback |
| `onEnterState(state, callback)` | Register enter state callback |
| `onExitState(state, callback)` | Register exit state callback |

### Diagnostics

| Method | Description |
|--------|-------------|
| `getTransitionHistory()` | Get all transitions |
| `getLastTransition()` | Get most recent transition |
| `getTotalBootTime()` | Get boot time (ms) |
| `getTimeInState(state)` | Get time in specific state |
| `getDiagnostics()` | Get diagnostic report |

## Best Practices

### 1. Use Validated Transitions

Always use `transition()` rather than `forceTransition()` except in emergency recovery scenarios.

### 2. Add Appropriate Conditions

Validate critical transitions:
```cpp
// Validate kernel before entry
bootSM.addTransitionCondition(
    VMBootState::KERNEL_LOAD,
    VMBootState::KERNEL_ENTRY,
    "kernel_checksum",
    "Kernel checksum must be valid",
    [&]() { return validateKernelChecksum(); }
);
```

### 3. Monitor Boot Progress

Use callbacks to track boot progress:
```cpp
bootSM.onEnterState(VMBootState::FIRMWARE_INIT, []() {
    updateBootStatus("Firmware Initializing...");
});
```

### 4. Handle Errors Gracefully

```cpp
if (!bootSM.transition(VMBootState::FIRMWARE_INIT)) {
    // Transition failed
    std::string failedCondition;
    if (!bootSM.canTransition(
        bootSM.getCurrentState(),
        VMBootState::FIRMWARE_INIT,
        &failedCondition
    )) {
        LOG_ERROR("Boot failed: " + failedCondition);
        bootSM.transition(VMBootState::BOOT_FAILED, failedCondition);
    }
}
```

### 5. Use Diagnostics for Debugging

```cpp
// On boot failure
if (bootSM.isInErrorState()) {
    std::string diagnostics = bootSM.getDiagnostics();
    LOG_ERROR("Boot diagnostics:\n" + diagnostics);
    
    // Save for analysis
    saveDiagnostics("boot-failure.log", diagnostics);
}
```

## See Also

- `include/VMBootStateMachine.h` - Full API documentation
- `tests/test_boot_state_machine.cpp` - Comprehensive test examples
- `docs/VM_LIFECYCLE.md` - VM lifecycle management
- `docs/VMMANAGER.md` - VM Manager documentation

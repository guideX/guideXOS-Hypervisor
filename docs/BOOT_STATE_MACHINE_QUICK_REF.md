# VM Boot State Machine - Quick Reference

## Boot States

### Pre-Boot
- `POWERED_OFF` - Initial state
- `POWER_ON` - Hardware initializing

### Boot Sequence
- `FIRMWARE_INIT` - BIOS/UEFI POST
- `BOOTLOADER_EXEC` - Bootloader running
- `KERNEL_LOAD` - Loading kernel
- `KERNEL_ENTRY` - Kernel entry point
- `KERNEL_INIT` - Kernel subsystems init
- `INIT_PROCESS` - Init/systemd starting
- `USERSPACE_RUNNING` - Fully booted ?

### Shutdown
- `SHUTDOWN_INIT` - Shutdown starting
- `STOPPING_SERVICES` - Services stopping
- `KERNEL_SHUTDOWN` - Kernel cleanup
- `FIRMWARE_SHUTDOWN` - Firmware cleanup
- `POWER_OFF` - Powering off

### Errors
- `BOOT_FAILED` - Boot failed
- `KERNEL_PANIC` - Kernel panic
- `EMERGENCY_HALT` - Critical failure

## Common Operations

### Power On
```cpp
VMBootStateMachine bootSM;
bootSM.powerOn();  // POWERED_OFF ? POWER_ON
```

### Complete Boot
```cpp
bootSM.powerOn();
bootSM.transition(VMBootState::FIRMWARE_INIT);
bootSM.transition(VMBootState::BOOTLOADER_EXEC);
bootSM.transition(VMBootState::KERNEL_LOAD);
bootSM.transition(VMBootState::KERNEL_ENTRY);
bootSM.transition(VMBootState::KERNEL_INIT);
bootSM.transition(VMBootState::INIT_PROCESS);
bootSM.transition(VMBootState::USERSPACE_RUNNING);
```

### Shutdown
```cpp
bootSM.shutdown();  // From USERSPACE_RUNNING ? SHUTDOWN_INIT
bootSM.transition(VMBootState::STOPPING_SERVICES);
bootSM.transition(VMBootState::KERNEL_SHUTDOWN);
bootSM.transition(VMBootState::FIRMWARE_SHUTDOWN);
bootSM.transition(VMBootState::POWER_OFF);
bootSM.transition(VMBootState::POWERED_OFF);
```

### Check State
```cpp
if (bootSM.isBootComplete()) { /* ... */ }
if (bootSM.isPoweredOff()) { /* ... */ }
if (bootSM.isInErrorState()) { /* ... */ }
```

## Validation

### Add Condition
```cpp
bootSM.addTransitionCondition(
    VMBootState::POWER_ON,
    VMBootState::FIRMWARE_INIT,
    "hardware_ready",
    "Hardware initialized",
    []() { return checkHardware(); }
);
```

### Remove Condition
```cpp
bootSM.removeTransitionCondition(
    VMBootState::POWER_ON,
    VMBootState::FIRMWARE_INIT,
    "hardware_ready"
);
```

### Test Transition
```cpp
std::string failed;
if (bootSM.canTransition(from, to, &failed)) {
    // Transition would succeed
} else {
    // Would fail: failed contains reason
}
```

## Events

### State Change
```cpp
bootSM.onStateChanged([](VMBootState from, VMBootState to, const std::string& reason) {
    std::cout << bootStateToString(to) << "\n";
});
```

### Validation Failure
```cpp
bootSM.onValidationFailure([](VMBootState from, VMBootState to, const std::string& condition) {
    std::cerr << "Failed: " << condition << "\n";
});
```

### Enter/Exit State
```cpp
bootSM.onEnterState(VMBootState::KERNEL_ENTRY, []() {
    std::cout << "Kernel starting\n";
});

bootSM.onExitState(VMBootState::FIRMWARE_INIT, []() {
    std::cout << "Firmware complete\n";
});
```

## Diagnostics

### Boot Time
```cpp
uint64_t bootTime = bootSM.getTotalBootTime();  // milliseconds
```

### Time Per State
```cpp
uint64_t firmwareTime = bootSM.getTimeInState(VMBootState::FIRMWARE_INIT);
```

### History
```cpp
auto history = bootSM.getTransitionHistory();
for (const auto& trans : history) {
    std::cout << bootStateToString(trans.fromState)
              << " -> " << bootStateToString(trans.toState)
              << (trans.successful ? " ?" : " ?") << "\n";
}
```

### Full Diagnostics
```cpp
std::string diag = bootSM.getDiagnostics();
std::cout << diag;
```

## VirtualMachine Integration

### Automatic Transitions
```cpp
VirtualMachine vm(memSize, cpuCount);

vm.init();                    // ? POWER_ON ? FIRMWARE_INIT
vm.loadProgram(k, sz, addr);  // ? BOOTLOADER_EXEC ? KERNEL_LOAD
vm.setEntryPoint(entry);      // ? KERNEL_ENTRY

// Manual transitions
vm.getBootStateMachine().transition(VMBootState::KERNEL_INIT);
vm.getBootStateMachine().transition(VMBootState::INIT_PROCESS);
vm.getBootStateMachine().transition(VMBootState::USERSPACE_RUNNING);
```

### Query Boot State
```cpp
VMBootState state = vm.getBootState();
bool booted = vm.isBootComplete();
uint64_t time = vm.getBootTime();
std::string diag = vm.getBootDiagnostics();
```

## Error Handling

### Boot Failure
```cpp
if (errorDuringBoot) {
    bootSM.transition(VMBootState::BOOT_FAILED, "Error details");
}
```

### Kernel Panic
```cpp
if (kernelPanic) {
    bootSM.transition(VMBootState::KERNEL_PANIC, "Panic reason");
}
```

### Emergency Halt
```cpp
if (criticalError) {
    bootSM.forceTransition(VMBootState::EMERGENCY_HALT, "Critical failure");
}
```

### Check for Errors
```cpp
if (bootSM.isInErrorState()) {
    VMBootState state = bootSM.getCurrentState();
    // Handle based on error type
}
```

## Reset

```cpp
bootSM.reset();  // Return to POWERED_OFF
```

## State Strings

```cpp
std::string stateStr = bootStateToString(VMBootState::KERNEL_ENTRY);
// Returns: "KERNEL_ENTRY"
```

## Valid Transitions

| From | To |
|------|-----|
| POWERED_OFF | POWER_ON |
| POWER_ON | FIRMWARE_INIT, BOOT_FAILED, EMERGENCY_HALT |
| FIRMWARE_INIT | BOOTLOADER_EXEC, BOOT_FAILED, EMERGENCY_HALT |
| BOOTLOADER_EXEC | KERNEL_LOAD, BOOT_FAILED, EMERGENCY_HALT |
| KERNEL_LOAD | KERNEL_ENTRY, BOOT_FAILED, EMERGENCY_HALT |
| KERNEL_ENTRY | KERNEL_INIT, KERNEL_PANIC, EMERGENCY_HALT |
| KERNEL_INIT | INIT_PROCESS, KERNEL_PANIC, EMERGENCY_HALT |
| INIT_PROCESS | USERSPACE_RUNNING, KERNEL_PANIC, EMERGENCY_HALT |
| USERSPACE_RUNNING | SHUTDOWN_INIT, KERNEL_PANIC, EMERGENCY_HALT |
| SHUTDOWN_INIT | STOPPING_SERVICES, EMERGENCY_HALT |
| STOPPING_SERVICES | KERNEL_SHUTDOWN, EMERGENCY_HALT |
| KERNEL_SHUTDOWN | FIRMWARE_SHUTDOWN, EMERGENCY_HALT |
| FIRMWARE_SHUTDOWN | POWER_OFF |
| POWER_OFF | POWERED_OFF |
| BOOT_FAILED | POWER_OFF, POWERED_OFF |
| KERNEL_PANIC | EMERGENCY_HALT, POWER_OFF |
| EMERGENCY_HALT | POWERED_OFF |

## Files

- **Header**: `include/VMBootStateMachine.h`
- **Implementation**: `src/vm/VMBootStateMachine.cpp`
- **Tests**: `tests/test_boot_state_machine.cpp`
- **Docs**: `docs/BOOT_STATE_MACHINE.md`
- **Summary**: `docs/BOOT_STATE_MACHINE_SUMMARY.md`

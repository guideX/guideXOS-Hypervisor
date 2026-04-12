# IA-64 Instruction Fuzzer

A comprehensive fuzzing system for generating and testing random valid IA-64 instruction bundles to detect crashes, NaT violations, and undefined behavior.

## Features

- **Random Bundle Generation**: Creates valid or invalid IA-64 instruction bundles with configurable parameters
- **Violation Detection**: Detects crashes, NaT violations, memory violations, timeouts, and infinite loops
- **Mutation Strategies**: Implements bit-flip, byte-flip, arithmetic, and interesting value mutations
- **Corpus Management**: Saves interesting test cases for future analysis and regression testing
- **Template Coverage**: Tracks which instruction templates have been tested
- **Performance Monitoring**: Measures execution time, instruction count, and cycle count

## Quick Start

### Basic Usage

```cpp
#include "Fuzzer.h"

// Create default configuration
ia64::FuzzConfig config;
config.maxInstructions = 10000;
config.timeout = std::chrono::milliseconds(1000);

// Create fuzzer and run campaign
ia64::InstructionFuzzer fuzzer(config);
fuzzer.run(1000);  // Run 1000 tests

// Get statistics
const ia64::FuzzStatistics& stats = fuzzer.getStatistics();
std::cout << stats.generateReport();
```

### Advanced Configuration

```cpp
ia64::FuzzConfig config;

// Execution limits
config.maxInstructions = 5000;
config.maxCycles = 50000;
config.timeout = std::chrono::milliseconds(500);

// Generation parameters
config.generateValidBundles = true;      // Only syntactically valid bundles
config.useValidTemplates = true;         // Only valid template types
config.randomizeRegisters = true;        // Random register operands
config.randomizeImmediates = true;       // Random immediate values
config.allowPrivilegedInstructions = false;
config.allowFloatingPoint = true;
config.allowBranches = true;

// Memory configuration
config.memorySize = 1024 * 1024;        // 1 MB
config.stackBase = 0x7FFFF000;
config.stackSize = 64 * 1024;           // 64 KB

// Corpus management
config.saveInterestingCases = true;
config.corpusDirectory = "fuzz_corpus";

// Reproducibility
config.seed = 12345;  // Fixed seed for reproducible fuzzing
```

### Testing Specific Bundles

```cpp
ia64::InstructionFuzzer fuzzer(config);

// Create a specific bundle to test
std::vector<uint8_t> bundle = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Test it
ia64::FuzzResult result = fuzzer.testBundle(bundle);

if (!result.passed) {
    std::cout << "Violation: " << ia64::FuzzUtils::violationTypeToString(result.violation) << "\n";
    std::cout << "Error: " << result.errorMessage << "\n";
    std::cout << "Crash IP: 0x" << std::hex << result.crashIP << "\n";
}
```

### Mutation-Based Fuzzing

```cpp
// Start with a known-good bundle
std::vector<uint8_t> original = /* ... */;

// Generate mutations
for (int i = 0; i < 100; ++i) {
    std::vector<uint8_t> mutated = fuzzer.mutateBundle(original);
    ia64::FuzzResult result = fuzzer.testBundle(mutated);
    
    if (!result.passed) {
        // Found an interesting case!
        std::cout << "Found violation in mutation " << i << "\n";
    }
}
```

## Violation Types

The fuzzer can detect the following violation types:

- **NONE**: No violation, execution completed successfully
- **CRASH**: Unhandled exception or segmentation fault
- **NAT_VIOLATION**: Consumption of a NaT (Not a Thing) register value
- **TIMEOUT**: Execution exceeded the configured timeout
- **ILLEGAL_INSTRUCTION**: Invalid opcode or instruction encoding
- **MEMORY_VIOLATION**: Invalid memory access (out of bounds)
- **DIVISION_BY_ZERO**: Arithmetic division by zero
- **STACK_OVERFLOW**: Stack overflow detected
- **INFINITE_LOOP**: Detected infinite loop pattern
- **UNDEFINED_BEHAVIOR**: Other undefined behavior

## Bundle Structure

IA-64 instruction bundles are 128-bit (16-byte) containers:

```
[template:5][slot0:41][slot1:41][slot2:41] = 128 bits
```

- **Template (5 bits)**: Determines execution unit types for each slot
- **Slot 0 (41 bits)**: First instruction
- **Slot 1 (41 bits)**: Second instruction  
- **Slot 2 (41 bits)**: Third instruction

### Valid Templates

- **MII** (0x00): M-unit, I-unit, I-unit
- **MLX** (0x04): M-unit, L-unit, X-unit (64-bit immediates)
- **MMI** (0x08): M-unit, M-unit, I-unit
- **MFI** (0x0C): M-unit, F-unit, I-unit
- **MIB** (0x10): M-unit, I-unit, B-unit (branch)
- **MBB** (0x12): M-unit, B-unit, B-unit
- **BBB** (0x16): B-unit, B-unit, B-unit
- **MMB** (0x18): M-unit, M-unit, B-unit
- **MFB** (0x1C): M-unit, F-unit, B-unit

(Each template also has a STOP variant with LSB set)

## Statistics and Reporting

The fuzzer tracks detailed statistics:

```cpp
const ia64::FuzzStatistics& stats = fuzzer.getStatistics();

std::cout << "Total Tests: " << stats.totalTests << "\n";
std::cout << "Passed: " << stats.passedTests << "\n";
std::cout << "Failed: " << stats.failedTests << "\n";

// Violation breakdown
for (const auto& entry : stats.violationCounts) {
    std::cout << ia64::FuzzUtils::violationTypeToString(entry.first) 
              << ": " << entry.second << "\n";
}

// Template coverage
for (const auto& entry : stats.templateCoverage) {
    std::cout << "Template 0x" << std::hex << entry.first 
              << ": " << std::dec << entry.second << " times\n";
}

// Interesting cases
std::cout << "Interesting cases: " << stats.interestingCases.size() << "\n";

// Generate full report
std::cout << stats.generateReport();
```

## Utility Functions

### Bundle Conversion

```cpp
// Convert bundle to hex string
std::string hex = ia64::FuzzUtils::bundleToHexString(bundle);

// Parse bundle from hex string
std::vector<uint8_t> bundle = ia64::FuzzUtils::hexStringToBundle(hex);
```

### Bundle Analysis

```cpp
// Validate bundle structure
bool valid = ia64::FuzzUtils::validateBundleStructure(bundle);

// Extract template
ia64::TemplateType tmpl = ia64::FuzzUtils::extractTemplate(bundle);

// Extract instruction slots
uint64_t slot0 = ia64::FuzzUtils::extractSlot(bundle, 0);
uint64_t slot1 = ia64::FuzzUtils::extractSlot(bundle, 1);
uint64_t slot2 = ia64::FuzzUtils::extractSlot(bundle, 2);
```

## Corpus Management

The fuzzer can save interesting test cases to disk for later analysis:

```cpp
// Save corpus after fuzzing
fuzzer.saveCorpus("fuzz_corpus");

// Load existing corpus for regression testing
fuzzer.loadCorpus("fuzz_corpus");
```

Corpus files are saved as binary bundles in the specified directory with names like `bundle_000000.bin`, `bundle_000001.bin`, etc.

## Performance Tuning

### For Maximum Coverage

```cpp
config.maxInstructions = 10000;
config.timeout = std::chrono::milliseconds(2000);
config.generateValidBundles = true;
config.useValidTemplates = true;
// Run many iterations
fuzzer.run(100000);
```

### For Deep Testing

```cpp
config.maxInstructions = 100000;
config.timeout = std::chrono::milliseconds(5000);
config.generateValidBundles = false;  // Include invalid bundles
config.useValidTemplates = false;     // Include invalid templates
fuzzer.run(10000);
```

### For Quick Smoke Testing

```cpp
config.maxInstructions = 1000;
config.timeout = std::chrono::milliseconds(100);
fuzzer.run(100);
```

## Example Test Program

See `tests/test_fuzzer.cpp` for a comprehensive test suite demonstrating all features:

- Basic fuzzing
- Aggressive fuzzing with invalid bundles
- Long-running campaigns
- Specific bundle testing
- Bundle generator validation
- Mutation strategies
- Template coverage analysis
- Performance benchmarking

Run with:

```bash
./test_fuzzer              # Standard tests
./test_fuzzer --long       # Include long-running campaign
```

## Implementation Details

### Random Generation

The fuzzer uses C++'s `std::mt19937_64` Mersenne Twister for high-quality random number generation. You can provide a seed for reproducible fuzzing.

### Timeout Handling

Execution timeout is enforced by checking wall-clock time during the execution loop. If a test exceeds the timeout, it's terminated and marked as a TIMEOUT violation.

### Infinite Loop Detection

The fuzzer tracks the last 100 instruction pointer values. If more than 75% of recent IPs are identical, the test is terminated as an infinite loop.

### Memory Isolation

Each test runs in a fresh VM instance to ensure complete isolation between tests. Memory corruption in one test cannot affect subsequent tests.

## Future Enhancements

Potential improvements:

- NaT bit tracking in register file
- Coverage-guided fuzzing (track code coverage)
- Multi-threaded fuzzing for better performance
- Integration with ASAN/MSAN for memory safety
- Symbolic execution integration
- Differential testing against real IA-64 hardware
- Automatic minimization of failing test cases

## License

Part of the guideXOS Hypervisor project.

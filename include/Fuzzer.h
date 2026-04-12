#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <random>
#include <map>
#include <set>
#include <chrono>
#include "decoder.h"
#include "cpu_state.h"

namespace ia64 {

// Forward declarations
class VirtualMachine;
class IMemory;

/**
 * Types of violations/crashes detected during fuzzing
 */
enum class ViolationType {
    NONE,                       // No violation
    CRASH,                      // Unhandled exception/segfault
    NAT_VIOLATION,              // NaT (Not a Thing) register consumption
    TIMEOUT,                    // Execution timeout exceeded
    ILLEGAL_INSTRUCTION,        // Invalid opcode or encoding
    MEMORY_VIOLATION,           // Invalid memory access (out of bounds)
    DIVISION_BY_ZERO,           // Arithmetic division by zero
    STACK_OVERFLOW,             // Stack overflow detected
    INFINITE_LOOP,              // Detected infinite loop pattern
    UNDEFINED_BEHAVIOR          // Other undefined behavior
};

/**
 * Result of a single fuzz test execution
 */
struct FuzzResult {
    bool passed;                            // Did execution complete without violations?
    ViolationType violation;                // Type of violation if any
    std::string errorMessage;               // Detailed error message
    uint64_t instructionsExecuted;          // Number of instructions executed
    uint64_t cyclesElapsed;                 // Execution cycles
    std::chrono::microseconds executionTime;// Wall-clock time
    std::vector<uint8_t> inputBundle;       // Bundle that caused the issue
    uint64_t crashIP;                       // Instruction pointer at crash
    uint64_t faultAddress;                  // Memory address that caused fault
    
    FuzzResult() 
        : passed(true)
        , violation(ViolationType::NONE)
        , errorMessage("")
        , instructionsExecuted(0)
        , cyclesElapsed(0)
        , executionTime(0)
        , crashIP(0)
        , faultAddress(0) {}
};

/**
 * Fuzzing configuration parameters
 */
struct FuzzConfig {
    // Execution limits
    uint64_t maxInstructions;               // Max instructions per test
    uint64_t maxCycles;                     // Max cycles per test
    std::chrono::milliseconds timeout;      // Wall-clock timeout
    
    // Generation parameters
    bool generateValidBundles;              // Only generate syntactically valid bundles
    bool useValidTemplates;                 // Only use valid template types
    bool randomizeRegisters;                // Randomize register operands
    bool randomizeImmediates;               // Randomize immediate values
    bool allowPrivilegedInstructions;       // Include privileged instructions
    bool allowFloatingPoint;                // Include floating-point instructions
    bool allowBranches;                     // Include branch instructions
    
    // Memory configuration
    uint64_t memorySize;                    // VM memory size in bytes
    uint64_t stackBase;                     // Stack base address
    uint64_t stackSize;                     // Stack size
    uint64_t heapBase;                      // Heap base address
    uint64_t heapSize;                      // Heap size
    
    // Random seed
    uint64_t seed;                          // Random seed (0 = use time)
    
    // Corpus management
    bool saveInterestingCases;              // Save interesting test cases
    std::string corpusDirectory;            // Directory to save corpus
    
    FuzzConfig()
        : maxInstructions(10000)
        , maxCycles(100000)
        , timeout(std::chrono::milliseconds(1000))
        , generateValidBundles(true)
        , useValidTemplates(true)
        , randomizeRegisters(true)
        , randomizeImmediates(true)
        , allowPrivilegedInstructions(false)
        , allowFloatingPoint(true)
        , allowBranches(true)
        , memorySize(1024 * 1024)           // 1 MB
        , stackBase(0x7FFFF000)
        , stackSize(64 * 1024)              // 64 KB
        , heapBase(0x10000)
        , heapSize(256 * 1024)              // 256 KB
        , seed(0)
        , saveInterestingCases(true)
        , corpusDirectory("fuzz_corpus") {}
};

/**
 * Statistics tracking for fuzzing campaign
 */
struct FuzzStatistics {
    uint64_t totalTests;
    uint64_t passedTests;
    uint64_t failedTests;
    std::map<ViolationType, uint64_t> violationCounts;
    std::map<TemplateType, uint64_t> templateCoverage;
    uint64_t totalInstructions;
    uint64_t totalCycles;
    std::chrono::microseconds totalTime;
    std::vector<FuzzResult> interestingCases;
    
    FuzzStatistics()
        : totalTests(0)
        , passedTests(0)
        , failedTests(0)
        , totalInstructions(0)
        , totalCycles(0)
        , totalTime(0) {}
        
    void recordResult(const FuzzResult& result);
    std::string generateReport() const;
};

/**
 * Random instruction bundle generator
 */
class BundleGenerator {
public:
    explicit BundleGenerator(uint64_t seed = 0);
    
    // Generate a random 128-bit bundle
    std::vector<uint8_t> generateBundle(const FuzzConfig& config);
    
    // Generate specific components
    TemplateType generateTemplate(bool validOnly);
    uint64_t generateInstruction(UnitType unit, const FuzzConfig& config);
    
    // Generate operands
    uint8_t generateRegister(bool allowR0 = false);
    uint8_t generatePredicateRegister();
    uint8_t generateBranchRegister();
    uint64_t generateImmediate(size_t bits);
    
    // Validate generated bundles
    bool isValidTemplate(TemplateType tmpl) const;
    bool isValidBundle(const std::vector<uint8_t>& bundle) const;
    
private:
    std::mt19937_64 rng_;
    
    // Template to unit type mapping
    std::vector<UnitType> getUnitsForTemplate(TemplateType tmpl) const;
    
    // Instruction encoding helpers
    uint64_t encodeNop() const;
    uint64_t encodeMov(const FuzzConfig& config);
    uint64_t encodeAdd(const FuzzConfig& config);
    uint64_t encodeLoad(const FuzzConfig& config);
    uint64_t encodeStore(const FuzzConfig& config);
    uint64_t encodeBranch(const FuzzConfig& config);
    
    // Pack 41-bit instruction into bundle
    void packInstruction(std::vector<uint8_t>& bundle, size_t slotIndex, uint64_t instruction);
    
    // Allow InstructionFuzzer to access RNG for mutations
    friend class InstructionFuzzer;
};

/**
 * Main fuzzer class
 */
class InstructionFuzzer {
public:
    explicit InstructionFuzzer(const FuzzConfig& config);
    ~InstructionFuzzer();
    
    // Run fuzzing campaign
    void run(uint64_t numTests);
    
    // Run a single test with given bundle
    FuzzResult testBundle(const std::vector<uint8_t>& bundle);
    
    // Get statistics
    const FuzzStatistics& getStatistics() const { return stats_; }
    
    // Save/load corpus
    void saveCorpus(const std::string& directory);
    void loadCorpus(const std::string& directory);
    
    // Mutation-based fuzzing
    std::vector<uint8_t> mutateBundle(const std::vector<uint8_t>& bundle);
    
private:
    FuzzConfig config_;
    BundleGenerator generator_;
    FuzzStatistics stats_;
    
    // Violation detection
    ViolationType detectViolation(VirtualMachine& vm, uint64_t instructionsExecuted);
    bool checkNaTViolation(const CPUState& state);
    bool checkMemoryViolation(VirtualMachine& vm);
    bool checkInfiniteLoop(const std::vector<uint64_t>& ipHistory);
    
    // Execution helpers
    bool executeWithTimeout(VirtualMachine& vm, 
                           uint64_t& instructionsExecuted,
                           ViolationType& violation);
    
    // Initialize VM with bundle
    void setupVM(VirtualMachine& vm, const std::vector<uint8_t>& bundle);
    
    // Corpus management
    std::set<std::vector<uint8_t>> corpus_;
    void addToCorpus(const std::vector<uint8_t>& bundle);
    bool isInCorpus(const std::vector<uint8_t>& bundle) const;
    
    // Mutation strategies
    void bitFlipMutation(std::vector<uint8_t>& bundle);
    void byteFlipMutation(std::vector<uint8_t>& bundle);
    void arithmeticMutation(std::vector<uint8_t>& bundle);
    void interestingValueMutation(std::vector<uint8_t>& bundle);
};

/**
 * Utility functions for fuzzing
 */
namespace FuzzUtils {
    // Convert violation type to string
    std::string violationTypeToString(ViolationType type);
    
    // Convert bundle to hex string for logging
    std::string bundleToHexString(const std::vector<uint8_t>& bundle);
    
    // Parse bundle from hex string
    std::vector<uint8_t> hexStringToBundle(const std::string& hex);
    
    // Validate bundle structure
    bool validateBundleStructure(const std::vector<uint8_t>& bundle);
    
    // Extract template from bundle
    TemplateType extractTemplate(const std::vector<uint8_t>& bundle);
    
    // Extract instruction slot from bundle
    uint64_t extractSlot(const std::vector<uint8_t>& bundle, size_t slotIndex);
}

} // namespace ia64

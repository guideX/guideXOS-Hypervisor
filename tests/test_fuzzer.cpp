#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include "../include/Fuzzer.h"
#include "../include/VirtualMachine.h"

using namespace ia64;

/**
 * Test 1: Basic fuzzing with default configuration
 */
void testBasicFuzzing() {
    std::cout << "\n========================================\n";
    std::cout << "Test 1: Basic Fuzzing (100 tests)\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.maxInstructions = 1000;
    config.timeout = std::chrono::milliseconds(100);
    config.generateValidBundles = true;
    config.useValidTemplates = true;
    config.saveInterestingCases = true;
    config.corpusDirectory = "fuzz_corpus_basic";
    
    InstructionFuzzer fuzzer(config);
    fuzzer.run(100);
    
    const FuzzStatistics& stats = fuzzer.getStatistics();
    std::cout << "\nTest 1 Summary:\n";
    std::cout << "  Total: " << stats.totalTests << "\n";
    std::cout << "  Passed: " << stats.passedTests << "\n";
    std::cout << "  Failed: " << stats.failedTests << "\n";
}

/**
 * Test 2: Aggressive fuzzing with invalid bundles
 */
void testAggressiveFuzzing() {
    std::cout << "\n========================================\n";
    std::cout << "Test 2: Aggressive Fuzzing (50 tests)\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.maxInstructions = 500;
    config.timeout = std::chrono::milliseconds(50);
    config.generateValidBundles = false;  // Allow invalid bundles
    config.useValidTemplates = false;     // Allow invalid templates
    config.saveInterestingCases = true;
    config.corpusDirectory = "fuzz_corpus_aggressive";
    
    InstructionFuzzer fuzzer(config);
    fuzzer.run(50);
    
    const FuzzStatistics& stats = fuzzer.getStatistics();
    std::cout << "\nTest 2 Summary:\n";
    std::cout << "  Total: " << stats.totalTests << "\n";
    std::cout << "  Passed: " << stats.passedTests << "\n";
    std::cout << "  Failed: " << stats.failedTests << "\n";
}

/**
 * Test 3: Long-running fuzzing campaign
 */
void testLongRunningFuzzing() {
    std::cout << "\n========================================\n";
    std::cout << "Test 3: Long-Running Campaign (1000 tests)\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.maxInstructions = 5000;
    config.timeout = std::chrono::milliseconds(500);
    config.generateValidBundles = true;
    config.useValidTemplates = true;
    config.allowBranches = true;
    config.allowFloatingPoint = true;
    config.saveInterestingCases = true;
    config.corpusDirectory = "fuzz_corpus_long";
    config.seed = 12345;  // Reproducible seed
    
    InstructionFuzzer fuzzer(config);
    fuzzer.run(1000);
    
    const FuzzStatistics& stats = fuzzer.getStatistics();
    std::cout << "\nTest 3 Summary:\n";
    std::cout << "  Total: " << stats.totalTests << "\n";
    std::cout << "  Passed: " << stats.passedTests << "\n";
    std::cout << "  Failed: " << stats.failedTests << "\n";
    std::cout << "  Interesting cases found: " << stats.interestingCases.size() << "\n";
}

/**
 * Test 4: Specific bundle validation
 */
void testSpecificBundles() {
    std::cout << "\n========================================\n";
    std::cout << "Test 4: Specific Bundle Testing\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.maxInstructions = 100;
    config.timeout = std::chrono::milliseconds(50);
    
    InstructionFuzzer fuzzer(config);
    
    // Test case 1: All zeros (NOPs)
    std::vector<uint8_t> nopBundle = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    std::cout << "\nTest Case 1: All NOPs\n";
    std::cout << "Bundle: " << FuzzUtils::bundleToHexString(nopBundle) << "\n";
    FuzzResult result1 = fuzzer.testBundle(nopBundle);
    std::cout << "Result: " << (result1.passed ? "PASSED" : "FAILED") << "\n";
    if (!result1.passed) {
        std::cout << "Violation: " << FuzzUtils::violationTypeToString(result1.violation) << "\n";
        std::cout << "Error: " << result1.errorMessage << "\n";
    }
    std::cout << "Instructions executed: " << result1.instructionsExecuted << "\n";
    
    // Test case 2: All ones (likely invalid)
    std::vector<uint8_t> onesBundle = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
    };
    
    std::cout << "\nTest Case 2: All Ones\n";
    std::cout << "Bundle: " << FuzzUtils::bundleToHexString(onesBundle) << "\n";
    FuzzResult result2 = fuzzer.testBundle(onesBundle);
    std::cout << "Result: " << (result2.passed ? "PASSED" : "FAILED") << "\n";
    if (!result2.passed) {
        std::cout << "Violation: " << FuzzUtils::violationTypeToString(result2.violation) << "\n";
        std::cout << "Error: " << result2.errorMessage << "\n";
    }
    std::cout << "Instructions executed: " << result2.instructionsExecuted << "\n";
    
    // Test case 3: Valid MII template with random data
    std::vector<uint8_t> mixedBundle = {
        0x00, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE,
        0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77
    };
    
    std::cout << "\nTest Case 3: MII Template with Random Data\n";
    std::cout << "Bundle: " << FuzzUtils::bundleToHexString(mixedBundle) << "\n";
    FuzzResult result3 = fuzzer.testBundle(mixedBundle);
    std::cout << "Result: " << (result3.passed ? "PASSED" : "FAILED") << "\n";
    if (!result3.passed) {
        std::cout << "Violation: " << FuzzUtils::violationTypeToString(result3.violation) << "\n";
        std::cout << "Error: " << result3.errorMessage << "\n";
    }
    std::cout << "Instructions executed: " << result3.instructionsExecuted << "\n";
}

/**
 * Test 5: Bundle generator validation
 */
void testBundleGenerator() {
    std::cout << "\n========================================\n";
    std::cout << "Test 5: Bundle Generator Validation\n";
    std::cout << "========================================\n";
    
    BundleGenerator generator(42);  // Fixed seed for reproducibility
    FuzzConfig config;
    
    std::cout << "\nGenerating 10 valid bundles:\n";
    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> bundle = generator.generateBundle(config);
        
        TemplateType tmpl = FuzzUtils::extractTemplate(bundle);
        bool valid = generator.isValidBundle(bundle);
        
        std::cout << "Bundle " << (i + 1) << ": ";
        std::cout << FuzzUtils::bundleToHexString(bundle) << "\n";
        std::cout << "  Template: 0x" << std::hex << static_cast<int>(tmpl) << std::dec;
        std::cout << " (" << (valid ? "VALID" : "INVALID") << ")\n";
        
        // Extract and display instructions
        for (size_t slot = 0; slot < 3; ++slot) {
            uint64_t instruction = FuzzUtils::extractSlot(bundle, slot);
            std::cout << "  Slot " << slot << ": 0x" << std::hex 
                     << std::setw(11) << std::setfill('0') << instruction << std::dec << "\n";
        }
    }
}

/**
 * Test 6: Mutation strategies
 */
void testMutationStrategies() {
    std::cout << "\n========================================\n";
    std::cout << "Test 6: Mutation Strategies\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.seed = 999;
    InstructionFuzzer fuzzer(config);
    
    // Original bundle
    std::vector<uint8_t> original = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    
    std::cout << "Original: " << FuzzUtils::bundleToHexString(original) << "\n\n";
    
    // Generate mutations
    for (int i = 0; i < 5; ++i) {
        std::vector<uint8_t> mutated = fuzzer.mutateBundle(original);
        std::cout << "Mutation " << (i + 1) << ": " 
                 << FuzzUtils::bundleToHexString(mutated) << "\n";
        
        // Count differences
        int diffCount = 0;
        for (size_t j = 0; j < original.size(); ++j) {
            if (original[j] != mutated[j]) {
                diffCount++;
            }
        }
        std::cout << "  Bytes changed: " << diffCount << "\n";
    }
}

/**
 * Test 7: Template coverage analysis
 */
void testTemplateCoverage() {
    std::cout << "\n========================================\n";
    std::cout << "Test 7: Template Coverage Analysis\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.maxInstructions = 100;
    config.timeout = std::chrono::milliseconds(50);
    config.useValidTemplates = true;
    
    InstructionFuzzer fuzzer(config);
    fuzzer.run(500);  // Run enough tests to get good coverage
    
    const FuzzStatistics& stats = fuzzer.getStatistics();
    
    std::cout << "\nTemplate Coverage Report:\n";
    std::cout << "Total templates covered: " << stats.templateCoverage.size() << "\n\n";
    
    std::vector<std::pair<TemplateType, uint64_t>> sortedTemplates(
        stats.templateCoverage.begin(), stats.templateCoverage.end());
    
    std::sort(sortedTemplates.begin(), sortedTemplates.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    
    for (const auto& entry : sortedTemplates) {
        std::cout << "  Template 0x" << std::hex << std::setw(2) << std::setfill('0') 
                 << static_cast<int>(entry.first) << std::dec 
                 << ": " << std::setw(6) << entry.second << " times ("
                 << std::fixed << std::setprecision(2)
                 << (100.0 * entry.second / stats.totalTests) << "%)\n";
    }
}

/**
 * Test 8: Performance benchmarking
 */
void testPerformanceBenchmark() {
    std::cout << "\n========================================\n";
    std::cout << "Test 8: Performance Benchmark\n";
    std::cout << "========================================\n";
    
    FuzzConfig config;
    config.maxInstructions = 1000;
    config.timeout = std::chrono::milliseconds(100);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    InstructionFuzzer fuzzer(config);
    fuzzer.run(500);
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    const FuzzStatistics& stats = fuzzer.getStatistics();
    
    std::cout << "\nPerformance Results:\n";
    std::cout << "  Total time: " << duration.count() << " ms\n";
    std::cout << "  Tests: " << stats.totalTests << "\n";
    std::cout << "  Tests/second: " << std::fixed << std::setprecision(2)
             << (stats.totalTests * 1000.0 / duration.count()) << "\n";
    std::cout << "  Avg time/test: " << std::fixed << std::setprecision(2)
             << (duration.count() / static_cast<double>(stats.totalTests)) << " ms\n";
}

int main(int argc, char* argv[]) {
    std::cout << "========================================\n";
    std::cout << "IA-64 Instruction Fuzzer Test Suite\n";
    std::cout << "========================================\n";
    
    try {
        // Run all tests
        testBasicFuzzing();
        testAggressiveFuzzing();
        testSpecificBundles();
        testBundleGenerator();
        testMutationStrategies();
        testTemplateCoverage();
        testPerformanceBenchmark();
        
        // Optionally run long campaign
        if (argc > 1 && std::string(argv[1]) == "--long") {
            testLongRunningFuzzing();
        }
        
        std::cout << "\n========================================\n";
        std::cout << "All Tests Completed Successfully!\n";
        std::cout << "========================================\n";
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\nFATAL ERROR: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "\nFATAL ERROR: Unknown exception\n";
        return 1;
    }
}

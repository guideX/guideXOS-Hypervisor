#include "Fuzzer.h"
#include "VirtualMachine.h"
#include "memory.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <thread>
#include <future>

namespace ia64 {

// =============================================================================
// FuzzStatistics Implementation
// =============================================================================

void FuzzStatistics::recordResult(const FuzzResult& result) {
    totalTests++;
    totalInstructions += result.instructionsExecuted;
    totalCycles += result.cyclesElapsed;
    totalTime += result.executionTime;
    
    if (result.passed) {
        passedTests++;
    } else {
        failedTests++;
        violationCounts[result.violation]++;
        
        // Track interesting cases
        if (result.violation != ViolationType::NONE) {
            interestingCases.push_back(result);
            
            // Keep only the most recent interesting cases (limit to 100)
            if (interestingCases.size() > 100) {
                interestingCases.erase(interestingCases.begin());
            }
        }
    }
}

std::string FuzzStatistics::generateReport() const {
    std::ostringstream report;
    
    report << "\n=== Fuzzing Campaign Report ===\n";
    report << "Total Tests: " << totalTests << "\n";
    report << "Passed: " << passedTests << " (" 
           << (totalTests > 0 ? (passedTests * 100 / totalTests) : 0) << "%)\n";
    report << "Failed: " << failedTests << " (" 
           << (totalTests > 0 ? (failedTests * 100 / totalTests) : 0) << "%)\n";
    
    report << "\nViolation Breakdown:\n";
    for (const auto& entry : violationCounts) {
        report << "  " << FuzzUtils::violationTypeToString(entry.first) 
               << ": " << entry.second << "\n";
    }
    
    report << "\nExecution Statistics:\n";
    report << "  Total Instructions: " << totalInstructions << "\n";
    report << "  Total Cycles: " << totalCycles << "\n";
    report << "  Total Time: " << totalTime.count() << " ?s\n";
    
    if (totalTests > 0) {
        report << "  Avg Instructions/Test: " << (totalInstructions / totalTests) << "\n";
        report << "  Avg Cycles/Test: " << (totalCycles / totalTests) << "\n";
        report << "  Avg Time/Test: " << (totalTime.count() / totalTests) << " ?s\n";
    }
    
    report << "\nTemplate Coverage:\n";
    for (const auto& entry : templateCoverage) {
        report << "  Template 0x" << std::hex << static_cast<int>(entry.first) 
               << std::dec << ": " << entry.second << " times\n";
    }
    
    report << "\nInteresting Cases Found: " << interestingCases.size() << "\n";
    
    return report.str();
}

// =============================================================================
// BundleGenerator Implementation
// =============================================================================

BundleGenerator::BundleGenerator(uint64_t seed) {
    if (seed == 0) {
        seed = static_cast<uint64_t>(std::chrono::high_resolution_clock::now()
                                      .time_since_epoch().count());
    }
    rng_.seed(seed);
}

std::vector<uint8_t> BundleGenerator::generateBundle(const FuzzConfig& config) {
    std::vector<uint8_t> bundle(16, 0);  // 128 bits = 16 bytes
    
    // Generate template
    TemplateType tmpl = generateTemplate(config.useValidTemplates);
    bundle[0] = static_cast<uint8_t>(tmpl) & 0x1F;  // Template is 5 bits
    
    // Get unit types for this template
    std::vector<UnitType> units = getUnitsForTemplate(tmpl);
    
    // Generate instructions for each slot
    for (size_t i = 0; i < 3 && i < units.size(); ++i) {
        uint64_t instruction = generateInstruction(units[i], config);
        packInstruction(bundle, i, instruction);
    }
    
    return bundle;
}

TemplateType BundleGenerator::generateTemplate(bool validOnly) {
    if (validOnly) {
        // List of valid templates
        static const TemplateType validTemplates[] = {
            TemplateType::MII,
            TemplateType::MII_STOP,
            TemplateType::MI_I,
            TemplateType::MI_I_STOP,
            TemplateType::MLX,
            TemplateType::MLX_STOP,
            TemplateType::MMI,
            TemplateType::MMI_STOP,
            TemplateType::MFI,
            TemplateType::MFI_STOP,
            TemplateType::MMF,
            TemplateType::MMF_STOP,
            TemplateType::MIB,
            TemplateType::MIB_STOP,
            TemplateType::MBB,
            TemplateType::MBB_STOP,
            TemplateType::BBB,
            TemplateType::BBB_STOP,
            TemplateType::MMB,
            TemplateType::MMB_STOP,
            TemplateType::MFB,
            TemplateType::MFB_STOP
        };
        
        std::uniform_int_distribution<size_t> dist(0, sizeof(validTemplates) / sizeof(validTemplates[0]) - 1);
        return validTemplates[dist(rng_)];
    } else {
        std::uniform_int_distribution<int> dist(0, 0x1F);
        return static_cast<TemplateType>(dist(rng_));
    }
}

uint64_t BundleGenerator::generateInstruction(UnitType unit, const FuzzConfig& config) {
    // Select instruction type based on unit and config
    std::uniform_int_distribution<int> typeDist(0, 100);
    int choice = typeDist(rng_);
    
    // Weight toward simpler, safer instructions
    if (choice < 30) {
        return encodeNop();
    } else if (choice < 50) {
        return encodeMov(config);
    } else if (choice < 70) {
        return encodeAdd(config);
    } else if (choice < 85 && unit == UnitType::M_UNIT) {
        return encodeLoad(config);
    } else if (choice < 95 && unit == UnitType::M_UNIT) {
        return encodeStore(config);
    } else if (config.allowBranches && unit == UnitType::B_UNIT) {
        return encodeBranch(config);
    } else {
        return encodeNop();
    }
}

uint8_t BundleGenerator::generateRegister(bool allowR0) {
    std::uniform_int_distribution<int> dist(allowR0 ? 0 : 1, 127);
    return static_cast<uint8_t>(dist(rng_));
}

uint8_t BundleGenerator::generatePredicateRegister() {
    std::uniform_int_distribution<int> dist(0, 63);
    return static_cast<uint8_t>(dist(rng_));
}

uint8_t BundleGenerator::generateBranchRegister() {
    std::uniform_int_distribution<int> dist(0, 7);
    return static_cast<uint8_t>(dist(rng_));
}

uint64_t BundleGenerator::generateImmediate(size_t bits) {
    if (bits >= 64) {
        std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
        return dist(rng_);
    } else {
        uint64_t mask = (1ULL << bits) - 1;
        std::uniform_int_distribution<uint64_t> dist(0, mask);
        return dist(rng_);
    }
}

bool BundleGenerator::isValidTemplate(TemplateType tmpl) const {
    // Check if template is one of the defined valid types
    switch (tmpl) {
        case TemplateType::MII:
        case TemplateType::MII_STOP:
        case TemplateType::MI_I:
        case TemplateType::MI_I_STOP:
        case TemplateType::MLX:
        case TemplateType::MLX_STOP:
        case TemplateType::MMI:
        case TemplateType::MMI_STOP:
        case TemplateType::MFI:
        case TemplateType::MFI_STOP:
        case TemplateType::MMF:
        case TemplateType::MMF_STOP:
        case TemplateType::MIB:
        case TemplateType::MIB_STOP:
        case TemplateType::MBB:
        case TemplateType::MBB_STOP:
        case TemplateType::BBB:
        case TemplateType::BBB_STOP:
        case TemplateType::MMB:
        case TemplateType::MMB_STOP:
        case TemplateType::MFB:
        case TemplateType::MFB_STOP:
            return true;
        default:
            return false;
    }
}

bool BundleGenerator::isValidBundle(const std::vector<uint8_t>& bundle) const {
    if (bundle.size() != 16) {
        return false;
    }
    
    TemplateType tmpl = static_cast<TemplateType>(bundle[0] & 0x1F);
    return isValidTemplate(tmpl);
}

std::vector<UnitType> BundleGenerator::getUnitsForTemplate(TemplateType tmpl) const {
    switch (tmpl) {
        case TemplateType::MII:
        case TemplateType::MII_STOP:
            return {UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT};
            
        case TemplateType::MI_I:
        case TemplateType::MI_I_STOP:
            return {UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT};
            
        case TemplateType::MLX:
        case TemplateType::MLX_STOP:
            return {UnitType::M_UNIT, UnitType::L_UNIT, UnitType::X_UNIT};
            
        case TemplateType::MMI:
        case TemplateType::MMI_STOP:
            return {UnitType::M_UNIT, UnitType::M_UNIT, UnitType::I_UNIT};
            
        case TemplateType::MFI:
        case TemplateType::MFI_STOP:
            return {UnitType::M_UNIT, UnitType::F_UNIT, UnitType::I_UNIT};
            
        case TemplateType::MMF:
        case TemplateType::MMF_STOP:
            return {UnitType::M_UNIT, UnitType::M_UNIT, UnitType::F_UNIT};
            
        case TemplateType::MIB:
        case TemplateType::MIB_STOP:
            return {UnitType::M_UNIT, UnitType::I_UNIT, UnitType::B_UNIT};
            
        case TemplateType::MBB:
        case TemplateType::MBB_STOP:
            return {UnitType::M_UNIT, UnitType::B_UNIT, UnitType::B_UNIT};
            
        case TemplateType::BBB:
        case TemplateType::BBB_STOP:
            return {UnitType::B_UNIT, UnitType::B_UNIT, UnitType::B_UNIT};
            
        case TemplateType::MMB:
        case TemplateType::MMB_STOP:
            return {UnitType::M_UNIT, UnitType::M_UNIT, UnitType::B_UNIT};
            
        case TemplateType::MFB:
        case TemplateType::MFB_STOP:
            return {UnitType::M_UNIT, UnitType::F_UNIT, UnitType::B_UNIT};
            
        default:
            return {UnitType::I_UNIT, UnitType::I_UNIT, UnitType::I_UNIT};
    }
}

uint64_t BundleGenerator::encodeNop() const {
    // NOP encoding (simplified)
    return 0x0000000000000000ULL;
}

uint64_t BundleGenerator::encodeMov(const FuzzConfig& config) {
    // Simplified MOV encoding: mov rDst = rSrc
    // Format: [opcode:8][predicate:6][dst:7][src:7][unused:13]
    uint64_t instruction = 0x01ULL;  // MOV opcode
    
    if (config.randomizeRegisters) {
        uint8_t dst = generateRegister(false);
        uint8_t src = generateRegister(true);
        uint8_t pred = generatePredicateRegister();
        
        instruction |= (static_cast<uint64_t>(pred) & 0x3F) << 8;
        instruction |= (static_cast<uint64_t>(dst) & 0x7F) << 14;
        instruction |= (static_cast<uint64_t>(src) & 0x7F) << 21;
    }
    
    return instruction & 0x1FFFFFFFFFFULL;  // 41 bits
}

uint64_t BundleGenerator::encodeAdd(const FuzzConfig& config) {
    // Simplified ADD encoding: add rDst = rSrc1, rSrc2
    uint64_t instruction = 0x02ULL;  // ADD opcode
    
    if (config.randomizeRegisters) {
        uint8_t dst = generateRegister(false);
        uint8_t src1 = generateRegister(true);
        uint8_t src2 = generateRegister(true);
        uint8_t pred = generatePredicateRegister();
        
        instruction |= (static_cast<uint64_t>(pred) & 0x3F) << 8;
        instruction |= (static_cast<uint64_t>(dst) & 0x7F) << 14;
        instruction |= (static_cast<uint64_t>(src1) & 0x7F) << 21;
        instruction |= (static_cast<uint64_t>(src2) & 0x7F) << 28;
    }
    
    return instruction & 0x1FFFFFFFFFFULL;  // 41 bits
}

uint64_t BundleGenerator::encodeLoad(const FuzzConfig& config) {
    // Simplified LD8 encoding: ld8 rDst = [rSrc]
    uint64_t instruction = 0x03ULL;  // LD8 opcode
    
    if (config.randomizeRegisters) {
        uint8_t dst = generateRegister(false);
        uint8_t src = generateRegister(true);
        uint8_t pred = generatePredicateRegister();
        
        instruction |= (static_cast<uint64_t>(pred) & 0x3F) << 8;
        instruction |= (static_cast<uint64_t>(dst) & 0x7F) << 14;
        instruction |= (static_cast<uint64_t>(src) & 0x7F) << 21;
    }
    
    return instruction & 0x1FFFFFFFFFFULL;  // 41 bits
}

uint64_t BundleGenerator::encodeStore(const FuzzConfig& config) {
    // Simplified ST8 encoding: st8 [rDst] = rSrc
    uint64_t instruction = 0x04ULL;  // ST8 opcode
    
    if (config.randomizeRegisters) {
        uint8_t dst = generateRegister(true);
        uint8_t src = generateRegister(true);
        uint8_t pred = generatePredicateRegister();
        
        instruction |= (static_cast<uint64_t>(pred) & 0x3F) << 8;
        instruction |= (static_cast<uint64_t>(dst) & 0x7F) << 14;
        instruction |= (static_cast<uint64_t>(src) & 0x7F) << 21;
    }
    
    return instruction & 0x1FFFFFFFFFFULL;  // 41 bits
}

uint64_t BundleGenerator::encodeBranch(const FuzzConfig& config) {
    // Simplified branch encoding
    uint64_t instruction = 0x05ULL;  // BR opcode
    
    if (config.randomizeImmediates) {
        // Generate small branch offset to avoid jumping too far
        uint64_t offset = generateImmediate(16);
        instruction |= (offset & 0xFFFF) << 8;
    }
    
    return instruction & 0x1FFFFFFFFFFULL;  // 41 bits
}

void BundleGenerator::packInstruction(std::vector<uint8_t>& bundle, size_t slotIndex, uint64_t instruction) {
    // Pack 41-bit instruction into the bundle
    // Slot 0: bits 5-45
    // Slot 1: bits 46-86
    // Slot 2: bits 87-127
    
    size_t bitOffset = 5 + (slotIndex * 41);
    
    for (size_t i = 0; i < 41; ++i) {
        size_t totalBit = bitOffset + i;
        size_t byteIndex = totalBit / 8;
        size_t bitInByte = totalBit % 8;
        
        if ((instruction >> i) & 1) {
            bundle[byteIndex] |= (1 << bitInByte);
        } else {
            bundle[byteIndex] &= ~(1 << bitInByte);
        }
    }
}

// =============================================================================
// InstructionFuzzer Implementation
// =============================================================================

InstructionFuzzer::InstructionFuzzer(const FuzzConfig& config)
    : config_(config)
    , generator_(config.seed)
    , stats_() {
}

InstructionFuzzer::~InstructionFuzzer() {
}

void InstructionFuzzer::run(uint64_t numTests) {
    std::cout << "Starting fuzzing campaign with " << numTests << " tests...\n";
    std::cout << "Configuration:\n";
    std::cout << "  Max Instructions: " << config_.maxInstructions << "\n";
    std::cout << "  Max Cycles: " << config_.maxCycles << "\n";
    std::cout << "  Timeout: " << config_.timeout.count() << " ms\n";
    std::cout << "  Memory Size: " << config_.memorySize << " bytes\n";
    std::cout << "\n";
    
    for (uint64_t i = 0; i < numTests; ++i) {
        // Generate or mutate bundle
        std::vector<uint8_t> bundle;
        
        if (!corpus_.empty() && (i % 10 == 0)) {
            // 10% of the time, mutate from corpus
            auto it = corpus_.begin();
            std::advance(it, i % corpus_.size());
            bundle = mutateBundle(*it);
        } else {
            // Generate new bundle
            bundle = generator_.generateBundle(config_);
        }
        
        // Track template coverage
        TemplateType tmpl = FuzzUtils::extractTemplate(bundle);
        stats_.templateCoverage[tmpl]++;
        
        // Execute test
        FuzzResult result = testBundle(bundle);
        stats_.recordResult(result);
        
        // Add to corpus if interesting
        if (config_.saveInterestingCases && !result.passed) {
            addToCorpus(bundle);
        }
        
        // Progress update every 100 tests
        if ((i + 1) % 100 == 0) {
            std::cout << "Progress: " << (i + 1) << "/" << numTests 
                     << " - Violations: " << stats_.failedTests << "\n";
        }
    }
    
    // Print final report
    std::cout << stats_.generateReport();
    
    // Save corpus if configured
    if (config_.saveInterestingCases && !config_.corpusDirectory.empty()) {
        saveCorpus(config_.corpusDirectory);
    }
}

FuzzResult InstructionFuzzer::testBundle(const std::vector<uint8_t>& bundle) {
    FuzzResult result;
    result.inputBundle = bundle;
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    try {
        // Create isolated VM instance
        VirtualMachine vm(config_.memorySize);
        
        if (!vm.init()) {
            result.passed = false;
            result.violation = ViolationType::CRASH;
            result.errorMessage = "Failed to initialize VM";
            return result;
        }
        
        // Setup VM with bundle
        setupVM(vm, bundle);
        
        // Execute with timeout
        uint64_t instructionsExecuted = 0;
        ViolationType violation = ViolationType::NONE;
        
        bool success = executeWithTimeout(vm, instructionsExecuted, violation);
        
        result.instructionsExecuted = instructionsExecuted;
        result.crashIP = vm.getCPU().getState().GetIP();
        
        if (!success || violation != ViolationType::NONE) {
            result.passed = false;
            result.violation = violation;
            result.errorMessage = FuzzUtils::violationTypeToString(violation);
        }
        
    } catch (const std::exception& e) {
        result.passed = false;
        result.violation = ViolationType::CRASH;
        result.errorMessage = std::string("Exception: ") + e.what();
    } catch (...) {
        result.passed = false;
        result.violation = ViolationType::CRASH;
        result.errorMessage = "Unknown exception";
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTime = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    return result;
}

void InstructionFuzzer::setupVM(VirtualMachine& vm, const std::vector<uint8_t>& bundle) {
    // Load bundle at start of memory
    uint64_t codeAddr = 0x1000;
    vm.loadProgram(bundle.data(), bundle.size(), codeAddr);
    vm.setEntryPoint(codeAddr);
    
    // Initialize stack
    CPUState& state = vm.getCPU().getState();
    state.SetGR(12, config_.stackBase + config_.stackSize);  // r12 = stack pointer
}

bool InstructionFuzzer::executeWithTimeout(VirtualMachine& vm, 
                                          uint64_t& instructionsExecuted,
                                          ViolationType& violation) {
    instructionsExecuted = 0;
    violation = ViolationType::NONE;
    
    std::vector<uint64_t> ipHistory;
    ipHistory.reserve(100);
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    while (instructionsExecuted < config_.maxInstructions) {
        // Check timeout
        auto now = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
        if (elapsed > config_.timeout) {
            violation = ViolationType::TIMEOUT;
            return false;
        }
        
        // Track IP for loop detection
        uint64_t currentIP = vm.getCPU().getState().GetIP();
        ipHistory.push_back(currentIP);
        if (ipHistory.size() > 100) {
            ipHistory.erase(ipHistory.begin());
        }
        
        // Check for infinite loop
        if (checkInfiniteLoop(ipHistory)) {
            violation = ViolationType::INFINITE_LOOP;
            return false;
        }
        
        // Execute one step
        try {
            bool shouldContinue = vm.step();
            instructionsExecuted++;
            
            // Check for violations after execution
            violation = detectViolation(vm, instructionsExecuted);
            if (violation != ViolationType::NONE) {
                return false;
            }
            
            if (!shouldContinue) {
                // Normal termination
                return true;
            }
        } catch (...) {
            violation = ViolationType::CRASH;
            return false;
        }
    }
    
    // Reached instruction limit
    return true;
}

ViolationType InstructionFuzzer::detectViolation(VirtualMachine& vm, uint64_t instructionsExecuted) {
const CPUState& state = vm.getCPU().getState();
    
    // Check for NaT violations
    if (checkNaTViolation(state)) {
        return ViolationType::NAT_VIOLATION;
    }
    
    // Check for memory violations
    if (checkMemoryViolation(vm)) {
        return ViolationType::MEMORY_VIOLATION;
    }
    
    return ViolationType::NONE;
}

bool InstructionFuzzer::checkNaTViolation(const CPUState& state) {
    // In a real implementation, we would check if any register marked as NaT
    // was consumed by an instruction
    // For now, this is a placeholder
    return false;
}

bool InstructionFuzzer::checkMemoryViolation(VirtualMachine& vm) {
    // Check if last memory access was out of bounds
    // This would require tracking last access in Memory class
    return false;
}

bool InstructionFuzzer::checkInfiniteLoop(const std::vector<uint64_t>& ipHistory) {
    if (ipHistory.size() < 20) {
        return false;
    }
    
    // Simple pattern detection: check if we're stuck on the same IP
    uint64_t lastIP = ipHistory.back();
    int sameCount = 0;
    
    for (size_t i = ipHistory.size() - 1; i >= ipHistory.size() - 20 && i < ipHistory.size(); --i) {
        if (ipHistory[i] == lastIP) {
            sameCount++;
        }
    }
    
    // If more than 15 out of last 20 IPs are the same, likely infinite loop
    return sameCount > 15;
}

void InstructionFuzzer::addToCorpus(const std::vector<uint8_t>& bundle) {
    corpus_.insert(bundle);
}

bool InstructionFuzzer::isInCorpus(const std::vector<uint8_t>& bundle) const {
    return corpus_.find(bundle) != corpus_.end();
}

std::vector<uint8_t> InstructionFuzzer::mutateBundle(const std::vector<uint8_t>& bundle) {
    std::vector<uint8_t> mutated = bundle;
    
    std::uniform_int_distribution<int> strategyDist(0, 3);
    int strategy = strategyDist(generator_.rng_);
    
    switch (strategy) {
        case 0: bitFlipMutation(mutated); break;
        case 1: byteFlipMutation(mutated); break;
        case 2: arithmeticMutation(mutated); break;
        case 3: interestingValueMutation(mutated); break;
    }
    
    return mutated;
}

void InstructionFuzzer::bitFlipMutation(std::vector<uint8_t>& bundle) {
    std::uniform_int_distribution<size_t> byteDist(0, bundle.size() - 1);
    std::uniform_int_distribution<int> bitDist(0, 7);
    
    size_t byteIndex = byteDist(generator_.rng_);
    int bitIndex = bitDist(generator_.rng_);
    
    bundle[byteIndex] ^= (1 << bitIndex);
}

void InstructionFuzzer::byteFlipMutation(std::vector<uint8_t>& bundle) {
    std::uniform_int_distribution<size_t> byteDist(0, bundle.size() - 1);
    size_t byteIndex = byteDist(generator_.rng_);
    
    bundle[byteIndex] = ~bundle[byteIndex];
}

void InstructionFuzzer::arithmeticMutation(std::vector<uint8_t>& bundle) {
    std::uniform_int_distribution<size_t> byteDist(0, bundle.size() - 1);
    std::uniform_int_distribution<int> deltaDist(-35, 35);
    
    size_t byteIndex = byteDist(generator_.rng_);
    int delta = deltaDist(generator_.rng_);
    
    bundle[byteIndex] = static_cast<uint8_t>(bundle[byteIndex] + delta);
}

void InstructionFuzzer::interestingValueMutation(std::vector<uint8_t>& bundle) {
    static const uint8_t interestingValues[] = {
        0x00, 0x01, 0x7F, 0x80, 0xFF,
        0x10, 0x20, 0x40
    };
    
    std::uniform_int_distribution<size_t> byteDist(0, bundle.size() - 1);
    std::uniform_int_distribution<size_t> valueDist(0, sizeof(interestingValues) - 1);
    
    size_t byteIndex = byteDist(generator_.rng_);
    bundle[byteIndex] = interestingValues[valueDist(generator_.rng_)];
}

void InstructionFuzzer::saveCorpus(const std::string& directory) {
    // Create directory if it doesn't exist
    // Note: This is platform-specific. For Windows, you might use _mkdir
    std::cout << "Saving corpus to " << directory << "...\n";
    std::cout << "Corpus size: " << corpus_.size() << " bundles\n";
    
    size_t index = 0;
    for (const auto& bundle : corpus_) {
        std::ostringstream filename;
        filename << directory << "/bundle_" << std::setw(6) << std::setfill('0') << index << ".bin";
        
        std::ofstream file(filename.str(), std::ios::binary);
        if (file) {
            file.write(reinterpret_cast<const char*>(bundle.data()), bundle.size());
        }
        
        index++;
    }
}

void InstructionFuzzer::loadCorpus(const std::string& directory) {
    std::cout << "Loading corpus from " << directory << "...\n";
    // Implementation would scan directory and load .bin files
    // Omitted for brevity
}

// =============================================================================
// FuzzUtils Implementation
// =============================================================================

namespace FuzzUtils {

std::string violationTypeToString(ViolationType type) {
    switch (type) {
        case ViolationType::NONE: return "NONE";
        case ViolationType::CRASH: return "CRASH";
        case ViolationType::NAT_VIOLATION: return "NAT_VIOLATION";
        case ViolationType::TIMEOUT: return "TIMEOUT";
        case ViolationType::ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case ViolationType::MEMORY_VIOLATION: return "MEMORY_VIOLATION";
        case ViolationType::DIVISION_BY_ZERO: return "DIVISION_BY_ZERO";
        case ViolationType::STACK_OVERFLOW: return "STACK_OVERFLOW";
        case ViolationType::INFINITE_LOOP: return "INFINITE_LOOP";
        case ViolationType::UNDEFINED_BEHAVIOR: return "UNDEFINED_BEHAVIOR";
        default: return "UNKNOWN";
    }
}

std::string bundleToHexString(const std::vector<uint8_t>& bundle) {
    std::ostringstream oss;
    for (size_t i = 0; i < bundle.size(); ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') 
            << static_cast<int>(bundle[i]);
        if (i < bundle.size() - 1) {
            oss << " ";
        }
    }
    return oss.str();
}

std::vector<uint8_t> hexStringToBundle(const std::string& hex) {
    std::vector<uint8_t> bundle;
    // Parse hex string - implementation omitted for brevity
    return bundle;
}

bool validateBundleStructure(const std::vector<uint8_t>& bundle) {
    return bundle.size() == 16;
}

TemplateType extractTemplate(const std::vector<uint8_t>& bundle) {
    if (bundle.empty()) {
        return TemplateType::INVALID;
    }
    return static_cast<TemplateType>(bundle[0] & 0x1F);
}

uint64_t extractSlot(const std::vector<uint8_t>& bundle, size_t slotIndex) {
    if (bundle.size() != 16 || slotIndex >= 3) {
        return 0;
    }
    
    size_t bitOffset = 5 + (slotIndex * 41);
    uint64_t instruction = 0;
    
    for (size_t i = 0; i < 41; ++i) {
        size_t totalBit = bitOffset + i;
        size_t byteIndex = totalBit / 8;
        size_t bitInByte = totalBit % 8;
        
        if (bundle[byteIndex] & (1 << bitInByte)) {
            instruction |= (1ULL << i);
        }
    }
    
    return instruction;
}

} // namespace FuzzUtils

} // namespace ia64

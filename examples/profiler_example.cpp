/**
 * Example: Using the IA-64 Profiler
 * 
 * This example demonstrates how to use the profiler to analyze
 * program execution, track instruction frequencies, memory access
 * patterns, register pressure, and generate control flow graphs.
 */

#include "cpu.h"
#include "memory.h"
#include "decoder.h"
#include "Profiler.h"
#include <iostream>
#include <fstream>
#include <algorithm>

using namespace ia64;

int main() {
    std::cout << "IA-64 Profiler Example\n";
    std::cout << "======================\n\n";
    
    // Step 1: Create the emulator components
    Memory memory(64 * 1024 * 1024);  // 64 MB
    InstructionDecoder decoder;
    CPU cpu(memory, decoder);
    
    // Step 2: Configure the profiler
    ProfilerConfig config;
    config.trackInstructionFrequency = true;
    config.trackExecutionPaths = true;
    config.trackRegisterPressure = true;
    config.trackMemoryAccess = true;
    config.trackControlFlow = true;
    config.hotPathThreshold = 10;  // Paths executed 10+ times are "hot"
    
    // Define memory regions for classification
    config.stackStart = 0x7FFF0000;
    config.stackEnd = 0x80000000;
    config.heapStart = 0x10000000;
    config.heapEnd = 0x20000000;
    config.codeStart = 0x00400000;
    config.codeEnd = 0x00500000;
    
    Profiler profiler(config);
    profiler.setStackRegion(config.stackStart, config.stackEnd);
    profiler.setHeapRegion(config.heapStart, config.heapEnd);
    profiler.setCodeRegion(config.codeStart, config.codeEnd);
    
    // Step 3: Attach profiler to CPU
    cpu.setProfiler(&profiler);
    
    std::cout << "Profiler configured and attached to CPU\n";
    std::cout << "- Instruction frequency tracking: ENABLED\n";
    std::cout << "- Execution path tracking: ENABLED\n";
    std::cout << "- Register pressure tracking: ENABLED\n";
    std::cout << "- Memory access tracking: ENABLED\n";
    std::cout << "- Control flow graph: ENABLED\n\n";
    
    // Step 4: Load a program (simplified example)
    // In a real scenario, you would load an ELF binary here
    // For this example, we'll simulate some instructions
    std::cout << "Loading program...\n";
    
    // Simulate program execution
    // (In a real scenario, you would load binary code and execute)
    std::cout << "Simulating program execution...\n";
    
    // Manually record some profiling events for demonstration
    for (int i = 0; i < 100; i++) {
        profiler.recordInstructionExecution(0x401000, "mov r1 = r2");
        profiler.recordGeneralRegisterRead(2);
        profiler.recordGeneralRegisterWrite(1);
        
        if (i % 3 == 0) {
            profiler.recordInstructionExecution(0x401010, "ld8 r3 = [r4]");
            profiler.recordMemoryRead(0x10001000 + i * 8, 8);
            profiler.recordGeneralRegisterRead(4);
            profiler.recordGeneralRegisterWrite(3);
        }
        
        if (i % 2 == 0) {
            profiler.recordInstructionExecution(0x401020, "st8 [r5] = r6");
            profiler.recordMemoryWrite(0x7FFF1000 + i * 8, 8);
            profiler.recordGeneralRegisterRead(5);
            profiler.recordGeneralRegisterRead(6);
        }
        
        profiler.recordBranch(0x401020, 0x401000, true, true);
    }
    
    std::cout << "Execution complete!\n\n";
    
    // Step 5: Query statistics
    std::cout << "=== Execution Statistics ===\n";
    std::cout << "Total instructions executed: " << profiler.getTotalInstructions() << "\n";
    std::cout << "Unique instructions: " << profiler.getUniqueInstructionCount() << "\n";
    std::cout << "Total memory reads: " << profiler.getTotalMemoryReads() << "\n";
    std::cout << "Total memory writes: " << profiler.getTotalMemoryWrites() << "\n\n";
    
    // Top instructions
    std::cout << "=== Top 10 Instructions ===\n";
    auto topInstr = profiler.getTopInstructions(10);
    for (size_t i = 0; i < topInstr.size(); i++) {
        std::cout << i + 1 << ". Address 0x" << std::hex << topInstr[i].first << std::dec
                  << " - Count: " << topInstr[i].second << "\n";
    }
    std::cout << "\n";
    
    // Memory statistics
    std::cout << "=== Memory Access Statistics ===\n";
    const auto& memStats = profiler.getMemoryStats();
    
    auto printMemStats = [](const char* region, const MemoryRegionStats& stats) {
        std::cout << region << ":\n";
        std::cout << "  Reads: " << stats.readCount << "\n";
        std::cout << "  Writes: " << stats.writeCount << "\n";
        std::cout << "  Total bytes: " << stats.totalBytes << "\n";
        std::cout << "  Unique addresses: " << stats.uniqueAddresses << "\n";
    };
    
    if (memStats.find(MemoryRegionType::STACK) != memStats.end()) {
        printMemStats("Stack", memStats.at(MemoryRegionType::STACK));
    }
    if (memStats.find(MemoryRegionType::HEAP) != memStats.end()) {
        printMemStats("Heap", memStats.at(MemoryRegionType::HEAP));
    }
    if (memStats.find(MemoryRegionType::CODE) != memStats.end()) {
        printMemStats("Code", memStats.at(MemoryRegionType::CODE));
    }
    std::cout << "\n";
    
    // Register pressure
    std::cout << "=== Register Pressure ===\n";
    const RegisterPressure& pressure = profiler.getRegisterPressure();
    
    std::cout << "Top 5 most used general registers:\n";
    std::vector<std::pair<size_t, uint64_t>> grUsage;
    for (size_t i = 0; i < pressure.grReadCount.size(); i++) {
        uint64_t total = pressure.grReadCount[i] + pressure.grWriteCount[i];
        if (total > 0) {
            grUsage.push_back(std::make_pair(i, total));
        }
    }
    std::sort(grUsage.begin(), grUsage.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    
    for (size_t i = 0; i < std::min(size_t(5), grUsage.size()); i++) {
        std::cout << "  GR" << grUsage[i].first << ": "
                  << grUsage[i].second << " accesses ("
                  << pressure.grReadCount[grUsage[i].first] << " reads, "
                  << pressure.grWriteCount[grUsage[i].first] << " writes)\n";
    }
    std::cout << "\n";
    
    // Hot paths
    std::cout << "=== Hot Execution Paths ===\n";
    auto hotPaths = profiler.getHotPaths(5);
    if (hotPaths.empty()) {
        std::cout << "No hot paths detected (threshold: " 
                  << config.hotPathThreshold << ")\n";
    } else {
        for (size_t i = 0; i < hotPaths.size(); i++) {
            std::cout << i + 1 << ". Path executed " << hotPaths[i].executionCount 
                      << " times, length " << hotPaths[i].addresses.size() << " instructions\n";
        }
    }
    std::cout << "\n";
    
    // Step 6: Export results
    std::cout << "=== Exporting Results ===\n";
    
    // JSON export
    std::string json = profiler.exportToJSON();
    std::ofstream jsonFile("profile_results.json");
    jsonFile << json;
    jsonFile.close();
    std::cout << "JSON export: profile_results.json (" << json.length() << " bytes)\n";
    
    // DOT export (control flow graph)
    std::string dot = profiler.exportControlFlowGraphDOT();
    std::ofstream dotFile("control_flow_graph.dot");
    dotFile << dot;
    dotFile.close();
    std::cout << "DOT export: control_flow_graph.dot (" << dot.length() << " bytes)\n";
    std::cout << "  Visualize with: dot -Tpng control_flow_graph.dot -o cfg.png\n";
    
    // CSV exports
    std::string instrCSV = profiler.exportInstructionFrequencyCSV();
    std::ofstream instrFile("instruction_frequency.csv");
    instrFile << instrCSV;
    instrFile.close();
    std::cout << "Instruction frequency CSV: instruction_frequency.csv\n";
    
    std::string regCSV = profiler.exportRegisterPressureCSV();
    std::ofstream regFile("register_pressure.csv");
    regFile << regCSV;
    regFile.close();
    std::cout << "Register pressure CSV: register_pressure.csv\n";
    
    std::string memCSV = profiler.exportMemoryAccessCSV();
    std::ofstream memFile("memory_access.csv");
    memFile << memCSV;
    memFile.close();
    std::cout << "Memory access CSV: memory_access.csv\n";
    
    std::cout << "\n=== Profiling Complete ===\n";
    std::cout << "All results have been exported successfully!\n";
    std::cout << "\nNext steps:\n";
    std::cout << "1. View JSON for overall statistics: profile_results.json\n";
    std::cout << "2. Visualize control flow: dot -Tpng control_flow_graph.dot -o cfg.png\n";
    std::cout << "3. Analyze CSV files with spreadsheet or Python/R\n";
    
    return 0;
}

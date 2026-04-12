#include "Profiler.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <fstream>

using namespace ia64;

// Test helper function
void assertTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(1);
    }
    std::cout << "PASSED: " << message << std::endl;
}

void testInstructionFrequencyTracking() {
    std::cout << "\n=== Test: Instruction Frequency Tracking ===" << std::endl;
    
    ProfilerConfig config;
    config.trackInstructionFrequency = true;
    Profiler profiler(config);
    
    // Simulate executing some instructions
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordInstructionExecution(0x1010, "add r3 = r1, r4");
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");  // Execute same instruction again
    profiler.recordInstructionExecution(0x1020, "ld8 r5 = [r6]");
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");  // Third time
    
    // Verify counts
    assertTrue(profiler.getTotalInstructions() == 5, "Total instruction count");
    assertTrue(profiler.getUniqueInstructionCount() == 3, "Unique instruction count");
    assertTrue(profiler.getInstructionCount(0x1000) == 3, "Instruction at 0x1000 count");
    assertTrue(profiler.getInstructionCount(0x1010) == 1, "Instruction at 0x1010 count");
    assertTrue(profiler.getInstructionCount(0x1020) == 1, "Instruction at 0x1020 count");
    
    // Test top instructions
    auto topInstr = profiler.getTopInstructions(10);
    assertTrue(topInstr.size() == 3, "Top instructions list size");
    assertTrue(topInstr[0].first == 0x1000, "Most frequent instruction address");
    assertTrue(topInstr[0].second == 3, "Most frequent instruction count");
}

void testRegisterPressureTracking() {
    std::cout << "\n=== Test: Register Pressure Tracking ===" << std::endl;
    
    ProfilerConfig config;
    config.trackRegisterPressure = true;
    Profiler profiler(config);
    
    // Simulate register accesses
    profiler.recordGeneralRegisterRead(1);
    profiler.recordGeneralRegisterRead(1);
    profiler.recordGeneralRegisterWrite(1);
    profiler.recordGeneralRegisterRead(2);
    profiler.recordGeneralRegisterWrite(3);
    
    profiler.recordPredicateRegisterRead(0);
    profiler.recordPredicateRegisterWrite(1);
    
    profiler.recordBranchRegisterRead(0);
    profiler.recordBranchRegisterWrite(0);
    
    profiler.recordApplicationRegisterRead(40);  // RSC
    profiler.recordApplicationRegisterWrite(40);
    
    const RegisterPressure& pressure = profiler.getRegisterPressure();
    
    assertTrue(pressure.grReadCount[1] == 2, "GR1 read count");
    assertTrue(pressure.grWriteCount[1] == 1, "GR1 write count");
    assertTrue(pressure.grReadCount[2] == 1, "GR2 read count");
    assertTrue(pressure.grWriteCount[3] == 1, "GR3 write count");
    
    assertTrue(pressure.prReadCount[0] == 1, "PR0 read count");
    assertTrue(pressure.prWriteCount[1] == 1, "PR1 write count");
    
    assertTrue(pressure.brReadCount[0] == 1, "BR0 read count");
    assertTrue(pressure.brWriteCount[0] == 1, "BR0 write count");
    
    assertTrue(pressure.arReadCount.at(40) == 1, "AR40 read count");
    assertTrue(pressure.arWriteCount.at(40) == 1, "AR40 write count");
}

void testMemoryAccessClassification() {
    std::cout << "\n=== Test: Memory Access Classification ===" << std::endl;
    
    ProfilerConfig config;
    config.trackMemoryAccess = true;
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
    
    // Simulate memory accesses
    profiler.recordMemoryRead(0x7FFF1000, 8);   // Stack read
    profiler.recordMemoryWrite(0x7FFF2000, 8);  // Stack write
    profiler.recordMemoryRead(0x10001000, 8);   // Heap read
    profiler.recordMemoryWrite(0x10002000, 4);  // Heap write
    profiler.recordMemoryRead(0x00401000, 16);  // Code read
    profiler.recordMemoryRead(0x90000000, 8);   // Unknown read
    
    const auto& memStats = profiler.getMemoryStats();
    
    assertTrue(memStats.at(MemoryRegionType::STACK).readCount == 1, "Stack read count");
    assertTrue(memStats.at(MemoryRegionType::STACK).writeCount == 1, "Stack write count");
    assertTrue(memStats.at(MemoryRegionType::HEAP).readCount == 1, "Heap read count");
    assertTrue(memStats.at(MemoryRegionType::HEAP).writeCount == 1, "Heap write count");
    assertTrue(memStats.at(MemoryRegionType::CODE).readCount == 1, "Code read count");
    assertTrue(memStats.at(MemoryRegionType::UNKNOWN).readCount == 1, "Unknown region read count");
    
    assertTrue(profiler.getTotalMemoryReads() == 4, "Total memory reads");
    assertTrue(profiler.getTotalMemoryWrites() == 2, "Total memory writes");
}

void testControlFlowGraphBuilding() {
    std::cout << "\n=== Test: Control Flow Graph Building ===" << std::endl;
    
    ProfilerConfig config;
    config.trackControlFlow = true;
    Profiler profiler(config);
    
    // Simulate sequential execution
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordInstructionExecution(0x1010, "add r3 = r1, r4");
    
    // Simulate branch
    profiler.recordBranch(0x1010, 0x2000, true, false);  // Unconditional branch taken
    profiler.recordInstructionExecution(0x2000, "sub r5 = r6, r7");
    
    // Simulate conditional branch not taken
    profiler.recordBranch(0x2000, 0x3000, false, true);  // Conditional branch not taken
    profiler.recordInstructionExecution(0x2010, "ld8 r8 = [r9]");
    
    // Simulate loop (same addresses executed multiple times)
    for (int i = 0; i < 5; i++) {
        profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    }
    
    assertTrue(profiler.getInstructionCount(0x1000) == 6, "Loop instruction count");
    assertTrue(profiler.getTotalInstructions() == 9, "Total instructions with loop");
}

void testHotPathDetection() {
    std::cout << "\n=== Test: Hot Path Detection ===" << std::endl;
    
    ProfilerConfig config;
    config.trackExecutionPaths = true;
    config.hotPathThreshold = 3;
    Profiler profiler(config);
    
    // Simulate a hot loop
    for (int i = 0; i < 10; i++) {
        profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
        profiler.recordInstructionExecution(0x1010, "add r3 = r1, r4");
        profiler.recordInstructionExecution(0x1020, "cmp.eq p1, p2 = r3, r0");
        profiler.recordBranch(0x1020, 0x1000, true, true);
    }
    
    auto hotPaths = profiler.getHotPaths(5);
    assertTrue(hotPaths.size() > 0, "Hot paths detected");
    
    if (hotPaths.size() > 0) {
        std::cout << "  Detected " << hotPaths.size() << " hot path(s)" << std::endl;
        std::cout << "  Hottest path executed " << hotPaths[0].executionCount << " times" << std::endl;
    }
}

void testJSONExport() {
    std::cout << "\n=== Test: JSON Export ===" << std::endl;
    
    ProfilerConfig config;
    Profiler profiler(config);
    
    // Add some data
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordInstructionExecution(0x1010, "add r3 = r1, r4");
    profiler.recordMemoryRead(0x7FFF1000, 8);
    profiler.recordMemoryWrite(0x10001000, 4);
    
    std::string json = profiler.exportToJSON();
    
    assertTrue(json.length() > 0, "JSON export produces output");
    assertTrue(json.find("summary") != std::string::npos, "JSON contains summary");
    assertTrue(json.find("topInstructions") != std::string::npos, "JSON contains top instructions");
    assertTrue(json.find("memoryStats") != std::string::npos, "JSON contains memory stats");
    
    std::cout << "  JSON export length: " << json.length() << " bytes" << std::endl;
}

void testDOTExport() {
    std::cout << "\n=== Test: DOT Export (Control Flow Graph) ===" << std::endl;
    
    ProfilerConfig config;
    config.trackControlFlow = true;
    Profiler profiler(config);
    
    // Build a simple CFG
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordInstructionExecution(0x1010, "add r3 = r1, r4");
    profiler.recordBranch(0x1010, 0x2000, true, false);
    profiler.recordInstructionExecution(0x2000, "sub r5 = r6, r7");
    
    std::string dot = profiler.exportControlFlowGraphDOT();
    
    assertTrue(dot.length() > 0, "DOT export produces output");
    assertTrue(dot.find("digraph") != std::string::npos, "DOT contains digraph declaration");
    assertTrue(dot.find("node_1000") != std::string::npos, "DOT contains nodes");
    assertTrue(dot.find("->") != std::string::npos, "DOT contains edges");
    
    std::cout << "  DOT export length: " << dot.length() << " bytes" << std::endl;
}

void testCSVExports() {
    std::cout << "\n=== Test: CSV Exports ===" << std::endl;
    
    ProfilerConfig config;
    config.stackStart = 0x7FFF0000;
    config.stackEnd = 0x80000000;
    Profiler profiler(config);
    profiler.setStackRegion(config.stackStart, config.stackEnd);
    
    // Add data
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordGeneralRegisterRead(1);
    profiler.recordGeneralRegisterWrite(2);
    profiler.recordMemoryRead(0x7FFF1000, 8);
    
    // Test instruction frequency CSV
    std::string instrCSV = profiler.exportInstructionFrequencyCSV();
    assertTrue(instrCSV.length() > 0, "Instruction frequency CSV export");
    assertTrue(instrCSV.find("Address,Count,Percentage") != std::string::npos, "CSV has header");
    
    // Test register pressure CSV
    std::string regCSV = profiler.exportRegisterPressureCSV();
    assertTrue(regCSV.length() > 0, "Register pressure CSV export");
    assertTrue(regCSV.find("Register,Type,Reads,Writes") != std::string::npos, "CSV has header");
    
    // Test memory access CSV
    std::string memCSV = profiler.exportMemoryAccessCSV();
    assertTrue(memCSV.length() > 0, "Memory access CSV export");
    assertTrue(memCSV.find("Timestamp,Address,Size,Type,Region") != std::string::npos, "CSV has header");
    
    std::cout << "  Instruction CSV: " << instrCSV.length() << " bytes" << std::endl;
    std::cout << "  Register CSV: " << regCSV.length() << " bytes" << std::endl;
    std::cout << "  Memory CSV: " << memCSV.length() << " bytes" << std::endl;
}

void testProfilerEnableDisable() {
    std::cout << "\n=== Test: Profiler Enable/Disable ===" << std::endl;
    
    Profiler profiler;
    
    assertTrue(profiler.isEnabled(), "Profiler enabled by default");
    
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    assertTrue(profiler.getTotalInstructions() == 1, "Profiler records when enabled");
    
    profiler.disable();
    assertTrue(!profiler.isEnabled(), "Profiler disabled");
    
    profiler.recordInstructionExecution(0x1010, "add r3 = r1, r4");
    assertTrue(profiler.getTotalInstructions() == 1, "Profiler doesn't record when disabled");
    
    profiler.enable();
    profiler.recordInstructionExecution(0x1020, "sub r5 = r6, r7");
    assertTrue(profiler.getTotalInstructions() == 2, "Profiler records when re-enabled");
}

void testProfilerReset() {
    std::cout << "\n=== Test: Profiler Reset ===" << std::endl;
    
    Profiler profiler;
    
    profiler.recordInstructionExecution(0x1000, "mov r1 = r2");
    profiler.recordGeneralRegisterRead(1);
    profiler.recordMemoryRead(0x1000, 8);
    
    assertTrue(profiler.getTotalInstructions() == 1, "Data recorded before reset");
    
    profiler.reset();
    
    assertTrue(profiler.getTotalInstructions() == 0, "Total instructions reset");
    assertTrue(profiler.getUniqueInstructionCount() == 0, "Unique instructions reset");
    assertTrue(profiler.getTotalMemoryReads() == 0, "Memory reads reset");
}

void testCompleteWorkflow() {
    std::cout << "\n=== Test: Complete Profiling Workflow ===" << std::endl;
    
    ProfilerConfig config;
    config.trackInstructionFrequency = true;
    config.trackExecutionPaths = true;
    config.trackRegisterPressure = true;
    config.trackMemoryAccess = true;
    config.trackControlFlow = true;
    config.hotPathThreshold = 5;
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
    
    // Simulate a simple program execution
    for (int i = 0; i < 10; i++) {
        // Loop body
        profiler.recordInstructionExecution(0x401000, "ld8 r1 = [r10]");
        profiler.recordMemoryRead(0x10001000 + i * 8, 8);  // Heap read
        profiler.recordGeneralRegisterRead(10);
        profiler.recordGeneralRegisterWrite(1);
        
        profiler.recordInstructionExecution(0x401010, "add r1 = r1, r2");
        profiler.recordGeneralRegisterRead(1);
        profiler.recordGeneralRegisterRead(2);
        profiler.recordGeneralRegisterWrite(1);
        
        profiler.recordInstructionExecution(0x401020, "st8 [r11] = r1");
        profiler.recordMemoryWrite(0x7FFF1000 + i * 8, 8);  // Stack write
        profiler.recordGeneralRegisterRead(11);
        profiler.recordGeneralRegisterRead(1);
        
        profiler.recordInstructionExecution(0x401030, "cmp.lt p1, p2 = r12, r13");
        profiler.recordGeneralRegisterRead(12);
        profiler.recordGeneralRegisterRead(13);
        profiler.recordPredicateRegisterWrite(1);
        profiler.recordPredicateRegisterWrite(2);
        
        profiler.recordBranch(0x401030, 0x401000, i < 9, true);  // Branch back to loop start
    }
    
    // Export all formats
    std::string json = profiler.exportToJSON();
    std::string dot = profiler.exportControlFlowGraphDOT();
    std::string instrCSV = profiler.exportInstructionFrequencyCSV();
    std::string regCSV = profiler.exportRegisterPressureCSV();
    std::string memCSV = profiler.exportMemoryAccessCSV();
    
    assertTrue(profiler.getTotalInstructions() == 40, "Complete workflow instruction count");
    assertTrue(profiler.getTotalMemoryReads() == 10, "Complete workflow memory reads");
    assertTrue(profiler.getTotalMemoryWrites() == 10, "Complete workflow memory writes");
    
    const auto& memStats = profiler.getMemoryStats();
    assertTrue(memStats.at(MemoryRegionType::HEAP).readCount == 10, "Heap accesses classified");
    assertTrue(memStats.at(MemoryRegionType::STACK).writeCount == 10, "Stack accesses classified");
    
    std::cout << "  Total instructions: " << profiler.getTotalInstructions() << std::endl;
    std::cout << "  Unique instructions: " << profiler.getUniqueInstructionCount() << std::endl;
    std::cout << "  Memory reads: " << profiler.getTotalMemoryReads() << std::endl;
    std::cout << "  Memory writes: " << profiler.getTotalMemoryWrites() << std::endl;
    std::cout << "  JSON export: " << json.length() << " bytes" << std::endl;
    std::cout << "  DOT export: " << dot.length() << " bytes" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "IA-64 Profiler Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;
    
    try {
        testInstructionFrequencyTracking();
        testRegisterPressureTracking();
        testMemoryAccessClassification();
        testControlFlowGraphBuilding();
        testHotPathDetection();
        testJSONExport();
        testDOTExport();
        testCSVExports();
        testProfilerEnableDisable();
        testProfilerReset();
        testCompleteWorkflow();
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "ALL TESTS PASSED!" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "\n========================================" << std::endl;
        std::cerr << "TEST FAILED WITH EXCEPTION:" << std::endl;
        std::cerr << e.what() << std::endl;
        std::cerr << "========================================" << std::endl;
        return 1;
    }
}

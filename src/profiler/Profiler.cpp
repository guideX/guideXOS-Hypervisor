#include "Profiler.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace ia64 {

Profiler::Profiler(const ProfilerConfig& config)
    : config_(config)
    , enabled_(true)
    , currentTimestamp_(0)
    , instructionStats_()
    , totalInstructions_(0)
    , currentPath_()
    , pathFrequency_()
    , executionHistory_()
    , registerPressure_()
    , registerPressureHistory_()
    , memoryAccessHistory_()
    , memoryRegionStats_()
    , cfgNodes_()
    , cfgEdges_()
    , lastExecutedAddress_(0) {
}

void Profiler::setStackRegion(uint64_t start, uint64_t end) {
    config_.stackStart = start;
    config_.stackEnd = end;
}

void Profiler::setHeapRegion(uint64_t start, uint64_t end) {
    config_.heapStart = start;
    config_.heapEnd = end;
}

void Profiler::setCodeRegion(uint64_t start, uint64_t end) {
    config_.codeStart = start;
    config_.codeEnd = end;
}

void Profiler::reset() {
    currentTimestamp_ = 0;
    instructionStats_.clear();
    totalInstructions_ = 0;
    currentPath_.clear();
    pathFrequency_.clear();
    executionHistory_.clear();
    registerPressure_.reset();
    registerPressureHistory_.clear();
    memoryAccessHistory_.clear();
    memoryRegionStats_.clear();
    cfgNodes_.clear();
    cfgEdges_.clear();
    lastExecutedAddress_ = 0;
}

// ============================================================================
// Instruction Tracking
// ============================================================================

void Profiler::recordInstructionExecution(uint64_t address, const std::string& disassembly) {
    if (!enabled_) return;
    
    totalInstructions_++;
    incrementTimestamp();
    
    // Track instruction frequency
    if (config_.trackInstructionFrequency) {
        auto it = instructionStats_.find(address);
        if (it == instructionStats_.end()) {
            instructionStats_[address] = InstructionStats(address, disassembly);
        }
        instructionStats_[address].executionCount++;
        instructionStats_[address].lastExecutionTimestamp = getCurrentTimestamp();
    }
    
    // Update control flow graph
    if (config_.trackControlFlow && lastExecutedAddress_ != 0) {
        // Sequential execution (fall-through)
        updateControlFlowGraph(lastExecutedAddress_, address, false, false);
    }
    
    // Update execution path
    if (config_.trackExecutionPaths) {
        updateExecutionPath(address);
    }
    
    lastExecutedAddress_ = address;
}

void Profiler::recordBranch(uint64_t fromAddress, uint64_t toAddress, bool taken, bool isConditional) {
    if (!enabled_) return;
    
    if (config_.trackControlFlow) {
        if (taken) {
            updateControlFlowGraph(fromAddress, toAddress, true, isConditional);
        } else if (isConditional) {
            // Conditional branch not taken - record fall-through edge
            updateControlFlowGraph(fromAddress, fromAddress + 16, true, isConditional);
        }
    }
}

// ============================================================================
// Register Tracking
// ============================================================================

void Profiler::recordGeneralRegisterRead(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.grReadCount.size()) {
        registerPressure_.grReadCount[index]++;
    }
}

void Profiler::recordGeneralRegisterWrite(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.grWriteCount.size()) {
        registerPressure_.grWriteCount[index]++;
    }
}

void Profiler::recordFloatRegisterRead(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.frReadCount.size()) {
        registerPressure_.frReadCount[index]++;
    }
}

void Profiler::recordFloatRegisterWrite(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.frWriteCount.size()) {
        registerPressure_.frWriteCount[index]++;
    }
}

void Profiler::recordPredicateRegisterRead(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.prReadCount.size()) {
        registerPressure_.prReadCount[index]++;
    }
}

void Profiler::recordPredicateRegisterWrite(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.prWriteCount.size()) {
        registerPressure_.prWriteCount[index]++;
    }
}

void Profiler::recordBranchRegisterRead(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.brReadCount.size()) {
        registerPressure_.brReadCount[index]++;
    }
}

void Profiler::recordBranchRegisterWrite(size_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    if (index < registerPressure_.brWriteCount.size()) {
        registerPressure_.brWriteCount[index]++;
    }
}

void Profiler::recordApplicationRegisterRead(uint64_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    registerPressure_.arReadCount[index]++;
}

void Profiler::recordApplicationRegisterWrite(uint64_t index) {
    if (!enabled_ || !config_.trackRegisterPressure) return;
    registerPressure_.arWriteCount[index]++;
}

// ============================================================================
// Memory Access Tracking
// ============================================================================

void Profiler::recordMemoryRead(uint64_t address, size_t size) {
    if (!enabled_ || !config_.trackMemoryAccess) return;
    
    MemoryRegionType regionType = classifyMemoryAddress(address);
    MemoryAccessInfo access(address, size, false, regionType, getCurrentTimestamp());
    
    // Add to history
    memoryAccessHistory_.push_back(access);
    if (memoryAccessHistory_.size() > config_.maxMemoryAccessHistory) {
        memoryAccessHistory_.pop_front();
    }
    
    // Update statistics
    memoryRegionStats_[regionType].recordAccess(access);
}

void Profiler::recordMemoryWrite(uint64_t address, size_t size) {
    if (!enabled_ || !config_.trackMemoryAccess) return;
    
    MemoryRegionType regionType = classifyMemoryAddress(address);
    MemoryAccessInfo access(address, size, true, regionType, getCurrentTimestamp());
    
    // Add to history
    memoryAccessHistory_.push_back(access);
    if (memoryAccessHistory_.size() > config_.maxMemoryAccessHistory) {
        memoryAccessHistory_.pop_front();
    }
    
    // Update statistics
    memoryRegionStats_[regionType].recordAccess(access);
}

MemoryRegionType Profiler::classifyMemoryAddress(uint64_t address) const {
    // Check stack region
    if (config_.stackStart != 0 && config_.stackEnd != 0) {
        if (address >= config_.stackStart && address < config_.stackEnd) {
            return MemoryRegionType::STACK;
        }
    }
    
    // Check heap region
    if (config_.heapStart != 0 && config_.heapEnd != 0) {
        if (address >= config_.heapStart && address < config_.heapEnd) {
            return MemoryRegionType::HEAP;
        }
    }
    
    // Check code region
    if (config_.codeStart != 0 && config_.codeEnd != 0) {
        if (address >= config_.codeStart && address < config_.codeEnd) {
            return MemoryRegionType::CODE;
        }
    }
    
    return MemoryRegionType::UNKNOWN;
}

// ============================================================================
// Control Flow Graph
// ============================================================================

void Profiler::updateControlFlowGraph(uint64_t fromAddress, uint64_t toAddress, bool isBranch, bool isConditional) {
    // Update or create edge
    auto edgeKey = std::make_pair(fromAddress, toAddress);
    auto it = cfgEdges_.find(edgeKey);
    
    if (it != cfgEdges_.end()) {
        it->second.executionCount++;
    } else {
        cfgEdges_[edgeKey] = CFGEdge(fromAddress, toAddress, isBranch, isConditional);
    }
    
    // Update or create nodes
    if (cfgNodes_.find(fromAddress) == cfgNodes_.end()) {
        cfgNodes_[fromAddress] = CFGNode(fromAddress);
    }
    
    if (cfgNodes_.find(toAddress) == cfgNodes_.end()) {
        cfgNodes_[toAddress] = CFGNode(toAddress);
    }
    
    cfgNodes_[fromAddress].executionCount++;
}

// ============================================================================
// Execution Path Tracking
// ============================================================================

void Profiler::updateExecutionPath(uint64_t address) {
    currentPath_.push_back(address);
    
    // Limit path length
    if (currentPath_.size() > config_.maxPathLength) {
        currentPath_.pop_front();
    }
    
    // Record path segments for hot path analysis
    if (currentPath_.size() >= 3) {
        std::vector<uint64_t> pathSegment;
        size_t startIdx = currentPath_.size() >= 5 ? currentPath_.size() - 5 : 0;
        for (size_t i = startIdx; i < currentPath_.size(); i++) {
            pathSegment.push_back(currentPath_[i]);
        }
        pathFrequency_[pathSegment]++;
    }
}

// ============================================================================
// Query Methods
// ============================================================================

uint64_t Profiler::getInstructionCount(uint64_t address) const {
    auto it = instructionStats_.find(address);
    if (it != instructionStats_.end()) {
        return it->second.executionCount;
    }
    return 0;
}

std::vector<std::pair<uint64_t, uint64_t>> Profiler::getTopInstructions(size_t count) const {
    std::vector<std::pair<uint64_t, uint64_t>> result;
    
    for (const auto& pair : instructionStats_) {
        result.push_back(std::make_pair(pair.first, pair.second.executionCount));
    }
    
    std::sort(result.begin(), result.end(),
        [](const std::pair<uint64_t, uint64_t>& a, const std::pair<uint64_t, uint64_t>& b) {
            return a.second > b.second;
        });
    
    if (result.size() > count) {
        result.resize(count);
    }
    
    return result;
}

std::vector<HotPath> Profiler::getHotPaths(size_t count) const {
    std::vector<HotPath> result;
    
    for (const auto& pair : pathFrequency_) {
        if (pair.second >= config_.hotPathThreshold) {
            HotPath hp;
            hp.addresses = pair.first;
            hp.executionCount = pair.second;
            hp.totalInstructions = pair.first.size() * pair.second;
            result.push_back(hp);
        }
    }
    
    std::sort(result.begin(), result.end(),
        [](const HotPath& a, const HotPath& b) {
            return a.executionCount > b.executionCount;
        });
    
    if (result.size() > count) {
        result.resize(count);
    }
    
    return result;
}

uint64_t Profiler::getTotalMemoryReads() const {
    uint64_t total = 0;
    for (const auto& pair : memoryRegionStats_) {
        total += pair.second.readCount;
    }
    return total;
}

uint64_t Profiler::getTotalMemoryWrites() const {
    uint64_t total = 0;
    for (const auto& pair : memoryRegionStats_) {
        total += pair.second.writeCount;
    }
    return total;
}

// ============================================================================
// Export - JSON
// ============================================================================

std::string Profiler::exportToJSON() const {
    std::ostringstream oss;
    oss << "{\n";
    
    // General statistics
    oss << "  \"summary\": {\n";
    oss << "    \"totalInstructions\": " << totalInstructions_ << ",\n";
    oss << "    \"uniqueInstructions\": " << instructionStats_.size() << ",\n";
    oss << "    \"totalMemoryReads\": " << getTotalMemoryReads() << ",\n";
    oss << "    \"totalMemoryWrites\": " << getTotalMemoryWrites() << "\n";
    oss << "  },\n";
    
    // Top instructions
    oss << "  \"topInstructions\": [\n";
    auto topInstr = getTopInstructions(20);
    for (size_t i = 0; i < topInstr.size(); i++) {
        oss << "    {\n";
        oss << "      \"address\": \"0x" << std::hex << topInstr[i].first << std::dec << "\",\n";
        oss << "      \"count\": " << topInstr[i].second;
        auto it = instructionStats_.find(topInstr[i].first);
        if (it != instructionStats_.end()) {
            oss << ",\n      \"disassembly\": \"" << it->second.disassembly << "\"";
        }
        oss << "\n    }";
        if (i < topInstr.size() - 1) oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";
    
    // Memory statistics by region
    oss << "  \"memoryStats\": {\n";
    const char* regionNames[] = {"stack", "heap", "code", "unknown"};
    int regionIdx = 0;
    for (int i = 0; i < 4; i++) {
        MemoryRegionType type = static_cast<MemoryRegionType>(i);
        auto it = memoryRegionStats_.find(type);
        
        if (regionIdx > 0) oss << ",\n";
        oss << "    \"" << regionNames[i] << "\": {\n";
        
        if (it != memoryRegionStats_.end()) {
            oss << "      \"reads\": " << it->second.readCount << ",\n";
            oss << "      \"writes\": " << it->second.writeCount << ",\n";
            oss << "      \"totalBytes\": " << it->second.totalBytes << ",\n";
            oss << "      \"uniqueAddresses\": " << it->second.uniqueAddresses << "\n";
        } else {
            oss << "      \"reads\": 0,\n";
            oss << "      \"writes\": 0,\n";
            oss << "      \"totalBytes\": 0,\n";
            oss << "      \"uniqueAddresses\": 0\n";
        }
        
        oss << "    }";
        regionIdx++;
    }
    oss << "\n  },\n";
    
    // Register pressure summary
    oss << "  \"registerPressure\": {\n";
    
    // Find most used general registers
    uint64_t maxGrUse = 0;
    size_t maxGrIdx = 0;
    for (size_t i = 0; i < registerPressure_.grReadCount.size(); i++) {
        uint64_t use = registerPressure_.grReadCount[i] + registerPressure_.grWriteCount[i];
        if (use > maxGrUse) {
            maxGrUse = use;
            maxGrIdx = i;
        }
    }
    
    oss << "    \"mostUsedGR\": " << maxGrIdx << ",\n";
    oss << "    \"mostUsedGRCount\": " << maxGrUse << ",\n";
    
    // General register usage count
    size_t grUsed = 0;
    for (size_t i = 0; i < registerPressure_.grReadCount.size(); i++) {
        if (registerPressure_.grReadCount[i] > 0 || registerPressure_.grWriteCount[i] > 0) {
            grUsed++;
        }
    }
    oss << "    \"generalRegistersUsed\": " << grUsed << "\n";
    
    oss << "  },\n";
    
    // Hot paths
    oss << "  \"hotPaths\": [\n";
    auto hotPaths = getHotPaths(10);
    for (size_t i = 0; i < hotPaths.size(); i++) {
        oss << "    {\n";
        oss << "      \"executionCount\": " << hotPaths[i].executionCount << ",\n";
        oss << "      \"length\": " << hotPaths[i].addresses.size() << ",\n";
        oss << "      \"addresses\": [";
        for (size_t j = 0; j < hotPaths[i].addresses.size(); j++) {
            oss << "\"0x" << std::hex << hotPaths[i].addresses[j] << std::dec << "\"";
            if (j < hotPaths[i].addresses.size() - 1) oss << ", ";
        }
        oss << "]\n";
        oss << "    }";
        if (i < hotPaths.size() - 1) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n";
    
    oss << "}\n";
    
    return oss.str();
}

// ============================================================================
// Export - DOT (Control Flow Graph)
// ============================================================================

std::string Profiler::exportControlFlowGraphDOT() const {
    std::ostringstream oss;
    
    oss << "digraph ControlFlowGraph {\n";
    oss << "  rankdir=TB;\n";
    oss << "  node [shape=box, style=filled, fillcolor=lightblue];\n\n";
    
    // Nodes
    for (const auto& pair : cfgNodes_) {
        const CFGNode& node = pair.second;
        oss << "  node_" << std::hex << node.startAddress << std::dec;
        oss << " [label=\"0x" << std::hex << node.startAddress << std::dec;
        oss << "\\nCount: " << node.executionCount << "\"";
        
        // Color hot nodes
        if (node.executionCount > config_.hotPathThreshold) {
            oss << ", fillcolor=orange";
        }
        
        oss << "];\n";
    }
    
    oss << "\n";
    
    // Edges
    for (const auto& pair : cfgEdges_) {
        const CFGEdge& edge = pair.second;
        oss << "  node_" << std::hex << edge.fromAddress << std::dec;
        oss << " -> node_" << std::hex << edge.toAddress << std::dec;
        oss << " [label=\"" << edge.executionCount << "\"";
        
        // Style conditional branches differently
        if (edge.isBranch) {
            if (edge.isConditional) {
                oss << ", color=red, style=dashed";
            } else {
                oss << ", color=blue, style=bold";
            }
        }
        
        // Thicker lines for hot edges
        if (edge.executionCount > config_.hotPathThreshold) {
            oss << ", penwidth=3.0";
        }
        
        oss << "];\n";
    }
    
    oss << "}\n";
    
    return oss.str();
}

// ============================================================================
// Export - CSV (Memory Access)
// ============================================================================

std::string Profiler::exportMemoryAccessCSV() const {
    std::ostringstream oss;
    
    oss << "Timestamp,Address,Size,Type,Region\n";
    
    for (const auto& access : memoryAccessHistory_) {
        oss << access.timestamp << ",";
        oss << "0x" << std::hex << access.address << std::dec << ",";
        oss << access.size << ",";
        oss << (access.isWrite ? "WRITE" : "READ") << ",";
        
        switch (access.regionType) {
            case MemoryRegionType::STACK: oss << "STACK"; break;
            case MemoryRegionType::HEAP: oss << "HEAP"; break;
            case MemoryRegionType::CODE: oss << "CODE"; break;
            case MemoryRegionType::UNKNOWN: oss << "UNKNOWN"; break;
        }
        
        oss << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// Export - CSV (Register Pressure)
// ============================================================================

std::string Profiler::exportRegisterPressureCSV() const {
    std::ostringstream oss;
    
    oss << "Register,Type,Reads,Writes,Total\n";
    
    // General registers
    for (size_t i = 0; i < registerPressure_.grReadCount.size(); i++) {
        uint64_t reads = registerPressure_.grReadCount[i];
        uint64_t writes = registerPressure_.grWriteCount[i];
        if (reads > 0 || writes > 0) {
            oss << "GR" << i << ",GENERAL," << reads << "," << writes << "," << (reads + writes) << "\n";
        }
    }
    
    // Floating-point registers
    for (size_t i = 0; i < registerPressure_.frReadCount.size(); i++) {
        uint64_t reads = registerPressure_.frReadCount[i];
        uint64_t writes = registerPressure_.frWriteCount[i];
        if (reads > 0 || writes > 0) {
            oss << "FR" << i << ",FLOAT," << reads << "," << writes << "," << (reads + writes) << "\n";
        }
    }
    
    // Predicate registers
    for (size_t i = 0; i < registerPressure_.prReadCount.size(); i++) {
        uint64_t reads = registerPressure_.prReadCount[i];
        uint64_t writes = registerPressure_.prWriteCount[i];
        if (reads > 0 || writes > 0) {
            oss << "PR" << i << ",PREDICATE," << reads << "," << writes << "," << (reads + writes) << "\n";
        }
    }
    
    // Branch registers
    for (size_t i = 0; i < registerPressure_.brReadCount.size(); i++) {
        uint64_t reads = registerPressure_.brReadCount[i];
        uint64_t writes = registerPressure_.brWriteCount[i];
        if (reads > 0 || writes > 0) {
            oss << "BR" << i << ",BRANCH," << reads << "," << writes << "," << (reads + writes) << "\n";
        }
    }
    
    // Application registers
    for (const auto& pair : registerPressure_.arReadCount) {
        uint64_t reads = pair.second;
        uint64_t writes = 0;
        auto it = registerPressure_.arWriteCount.find(pair.first);
        if (it != registerPressure_.arWriteCount.end()) {
            writes = it->second;
        }
        oss << "AR" << pair.first << ",APPLICATION," << reads << "," << writes << "," << (reads + writes) << "\n";
    }
    
    return oss.str();
}

// ============================================================================
// Export - CSV (Instruction Frequency)
// ============================================================================

std::string Profiler::exportInstructionFrequencyCSV() const {
    std::ostringstream oss;
    
    oss << "Address,Count,Percentage,Disassembly\n";
    
    // Sort by execution count
    std::vector<std::pair<uint64_t, InstructionStats>> sorted;
    for (const auto& pair : instructionStats_) {
        sorted.push_back(std::make_pair(pair.first, pair.second));
    }
    
    std::sort(sorted.begin(), sorted.end(),
        [](const std::pair<uint64_t, InstructionStats>& a, const std::pair<uint64_t, InstructionStats>& b) {
            return a.second.executionCount > b.second.executionCount;
        });
    
    for (const auto& pair : sorted) {
        const InstructionStats& stats = pair.second;
        double percentage = (totalInstructions_ > 0) ? 
            (static_cast<double>(stats.executionCount) / totalInstructions_ * 100.0) : 0.0;
        
        oss << "0x" << std::hex << stats.address << std::dec << ",";
        oss << stats.executionCount << ",";
        oss << std::fixed << std::setprecision(2) << percentage << ",";
        oss << "\"" << stats.disassembly << "\"\n";
    }
    
    return oss.str();
}

} // namespace ia64

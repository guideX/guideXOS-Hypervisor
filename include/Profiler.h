#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <unordered_map>
#include <set>
#include <deque>
#include <memory>
#include <chrono>

namespace ia64 {

// Forward declarations
struct CPUState;
class IMemory;

/**
 * Memory access classification
 */
enum class MemoryRegionType {
    STACK,      // Stack region access
    HEAP,       // Heap region access
    CODE,       // Code/text region access
    UNKNOWN     // Unknown or unclassified region
};

/**
 * Memory access pattern tracking
 */
struct MemoryAccessInfo {
    uint64_t address;
    size_t size;
    bool isWrite;
    MemoryRegionType regionType;
    uint64_t timestamp;
    
    MemoryAccessInfo()
        : address(0)
        , size(0)
        , isWrite(false)
        , regionType(MemoryRegionType::UNKNOWN)
        , timestamp(0) {}
        
    MemoryAccessInfo(uint64_t addr, size_t sz, bool write, MemoryRegionType region, uint64_t ts)
        : address(addr)
        , size(sz)
        , isWrite(write)
        , regionType(region)
        , timestamp(ts) {}
};

/**
 * Memory access statistics per region type
 */
struct MemoryRegionStats {
    uint64_t readCount;
    uint64_t writeCount;
    uint64_t totalBytes;
    uint64_t uniqueAddresses;
    std::set<uint64_t> addressSet;
    
    MemoryRegionStats()
        : readCount(0)
        , writeCount(0)
        , totalBytes(0)
        , uniqueAddresses(0)
        , addressSet() {}
        
    void recordAccess(const MemoryAccessInfo& access) {
        if (access.isWrite) {
            writeCount++;
        } else {
            readCount++;
        }
        totalBytes += access.size;
        addressSet.insert(access.address);
        uniqueAddresses = addressSet.size();
    }
};

/**
 * Register pressure tracking
 */
struct RegisterPressure {
    // General registers
    std::vector<uint64_t> grReadCount;    // Per-register read count
    std::vector<uint64_t> grWriteCount;   // Per-register write count
    
    // Floating-point registers
    std::vector<uint64_t> frReadCount;
    std::vector<uint64_t> frWriteCount;
    
    // Predicate registers
    std::vector<uint64_t> prReadCount;
    std::vector<uint64_t> prWriteCount;
    
    // Branch registers
    std::vector<uint64_t> brReadCount;
    std::vector<uint64_t> brWriteCount;
    
    // Application registers
    std::map<uint64_t, uint64_t> arReadCount;
    std::map<uint64_t, uint64_t> arWriteCount;
    
    RegisterPressure()
        : grReadCount(128, 0)
        , grWriteCount(128, 0)
        , frReadCount(128, 0)
        , frWriteCount(128, 0)
        , prReadCount(64, 0)
        , prWriteCount(64, 0)
        , brReadCount(8, 0)
        , brWriteCount(8, 0)
        , arReadCount()
        , arWriteCount() {}
    
    void reset() {
        std::fill(grReadCount.begin(), grReadCount.end(), 0);
        std::fill(grWriteCount.begin(), grWriteCount.end(), 0);
        std::fill(frReadCount.begin(), frReadCount.end(), 0);
        std::fill(frWriteCount.begin(), frWriteCount.end(), 0);
        std::fill(prReadCount.begin(), prReadCount.end(), 0);
        std::fill(prWriteCount.begin(), prWriteCount.end(), 0);
        std::fill(brReadCount.begin(), brReadCount.end(), 0);
        std::fill(brWriteCount.begin(), brWriteCount.end(), 0);
        arReadCount.clear();
        arWriteCount.clear();
    }
};

/**
 * Control flow graph edge
 */
struct CFGEdge {
    uint64_t fromAddress;
    uint64_t toAddress;
    uint64_t executionCount;
    bool isBranch;
    bool isConditional;
    
    CFGEdge()
        : fromAddress(0)
        , toAddress(0)
        , executionCount(0)
        , isBranch(false)
        , isConditional(false) {}
        
    CFGEdge(uint64_t from, uint64_t to, bool branch, bool conditional)
        : fromAddress(from)
        , toAddress(to)
        , executionCount(1)
        , isBranch(branch)
        , isConditional(conditional) {}
};

/**
 * Control flow graph node (basic block)
 */
struct CFGNode {
    uint64_t startAddress;
    uint64_t endAddress;
    uint64_t executionCount;
    std::vector<uint64_t> instructions;
    
    CFGNode()
        : startAddress(0)
        , endAddress(0)
        , executionCount(0)
        , instructions() {}
        
    CFGNode(uint64_t start)
        : startAddress(start)
        , endAddress(start)
        , executionCount(0)
        , instructions() {}
};

/**
 * Instruction execution statistics
 */
struct InstructionStats {
    uint64_t address;
    std::string disassembly;
    uint64_t executionCount;
    uint64_t lastExecutionTimestamp;
    
    InstructionStats()
        : address(0)
        , disassembly()
        , executionCount(0)
        , lastExecutionTimestamp(0) {}
        
    InstructionStats(uint64_t addr, const std::string& disasm)
        : address(addr)
        , disassembly(disasm)
        , executionCount(0)
        , lastExecutionTimestamp(0) {}
};

/**
 * Execution path segment
 */
struct ExecutionPathSegment {
    uint64_t startAddress;
    uint64_t endAddress;
    uint64_t timestamp;
    size_t instructionCount;
    
    ExecutionPathSegment()
        : startAddress(0)
        , endAddress(0)
        , timestamp(0)
        , instructionCount(0) {}
        
    ExecutionPathSegment(uint64_t start, uint64_t end, uint64_t ts, size_t count)
        : startAddress(start)
        , endAddress(end)
        , timestamp(ts)
        , instructionCount(count) {}
};

/**
 * Hot path tracking
 */
struct HotPath {
    std::vector<uint64_t> addresses;
    uint64_t executionCount;
    uint64_t totalInstructions;
    
    HotPath()
        : addresses()
        , executionCount(0)
        , totalInstructions(0) {}
};

/**
 * Profiler configuration
 */
struct ProfilerConfig {
    bool trackInstructionFrequency;
    bool trackExecutionPaths;
    bool trackRegisterPressure;
    bool trackMemoryAccess;
    bool trackControlFlow;
    
    size_t maxPathLength;              // Maximum execution path length to track
    size_t maxMemoryAccessHistory;     // Maximum memory access history to keep
    size_t hotPathThreshold;           // Minimum execution count to consider a path "hot"
    
    // Memory region bounds for classification
    uint64_t stackStart;
    uint64_t stackEnd;
    uint64_t heapStart;
    uint64_t heapEnd;
    uint64_t codeStart;
    uint64_t codeEnd;
    
    ProfilerConfig()
        : trackInstructionFrequency(true)
        , trackExecutionPaths(true)
        , trackRegisterPressure(true)
        , trackMemoryAccess(true)
        , trackControlFlow(true)
        , maxPathLength(1000)
        , maxMemoryAccessHistory(10000)
        , hotPathThreshold(100)
        , stackStart(0)
        , stackEnd(0)
        , heapStart(0)
        , heapEnd(0)
        , codeStart(0)
        , codeEnd(0) {}
};

/**
 * Profiler - Runtime profiling and analysis system
 * 
 * Tracks:
 * - Instruction execution frequency
 * - Hot execution paths
 * - Register pressure over time
 * - Memory access patterns
 * - Control flow graphs
 * 
 * Exports:
 * - JSON format for statistics
 * - DOT format for control flow visualization
 * - CSV format for time-series data
 */
class Profiler {
public:
    explicit Profiler(const ProfilerConfig& config = ProfilerConfig());
    ~Profiler() = default;
    
    // Configuration
    void setConfig(const ProfilerConfig& config) { config_ = config; }
    const ProfilerConfig& getConfig() const { return config_; }
    
    // Memory region configuration
    void setStackRegion(uint64_t start, uint64_t end);
    void setHeapRegion(uint64_t start, uint64_t end);
    void setCodeRegion(uint64_t start, uint64_t end);
    
    // Profiling control
    void enable() { enabled_ = true; }
    void disable() { enabled_ = false; }
    bool isEnabled() const { return enabled_; }
    void reset();
    
    // Instruction tracking
    void recordInstructionExecution(uint64_t address, const std::string& disassembly);
    void recordBranch(uint64_t fromAddress, uint64_t toAddress, bool taken, bool isConditional);
    
    // Register tracking
    void recordGeneralRegisterRead(size_t index);
    void recordGeneralRegisterWrite(size_t index);
    void recordFloatRegisterRead(size_t index);
    void recordFloatRegisterWrite(size_t index);
    void recordPredicateRegisterRead(size_t index);
    void recordPredicateRegisterWrite(size_t index);
    void recordBranchRegisterRead(size_t index);
    void recordBranchRegisterWrite(size_t index);
    void recordApplicationRegisterRead(uint64_t index);
    void recordApplicationRegisterWrite(uint64_t index);
    
    // Memory access tracking
    void recordMemoryRead(uint64_t address, size_t size);
    void recordMemoryWrite(uint64_t address, size_t size);
    
    // Query methods
    uint64_t getInstructionCount(uint64_t address) const;
    const std::map<uint64_t, InstructionStats>& getInstructionStats() const { return instructionStats_; }
    const RegisterPressure& getRegisterPressure() const { return registerPressure_; }
    const std::map<MemoryRegionType, MemoryRegionStats>& getMemoryStats() const { return memoryRegionStats_; }
    
    std::vector<std::pair<uint64_t, uint64_t>> getTopInstructions(size_t count) const;
    std::vector<HotPath> getHotPaths(size_t count) const;
    
    // Export functionality
    std::string exportToJSON() const;
    std::string exportControlFlowGraphDOT() const;
    std::string exportMemoryAccessCSV() const;
    std::string exportRegisterPressureCSV() const;
    std::string exportInstructionFrequencyCSV() const;
    
    // Statistics
    uint64_t getTotalInstructions() const { return totalInstructions_; }
    uint64_t getTotalMemoryReads() const;
    uint64_t getTotalMemoryWrites() const;
    uint64_t getUniqueInstructionCount() const { return instructionStats_.size(); }
    
private:
    // Helper methods
    MemoryRegionType classifyMemoryAddress(uint64_t address) const;
    void updateControlFlowGraph(uint64_t fromAddress, uint64_t toAddress, bool isBranch, bool isConditional);
    void updateExecutionPath(uint64_t address);
    uint64_t getCurrentTimestamp() const { return currentTimestamp_; }
    void incrementTimestamp() { currentTimestamp_++; }
    
    // Configuration
    ProfilerConfig config_;
    bool enabled_;
    
    // Timing
    uint64_t currentTimestamp_;
    
    // Instruction statistics
    std::map<uint64_t, InstructionStats> instructionStats_;
    uint64_t totalInstructions_;
    
    // Execution path tracking
    std::deque<uint64_t> currentPath_;
    std::map<std::vector<uint64_t>, uint64_t> pathFrequency_;
    std::deque<ExecutionPathSegment> executionHistory_;
    
    // Register pressure
    RegisterPressure registerPressure_;
    std::vector<RegisterPressure> registerPressureHistory_;
    
    // Memory access tracking
    std::deque<MemoryAccessInfo> memoryAccessHistory_;
    std::map<MemoryRegionType, MemoryRegionStats> memoryRegionStats_;
    
    // Control flow graph
    std::map<uint64_t, CFGNode> cfgNodes_;
    std::map<std::pair<uint64_t, uint64_t>, CFGEdge> cfgEdges_;
    uint64_t lastExecutedAddress_;
};

} // namespace ia64

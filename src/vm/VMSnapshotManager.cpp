#include "VMSnapshot.h"
#include "VMSnapshotManager.h"
#include "VirtualMachine.h"
#include "cpu.h"
#include "memory.h"
#include "Console.h"
#include "Timer.h"
#include "InterruptController.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>

namespace ia64 {

// ============================================================================
// Constructor / Destructor
// ============================================================================

VMSnapshotManager::VMSnapshotManager()
    : fullSnapshots_()
    , deltaSnapshots_()
    , snapshotOrder_()
    , nextSnapshotNumber_(1) {
}

VMSnapshotManager::~VMSnapshotManager() {
    clear();
}

// ============================================================================
// Snapshot Creation
// ============================================================================

std::string VMSnapshotManager::createFullSnapshot(
const VirtualMachine& vm,
const std::string& name,
const std::string& description) {

VMStateSnapshot snapshot = captureFullSnapshot(vm);
    
    // Set metadata
    snapshot.metadata.snapshotId = generateSnapshotId();
    snapshot.metadata.snapshotName = name.empty() 
        ? "Snapshot " + std::to_string(nextSnapshotNumber_++)
        : name;
    snapshot.metadata.description = description;
    snapshot.metadata.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    snapshot.metadata.isDelta = false;
    snapshot.metadata.parentSnapshotId = "";
    
    // Estimate snapshot size
    size_t size = 0;
    size += snapshot.memoryState.data.size();
    size += snapshot.cpus.size() * sizeof(CPUSnapshotRecord);
    size += 1024;  // Approximate overhead
    snapshot.metadata.fullSnapshotSize = size;
    snapshot.metadata.deltaSize = 0;
    
    // Store snapshot
    std::string id = snapshot.metadata.snapshotId;
    fullSnapshots_[id] = snapshot;
    snapshotOrder_.push_back(id);
    
    return id;
}

std::string VMSnapshotManager::createDeltaSnapshot(
    const VirtualMachine& vm,
    const std::string& parentSnapshotId,
    const std::string& name,
    const std::string& description) {

    // Check if parent exists
    if (fullSnapshots_.find(parentSnapshotId) == fullSnapshots_.end() &&
        deltaSnapshots_.find(parentSnapshotId) == deltaSnapshots_.end()) {
        return "";  // Parent not found
    }
    
    // Capture current state
    VMStateSnapshot currentSnapshot = captureFullSnapshot(vm);
    
    // Resolve parent to full snapshot
    VMStateSnapshot parentSnapshot = resolveSnapshot(parentSnapshotId);
    
    // Compute delta
    VMStateSnapshotDelta delta = computeDelta(currentSnapshot, parentSnapshot);
    
    // Set metadata
    delta.metadata.snapshotId = generateSnapshotId();
    delta.metadata.snapshotName = name.empty()
        ? "Delta Snapshot " + std::to_string(nextSnapshotNumber_++)
        : name;
    delta.metadata.description = description;
    delta.metadata.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    delta.metadata.isDelta = true;
    delta.metadata.parentSnapshotId = parentSnapshotId;
    
    // Estimate sizes
    size_t deltaSize = 0;
    deltaSize += delta.memoryDelta.totalChangedBytes;
    deltaSize += delta.cpuDeltas.size() * 512;  // Approximate per-CPU delta size
    delta.metadata.deltaSize = deltaSize;
    delta.metadata.fullSnapshotSize = currentSnapshot.metadata.fullSnapshotSize;
    
    // Store delta snapshot
    std::string id = delta.metadata.snapshotId;
    deltaSnapshots_[id] = delta;
    snapshotOrder_.push_back(id);
    
    return id;
}

// ============================================================================
// Snapshot Restoration
// ============================================================================

bool VMSnapshotManager::restoreSnapshot(
VirtualMachine& vm,
const std::string& snapshotId) {

// Resolve snapshot (handles both full and delta)
VMStateSnapshot snapshot = resolveSnapshot(snapshotId);
    
    if (snapshot.metadata.snapshotId.empty()) {
        return false;  // Snapshot not found
    }
    
    // Restore VM state (implementation delegates to VirtualMachine)
    // Note: This requires VirtualMachine to have a restoreFromSnapshot method
    // For now, we'll return true - actual restoration will be implemented
    // in the VirtualMachine integration step
    
    return true;
}

bool VMSnapshotManager::restoreLatestSnapshot(VirtualMachine& vm) {
    if (snapshotOrder_.empty()) {
        return false;
    }
    
    std::string latestId = snapshotOrder_.back();
    return restoreSnapshot(vm, latestId);
}

// ============================================================================
// Snapshot Management
// ============================================================================

bool VMSnapshotManager::deleteSnapshot(const std::string& snapshotId) {
    bool found = false;
    
    // Remove from full snapshots
    if (fullSnapshots_.erase(snapshotId) > 0) {
        found = true;
    }
    
    // Remove from delta snapshots
    if (deltaSnapshots_.erase(snapshotId) > 0) {
        found = true;
    }
    
    // Remove from order
    auto it = std::find(snapshotOrder_.begin(), snapshotOrder_.end(), snapshotId);
    if (it != snapshotOrder_.end()) {
        snapshotOrder_.erase(it);
    }
    
    return found;
}

const VMSnapshotMetadata* VMSnapshotManager::getSnapshotMetadata(
    const std::string& snapshotId) const {

    auto fullIt = fullSnapshots_.find(snapshotId);
    if (fullIt != fullSnapshots_.end()) {
        return &fullIt->second.metadata;
    }
    
    auto deltaIt = deltaSnapshots_.find(snapshotId);
    if (deltaIt != deltaSnapshots_.end()) {
        return &deltaIt->second.metadata;
    }
    
    return nullptr;
}

std::vector<VMSnapshotMetadata> VMSnapshotManager::listSnapshots() const {
    std::vector<VMSnapshotMetadata> result;
    result.reserve(snapshotOrder_.size());
    
    for (const auto& id : snapshotOrder_) {
        const VMSnapshotMetadata* metadata = getSnapshotMetadata(id);
        if (metadata) {
            result.push_back(*metadata);
        }
    }
    
    return result;
}

size_t VMSnapshotManager::getSnapshotCount() const {
    return fullSnapshots_.size() + deltaSnapshots_.size();
}

void VMSnapshotManager::clear() {
    fullSnapshots_.clear();
    deltaSnapshots_.clear();
    snapshotOrder_.clear();
    nextSnapshotNumber_ = 1;
}

// ============================================================================
// Snapshot Analysis
// ============================================================================

SnapshotCompressionStats VMSnapshotManager::getCompressionStats(
    const std::string& snapshotId) const {

    SnapshotCompressionStats stats;
    
    const VMSnapshotMetadata* metadata = getSnapshotMetadata(snapshotId);
    if (!metadata) {
        return stats;
    }
    
    stats.fullSnapshotSize = metadata->fullSnapshotSize;
    stats.deltaSnapshotSize = metadata->deltaSize;
    
    if (metadata->isDelta) {
        auto deltaIt = deltaSnapshots_.find(snapshotId);
        if (deltaIt != deltaSnapshots_.end()) {
            stats.memoryBytesChanged = deltaIt->second.memoryDelta.totalChangedBytes;
            stats.cpuRegistersChanged = 0;
            for (const auto& cpuDelta : deltaIt->second.cpuDeltas) {
                stats.cpuRegistersChanged += cpuDelta.changedGR.size();
                stats.cpuRegistersChanged += cpuDelta.changedFR.size();
                stats.cpuRegistersChanged += cpuDelta.changedPR.size();
                stats.cpuRegistersChanged += cpuDelta.changedBR.size();
                stats.cpuRegistersChanged += cpuDelta.changedAR.size();
            }
        }
    }
    
    if (stats.fullSnapshotSize > 0) {
        stats.compressionRatio = static_cast<double>(stats.deltaSnapshotSize) / 
                                static_cast<double>(stats.fullSnapshotSize);
    }
    
    return stats;
}

size_t VMSnapshotManager::getTotalMemoryUsage() const {
    size_t total = 0;
    
    for (const auto& pair : fullSnapshots_) {
        total += pair.second.metadata.fullSnapshotSize;
    }
    
    for (const auto& pair : deltaSnapshots_) {
        total += pair.second.metadata.deltaSize;
    }
    
    return total;
}

// ============================================================================
// Delta Computation
// ============================================================================

VMStateSnapshotDelta VMSnapshotManager::computeDelta(
    const VMStateSnapshot& current,
    const VMStateSnapshot& parent) {

    VMStateSnapshotDelta delta;
    
    // Compute memory delta
    delta.memoryDelta = computeMemoryDelta(current.memoryState, parent.memoryState);
    
    // Compute CPU deltas
    for (size_t i = 0; i < current.cpus.size() && i < parent.cpus.size(); ++i) {
        CPUStateDelta cpuDelta = computeCPUStateDelta(current.cpus[i], parent.cpus[i]);
        
        // Only store delta if there are changes
        if (cpuDelta.changedGR.size() > 0 || cpuDelta.changedFR.size() > 0 ||
            cpuDelta.changedPR.size() > 0 || cpuDelta.changedBR.size() > 0 ||
            cpuDelta.changedAR.size() > 0 || cpuDelta.ipChanged ||
            cpuDelta.cfmChanged || cpuDelta.psrChanged ||
            cpuDelta.executionStateChanged || cpuDelta.countersChanged) {
            delta.cpuDeltas.push_back(cpuDelta);
        }
    }
    
    // Check active CPU change
    if (current.activeCPUIndex != parent.activeCPUIndex) {
        delta.activeCPUChanged = true;
        delta.newActiveCPUIndex = current.activeCPUIndex;
    }
    
    // Check console state change
    if (current.consoleState.currentBuffer != parent.consoleState.currentBuffer ||
        current.consoleState.totalBytesWritten != parent.consoleState.totalBytesWritten) {
        delta.consoleStateChanged = true;
        delta.newConsoleState = current.consoleState;
    }
    
    // Check timer state change
    if (current.timerState.intervalCycles != parent.timerState.intervalCycles ||
        current.timerState.elapsedCycles != parent.timerState.elapsedCycles ||
        current.timerState.enabled != parent.timerState.enabled ||
        current.timerState.periodic != parent.timerState.periodic ||
        current.timerState.interruptPending != parent.timerState.interruptPending) {
        delta.timerStateChanged = true;
        delta.newTimerState = current.timerState;
    }
    
    // Check VM state change
    if (current.vmStateValue != parent.vmStateValue) {
        delta.vmStateChanged = true;
        delta.newVMStateValue = current.vmStateValue;
    }
    
    if (current.totalCyclesExecuted != parent.totalCyclesExecuted) {
        delta.cyclesChanged = true;
        delta.newTotalCyclesExecuted = current.totalCyclesExecuted;
    }
    
    if (current.quantumSize != parent.quantumSize) {
        delta.quantumChanged = true;
        delta.newQuantumSize = current.quantumSize;
    }
    
    return delta;
}

VMStateSnapshot VMSnapshotManager::applyDelta(
    const VMStateSnapshot& base,
    const VMStateSnapshotDelta& delta) {

    VMStateSnapshot result = base;
    
    // Apply memory delta
    result.memoryState = applyMemoryDelta(base.memoryState, delta.memoryDelta);
    
    // Apply CPU deltas
    for (const auto& cpuDelta : delta.cpuDeltas) {
        if (cpuDelta.cpuId < result.cpus.size()) {
            result.cpus[cpuDelta.cpuId] = applyCPUStateDelta(
                result.cpus[cpuDelta.cpuId],
                cpuDelta
            );
        }
    }
    
    // Apply other state changes
    if (delta.activeCPUChanged) {
        result.activeCPUIndex = delta.newActiveCPUIndex;
    }
    
    if (delta.consoleStateChanged) {
        result.consoleState = delta.newConsoleState;
    }
    
    if (delta.timerStateChanged) {
        result.timerState = delta.newTimerState;
    }
    
    if (delta.vmStateChanged) {
        result.vmStateValue = delta.newVMStateValue;
    }
    
    if (delta.cyclesChanged) {
        result.totalCyclesExecuted = delta.newTotalCyclesExecuted;
    }
    
    if (delta.quantumChanged) {
        result.quantumSize = delta.newQuantumSize;
    }
    
    // Update metadata
    result.metadata = delta.metadata;
    result.metadata.isDelta = false;  // Result is now a full snapshot
    
    return result;
}

// ============================================================================
// Internal Helpers
// ============================================================================

std::string VMSnapshotManager::generateSnapshotId() {
    // Simple UUID-like generation (not cryptographically secure)
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    
    auto now = std::chrono::system_clock::now().time_since_epoch().count();
    ss << std::setw(8) << (now & 0xFFFFFFFF);
    ss << "-";
    ss << std::setw(4) << ((now >> 32) & 0xFFFF);
    ss << "-";
    ss << std::setw(4) << nextSnapshotNumber_;
    ss << "-";
    ss << std::setw(4) << (now & 0xFFFF);
    ss << "-";
    ss << std::setw(12) << ((now >> 16) & 0xFFFFFFFFFFFF);
    
    return ss.str();
}

VMStateSnapshot VMSnapshotManager::captureFullSnapshot(const VirtualMachine& vm) const {
    VMStateSnapshot snapshot;
    
    // Capture CPU states
    for (size_t i = 0; i < vm.getCPUCount(); ++i) {
        CPUSnapshotRecord cpuRecord;
        cpuRecord.cpuId = static_cast<uint32_t>(i);
        
        const CPUContext* ctx = vm.getCPUContext(static_cast<int>(i));
        if (ctx && ctx->cpu) {
            cpuRecord.architecturalState = ctx->cpu->getState();
            cpuRecord.executionState = ctx->state;
            cpuRecord.cyclesExecuted = ctx->cyclesExecuted;
            cpuRecord.instructionsExecuted = ctx->instructionsExecuted;
            cpuRecord.idleCycles = ctx->idleCycles;
            cpuRecord.enabled = ctx->enabled;
            cpuRecord.lastActivationTime = ctx->lastActivationTime;
            
            // Capture runtime state
            auto runtimeSnapshot = ctx->cpu->createSnapshot();
            cpuRecord.currentBundle = std::vector<uint8_t>(16);  // Bundle size
            cpuRecord.currentSlot = runtimeSnapshot.currentSlot;
            cpuRecord.bundleValid = runtimeSnapshot.bundleValid;
            cpuRecord.pendingInterrupts = runtimeSnapshot.pendingInterrupts;
            cpuRecord.interruptVectorBase = runtimeSnapshot.interruptVectorBase;
        }
        
        snapshot.cpus.push_back(cpuRecord);
    }
    
    snapshot.activeCPUIndex = vm.getActiveCPUIndex();
    
    // Capture memory state
    // Note: getMemory() returns IMemory&, but we need Memory* to call CreateSnapshot
    // We can safely cast since VirtualMachine uses Memory internally
    const Memory* memory = dynamic_cast<const Memory*>(&vm.getMemory());
    if (memory) {
        snapshot.memoryState = memory->CreateSnapshot();
    }
    
    // Capture device states
    if (vm.getConsoleBaseAddress() != 0) {
        // Console state would be captured here if we had access
        // For now, create empty state
        snapshot.consoleState.baseAddress = vm.getConsoleBaseAddress();
    }
    
    if (vm.getTimerBaseAddress() != 0) {
        // Timer state would be captured here if we had access
        snapshot.timerState.baseAddress = vm.getTimerBaseAddress();
    }
    
    // Capture VM state
    snapshot.vmStateValue = static_cast<uint32_t>(vm.getState());
    snapshot.totalCyclesExecuted = 0;  // Would need to be exposed by VM
    snapshot.quantumSize = vm.getQuantumSize();
    
    return snapshot;
}

MemoryDelta VMSnapshotManager::computeMemoryDelta(
    const MemorySnapshot& current,
    const MemorySnapshot& parent) {

    MemoryDelta delta;
    
    // Find changed memory regions
    size_t minSize = std::min(current.data.size(), parent.data.size());
    
    size_t changeStart = 0;
    bool inChange = false;
    
    for (size_t i = 0; i < minSize; ++i) {
        if (current.data[i] != parent.data[i]) {
            if (!inChange) {
                changeStart = i;
                inChange = true;
            }
        } else {
            if (inChange) {
                // End of change region
                size_t changeSize = i - changeStart;
                std::vector<uint8_t> changedData(
                    current.data.begin() + changeStart,
                    current.data.begin() + i
                );
                delta.changes.emplace_back(changeStart, changedData);
                delta.totalChangedBytes += changeSize;
                inChange = false;
            }
        }
    }
    
    // Handle final change region
    if (inChange) {
        size_t changeSize = minSize - changeStart;
        std::vector<uint8_t> changedData(
            current.data.begin() + changeStart,
            current.data.begin() + minSize
        );
        delta.changes.emplace_back(changeStart, changedData);
        delta.totalChangedBytes += changeSize;
    }
    
    // Handle size difference
    if (current.data.size() > parent.data.size()) {
        std::vector<uint8_t> additionalData(
            current.data.begin() + parent.data.size(),
            current.data.end()
        );
        delta.changes.emplace_back(parent.data.size(), additionalData);
        delta.totalChangedBytes += additionalData.size();
    }
    
    return delta;
}

CPUStateDelta VMSnapshotManager::computeCPUStateDelta(
    const CPUSnapshotRecord& current,
    const CPUSnapshotRecord& parent) {

    CPUStateDelta delta;
    delta.cpuId = current.cpuId;
    
    // Compare general registers
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; ++i) {
        uint64_t currVal = current.architecturalState.GetGR(i);
        uint64_t parentVal = parent.architecturalState.GetGR(i);
        if (currVal != parentVal) {
            delta.changedGR[i] = currVal;
        }
    }
    
    // Compare predicate registers
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        bool currVal = current.architecturalState.GetPR(i);
        bool parentVal = parent.architecturalState.GetPR(i);
        if (currVal != parentVal) {
            delta.changedPR[i] = currVal;
        }
    }
    
    // Compare branch registers
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) {
        uint64_t currVal = current.architecturalState.GetBR(i);
        uint64_t parentVal = parent.architecturalState.GetBR(i);
        if (currVal != parentVal) {
            delta.changedBR[i] = currVal;
        }
    }
    
    // Compare special registers
    if (current.architecturalState.GetIP() != parent.architecturalState.GetIP()) {
        delta.ipChanged = true;
        delta.newIP = current.architecturalState.GetIP();
    }
    
    if (current.architecturalState.GetCFM() != parent.architecturalState.GetCFM()) {
        delta.cfmChanged = true;
        delta.newCFM = current.architecturalState.GetCFM();
    }
    
    if (current.architecturalState.GetPSR() != parent.architecturalState.GetPSR()) {
        delta.psrChanged = true;
        delta.newPSR = current.architecturalState.GetPSR();
    }
    
    // Compare execution state
    if (current.executionState != parent.executionState) {
        delta.executionStateChanged = true;
        delta.newExecutionState = current.executionState;
    }
    
    // Compare counters
    if (current.cyclesExecuted != parent.cyclesExecuted ||
        current.instructionsExecuted != parent.instructionsExecuted ||
        current.idleCycles != parent.idleCycles) {
        delta.countersChanged = true;
        delta.newCyclesExecuted = current.cyclesExecuted;
        delta.newInstructionsExecuted = current.instructionsExecuted;
        delta.newIdleCycles = current.idleCycles;
    }
    
    return delta;
}

MemorySnapshot VMSnapshotManager::applyMemoryDelta(
    const MemorySnapshot& base,
    const MemoryDelta& delta) {

    MemorySnapshot result = base;
    
    // Apply all memory changes
    for (const auto& change : delta.changes) {
        uint64_t addr = change.address;
        
        // Expand memory if needed
        if (addr + change.data.size() > result.data.size()) {
            result.data.resize(addr + change.data.size());
        }
        
        // Apply change
        std::copy(
            change.data.begin(),
            change.data.end(),
            result.data.begin() + addr
        );
    }
    
    return result;
}

CPUSnapshotRecord VMSnapshotManager::applyCPUStateDelta(
    const CPUSnapshotRecord& base,
    const CPUStateDelta& delta) {

    CPUSnapshotRecord result = base;
    
    // Apply general register changes
    for (const auto& pair : delta.changedGR) {
        result.architecturalState.SetGR(pair.first, pair.second);
    }
    
    // Apply predicate register changes
    for (const auto& pair : delta.changedPR) {
        result.architecturalState.SetPR(pair.first, pair.second);
    }
    
    // Apply branch register changes
    for (const auto& pair : delta.changedBR) {
        result.architecturalState.SetBR(pair.first, pair.second);
    }
    
    // Apply special register changes
    if (delta.ipChanged) {
        result.architecturalState.SetIP(delta.newIP);
    }
    
    if (delta.cfmChanged) {
        result.architecturalState.SetCFM(delta.newCFM);
    }
    
    if (delta.psrChanged) {
        result.architecturalState.SetPSR(delta.newPSR);
    }
    
    // Apply execution state changes
    if (delta.executionStateChanged) {
        result.executionState = delta.newExecutionState;
    }
    
    // Apply counter changes
    if (delta.countersChanged) {
        result.cyclesExecuted = delta.newCyclesExecuted;
        result.instructionsExecuted = delta.newInstructionsExecuted;
        result.idleCycles = delta.newIdleCycles;
    }
    
    return result;
}

VMStateSnapshot VMSnapshotManager::resolveSnapshot(const std::string& snapshotId) const {
    // Check if it's a full snapshot
    auto fullIt = fullSnapshots_.find(snapshotId);
    if (fullIt != fullSnapshots_.end()) {
        return fullIt->second;
    }
    
    // Check if it's a delta snapshot
    auto deltaIt = deltaSnapshots_.find(snapshotId);
    if (deltaIt != deltaSnapshots_.end()) {
        const VMStateSnapshotDelta& delta = deltaIt->second;
        
        // Recursively resolve parent
        VMStateSnapshot parent = resolveSnapshot(delta.metadata.parentSnapshotId);
        
        if (parent.metadata.snapshotId.empty()) {
            // Parent not found - return empty snapshot
            return VMStateSnapshot();
        }
        
        // Apply delta to parent
        return applyDelta(parent, delta);
    }
    
    // Not found - return empty snapshot
    return VMStateSnapshot();
}

} // namespace ia64

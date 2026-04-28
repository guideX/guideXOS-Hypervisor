#include "CPUContext.h"
#include "cpu.h"

namespace ia64 {

// ============================================================================
// CPUContext Implementation
// ============================================================================

CPUContext::CPUContext()
    : isaPlugin(nullptr),
      cpu(nullptr),
      cpuId(0),
      state(CPUExecutionState::IDLE),
      cyclesExecuted(0),
      instructionsExecuted(0),
      idleCycles(0),
      enabled(false),
      lastActivationTime(0) {
}

CPUContext::CPUContext(uint32_t id)
    : isaPlugin(nullptr),
      cpu(nullptr),
      cpuId(id),
      state(CPUExecutionState::IDLE),
      cyclesExecuted(0),
      instructionsExecuted(0),
      idleCycles(0),
      enabled(false),
      lastActivationTime(0) {
}

CPUContext::~CPUContext() {
    // Destructor defined here where CPU type is complete
    // unique_ptr will properly delete CPU
}

CPUContext::CPUContext(CPUContext&& other) noexcept
    : isaPlugin(std::move(other.isaPlugin)),
      cpu(std::move(other.cpu)),
      cpuId(other.cpuId),
      state(other.state),
      cyclesExecuted(other.cyclesExecuted),
      instructionsExecuted(other.instructionsExecuted),
      idleCycles(other.idleCycles),
      enabled(other.enabled),
      lastActivationTime(other.lastActivationTime) {
}

CPUContext& CPUContext::operator=(CPUContext&& other) noexcept {
    if (this != &other) {
        isaPlugin = std::move(other.isaPlugin);
        cpu = std::move(other.cpu);
        cpuId = other.cpuId;
        state = other.state;
        cyclesExecuted = other.cyclesExecuted;
        instructionsExecuted = other.instructionsExecuted;
        idleCycles = other.idleCycles;
        enabled = other.enabled;
        lastActivationTime = other.lastActivationTime;
    }
    return *this;
}

} // namespace ia64

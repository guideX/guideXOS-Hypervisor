#pragma once

#include <cstdint>
#include <string>

namespace ia64 {

class VirtualMachine;

/**
 * On-disk full-state checkpoint codec used by the IA-64 boot investigation.
 * The format is intentionally versioned and machine-specific; it is not the
 * legacy debugger/VMSnapshotManager format.
 */
class VMCheckpointCodec {
public:
    static constexpr uint32_t kFormatVersion = 1;

    static bool write(const VirtualMachine& vm,
                      const std::string& path,
                      const std::string& guestIdentity,
                      std::string* error);
    static bool read(VirtualMachine& vm,
                     const std::string& path,
                     const std::string& expectedGuestIdentity,
                     std::string* error);
};

} // namespace ia64

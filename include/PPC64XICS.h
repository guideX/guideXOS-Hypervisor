#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ia64::ppc64 {

inline constexpr uint32_t PPC64_XICS_SNAPSHOT_VERSION = 1;
inline constexpr uint32_t PPC64_XICS_FIRST_SOURCE = 0x1100;
inline constexpr uint32_t PPC64_XICS_LAST_SOURCE = 0x113f;
inline constexpr size_t PPC64_XICS_SOURCE_COUNT = 64;
inline constexpr uint32_t PPC64_XICS_IPI_XISR = 2;
inline constexpr uint32_t PPC64_XICS_XISR_MASK = 0x00ffffffU;
inline constexpr uint32_t PPC64_XICS_XIRR_CPPR_MASK = 0xff000000U;
inline constexpr uint32_t PPC64_XICS_SERVER_ID = 0;

enum class PPC64InterruptMode : uint32_t {
    LEGACY_EXTERNAL = 0,
    XICS_COMPAT = 1,
};

enum class PPC64XICSSourceType : uint8_t {
    LEVEL = 1,
    EDGE = 2,
};

struct PPC64XICSSourceSnapshot {
    uint32_t sourceId = 0;
    PPC64XICSSourceType type = PPC64XICSSourceType::LEVEL;
    bool allocated = false;
    bool enabled = false;
    uint32_t serverId = PPC64_XICS_SERVER_ID;
    uint8_t priority = 0xff;
    bool asserted = false;
    bool pending = false;
    bool presented = false;
    bool acknowledged = false;
    uint16_t ownerLPID = 0;
};

struct PPC64XICSOutstandingSnapshot {
    uint32_t xirr = 0;
    uint32_t sourceId = 0;
    uint16_t ownerLPID = 0;
    uint8_t acceptedPriority = 0xff;
};

struct PPC64XICSPresentationSnapshot {
    uint16_t ownerLPID = 0;
    uint8_t cppr = 0;
    uint8_t mfrr = 0xff;
    uint32_t presentedSourceId = 0;
    uint8_t pendingPriority = 0xff;
    std::vector<PPC64XICSOutstandingSnapshot> outstanding;
};

struct PPC64XICSSnapshot {
    uint32_t version = PPC64_XICS_SNAPSHOT_VERSION;
    bool enabled = false;
    uint16_t activeLPID = 0;
    std::vector<PPC64XICSSourceSnapshot> sources;
    std::vector<PPC64XICSPresentationSnapshot> presentations;
};

// Bounded pSeries XICS compatibility model. The source namespace begins at
// QEMU's pSeries XICS IRQ base (BUID 1, 0x1000); this profile allocates only
// 64 host-owned source IDs in the 0x1100..0x113f portion. Each LPID retains
// an isolated one-server presentation, while only the active LPID can assert
// the single vCPU external-interrupt output.
class PPC64XICS {
public:
    explicit PPC64XICS(uint16_t activeLPID = 0);

    void reset();
    bool bindActiveLPID(uint16_t lpid, std::string& reason);
    bool configureSource(uint32_t sourceId, PPC64XICSSourceType type,
                         uint8_t priority, uint32_t serverId,
                         uint16_t ownerLPID, bool enabled,
                         std::string& reason);
    bool setSourceEnabled(uint32_t sourceId, uint16_t ownerLPID,
                          bool enabled, std::string& reason);
    bool assertSource(uint32_t sourceId, uint16_t ownerLPID,
                      std::string& reason);
    bool deassertSource(uint32_t sourceId, uint16_t ownerLPID,
                        std::string& reason);
    bool removeSource(uint32_t sourceId, uint16_t ownerLPID,
                      std::string& reason);
    bool getSource(uint32_t sourceId, PPC64XICSSourceSnapshot& result) const;

    bool setCPPR(uint8_t cppr, uint16_t callerLPID, std::string& reason);
    bool acknowledge(uint16_t callerLPID, uint32_t& xirr,
                     std::string& reason);
    bool endOfInterrupt(uint32_t xirr, uint16_t callerLPID,
                        std::string& reason);
    bool poll(uint32_t serverId, uint16_t callerLPID, uint32_t& xirr,
              uint32_t& mfrr, std::string& reason) const;

    bool externalOutputAsserted() const;
    uint16_t activeLPID() const { return activeLPID_; }
    PPC64XICSSnapshot createSnapshot() const;
    bool restoreSnapshot(const PPC64XICSSnapshot& snapshot,
                         const std::vector<uint16_t>& allowedLPIDs,
                         uint16_t expectedActiveLPID,
                         std::string& reason);

    static bool validSourceId(uint32_t sourceId);
    static bool validateSnapshot(const PPC64XICSSnapshot& snapshot,
                                 const std::vector<uint16_t>& allowedLPIDs,
                                 uint16_t expectedActiveLPID,
                                 std::string& reason);

private:
    struct Source {
        PPC64XICSSourceSnapshot state;
    };
    struct Presentation {
        uint8_t cppr = 0;
        uint8_t mfrr = 0xff;
        uint32_t presentedSourceId = 0;
        uint8_t pendingPriority = 0xff;
        std::vector<PPC64XICSOutstandingSnapshot> outstanding;
    };

    static size_t sourceIndex(uint32_t sourceId);
    Presentation& presentation(uint16_t lpid);
    const Presentation* findPresentation(uint16_t lpid) const;
    void reevaluate(uint16_t lpid);
    uint32_t currentXirr(const Presentation& state) const;

    std::array<Source, PPC64_XICS_SOURCE_COUNT> sources_{};
    std::map<uint16_t, Presentation> presentations_;
    uint16_t activeLPID_ = 0;
};

} // namespace ia64::ppc64

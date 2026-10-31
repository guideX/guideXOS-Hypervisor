#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include "PPC64XICS.h"

namespace ia64::ppc64 {

inline constexpr uint32_t PPC64_DEFAULT_VIRTUAL_TERMINAL_ID = 0x30000000U;
inline constexpr size_t PPC64_TERMINAL_QUEUE_CAPACITY = 4096U;
inline constexpr size_t PPC64_TERMINAL_MAX_CHANNELS = 64U;
inline constexpr uint32_t PPC64_TERMINAL_STATE_VERSION = 2U;
inline constexpr uint8_t PPC64_TERMINAL_XICS_PRIORITY = 0x20U;

struct PPC64TerminalInterruptAttachmentSnapshot {
    bool attached = true;
    uint16_t ownerLPID = 0;
    uint32_t terminalId = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID;
    uint32_t sourceId = 0;
    uint32_t serverId = PPC64_XICS_SERVER_ID;
    PPC64XICSSourceType sourceType = PPC64XICSSourceType::EDGE;
    uint8_t priority = PPC64_TERMINAL_XICS_PRIORITY;
};

struct PPC64VirtualTerminalChannelSnapshot {
    uint16_t lpid = 0;
    std::vector<uint8_t> input;
    std::vector<uint8_t> output;
};

struct PPC64VirtualTerminalSnapshot {
    uint32_t version = PPC64_TERMINAL_STATE_VERSION;
    uint32_t terminalId = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID;
    bool enabled = true;
    std::vector<PPC64VirtualTerminalChannelSnapshot> channels;
    std::vector<PPC64TerminalInterruptAttachmentSnapshot> attachments;
};

enum class PPC64TerminalResult : uint8_t {
    SUCCESS,
    BUSY,
    PARAMETER,
    RESOURCE,
};

// Bounded, binary-safe PAPR hvterm1 byte transport. Each LPID has an isolated
// channel within this VM's terminal namespace. Guest calls always select the
// active LPID in the PPC64 plugin; host APIs must name the intended LPID.
class PPC64VirtualTerminal {
public:
    explicit PPC64VirtualTerminal(
        uint32_t terminalId = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID,
        bool enabled = true);

    uint32_t terminalId() const;
    bool isEnabled() const;
    bool reconfigure(uint32_t terminalId, bool enabled);
    void setEnabled(bool enabled);
    void reset();

    PPC64TerminalResult put(uint32_t terminalId, uint16_t lpid,
                            const uint8_t* bytes, size_t count);
    PPC64TerminalResult get(uint32_t terminalId, uint16_t lpid,
                            std::array<uint8_t, 16>& bytes,
                            size_t& count);

    // Host injection is all-or-nothing. False means invalid identity/LPID,
    // exhausted capacity, or too many active LPID channels.
    bool injectInput(uint16_t lpid, const uint8_t* bytes, size_t count);
    PPC64TerminalResult injectInputWithStatus(uint16_t lpid,
                                              const uint8_t* bytes,
                                              size_t count,
                                              const std::function<bool()>&
                                                  onEmptyToNonempty = {});
    bool attachInterrupt(
        const PPC64TerminalInterruptAttachmentSnapshot& attachment);
    bool detachInterrupt(uint16_t ownerLPID,
                         PPC64TerminalInterruptAttachmentSnapshot* removed = nullptr);
    bool getInterruptAttachment(
        uint16_t ownerLPID,
        PPC64TerminalInterruptAttachmentSnapshot& attachment) const;
    std::vector<PPC64TerminalInterruptAttachmentSnapshot>
    getInterruptAttachments() const;
    void clearInterruptAttachmentsExcept(uint16_t ownerLPID);
    void discardChannel(uint16_t lpid);
    std::vector<uint8_t> readOutput(uint16_t lpid,
                                    size_t maximumCount = 0U) const;
    std::vector<uint8_t> drainOutput(uint16_t lpid,
                                     size_t maximumCount = 0U);
    size_t pendingInputBytes(uint16_t lpid) const;
    size_t pendingOutputBytes(uint16_t lpid) const;

    PPC64VirtualTerminalSnapshot createSnapshot() const;
    bool restoreSnapshot(const PPC64VirtualTerminalSnapshot& snapshot,
                         const std::vector<uint16_t>& authorizedLPIDs);

    static bool validTerminalId(uint32_t terminalId);

private:
    struct Channel {
        std::deque<uint8_t> input;
        std::deque<uint8_t> output;
    };

    Channel* channelForWrite(uint16_t lpid);

    mutable std::mutex mutex_;
    uint32_t terminalId_;
    bool enabled_;
    std::map<uint16_t, Channel> channels_;
    std::map<uint16_t, PPC64TerminalInterruptAttachmentSnapshot> attachments_;
};

} // namespace ia64::ppc64

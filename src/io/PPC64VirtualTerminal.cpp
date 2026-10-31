#include "PPC64VirtualTerminal.h"

#include <algorithm>
#include <set>

namespace ia64::ppc64 {

PPC64VirtualTerminal::PPC64VirtualTerminal(uint32_t terminalId, bool enabled)
    : terminalId_(terminalId), enabled_(enabled) {
    if (!validTerminalId(terminalId)) {
        terminalId_ = PPC64_DEFAULT_VIRTUAL_TERMINAL_ID;
        enabled_ = false;
    }
}

bool PPC64VirtualTerminal::validTerminalId(uint32_t terminalId) {
    // pSeries VIO assigns vty unit addresses in the 0x30000000 range, in
    // 0x1000 increments (QEMU's spapr_vio register convention).
    return terminalId >= PPC64_DEFAULT_VIRTUAL_TERMINAL_ID &&
           terminalId <= 0x3000f000U &&
           (terminalId & 0xfffU) == 0;
}

uint32_t PPC64VirtualTerminal::terminalId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return terminalId_;
}

bool PPC64VirtualTerminal::isEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

bool PPC64VirtualTerminal::reconfigure(uint32_t terminalId, bool enabled) {
    if (!validTerminalId(terminalId)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!channels_.empty() &&
        (terminalId != terminalId_ || enabled != enabled_)) return false;
    terminalId_ = terminalId;
    enabled_ = enabled;
    attachments_.clear();
    return true;
}

void PPC64VirtualTerminal::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
}

void PPC64VirtualTerminal::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.clear();
}

PPC64VirtualTerminal::Channel* PPC64VirtualTerminal::channelForWrite(
    uint16_t lpid) {
    auto found = channels_.find(lpid);
    if (found != channels_.end()) return &found->second;
    if (channels_.size() >= PPC64_TERMINAL_MAX_CHANNELS) return nullptr;
    return &channels_.emplace(lpid, Channel{}).first->second;
}

PPC64TerminalResult PPC64VirtualTerminal::put(
    uint32_t terminalId, uint16_t lpid, const uint8_t* bytes, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || terminalId != terminalId_ || count > 16U ||
        (count != 0 && bytes == nullptr)) {
        return PPC64TerminalResult::PARAMETER;
    }
    if (count == 0) return PPC64TerminalResult::SUCCESS;
    Channel* channel = channelForWrite(lpid);
    if (channel == nullptr) return PPC64TerminalResult::RESOURCE;
    if (count > PPC64_TERMINAL_QUEUE_CAPACITY - channel->output.size()) {
        return PPC64TerminalResult::BUSY;
    }
    for (size_t i = 0; i < count; ++i) channel->output.push_back(bytes[i]);
    return PPC64TerminalResult::SUCCESS;
}

PPC64TerminalResult PPC64VirtualTerminal::get(
    uint32_t terminalId, uint16_t lpid, std::array<uint8_t, 16>& bytes,
    size_t& count) {
    std::lock_guard<std::mutex> lock(mutex_);
    count = 0;
    bytes.fill(0);
    if (!enabled_ || terminalId != terminalId_) {
        return PPC64TerminalResult::PARAMETER;
    }
    auto found = channels_.find(lpid);
    if (found == channels_.end()) return PPC64TerminalResult::SUCCESS;
    Channel& channel = found->second;
    count = std::min(bytes.size(), channel.input.size());
    for (size_t i = 0; i < count; ++i) {
        bytes[i] = channel.input.front();
        channel.input.pop_front();
    }
    return PPC64TerminalResult::SUCCESS;
}

bool PPC64VirtualTerminal::injectInput(uint16_t lpid, const uint8_t* bytes,
                                      size_t count) {
    return injectInputWithStatus(lpid, bytes, count) ==
           PPC64TerminalResult::SUCCESS;
}

PPC64TerminalResult PPC64VirtualTerminal::injectInputWithStatus(
    uint16_t lpid, const uint8_t* bytes, size_t count,
    const std::function<bool()>& onEmptyToNonempty) {
    if (count != 0 && bytes == nullptr) return PPC64TerminalResult::PARAMETER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return PPC64TerminalResult::PARAMETER;
    if (count == 0) return PPC64TerminalResult::SUCCESS;
    if (channels_.find(lpid) == channels_.end() &&
        channels_.size() >= PPC64_TERMINAL_MAX_CHANNELS) {
        return PPC64TerminalResult::RESOURCE;
    }
    Channel* channel = channelForWrite(lpid);
    if (channel == nullptr) return PPC64TerminalResult::RESOURCE;
    if (count > PPC64_TERMINAL_QUEUE_CAPACITY - channel->input.size())
        return PPC64TerminalResult::BUSY;
    const bool wasEmpty = channel->input.empty();
    for (size_t i = 0; i < count; ++i) channel->input.push_back(bytes[i]);
    if (wasEmpty && onEmptyToNonempty) {
        bool notified = false;
        try {
            notified = onEmptyToNonempty();
        } catch (...) {
            notified = false;
        }
        if (!notified) {
            for (size_t i = 0; i < count; ++i) channel->input.pop_back();
            return PPC64TerminalResult::RESOURCE;
        }
    }
    return PPC64TerminalResult::SUCCESS;
}

bool PPC64VirtualTerminal::attachInterrupt(
    const PPC64TerminalInterruptAttachmentSnapshot& attachment) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !attachment.attached ||
        attachment.terminalId != terminalId_ ||
        !PPC64XICS::validSourceId(attachment.sourceId) ||
        attachment.serverId != PPC64_XICS_SERVER_ID ||
        attachment.sourceType != PPC64XICSSourceType::EDGE ||
        attachment.priority == 0xffU ||
        attachments_.find(attachment.ownerLPID) != attachments_.end()) {
        return false;
    }
    for (const auto& [owner, existing] : attachments_) {
        (void)owner;
        if (existing.sourceId == attachment.sourceId) return false;
    }
    attachments_.emplace(attachment.ownerLPID, attachment);
    return true;
}

bool PPC64VirtualTerminal::detachInterrupt(
    uint16_t ownerLPID,
    PPC64TerminalInterruptAttachmentSnapshot* removed) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = attachments_.find(ownerLPID);
    if (found == attachments_.end()) return false;
    if (removed != nullptr) *removed = found->second;
    attachments_.erase(found);
    return true;
}

bool PPC64VirtualTerminal::getInterruptAttachment(
    uint16_t ownerLPID,
    PPC64TerminalInterruptAttachmentSnapshot& attachment) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = attachments_.find(ownerLPID);
    if (found == attachments_.end()) return false;
    attachment = found->second;
    return true;
}

std::vector<PPC64TerminalInterruptAttachmentSnapshot>
PPC64VirtualTerminal::getInterruptAttachments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PPC64TerminalInterruptAttachmentSnapshot> result;
    result.reserve(attachments_.size());
    for (const auto& [owner, attachment] : attachments_) {
        (void)owner;
        result.push_back(attachment);
    }
    return result;
}

void PPC64VirtualTerminal::clearInterruptAttachmentsExcept(
    uint16_t ownerLPID) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto current = attachments_.begin(); current != attachments_.end();) {
        if (current->first != ownerLPID) current = attachments_.erase(current);
        else ++current;
    }
}

void PPC64VirtualTerminal::discardChannel(uint16_t lpid) {
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.erase(lpid);
}

std::vector<uint8_t> PPC64VirtualTerminal::readOutput(
    uint16_t lpid, size_t maximumCount) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> result;
    auto found = channels_.find(lpid);
    if (found == channels_.end()) return result;
    const auto& output = found->second.output;
    const size_t count = maximumCount == 0U
        ? output.size()
        : std::min(maximumCount, output.size());
    result.reserve(count);
    auto current = output.begin();
    for (size_t i = 0; i < count; ++i, ++current) result.push_back(*current);
    return result;
}

std::vector<uint8_t> PPC64VirtualTerminal::drainOutput(
    uint16_t lpid, size_t maximumCount) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> result;
    auto found = channels_.find(lpid);
    if (found == channels_.end()) return result;
    Channel& channel = found->second;
    const size_t count = maximumCount == 0
        ? channel.output.size()
        : std::min(maximumCount, channel.output.size());
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(channel.output.front());
        channel.output.pop_front();
    }
    return result;
}

size_t PPC64VirtualTerminal::pendingInputBytes(uint16_t lpid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = channels_.find(lpid);
    return found == channels_.end() ? 0U : found->second.input.size();
}

size_t PPC64VirtualTerminal::pendingOutputBytes(uint16_t lpid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = channels_.find(lpid);
    return found == channels_.end() ? 0U : found->second.output.size();
}

PPC64VirtualTerminalSnapshot PPC64VirtualTerminal::createSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PPC64VirtualTerminalSnapshot snapshot;
    snapshot.terminalId = terminalId_;
    snapshot.enabled = enabled_;
    snapshot.channels.reserve(channels_.size());
    for (const auto& [lpid, channel] : channels_) {
        PPC64VirtualTerminalChannelSnapshot record;
        record.lpid = lpid;
        record.input.assign(channel.input.begin(), channel.input.end());
        record.output.assign(channel.output.begin(), channel.output.end());
        snapshot.channels.push_back(std::move(record));
    }
    snapshot.attachments.reserve(attachments_.size());
    for (const auto& [lpid, attachment] : attachments_) {
        (void)lpid;
        snapshot.attachments.push_back(attachment);
    }
    return snapshot;
}

bool PPC64VirtualTerminal::restoreSnapshot(
    const PPC64VirtualTerminalSnapshot& snapshot,
    const std::vector<uint16_t>& authorizedLPIDs) {
    if ((snapshot.version != 1U &&
         snapshot.version != PPC64_TERMINAL_STATE_VERSION) ||
        !validTerminalId(snapshot.terminalId) ||
        snapshot.channels.size() > PPC64_TERMINAL_MAX_CHANNELS ||
        snapshot.attachments.size() > PPC64_TERMINAL_MAX_CHANNELS ||
        (snapshot.version == 1U && !snapshot.attachments.empty())) return false;
    std::set<uint16_t> allowed(authorizedLPIDs.begin(), authorizedLPIDs.end());
    allowed.insert(0);
    std::map<uint16_t, Channel> staged;
    for (const auto& record : snapshot.channels) {
        if (allowed.find(record.lpid) == allowed.end() ||
            record.input.size() > PPC64_TERMINAL_QUEUE_CAPACITY ||
            record.output.size() > PPC64_TERMINAL_QUEUE_CAPACITY) return false;
        Channel channel;
        channel.input.assign(record.input.begin(), record.input.end());
        channel.output.assign(record.output.begin(), record.output.end());
        if (!staged.emplace(record.lpid, std::move(channel)).second) return false;
    }
    std::map<uint16_t, PPC64TerminalInterruptAttachmentSnapshot> stagedAttachments;
    std::set<uint32_t> attachedSources;
    for (const auto& attachment : snapshot.attachments) {
        if (!attachment.attached ||
            (allowed.find(attachment.ownerLPID) == allowed.end() &&
             attachment.ownerLPID != 0) ||
            attachment.terminalId != snapshot.terminalId ||
            !PPC64XICS::validSourceId(attachment.sourceId) ||
            attachment.serverId != PPC64_XICS_SERVER_ID ||
            attachment.sourceType != PPC64XICSSourceType::EDGE ||
            attachment.priority == 0xffU ||
            !attachedSources.insert(attachment.sourceId).second ||
            !stagedAttachments.emplace(attachment.ownerLPID, attachment).second) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.terminalId != terminalId_) return false;
    terminalId_ = snapshot.terminalId;
    enabled_ = snapshot.enabled;
    channels_.swap(staged);
    attachments_.swap(stagedAttachments);
    return true;
}

} // namespace ia64::ppc64

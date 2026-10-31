#include "PPC64XICS.h"

#include <algorithm>
#include <set>

namespace ia64::ppc64 {
namespace {

void setFailure(std::string& reason, const char* message) {
    reason = message;
}

bool allowedLPID(uint16_t lpid, const std::vector<uint16_t>& allowed) {
    return lpid == 0 || std::find(allowed.begin(), allowed.end(), lpid) != allowed.end();
}

} // namespace

PPC64XICS::PPC64XICS(uint16_t activeLPID) : activeLPID_(activeLPID) {
    presentations_.try_emplace(activeLPID_);
}

bool PPC64XICS::validSourceId(uint32_t sourceId) {
    return sourceId >= PPC64_XICS_FIRST_SOURCE &&
           sourceId <= PPC64_XICS_LAST_SOURCE;
}

size_t PPC64XICS::sourceIndex(uint32_t sourceId) {
    return static_cast<size_t>(sourceId - PPC64_XICS_FIRST_SOURCE);
}

PPC64XICS::Presentation& PPC64XICS::presentation(uint16_t lpid) {
    return presentations_.try_emplace(lpid).first->second;
}

const PPC64XICS::Presentation* PPC64XICS::findPresentation(uint16_t lpid) const {
    const auto found = presentations_.find(lpid);
    return found == presentations_.end() ? nullptr : &found->second;
}

void PPC64XICS::reset() {
    for (size_t i = 0; i < sources_.size(); ++i) {
        sources_[i].state = PPC64XICSSourceSnapshot{};
        sources_[i].state.sourceId = PPC64_XICS_FIRST_SOURCE +
                                     static_cast<uint32_t>(i);
    }
    presentations_.clear();
    activeLPID_ = 0;
    presentations_.try_emplace(0);
}

bool PPC64XICS::bindActiveLPID(uint16_t lpid, std::string& reason) {
    if (presentations_.find(lpid) == presentations_.end() &&
        presentations_.size() >= PPC64_XICS_SOURCE_COUNT + 1U) {
        setFailure(reason, "XICS presentation owner limit was reached");
        return false;
    }
    activeLPID_ = lpid;
    presentation(lpid);
    reevaluate(lpid);
    reason.clear();
    return true;
}

bool PPC64XICS::configureSource(uint32_t sourceId, PPC64XICSSourceType type,
                                uint8_t priority, uint32_t serverId,
                                uint16_t ownerLPID, bool enabled,
                                std::string& reason) {
    if (!validSourceId(sourceId)) {
        setFailure(reason, "XICS source ID is outside the allocated 0x1100..0x113f range");
        return false;
    }
    if (type != PPC64XICSSourceType::LEVEL && type != PPC64XICSSourceType::EDGE) {
        setFailure(reason, "unsupported XICS source type");
        return false;
    }
    if (serverId != PPC64_XICS_SERVER_ID) {
        setFailure(reason, "XICS source destination is not the single server 0");
        return false;
    }
    if (enabled && priority == 0xff) {
        setFailure(reason, "priority 0xff is reserved as the disabled priority");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (source.state.allocated) {
        setFailure(reason, "XICS source ID is already allocated");
        return false;
    }
    source.state.sourceId = sourceId;
    source.state.type = type;
    source.state.allocated = true;
    source.state.enabled = enabled;
    source.state.serverId = serverId;
    source.state.priority = priority;
    source.state.ownerLPID = ownerLPID;
    presentation(ownerLPID);
    reevaluate(activeLPID_);
    reason.clear();
    return true;
}

bool PPC64XICS::setSourceEnabled(uint32_t sourceId, uint16_t ownerLPID,
                                 bool enabled, std::string& reason) {
    if (!validSourceId(sourceId)) {
        setFailure(reason, "invalid XICS source ID");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (!source.state.allocated || source.state.ownerLPID != ownerLPID) {
        setFailure(reason, "XICS source is not owned by the requested LPID");
        return false;
    }
    if (enabled && source.state.priority == 0xff) {
        setFailure(reason, "source priority 0xff cannot be enabled");
        return false;
    }
    source.state.enabled = enabled;
    if (source.state.type == PPC64XICSSourceType::LEVEL &&
        source.state.asserted && !source.state.acknowledged && enabled) {
        source.state.pending = true;
    }
    reevaluate(ownerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::assertSource(uint32_t sourceId, uint16_t ownerLPID,
                             std::string& reason) {
    if (!validSourceId(sourceId)) {
        setFailure(reason, "invalid XICS source ID");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (!source.state.allocated || source.state.ownerLPID != ownerLPID) {
        setFailure(reason, "XICS source is not owned by the requested LPID");
        return false;
    }
    if (!source.state.enabled) {
        setFailure(reason, "XICS source is disabled");
        return false;
    }
    if (source.state.type == PPC64XICSSourceType::LEVEL) {
        source.state.asserted = true;
        if (!source.state.acknowledged) source.state.pending = true;
    } else if (!source.state.asserted) {
        source.state.asserted = true;
        source.state.pending = true;
    }
    reevaluate(ownerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::deassertSource(uint32_t sourceId, uint16_t ownerLPID,
                               std::string& reason) {
    if (!validSourceId(sourceId)) {
        setFailure(reason, "invalid XICS source ID");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (!source.state.allocated || source.state.ownerLPID != ownerLPID) {
        setFailure(reason, "XICS source is not owned by the requested LPID");
        return false;
    }
    source.state.asserted = false;
    if (source.state.type == PPC64XICSSourceType::LEVEL &&
        !source.state.acknowledged) {
        source.state.pending = false;
    }
    reevaluate(ownerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::removeSource(uint32_t sourceId, uint16_t ownerLPID,
                             std::string& reason) {
    if (!validSourceId(sourceId)) {
        setFailure(reason, "invalid XICS source ID");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (!source.state.allocated || source.state.ownerLPID != ownerLPID) {
        setFailure(reason, "XICS source is not owned by the requested LPID");
        return false;
    }
    if (source.state.asserted || source.state.pending || source.state.presented ||
        source.state.acknowledged) {
        setFailure(reason, "pending or active XICS source must be quiesced before removal");
        return false;
    }
    source.state = PPC64XICSSourceSnapshot{};
    source.state.sourceId = sourceId;
    reason.clear();
    return true;
}

bool PPC64XICS::resetSourceState(uint32_t sourceId, uint16_t ownerLPID,
                                std::string& reason) {
    if (!validSourceId(sourceId)) {
        setFailure(reason, "invalid XICS source ID");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (!source.state.allocated || source.state.ownerLPID != ownerLPID) {
        setFailure(reason, "XICS source is not owned by the requested LPID");
        return false;
    }
    auto found = presentations_.find(ownerLPID);
    if (found != presentations_.end()) {
        Presentation& state = found->second;
        if (state.presentedSourceId == sourceId) {
            state.presentedSourceId = 0;
            state.pendingPriority = 0xff;
        }
        for (auto current = state.outstanding.begin();
             current != state.outstanding.end();) {
            if (current->sourceId == sourceId) {
                if (current + 1 == state.outstanding.end()) {
                    state.cppr = static_cast<uint8_t>(
                        (current->xirr & PPC64_XICS_XIRR_CPPR_MASK) >> 24);
                }
                current = state.outstanding.erase(current);
            } else {
                ++current;
            }
        }
    }
    source.state.asserted = false;
    source.state.pending = false;
    source.state.presented = false;
    source.state.acknowledged = false;
    reevaluate(ownerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::teardownSource(uint32_t sourceId, uint16_t ownerLPID,
                              std::string& reason) {
    if (!resetSourceState(sourceId, ownerLPID, reason)) return false;
    sources_[sourceIndex(sourceId)].state = PPC64XICSSourceSnapshot{};
    sources_[sourceIndex(sourceId)].state.sourceId = sourceId;
    reevaluate(ownerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::getSource(uint32_t sourceId,
                          PPC64XICSSourceSnapshot& result) const {
    if (!validSourceId(sourceId)) return false;
    result = sources_[sourceIndex(sourceId)].state;
    return result.allocated;
}

bool PPC64XICS::setCPPR(uint8_t cppr, uint16_t callerLPID,
                        std::string& reason) {
    if (callerLPID != activeLPID_) {
        setFailure(reason, "XICS CPPR request does not match the active partition");
        return false;
    }
    presentation(callerLPID).cppr = cppr;
    reevaluate(callerLPID);
    reason.clear();
    return true;
}

uint32_t PPC64XICS::currentXirr(const Presentation& state) const {
    return (static_cast<uint32_t>(state.cppr) << 24) |
           (state.presentedSourceId & PPC64_XICS_XISR_MASK);
}

bool PPC64XICS::acknowledge(uint16_t callerLPID, uint32_t& xirr,
                            std::string& reason) {
    if (callerLPID != activeLPID_) {
        setFailure(reason, "XICS acknowledgment does not match the active partition");
        return false;
    }
    Presentation& state = presentation(callerLPID);
    xirr = currentXirr(state);
    if (state.presentedSourceId == 0) {
        state.cppr = 0xff;
        state.pendingPriority = 0xff;
        reevaluate(callerLPID);
        reason.clear();
        return true;
    }
    Source& source = sources_[sourceIndex(state.presentedSourceId)];
    if (!source.state.allocated || !source.state.pending ||
        source.state.acknowledged || source.state.ownerLPID != callerLPID) {
        setFailure(reason, "XICS presentation does not identify an eligible pending source");
        return false;
    }
    source.state.pending = false;
    source.state.presented = false;
    source.state.acknowledged = true;
    state.outstanding.push_back(PPC64XICSOutstandingSnapshot{
        xirr, source.state.sourceId, callerLPID, source.state.priority});
    state.cppr = source.state.priority;
    state.presentedSourceId = 0;
    state.pendingPriority = 0xff;
    reevaluate(callerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::endOfInterrupt(uint32_t xirr, uint16_t callerLPID,
                              std::string& reason) {
    if (callerLPID != activeLPID_) {
        setFailure(reason, "XICS EOI does not match the active partition");
        return false;
    }
    const uint32_t sourceId = xirr & PPC64_XICS_XISR_MASK;
    if (!validSourceId(sourceId)) {
        setFailure(reason, "XICS EOI XISR is not an allocated source identifier");
        return false;
    }
    Presentation& state = presentation(callerLPID);
    if (state.outstanding.empty() || state.outstanding.back().xirr != xirr ||
        state.outstanding.back().sourceId != sourceId ||
        state.outstanding.back().ownerLPID != callerLPID) {
        setFailure(reason, "XICS EOI does not match the most recently acknowledged interrupt");
        return false;
    }
    Source& source = sources_[sourceIndex(sourceId)];
    if (!source.state.allocated || !source.state.acknowledged ||
        source.state.ownerLPID != callerLPID) {
        setFailure(reason, "XICS EOI source ownership or acknowledgment state is invalid");
        return false;
    }
    state.outstanding.pop_back();
    source.state.acknowledged = false;
    if (source.state.type == PPC64XICSSourceType::LEVEL) {
        source.state.pending = source.state.enabled && source.state.asserted;
    }
    state.cppr = static_cast<uint8_t>((xirr & PPC64_XICS_XIRR_CPPR_MASK) >> 24);
    reevaluate(callerLPID);
    reason.clear();
    return true;
}

bool PPC64XICS::poll(uint32_t serverId, uint16_t callerLPID, uint32_t& xirr,
                    uint32_t& mfrr, std::string& reason) const {
    if (serverId != PPC64_XICS_SERVER_ID || callerLPID != activeLPID_) {
        setFailure(reason, "XICS IPOLL names an unsupported server or inactive partition");
        return false;
    }
    const Presentation* state = findPresentation(callerLPID);
    if (state == nullptr) {
        setFailure(reason, "XICS presentation state is unavailable");
        return false;
    }
    xirr = currentXirr(*state);
    mfrr = state->mfrr;
    reason.clear();
    return true;
}

bool PPC64XICS::externalOutputAsserted() const {
    const Presentation* state = findPresentation(activeLPID_);
    return state != nullptr && state->presentedSourceId != 0;
}

void PPC64XICS::reevaluate(uint16_t lpid) {
    Presentation& state = presentation(lpid);
    uint32_t bestId = 0;
    uint8_t bestPriority = 0xff;
    for (const Source& candidate : sources_) {
        const auto& source = candidate.state;
        if (!source.allocated || !source.enabled || !source.pending ||
            source.acknowledged || source.ownerLPID != lpid ||
            source.serverId != PPC64_XICS_SERVER_ID ||
            source.priority >= state.cppr) {
            continue;
        }
        if (bestId == 0 || source.priority < bestPriority ||
            (source.priority == bestPriority && source.sourceId < bestId)) {
            bestId = source.sourceId;
            bestPriority = source.priority;
        }
    }
    if (state.presentedSourceId != bestId) {
        if (validSourceId(state.presentedSourceId)) {
            sources_[sourceIndex(state.presentedSourceId)].state.presented = false;
        }
        state.presentedSourceId = bestId;
        if (validSourceId(bestId)) sources_[sourceIndex(bestId)].state.presented = true;
    }
    state.pendingPriority = bestId == 0 ? 0xff : bestPriority;
}

PPC64XICSSnapshot PPC64XICS::createSnapshot() const {
    PPC64XICSSnapshot snapshot;
    snapshot.enabled = true;
    snapshot.activeLPID = activeLPID_;
    for (const Source& source : sources_) {
        if (source.state.allocated) snapshot.sources.push_back(source.state);
    }
    for (const auto& [lpid, state] : presentations_) {
        PPC64XICSPresentationSnapshot saved;
        saved.ownerLPID = lpid;
        saved.cppr = state.cppr;
        saved.mfrr = state.mfrr;
        saved.presentedSourceId = state.presentedSourceId;
        saved.pendingPriority = state.pendingPriority;
        saved.outstanding = state.outstanding;
        snapshot.presentations.push_back(std::move(saved));
    }
    return snapshot;
}

bool PPC64XICS::validateSnapshot(const PPC64XICSSnapshot& snapshot,
                                 const std::vector<uint16_t>& allowedLPIDs,
                                 uint16_t expectedActiveLPID,
                                 std::string& reason) {
    if (snapshot.version != PPC64_XICS_SNAPSHOT_VERSION || !snapshot.enabled ||
        snapshot.activeLPID != expectedActiveLPID ||
        !allowedLPID(snapshot.activeLPID, allowedLPIDs) ||
        snapshot.sources.size() > PPC64_XICS_SOURCE_COUNT ||
        snapshot.presentations.size() > PPC64_XICS_SOURCE_COUNT + 1U) {
        setFailure(reason, "XICS snapshot version, active LPID, or bounded counts are invalid");
        return false;
    }
    std::array<const PPC64XICSSourceSnapshot*, PPC64_XICS_SOURCE_COUNT> sources{};
    for (const auto& source : snapshot.sources) {
        if (!validSourceId(source.sourceId) || !source.allocated ||
            (source.type != PPC64XICSSourceType::LEVEL &&
             source.type != PPC64XICSSourceType::EDGE) ||
            source.serverId != PPC64_XICS_SERVER_ID ||
            (source.enabled && source.priority == 0xff) ||
            !allowedLPID(source.ownerLPID, allowedLPIDs) ||
            (source.presented && source.acknowledged) ||
            (source.type == PPC64XICSSourceType::LEVEL && source.pending &&
             !source.asserted && !source.acknowledged) ||
            (source.type == PPC64XICSSourceType::LEVEL && source.enabled &&
             source.asserted && !source.pending && !source.acknowledged)) {
            setFailure(reason, "XICS snapshot contains an invalid source allocation or owner");
            return false;
        }
        const size_t index = sourceIndex(source.sourceId);
        if (sources[index] != nullptr) {
            setFailure(reason, "XICS snapshot contains a duplicate source allocation");
            return false;
        }
        sources[index] = &source;
    }
    std::set<uint16_t> seenPresentations;
    bool activeFound = false;
    std::set<uint32_t> globallyOutstanding;
    for (const auto& presentation : snapshot.presentations) {
        if (!allowedLPID(presentation.ownerLPID, allowedLPIDs) ||
            !seenPresentations.insert(presentation.ownerLPID).second ||
            presentation.outstanding.size() > PPC64_XICS_SOURCE_COUNT) {
            setFailure(reason, "XICS snapshot has duplicate or unauthorized presentation state");
            return false;
        }
        activeFound |= presentation.ownerLPID == snapshot.activeLPID;
        uint32_t bestSourceId = 0;
        uint8_t bestPriority = 0xff;
        for (const auto* source : sources) {
            if (source == nullptr || !source->enabled || !source->pending ||
                source->acknowledged || source->ownerLPID != presentation.ownerLPID ||
                source->serverId != PPC64_XICS_SERVER_ID ||
                source->priority >= presentation.cppr) {
                continue;
            }
            if (bestSourceId == 0 || source->priority < bestPriority ||
                (source->priority == bestPriority &&
                 source->sourceId < bestSourceId)) {
                bestSourceId = source->sourceId;
                bestPriority = source->priority;
            }
        }
        if (presentation.presentedSourceId != bestSourceId ||
            presentation.pendingPriority !=
                (bestSourceId == 0 ? 0xff : bestPriority)) {
            setFailure(reason, "XICS presentation does not match priority arbitration");
            return false;
        }
        if (presentation.presentedSourceId == 0) {
            if (presentation.pendingPriority != 0xff) {
                setFailure(reason, "XICS empty presentation has a nonempty priority");
                return false;
            }
        } else {
            if (!validSourceId(presentation.presentedSourceId)) {
                setFailure(reason, "XICS presentation contains an invalid XISR");
                return false;
            }
            const auto* source = sources[sourceIndex(presentation.presentedSourceId)];
            if (source == nullptr || !source->pending || source->acknowledged ||
                !source->enabled || !source->presented ||
                source->ownerLPID != presentation.ownerLPID ||
                source->priority != presentation.pendingPriority ||
                source->priority >= presentation.cppr) {
                setFailure(reason, "XICS presented source and ICP priority state disagree");
                return false;
            }
        }
        std::set<uint32_t> localOutstanding;
        for (const auto& outstanding : presentation.outstanding) {
            if (!validSourceId(outstanding.sourceId) ||
                (outstanding.xirr & PPC64_XICS_XISR_MASK) != outstanding.sourceId ||
                outstanding.ownerLPID != presentation.ownerLPID ||
                !localOutstanding.insert(outstanding.sourceId).second ||
                !globallyOutstanding.insert(outstanding.sourceId).second) {
                setFailure(reason, "XICS outstanding acknowledgment record is malformed");
                return false;
            }
            const auto* source = sources[sourceIndex(outstanding.sourceId)];
            if (source == nullptr || !source->acknowledged ||
                (source->pending && source->type == PPC64XICSSourceType::LEVEL) ||
                source->ownerLPID != presentation.ownerLPID ||
                source->priority != outstanding.acceptedPriority) {
                setFailure(reason, "XICS acknowledgment and source state disagree");
                return false;
            }
        }
    }
    if (!activeFound) {
        setFailure(reason, "XICS snapshot omits the active server presentation");
        return false;
    }
    for (const auto* source : sources) {
        if (source != nullptr && source->acknowledged &&
            globallyOutstanding.find(source->sourceId) == globallyOutstanding.end()) {
            setFailure(reason, "XICS acknowledged source has no presentation record");
            return false;
        }
        if (source != nullptr && source->presented) {
            const auto presentation = std::find_if(
                snapshot.presentations.begin(), snapshot.presentations.end(),
                [source](const auto& p) {
                    return p.ownerLPID == source->ownerLPID &&
                           p.presentedSourceId == source->sourceId;
                });
            if (presentation == snapshot.presentations.end()) {
                setFailure(reason, "XICS source presentation has no matching ICP state");
                return false;
            }
        }
    }
    reason.clear();
    return true;
}

bool PPC64XICS::restoreSnapshot(const PPC64XICSSnapshot& snapshot,
                                const std::vector<uint16_t>& allowedLPIDs,
                                uint16_t expectedActiveLPID,
                                std::string& reason) {
    if (!validateSnapshot(snapshot, allowedLPIDs, expectedActiveLPID, reason)) {
        return false;
    }
    std::array<Source, PPC64_XICS_SOURCE_COUNT> stagedSources{};
    for (size_t i = 0; i < stagedSources.size(); ++i) {
        stagedSources[i].state.sourceId = PPC64_XICS_FIRST_SOURCE +
                                          static_cast<uint32_t>(i);
    }
    for (const auto& source : snapshot.sources) {
        stagedSources[sourceIndex(source.sourceId)].state = source;
    }
    std::map<uint16_t, Presentation> stagedPresentations;
    for (const auto& saved : snapshot.presentations) {
        Presentation state;
        state.cppr = saved.cppr;
        state.mfrr = saved.mfrr;
        state.presentedSourceId = saved.presentedSourceId;
        state.pendingPriority = saved.pendingPriority;
        state.outstanding = saved.outstanding;
        stagedPresentations.emplace(saved.ownerLPID, std::move(state));
    }
    sources_ = std::move(stagedSources);
    presentations_ = std::move(stagedPresentations);
    activeLPID_ = snapshot.activeLPID;
    reason.clear();
    return true;
}

} // namespace ia64::ppc64

#include "aarch64/AArch64DeviceSnapshot.h"

#include "IMemory.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace aarch64 {
namespace {

constexpr std::array<uint8_t, 8> kMagic{{'G', 'X', 'A', '6', '4', 'S', 'N', 'P'}};
constexpr uint64_t kHeaderBytes = 128;
constexpr uint64_t kDigestOffset = 88;
constexpr uint64_t kDigestBytes = 32;
constexpr uint32_t kMachineSection = 1;
constexpr uint32_t kDeviceSection = 2;
constexpr uint32_t kInterruptSection = 3;
constexpr uint32_t kRequiredSections = 3;
constexpr uint64_t kSectionHeaderBytes = 16;
constexpr uint64_t kInterruptRecordBytes = 20;

struct SectionView { uint32_t type = 0; std::size_t offset = 0; std::size_t size = 0; };
struct DeviceRecord {
    uint64_t id = 0;
    uint32_t schema = 0;
    std::string type;
    std::size_t stateOffset = 0;
    std::size_t stateSize = 0;
};
struct ParsedEnvelope {
    SectionView machine;
    SectionView devices;
    SectionView interrupts;
    std::vector<DeviceRecord> deviceRecords;
    std::vector<AArch64InterruptSourceState> sourceRecords;
    std::vector<uint8_t> machineBytes;
    AArch64SnapshotInfo machineInfo;
    uint64_t totalDeviceStateBytes = 0;
    bool legacyDigest = false;
};

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

void putU32(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}
void putU64(uint8_t* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (i * 8));
}
uint32_t getU32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}
uint64_t getU64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | in[i];
    return value;
}
bool addFits(uint64_t a, uint64_t b, uint64_t& result) {
    if (b > std::numeric_limits<uint64_t>::max() - a) return false;
    result = a + b;
    return true;
}
void appendU32(std::vector<uint8_t>& out, uint32_t value) {
    const std::size_t at = out.size(); out.resize(at + 4); putU32(out.data() + at, value);
}
void appendU64(std::vector<uint8_t>& out, uint64_t value) {
    const std::size_t at = out.size(); out.resize(at + 8); putU64(out.data() + at, value);
}

class Sha256 final {
public:
    explicit Sha256(bool legacyDigest = false) : legacyDigest_(legacyDigest) {}
    void update(const uint8_t* data, std::size_t size) {
        totalBytes_ += static_cast<uint64_t>(size);
        while (size != 0) {
            const std::size_t count = std::min(size, buffer_.size() - buffered_);
            std::copy_n(data, count, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            data += count; size -= count; buffered_ += count;
            if (buffered_ == buffer_.size()) { transform(buffer_.data()); buffered_ = 0; }
        }
    }
    std::array<uint8_t, 32> finish() {
        const uint64_t bitLength = totalBytes_ * 8u;
        buffer_[buffered_++] = 0x80u;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
            transform(buffer_.data()); buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin() + 56, 0);
        for (unsigned i = 0; i < 8; ++i) buffer_[63 - i] = static_cast<uint8_t>(bitLength >> (i * 8));
        transform(buffer_.data());
        std::array<uint8_t, 32> digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return digest;
    }
private:
    static uint32_t rotr(uint32_t v, unsigned n) { return (v >> n) | (v << (32u - n)); }
    void transform(const uint8_t* block) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
        };
        uint32_t w[64]{};
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned o = i * 4;
            w[i] = (static_cast<uint32_t>(block[o]) << 24) | (static_cast<uint32_t>(block[o + 1]) << 16) |
                   (static_cast<uint32_t>(block[o + 2]) << 8) | static_cast<uint32_t>(block[o + 3]);
        }
        for (unsigned i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a=state_[0],b=state_[1],c=state_[2],d=state_[3];
        uint32_t e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for (unsigned i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr(e,6)^rotr(e,11)^rotr(e,25), ch=(e&f)^(~e&g);
            const uint32_t roundConstant = legacyDigest_ && i == 34 ? 0x4d2c2dfcu : k[i];
            const uint32_t t1=h+s1+ch+roundConstant+w[i], s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            const uint32_t maj=(a&b)^(a&c)^(b&c), t2=s0+maj;
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;
        state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }
    std::array<uint32_t,8> state_{{0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u}};
    std::array<uint8_t,64> buffer_{};
    std::size_t buffered_ = 0;
    uint64_t totalBytes_ = 0;
    bool legacyDigest_ = false;
};

std::array<uint8_t,32> digest(const std::vector<uint8_t>& bytes, bool legacyDigest = false) {
    Sha256 hash(legacyDigest);
    hash.update(bytes.data(), static_cast<std::size_t>(kDigestOffset));
    constexpr std::array<uint8_t,kDigestBytes> zero{};
    hash.update(zero.data(), zero.size());
    const std::size_t tail = static_cast<std::size_t>(kDigestOffset + kDigestBytes);
    hash.update(bytes.data() + tail, bytes.size() - tail);
    return hash.finish();
}

bool validType(const uint8_t* text, std::size_t size) {
    if (size == 0 || size > kMaxDeviceTypeIdBytes || text[0] < 'a' || text[0] > 'z') return false;
    for (std::size_t i = 0; i < size; ++i) {
        const uint8_t c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

bool parseEnvelope(const std::vector<uint8_t>& bytes, ParsedEnvelope& parsed,
                   std::string* error, const AArch64DeviceSnapshotLimits& limits) {
    if (bytes.size() < kHeaderBytes) { setError(error, "device snapshot is shorter than its fixed header"); return false; }
    if (bytes.size() > limits.maxSnapshotBytes) { setError(error, "device snapshot exceeds the configured byte limit"); return false; }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) { setError(error, "device snapshot magic is invalid"); return false; }
    if (getU32(bytes.data()+8) != AArch64DeviceSnapshot::kFormatVersion) { setError(error, "unsupported device snapshot envelope version"); return false; }
    if (getU32(bytes.data()+12) != kHeaderBytes) { setError(error, "device snapshot header length is invalid"); return false; }
    if (getU32(bytes.data()+16) != AArch64ExecutionSnapshot::kArchitectureId) { setError(error, "device snapshot architecture is not AArch64"); return false; }
    if (getU32(bytes.data()+20) != AArch64ExecutionSnapshot::kExecutionProfileId || getU32(bytes.data()+24) != 1) { setError(error, "unsupported device snapshot execution profile"); return false; }
    if (getU32(bytes.data()+28) != kRequiredSections || getU64(bytes.data()+32) != bytes.size() || getU64(bytes.data()+40) != kHeaderBytes) { setError(error, "device snapshot section count or total length is inconsistent"); return false; }
    for (std::size_t i = 48; i < kDigestOffset; ++i) {
        if (bytes[i] != 0) { setError(error, "device snapshot reserved header bytes are nonzero"); return false; }
    }
    if (getU32(bytes.data()+120) != 0 || getU32(bytes.data()+124) != 0) { setError(error, "device snapshot reserved fields are nonzero"); return false; }

    auto actualDigest = digest(bytes);
    uint8_t difference = 0;
    for (std::size_t i = 0; i < actualDigest.size(); ++i) difference |= static_cast<uint8_t>(actualDigest[i] ^ bytes[static_cast<std::size_t>(kDigestOffset)+i]);
    if (difference != 0) {
        // Preserve snapshots emitted by the original partial AARCH64-9 build,
        // whose SHA-256 round table had one incorrect constant. New writers
        // always use standard SHA-256; this read-only fallback is compatibility
        // for those already-created v2 envelopes.
        actualDigest = digest(bytes, true);
        difference = 0;
        for (std::size_t i = 0; i < actualDigest.size(); ++i) difference |= static_cast<uint8_t>(actualDigest[i] ^ bytes[static_cast<std::size_t>(kDigestOffset)+i]);
        if (difference != 0) { setError(error, "device snapshot SHA-256 integrity check failed"); return false; }
        parsed.legacyDigest = true;
    }

    std::array<SectionView,4> sections{};
    std::size_t cursor = static_cast<std::size_t>(kHeaderBytes);
    for (uint32_t i = 0; i < kRequiredSections; ++i) {
        if (cursor > bytes.size() || bytes.size() - cursor < kSectionHeaderBytes) { setError(error, "truncated device snapshot section header"); return false; }
        const uint32_t type = getU32(bytes.data()+cursor);
        const uint32_t flags = getU32(bytes.data()+cursor+4);
        const uint64_t length = getU64(bytes.data()+cursor+8);
        uint64_t end64 = 0;
        if (flags != 0 || !addFits(static_cast<uint64_t>(cursor) + kSectionHeaderBytes, length, end64) || end64 > bytes.size()) { setError(error, "device snapshot section length or flags are invalid"); return false; }
        if (type < kMachineSection || type > kInterruptSection || sections[type].type != 0) { setError(error, "unknown or duplicate device snapshot section type"); return false; }
        sections[type] = SectionView{type, cursor + static_cast<std::size_t>(kSectionHeaderBytes), static_cast<std::size_t>(length)};
        cursor = static_cast<std::size_t>(end64);
    }
    if (cursor != bytes.size() || sections[1].type == 0 || sections[2].type == 0 || sections[3].type == 0) { setError(error, "device snapshot has trailing data or missing mandatory sections"); return false; }
    parsed.machine = sections[1]; parsed.devices = sections[2]; parsed.interrupts = sections[3];

    try { parsed.machineBytes.assign(bytes.begin()+static_cast<std::ptrdiff_t>(parsed.machine.offset), bytes.begin()+static_cast<std::ptrdiff_t>(parsed.machine.offset+parsed.machine.size)); }
    catch (...) { setError(error, "unable to stage nested RAM snapshot validation"); return false; }
    if (parsed.machineBytes.size() > limits.machine.maxSnapshotBytes || !AArch64ExecutionSnapshot::validate(parsed.machineBytes, &parsed.machineInfo, error, limits.machine)) return false;

    const uint8_t* dp = bytes.data() + parsed.devices.offset;
    std::size_t remaining = parsed.devices.size;
    if (remaining < 4) { setError(error, "device state section is truncated"); return false; }
    const uint32_t count = getU32(dp); dp += 4; remaining -= 4;
    if (count > kMaxRegisteredDevices) { setError(error, "device count exceeds the supported limit"); return false; }
    try { parsed.deviceRecords.reserve(count); } catch (...) { setError(error, "unable to allocate bounded device topology records"); return false; }
    uint64_t previousId = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (remaining < 17) { setError(error, "device state record header is truncated"); return false; }
        DeviceRecord record;
        record.id = getU64(dp); record.schema = getU32(dp+8);
        const uint8_t typeBytes = dp[12]; const uint32_t stateBytes = getU32(dp+13);
        dp += 17; remaining -= 17;
        if (record.id == 0 || record.id <= previousId || record.schema == 0 || !validType(dp, typeBytes) || stateBytes > kMaxDeviceStateBytes) { setError(error, "device identity, type, schema, ordering, or payload bound is invalid"); return false; }
        uint64_t needed = static_cast<uint64_t>(typeBytes) + stateBytes;
        if (needed > remaining) { setError(error, "device type or state payload is truncated"); return false; }
        try { record.type.assign(reinterpret_cast<const char*>(dp), typeBytes); }
        catch (...) { setError(error, "unable to allocate bounded device type identifier"); return false; }
        dp += typeBytes; remaining -= typeBytes;
        record.stateOffset = static_cast<std::size_t>(dp - bytes.data());
        record.stateSize = stateBytes;
        dp += stateBytes; remaining -= stateBytes;
        if (parsed.totalDeviceStateBytes > kMaxTotalDeviceStateBytes - stateBytes) { setError(error, "total device state exceeds the supported limit"); return false; }
        parsed.totalDeviceStateBytes += stateBytes;
        previousId = record.id;
        parsed.deviceRecords.push_back(std::move(record));
    }
    if (remaining != 0) { setError(error, "device state section contains trailing data"); return false; }

    const uint8_t* sp = bytes.data() + parsed.interrupts.offset;
    remaining = parsed.interrupts.size;
    if (remaining < 4) { setError(error, "interrupt-source section is truncated"); return false; }
    const uint32_t sourceCount = getU32(sp); sp += 4; remaining -= 4;
    if (sourceCount > kMaxInterruptSources || remaining != static_cast<std::size_t>(sourceCount) * kInterruptRecordBytes) { setError(error, "interrupt-source count or section length is invalid"); return false; }
    try { parsed.sourceRecords.reserve(sourceCount); } catch (...) { setError(error, "unable to allocate bounded interrupt-source records"); return false; }
    previousId = 0;
    for (uint32_t i = 0; i < sourceCount; ++i) {
        AArch64InterruptSourceState source;
        source.id = getU64(sp); source.ownerDeviceId = getU64(sp+8);
        source.mode = static_cast<AArch64InterruptMode>(sp[16]);
        const uint8_t asserted = sp[17], pending = sp[18], reserved = sp[19];
        sp += kInterruptRecordBytes;
        if (source.id == 0 || source.id <= previousId || source.ownerDeviceId == 0 ||
            (source.mode != AArch64InterruptMode::Level && source.mode != AArch64InterruptMode::Edge) ||
            asserted > 1 || pending > 1 || reserved != 0 ||
            (source.mode == AArch64InterruptMode::Level && pending != 0) ||
            (source.mode == AArch64InterruptMode::Edge && asserted != 0)) {
            setError(error, "interrupt-source identity, ownership encoding, mode, or state is invalid"); return false;
        }
        source.asserted = asserted != 0; source.pending = pending != 0;
        const auto owner = std::lower_bound(parsed.deviceRecords.begin(), parsed.deviceRecords.end(),
            source.ownerDeviceId, [](const DeviceRecord& record, uint64_t id) { return record.id < id; });
        if (owner == parsed.deviceRecords.end() || owner->id != source.ownerDeviceId) {
            setError(error, "interrupt source refers to an owner absent from the snapshot topology"); return false;
        }
        previousId = source.id;
        parsed.sourceRecords.push_back(source);
    }
    if (error != nullptr) error->clear();
    return true;
}

template <typename Callback>
struct ScopeExit {
    Callback callback;
    ~ScopeExit() { callback(); }
};

bool quiesceAll(const std::vector<AArch64StatefulDevice*>& devices,
                std::vector<AArch64StatefulDevice*>& quiesced,
                std::string* error) {
    for (AArch64StatefulDevice* device : devices) {
        try {
            if (!device->quiesce(error)) {
                if (error != nullptr && error->empty()) *error = "device " + std::to_string(device->instanceId()) + " refused quiescence";
                try { device->resume(); } catch (...) {}
                for (auto it = quiesced.rbegin(); it != quiesced.rend(); ++it) (*it)->resume();
                quiesced.clear();
                return false;
            }
            quiesced.push_back(device);
        } catch (const std::exception& exception) {
            setError(error, "device " + std::to_string(device->instanceId()) + " quiesce threw: " + exception.what());
            try { device->resume(); } catch (...) {}
            for (auto it = quiesced.rbegin(); it != quiesced.rend(); ++it) (*it)->resume();
            quiesced.clear();
            return false;
        } catch (...) {
            setError(error, "device quiesce threw");
            try { device->resume(); } catch (...) {}
            for (auto it = quiesced.rbegin(); it != quiesced.rend(); ++it) (*it)->resume();
            quiesced.clear();
            return false;
        }
    }
    return true;
}
void resumeAll(std::vector<AArch64StatefulDevice*>& quiesced) noexcept {
    for (auto it = quiesced.rbegin(); it != quiesced.rend(); ++it) (*it)->resume();
    quiesced.clear();
}

void appendSection(std::vector<uint8_t>& out, uint32_t type,
                   const std::vector<uint8_t>& payload) {
    const std::size_t at = out.size(); out.resize(at + static_cast<std::size_t>(kSectionHeaderBytes));
    putU32(out.data()+at, type); putU32(out.data()+at+4, 0); putU64(out.data()+at+8, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

bool AArch64DeviceSnapshot::beginMachineBoundary(const AArch64ISAPlugin& cpu,
                                                 const ia64::Memory& memory,
                                                 std::string* error) {
    auto& cpuState = const_cast<Cpu&>(cpu.cpu_);
    {
        std::unique_lock<std::recursive_mutex> cpuLock(cpuState.executionMutex_, std::try_to_lock);
        if (!cpuLock.owns_lock()) {
            setError(error, "AArch64 CPU is executing; snapshot coordination requires an instruction boundary");
            return false;
        }
        if (cpu.instructionDecodedPending_) {
            setError(error, "AArch64 plugin has a decoded instruction pending execution");
            return false;
        }
        if (cpuState.snapshotBoundaryActive_) {
            setError(error, "another snapshot coordinator already owns the AArch64 CPU boundary");
            return false;
        }
        cpuState.snapshotBoundaryActive_ = true;
        cpuState.allowCoordinatedSnapshotCodec_ = false;
        cpuState.snapshotBoundaryOwner_ = std::this_thread::get_id();
    }

    auto& ram = const_cast<ia64::Memory&>(memory);
    std::unique_lock<std::recursive_mutex> memoryLock(ram.accessMutex_, std::try_to_lock);
    if (!memoryLock.owns_lock()) {
        std::lock_guard<std::recursive_mutex> cpuLock(cpuState.executionMutex_);
        cpuState.snapshotBoundaryActive_ = false;
        cpuState.snapshotBoundaryOwner_ = {};
        setError(error, "guest RAM has an active memory operation; snapshot coordination requires quiescence");
        return false;
    }
    if (ram.snapshotBoundaryActive_) {
        memoryLock.unlock();
        std::lock_guard<std::recursive_mutex> cpuLock(cpuState.executionMutex_);
        cpuState.snapshotBoundaryActive_ = false;
        cpuState.snapshotBoundaryOwner_ = {};
        setError(error, "another snapshot coordinator already owns guest RAM");
        return false;
    }
    ram.snapshotBoundaryActive_ = true;
    if (error != nullptr) error->clear();
    return true;
}

void AArch64DeviceSnapshot::endMachineBoundary(const AArch64ISAPlugin& cpu,
                                               const ia64::Memory& memory) noexcept {
    try {
        auto& ram = const_cast<ia64::Memory&>(memory);
        {
            std::lock_guard<std::recursive_mutex> memoryLock(ram.accessMutex_);
            ram.snapshotBoundaryActive_ = false;
        }
        auto& cpuState = const_cast<Cpu&>(cpu.cpu_);
        std::lock_guard<std::recursive_mutex> cpuLock(cpuState.executionMutex_);
        cpuState.allowCoordinatedSnapshotCodec_ = false;
        cpuState.snapshotBoundaryOwner_ = {};
        cpuState.snapshotBoundaryActive_ = false;
    } catch (...) {
        // A standard mutex failure during cleanup is unrecoverable but must
        // not escape a scope guard during stack unwinding.
    }
}

bool AArch64DeviceSnapshot::setNestedCodecAccess(const AArch64ISAPlugin& cpu,
                                                  bool enabled,
                                                  std::string* error) {
    auto& cpuState = const_cast<Cpu&>(cpu.cpu_);
    std::lock_guard<std::recursive_mutex> cpuLock(cpuState.executionMutex_);
    if (!cpuState.snapshotBoundaryActive_ ||
        cpuState.snapshotBoundaryOwner_ != std::this_thread::get_id()) {
        setError(error, "nested CPU/RAM snapshot codec access is outside its owning coordinator");
        return false;
    }
    cpuState.allowCoordinatedSnapshotCodec_ = enabled;
    return true;
}

bool AArch64DeviceSnapshot::capture(const AArch64ISAPlugin& cpu,
                                    const ia64::IMemory& memory,
                                    AArch64DeviceRegistry& devices,
                                    std::vector<uint8_t>& snapshot,
                                    std::string* error,
                                    const AArch64DeviceSnapshotLimits& limits) {
    snapshot.clear();
    if (error != nullptr) error->clear();
    std::vector<AArch64StatefulDevice*> registered;
    std::vector<AArch64InterruptSourceState> sources;
    if (!devices.beginSnapshot(registered, sources, error)) return false;
    ScopeExit operation{[&devices] { devices.endSnapshot(); }};
    const auto* ram = dynamic_cast<const ia64::Memory*>(&memory);
    if (ram == nullptr) { setError(error, "device snapshot requires the production contiguous ia64::Memory backend"); return false; }
    if (!beginMachineBoundary(cpu, *ram, error)) return false;
    ScopeExit boundary{[&cpu, ram] { endMachineBoundary(cpu, *ram); }};
    std::vector<AArch64StatefulDevice*> quiesced;
    try { quiesced.reserve(registered.size()); }
    catch (...) { setError(error, "unable to allocate device quiescence bookkeeping"); return false; }
    if (!quiesceAll(registered, quiesced, error)) return false;

    std::vector<uint8_t> machine;
    bool machineCaptured = false;
    if (!setNestedCodecAccess(cpu, true, error)) { resumeAll(quiesced); return false; }
    {
        ScopeExit nestedCodec{[&cpu] { std::string ignored; setNestedCodecAccess(cpu, false, &ignored); }};
        machineCaptured = AArch64ExecutionSnapshot::capture(cpu, memory, machine, error, limits.machine);
    }
    if (!machineCaptured) {
        resumeAll(quiesced); return false;
    }
    std::vector<uint8_t> devicePayload, sourcePayload;
    try {
        appendU32(devicePayload, static_cast<uint32_t>(registered.size()));
        uint64_t totalState = 0;
        for (AArch64StatefulDevice* device : registered) {
            std::vector<uint8_t> state;
            if (!device->captureState(state, error)) {
                if (error != nullptr && error->empty()) *error = "device " + std::to_string(device->instanceId()) + " state capture failed";
                resumeAll(quiesced); return false;
            }
            const std::string type = device->typeId();
            const auto registration = devices.devices_.find(device->instanceId());
            if (!validType(reinterpret_cast<const uint8_t*>(type.data()), type.size()) ||
                registration == devices.devices_.end() || registration->second.device.get() != device ||
                type != registration->second.type || device->stateSchemaVersion() != registration->second.schema ||
                !device->supportsSnapshot() || state.size() > registration->second.maxStateBytes ||
                state.size() > device->maxStateBytes() ||
                state.size() > kMaxDeviceStateBytes || totalState > kMaxTotalDeviceStateBytes - state.size()) {
                setError(error, "device " + std::to_string(device->instanceId()) + " emitted invalid identity or an oversized state");
                resumeAll(quiesced); return false;
            }
            totalState += state.size();
            appendU64(devicePayload, device->instanceId());
            appendU32(devicePayload, device->stateSchemaVersion());
            devicePayload.push_back(static_cast<uint8_t>(type.size()));
            appendU32(devicePayload, static_cast<uint32_t>(state.size()));
            devicePayload.insert(devicePayload.end(), type.begin(), type.end());
            devicePayload.insert(devicePayload.end(), state.begin(), state.end());
        }
        appendU32(sourcePayload, static_cast<uint32_t>(sources.size()));
        for (const auto& source : sources) {
            appendU64(sourcePayload, source.id); appendU64(sourcePayload, source.ownerDeviceId);
            sourcePayload.push_back(static_cast<uint8_t>(source.mode));
            sourcePayload.push_back(source.asserted ? 1 : 0);
            sourcePayload.push_back(source.pending ? 1 : 0); sourcePayload.push_back(0);
        }
        uint64_t total = kHeaderBytes;
        for (const auto* section : {&machine, &devicePayload, &sourcePayload}) {
            uint64_t next = 0;
            if (!addFits(total, kSectionHeaderBytes, next) || !addFits(next, section->size(), total)) {
                setError(error, "device snapshot total size overflow"); resumeAll(quiesced); return false;
            }
        }
        if (total > limits.maxSnapshotBytes || total > std::numeric_limits<std::size_t>::max()) {
            setError(error, "device snapshot exceeds the configured total-size limit"); resumeAll(quiesced); return false;
        }
        std::vector<uint8_t> output(static_cast<std::size_t>(kHeaderBytes), 0);
        std::copy(kMagic.begin(), kMagic.end(), output.begin());
        putU32(output.data()+8, kFormatVersion); putU32(output.data()+12, static_cast<uint32_t>(kHeaderBytes));
        putU32(output.data()+16, AArch64ExecutionSnapshot::kArchitectureId);
        putU32(output.data()+20, AArch64ExecutionSnapshot::kExecutionProfileId);
        putU32(output.data()+24, 1); putU32(output.data()+28, kRequiredSections);
        putU64(output.data()+32, total); putU64(output.data()+40, kHeaderBytes);
        appendSection(output, kMachineSection, machine);
        appendSection(output, kDeviceSection, devicePayload);
        appendSection(output, kInterruptSection, sourcePayload);
        const auto sum = digest(output);
        std::copy(sum.begin(), sum.end(), output.begin()+static_cast<std::ptrdiff_t>(kDigestOffset));
        snapshot.swap(output);
    } catch (const std::exception& exception) {
        setError(error, std::string("device snapshot capture failed: ") + exception.what());
        resumeAll(quiesced); snapshot.clear(); return false;
    } catch (...) {
        setError(error, "device snapshot capture failed due to a host allocation or callback error");
        resumeAll(quiesced); snapshot.clear(); return false;
    }
    resumeAll(quiesced);
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceSnapshot::validate(const std::vector<uint8_t>& snapshot,
                                     AArch64DeviceSnapshotInfo* info,
                                     std::string* error,
                                     const AArch64DeviceSnapshotLimits& limits) {
    if (snapshot.size() >= 12 && getU32(snapshot.data()+8) == AArch64ExecutionSnapshot::kFormatVersion) {
        AArch64SnapshotInfo legacy;
        if (!AArch64ExecutionSnapshot::validate(snapshot, &legacy, error, limits.machine)) return false;
        if (info != nullptr) {
            static_cast<AArch64SnapshotInfo&>(*info) = legacy;
            info->envelopeVersion = AArch64ExecutionSnapshot::kFormatVersion;
            info->deviceCount = 0;
            info->interruptSourceCount = 0;
            info->totalDeviceStateBytes = 0;
        }
        return true;
    }
    ParsedEnvelope parsed;
    if (!parseEnvelope(snapshot, parsed, error, limits)) return false;
    if (info != nullptr) {
        static_cast<AArch64SnapshotInfo&>(*info) = parsed.machineInfo;
        info->envelopeVersion = kFormatVersion;
        info->deviceCount = static_cast<uint32_t>(parsed.deviceRecords.size());
        info->interruptSourceCount = static_cast<uint32_t>(parsed.sourceRecords.size());
        info->totalDeviceStateBytes = parsed.totalDeviceStateBytes;
        info->formatVersion = kFormatVersion;
        info->totalBytes = snapshot.size();
        if (parsed.legacyDigest) {
            std::copy_n(snapshot.begin() + static_cast<std::ptrdiff_t>(kDigestOffset),
                        info->sha256.size(), info->sha256.begin());
        } else {
            info->sha256 = digest(snapshot);
        }
    }
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64DeviceSnapshot::restore(AArch64ISAPlugin& cpu, ia64::IMemory& memory,
                                    AArch64DeviceRegistry& devices,
                                    const std::vector<uint8_t>& snapshot,
                                    std::string* error,
                                    const AArch64DeviceSnapshotLimits& limits) {
    if (snapshot.size() >= 12 && getU32(snapshot.data()+8) == AArch64ExecutionSnapshot::kFormatVersion) {
        if (!AArch64ExecutionSnapshot::validate(snapshot, nullptr, error, limits.machine)) return false;
        std::vector<AArch64StatefulDevice*> legacyDevices;
        std::vector<AArch64InterruptSourceState> legacySources;
        if (!devices.beginSnapshot(legacyDevices, legacySources, error)) return false;
        ScopeExit legacyOperation{[&devices] { devices.endSnapshot(); }};
        if (!legacyDevices.empty() || !legacySources.empty()) {
            setError(error, "legacy RAM-only snapshot is incompatible with a stateful-device topology");
            return false;
        }
        return AArch64ExecutionSnapshot::restore(cpu, memory, snapshot, error, limits.machine);
    }
    ParsedEnvelope parsed;
    if (!parseEnvelope(snapshot, parsed, error, limits)) return false;
    auto* ram = dynamic_cast<ia64::Memory*>(&memory);
    if (ram == nullptr) { setError(error, "device restore requires the production contiguous ia64::Memory backend"); return false; }

    std::vector<AArch64StatefulDevice*> registered;
    std::vector<AArch64InterruptSourceState> currentSources;
    if (!devices.beginSnapshot(registered, currentSources, error)) return false;
    ScopeExit operation{[&devices] { devices.endSnapshot(); }};
    if (!beginMachineBoundary(cpu, *ram, error)) return false;
    ScopeExit boundary{[&cpu, ram] { endMachineBoundary(cpu, *ram); }};
    if (registered.size() != parsed.deviceRecords.size() || currentSources.size() != parsed.sourceRecords.size()) {
        setError(error, "destination device or interrupt-source topology count does not match snapshot"); return false;
    }
    std::vector<std::unique_ptr<AArch64DeviceRestoreToken>> tokens;
    try { tokens.reserve(registered.size()); }
    catch (...) { setError(error, "unable to allocate device restore staging table"); return false; }

    for (std::size_t i = 0; i < registered.size(); ++i) {
        AArch64StatefulDevice* device = registered[i];
        const DeviceRecord& record = parsed.deviceRecords[i];
        std::string type;
        try { type = device->typeId(); }
        catch (...) { setError(error, "destination device type identity callback failed"); return false; }
        const auto registration = devices.devices_.find(device->instanceId());
        if (device->instanceId() != record.id || type != record.type ||
            device->stateSchemaVersion() != record.schema || !device->supportsSnapshot() ||
            registration == devices.devices_.end() || registration->second.device.get() != device ||
            registration->second.type != record.type || registration->second.schema != record.schema ||
            record.stateSize > registration->second.maxStateBytes ||
            record.stateSize > device->maxStateBytes()) {
            setError(error, "destination device identity, type, schema, or state bound does not match snapshot"); return false;
        }
    }
    for (std::size_t i = 0; i < currentSources.size(); ++i) {
        const auto& current = currentSources[i]; const auto& saved = parsed.sourceRecords[i];
        if (current.id != saved.id || current.ownerDeviceId != saved.ownerDeviceId || current.mode != saved.mode) {
            setError(error, "destination interrupt-source identity, owner, or trigger mode does not match snapshot"); return false;
        }
    }

    for (std::size_t i = 0; i < registered.size(); ++i) {
        std::unique_ptr<AArch64DeviceRestoreToken> token;
        const DeviceRecord& record = parsed.deviceRecords[i];
        try {
            if (!registered[i]->stageRestore(snapshot.data()+record.stateOffset, record.stateSize, token, error) || !token) {
                if (error != nullptr && error->empty()) *error = "device " + std::to_string(record.id) + " rejected restore preparation";
                return false;
            }
        } catch (const std::exception& exception) {
            setError(error, "device " + std::to_string(record.id) + " restore preparation threw: " + exception.what()); return false;
        } catch (...) {
            setError(error, "device restore preparation threw"); return false;
        }
        tokens.push_back(std::move(token));
    }

    std::vector<AArch64StatefulDevice*> quiesced;
    try { quiesced.reserve(registered.size()); }
    catch (...) { setError(error, "unable to allocate restore quiescence bookkeeping"); return false; }
    if (!quiesceAll(registered, quiesced, error)) return false;

    bool restored = false;
    try {
        {
            std::unique_lock<std::recursive_mutex> cpuLock(cpu.cpu_.executionMutex_, std::try_to_lock);
            if (!cpuLock.owns_lock()) {
                setError(error, "destination AArch64 CPU is executing; restore requires quiescence");
            } else if (cpu.instructionDecodedPending_) {
                setError(error, "destination AArch64 plugin has a decoded instruction pending execution");
            } else {
                std::unique_lock<std::recursive_mutex> memoryLock(ram->accessMutex_, std::try_to_lock);
                if (!memoryLock.owns_lock()) {
                    setError(error, "destination guest RAM has an active memory operation");
                } else {
                    // Acquire every lock that the no-fail commit needs before
                    // the inner coordinator crosses its CPU/RAM commit point.
                    std::unique_lock<std::mutex> sourceLock(devices.mutex_, std::try_to_lock);
                    if (!sourceLock.owns_lock()) {
                        setError(error, "interrupt-source registry is busy during restore");
                    } else {
                        bool machineRestored = false;
                        if (setNestedCodecAccess(cpu, true, error)) {
                            ScopeExit nestedCodec{[&cpu] { setNestedCodecAccess(cpu, false, nullptr); }};
                            machineRestored = AArch64ExecutionSnapshot::restore(
                                cpu, memory, parsed.machineBytes, error, limits.machine);
                        }
                        if (machineRestored) {
                        // The recursive CPU/RAM locks remain held. Source IDs
                        // were validated while the operation gate held topology.
                        for (const auto& source : parsed.sourceRecords) {
                            auto live = devices.sources_.find(source.id);
                            live->second.asserted = source.asserted;
                            live->second.pending = source.pending;
                        }
                        sourceLock.unlock();
                        for (std::size_t i = 0; i < registered.size(); ++i)
                            registered[i]->commitRestore(std::move(tokens[i]));
                        restored = true;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& exception) {
        setError(error, std::string("restore coordination failed before device commit: ") + exception.what());
    } catch (...) {
        setError(error, "restore coordination failed before device commit");
    }
    resumeAll(quiesced);
    if (!restored) return false;
    if (error != nullptr) error->clear();
    return true;
}

} // namespace aarch64

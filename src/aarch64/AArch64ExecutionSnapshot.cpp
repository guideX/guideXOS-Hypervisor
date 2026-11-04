#include "aarch64/AArch64ExecutionSnapshot.h"

#include "IMemory.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>

namespace aarch64 {
namespace {

constexpr std::array<uint8_t, 8> kMagic{{'G', 'X', 'A', '6', '4', 'S', 'N', 'P'}};
constexpr uint64_t kCpuOffset = AArch64ExecutionSnapshot::kHeaderBytes;
constexpr uint64_t kRegionTableOffset = kCpuOffset +
    AArch64ExecutionSnapshot::kCpuCheckpointBytes;
constexpr uint64_t kRamOffset = kRegionTableOffset +
    AArch64ExecutionSnapshot::kRegionRecordBytes;
constexpr uint64_t kDigestOffset = 88;
constexpr uint64_t kDigestBytes = 32;
constexpr uint32_t kRegionRam = 1;
constexpr uint32_t kNoFlags = 0;

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

void putU32LE(uint8_t* out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

void putU64LE(uint8_t* out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint32_t getU32LE(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) |
           (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) |
           (static_cast<uint32_t>(in[3]) << 24);
}

uint64_t getU64LE(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | in[i];
    return value;
}

class Sha256 final {
public:
    void update(const uint8_t* data, size_t size) {
        totalBytes_ += static_cast<uint64_t>(size);
        while (size != 0) {
            const size_t count = std::min(size, buffer_.size() - buffered_);
            std::copy_n(data, count, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
            data += count;
            size -= count;
            buffered_ += count;
            if (buffered_ == buffer_.size()) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        const uint64_t bitLength = totalBytes_ * 8u;
        buffer_[buffered_++] = 0x80u;
        if (buffered_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.end(), 0);
            transform(buffer_.data());
            buffered_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_),
                  buffer_.begin() + 56, 0);
        for (unsigned i = 0; i < 8; ++i) {
            buffer_[63 - i] = static_cast<uint8_t>(bitLength >> (i * 8));
        }
        transform(buffer_.data());

        std::array<uint8_t, 32> digest{};
        for (size_t i = 0; i < state_.size(); ++i) {
            digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return digest;
    }

private:
    static uint32_t rotateRight(uint32_t value, unsigned amount) {
        return (value >> amount) | (value << (32u - amount));
    }

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
        uint32_t words[64]{};
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned offset = i * 4;
            words[i] = (static_cast<uint32_t>(block[offset]) << 24) |
                       (static_cast<uint32_t>(block[offset + 1]) << 16) |
                       (static_cast<uint32_t>(block[offset + 2]) << 8) |
                       static_cast<uint32_t>(block[offset + 3]);
        }
        for (unsigned i = 16; i < 64; ++i) {
            const uint32_t s0 = rotateRight(words[i - 15], 7) ^
                                rotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const uint32_t s1 = rotateRight(words[i - 2], 17) ^
                                rotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (unsigned i = 0; i < 64; ++i) {
            const uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
            const uint32_t choice = (e & f) ^ (~e & g);
            const uint32_t temp1 = h + sum1 + choice + k[i] + words[i];
            const uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint32_t, 8> state_{{
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    }};
    std::array<uint8_t, 64> buffer_{};
    size_t buffered_ = 0;
    uint64_t totalBytes_ = 0;
};

std::array<uint8_t, 32> snapshotDigest(const std::vector<uint8_t>& bytes) {
    Sha256 hash;
    hash.update(bytes.data(), static_cast<size_t>(kDigestOffset));
    constexpr std::array<uint8_t, kDigestBytes> zeroDigest{};
    hash.update(zeroDigest.data(), zeroDigest.size());
    const size_t tailOffset = static_cast<size_t>(kDigestOffset + kDigestBytes);
    hash.update(bytes.data() + tailOffset, bytes.size() - tailOffset);
    return hash.finish();
}

bool validateSnapshot(const std::vector<uint8_t>& snapshot,
                      AArch64SnapshotInfo* info,
                      std::string* error,
                      const AArch64SnapshotLimits& limits) {
    if (snapshot.size() < AArch64ExecutionSnapshot::kHeaderBytes) {
        setError(error, "snapshot is shorter than its fixed header");
        return false;
    }
    if (snapshot.size() > limits.maxSnapshotBytes) {
        setError(error, "snapshot exceeds the configured byte limit");
        return false;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), snapshot.begin())) {
        setError(error, "snapshot magic is invalid");
        return false;
    }
    if (getU32LE(snapshot.data() + 8) != AArch64ExecutionSnapshot::kFormatVersion) {
        setError(error, "unsupported snapshot envelope version");
        return false;
    }
    if (getU32LE(snapshot.data() + 12) != AArch64ExecutionSnapshot::kHeaderBytes) {
        setError(error, "snapshot header length is invalid");
        return false;
    }
    const uint32_t architectureId = getU32LE(snapshot.data() + 16);
    if (architectureId != AArch64ExecutionSnapshot::kArchitectureId) {
        setError(error, "snapshot architecture is not AArch64");
        return false;
    }
    const uint32_t profileId = getU32LE(snapshot.data() + 20);
    if (profileId != AArch64ExecutionSnapshot::kExecutionProfileId) {
        setError(error, "unsupported AArch64 execution profile");
        return false;
    }
    if (getU32LE(snapshot.data() + 24) != AArch64ExecutionSnapshot::kCpuCount) {
        setError(error, "snapshot CPU count is unsupported");
        return false;
    }
    if (getU32LE(snapshot.data() + 28) != AArch64ExecutionSnapshot::kCpuCheckpointVersion) {
        setError(error, "snapshot CPU checkpoint version is unsupported");
        return false;
    }

    const uint64_t cpuOffset = getU64LE(snapshot.data() + 32);
    const uint64_t cpuBytes = getU64LE(snapshot.data() + 40);
    const uint64_t regionOffset = getU64LE(snapshot.data() + 48);
    const uint32_t regionCount = getU32LE(snapshot.data() + 56);
    const uint32_t regionRecordBytes = getU32LE(snapshot.data() + 60);
    const uint64_t ramOffset = getU64LE(snapshot.data() + 64);
    const uint64_t ramBytes = getU64LE(snapshot.data() + 72);
    const uint64_t totalBytes = getU64LE(snapshot.data() + 80);
    if (cpuOffset != kCpuOffset || cpuBytes != AArch64ExecutionSnapshot::kCpuCheckpointBytes ||
        regionOffset != kRegionTableOffset || regionCount != 1 ||
        regionRecordBytes != AArch64ExecutionSnapshot::kRegionRecordBytes ||
        ramOffset != kRamOffset || totalBytes != snapshot.size()) {
        setError(error, "snapshot section boundaries or lengths are inconsistent");
        return false;
    }
    if (ramBytes == 0 || (ramBytes & 0xFFFu) != 0) {
        setError(error, "guest RAM size must be nonzero and 4 KiB aligned");
        return false;
    }
    if (ramBytes > limits.maxRamBytes ||
        ramBytes > std::numeric_limits<uint64_t>::max() - kRamOffset ||
        kRamOffset + ramBytes != totalBytes ||
        ramBytes > std::numeric_limits<size_t>::max()) {
        setError(error, "guest RAM size exceeds limits or overflows snapshot boundaries");
        return false;
    }
    if (getU32LE(snapshot.data() + 120) != kNoFlags ||
        getU32LE(snapshot.data() + 124) != 0) {
        setError(error, "snapshot flags or reserved header field is unsupported");
        return false;
    }

    const size_t region = static_cast<size_t>(regionOffset);
    if (getU64LE(snapshot.data() + region) != 0 ||
        getU64LE(snapshot.data() + region + 8) != ramBytes ||
        getU32LE(snapshot.data() + region + 16) != kRegionRam ||
        getU32LE(snapshot.data() + region + 20) != 0) {
        setError(error, "physical RAM region metadata is invalid or unsupported");
        return false;
    }

    const size_t cpu = static_cast<size_t>(cpuOffset);
    if (getU32LE(snapshot.data() + cpu) != kStateMagic ||
        getU32LE(snapshot.data() + cpu + 4) != AArch64ExecutionSnapshot::kCpuCheckpointVersion) {
        setError(error, "embedded AArch64 CPU checkpoint identity is invalid");
        return false;
    }
    AArch64State parsedCpu;
    try {
        parsedCpu.deserialize(snapshot.data() + cpu);
    } catch (const std::exception&) {
        setError(error, "embedded AArch64 CPU checkpoint is malformed");
        return false;
    } catch (...) {
        setError(error, "embedded AArch64 CPU checkpoint could not be validated");
        return false;
    }

    const std::array<uint8_t, 32> digest = snapshotDigest(snapshot);
    uint8_t difference = 0;
    for (size_t i = 0; i < digest.size(); ++i) {
        difference |= static_cast<uint8_t>(digest[i] ^ snapshot[static_cast<size_t>(kDigestOffset) + i]);
    }
    if (difference != 0) {
        setError(error, "snapshot SHA-256 integrity check failed");
        return false;
    }

    if (info != nullptr) {
        info->formatVersion = AArch64ExecutionSnapshot::kFormatVersion;
        info->architectureId = architectureId;
        info->executionProfileId = profileId;
        info->cpuCount = AArch64ExecutionSnapshot::kCpuCount;
        info->cpuCheckpointVersion = AArch64ExecutionSnapshot::kCpuCheckpointVersion;
        info->cpuCheckpointBytes = cpuBytes;
        info->guestRamBytes = ramBytes;
        info->totalBytes = totalBytes;
        info->savedPc = parsedCpu.pc();
        info->sha256 = digest;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool memorySupportsSnapshot(bool genericMmuEnabled, bool hasDevices,
                            size_t ramBytes, uint64_t maxRamBytes,
                            std::string* error) {
    if (genericMmuEnabled) {
        setError(error, "AArch64 snapshot requires the generic memory MMU to be disabled");
        return false;
    }
    if (hasDevices) {
        setError(error, "AArch64 snapshot does not support memory-mapped devices");
        return false;
    }
    if (ramBytes == 0 || (ramBytes & 0xFFFu) != 0) {
        setError(error, "physical RAM must be nonzero and 4 KiB aligned");
        return false;
    }
    if (ramBytes > maxRamBytes) {
        setError(error, "physical RAM exceeds the configured snapshot limit");
        return false;
    }
    return true;
}

} // namespace

bool AArch64ExecutionSnapshot::capture(const AArch64ISAPlugin& cpu,
                                       const ia64::IMemory& memory,
                                       std::vector<uint8_t>& snapshot,
                                       std::string* error,
                                       const AArch64SnapshotLimits& limits) {
    snapshot.clear();
    if (error != nullptr) error->clear();
    const auto* ram = dynamic_cast<const ia64::Memory*>(&memory);
    if (ram == nullptr) {
        setError(error, "snapshot requires the production contiguous ia64::Memory backend");
        return false;
    }

    std::unique_lock<std::recursive_mutex> cpuLock(cpu.cpu_.executionMutex_, std::try_to_lock);
    if (!cpuLock.owns_lock()) {
        setError(error, "AArch64 CPU is executing; capture requires a quiescent boundary");
        return false;
    }
    if (cpu.instructionDecodedPending_) {
        setError(error, "AArch64 plugin has a decoded instruction pending execution");
        return false;
    }
    std::unique_lock<std::recursive_mutex> memoryLock(ram->accessMutex_, std::try_to_lock);
    if (!memoryLock.owns_lock()) {
        setError(error, "guest RAM has an active memory operation");
        return false;
    }
    if (!memorySupportsSnapshot(ram->mmu_ == nullptr || ram->mmu_->IsEnabled(),
                                !ram->devices_.empty(), ram->data_.size(),
                                limits.maxRamBytes, error)) return false;

    const uint64_t ramBytes = static_cast<uint64_t>(ram->data_.size());
    if (ramBytes > std::numeric_limits<uint64_t>::max() - kRamOffset) {
        setError(error, "guest RAM length overflows snapshot size arithmetic");
        return false;
    }
    const uint64_t totalBytes = kRamOffset + ramBytes;
    if (totalBytes > limits.maxSnapshotBytes || totalBytes > std::numeric_limits<size_t>::max()) {
        setError(error, "snapshot exceeds the configured total byte limit");
        return false;
    }

    try {
        const std::vector<uint8_t> cpuRecord = cpu.serialize_state();
        if (cpuRecord.size() != kCpuCheckpointBytes ||
            getU32LE(cpuRecord.data() + 4) != kCpuCheckpointVersion) {
            setError(error, "production CPU checkpoint is not the supported v3 record");
            return false;
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(totalBytes), 0);
        std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
        putU32LE(bytes.data() + 8, kFormatVersion);
        putU32LE(bytes.data() + 12, static_cast<uint32_t>(kHeaderBytes));
        putU32LE(bytes.data() + 16, kArchitectureId);
        putU32LE(bytes.data() + 20, kExecutionProfileId);
        putU32LE(bytes.data() + 24, kCpuCount);
        putU32LE(bytes.data() + 28, kCpuCheckpointVersion);
        putU64LE(bytes.data() + 32, kCpuOffset);
        putU64LE(bytes.data() + 40, kCpuCheckpointBytes);
        putU64LE(bytes.data() + 48, kRegionTableOffset);
        putU32LE(bytes.data() + 56, 1);
        putU32LE(bytes.data() + 60, static_cast<uint32_t>(kRegionRecordBytes));
        putU64LE(bytes.data() + 64, kRamOffset);
        putU64LE(bytes.data() + 72, ramBytes);
        putU64LE(bytes.data() + 80, totalBytes);
        std::copy(cpuRecord.begin(), cpuRecord.end(), bytes.begin() + static_cast<std::ptrdiff_t>(kCpuOffset));
        const size_t region = static_cast<size_t>(kRegionTableOffset);
        putU64LE(bytes.data() + region, 0);
        putU64LE(bytes.data() + region + 8, ramBytes);
        putU32LE(bytes.data() + region + 16, kRegionRam);
        putU32LE(bytes.data() + region + 20, 0);
        std::copy(ram->data_.begin(), ram->data_.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(kRamOffset));
        const std::array<uint8_t, 32> digest = snapshotDigest(bytes);
        std::copy(digest.begin(), digest.end(),
                  bytes.begin() + static_cast<std::ptrdiff_t>(kDigestOffset));
        snapshot.swap(bytes);
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("snapshot capture failed: ") + exception.what());
        snapshot.clear();
        return false;
    } catch (...) {
        setError(error, "snapshot capture failed due to an unknown host error");
        snapshot.clear();
        return false;
    }
}

bool AArch64ExecutionSnapshot::validate(const std::vector<uint8_t>& snapshot,
                                       AArch64SnapshotInfo* info,
                                       std::string* error,
                                       const AArch64SnapshotLimits& limits) {
    return validateSnapshot(snapshot, info, error, limits);
}

bool AArch64ExecutionSnapshot::restore(AArch64ISAPlugin& cpu,
                                       ia64::IMemory& memory,
                                       const std::vector<uint8_t>& snapshot,
                                       std::string* error,
                                       const AArch64SnapshotLimits& limits) {
    AArch64SnapshotInfo info;
    if (!validateSnapshot(snapshot, &info, error, limits)) return false;
    auto* ram = dynamic_cast<ia64::Memory*>(&memory);
    if (ram == nullptr) {
        setError(error, "restore requires the production contiguous ia64::Memory backend");
        return false;
    }

    AArch64State replacementCpu;
    try {
        replacementCpu.deserialize(snapshot.data() + static_cast<size_t>(kCpuOffset));
    } catch (...) {
        setError(error, "validated CPU state could not be staged for restoration");
        return false;
    }
    std::vector<uint8_t> replacementRam;
    try {
        const size_t offset = static_cast<size_t>(kRamOffset);
        replacementRam.assign(snapshot.begin() + static_cast<std::ptrdiff_t>(offset), snapshot.end());
    } catch (const std::exception& exception) {
        setError(error, std::string("unable to stage replacement guest RAM: ") + exception.what());
        return false;
    } catch (...) {
        setError(error, "unable to stage replacement guest RAM");
        return false;
    }

    std::unique_lock<std::recursive_mutex> cpuLock(cpu.cpu_.executionMutex_, std::try_to_lock);
    if (!cpuLock.owns_lock()) {
        setError(error, "destination AArch64 CPU is executing; restore requires quiescence");
        return false;
    }
    if (cpu.instructionDecodedPending_) {
        setError(error, "destination AArch64 plugin has a decoded instruction pending execution");
        return false;
    }
    std::unique_lock<std::recursive_mutex> memoryLock(ram->accessMutex_, std::try_to_lock);
    if (!memoryLock.owns_lock()) {
        setError(error, "destination guest RAM has an active memory operation");
        return false;
    }
    if (!memorySupportsSnapshot(ram->mmu_ == nullptr || ram->mmu_->IsEnabled(),
                                !ram->devices_.empty(), ram->data_.size(),
                                limits.maxRamBytes, error)) return false;
    if (ram->data_.size() != replacementRam.size()) {
        setError(error, "destination guest RAM geometry does not match the snapshot");
        return false;
    }

    // All parsing, validation and allocations have completed. The following
    // operations are bounded assignments over prevalidated storage and do not
    // allocate; both guest components remain locked until the commit finishes.
    std::copy(replacementRam.begin(), replacementRam.end(), ram->data_.begin());
    replacementCpu.applyTo(cpu.cpu_);
    cpu.state_ = replacementCpu;
    cpu.cpu_.lastFault_ = Fault::None;
    cpu.cpu_.lastError_.clear();
    cpu.lastDecoded_.valid = false;
    cpu.lastDecoded_.hasMemoryAbort = false;
    cpu.instructionDecodedPending_ = false;
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64ExecutionSnapshot::saveFile(const std::filesystem::path& path,
                                        const std::vector<uint8_t>& snapshot,
                                        std::string* error,
                                        const AArch64SnapshotLimits& limits) {
    if (!validateSnapshot(snapshot, nullptr, error, limits)) return false;
    if (snapshot.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        setError(error, "snapshot is too large for the file stream implementation");
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        setError(error, "snapshot destination already exists; refusing to overwrite it");
        return false;
    }
    if (ec) {
        setError(error, "unable to inspect snapshot destination: " + ec.message());
        return false;
    }

    std::filesystem::path temporary;
    bool opened = false;
    std::ofstream output;
    for (unsigned attempt = 0; attempt < 32 && !opened; ++attempt) {
        temporary = path;
        temporary += ".aarch64-tmp-" + std::to_string(attempt);
        ec.clear();
        if (std::filesystem::exists(temporary, ec)) continue;
        if (ec) break;
        output.open(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
        opened = output.is_open();
    }
    if (!opened) {
        setError(error, "unable to create a unique temporary snapshot file");
        return false;
    }

    output.write(reinterpret_cast<const char*>(snapshot.data()),
                 static_cast<std::streamsize>(snapshot.size()));
    output.flush();
    output.close();
    if (!output) {
        std::filesystem::remove(temporary, ec);
        setError(error, "snapshot file write or close failed");
        return false;
    }
    // Linking a complete same-directory temporary file publishes it atomically
    // and fails if another writer created the destination in the meantime.
    ec.clear();
    std::filesystem::create_hard_link(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        setError(error, "unable to publish complete snapshot file: " + ec.message());
        return false;
    }
    std::filesystem::remove(temporary, ec);
    if (error != nullptr) error->clear();
    return true;
}

bool AArch64ExecutionSnapshot::loadFile(const std::filesystem::path& path,
                                        std::vector<uint8_t>& snapshot,
                                        std::string* error,
                                        const AArch64SnapshotLimits& limits) {
    snapshot.clear();
    std::error_code ec;
    const uint64_t fileBytes = std::filesystem::file_size(path, ec);
    if (ec) {
        setError(error, "unable to read snapshot file size: " + ec.message());
        return false;
    }
    if (fileBytes > limits.maxSnapshotBytes || fileBytes > std::numeric_limits<size_t>::max() ||
        fileBytes > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        setError(error, "snapshot file exceeds the configured read limit");
        return false;
    }
    try {
        std::vector<uint8_t> bytes(static_cast<size_t>(fileBytes));
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) {
            setError(error, "unable to open snapshot file");
            return false;
        }
        if (!bytes.empty()) {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        if (!input || input.peek() != std::char_traits<char>::eof()) {
            setError(error, "snapshot file changed while being read or is truncated");
            return false;
        }
        if (!validateSnapshot(bytes, nullptr, error, limits)) return false;
        snapshot.swap(bytes);
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("unable to stage snapshot file: ") + exception.what());
        snapshot.clear();
        return false;
    } catch (...) {
        setError(error, "unable to stage snapshot file");
        snapshot.clear();
        return false;
    }
}

} // namespace aarch64

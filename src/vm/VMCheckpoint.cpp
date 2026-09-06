#include "VMCheckpoint.h"

#include "IA64ISAPlugin.h"
#include "SimpleCPUScheduler.h"
#include "VirtualMachine.h"
#include "VMSnapshot.h"
#include "Console.h"
#include "Timer.h"
#include "InterruptController.h"
#include "FramebufferDevice.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ia64 {

namespace {

constexpr char kMagic[] = "GXIA64VM";
constexpr size_t kMagicSize = sizeof(kMagic) - 1;
constexpr uint32_t kPayloadVersion = 1;
constexpr size_t kMaxHeaderString = 1U << 20;
constexpr size_t kMaxPayloadBytes = (2ULL << 30);
constexpr size_t kMaxPageEntries = 16U * 1024U * 1024U;
constexpr size_t kMaxCpuCount = 64;
constexpr size_t kMaxDeviceBytes = 256U * 1024U * 1024U;
constexpr uint32_t kLittleEndianMarker = 0x01020304U;
constexpr const char* kCheckpointBuildIdentity = "guideXOS-Hypervisor/IA64-checkpoint-v1";

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) *error = message;
}

class Writer {
public:
    std::vector<uint8_t> data;

    void u8(uint8_t value) { data.push_back(value); }
    void u32(uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            data.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void u64(uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            data.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
        }
    }
    void raw(const uint8_t* bytes, size_t size) {
        if (size != 0) data.insert(data.end(), bytes, bytes + size);
    }
    void bytes(const std::vector<uint8_t>& value) {
        u64(static_cast<uint64_t>(value.size()));
        raw(value.data(), value.size());
    }
    void string(const std::string& value) {
        u64(static_cast<uint64_t>(value.size()));
        raw(reinterpret_cast<const uint8_t*>(value.data()), value.size());
    }
    void patchU32(size_t offset, uint32_t value) {
        if (offset + 4 > data.size()) throw std::runtime_error("checkpoint header patch out of range");
        for (unsigned shift = 0; shift < 32; shift += 8) {
            data[offset + shift / 8] = static_cast<uint8_t>((value >> shift) & 0xFFU);
        }
    }
};

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& data)
        : data_(data), offset_(0), valid_(true) {}

    bool valid() const { return valid_ && offset_ <= data_.size(); }
    size_t remaining() const { return valid() ? data_.size() - offset_ : 0; }
    uint8_t u8() {
        if (remaining() < 1) { valid_ = false; return 0; }
        return data_[offset_++];
    }
    uint32_t u32() {
        if (remaining() < 4) { valid_ = false; return 0; }
        uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<uint32_t>(data_[offset_++]) << shift;
        }
        return value;
    }
    uint64_t u64() {
        if (remaining() < 8) { valid_ = false; return 0; }
        uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 8) {
            value |= static_cast<uint64_t>(data_[offset_++]) << shift;
        }
        return value;
    }
    std::vector<uint8_t> bytes(size_t maximum) {
        const uint64_t count = u64();
        if (!valid_ || count > maximum || count > remaining()) {
            valid_ = false;
            return {};
        }
        std::vector<uint8_t> result(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += static_cast<size_t>(count);
        return result;
    }
    std::vector<uint8_t> rawBytes(size_t count) {
        if (!valid_ || count > remaining()) {
            valid_ = false;
            return {};
        }
        std::vector<uint8_t> result(
            data_.begin() + static_cast<std::ptrdiff_t>(offset_),
            data_.begin() + static_cast<std::ptrdiff_t>(offset_ + count));
        offset_ += count;
        return result;
    }
    std::string string(size_t maximum) {
        const uint64_t count = u64();
        if (!valid_ || count > maximum || count > remaining()) {
            valid_ = false;
            return {};
        }
        std::string result(reinterpret_cast<const char*>(data_.data() + offset_),
                           static_cast<size_t>(count));
        offset_ += static_cast<size_t>(count);
        return result;
    }

private:
    const std::vector<uint8_t>& data_;
    size_t offset_;
    bool valid_;
};

class Sha256 {
public:
    Sha256() : h_{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U},
        bitCount_(0), blockSize_(0), block_{} {}

    void update(const uint8_t* data, size_t size) {
        if (size == 0) return;
        bitCount_ += static_cast<uint64_t>(size) * 8ULL;
        while (size != 0) {
            const size_t copy = std::min(size, block_.size() - blockSize_);
            std::memcpy(block_.data() + blockSize_, data, copy);
            blockSize_ += copy;
            data += copy;
            size -= copy;
            if (blockSize_ == block_.size()) {
                transform(block_.data());
                blockSize_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> final() {
        const uint64_t length = bitCount_;
        block_[blockSize_++] = 0x80;
        if (blockSize_ > 56) {
            while (blockSize_ < 64) block_[blockSize_++] = 0;
            transform(block_.data());
            blockSize_ = 0;
        }
        while (blockSize_ < 56) block_[blockSize_++] = 0;
        for (unsigned shift = 56; shift >= 8; shift -= 8) {
            block_[blockSize_++] = static_cast<uint8_t>((length >> shift) & 0xFFU);
        }
        block_[blockSize_++] = static_cast<uint8_t>(length & 0xFFU);
        transform(block_.data());

        std::array<uint8_t, 32> result{};
        for (size_t i = 0; i < h_.size(); ++i) {
            result[i * 4] = static_cast<uint8_t>(h_[i] >> 24);
            result[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            result[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            result[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
        }
        return result;
    }

private:
    static uint32_t rotr(uint32_t value, unsigned count) {
        return (value >> count) | (value << (32 - count));
    }
    static uint32_t choose(uint32_t e, uint32_t f, uint32_t g) { return (e & f) ^ (~e & g); }
    static uint32_t majority(uint32_t a, uint32_t b, uint32_t c) { return (a & b) ^ (a & c) ^ (b & c); }
    static uint32_t bigSigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
    static uint32_t bigSigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
    static uint32_t smallSigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
    static uint32_t smallSigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

    void transform(const uint8_t* block) {
        static constexpr uint32_t k[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};
        uint32_t w[64]{};
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < 64; ++i) w[i] = smallSigma1(w[i - 2]) + w[i - 7] + smallSigma0(w[i - 15]) + w[i - 16];
        uint32_t a=h_[0], b=h_[1], c=h_[2], d=h_[3], e=h_[4], f=h_[5], g=h_[6], h=h_[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t t1 = h + bigSigma1(e) + choose(e,f,g) + k[i] + w[i];
            const uint32_t t2 = bigSigma0(a) + majority(a,b,c);
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=h;
    }

    std::array<uint32_t, 8> h_;
    uint64_t bitCount_;
    size_t blockSize_;
    std::array<uint8_t, 64> block_;
};

std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data) {
    Sha256 hash;
    hash.update(data.data(), data.size());
    return hash.final();
}

struct Header {
    uint32_t version = 0;
    uint64_t payloadSize = 0;
    std::array<uint8_t, 32> payloadHash{};
    uint64_t ramSize = 0;
    uint32_t cpuCount = 0;
    uint32_t endianMarker = 0;
    uint64_t pageSize = 0;
    bool mmuEnabled = false;
    uint64_t consoleBase = 0;
    uint64_t timerBase = 0;
    uint64_t framebufferBase = 0;
    uint64_t framebufferSize = 0;
    uint64_t checkpointCycles = 0;
    uint64_t checkpointIP = 0;
    uint32_t checkpointSlot = 0;
    std::string guestIdentity;
    std::string architecture;
    std::string checkpointKind;
    std::string buildIdentity;
};

void writeHeader(Writer& writer, const Header& header) {
    writer.raw(reinterpret_cast<const uint8_t*>(kMagic), kMagicSize);
    writer.u32(header.version);
    writer.u32(0); // patched after variable-length fields are written
    writer.u64(header.payloadSize);
    writer.raw(header.payloadHash.data(), header.payloadHash.size());
    writer.u64(header.ramSize);
    writer.u32(header.cpuCount);
    writer.u32(header.endianMarker);
    writer.u64(header.pageSize);
    writer.u8(header.mmuEnabled ? 1 : 0);
    for (int i = 0; i < 7; ++i) writer.u8(0);
    writer.u64(header.consoleBase);
    writer.u64(header.timerBase);
    writer.u64(header.framebufferBase);
    writer.u64(header.framebufferSize);
    writer.u64(header.checkpointCycles);
    writer.u64(header.checkpointIP);
    writer.u32(header.checkpointSlot);
    writer.u32(0);
    writer.string(header.guestIdentity);
    writer.string(header.architecture);
    writer.string(header.checkpointKind);
    writer.string(header.buildIdentity);
    writer.patchU32(12, static_cast<uint32_t>(writer.data.size()));
}

bool readExact(std::istream& input, uint8_t* data, size_t size) {
    if (size == 0) return true;
    input.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return input.good() && static_cast<size_t>(input.gcount()) == size;
}

bool readU8(std::istream& input, uint8_t& value) { return readExact(input, &value, 1); }
bool readU32(std::istream& input, uint32_t& value) {
    uint8_t bytes[4];
    if (!readExact(input, bytes, sizeof(bytes))) return false;
    value = static_cast<uint32_t>(bytes[0]) |
            (static_cast<uint32_t>(bytes[1]) << 8) |
            (static_cast<uint32_t>(bytes[2]) << 16) |
            (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}
bool readU64(std::istream& input, uint64_t& value) {
    uint8_t bytes[8];
    if (!readExact(input, bytes, sizeof(bytes))) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) value |= static_cast<uint64_t>(bytes[shift / 8]) << shift;
    return true;
}
bool readString(std::istream& input, std::string& value, size_t maximum) {
    uint64_t size = 0;
    if (!readU64(input, size) || size > maximum) return false;
    value.resize(static_cast<size_t>(size));
    return readExact(input, reinterpret_cast<uint8_t*>(value.data()), value.size());
}

bool readHeader(std::istream& input, Header& header, std::string* error) {
    const std::streampos headerStart = input.tellg();
    std::array<uint8_t, kMagicSize> magic{};
    if (!readExact(input, magic.data(), magic.size()) ||
        std::memcmp(magic.data(), kMagic, kMagicSize) != 0) {
        setError(error, "invalid checkpoint magic or truncated header");
        return false;
    }
    uint32_t headerSize = 0;
    if (!readU32(input, header.version) || !readU32(input, headerSize) ||
        header.version != VMCheckpointCodec::kFormatVersion) {
        setError(error, "unsupported checkpoint format version");
        return false;
    }
    if (!readU64(input, header.payloadSize) || header.payloadSize > kMaxPayloadBytes ||
        !readExact(input, header.payloadHash.data(), header.payloadHash.size()) ||
        !readU64(input, header.ramSize) || !readU32(input, header.cpuCount)) {
        setError(error, "truncated checkpoint header");
        return false;
    }
    uint32_t reserved = 0;
    if (!readU32(input, header.endianMarker) || header.endianMarker != kLittleEndianMarker ||
        !readU64(input, header.pageSize)) {
        setError(error, "truncated checkpoint machine header");
        return false;
    }
    uint8_t mmu = 0;
    if (!readU8(input, mmu)) return false;
    header.mmuEnabled = mmu != 0;
    std::array<uint8_t, 7> padding{};
    if (!readExact(input, padding.data(), padding.size()) ||
        !readU64(input, header.consoleBase) || !readU64(input, header.timerBase) ||
        !readU64(input, header.framebufferBase) || !readU64(input, header.framebufferSize) ||
        !readU64(input, header.checkpointCycles) || !readU64(input, header.checkpointIP) ||
        !readU32(input, header.checkpointSlot) || !readU32(input, reserved) ||
        !readString(input, header.guestIdentity, kMaxHeaderString) ||
        !readString(input, header.architecture, kMaxHeaderString) ||
        !readString(input, header.checkpointKind, kMaxHeaderString) ||
        !readString(input, header.buildIdentity, kMaxHeaderString)) {
        setError(error, "truncated checkpoint header strings");
        return false;
    }
    const std::streampos payloadStart = input.tellg();
    if (headerStart == std::streampos(-1) || payloadStart == std::streampos(-1) ||
        headerSize != static_cast<uint32_t>(payloadStart - headerStart)) {
        setError(error, "checkpoint header size is inconsistent");
        return false;
    }
    return true;
}

void writeConsole(Writer& writer, const ConsoleDeviceState& state) {
    writer.u64(state.baseAddress);
    writer.string(state.currentBuffer);
    writer.u64(static_cast<uint64_t>(state.completeLines.size()));
    for (const auto& line : state.completeLines) writer.string(line);
    writer.string(state.currentLine);
    writer.u64(static_cast<uint64_t>(state.maxLines));
    writer.u64(state.totalBytesWritten);
}

bool readConsole(Reader& reader, ConsoleDeviceState& state) {
    state.baseAddress = reader.u64();
    state.currentBuffer = reader.string(kMaxDeviceBytes);
    const uint64_t lineCount = reader.u64();
    if (!reader.valid() || lineCount > 1'000'000) return false;
    state.completeLines.clear();
    state.completeLines.reserve(static_cast<size_t>(lineCount));
    for (uint64_t i = 0; i < lineCount; ++i) state.completeLines.push_back(reader.string(kMaxHeaderString));
    state.currentLine = reader.string(kMaxHeaderString);
    state.maxLines = static_cast<size_t>(reader.u64());
    state.totalBytesWritten = reader.u64();
    return reader.valid();
}

void writeTimer(Writer& writer, const TimerDeviceState& state) {
    writer.u64(state.baseAddress); writer.u64(state.intervalCycles); writer.u64(state.elapsedCycles);
    writer.u8(state.interruptVector); writer.u8(state.enabled ? 1 : 0); writer.u8(state.periodic ? 1 : 0);
    writer.u8(state.interruptPending ? 1 : 0);
}

bool readTimer(Reader& reader, TimerDeviceState& state) {
    state.baseAddress = reader.u64(); state.intervalCycles = reader.u64(); state.elapsedCycles = reader.u64();
    state.interruptVector = reader.u8(); state.enabled = reader.u8() != 0; state.periodic = reader.u8() != 0;
    state.interruptPending = reader.u8() != 0;
    return reader.valid();
}

void writeInterrupts(Writer& writer, const InterruptControllerState& state) {
    writer.u64(static_cast<uint64_t>(state.pendingInterrupts.size()));
    for (bool value : state.pendingInterrupts) writer.u8(value ? 1 : 0);
    writer.u64(static_cast<uint64_t>(state.maskedInterrupts.size()));
    for (bool value : state.maskedInterrupts) writer.u8(value ? 1 : 0);
    writer.u64(state.vectorBase); writer.u8(state.enabled ? 1 : 0);
    writer.u64(static_cast<uint64_t>(state.sources.size()));
    for (const InterruptSource& source : state.sources) {
        writer.u64(static_cast<uint64_t>(source.id)); writer.string(source.name);
        writer.u8(source.vector); writer.u8(source.enabled ? 1 : 0);
    }
    writer.u64(static_cast<uint64_t>(state.nextSourceId));
}

bool readInterrupts(Reader& reader, InterruptControllerState& state) {
    const uint64_t pendingCount = reader.u64();
    if (!reader.valid() || pendingCount > 256) return false;
    state.pendingInterrupts.resize(static_cast<size_t>(pendingCount));
    for (size_t i = 0; i < state.pendingInterrupts.size(); ++i) state.pendingInterrupts[i] = reader.u8() != 0;
    const uint64_t maskedCount = reader.u64();
    if (!reader.valid() || maskedCount > 256) return false;
    state.maskedInterrupts.resize(static_cast<size_t>(maskedCount));
    for (size_t i = 0; i < state.maskedInterrupts.size(); ++i) state.maskedInterrupts[i] = reader.u8() != 0;
    state.vectorBase = reader.u64(); state.enabled = reader.u8() != 0;
    const uint64_t sourceCount = reader.u64();
    if (!reader.valid() || sourceCount > 1'000'000) return false;
    state.sources.clear(); state.sources.reserve(static_cast<size_t>(sourceCount));
    for (uint64_t i = 0; i < sourceCount; ++i) {
        InterruptSource source;
        source.id = static_cast<size_t>(reader.u64()); source.name = reader.string(kMaxHeaderString);
        source.vector = reader.u8(); source.enabled = reader.u8() != 0;
        state.sources.push_back(std::move(source));
    }
    state.nextSourceId = static_cast<size_t>(reader.u64());
    return reader.valid();
}

struct ParsedCPU {
    uint32_t id = 0;
    CPUExecutionState executionState = CPUExecutionState::IDLE;
    uint64_t cycles = 0;
    uint64_t instructions = 0;
    uint64_t idle = 0;
    bool enabled = false;
    uint64_t lastActivation = 0;
    bool halted = false;
    size_t slot = 0;
    bool bundleValid = false;
    std::vector<uint8_t> pendingInterrupts;
    uint64_t interruptVectorBase = 0;
    std::vector<uint64_t> pendingCallInputs;
    std::vector<uint8_t> plugin;
};

} // namespace

bool VMCheckpointCodec::write(const VirtualMachine& vm,
                              const std::string& path,
                              const std::string& guestIdentity,
                              std::string* error) {
    try {
        if (guestIdentity.empty()) { setError(error, "guest identity is required"); return false; }
        if (vm.cpus_.empty() || vm.cpus_.size() > kMaxCpuCount || !vm.memory_ ||
            !vm.consoleDevice_ || !vm.timerDevice_ || !vm.interruptController_ || !vm.framebufferDevice_) {
            setError(error, "checkpoint machine is incomplete"); return false;
        }
        const auto* scheduler = dynamic_cast<const SimpleCPUScheduler*>(vm.scheduler_.get());
        if (scheduler == nullptr) { setError(error, "unsupported scheduler for checkpoint"); return false; }

        Writer payload;
        payload.u32(kPayloadVersion);
        payload.u32(static_cast<uint32_t>(vm.cpus_.size()));
        for (const CPUContext& context : vm.cpus_) {
            if (!context.cpu || !context.isaPlugin) { setError(error, "CPU lacks IA-64 plugin state"); return false; }
            const auto* plugin = dynamic_cast<const IA64ISAPlugin*>(context.isaPlugin.get());
            if (plugin == nullptr) { setError(error, "checkpoint requires IA-64 plugin CPUs"); return false; }
            const CPURuntimeStateSnapshot runtime = context.cpu->createSnapshot();
            if (runtime.bundleValid || runtime.currentSlot != 0) {
                setError(error, "checkpoint is not at an instruction bundle boundary"); return false;
            }
            const std::vector<uint8_t> pluginState = plugin->serializeCheckpointState();
            if (pluginState.empty()) { setError(error, "IA-64 plugin rejected non-handoff checkpoint state"); return false; }
            payload.u32(context.cpuId);
            payload.u32(static_cast<uint32_t>(context.state));
            payload.u64(context.cyclesExecuted); payload.u64(context.instructionsExecuted); payload.u64(context.idleCycles);
            payload.u8(context.enabled ? 1 : 0); payload.u64(context.lastActivationTime);
            payload.u8(runtime.halted ? 1 : 0); payload.u64(static_cast<uint64_t>(runtime.currentSlot));
            payload.u8(runtime.bundleValid ? 1 : 0); payload.bytes(runtime.pendingInterrupts);
            payload.u64(runtime.interruptVectorBase);
            payload.u64(static_cast<uint64_t>(runtime.pendingCallInputs.size()));
            for (uint64_t value : runtime.pendingCallInputs) payload.u64(value);
            payload.bytes(pluginState);
        }

        const size_t ramSize = vm.memory_->GetTotalSize();
        payload.u64(static_cast<uint64_t>(ramSize));
        payload.raw(vm.memory_->GetRawData(), ramSize);
        const MMU& mmu = vm.memory_->GetMMU();
        payload.u64(static_cast<uint64_t>(mmu.GetPageTable().size()));
        for (const auto& [virtualAddress, entry] : mmu.GetPageTable()) {
            payload.u64(virtualAddress); payload.u64(entry.physicalAddress);
            payload.u8(static_cast<uint8_t>(entry.permissions)); payload.u8(entry.present ? 1 : 0);
            payload.u8(entry.accessed ? 1 : 0); payload.u8(entry.dirty ? 1 : 0);
        }

        writeConsole(payload, vm.consoleDevice_->createSnapshot());
        writeTimer(payload, vm.timerDevice_->createSnapshot());
        writeInterrupts(payload, vm.interruptController_->createSnapshot());
        const FramebufferDeviceState framebuffer = vm.framebufferDevice_->createSnapshot();
        payload.u64(framebuffer.baseAddress); payload.u64(framebuffer.width); payload.u64(framebuffer.height);
        payload.u64(framebuffer.pitch); payload.bytes(framebuffer.framebuffer);

        payload.u32(static_cast<uint32_t>(vm.state_)); payload.u64(vm.cyclesExecuted_);
        payload.u32(static_cast<uint32_t>(vm.activeCPUIndex_));
        payload.u32(static_cast<uint32_t>(vm.bootStateMachine_.getCurrentState()));
        payload.u32(static_cast<uint32_t>(vm.bootStateMachine_.getPreviousState()));
        const SimpleCPUSchedulerState schedulerState = scheduler->createSnapshot();
        payload.u32(static_cast<uint32_t>(schedulerState.lastCPUIndex));
        payload.u32(static_cast<uint32_t>(schedulerState.numCPUs)); payload.u64(schedulerState.quantumSize);
        payload.u64(static_cast<uint64_t>(schedulerState.cpuBundleCounters.size()));
        for (uint64_t value : schedulerState.cpuBundleCounters) payload.u64(value);

        if (payload.data.size() > kMaxPayloadBytes) { setError(error, "checkpoint payload is too large"); return false; }
        Header header;
        header.version = kFormatVersion;
        header.payloadSize = payload.data.size();
        header.payloadHash = sha256(payload.data);
        header.ramSize = ramSize;
        header.cpuCount = static_cast<uint32_t>(vm.cpus_.size());
        header.endianMarker = kLittleEndianMarker;
        header.pageSize = mmu.GetPageSize(); header.mmuEnabled = mmu.IsEnabled();
        header.consoleBase = vm.consoleDevice_->GetBaseAddress(); header.timerBase = vm.timerDevice_->GetBaseAddress();
        header.framebufferBase = framebuffer.baseAddress; header.framebufferSize = framebuffer.framebuffer.size();
        header.checkpointCycles = vm.cyclesExecuted_; header.checkpointIP = vm.cpus_[0].cpu->getIP(); header.checkpointSlot = 0;
        header.guestIdentity = guestIdentity; header.architecture = "IA-64";
        header.checkpointKind = "after-ExitBootServices-kernel-handoff";
        header.buildIdentity = kCheckpointBuildIdentity;
        Writer headerBytes; writeHeader(headerBytes, header);

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) { setError(error, "cannot open checkpoint for writing"); return false; }
        output.write(reinterpret_cast<const char*>(headerBytes.data.data()), static_cast<std::streamsize>(headerBytes.data.size()));
        output.write(reinterpret_cast<const char*>(payload.data.data()), static_cast<std::streamsize>(payload.data.size()));
        if (!output.good()) { setError(error, "checkpoint write failed"); return false; }
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("checkpoint write exception: ") + exception.what());
        return false;
    }
}

bool VMCheckpointCodec::read(VirtualMachine& vm,
                             const std::string& path,
                             const std::string& expectedGuestIdentity,
                             std::string* error) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) { setError(error, "cannot open checkpoint for reading"); return false; }
        Header header;
        if (!readHeader(input, header, error)) return false;
        if (header.architecture != "IA-64" ||
            header.buildIdentity != kCheckpointBuildIdentity ||
            header.checkpointKind != "after-ExitBootServices-kernel-handoff" ||
            (!expectedGuestIdentity.empty() && header.guestIdentity != expectedGuestIdentity)) {
            setError(error, "checkpoint guest identity or architecture mismatch"); return false;
        }
        if (header.ramSize != vm.memory_->GetTotalSize() || header.cpuCount != vm.cpus_.size() ||
            header.pageSize != vm.memory_->GetMMU().GetPageSize() ||
            header.mmuEnabled != vm.memory_->GetMMU().IsEnabled() ||
            header.consoleBase != vm.consoleDevice_->GetBaseAddress() ||
            header.timerBase != vm.timerDevice_->GetBaseAddress() ||
            header.framebufferBase != vm.framebufferDevice_->GetBaseAddress() ||
            header.framebufferSize != vm.framebufferDevice_->GetSize()) {
            setError(error, "checkpoint machine configuration mismatch"); return false;
        }
        if (header.cpuCount == 0 || header.cpuCount > kMaxCpuCount || header.payloadSize > kMaxPayloadBytes) {
            setError(error, "checkpoint dimensions are invalid"); return false;
        }
        std::vector<uint8_t> payload(static_cast<size_t>(header.payloadSize));
        if (!readExact(input, payload.data(), payload.size())) { setError(error, "checkpoint payload is truncated"); return false; }
        if (input.peek() != std::char_traits<char>::eof()) {
            setError(error, "checkpoint contains trailing bytes after payload");
            return false;
        }
        if (sha256(payload) != header.payloadHash) { setError(error, "checkpoint payload hash mismatch"); return false; }

        Reader reader(payload);
        if (reader.u32() != kPayloadVersion || reader.u32() != header.cpuCount) {
            setError(error, "checkpoint payload version or CPU count mismatch"); return false;
        }
        std::vector<ParsedCPU> parsedCPUs;
        parsedCPUs.reserve(header.cpuCount);
        for (uint32_t i = 0; i < header.cpuCount; ++i) {
            ParsedCPU parsed;
            parsed.id = reader.u32(); parsed.executionState = static_cast<CPUExecutionState>(reader.u32());
            parsed.cycles = reader.u64(); parsed.instructions = reader.u64(); parsed.idle = reader.u64();
            parsed.enabled = reader.u8() != 0; parsed.lastActivation = reader.u64();
            parsed.halted = reader.u8() != 0; parsed.slot = static_cast<size_t>(reader.u64());
            parsed.bundleValid = reader.u8() != 0;
            parsed.pendingInterrupts = reader.bytes(1'000'000); parsed.interruptVectorBase = reader.u64();
            const uint64_t inputCount = reader.u64();
            if (!reader.valid() || inputCount > 1'000'000) { setError(error, "invalid CPU pending-input state"); return false; }
            parsed.pendingCallInputs.reserve(static_cast<size_t>(inputCount));
            for (uint64_t n = 0; n < inputCount; ++n) parsed.pendingCallInputs.push_back(reader.u64());
            parsed.plugin = reader.bytes(64U * 1024U * 1024U);
            if (!reader.valid() || parsed.id != i || parsed.bundleValid || parsed.slot != 0 || parsed.plugin.empty()) {
                setError(error, "invalid CPU checkpoint state"); return false;
            }
            parsedCPUs.push_back(std::move(parsed));
        }

        const uint64_t payloadRamSize = reader.u64();
        if (!reader.valid() || payloadRamSize != header.ramSize || payloadRamSize > kMaxPayloadBytes ||
            payloadRamSize > reader.remaining()) {
            std::ostringstream detail;
            detail << "invalid checkpoint RAM section count=" << payloadRamSize
                   << " expected=" << header.ramSize
                   << " remaining=" << reader.remaining()
                   << " valid=" << (reader.valid() ? 1 : 0);
            setError(error, detail.str());
            return false;
        }
        std::vector<uint8_t> ram(reader.rawBytes(static_cast<size_t>(payloadRamSize)));
        if (ram.size() != payloadRamSize) { setError(error, "truncated checkpoint RAM section"); return false; }
        const uint64_t pageCount = reader.u64();
        if (!reader.valid() || pageCount > kMaxPageEntries) { setError(error, "invalid checkpoint page table count"); return false; }
        std::map<uint64_t, PageEntry> pageTable;
        for (uint64_t i = 0; i < pageCount; ++i) {
            const uint64_t virtualAddress = reader.u64();
            PageEntry entry;
            entry.physicalAddress = reader.u64(); entry.permissions = static_cast<PermissionFlags>(reader.u8());
            entry.present = reader.u8() != 0; entry.accessed = reader.u8() != 0; entry.dirty = reader.u8() != 0;
            if (!pageTable.emplace(virtualAddress, entry).second) {
                setError(error, "checkpoint page table contains duplicate virtual address");
                return false;
            }
        }
        ConsoleDeviceState console; if (!readConsole(reader, console)) { setError(error, "invalid console state"); return false; }
        TimerDeviceState timer; if (!readTimer(reader, timer)) { setError(error, "invalid timer state"); return false; }
        InterruptControllerState interrupts; if (!readInterrupts(reader, interrupts)) { setError(error, "invalid interrupt state"); return false; }
        FramebufferDeviceState framebuffer;
        framebuffer.baseAddress = reader.u64(); framebuffer.width = static_cast<size_t>(reader.u64());
        framebuffer.height = static_cast<size_t>(reader.u64()); framebuffer.pitch = static_cast<size_t>(reader.u64());
        framebuffer.framebuffer = reader.bytes(kMaxDeviceBytes);
        if (!reader.valid() || framebuffer.framebuffer.size() != header.framebufferSize) { setError(error, "invalid framebuffer state"); return false; }
        const VMState vmState = static_cast<VMState>(reader.u32());
        const uint64_t cycles = reader.u64();
        const int activeCPU = static_cast<int32_t>(reader.u32());
        const VMBootState bootState = static_cast<VMBootState>(reader.u32());
        const VMBootState previousBootState = static_cast<VMBootState>(reader.u32());
        SimpleCPUSchedulerState schedulerState;
        schedulerState.lastCPUIndex = static_cast<int32_t>(reader.u32());
        schedulerState.numCPUs = static_cast<int32_t>(reader.u32()); schedulerState.quantumSize = reader.u64();
        const uint64_t bundleCounterCount = reader.u64();
        if (!reader.valid() || bundleCounterCount > kMaxCpuCount) { setError(error, "invalid scheduler state"); return false; }
        schedulerState.cpuBundleCounters.reserve(static_cast<size_t>(bundleCounterCount));
        for (uint64_t i = 0; i < bundleCounterCount; ++i) schedulerState.cpuBundleCounters.push_back(reader.u64());
        const FramebufferDeviceState currentFramebuffer = vm.framebufferDevice_->createSnapshot();
        const bool vmStateValid = static_cast<uint32_t>(vmState) <= static_cast<uint32_t>(VMState::RESTORING);
        const bool bootStateValid = static_cast<uint32_t>(bootState) <= static_cast<uint32_t>(VMBootState::EMERGENCY_HALT);
        const bool previousBootStateValid = static_cast<uint32_t>(previousBootState) <= static_cast<uint32_t>(VMBootState::EMERGENCY_HALT);
        const bool framebufferShapeValid = framebuffer.baseAddress == currentFramebuffer.baseAddress &&
            framebuffer.width == currentFramebuffer.width && framebuffer.height == currentFramebuffer.height &&
            framebuffer.pitch == currentFramebuffer.pitch && framebuffer.framebuffer.size() == currentFramebuffer.framebuffer.size();
        const bool schedulerShapeValid = schedulerState.lastCPUIndex >= -1 &&
            schedulerState.lastCPUIndex < schedulerState.numCPUs &&
            schedulerState.numCPUs == static_cast<int>(header.cpuCount) &&
            schedulerState.quantumSize > 0 &&
            schedulerState.cpuBundleCounters.size() == header.cpuCount;
        if (!reader.valid() || reader.remaining() != 0 || activeCPU < -1 || activeCPU >= static_cast<int>(header.cpuCount) ||
            schedulerState.numCPUs != static_cast<int>(header.cpuCount) || !vmStateValid || !bootStateValid ||
            !previousBootStateValid || !framebufferShapeValid || !schedulerShapeValid) {
            setError(error, "checkpoint payload has invalid trailing or VM state"); return false;
        }

        for (const ParsedCPU& parsed : parsedCPUs) {
            if (static_cast<uint32_t>(parsed.executionState) > static_cast<uint32_t>(CPUExecutionState::ERROR) ||
                parsed.slot != 0 || parsed.bundleValid) {
                setError(error, "checkpoint CPU execution state is invalid");
                return false;
            }
        }

        if (vm.cpus_.size() != 1) { setError(error, "current diagnostic restore supports one IA-64 CPU"); return false; }
        auto* plugin = dynamic_cast<IA64ISAPlugin*>(vm.cpus_[0].isaPlugin.get());
        if (plugin == nullptr || !plugin->deserializeCheckpointState(parsedCPUs[0].plugin)) {
            setError(error, "IA-64 plugin rejected checkpoint state"); return false;
        }
        vm.memory_->loadBuffer(0, ram.data(), ram.size());
        vm.memory_->GetMMU().RestorePageTable(pageTable);
        vm.memory_->SetMMUEnabled(header.mmuEnabled);
        vm.consoleDevice_->restoreSnapshot(console);
        vm.timerDevice_->restoreSnapshot(timer);
        vm.interruptController_->restoreSnapshot(interrupts);
        if (!vm.framebufferDevice_->restoreSnapshot(framebuffer)) { setError(error, "framebuffer restore failed"); return false; }

        CPURuntimeStateSnapshot runtime = vm.cpus_[0].cpu->createSnapshot();
        runtime.currentSlot = parsedCPUs[0].slot; runtime.bundleValid = parsedCPUs[0].bundleValid;
        runtime.pendingInterrupts = parsedCPUs[0].pendingInterrupts; runtime.interruptVectorBase = parsedCPUs[0].interruptVectorBase;
        runtime.halted = parsedCPUs[0].halted; runtime.pendingCallInputs = parsedCPUs[0].pendingCallInputs;
        vm.cpus_[0].cpu->restoreSnapshot(runtime);
        vm.cpus_[0].state = parsedCPUs[0].executionState; vm.cpus_[0].cyclesExecuted = parsedCPUs[0].cycles;
        vm.cpus_[0].instructionsExecuted = parsedCPUs[0].instructions; vm.cpus_[0].idleCycles = parsedCPUs[0].idle;
        vm.cpus_[0].enabled = parsedCPUs[0].enabled; vm.cpus_[0].lastActivationTime = parsedCPUs[0].lastActivation;
        vm.activeCPUIndex_ = activeCPU; vm.state_ = vmState; vm.cyclesExecuted_ = cycles;
        auto* scheduler = dynamic_cast<SimpleCPUScheduler*>(vm.scheduler_.get());
        if (scheduler == nullptr || !scheduler->restoreSnapshot(schedulerState)) { setError(error, "scheduler restore failed"); return false; }
        vm.bootStateMachine_.restoreCheckpointState(bootState, previousBootState);
        vm.memory_->GetMMU().SetCPUStateReference(&vm.cpus_[0].cpu->getState());
        return true;
    } catch (const std::exception& exception) {
        setError(error, std::string("checkpoint restore exception: ") + exception.what());
        return false;
    }
}

} // namespace ia64

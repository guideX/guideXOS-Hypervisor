#include "aarch64/AArch64DeviceSnapshot.h"
#include "aarch64/AArch64DeviceState.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "aarch64/AArch64MMU.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <bcrypt.h>
#endif

using namespace aarch64;

namespace {
int checks = 0;
int failures = 0;

void check(bool value, const char* expression, unsigned line) {
    ++checks;
    if (!value) { ++failures; std::cerr << "AARCH64-9 FAILED line " << line << ": " << expression << '\n'; }
}
#define CHECK(value) check((value), #value, __LINE__)

struct DeviceState {
    uint32_t control = 0;
    uint32_t status = 0;
    uint64_t counter = 0;
    uint32_t events = 0;
    uint32_t resetGeneration = 0;
    bool operator==(const DeviceState& other) const {
        return control == other.control && status == other.status && counter == other.counter &&
               events == other.events && resetGeneration == other.resetGeneration;
    }
};

void put32(uint8_t* p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=static_cast<uint8_t>(v>>(i*8)); }
void put64(uint8_t* p, uint64_t v) { for (unsigned i=0;i<8;++i) p[i]=static_cast<uint8_t>(v>>(i*8)); }
uint32_t get32(const uint8_t* p) { return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24); }
uint64_t get64(const uint8_t* p) { uint64_t v=0; for (int i=7;i>=0;--i) v=(v<<8)|p[i]; return v; }
std::string hexDigest(const std::array<uint8_t,32>& digest) {
    static constexpr char digits[]="0123456789abcdef"; std::string result; result.reserve(64);
    for (uint8_t b : digest) { result.push_back(digits[b>>4]); result.push_back(digits[b&15]); }
    return result;
}

std::array<uint8_t, 32> fixtureDigest(const std::vector<uint8_t>& bytes,
                                      std::size_t digestOffset = 88) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, resultBytes = 0, digestBytes = 0;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA-256) failed");
    const auto closeAlgorithm = [&] { if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (!BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &resultBytes, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &resultBytes, 0)) ||
        digestBytes != 32 || digestOffset > bytes.size() || bytes.size() - digestOffset < digestBytes) {
        closeAlgorithm();
        throw std::runtime_error("BCrypt SHA-256 parameters or digest range are invalid");
    }
    std::vector<uint8_t> object(objectBytes);
    if (!BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0))) {
        closeAlgorithm();
        throw std::runtime_error("BCryptCreateHash(SHA-256) failed");
    }
    const auto closeHash = [&] { if (hash != nullptr) BCryptDestroyHash(hash); closeAlgorithm(); };
    constexpr std::array<uint8_t, 32> zero{};
    const auto update = [&](const uint8_t* data, std::size_t size) {
        while (size != 0) {
            const ULONG part = static_cast<ULONG>(std::min<std::size_t>(size, 0xFFFFFFFFu));
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(data), part, 0))) return false;
            data += part;
            size -= part;
        }
        return true;
    };
    if (!update(bytes.data(), digestOffset) || !update(zero.data(), zero.size()) ||
        !update(bytes.data() + digestOffset + digestBytes, bytes.size() - digestOffset - digestBytes)) {
        closeHash();
        throw std::runtime_error("BCryptHashData(SHA-256) failed");
    }
    std::array<uint8_t, 32> result{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0))) {
        closeHash();
        throw std::runtime_error("BCryptFinishHash(SHA-256) failed");
    }
    closeHash();
    return result;
#else
    (void)bytes;
    (void)digestOffset;
    throw std::runtime_error("snapshot fixture SHA-256 helper requires Windows CNG");
#endif
}

bool snapshotDigestMatches(const std::vector<uint8_t>& bytes, std::size_t digestOffset = 88) {
    if (bytes.size() < digestOffset + 32) return false;
    const auto actual = fixtureDigest(bytes, digestOffset);
    return std::equal(actual.begin(), actual.end(), bytes.begin() + static_cast<std::ptrdiff_t>(digestOffset));
}

void resealSnapshot(std::vector<uint8_t>& bytes, bool nestedMachine = false) {
    if (nestedMachine) {
        std::size_t cursor = 128;
        bool foundMachine = false;
        for (unsigned index = 0; index < 3; ++index) {
            if (bytes.size() - cursor < 16) throw std::runtime_error("truncated section while resealing");
            const uint32_t type = get32(bytes.data() + cursor);
            const uint64_t payloadBytes = get64(bytes.data() + cursor + 8);
            if (payloadBytes > bytes.size() - cursor - 16) throw std::runtime_error("section exceeds fixture");
            if (type == 1) {
                const std::size_t machine = cursor + 16;
                const std::size_t machineBytes = static_cast<std::size_t>(payloadBytes);
                std::vector<uint8_t> nestedBytes(bytes.begin() + static_cast<std::ptrdiff_t>(machine),
                    bytes.begin() + static_cast<std::ptrdiff_t>(machine + machineBytes));
                const auto nested = fixtureDigest(nestedBytes);
                const std::size_t digestOffset = machine + 88;
                std::copy(nested.begin(), nested.end(), bytes.begin() + static_cast<std::ptrdiff_t>(digestOffset));
                foundMachine = true;
                break;
            }
            cursor += 16 + static_cast<std::size_t>(payloadBytes);
        }
        if (!foundMachine) throw std::runtime_error("machine section not found while resealing");
    }
    const auto outer = fixtureDigest(bytes);
    std::copy(outer.begin(), outer.end(), bytes.begin() + 88);
}

struct SnapshotSection {
    uint32_t type = 0;
    std::size_t header = 0;
    std::size_t payload = 0;
    std::size_t size = 0;
};

std::array<SnapshotSection, 3> snapshotSections(const std::vector<uint8_t>& bytes) {
    std::array<SnapshotSection, 3> sections{};
    std::size_t cursor = 128;
    for (auto& section : sections) {
        if (bytes.size() - cursor < 16) throw std::runtime_error("snapshot section header is truncated");
        section.type = get32(bytes.data() + cursor);
        section.header = cursor;
        section.payload = cursor + 16;
        const uint64_t size = get64(bytes.data() + cursor + 8);
        if (size > bytes.size() - section.payload) throw std::runtime_error("snapshot section exceeds envelope");
        section.size = static_cast<std::size_t>(size);
        cursor = section.payload + section.size;
    }
    return sections;
}

bool nestedMachineDigestMatches(const std::vector<uint8_t>& bytes) {
    const auto sections = snapshotSections(bytes);
    const auto machine = std::find_if(sections.begin(), sections.end(), [](const SnapshotSection& item) { return item.type == 1; });
    if (machine == sections.end() || machine->size < 120) return false;
    std::vector<uint8_t> nested(bytes.begin() + static_cast<std::ptrdiff_t>(machine->payload),
        bytes.begin() + static_cast<std::ptrdiff_t>(machine->payload + machine->size));
    return snapshotDigestMatches(nested);
}

struct DeviceRecordOffsets {
    std::size_t id = 0, schema = 0, stateLength = 0, type = 0, state = 0;
    uint8_t typeBytes = 0;
    uint32_t stateBytes = 0;
};

std::vector<DeviceRecordOffsets> deviceRecordOffsets(const std::vector<uint8_t>& bytes) {
    const auto sections = snapshotSections(bytes);
    const auto section = std::find_if(sections.begin(), sections.end(), [](const SnapshotSection& item) { return item.type == 2; });
    if (section == sections.end() || section->size < 4) throw std::runtime_error("device section is absent");
    const uint32_t count = get32(bytes.data() + section->payload);
    std::vector<DeviceRecordOffsets> records;
    std::size_t cursor = section->payload + 4;
    for (uint32_t index = 0; index < count; ++index) {
        if (bytes.size() - cursor < 17) throw std::runtime_error("device record header is truncated");
        DeviceRecordOffsets record;
        record.id = cursor;
        record.schema = cursor + 8;
        record.typeBytes = bytes[cursor + 12];
        record.stateLength = cursor + 13;
        record.stateBytes = get32(bytes.data() + record.stateLength);
        record.type = cursor + 17;
        record.state = record.type + record.typeBytes;
        if (record.state > bytes.size() || record.stateBytes > bytes.size() - record.state)
            throw std::runtime_error("device record exceeds snapshot");
        cursor = record.state + record.stateBytes;
        records.push_back(record);
    }
    return records;
}

std::vector<std::size_t> interruptRecordOffsets(const std::vector<uint8_t>& bytes) {
    const auto sections = snapshotSections(bytes);
    const auto section = std::find_if(sections.begin(), sections.end(), [](const SnapshotSection& item) { return item.type == 3; });
    if (section == sections.end() || section->size < 4) throw std::runtime_error("interrupt section is absent");
    const uint32_t count = get32(bytes.data() + section->payload);
    std::vector<std::size_t> records;
    for (uint32_t index = 0; index < count; ++index) records.push_back(section->payload + 4 + index * 20u);
    return records;
}

class TestRestoreToken final : public AArch64DeviceRestoreToken {
public:
    explicit TestRestoreToken(DeviceState value) : state(value) {}
    DeviceState state;
};

class TestStatefulDevice final : public AArch64StatefulDevice {
public:
    explicit TestStatefulDevice(uint64_t id, std::string type = "test.stateful", uint32_t schema = 1)
        : id_(id), type_(std::move(type)), schema_(schema) {}
    uint64_t instanceId() const noexcept override { return id_; }
    std::string typeId() const override { return type_; }
    uint32_t stateSchemaVersion() const noexcept override { return schema_; }
    std::size_t maxStateBytes() const noexcept override { return 32; }
    bool supportsSnapshot() const noexcept override { return true; }
    bool reset(std::string*) override { state_ = DeviceState{0, 0, 0, 0, 1}; quiesced_ = false; return true; }
    bool quiesce(std::string* error) override {
        ++quiesceCalls;
        if (onQuiesce) onQuiesce();
        if (refuseQuiesce) { if (error) *error = "test device refused quiescence"; return false; }
        quiesced_ = true; return true;
    }
    void resume() noexcept override { quiesced_ = false; }
    bool captureState(std::vector<uint8_t>& bytes, std::string*) const override {
        ++captureCalls;
        if (onCaptureState) onCaptureState();
        bytes.assign(24, 0); put32(bytes.data(), state_.control); put32(bytes.data()+4, state_.status);
        put64(bytes.data()+8, state_.counter); put32(bytes.data()+16, state_.events);
        put32(bytes.data()+20, state_.resetGeneration); return true;
    }
    bool stageRestore(const uint8_t* bytes, std::size_t size,
                      std::unique_ptr<AArch64DeviceRestoreToken>& token,
                      std::string* error) const override {
        ++stageCalls;
        if (onStageRestore) onStageRestore();
        if (rejectRestorePreparation) { if (error) *error = "injected restore preparation failure"; return false; }
        if (size != 24) { if (error) *error = "test device payload must be 24 bytes"; return false; }
        DeviceState replacement{get32(bytes), get32(bytes+4), get64(bytes+8), get32(bytes+16), get32(bytes+20)};
        if (replacement.resetGeneration == 0) { if (error) *error = "invalid reset generation"; return false; }
        token = std::make_unique<TestRestoreToken>(replacement); return true;
    }
    void commitRestore(std::unique_ptr<AArch64DeviceRestoreToken> token) noexcept override {
        auto* staged = static_cast<TestRestoreToken*>(token.get());
        state_ = staged->state;
    }
    void mutate(uint64_t amount) {
        if (quiesced_) throw std::logic_error("test mutation while quiesced");
        state_.control += static_cast<uint32_t>(amount);
        state_.status = static_cast<uint32_t>(state_.counter + amount);
        state_.counter += amount; ++state_.events;
    }
    DeviceState state() const { return state_; }
    std::vector<uint8_t> stateBytes() const {
        std::vector<uint8_t> bytes; std::string error;
        if (!captureState(bytes, &error)) throw std::runtime_error(error);
        return bytes;
    }
    bool isQuiesced() const { return quiesced_; }
    bool refuseQuiesce = false;
    mutable bool rejectRestorePreparation = false;
    mutable std::atomic<uint32_t> captureCalls{0};
    mutable std::atomic<uint32_t> stageCalls{0};
    std::atomic<uint32_t> quiesceCalls{0};
    std::function<void()> onQuiesce;
    mutable std::function<void()> onCaptureState;
    mutable std::function<void()> onStageRestore;
private:
    uint64_t id_;
    std::string type_;
    uint32_t schema_;
    DeviceState state_{};
    bool quiesced_ = false;
};

void write32(ia64::Memory& memory, uint64_t address, uint32_t value) {
    uint8_t bytes[4]; put32(bytes, value); memory.Write(address, bytes, sizeof(bytes));
}
void write64(ia64::Memory& memory, uint64_t address, uint64_t value) {
    uint8_t bytes[8]; put64(bytes, value); memory.Write(address, bytes, sizeof(bytes));
}
uint32_t movz(unsigned rd, uint16_t imm, unsigned hw = 0) { return 0xD2800000u | (static_cast<uint32_t>(hw & 3u) << 21) | (static_cast<uint32_t>(imm) << 5) | (rd & 31u); }
uint32_t movk(unsigned rd, uint16_t imm, unsigned hw) { return 0xF2800000u | (static_cast<uint32_t>(hw & 3u) << 21) | (static_cast<uint32_t>(imm) << 5) | (rd & 31u); }
uint32_t addXImm(unsigned rd, unsigned rn, uint16_t imm) {
    return 0x91000000u | (static_cast<uint32_t>(imm) << 10) | ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t strX(unsigned rt, unsigned rn, unsigned byteOffset = 0) { return 0xF9000000u | ((byteOffset / 8u) << 10) | ((rn & 31u) << 5) | (rt & 31u); }
uint32_t ldrX(unsigned rt, unsigned rn, unsigned byteOffset = 0) { return 0xF9400000u | ((byteOffset / 8u) << 10) | ((rn & 31u) << 5) | (rt & 31u); }
uint32_t msr(uint8_t crn, uint8_t crm, uint8_t op2, unsigned rt) {
    return 0xD5000000u | (3u << 19) | (static_cast<uint32_t>(crn) << 12) |
           (static_cast<uint32_t>(crm) << 8) | (static_cast<uint32_t>(op2) << 5) | (rt & 31u);
}

void appendConstant(std::vector<uint32_t>& words, unsigned rd, uint64_t value) {
    words.push_back(movz(rd, static_cast<uint16_t>(value), 0));
    for (unsigned hw = 1; hw < 4; ++hw) {
        const uint16_t part = static_cast<uint16_t>(value >> (hw * 16));
        if (part != 0) words.push_back(movk(rd, part, hw));
    }
}

uint64_t page(uint64_t pa) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u | (1ULL << 6) | (3ULL << 8) | (1ULL << 10);
}

struct GuestProgram {
    std::vector<uint32_t> words;
    std::size_t mmuEnableIndex = 0;
    std::size_t syncBoundaryIndex = 0;
    std::size_t snapshotIndex = 0;
};

GuestProgram buildGuestDrivenMmuProgram() {
    GuestProgram program;
    auto& words = program.words;
    constexpr uint64_t tcr = 25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32);
    const auto addLeaf = [&](unsigned index, uint64_t pa) {
        appendConstant(words, 10, page(pa));
        words.push_back(strX(10, 22, index * 8u));
    };
    appendConstant(words, 20, 0x10000); // guest-created root
    appendConstant(words, 21, 0x11000); // guest-created L2
    appendConstant(words, 22, 0x12000); // guest-created L3
    appendConstant(words, 0, 0x11003);
    words.push_back(strX(0, 20));
    appendConstant(words, 0, 0x12003);
    words.push_back(strX(0, 21));
    addLeaf(1, 0x1000); // instruction VA 0x1000 -> PA 0x1000
    addLeaf(2, 0x2000); // later instruction VA range
    addLeaf(4, 0x8000); // data VA 0x4000 -> PA 0x8000
    appendConstant(words, 2, tcr);
    words.push_back(msr(2, 0, 2, 2)); // TCR_EL1
    appendConstant(words, 3, 0x10000);
    words.push_back(msr(2, 0, 0, 3)); // TTBR0_EL1
    appendConstant(words, 4, 0xFF);
    words.push_back(msr(10, 2, 0, 4)); // MAIR_EL1
    appendConstant(words, 5, 1);
    program.mmuEnableIndex = words.size();
    words.push_back(msr(1, 0, 0, 5)); // SCTLR_EL1.M
    words.push_back(0xD5033A9Fu); // DSB ISHST
    words.push_back(0xD5033FDFu); // ISB commits the translation controls
    program.syncBoundaryIndex = words.size();
    appendConstant(words, 11, 0x4000);
    words.push_back(ldrX(12, 11));
    words.push_back(addXImm(13, 12, 1));
    words.push_back(strX(13, 11));
    program.snapshotIndex = words.size();
    words.push_back(addXImm(13, 13, 1));
    words.push_back(strX(13, 11));
    words.push_back(0xD503201Fu); // NOP at the documented final boundary
    return program;
}
void prepareTranslatedGuest(AArch64ISAPlugin& cpu, ia64::Memory& ram) {
    // Guest VA 0x1000 maps to PA 0x1000; VA 0x4000 maps to PA 0x8000.
    write64(ram, 0x10000, 0x11000 | 3u);
    write64(ram, 0x11000, 0x12000 | 3u);
    write64(ram, 0x12000 + 1 * 8, page(0x1000));
    write64(ram, 0x12000 + 4 * 8, page(0x8000));
    const std::array<uint32_t, 5> code{{movz(0, 5), movz(1, 0x4000), strX(0, 1), addXImm(0, 0, 1), strX(0, 1)}};
    for (std::size_t i=0; i<code.size(); ++i) write32(ram, 0x1000 + i*4, code[i]);
    cpu.cpu().setTtbr0El1(0x10000);
    cpu.cpu().setTcrEl1(25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32));
    cpu.cpu().setMairEl1(0xFF);
    cpu.cpu().setSctlrEl1(1);
}

bool step(AArch64ISAPlugin& cpu, ia64::Memory& ram) {
    const Fault result = cpu.cpu().step(ram);
    if (result != Fault::None) std::cerr << "guest step fault pc=0x" << std::hex << cpu.getPC()
        << " fault=" << static_cast<unsigned>(result) << " error=" << cpu.cpu().lastError() << std::dec << '\n';
    return result == Fault::None;
}

bool runGuestSteps(AArch64ISAPlugin& cpu, ia64::Memory& ram, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) if (!step(cpu, ram)) return false;
    return true;
}

std::vector<uint8_t> inventory(AArch64ISAPlugin& cpu, ia64::Memory& ram,
                               TestStatefulDevice& a, TestStatefulDevice& b,
                               AArch64DeviceRegistry& sources) {
    std::vector<uint8_t> out = cpu.serialize_state();
    out.insert(out.end(), ram.GetRawData(), ram.GetRawData()+ram.GetTotalSize());
    for (const TestStatefulDevice* device : {&a, &b}) {
        const std::string type = device->typeId();
        const auto identityAt = out.size(); out.resize(identityAt + 13 + type.size());
        put64(out.data()+identityAt,device->instanceId()); put32(out.data()+identityAt+8,device->stateSchemaVersion());
        out[identityAt+12]=static_cast<uint8_t>(type.size()); std::copy(type.begin(),type.end(),out.begin()+static_cast<std::ptrdiff_t>(identityAt+13));
        const auto state = device->stateBytes(); out.insert(out.end(),state.begin(),state.end());
    }
    const auto countsAt = out.size(); out.resize(countsAt + 8); put32(out.data()+countsAt,static_cast<uint32_t>(sources.deviceCount())); put32(out.data()+countsAt+4,static_cast<uint32_t>(sources.interruptSourceCount()));
    for (uint64_t id : {uint64_t{101}, uint64_t{102}}) {
        AArch64InterruptSourceState state; std::string error;
        if (!sources.queryInterruptSource(id, state, &error)) throw std::runtime_error(error);
        const auto at=out.size(); out.resize(at+20); put64(out.data()+at,state.id); put64(out.data()+at+8,state.ownerDeviceId);
        out[at+16]=static_cast<uint8_t>(state.mode); out[at+17]=state.asserted?1:0; out[at+18]=state.pending?1:0; out[at+19]=0;
    }
    return out;
}

struct Machine {
    AArch64ISAPlugin cpu;
    ia64::Memory ram{128 * 1024, false};
    AArch64DeviceRegistry devices;
    TestStatefulDevice* d1 = nullptr;
    TestStatefulDevice* d2 = nullptr;
    explicit Machine(uint64_t entryPoint = 0x1000, ExceptionLevel level = ExceptionLevel::EL0)
        : cpu(entryPoint, level) {
        auto first=std::make_unique<TestStatefulDevice>(11); d1=first.get();
        auto second=std::make_unique<TestStatefulDevice>(12); d2=second.get();
        std::string error;
        if (!devices.registerDevice(std::move(first), &error) || !devices.registerDevice(std::move(second), &error) ||
            !devices.registerInterruptSource(101,11,AArch64InterruptMode::Level,&error) ||
            !devices.registerInterruptSource(102,12,AArch64InterruptMode::Edge,&error)) throw std::runtime_error(error);
    }
};

struct DeviceSpec { uint64_t id; std::string type = "test.stateful"; uint32_t schema = 1; };
struct SourceSpec { uint64_t id; uint64_t owner; AArch64InterruptMode mode; };

struct TopologyMachine {
    AArch64ISAPlugin cpu{0x180, ExceptionLevel::EL0};
    ia64::Memory ram{128 * 1024, false};
    AArch64DeviceRegistry devices;
    std::vector<TestStatefulDevice*> devicePointers;
    std::vector<uint64_t> sourceIds;

    TopologyMachine(const std::vector<DeviceSpec>& deviceSpecs,
                    const std::vector<SourceSpec>& sourceSpecs,
                    std::size_t ramBytes = 128 * 1024) : ram(ramBytes, false) {
        std::string error;
        for (const auto& spec : deviceSpecs) {
            auto device = std::make_unique<TestStatefulDevice>(spec.id, spec.type, spec.schema);
            TestStatefulDevice* pointer = device.get();
            if (!devices.registerDevice(std::move(device), &error)) throw std::runtime_error(error);
            devicePointers.push_back(pointer);
        }
        for (const auto& spec : sourceSpecs) {
            if (!devices.registerInterruptSource(spec.id, spec.owner, spec.mode, &error)) throw std::runtime_error(error);
            sourceIds.push_back(spec.id);
        }
        for (std::size_t i = 0; i < devicePointers.size(); ++i) devicePointers[i]->mutate(31 + i);
        for (uint64_t id : sourceIds) {
            AArch64InterruptSourceState state;
            if (!devices.queryInterruptSource(id, state, &error)) throw std::runtime_error(error);
            if (state.mode == AArch64InterruptMode::Level) {
                if (!devices.setLevel(id, true, &error)) throw std::runtime_error(error);
            } else if (!devices.signalEdge(id, &error)) throw std::runtime_error(error);
        }
        cpu.cpu().writeX(0, 0xD00DFEED);
        ram.write<uint64_t>(0x300, 0xAABBCCDDEEFF0011ULL);
    }

    std::vector<uint8_t> fullInventory() {
        std::vector<uint8_t> out = cpu.serialize_state();
        out.insert(out.end(), ram.GetRawData(), ram.GetRawData() + ram.GetTotalSize());
        const std::size_t countsAt = out.size();
        out.resize(countsAt + 8);
        put32(out.data() + countsAt, static_cast<uint32_t>(devices.deviceCount()));
        put32(out.data() + countsAt + 4, static_cast<uint32_t>(devices.interruptSourceCount()));
        for (const auto* device : devicePointers) {
            const std::size_t identityAt = out.size();
            const std::string type = device->typeId();
            out.resize(identityAt + 13 + type.size());
            put64(out.data() + identityAt, device->instanceId());
            put32(out.data() + identityAt + 8, device->stateSchemaVersion());
            out[identityAt + 12] = static_cast<uint8_t>(type.size());
            std::copy(type.begin(), type.end(), out.begin() + static_cast<std::ptrdiff_t>(identityAt + 13));
            const auto state = device->stateBytes();
            out.insert(out.end(), state.begin(), state.end());
        }
        for (uint64_t id : sourceIds) {
            AArch64InterruptSourceState state;
            std::string error;
            if (!devices.queryInterruptSource(id, state, &error)) throw std::runtime_error(error);
            const std::size_t at = out.size();
            out.resize(at + 20);
            put64(out.data() + at, state.id);
            put64(out.data() + at + 8, state.ownerDeviceId);
            out[at + 16] = static_cast<uint8_t>(state.mode);
            out[at + 17] = state.asserted ? 1 : 0;
            out[at + 18] = state.pending ? 1 : 0;
            out[at + 19] = 0;
        }
        return out;
    }

    TestStatefulDevice* device(uint64_t id) const {
        for (auto* item : devicePointers) if (item->instanceId() == id) return item;
        return nullptr;
    }

    bool removeDevice(uint64_t id, std::string* error = nullptr) {
        if (!devices.unregisterDevice(id, error)) return false;
        devicePointers.erase(std::remove_if(devicePointers.begin(), devicePointers.end(),
            [&](TestStatefulDevice* item) { return item->instanceId() == id; }), devicePointers.end());
        sourceIds.erase(std::remove_if(sourceIds.begin(), sourceIds.end(), [&](uint64_t sourceId) {
            AArch64InterruptSourceState state;
            std::string ignored;
            if (!devices.queryInterruptSource(sourceId, state, &ignored)) return true;
            return state.ownerDeviceId == id;
        }), sourceIds.end());
        return true;
    }
};

void testDeviceReplayAndAtomicRestore() {
    Machine source;
    prepareTranslatedGuest(source.cpu, source.ram);
    CHECK(step(source.cpu, source.ram)); CHECK(step(source.cpu, source.ram)); CHECK(step(source.cpu, source.ram));
    CHECK(source.cpu.getPC() == 0x100C);
    uint8_t data[8]{}; source.ram.Read(0x8000, data, sizeof(data)); CHECK(get64(data) == 5);
    source.d1->mutate(3); source.d2->mutate(8);
    std::string error;
    CHECK(source.devices.setLevel(101, true, &error));
    CHECK(source.devices.signalEdge(102, &error));
    CHECK(!source.devices.signalEdge(102, &error)); // one pending edge is the bounded capacity.

    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices, snapshot, &error));
    std::vector<uint8_t> repeated;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices, repeated, &error));
    CHECK(snapshot == repeated);
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    CHECK(info.envelopeVersion == 2 && info.deviceCount == 2 && info.interruptSourceCount == 2);
    CHECK(snapshotDigestMatches(snapshot));
    // The original partial AARCH64-9 build emitted this exact v2 payload with
    // its nonstandard SHA-256 round constant. Keep a fixture for that durable
    // artifact format while requiring all new captures to use standard SHA.
    auto historicalSnapshot = snapshot;
    constexpr std::array<uint8_t, 32> historicalDigest{{
        0x41,0xf4,0x84,0xee,0x8d,0xcb,0xff,0xa4,0xab,0xe2,0x7f,0x2a,0x17,0x5b,0xa0,0x4e,
        0x00,0xe7,0xf2,0x72,0x56,0xf2,0xe4,0xb9,0x06,0xa5,0x78,0xe9,0xa4,0xcb,0x4a,0x53}};
    std::copy(historicalDigest.begin(), historicalDigest.end(), historicalSnapshot.begin() + 88);
    AArch64DeviceSnapshotInfo historicalInfo;
    CHECK(AArch64DeviceSnapshot::validate(historicalSnapshot, &historicalInfo, &error));
    CHECK(historicalInfo.sha256 == historicalDigest);
    Machine historicalRestore;
    CHECK(AArch64DeviceSnapshot::restore(historicalRestore.cpu, historicalRestore.ram,
        historicalRestore.devices, historicalSnapshot, &error));
    CHECK(inventory(historicalRestore.cpu, historicalRestore.ram, *historicalRestore.d1,
        *historicalRestore.d2, historicalRestore.devices) ==
        inventory(source.cpu, source.ram, *source.d1, *source.d2, source.devices));
    const auto savedCpu = source.cpu.serialize_state();
    const std::vector<uint8_t> savedRam(source.ram.GetRawData(), source.ram.GetRawData()+source.ram.GetTotalSize());
    const auto savedD1=source.d1->stateBytes(), savedD2=source.d2->stateBytes();
    CHECK(source.d1->isQuiesced() == false && source.d2->isQuiesced() == false);

    // Path A runs uninterrupted after the snapshot.
    CHECK(step(source.cpu, source.ram)); CHECK(step(source.cpu, source.ram));
    source.d1->mutate(5); source.d2->mutate(2);
    CHECK(source.devices.setLevel(101, false, &error));
    bool consumed=false; CHECK(source.devices.consumeEdge(102,consumed,&error) && consumed);
    const auto finalCpu=source.cpu.serialize_state();
    const std::vector<uint8_t> finalRam(source.ram.GetRawData(), source.ram.GetRawData()+source.ram.GetTotalSize());
    const auto finalD1=source.d1->stateBytes(), finalD2=source.d2->stateBytes();

    // Path B is an independently constructed CPU/RAM/device/source topology.
    Machine restored;
    std::vector<uint8_t> poison(restored.ram.GetTotalSize(), 0xCC);
    restored.ram.loadBuffer(0, poison);
    restored.cpu.setPC(0x80);
    restored.d1->mutate(90); restored.d2->mutate(91);
    const auto beforeFailure=inventory(restored.cpu,restored.ram,*restored.d1,*restored.d2,restored.devices);
    const uint32_t firstStageBefore=restored.d1->stageCalls.load();
    const uint32_t secondStageBefore=restored.d2->stageCalls.load();
    restored.d2->rejectRestorePreparation=true;
    CHECK(!AArch64DeviceSnapshot::restore(restored.cpu,restored.ram,restored.devices,snapshot,&error));
    restored.d2->rejectRestorePreparation=false;
    CHECK(restored.d1->stageCalls.load() == firstStageBefore + 1);
    CHECK(restored.d2->stageCalls.load() == secondStageBefore + 1);
    CHECK(inventory(restored.cpu,restored.ram,*restored.d1,*restored.d2,restored.devices)==beforeFailure);

    auto badDigest=snapshot; badDigest.back()^=1;
    CHECK(!AArch64DeviceSnapshot::restore(restored.cpu,restored.ram,restored.devices,badDigest,&error));
    CHECK(inventory(restored.cpu,restored.ram,*restored.d1,*restored.d2,restored.devices)==beforeFailure);
    CHECK(AArch64DeviceSnapshot::restore(restored.cpu,restored.ram,restored.devices,snapshot,&error));
    CHECK(restored.cpu.serialize_state()==savedCpu);
    CHECK(std::equal(savedRam.begin(),savedRam.end(),restored.ram.GetRawData()));
    CHECK(restored.d1->stateBytes()==savedD1 && restored.d2->stateBytes()==savedD2);
    AArch64InterruptSourceState level,edge;
    CHECK(restored.devices.queryInterruptSource(101,level,&error) && level.asserted);
    CHECK(restored.devices.queryInterruptSource(102,edge,&error) && edge.pending);
    CHECK(restored.cpu.getPC()==0x100C);

    CHECK(step(restored.cpu,restored.ram)); CHECK(step(restored.cpu,restored.ram));
    restored.d1->mutate(5); restored.d2->mutate(2);
    CHECK(restored.devices.setLevel(101,false,&error));
    consumed=false; CHECK(restored.devices.consumeEdge(102,consumed,&error) && consumed);
    bool secondConsume=false; CHECK(restored.devices.consumeEdge(102,secondConsume,&error) && !secondConsume);
    CHECK(restored.cpu.serialize_state()==finalCpu);
    CHECK(std::equal(finalRam.begin(),finalRam.end(),restored.ram.GetRawData()));
    CHECK(restored.d1->stateBytes()==finalD1 && restored.d2->stateBytes()==finalD2);
    CHECK(restored.cpu.getPC()==0x1014);

    // Restore rejection against a wrong identity preserves its full live inventory.
    AArch64DeviceRegistry wrongTopology;
    auto wrong=std::make_unique<TestStatefulDevice>(13); TestStatefulDevice* wrongPtr=wrong.get();
    auto peer=std::make_unique<TestStatefulDevice>(12); TestStatefulDevice* peerPtr=peer.get();
    CHECK(wrongTopology.registerDevice(std::move(wrong),&error)); CHECK(wrongTopology.registerDevice(std::move(peer),&error));
    CHECK(wrongTopology.registerInterruptSource(101,13,AArch64InterruptMode::Level,&error));
    CHECK(wrongTopology.registerInterruptSource(102,12,AArch64InterruptMode::Edge,&error));
    const auto wrongCpu=restored.cpu.serialize_state();
    const std::vector<uint8_t> wrongRam(restored.ram.GetRawData(),restored.ram.GetRawData()+restored.ram.GetTotalSize());
    const auto wrongStateA=wrongPtr->stateBytes(),wrongStateB=peerPtr->stateBytes();
    CHECK(!AArch64DeviceSnapshot::restore(restored.cpu,restored.ram,wrongTopology,snapshot,&error));
    CHECK(restored.cpu.serialize_state()==wrongCpu);
    CHECK(std::equal(wrongRam.begin(),wrongRam.end(),restored.ram.GetRawData()));
    CHECK(wrongPtr->stateBytes()==wrongStateA && peerPtr->stateBytes()==wrongStateB);

    std::cout << "AARCH64-9 scenario A/D: translated A64 device snapshot restore and independent replay passed; savedPC=0x100c restoredPC=0x100c finalPC=0x1014 bytes=" << snapshot.size() << " sha256=" << hexDigest(info.sha256) << '\n';
}

void testGuestDrivenMmuDeviceReplay() {
    GuestProgram program = buildGuestDrivenMmuProgram();
    Machine source(0x1000, ExceptionLevel::EL1);
    for (std::size_t index = 0; index < program.words.size(); ++index)
        write32(source.ram, 0x1000 + index * 4, program.words[index]);
    source.ram.write<uint64_t>(0x4000, 0xBAD0BAD0BAD0BAD0ULL);
    source.ram.write<uint64_t>(0x8000, 10);
    const uint64_t initialPc = source.cpu.getPC();
    const uint64_t mmuEnablePc = initialPc + program.mmuEnableIndex * 4;
    const uint64_t syncBoundaryPc = initialPc + program.syncBoundaryIndex * 4;
    const uint64_t snapshotPc = initialPc + program.snapshotIndex * 4;
    const uint64_t finalPc = initialPc + program.words.size() * 4;
    CHECK(source.cpu.cpu().sctlrEl1() == 0 && source.cpu.cpu().effectiveSctlrEl1() == 0);

    CHECK(runGuestSteps(source.cpu, source.ram, program.mmuEnableIndex));
    CHECK(source.cpu.getPC() == mmuEnablePc);
    CHECK(step(source.cpu, source.ram)); // guest MSR SCTLR_EL1.M
    CHECK(source.cpu.getPC() == mmuEnablePc + 4);
    CHECK(step(source.cpu, source.ram)); // DSB ISHST
    CHECK(step(source.cpu, source.ram)); // ISB
    CHECK(source.cpu.getPC() == syncBoundaryPc);
    CHECK((source.cpu.cpu().sctlrEl1() & 1u) != 0);
    CHECK((source.cpu.cpu().effectiveSctlrEl1() & 1u) != 0);
    CHECK(source.cpu.cpu().ttbr0El1() == 0x10000);
    CHECK(source.cpu.cpu().tcrEl1() == (25u | (25ULL << 16) | (2ULL << 30) | (5ULL << 32)));
    CHECK(source.cpu.cpu().mairEl1() == 0xFF);
    CHECK(runGuestSteps(source.cpu, source.ram, program.snapshotIndex - program.syncBoundaryIndex));
    CHECK(source.cpu.getPC() == snapshotPc);
    CHECK(source.cpu.cpu().readX(12) == 10);
    CHECK(source.ram.read<uint64_t>(0x8000) == 11);
    CHECK(source.ram.read<uint64_t>(0x4000) == 0xBAD0BAD0BAD0BAD0ULL);

    const auto translation = AArch64MMU::translate(source.ram, 0x4000, 1,
        MMUAccess::Read, source.cpu.cpu().effectiveTcrEl1(),
        source.cpu.cpu().effectiveTtbr0El1(), source.cpu.cpu().effectiveTtbr1El1(),
        source.cpu.cpu().effectiveMairEl1(), false);
    CHECK(translation.success && translation.physicalAddress == 0x8000);

    source.d1->mutate(3);
    source.d2->mutate(8);
    std::string error;
    CHECK(source.devices.setLevel(101, true, &error));
    CHECK(source.devices.signalEdge(102, &error));
    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices, snapshot, &error));
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    const auto savedCpu = source.cpu.serialize_state();
    const std::vector<uint8_t> savedRam(source.ram.GetRawData(), source.ram.GetRawData() + source.ram.GetTotalSize());
    const auto savedDevice1 = source.d1->stateBytes(), savedDevice2 = source.d2->stateBytes();

    CHECK(runGuestSteps(source.cpu, source.ram, program.words.size() - program.snapshotIndex));
    CHECK(source.cpu.getPC() == finalPc);
    CHECK(source.ram.read<uint64_t>(0x8000) == 12);
    const auto uninterruptedCpu = source.cpu.serialize_state();
    const std::vector<uint8_t> uninterruptedRam(source.ram.GetRawData(), source.ram.GetRawData() + source.ram.GetTotalSize());
    const auto uninterruptedDevice1 = source.d1->stateBytes(), uninterruptedDevice2 = source.d2->stateBytes();
    AArch64InterruptSourceState uninterruptedLevel, uninterruptedEdge;
    CHECK(source.devices.queryInterruptSource(101, uninterruptedLevel, &error));
    CHECK(source.devices.queryInterruptSource(102, uninterruptedEdge, &error));

    Machine restored(0x1000, ExceptionLevel::EL1);
    std::vector<uint8_t> poison(restored.ram.GetTotalSize(), 0xCC);
    restored.ram.loadBuffer(0, poison);
    for (std::size_t index = 0; index < program.words.size(); ++index)
        write32(restored.ram, 0x1000 + index * 4, program.words[index]);
    restored.cpu.cpu().writeX(0, 0x1234);
    restored.d1->mutate(50);
    restored.d2->mutate(51);
    CHECK(AArch64DeviceSnapshot::restore(restored.cpu, restored.ram, restored.devices, snapshot, &error));
    CHECK(restored.cpu.serialize_state() == savedCpu);
    CHECK(std::equal(savedRam.begin(), savedRam.end(), restored.ram.GetRawData()));
    CHECK(restored.d1->stateBytes() == savedDevice1 && restored.d2->stateBytes() == savedDevice2);
    CHECK(restored.cpu.getPC() == snapshotPc);
    CHECK(restored.cpu.cpu().effectiveSctlrEl1() == source.cpu.cpu().effectiveSctlrEl1());
    CHECK(restored.cpu.cpu().effectiveTcrEl1() == source.cpu.cpu().effectiveTcrEl1());
    CHECK(restored.cpu.cpu().effectiveTtbr0El1() == source.cpu.cpu().effectiveTtbr0El1());
    CHECK(restored.cpu.cpu().effectiveMairEl1() == source.cpu.cpu().effectiveMairEl1());
    CHECK(restored.cpu.cpu().tlb().validEntryCount() == 0);
    AArch64InterruptSourceState restoredLevel, restoredEdge;
    CHECK(restored.devices.queryInterruptSource(101, restoredLevel, &error) && restoredLevel.asserted);
    CHECK(restored.devices.queryInterruptSource(102, restoredEdge, &error) && restoredEdge.pending);
    const auto restoredTranslation = AArch64MMU::translate(restored.ram, 0x4000, 1,
        MMUAccess::Read, restored.cpu.cpu().effectiveTcrEl1(),
        restored.cpu.cpu().effectiveTtbr0El1(), restored.cpu.cpu().effectiveTtbr1El1(),
        restored.cpu.cpu().effectiveMairEl1(), false);
    CHECK(restoredTranslation.success && restoredTranslation.physicalAddress == 0x8000);
    CHECK(runGuestSteps(restored.cpu, restored.ram, program.words.size() - program.snapshotIndex));
    CHECK(restored.cpu.getPC() == finalPc);
    CHECK(restored.cpu.serialize_state() == uninterruptedCpu);
    CHECK(std::equal(uninterruptedRam.begin(), uninterruptedRam.end(), restored.ram.GetRawData()));
    CHECK(restored.d1->stateBytes() == uninterruptedDevice1 && restored.d2->stateBytes() == uninterruptedDevice2);
    CHECK(restored.devices.queryInterruptSource(101, restoredLevel, &error));
    CHECK(restored.devices.queryInterruptSource(102, restoredEdge, &error));
    CHECK(restoredLevel.asserted == uninterruptedLevel.asserted && restoredEdge.pending == uninterruptedEdge.pending);
    CHECK(snapshotDigestMatches(snapshot));

    std::cout << "AARCH64-9R guest-driven MMU: initialPC=0x" << std::hex << initialPc
              << " mmuEnableInstructionPC=0x" << mmuEnablePc
              << " syncBoundaryPC=0x" << syncBoundaryPc
              << " snapshotPC=0x" << snapshotPc << " restoredPC=0x" << snapshotPc
              << " finalPC=0x" << finalPc << std::dec
              << " preEnableInstructions=" << program.mmuEnableIndex
              << " throughSyncInstructions=" << program.syncBoundaryIndex
              << " throughSnapshotInstructions=" << program.snapshotIndex
              << " replayInstructions=" << (program.words.size() - program.snapshotIndex)
              << " VA=0x4000 PA=0x8000 bytes=" << snapshot.size()
              << " sha256=" << hexDigest(info.sha256) << '\n';
}

void testHashCorrectMalformedSnapshots() {
    Machine source;
    std::string error;
    std::vector<uint8_t> good;
    CHECK(AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices, good, &error));
    CHECK(snapshotDigestMatches(good));
    CHECK(nestedMachineDigestMatches(good));
    const auto sections = snapshotSections(good);
    const auto machineSection = std::find_if(sections.begin(), sections.end(), [](const SnapshotSection& item) { return item.type == 1; });
    const auto deviceSection = std::find_if(sections.begin(), sections.end(), [](const SnapshotSection& item) { return item.type == 2; });
    const auto sourceSection = std::find_if(sections.begin(), sections.end(), [](const SnapshotSection& item) { return item.type == 3; });
    if (machineSection == sections.end() || deviceSection == sections.end() || sourceSection == sections.end())
        throw std::runtime_error("known-good v2 fixture is missing a mandatory section");
    const auto deviceOffsets = deviceRecordOffsets(good);
    const auto sourceOffsets = interruptRecordOffsets(good);
    if (deviceOffsets.size() != 2 || sourceOffsets.size() != 2)
        throw std::runtime_error("known-good v2 fixture has unexpected topology");

    Machine destination;
    destination.cpu.setPC(0x80);
    destination.cpu.cpu().writeX(0, 0xFACEB00C);
    destination.ram.write<uint64_t>(0x220, 0x1020304050607080ULL);
    destination.d1->mutate(70);
    destination.d2->mutate(71);
    CHECK(destination.devices.setLevel(101, true, &error));
    CHECK(destination.devices.signalEdge(102, &error));

    struct Case {
        const char* name;
        const char* expected;
        bool nestedDigest;
        bool reachesDeviceStage;
        std::function<void(std::vector<uint8_t>&)> mutate;
    };
    const auto outerSection = [&](const std::vector<uint8_t>& bytes, uint32_t type) {
        const auto current = snapshotSections(bytes);
        const auto found = std::find_if(current.begin(), current.end(), [&](const SnapshotSection& item) { return item.type == type; });
        if (found == current.end()) throw std::runtime_error("requested outer section is absent");
        return *found;
    };
    std::vector<Case> cases;
    cases.push_back({"unsupported envelope version", "unsupported device snapshot envelope version", false, false,
        [](auto& b) { put32(b.data() + 8, 99); }});
    cases.push_back({"wrong architecture", "architecture is not AArch64", false, false,
        [](auto& b) { put32(b.data() + 16, 0x12345678); }});
    cases.push_back({"unsupported execution profile", "execution profile", false, false,
        [](auto& b) { put32(b.data() + 20, 99); }});
    cases.push_back({"invalid section count", "section count or total length", false, false,
        [](auto& b) { put32(b.data() + 28, 4); }});
    cases.push_back({"duplicate mandatory section", "unknown or duplicate", false, false,
        [=](auto& b) { put32(b.data() + sections[1].header, 1); }});
    cases.push_back({"missing CPU machine section", "unknown or duplicate", false, false,
        [=](auto& b) { put32(b.data() + sections[0].header, 0); }});
    cases.push_back({"missing nested RAM region", "section boundaries or lengths", true, false,
        [=](auto& b) { put32(b.data() + machineSection->payload + 56, 0); }});
    cases.push_back({"missing device section", "unknown or duplicate", false, false,
        [=](auto& b) { put32(b.data() + sections[1].header, 0xFFFFFFFFu); }});
    cases.push_back({"missing interrupt-source section", "unknown or duplicate", false, false,
        [=](auto& b) { put32(b.data() + sections[2].header, 0xFFFFFFFFu); }});
    cases.push_back({"nested CPU offset outside payload", "section boundaries or lengths", true, false,
        [=](auto& b) { put64(b.data() + machineSection->payload + 32, std::numeric_limits<uint64_t>::max()); }});
    cases.push_back({"overlapping nested sections", "section boundaries or lengths", true, false,
        [=](auto& b) { put64(b.data() + machineSection->payload + 64, 128); }});
    cases.push_back({"outer section length beyond envelope", "section length or flags", false, false,
        [=](auto& b) { put64(b.data() + sections[0].header + 8, sections[0].size + 1); }});
    cases.push_back({"outer section offset-plus-length overflow", "section length or flags", false, false,
        [=](auto& b) { put64(b.data() + sections[0].header + 8,
            std::numeric_limits<uint64_t>::max() - sections[0].payload + 1); }});
    cases.push_back({"unsupported section flags", "section length or flags", false, false,
        [=](auto& b) { put32(b.data() + sections[0].header + 4, 1); }});
    cases.push_back({"nonzero reserved header field", "reserved header", false, false,
        [](auto& b) { b[48] = 1; }});
    cases.push_back({"invalid device count", "device count exceeds", false, false,
        [=](auto& b) { put32(b.data() + deviceSection->payload, static_cast<uint32_t>(kMaxRegisteredDevices + 1)); }});
    cases.push_back({"device payload exceeds supported maximum", "payload bound is invalid", false, false,
        [=](auto& b) { put32(b.data() + deviceOffsets[0].stateLength, static_cast<uint32_t>(kMaxDeviceStateBytes + 1)); }});
    cases.push_back({"device payload shorter than schema", "test device payload must be 24 bytes", false, true,
        [=](auto& b) {
            put32(b.data() + deviceOffsets[0].stateLength, deviceOffsets[0].stateBytes - 1);
            const auto dev = outerSection(b, 2);
            b.erase(b.begin() + static_cast<std::ptrdiff_t>(deviceOffsets[0].state + deviceOffsets[0].stateBytes - 1));
            put64(b.data() + dev.header + 8, dev.size - 1);
            put64(b.data() + 32, b.size());
        }});
    cases.push_back({"invalid device-state field encoding", "invalid reset generation", false, true,
        [=](auto& b) { put32(b.data() + deviceOffsets[0].state + 20, 0); }});
    cases.push_back({"invalid asserted-state encoding", "identity, ownership encoding", false, false,
        [=](auto& b) { b[sourceOffsets[0] + 17] = 2; }});
    cases.push_back({"invalid pending-event encoding", "identity, ownership encoding", false, false,
        [=](auto& b) { b[sourceOffsets[1] + 18] = 2; }});
    cases.push_back({"invalid interrupt-source count", "count or section length is invalid", false, false,
        [=](auto& b) { put32(b.data() + sourceSection->payload, static_cast<uint32_t>(kMaxInterruptSources + 1)); }});
    cases.push_back({"unsupported device schema version", "device identity, type, schema", false, false,
        [=](auto& b) { put32(b.data() + deviceOffsets[0].schema, 0); }});
    cases.push_back({"unsupported interrupt-source type", "identity, ownership encoding", false, false,
        [=](auto& b) { b[sourceOffsets[0] + 16] = 3; }});
    cases.push_back({"duplicate stable device IDs", "identity, type, schema, ordering", false, false,
        [=](auto& b) { put64(b.data() + deviceOffsets[1].id, get64(b.data() + deviceOffsets[0].id)); }});
    cases.push_back({"duplicate interrupt-source IDs", "identity, ownership encoding", false, false,
        [=](auto& b) { put64(b.data() + sourceOffsets[1], get64(b.data() + sourceOffsets[0])); }});
    cases.push_back({"unknown required section", "unknown or duplicate", false, false,
        [=](auto& b) { put32(b.data() + sections[2].header, 99); }});
    cases.push_back({"declared total size inconsistent", "section count or total length", false, false,
        [](auto& b) { put64(b.data() + 32, b.size() - 1); }});
    cases.push_back({"unexpected trailing byte", "trailing data or missing mandatory", false, false,
        [](auto& b) { b.push_back(0xA5); put64(b.data() + 32, b.size()); }});
    cases.push_back({"unsupported CPU checkpoint version", "CPU checkpoint version is unsupported", true, false,
        [=](auto& b) { put32(b.data() + machineSection->payload + 28, 99); }});
    cases.push_back({"unsupported CPU checkpoint length", "section boundaries or lengths", true, false,
        [=](auto& b) { put64(b.data() + machineSection->payload + 40, 399); }});
    cases.push_back({"valid digest with unknown interrupt owner", "owner absent from the snapshot topology", false, false,
        [=](auto& b) { put64(b.data() + sourceOffsets[0] + 8, 999); }});
    cases.push_back({"invalid device identity encoding", "identity, type, schema, ordering", false, false,
        [=](auto& b) { put64(b.data() + deviceOffsets[0].id, 0); }});
    cases.push_back({"invalid device type encoding", "identity, type, schema, ordering", false, false,
        [=](auto& b) { b[deviceOffsets[0].type] = 'T'; }});
    cases.push_back({"nonzero interrupt-source reserved field", "identity, ownership encoding", false, false,
        [=](auto& b) { b[sourceOffsets[0] + 19] = 1; }});

    for (const auto& item : cases) {
        std::vector<uint8_t> malformed = good;
        item.mutate(malformed);
        resealSnapshot(malformed, item.nestedDigest);
        const bool digestOkay = snapshotDigestMatches(malformed);
        CHECK(digestOkay);
        if (item.nestedDigest) CHECK(nestedMachineDigestMatches(malformed));
        const auto before = inventory(destination.cpu, destination.ram, *destination.d1, *destination.d2, destination.devices);
        const uint32_t stagesBefore = destination.d1->stageCalls.load() + destination.d2->stageCalls.load();
        const bool restored = AArch64DeviceSnapshot::restore(destination.cpu, destination.ram,
            destination.devices, malformed, &error);
        const uint32_t stagesAfter = destination.d1->stageCalls.load() + destination.d2->stageCalls.load();
        const bool stageReached = stagesAfter != stagesBefore;
        CHECK(!restored);
        CHECK(error.find(item.expected) != std::string::npos);
        CHECK(stageReached == item.reachesDeviceStage);
        CHECK(inventory(destination.cpu, destination.ram, *destination.d1, *destination.d2, destination.devices) == before);
        std::cout << "malformed[" << item.name << "] hash=valid stage=" << (stageReached ? "yes" : "no")
                  << " expected='" << item.expected << "' actual='" << error << "' state=unchanged\n";
    }

    TopologyMachine small({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}}, 64 * 1024);
    const auto beforeGeometry = small.fullInventory();
    const uint32_t stageCount = small.device(11)->stageCalls.load() + small.device(12)->stageCalls.load();
    CHECK(!AArch64DeviceSnapshot::restore(small.cpu, small.ram, small.devices, good, &error));
    CHECK(error.find("RAM geometry") != std::string::npos);
    CHECK(small.device(11)->stageCalls.load() + small.device(12)->stageCalls.load() > stageCount);
    CHECK(small.fullInventory() == beforeGeometry);
    CHECK(!small.device(11)->isQuiesced() && !small.device(12)->isQuiesced());

    AArch64DeviceSnapshotLimits limited;
    limited.maxSnapshotBytes = good.size() - 1;
    const auto beforeLimit = inventory(destination.cpu, destination.ram, *destination.d1, *destination.d2, destination.devices);
    CHECK(!AArch64DeviceSnapshot::restore(destination.cpu, destination.ram, destination.devices, good, &error, limited));
    CHECK(error.find("configured byte limit") != std::string::npos);
    CHECK(inventory(destination.cpu, destination.ram, *destination.d1, *destination.d2, destination.devices) == beforeLimit);

    std::vector<uint8_t> cpuInvalid = good;
    put32(cpuInvalid.data() + machineSection->payload + 128 + 280, 0xFFFFFFFFu);
    resealSnapshot(cpuInvalid, true);
    CHECK(snapshotDigestMatches(cpuInvalid) && nestedMachineDigestMatches(cpuInvalid));
    const auto beforeCpuInvalid = inventory(destination.cpu, destination.ram, *destination.d1, *destination.d2, destination.devices);
    CHECK(!AArch64DeviceSnapshot::restore(destination.cpu, destination.ram, destination.devices, cpuInvalid, &error));
    CHECK(error.find("embedded AArch64 CPU checkpoint is malformed") != std::string::npos);
    CHECK(inventory(destination.cpu, destination.ram, *destination.d1, *destination.d2, destination.devices) == beforeCpuInvalid);

    std::cout << "AARCH64-9R malformed matrix cases=" << cases.size()
              << " plus RAM geometry, CPU-state, and configured-limit failures passed\n";
}

void expectTopologyRejected(const char* name, const std::vector<uint8_t>& snapshot,
                            TopologyMachine& destination, const char* expected) {
    const auto before = destination.fullInventory();
    std::string error;
    const bool restored = AArch64DeviceSnapshot::restore(destination.cpu, destination.ram,
        destination.devices, snapshot, &error);
    CHECK(!restored);
    CHECK(error.find(expected) != std::string::npos);
    CHECK(destination.fullInventory() == before);
    std::cout << "topology[" << name << "] expected='" << expected
              << "' actual='" << error << "' state=unchanged\n";
}

std::vector<uint8_t> makeSnapshotWithStandardTopology() {
    Machine source;
    source.d1->mutate(41);
    source.d2->mutate(42);
    std::string error;
    if (!source.devices.setLevel(101, true, &error) || !source.devices.signalEdge(102, &error))
        throw std::runtime_error(error);
    std::vector<uint8_t> snapshot;
    if (!AArch64DeviceSnapshot::capture(source.cpu, source.ram, source.devices, snapshot, &error))
        throw std::runtime_error(error);
    return snapshot;
}

void testCompleteTopologyMatrix() {
    const auto snapshot = makeSnapshotWithStandardTopology();
    std::string error;

    TopologyMachine identical({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    CHECK(AArch64DeviceSnapshot::restore(identical.cpu, identical.ram, identical.devices, snapshot, &error));
    Machine reference;
    reference.d1->mutate(41);
    reference.d2->mutate(42);
    CHECK(identical.device(11)->stateBytes() == reference.d1->stateBytes());
    CHECK(identical.device(12)->stateBytes() == reference.d2->stateBytes());
    AArch64InterruptSourceState state;
    CHECK(identical.devices.queryInterruptSource(101, state, &error) && state.asserted);
    CHECK(identical.devices.queryInterruptSource(102, state, &error) && state.pending);

    TopologyMachine wrongInstance({{21}, {12}}, {{101,21,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    expectTopologyRejected("correct type, wrong instance ID", snapshot, wrongInstance, "destination device identity");
    TopologyMachine wrongType({{11,"test.other",1}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    expectTopologyRejected("correct ID, wrong type", snapshot, wrongType, "destination device identity");
    TopologyMachine wrongSchema({{11,"test.stateful",2}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    expectTopologyRejected("unsupported destination schema", snapshot, wrongSchema, "destination device identity");
    TopologyMachine missingDevice({{11}}, {{101,11,AArch64InterruptMode::Level}});
    expectTopologyRejected("missing destination device", snapshot, missingDevice, "topology count does not match");
    TopologyMachine extraDevice({{11}, {12}, {13}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}, {103,13,AArch64InterruptMode::Edge}});
    expectTopologyRejected("extra destination device", snapshot, extraDevice, "topology count does not match");
    TopologyMachine reverseRegistration({{12}, {11}}, {{102,12,AArch64InterruptMode::Edge}, {101,11,AArch64InterruptMode::Level}});
    CHECK(AArch64DeviceSnapshot::restore(reverseRegistration.cpu, reverseRegistration.ram,
        reverseRegistration.devices, snapshot, &error));
    CHECK(reverseRegistration.device(11)->stateBytes() == reference.d1->stateBytes());
    CHECK(reverseRegistration.device(12)->stateBytes() == reference.d2->stateBytes());

    TopologyMachine removed({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    CHECK(removed.removeDevice(12, &error));
    CHECK(removed.devices.deviceCount() == 1 && removed.devices.interruptSourceCount() == 1);
    expectTopologyRejected("device removed after capture", snapshot, removed, "topology count does not match");

    TopologyMachine recreated({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    CHECK(recreated.removeDevice(12, &error));
    auto replacement = std::make_unique<TestStatefulDevice>(12, "test.stateful", 2);
    TestStatefulDevice* replacementPointer = replacement.get();
    CHECK(recreated.devices.registerDevice(std::move(replacement), &error));
    recreated.devicePointers.push_back(replacementPointer);
    CHECK(recreated.devices.registerInterruptSource(102,12,AArch64InterruptMode::Edge,&error));
    recreated.sourceIds.push_back(102);
    expectTopologyRejected("device recreated with a different schema", snapshot, recreated, "destination device identity");

    TopologyMachine missingSource({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}});
    expectTopologyRejected("missing required source", snapshot, missingSource, "topology count does not match");
    TopologyMachine extraSource({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}, {103,11,AArch64InterruptMode::Edge}});
    expectTopologyRejected("extra source", snapshot, extraSource, "topology count does not match");
    TopologyMachine wrongOwner({{11}, {12}}, {{101,12,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Edge}});
    expectTopologyRejected("incorrect source owner identity", snapshot, wrongOwner, "interrupt-source identity, owner, or trigger mode");
    TopologyMachine levelReplaced({{11}, {12}}, {{101,11,AArch64InterruptMode::Edge}, {102,12,AArch64InterruptMode::Edge}});
    expectTopologyRejected("level source replaced by edge", snapshot, levelReplaced, "interrupt-source identity, owner, or trigger mode");
    TopologyMachine edgeReplaced({{11}, {12}}, {{101,11,AArch64InterruptMode::Level}, {102,12,AArch64InterruptMode::Level}});
    expectTopologyRejected("edge source replaced by level", snapshot, edgeReplaced, "interrupt-source identity, owner, or trigger mode");

    AArch64DeviceRegistry fullDeviceRegistry;
    for (uint64_t id = 1; id <= kMaxRegisteredDevices; ++id)
        CHECK(fullDeviceRegistry.registerDevice(std::make_unique<TestStatefulDevice>(id), &error));
    CHECK(!fullDeviceRegistry.registerDevice(std::make_unique<TestStatefulDevice>(kMaxRegisteredDevices + 1), &error));
    CHECK(fullDeviceRegistry.deviceCount() == kMaxRegisteredDevices);

    AArch64DeviceRegistry fullSourceRegistry;
    CHECK(fullSourceRegistry.registerDevice(std::make_unique<TestStatefulDevice>(1), &error));
    for (uint64_t id = 1; id <= kMaxInterruptSources; ++id) {
        const auto mode = (id & 1u) ? AArch64InterruptMode::Level : AArch64InterruptMode::Edge;
        CHECK(fullSourceRegistry.registerInterruptSource(id, 1, mode, &error));
    }
    CHECK(!fullSourceRegistry.registerInterruptSource(kMaxInterruptSources + 1, 1, AArch64InterruptMode::Edge, &error));
    CHECK(fullSourceRegistry.interruptSourceCount() == kMaxInterruptSources);

    std::cout << "AARCH64-9R topology matrix: matching and order-independent restore passed; device/source identity, type, schema, count, ownership, mode, removal, recreation, and capacity mismatches rejected\n";
}

class CallbackHold final {
public:
    void stopInsideCallback() {
        std::unique_lock<std::mutex> lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        if (!changed_.wait_for(lock, std::chrono::seconds(5), [&] { return released_; })) timedOut_ = true;
    }
    bool waitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(2), [&] { return entered_; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        changed_.notify_all();
    }
    bool timedOut() {
        std::lock_guard<std::mutex> lock(mutex_);
        return timedOut_;
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
    bool timedOut_ = false;
};

void testDeterministicOverlapAndLifecycleSafety() {
    const auto restoreImage = makeSnapshotWithStandardTopology();
    ia64::Memory legacyRam(128 * 1024, false);
    AArch64ISAPlugin legacyCpu(0x88);
    std::vector<uint8_t> legacyImage;
    std::string error;
    CHECK(AArch64ExecutionSnapshot::capture(legacyCpu, legacyRam, legacyImage, &error));
    constexpr unsigned repetitions = 5;
    for (unsigned repetition = 0; repetition < repetitions; ++repetition) {
        Machine machine;
        write32(machine.ram, 0x1000, 0xD503201Fu);
        CallbackHold hold;
        bool nestedResetAccepted = true;
        bool nestedSnapshotAccepted = true;
        bool nestedLegacySnapshotAccepted = true;
        bool callbackStepRejected = false;
        bool callbackRamWriteRejected = false;
        bool captureAccepted = false;
        std::string captureError;
        std::vector<uint8_t> captured;
        machine.d1->onQuiesce = [&] {
            std::string nestedError;
            nestedResetAccepted = machine.devices.resetAll(&nestedError);
            hold.stopInsideCallback();
        };
        machine.d1->onCaptureState = [&] {
            std::vector<uint8_t> nested;
            std::string nestedError;
            nestedSnapshotAccepted = AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
                machine.devices, nested, &nestedError);
            nestedLegacySnapshotAccepted = AArch64ExecutionSnapshot::capture(machine.cpu,
                machine.ram, nested, &nestedError);
            callbackStepRejected = machine.cpu.cpu().step(machine.ram) == Fault::SnapshotBoundary;
            try { machine.ram.write<uint64_t>(0x200, 0xFFFFFFFFFFFFFFFFULL); }
            catch (const std::logic_error&) { callbackRamWriteRejected = true; }
        };
        std::thread captureThread([&] {
            captureAccepted = AArch64DeviceSnapshot::capture(machine.cpu, machine.ram,
                machine.devices, captured, &captureError);
        });
        const bool captureEntered = hold.waitUntilEntered();
        CHECK(captureEntered);
        if (captureEntered) {
            std::vector<uint8_t> overlapping;
            CHECK(!AArch64DeviceSnapshot::capture(machine.cpu, machine.ram, machine.devices, overlapping, &error));
            CHECK(error.find("overlapping") != std::string::npos);
            CHECK(!AArch64DeviceSnapshot::restore(machine.cpu, machine.ram, machine.devices, restoreImage, &error));
            CHECK(error.find("overlapping") != std::string::npos);
            CHECK(!AArch64ExecutionSnapshot::restore(machine.cpu, machine.ram, legacyImage, &error));
            CHECK(error.find("coordinator owns") != std::string::npos);
            std::vector<uint8_t> legacyCapture;
            CHECK(!AArch64ExecutionSnapshot::capture(machine.cpu, machine.ram, legacyCapture, &error));
            CHECK(error.find("coordinator owns") != std::string::npos);
            CHECK(!machine.devices.unregisterDevice(11, &error));
            CHECK(!machine.devices.registerDevice(std::make_unique<TestStatefulDevice>(13), &error));
            CHECK(!machine.devices.resetAll(&error));
            CHECK(!machine.devices.setLevel(101, false, &error));
            CHECK(!machine.devices.signalEdge(102, &error));
            CHECK(!machine.devices.removeInterruptSource(101, &error));
            AArch64InterruptSourceState queryState;
            CHECK(!machine.devices.queryInterruptSource(101, queryState, &error));
            CHECK(machine.cpu.cpu().step(machine.ram) == Fault::SnapshotBoundary);
            CHECK(machine.cpu.getPC() == 0x1000);
            bool ramWriteRejected = false;
            try { machine.ram.write<uint64_t>(0x204, 0xDEADBEEF); }
            catch (const std::logic_error&) { ramWriteRejected = true; }
            CHECK(ramWriteRejected);
        }
        hold.release();
        captureThread.join();
        CHECK(!hold.timedOut());
        CHECK(captureAccepted);
        CHECK(!nestedResetAccepted && !nestedSnapshotAccepted && !nestedLegacySnapshotAccepted);
        CHECK(callbackStepRejected && callbackRamWriteRejected);
        CHECK(machine.devices.deviceCount() == 2 && machine.devices.interruptSourceCount() == 2);
        CHECK(!machine.d1->isQuiesced() && !machine.d2->isQuiesced());
        AArch64DeviceSnapshotInfo capturedInfo;
        CHECK(AArch64DeviceSnapshot::validate(captured, &capturedInfo, &error));
        CHECK(capturedInfo.savedPc == 0x1000);
        CHECK(machine.cpu.cpu().step(machine.ram) == Fault::None); // stepping resumes once the coordinator releases the gate
        CHECK(machine.cpu.getPC() == 0x1004);
        CHECK(machine.devices.unregisterDevice(11, &error)); // owner destruction waits until its operation ends
        CHECK(machine.devices.deviceCount() == 1 && machine.devices.interruptSourceCount() == 1);

        Machine target;
        CallbackHold restoreHold;
        bool nestedCaptureAccepted = true;
        bool nestedLegacyRestoreAccepted = true;
        bool restoreAccepted = false;
        std::string restoreError;
        target.d1->onStageRestore = [&] {
            restoreHold.stopInsideCallback();
            std::vector<uint8_t> nested;
            std::string nestedError;
            nestedCaptureAccepted = AArch64DeviceSnapshot::capture(target.cpu, target.ram,
                target.devices, nested, &nestedError);
            nestedLegacyRestoreAccepted = AArch64ExecutionSnapshot::restore(target.cpu,
                target.ram, legacyImage, &nestedError);
        };
        std::thread restoreThread([&] {
            restoreAccepted = AArch64DeviceSnapshot::restore(target.cpu, target.ram,
                target.devices, restoreImage, &restoreError);
        });
        const bool restoreEntered = restoreHold.waitUntilEntered();
        CHECK(restoreEntered);
        if (restoreEntered) {
            CHECK(!AArch64DeviceSnapshot::restore(target.cpu, target.ram, target.devices, restoreImage, &error));
            CHECK(error.find("overlapping") != std::string::npos);
            std::vector<uint8_t> overlapping;
            CHECK(!AArch64DeviceSnapshot::capture(target.cpu, target.ram, target.devices, overlapping, &error));
            CHECK(error.find("overlapping") != std::string::npos);
            CHECK(!AArch64ExecutionSnapshot::restore(target.cpu, target.ram, legacyImage, &error));
            CHECK(error.find("coordinator owns") != std::string::npos);
            CHECK(!target.devices.removeInterruptSource(101, &error));
            CHECK(!target.devices.registerDevice(std::make_unique<TestStatefulDevice>(13), &error));
            CHECK(!target.devices.resetAll(&error));
            CHECK(!target.devices.setLevel(101, false, &error));
        }
        restoreHold.release();
        restoreThread.join();
        CHECK(!restoreHold.timedOut());
        CHECK(restoreAccepted && !nestedCaptureAccepted && !nestedLegacyRestoreAccepted);
        CHECK(!target.d1->isQuiesced() && !target.d2->isQuiesced());
        CHECK(target.devices.deviceCount() == 2 && target.devices.interruptSourceCount() == 2);
    }
    std::cout << "AARCH64-9R deterministic overlap tests repetitions=" << repetitions
              << " capture/capture, restore/capture, capture/restore, restore/restore, reset, registry/source mutation, CPU stepping, callback nesting, and owner removal passed\n";
}

void testLifecycleAndInterruptSources() {
    std::string error;
    AArch64DeviceRegistry registry;
    auto first=std::make_unique<TestStatefulDevice>(1); TestStatefulDevice* firstPtr=first.get();
    CHECK(registry.registerDevice(std::move(first),&error));
    auto duplicate=std::make_unique<TestStatefulDevice>(1);
    CHECK(!registry.registerDevice(std::move(duplicate),&error));
    CHECK(!registry.registerInterruptSource(1,99,AArch64InterruptMode::Level,&error));
    CHECK(registry.registerInterruptSource(7,1,AArch64InterruptMode::Level,&error));
    CHECK(registry.registerInterruptSource(8,1,AArch64InterruptMode::Edge,&error));
    CHECK(!registry.registerInterruptSource(8,1,AArch64InterruptMode::Edge,&error));
    CHECK(registry.setLevel(7,true,&error));
    CHECK(registry.signalEdge(8,&error));
    CHECK(registry.resetAll(&error));
    AArch64InterruptSourceState level,edge;
    CHECK(registry.queryInterruptSource(7,level,&error) && !level.asserted);
    CHECK(registry.queryInterruptSource(8,edge,&error) && !edge.pending);
    const auto reset1=firstPtr->stateBytes();
    firstPtr->mutate(9); CHECK(registry.resetAll(&error));
    const auto reset2=firstPtr->stateBytes(); CHECK(reset1==reset2);
    CHECK(registry.setLevel(7,true,&error)); CHECK(registry.signalEdge(8,&error));
    bool consumed=false; CHECK(registry.consumeEdge(8,consumed,&error) && consumed);
    CHECK(registry.consumeEdge(8,consumed,&error) && !consumed);
    CHECK(!registry.setLevel(999,true,&error)); CHECK(!registry.signalEdge(999,&error));
    CHECK(registry.unregisterDevice(1,&error));
    CHECK(registry.deviceCount()==0 && registry.interruptSourceCount()==0);
    CHECK(!registry.queryInterruptSource(7,level,&error));
    auto replacement=std::make_unique<TestStatefulDevice>(1); CHECK(registry.registerDevice(std::move(replacement),&error));
    CHECK(registry.registerInterruptSource(7,1,AArch64InterruptMode::Level,&error));
    CHECK(registry.queryInterruptSource(7,level,&error) && !level.asserted);

    // The AARCH64-8 v1 RAM-only envelope remains accepted by the device-aware
    // facade only when the destination topology is empty.
    ia64::Memory legacySourceRam(4096,false), legacyDestinationRam(4096,false);
    AArch64ISAPlugin legacySourceCpu(0x20), legacyDestinationCpu(0x80);
    legacySourceRam.write<uint64_t>(0x100,0x1122334455667788ULL);
    std::vector<uint8_t> legacy;
    CHECK(AArch64ExecutionSnapshot::capture(legacySourceCpu,legacySourceRam,legacy,&error));
    AArch64SnapshotInfo legacyInfo;
    CHECK(AArch64ExecutionSnapshot::validate(legacy,&legacyInfo,&error));
    CHECK(snapshotDigestMatches(legacy));
    CHECK(hexDigest(legacyInfo.sha256) == "41a4922957d41e8a09bcd56963264836dfa7918355173d35a954816e6def6941");
    std::cout << "legacy v1 fixture sha256=" << hexDigest(legacyInfo.sha256) << '\n';
    CHECK(AArch64DeviceSnapshot::validate(legacy,nullptr,&error));
    AArch64DeviceRegistry emptyTopology;
    CHECK(AArch64DeviceSnapshot::restore(legacyDestinationCpu,legacyDestinationRam,emptyTopology,legacy,&error));
    CHECK(legacyDestinationCpu.getPC()==0x20 && legacyDestinationRam.read<uint64_t>(0x100)==0x1122334455667788ULL);
    const auto legacyDeviceBefore=firstPtr->stateBytes();
    CHECK(!AArch64DeviceSnapshot::restore(legacyDestinationCpu,legacyDestinationRam,registry,legacy,&error));
    CHECK(firstPtr->stateBytes()==legacyDeviceBefore);

    Machine machine;
    machine.d2->refuseQuiesce=true;
    std::vector<uint8_t> snapshot;
    ia64::Memory ram(4096,false); AArch64ISAPlugin cpu(0);
    const auto beforeQuiesceRefusal = inventory(machine.cpu,machine.ram,*machine.d1,*machine.d2,machine.devices);
    // Device 11 successfully quiesces before 12 refuses; both holds must be released.
    CHECK(!AArch64DeviceSnapshot::capture(cpu,ram,machine.devices,snapshot,&error));
    CHECK(error.find("test device refused quiescence") != std::string::npos);
    CHECK(inventory(machine.cpu,machine.ram,*machine.d1,*machine.d2,machine.devices)==beforeQuiesceRefusal);
    CHECK(!machine.d1->isQuiesced() && !machine.d2->isQuiesced());
    machine.d2->refuseQuiesce=false;
    CHECK(machine.devices.setLevel(101,true,&error));

    Machine restoreRefusal;
    std::vector<uint8_t> saved;
    CHECK(AArch64DeviceSnapshot::capture(restoreRefusal.cpu,restoreRefusal.ram,restoreRefusal.devices,saved,&error));
    Machine restoreDestination;
    restoreDestination.d2->refuseQuiesce=true;
    restoreDestination.d1->mutate(111); restoreDestination.d2->mutate(112);
    const auto beforeRestoreQuiesceRefusal = inventory(restoreDestination.cpu,restoreDestination.ram,
        *restoreDestination.d1,*restoreDestination.d2,restoreDestination.devices);
    CHECK(!AArch64DeviceSnapshot::restore(restoreDestination.cpu,restoreDestination.ram,
        restoreDestination.devices,saved,&error));
    CHECK(error.find("test device refused quiescence") != std::string::npos);
    CHECK(inventory(restoreDestination.cpu,restoreDestination.ram,*restoreDestination.d1,
        *restoreDestination.d2,restoreDestination.devices)==beforeRestoreQuiesceRefusal);
    CHECK(!restoreDestination.d1->isQuiesced() && !restoreDestination.d2->isQuiesced());

    std::cout << "AARCH64-9 scenario B: bounded edge consumption, level state, reset, owner removal and quiescence failure passed\n";
}

struct ProcessMemoryObservation {
    uint64_t privateBytes = 0;
    uint64_t workingSetBytes = 0;
    uint64_t peakWorkingSetBytes = 0;
    uint64_t peakCommitBytes = 0;
    bool available = false;
};

ProcessMemoryObservation observeProcessMemory() {
    ProcessMemoryObservation result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters))) {
        result.privateBytes = static_cast<uint64_t>(counters.PrivateUsage);
        result.workingSetBytes = static_cast<uint64_t>(counters.WorkingSetSize);
        result.peakWorkingSetBytes = static_cast<uint64_t>(counters.PeakWorkingSetSize);
        result.peakCommitBytes = static_cast<uint64_t>(counters.PeakPagefileUsage);
        result.available = true;
    }
#endif
    return result;
}

void runMeasuredLifecycleCycle(unsigned cycle) {
    std::string error;
    AArch64ISAPlugin cpu(0x1000, ExceptionLevel::EL0);
    ia64::Memory ram(16 * 1024, false);
    AArch64DeviceRegistry devices;
    auto first = std::make_unique<TestStatefulDevice>(11);
    auto second = std::make_unique<TestStatefulDevice>(12);
    TestStatefulDevice* d1 = first.get();
    TestStatefulDevice* d2 = second.get();
    CHECK(devices.registerDevice(std::move(first), &error));
    CHECK(devices.registerDevice(std::move(second), &error));
    CHECK(devices.registerInterruptSource(101, 11, AArch64InterruptMode::Level, &error));
    CHECK(devices.registerInterruptSource(102, 12, AArch64InterruptMode::Edge, &error));
    d1->mutate(cycle + 1);
    d2->mutate(cycle + 1001);
    CHECK(devices.setLevel(101, true, &error));
    CHECK(devices.signalEdge(102, &error));
    cpu.cpu().writeX(0, cycle + 0xABC);
    cpu.setPC(0x1000 + cycle * 4);
    ram.write<uint64_t>(0x300, 0x1122334400000000ULL | cycle);
    std::vector<uint8_t> snapshot;
    CHECK(AArch64DeviceSnapshot::capture(cpu, ram, devices, snapshot, &error));
    AArch64DeviceSnapshotInfo info;
    CHECK(AArch64DeviceSnapshot::validate(snapshot, &info, &error));
    CHECK(info.deviceCount == 2 && info.interruptSourceCount == 2);
    const auto savedCpu = cpu.serialize_state();
    const std::vector<uint8_t> savedRam(ram.GetRawData(), ram.GetRawData() + ram.GetTotalSize());
    const auto savedD1 = d1->stateBytes(), savedD2 = d2->stateBytes();
    AArch64InterruptSourceState savedLevel, savedEdge;
    CHECK(devices.queryInterruptSource(101, savedLevel, &error));
    CHECK(devices.queryInterruptSource(102, savedEdge, &error));

    d1->mutate(71); d2->mutate(72);
    CHECK(devices.setLevel(101, false, &error));
    bool consumed = false;
    CHECK(devices.consumeEdge(102, consumed, &error) && consumed);
    cpu.cpu().writeX(0, cycle + 0xDEF);
    cpu.setPC(0x5000 + cycle * 4);
    ram.write<uint64_t>(0x300, 0xFFFFFFFFFFFFFFFFULL);

    CHECK(AArch64DeviceSnapshot::restore(cpu, ram, devices, snapshot, &error));
    CHECK(cpu.serialize_state() == savedCpu);
    CHECK(std::equal(savedRam.begin(), savedRam.end(), ram.GetRawData()));
    CHECK(d1->stateBytes() == savedD1 && d2->stateBytes() == savedD2);
    AArch64InterruptSourceState restoredLevel, restoredEdge;
    CHECK(devices.queryInterruptSource(101, restoredLevel, &error));
    CHECK(devices.queryInterruptSource(102, restoredEdge, &error));
    CHECK(restoredLevel.asserted == savedLevel.asserted && restoredEdge.pending == savedEdge.pending);

    CHECK(devices.unregisterDevice(11, &error));
    CHECK(devices.deviceCount() == 1 && devices.interruptSourceCount() == 1);
    CHECK(devices.unregisterDevice(12, &error));
    CHECK(devices.deviceCount() == 0 && devices.interruptSourceCount() == 0);
}

void testLifecycleMemoryGrowth() {
    const ProcessMemoryObservation baseline = observeProcessMemory();
    CHECK(baseline.available);
    if (!baseline.available) {
        std::cout << "lifecycle memory measurement unavailable on this host\n";
        return;
    }
    constexpr unsigned warmupCycles = 10;
    constexpr unsigned cyclesPerBatch = 100;
    constexpr unsigned stressBatches = 5;
    for (unsigned cycle = 0; cycle < warmupCycles; ++cycle) runMeasuredLifecycleCycle(cycle);
    const ProcessMemoryObservation warm = observeProcessMemory();
    CHECK(warm.available);
    std::array<ProcessMemoryObservation, stressBatches> batches{};
    for (unsigned batch = 0; batch < stressBatches; ++batch) {
        for (unsigned cycle = 0; cycle < cyclesPerBatch; ++cycle)
            runMeasuredLifecycleCycle(warmupCycles + batch * cyclesPerBatch + cycle);
        batches[batch] = observeProcessMemory();
        CHECK(batches[batch].available);
    }
    const ProcessMemoryObservation final = observeProcessMemory();
    CHECK(final.available);
    constexpr uint64_t boundedGrowthBytes = 16ULL * 1024ULL * 1024ULL;
    CHECK(batches[1].privateBytes <= batches[0].privateBytes + boundedGrowthBytes);
    CHECK(batches[2].privateBytes <= batches[1].privateBytes + boundedGrowthBytes);
    CHECK(final.privateBytes <= warm.privateBytes + 2 * boundedGrowthBytes);
    uint64_t peakWorkingSet = std::max(baseline.peakWorkingSetBytes, warm.peakWorkingSetBytes);
    uint64_t peakCommit = std::max(baseline.peakCommitBytes, warm.peakCommitBytes);
    for (const auto& batch : batches) {
        peakWorkingSet = std::max(peakWorkingSet, batch.peakWorkingSetBytes);
        peakCommit = std::max(peakCommit, batch.peakCommitBytes);
    }
    peakWorkingSet = std::max(peakWorkingSet, final.peakWorkingSetBytes);
    peakCommit = std::max(peakCommit, final.peakCommitBytes);
    std::cout << "AARCH64-9R lifecycle memory bytes: baselinePrivate=" << baseline.privateBytes
              << " warmPrivate=" << warm.privateBytes << " batches=";
    for (const auto& batch : batches) std::cout << batch.privateBytes << ',';
    std::cout << " finalPrivate=" << final.privateBytes
              << " baselineWorkingSet=" << baseline.workingSetBytes
              << " finalWorkingSet=" << final.workingSetBytes
              << " peakWorkingSet=" << peakWorkingSet << " peakCommit=" << peakCommit
              << " boundedPerBatchBytes=" << boundedGrowthBytes
              << " cycles=" << (warmupCycles + cyclesPerBatch * stressBatches) << '\n';
}

} // namespace

int main() {
    try {
        testDeviceReplayAndAtomicRestore();
        testGuestDrivenMmuDeviceReplay();
        testHashCorrectMalformedSnapshots();
        testCompleteTopologyMatrix();
        testDeterministicOverlapAndLifecycleSafety();
        testLifecycleAndInterruptSources();
        testLifecycleMemoryGrowth();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "AARCH64-9 host exception: " << exception.what() << '\n';
    }
    std::cout << "AARCH64-9 device snapshot assertions=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

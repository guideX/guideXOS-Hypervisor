#include "aarch64/AArch64DeviceSnapshot.h"
#include "aarch64/AArch64DeviceState.h"
#include "aarch64/AArch64ISAPlugin.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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
        if (refuseQuiesce) { if (error) *error = "test device refused quiescence"; return false; }
        quiesced_ = true; return true;
    }
    void resume() noexcept override { quiesced_ = false; }
    bool captureState(std::vector<uint8_t>& bytes, std::string*) const override {
        bytes.assign(24, 0); put32(bytes.data(), state_.control); put32(bytes.data()+4, state_.status);
        put64(bytes.data()+8, state_.counter); put32(bytes.data()+16, state_.events);
        put32(bytes.data()+20, state_.resetGeneration); return true;
    }
    bool stageRestore(const uint8_t* bytes, std::size_t size,
                      std::unique_ptr<AArch64DeviceRestoreToken>& token,
                      std::string* error) const override {
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
uint32_t movz(unsigned rd, uint16_t imm) { return 0xD2800000u | (static_cast<uint32_t>(imm) << 5) | (rd & 31u); }
uint32_t addXImm(unsigned rd, unsigned rn, uint16_t imm) {
    return 0x91000000u | (static_cast<uint32_t>(imm) << 10) | ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t strX(unsigned rt, unsigned rn) { return 0xF9000000u | ((rn & 31u) << 5) | (rt & 31u); }

uint64_t page(uint64_t pa) {
    return (pa & 0x0000FFFFFFFFF000ULL) | 3u | (1ULL << 6) | (3ULL << 8) | (1ULL << 10);
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

std::vector<uint8_t> inventory(AArch64ISAPlugin& cpu, ia64::Memory& ram,
                               TestStatefulDevice& a, TestStatefulDevice& b,
                               AArch64DeviceRegistry& sources) {
    std::vector<uint8_t> out = cpu.serialize_state();
    out.insert(out.end(), ram.GetRawData(), ram.GetRawData()+ram.GetTotalSize());
    const auto da = a.stateBytes(), db = b.stateBytes(); out.insert(out.end(), da.begin(), da.end()); out.insert(out.end(), db.begin(), db.end());
    for (uint64_t id : {uint64_t{101}, uint64_t{102}}) {
        AArch64InterruptSourceState state; std::string error;
        if (!sources.queryInterruptSource(id, state, &error)) throw std::runtime_error(error);
        const auto at=out.size(); out.resize(at+20); put64(out.data()+at,state.id); put64(out.data()+at+8,state.ownerDeviceId);
        out[at+16]=static_cast<uint8_t>(state.mode); out[at+17]=state.asserted?1:0; out[at+18]=state.pending?1:0; out[at+19]=0;
    }
    return out;
}

struct Machine {
    AArch64ISAPlugin cpu{0x1000, ExceptionLevel::EL0};
    ia64::Memory ram{128 * 1024, false};
    AArch64DeviceRegistry devices;
    TestStatefulDevice* d1 = nullptr;
    TestStatefulDevice* d2 = nullptr;
    Machine() {
        auto first=std::make_unique<TestStatefulDevice>(11); d1=first.get();
        auto second=std::make_unique<TestStatefulDevice>(12); d2=second.get();
        std::string error;
        if (!devices.registerDevice(std::move(first), &error) || !devices.registerDevice(std::move(second), &error) ||
            !devices.registerInterruptSource(101,11,AArch64InterruptMode::Level,&error) ||
            !devices.registerInterruptSource(102,12,AArch64InterruptMode::Edge,&error)) throw std::runtime_error(error);
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
    restored.d1->rejectRestorePreparation=true;
    CHECK(!AArch64DeviceSnapshot::restore(restored.cpu,restored.ram,restored.devices,snapshot,&error));
    restored.d1->rejectRestorePreparation=false;
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
    CHECK(AArch64DeviceSnapshot::validate(legacy,nullptr,&error));
    AArch64DeviceRegistry emptyTopology;
    CHECK(AArch64DeviceSnapshot::restore(legacyDestinationCpu,legacyDestinationRam,emptyTopology,legacy,&error));
    CHECK(legacyDestinationCpu.getPC()==0x20 && legacyDestinationRam.read<uint64_t>(0x100)==0x1122334455667788ULL);
    const auto legacyDeviceBefore=firstPtr->stateBytes();
    CHECK(!AArch64DeviceSnapshot::restore(legacyDestinationCpu,legacyDestinationRam,registry,legacy,&error));
    CHECK(firstPtr->stateBytes()==legacyDeviceBefore);

    Machine machine;
    machine.d1->refuseQuiesce=true;
    std::vector<uint8_t> snapshot;
    ia64::Memory ram(4096,false); AArch64ISAPlugin cpu(0);
    // A failed callback must release registry operation state and leave the device usable.
    CHECK(!AArch64DeviceSnapshot::capture(cpu,ram,machine.devices,snapshot,&error));
    machine.d1->refuseQuiesce=false;
    CHECK(!machine.d1->isQuiesced() && !machine.d2->isQuiesced());
    CHECK(machine.devices.setLevel(101,true,&error));

    std::cout << "AARCH64-9 scenario B: bounded edge consumption, level state, reset, owner removal and quiescence failure passed\n";
}

void testLifecycleStress() {
    std::string error;
    for (unsigned cycle=0; cycle<100; ++cycle) {
        AArch64ISAPlugin cpuA(0), cpuB(4);
        ia64::Memory ramA(4096,false), ramB(4096,false);
        AArch64DeviceRegistry registryA, registryB;
        auto a=std::make_unique<TestStatefulDevice>(1); TestStatefulDevice* aPtr=a.get();
        auto b=std::make_unique<TestStatefulDevice>(1); TestStatefulDevice* bPtr=b.get();
        CHECK(registryA.registerDevice(std::move(a),&error)); CHECK(registryB.registerDevice(std::move(b),&error));
        CHECK(registryA.registerInterruptSource(1,1,AArch64InterruptMode::Edge,&error));
        CHECK(registryB.registerInterruptSource(1,1,AArch64InterruptMode::Edge,&error));
        aPtr->mutate(cycle+1); bPtr->mutate(cycle+1000);
        CHECK(registryA.signalEdge(1,&error));
        std::vector<uint8_t> snap;
        CHECK(AArch64DeviceSnapshot::capture(cpuA,ramA,registryA,snap,&error));
        const DeviceState saved = aPtr->state();
        aPtr->mutate(50);
        CHECK(AArch64DeviceSnapshot::restore(cpuB,ramB,registryB,snap,&error));
        CHECK(saved==bPtr->state());
        AArch64InterruptSourceState src; CHECK(registryB.queryInterruptSource(1,src,&error) && src.pending);
        CHECK(registryA.unregisterDevice(1,&error)); CHECK(registryB.unregisterDevice(1,&error));
        CHECK(registryA.deviceCount()==0 && registryA.interruptSourceCount()==0);
        auto fresh=std::make_unique<TestStatefulDevice>(1); TestStatefulDevice* freshPtr=fresh.get();
        CHECK(registryA.registerDevice(std::move(fresh),&error)); CHECK(freshPtr->state().counter==0);
        CHECK(registryA.registerInterruptSource(1,1,AArch64InterruptMode::Edge,&error));
        bool consumed=false; CHECK(registryA.consumeEdge(1,consumed,&error) && !consumed);
    }
    std::cout << "AARCH64-9 scenario E: 100 bounded independent VM/device/source lifecycle cycles passed\n";
}

} // namespace

int main() {
    try {
        testDeviceReplayAndAtomicRestore();
        testLifecycleAndInterruptSources();
        testLifecycleStress();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "AARCH64-9 host exception: " << exception.what() << '\n';
    }
    std::cout << "AARCH64-9 device snapshot assertions=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

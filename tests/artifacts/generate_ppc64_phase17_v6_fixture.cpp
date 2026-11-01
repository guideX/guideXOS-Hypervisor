#include "PPC64ISAPlugin.h"
#include "VirtualMachine.h"
#include "VMCheckpoint.h"
#include "memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace ia64;
using namespace ia64::ppc64;

namespace {
constexpr uint64_t kEntry = 0x10000;
constexpr size_t kRamSize = 2U << 20U;
constexpr uint64_t kPartitionTable = 0x40000;
constexpr uint64_t kPartitionRoot = 0x50000;
constexpr uint64_t kPartitionL1 = 0x51000;
constexpr uint64_t kPartitionL2 = 0x52000;
constexpr uint64_t kPartitionL3 = 0x53000;
constexpr uint64_t kPteValid = 0x8000000000000000ULL;

uint32_t addi(uint8_t rt, uint8_t ra, int16_t imm) {
    return (14U << 26) | (static_cast<uint32_t>(rt) << 21) |
           (static_cast<uint32_t>(ra) << 16) | static_cast<uint16_t>(imm);
}
uint32_t mtspr(uint8_t rs, uint16_t spr) {
    return (31U << 26) | (static_cast<uint32_t>(rs) << 21) |
           (static_cast<uint32_t>(spr & 0x1fU) << 16) |
           (static_cast<uint32_t>((spr >> 5) & 0x1fU) << 11) | (467U << 1);
}
std::vector<uint8_t> encode(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> out;
    for (uint32_t word : words) {
        out.push_back(static_cast<uint8_t>(word >> 24));
        out.push_back(static_cast<uint8_t>(word >> 16));
        out.push_back(static_cast<uint8_t>(word >> 8));
        out.push_back(static_cast<uint8_t>(word));
    }
    return out;
}
void writeBE64(IMemory& memory, uint64_t address, uint64_t value) {
    std::array<uint8_t, 8> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>(value >> (56U - 8U * i));
    memory.Write(address, bytes.data(), bytes.size());
}
uint64_t partitionRadixRootWord(uint64_t root) {
    constexpr uint64_t rts = 21;
    return (root & 0x0fffffffffffff00ULL) |
           ((rts & 0x18ULL) << 58) | ((rts & 0x7ULL) << 5) | 13ULL;
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    PPC64PlatformConfiguration config;
    config.interruptMode = PPC64InterruptMode::XICS_COMPAT;
    config.virtualTerminalEnabled = true;
    config.reservedRanges.push_back({kPartitionTable, 0x10000, "partition table", true});
    config.reservedRanges.push_back({kPartitionRoot, 0x1000, "partition radix root", true});
    config.reservedRanges.push_back({kPartitionL1, 0x1000, "partition radix l1", true});
    config.reservedRanges.push_back({kPartitionL2, 0x1000, "partition radix l2", true});
    config.reservedRanges.push_back({kPartitionL3, 0x1000, "partition radix l3", true});
    VirtualMachine vm(kRamSize, 1, "PPC64", config);
    std::string error;
    if (!vm.init()) { std::fprintf(stderr, "init failed\n"); return 3; }
    const auto guest = encode({
        addi(7, 0, 0x80), mtspr(7, SPR_DEC), addi(10, 10, 1),
        ENCODING_SC_HCALL, addi(10, 10, 1)});
    if (!vm.preparePPC64Platform(guest, kEntry, kEntry, &error)) {
        std::fprintf(stderr, "prepare failed: %s\n", error.c_str()); return 4;
    }
    const std::array<uint8_t, 8> ram{{'V','6','R','A','M',0x12,0x34,0x56}};
    vm.getMemory().Write(0x2000, ram.data(), ram.size());
    if (!vm.configurePPC64InterruptSource(0x1111, PPC64XICSSourceType::EDGE,
                                           0x33, PPC64_XICS_SERVER_ID, 0,
                                           true, &error)) {
        std::fprintf(stderr, "XICS configure failed: %s\n", error.c_str()); return 5;
    }
    auto* manager = vm.getPPC64PartitionManager();
    auto* cpu = vm.getPPC64ISAPlugin();
    if (manager == nullptr || cpu == nullptr ||
        !manager->configurePartitionTable(vm.getMemory(), kPartitionTable, 4, error)) {
        std::fprintf(stderr, "partition table setup failed: %s\n", error.c_str()); return 6;
    }
    const auto partition = manager->createPartition(7, error);
    if (!partition) {
        std::fprintf(stderr, "partition activation failed: %s\n", error.c_str()); return 7;
    }
    writeBE64(vm.getMemory(), kPartitionRoot,
              kPteValid | kPartitionL1 | 9ULL);
    writeBE64(vm.getMemory(), kPartitionL1,
              kPteValid | kPartitionL2 | 9ULL);
    writeBE64(vm.getMemory(), kPartitionL2 + 8U,
              kPteValid | kPartitionL3 | 9ULL);
    const uint64_t pate = kPartitionTable + 7U * 16U;
    writeBE64(vm.getMemory(), pate,
              PPC64_PATE0_HR | partitionRadixRootWord(kPartitionRoot));
    writeBE64(vm.getMemory(), pate + 8U, 0);
    cpu->setMSR(MSR_ME | MSR_RI);
    cpu->writeGPR(3, PPC64_H_PUT_TERM_CHAR);
    cpu->writeGPR(4, vm.getPPC64VirtualTerminalId());
    cpu->writeGPR(5, 4);
    cpu->writeGPR(6, 0x56364f4b00000000ULL); // guest emits "V6OK"
    cpu->writeGPR(10, 0);
    cpu->setPC(kEntry);
    for (unsigned i = 0; i < 4; ++i) {
        if (!vm.step()) {
            std::fprintf(stderr, "guest step %u failed: %s\n", i,
                         vm.getLastExecutionDiagnostic().c_str()); return 8;
        }
    }
    if (cpu->getNIP() != kEntry + 16 || cpu->readGPR(10) != 1 ||
        cpu->getDEC() == 0 || vm.readPPC64TerminalOutput() !=
            std::vector<uint8_t>({'V','6','O','K'})) {
        std::fprintf(stderr, "fixture state self-check failed nip=%llx r10=%llu dec=%u output=%zu\n",
            static_cast<unsigned long long>(cpu->getNIP()),
            static_cast<unsigned long long>(cpu->readGPR(10)), cpu->getDEC(),
            vm.readPPC64TerminalOutput().size()); return 9;
    }
    const auto xics = vm.getPPC64XICSSnapshot();
    std::printf("historical writer input XICS sources=%zu presentations=%zu",
                xics.sources.size(), xics.presentations.size());
    if (!xics.sources.empty())
        std::printf(" first=%u priority=%u enabled=%u",
                    xics.sources[0].sourceId, xics.sources[0].priority,
                    xics.sources[0].enabled ? 1U : 0U);
    std::printf("\n");
    if (!VMCheckpointCodec::write(vm, argv[1], "ppc64-phase17-v6-fixture", &error)) {
        std::fprintf(stderr, "checkpoint write failed: %s\n", error.c_str()); return 10;
    }
    std::printf("created %s nip=0x%llx active_lpid=%u dec=%u output=V6OK\n", argv[1],
        static_cast<unsigned long long>(cpu->getNIP()), cpu->getActiveLPID(), cpu->getDEC());
    return 0;
}

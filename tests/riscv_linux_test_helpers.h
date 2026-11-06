#pragma once

#include "riscv/riscv_boot_contract.h"
#include "riscv/riscv_linux_image_loader.h"

#include <algorithm>
#include <array>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool LinuxFdtString(const FdtTree& tree, const std::string& path,
                    const std::string& name, const std::string& expected) {
    const auto* value = Property(tree, path, name);
    if (value == nullptr) return false;
    const auto terminator = std::find(value->begin(), value->end(), 0);
    std::string actual(value->begin(), terminator);
    return actual == expected;
}

bool LinuxFdtU32(const FdtTree& tree, const std::string& path,
                 const std::string& name, uint32_t expected) {
    const auto* value = Property(tree, path, name);
    return value != nullptr && value->size() == 4 && Be32(*value, 0) == expected;
}

bool LinuxFdtU64(const FdtTree& tree, const std::string& path,
                 const std::string& name, uint64_t expected) {
    const auto* value = Property(tree, path, name);
    return value != nullptr && value->size() == 8 && Be64Pair(*value, 0) == expected;
}

std::vector<std::string> LinuxFdtStringList(const std::vector<uint8_t>* property) {
    std::vector<std::string> result;
    if (property == nullptr) return result;
    size_t begin = 0;
    while (begin < property->size()) {
        size_t end = begin;
        while (end < property->size() && (*property)[end] != 0) ++end;
        if (end == begin || end == property->size()) return {};
        result.emplace_back(property->begin() + static_cast<std::ptrdiff_t>(begin),
                            property->begin() + static_cast<std::ptrdiff_t>(end));
        begin = end + 1;
    }
    return result;
}

bool LinuxFdtReg(const FdtTree& tree, const std::string& path,
                 uint64_t expectedBase, uint64_t expectedSize) {
    const auto* value = Property(tree, path, "reg");
    return value != nullptr && value->size() >= 16 && value->size() % 16 == 0 &&
           Be64Pair(*value, 0) == expectedBase &&
           Be64Pair(*value, 8) == expectedSize;
}

bool LinuxFdtNodeHasProperty(const FdtTree& tree, const std::string& path,
                             const std::string& name) {
    return Property(tree, path, name) != nullptr;
}

bool LinuxValidatePlatformFdt(const std::vector<uint8_t>& bytes,
                              bool postOpenSbi, std::string* failure = nullptr) {
    const auto fail = [&](const char* reason) {
        if (failure != nullptr) *failure = reason;
        return false;
    };
    const FdtTree tree = ParseFdt(bytes);
    if (!tree.valid || tree.totalSize == 0 || tree.totalSize != bytes.size()) {
        return fail("FDT header or structure is invalid");
    }
    if (!LinuxFdtString(tree, "/", "compatible", "guidexos,riscv18-linux-entry")) {
        return fail("root compatible does not identify RISCV-18 Linux-entry profile");
    }
    if (!LinuxFdtU32(tree, "/", "#address-cells", 2) ||
        !LinuxFdtU32(tree, "/", "#size-cells", 2)) {
        return fail("root address/size cells do not match the 64-bit physical map");
    }
    if (!LinuxFdtReg(tree, "/memory@80000000", boot_contract::kRamBase,
                     boot_contract::kRamSize) ||
        !LinuxFdtString(tree, "/memory@80000000", "device_type", "memory")) {
        return fail("FDT RAM range differs from the authoritative 64 MiB inventory");
    }
    if (!LinuxFdtU32(tree, "/cpus", "timebase-frequency",
                     static_cast<uint32_t>(boot_contract::kTimerFrequency))) {
        return fail("FDT timebase does not match the configured 10 MHz timer");
    }

    size_t cpuNodeCount = 0;
    for (const auto& [path, node] : tree.nodes) {
        (void)node;
        constexpr std::string_view cpuPrefix = "/cpus/cpu@";
        if (path.rfind(cpuPrefix, 0) == 0 &&
            path.find('/', cpuPrefix.size()) == std::string::npos) ++cpuNodeCount;
    }
    if (cpuNodeCount != 1) return fail("FDT must describe exactly one hart node");
    if (!LinuxFdtString(tree, "/cpus/cpu@0", "status", "okay") ||
        !LinuxFdtString(tree, "/cpus/cpu@0", "mmu-type", "riscv,sv39")) {
        return fail("hart 0 is disabled or does not declare Sv39");
    }
    const auto* hartReg = Property(tree, "/cpus/cpu@0", "reg");
    if (hartReg == nullptr || hartReg->size() != 4 || Be32(*hartReg, 0) != 0) {
        return fail("the only CPU node does not identify hart 0");
    }
    if (!LinuxFdtString(tree, "/cpus/cpu@0", "riscv,isa",
                        "rv64ima_zicsr_zifencei") ||
        !LinuxFdtString(tree, "/cpus/cpu@0", "riscv,isa-base", "rv64i")) {
        return fail("legacy and standardized ISA properties do not match the emulator profile");
    }
    const std::set<std::string> expectedExtensions{
        "i", "m", "a", "zicsr", "zifencei",
    };
    const auto extensionList = LinuxFdtStringList(
        Property(tree, "/cpus/cpu@0", "riscv,isa-extensions"));
    const std::set<std::string> extensions(extensionList.begin(), extensionList.end());
    if (extensionList.size() != expectedExtensions.size() ||
        extensions != expectedExtensions) {
        return fail("standard ISA extension properties advertise missing or unsupported extensions");
    }
    if (!LinuxFdtString(tree, "/cpus/cpu@0", "compatible", "riscv") ||
        !LinuxFdtString(tree, "/cpus/cpu@0/interrupt-controller", "compatible",
                        "riscv,cpu-intc") ||
        !LinuxFdtNodeHasProperty(tree, "/cpus/cpu@0/interrupt-controller",
                                 "interrupt-controller") ||
        !LinuxFdtU32(tree, "/cpus/cpu@0/interrupt-controller", "#interrupt-cells", 1)) {
        return fail("boot CPU interrupt-controller binding is invalid");
    }

    if (!LinuxFdtString(tree, "/chosen", "stdout-path", "/soc/serial@10000000")) {
        return fail("chosen console does not point at UART0");
    }
    for (const auto& uart : std::array<std::array<uint64_t, 3>, 2>{{
             {{0x10000000ULL, 0x100ULL, 10}},
             {{0x1000A000ULL, 0x100ULL, 12}},
         }}) {
        const std::string path = uart[0] == 0x10000000ULL
            ? "/soc/serial@10000000" : "/soc/serial@1000a000";
        if (!LinuxFdtReg(tree, path, uart[0], uart[1]) ||
            !LinuxFdtString(tree, path, "compatible", "ns16550a")) {
            return fail("UART MMIO range or NS16550 compatible property is invalid");
        }
        if (!LinuxFdtU32(tree, path, "reg-io-width", 1) ||
            !LinuxFdtU32(tree, path, "interrupt-parent", 2) ||
            !LinuxFdtU32(tree, path, "interrupts", static_cast<uint32_t>(uart[2]))) {
            return fail("UART register width or PLIC interrupt binding is invalid");
        }
    }
    if (!LinuxFdtReg(tree, "/soc/interrupt-controller@c000000",
                     0x0C000000ULL, 0x00400000ULL) ||
        !LinuxFdtU32(tree, "/soc/interrupt-controller@c000000", "riscv,ndev", 96) ||
        !LinuxFdtString(tree, "/soc/interrupt-controller@c000000", "compatible",
                        "sifive,plic-1.0.0")) {
        return fail("PLIC binding, range, or source count is invalid");
    }
    const auto* plicInterrupts = Property(
        tree, "/soc/interrupt-controller@c000000", "interrupts-extended");
    if (plicInterrupts == nullptr || plicInterrupts->size() != 16 ||
        Be32(*plicInterrupts, 0) != 1 ||
        Be32(*plicInterrupts, 4) != (postOpenSbi ? UINT32_MAX : 11U) ||
        Be32(*plicInterrupts, 8) != 1 ||
        Be32(*plicInterrupts, 12) != 9 ||
        !LinuxFdtNodeHasProperty(tree, "/soc/interrupt-controller@c000000",
                                 "interrupt-controller") ||
        !LinuxFdtU32(tree, "/soc/interrupt-controller@c000000",
                     "#interrupt-cells", 1)) {
        return fail("PLIC machine and supervisor interrupt contexts are invalid");
    }
    const auto* aclintReg = Property(tree, "/aclint-mtimer@200bff8", "reg");
    if (aclintReg == nullptr || aclintReg->size() != 32 ||
        Be64Pair(*aclintReg, 0) != 0x0200BFF8ULL ||
        Be64Pair(*aclintReg, 8) != 8ULL ||
        Be64Pair(*aclintReg, 16) != 0x02004000ULL ||
        Be64Pair(*aclintReg, 24) != 8ULL ||
        !LinuxFdtString(tree, "/aclint-mtimer@200bff8", "compatible",
                        "riscv,aclint-mtimer")) {
        return fail("ACLINT mtime and mtimecmp ranges are invalid");
    }
    const auto* aclintInterrupts = Property(
        tree, "/aclint-mtimer@200bff8", "interrupts-extended");
    if (aclintInterrupts == nullptr || aclintInterrupts->size() != 8 ||
        Be32(*aclintInterrupts, 0) != 1 || Be32(*aclintInterrupts, 4) != 7) {
        return fail("ACLINT machine-timer interrupt binding is invalid");
    }
    if (!LinuxFdtU32(tree, "/reserved-memory", "#address-cells", 2) ||
        !LinuxFdtU32(tree, "/reserved-memory", "#size-cells", 2) ||
        !LinuxFdtNodeHasProperty(tree, "/reserved-memory", "ranges")) {
        return fail("reserved-memory address translation contract is invalid");
    }
    if (!LinuxFdtString(tree, "/aclint-mtimer@200bff8", "status",
                        postOpenSbi ? "disabled" : "okay")) {
        return fail("ACLINT supervisor visibility does not match firmware ownership");
    }

    if (!postOpenSbi) {
        if (!LinuxFdtU64(tree, "/chosen/opensbi-domains/supervisor-domain",
                         "next-addr", boot_contract::kLinuxImageEntry) ||
            !LinuxFdtU64(tree, "/chosen/opensbi-domains/supervisor-domain",
                         "next-arg1", boot_contract::kLinuxFdtAddress) ||
            !LinuxFdtU32(tree, "/chosen/opensbi-domains/supervisor-domain",
                         "next-mode", 1)) {
            return fail("OpenSBI domain entry, FDT argument, or next-mode is stale");
        }
        const auto* regions = Property(
            tree, "/chosen/opensbi-domains/supervisor-domain", "regions");
        if (regions == nullptr || regions->size() != 10U * 8U ||
            tree.nodes.find("/chosen/opensbi-domains/guest-page-tables") != tree.nodes.end()) {
            return fail("Linux OpenSBI domain retains test-only PMP regions");
        }
        if (tree.nodes.find("/reserved-memory/opensbi-slot-gap@800a0000") ==
                tree.nodes.end() ||
            tree.nodes.find("/reserved-memory/firmware-fdt@82200000") ==
                tree.nodes.end() ||
            tree.nodes.find("/reserved-memory/supervisor-payload@80200000") !=
                tree.nodes.end() ||
            tree.nodes.find("/reserved-memory/guest-page-tables@83c00000") !=
                tree.nodes.end() ||
            tree.nodes.find("/reserved-memory/guest-result@83e00000") !=
                tree.nodes.end() ||
            tree.nodes.find("/reserved-memory/guest-stack@83ff0000") !=
                tree.nodes.end()) {
            return fail("Linux reserved-memory contains absent or test-only ranges");
        }
    } else {
        if (tree.nodes.find("/chosen/opensbi-domains") != tree.nodes.end() ||
            LinuxFdtNodeHasProperty(tree, "/cpus/cpu@0", "opensbi-domain")) {
            return fail("OpenSBI domain-only configuration leaked into the S-mode FDT");
        }
    }

    for (const auto& [path, node] : tree.nodes) {
        if (path.find("virtio") != std::string::npos ||
            path.find("pci") != std::string::npos ||
            path.find("ethernet") != std::string::npos ||
            path.find("network") != std::string::npos ||
            path.find("block") != std::string::npos) {
            return fail("FDT advertises a device outside the RISCV-18 platform");
        }
        const auto compatible = node.properties.find("compatible");
        if (compatible != node.properties.end()) {
            const std::string list(compatible->second.begin(), compatible->second.end());
            if (list.find("virtio") != std::string::npos ||
                list.find("pci") != std::string::npos ||
                list.find("ethernet") != std::string::npos ||
                list.find("block") != std::string::npos) {
                return fail("FDT compatible list advertises unsupported hardware");
            }
        }
    }
    return true;
}

uint64_t LinuxReadRangeCell(const std::vector<uint8_t>* property, size_t offset) {
    if (property == nullptr || property->size() < offset + 8) return 0;
    return Be64Pair(*property, offset);
}

bool LinuxFdtBootHartEnabled(const FdtTree& tree, uint64_t& hartId) {
    hartId = UINT64_MAX;
    if (!tree.valid || !LinuxFdtString(tree, "/cpus/cpu@0", "status", "okay")) {
        return false;
    }
    const auto* reg = Property(tree, "/cpus/cpu@0", "reg");
    if (reg == nullptr || reg->size() != 4) return false;
    hartId = Be32(*reg, 0);
    return hartId == boot_contract::kLinuxBootHartId;
}

uint64_t LinuxFdtNextAddress(const FdtTree& tree) {
    const auto* property = Property(
        tree, "/chosen/opensbi-domains/supervisor-domain", "next-addr");
    return LinuxReadRangeCell(property, 0);
}

std::array<RiscvLinuxReservedRange, 4> LinuxExpectedReservations() {
    std::array<RiscvLinuxReservedRange, 4> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        const auto& range = boot_contract::kLinuxReservations[index];
        result[index] = {range.name, range.start, range.size, range.alignment};
    }
    return result;
}

std::array<RiscvLinuxPmpRegion, 5> LinuxExpectedPmpRegions() {
    std::array<RiscvLinuxPmpRegion, 5> result{};
    for (size_t index = 0; index < result.size(); ++index) {
        const auto& range = boot_contract::kPmpGuestRamWindows[index];
        result[index] = {
            index, range.start, range.start + range.size, 0x1FU, true,
        };
    }
    return result;
}

RiscvLinuxImageLoadOptions LinuxMakeImageOptions(
    const std::vector<uint8_t>& sourceFdt, const FdtTree& tree,
    std::span<const RiscvLinuxReservedRange> reservations,
    std::span<const RiscvLinuxPmpRegion> pmpRegions) {
    RiscvLinuxImageLoadOptions options;
    options.ramBase = boot_contract::kRamBase;
    options.ramSize = boot_contract::kRamSize;
    options.requiredTextOffset = boot_contract::kLinuxImageTextOffset;
    options.residentFirmwareBase = boot_contract::kFirmwareEntry;
    options.residentFirmwareSize =
        boot_contract::kFirmwareSlotEnd - boot_contract::kFirmwareEntry;
    options.firmwareJumpAddress = LinuxFdtNextAddress(tree);
    options.fdtAddress = boot_contract::kLinuxFdtAddress;
    options.fdtCapacity = boot_contract::kLinuxFdtCapacity;
    options.minimumFdtCapacity = boot_contract::kLinuxFdtCapacity;
    options.fdtActualBytes = tree.totalSize;
    options.fdtValid = LinuxValidatePlatformFdt(sourceFdt, false);
    options.bootHartEnabled = LinuxFdtBootHartEnabled(tree, options.bootHartId);
    options.expectedBootHartId = boot_contract::kLinuxBootHartId;
    options.reservedRanges = reservations;
    options.pmpRegions = pmpRegions;
    options.maxFileBytes = static_cast<size_t>(boot_contract::kRamSize);
    return options;
}

} // namespace

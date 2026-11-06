#define RISCV_OPENSBI_FWJUMP_HELPERS_ONLY
#include "test_riscv_opensbi_fw_jump.cpp"
#include "riscv_linux_test_helpers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace {

void WriteLe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

void WriteLe64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

std::string JsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t keyAt = json.find(needle);
    if (keyAt == std::string::npos) return {};
    const size_t colon = json.find(':', keyAt + needle.size());
    if (colon == std::string::npos) return {};
    const size_t beginQuote = json.find('"', colon + 1);
    if (beginQuote == std::string::npos) return {};
    const size_t endQuote = json.find('"', beginQuote + 1);
    if (endQuote == std::string::npos) return {};
    return json.substr(beginQuote + 1, endQuote - beginQuote - 1);
}

std::string AsString(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

void ExpectPlanError(std::span<const uint8_t> image,
                     const RiscvLinuxImageLoadOptions& options,
                     RiscvLinuxImageError expected,
                     const std::string& label) {
    const auto result = planRiscvLinuxImage(image, options);
    Expect(result.error == expected,
           label + " expected=" + RiscvLinuxImageErrorToString(expected) +
               " actual=" + RiscvLinuxImageErrorToString(result.error) +
               " detail=" + result.detail);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: test_riscv_linux_image_loader Image manifest.json platform.dtb\n";
        return 2;
    }
    const std::vector<uint8_t> image = ReadFile(argv[1]);
    const std::string manifest = AsString(ReadFile(argv[2]));
    const std::vector<uint8_t> sourceFdt = ReadFile(argv[3]);
    const FdtTree sourceTree = ParseFdt(sourceFdt);
    std::string expectedHash = JsonString(manifest, "imageSha256");
    std::transform(expectedHash.begin(), expectedHash.end(), expectedHash.begin(),
        [](unsigned char byte) { return static_cast<char>(std::toupper(byte)); });
    const auto reservations = LinuxExpectedReservations();
    const auto pmpRegions = LinuxExpectedPmpRegions();
    auto options = LinuxMakeImageOptions(sourceFdt, sourceTree,
                                         reservations, pmpRegions);

    Expect(!image.empty(), "authentic pinned Linux Image fixture is available");
    Expect(!expectedHash.empty() && Sha256(image) == expectedHash,
           "Linux Image SHA-256 matches the committed v6.12 fixture manifest");
    std::string sourceFdtFailure;
    const bool sourceFdtValid =
        LinuxValidatePlatformFdt(sourceFdt, false, &sourceFdtFailure);
    Expect(sourceFdtValid,
           "Linux-specific source FDT is structurally valid and describes the selected platform: " +
               sourceFdtFailure);
    Expect(options.firmwareJumpAddress == boot_contract::kLinuxImageEntry,
           "source FDT OpenSBI next-addr equals the Linux image entry");
    const auto validPlan = planRiscvLinuxImage(image, options);
    Expect(validPlan.ok(), "authentic pinned Linux Image parses and passes placement validation");
    if (validPlan.ok()) {
        Expect(validPlan.plan.header.version == boot_contract::kLinuxImageHeaderVersion &&
               validPlan.plan.header.textOffset == boot_contract::kLinuxImageTextOffset &&
               validPlan.plan.loadAddress == boot_contract::kLinuxImageEntry &&
               validPlan.plan.entryPoint == boot_contract::kLinuxImageEntry,
               "parsed v0.2 header selects the aligned 2 MiB RV64 S-mode entry");
        Expect(validPlan.plan.fileSize == image.size() &&
               validPlan.plan.effectiveSize == validPlan.plan.header.imageSize &&
               validPlan.plan.fileSize <= validPlan.plan.effectiveSize,
               "file-backed bytes and the effective runtime footprint remain distinct");
        Expect(validPlan.plan.initialBranchTarget ==
                   boot_contract::kLinuxImageEntry + 0xD4ULL,
               "actual Linux header JAL enters the in-range _start_kernel body");
        Expect(validPlan.plan.effectiveSize <=
                   boot_contract::kFdtAddress - validPlan.plan.loadAddress,
               "complete effective Image footprint ends before the active FDT reservation");
    }

    std::vector<uint8_t> shortHeader(image.begin(),
        image.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(image.size(), 63)));
    ExpectPlanError(shortHeader, options, RiscvLinuxImageError::TruncatedHeader,
                    "truncated header");

    auto badMagic = image;
    if (badMagic.size() >= 64) {
        WriteLe32(badMagic, 56, 0);
        ExpectPlanError(badMagic, options, RiscvLinuxImageError::BadMagic,
                        "bad magic2");
    }

    auto unsupportedVersion = image;
    if (unsupportedVersion.size() >= 64) {
        WriteLe32(unsupportedVersion, 32, 0x00010000U);
        ExpectPlanError(unsupportedVersion, options,
                        RiscvLinuxImageError::UnsupportedVersion,
                        "unsupported header version");
    }

    auto bigEndian = image;
    if (bigEndian.size() >= 64) {
        WriteLe64(bigEndian, 24, 1);
        ExpectPlanError(bigEndian, options,
                        RiscvLinuxImageError::UnsupportedEndianness,
                        "big-endian flag");
    }

    auto unknownFlags = image;
    if (unknownFlags.size() >= 64) {
        WriteLe64(unknownFlags, 24, 2);
        ExpectPlanError(unknownFlags, options,
                        RiscvLinuxImageError::UnsupportedFlags,
                        "unknown header flag");
    }

    auto nonzeroReserved = image;
    if (nonzeroReserved.size() >= 64) {
        WriteLe32(nonzeroReserved, 36, 1);
        ExpectPlanError(nonzeroReserved, options,
                        RiscvLinuxImageError::NonzeroReservedField,
                        "nonzero reserved header word");
    }

    auto zeroSize = image;
    if (zeroSize.size() >= 64) {
        WriteLe64(zeroSize, 16, 0);
        ExpectPlanError(zeroSize, options, RiscvLinuxImageError::InvalidImageSize,
                        "zero effective image size");
        WriteLe64(zeroSize, 16, 63);
        ExpectPlanError(zeroSize, options, RiscvLinuxImageError::InvalidImageSize,
                        "effective size smaller than the header");
    }

    auto shortFootprint = image;
    if (shortFootprint.size() >= 64 && image.size() > 65) {
        WriteLe64(shortFootprint, 16, static_cast<uint64_t>(image.size() - 1));
        ExpectPlanError(shortFootprint, options,
                        RiscvLinuxImageError::FileExceedsImageFootprint,
                        "file length exceeds effective image size");
    }

    auto outsideEntry = std::vector<uint8_t>(image.begin(), image.begin() + 64);
    WriteLe64(outsideEntry, 16, 64);
    ExpectPlanError(outsideEntry, options,
                    RiscvLinuxImageError::EntryOutsideImage,
                    "header branch target outside the effective image");

    auto deprecatedMagicOmitted = image;
    if (deprecatedMagicOmitted.size() >= 64) {
        WriteLe64(deprecatedMagicOmitted, 48, 0);
        Expect(planRiscvLinuxImage(deprecatedMagicOmitted, options).ok(),
               "modern magic2 remains sufficient when deprecated magic is absent");
    }

    auto wrongTextOffset = image;
    if (wrongTextOffset.size() >= 64) {
        WriteLe64(wrongTextOffset, 8, 0);
        ExpectPlanError(wrongTextOffset, options,
                        RiscvLinuxImageError::UnsupportedTextOffset,
                        "wrong text_offset");
        WriteLe64(wrongTextOffset, 8, boot_contract::kLinuxImageTextOffset + 4);
        auto misalignedOptions = options;
        misalignedOptions.requiredTextOffset = boot_contract::kLinuxImageTextOffset + 4;
        ExpectPlanError(wrongTextOffset, misalignedOptions,
                        RiscvLinuxImageError::MisalignedLoadAddress,
                        "misaligned text_offset");
    }

    auto overflowOptions = options;
    overflowOptions.ramBase = 0xFFFFFFFFFFE00000ULL;
    overflowOptions.ramSize = 0x1000ULL;
    ExpectPlanError(image, overflowOptions,
                    RiscvLinuxImageError::ArithmeticOverflow,
                    "overflowing image entry arithmetic");

    auto beyondRamOptions = options;
    beyondRamOptions.ramSize =
        boot_contract::kLinuxImageTextOffset + validPlan.plan.effectiveSize - 1;
    ExpectPlanError(image, beyondRamOptions,
                    RiscvLinuxImageError::ImageOutsideRam,
                    "effective Image beyond guest RAM");

    auto wrongFirmwareTarget = options;
    wrongFirmwareTarget.firmwareJumpAddress =
        boot_contract::kLinuxImageEntry + validPlan.plan.effectiveSize;
    ExpectPlanError(image, wrongFirmwareTarget,
                    RiscvLinuxImageError::FirmwareJumpMismatch,
                    "entrypoint outside loaded effective memory");

    auto firmwareCollision = options;
    firmwareCollision.residentFirmwareBase = boot_contract::kLinuxImageEntry - 0x1000;
    firmwareCollision.residentFirmwareSize = 0x2000;
    ExpectPlanError(image, firmwareCollision,
                    RiscvLinuxImageError::FirmwareOverlap,
                    "resident firmware overlap");

    auto fdtCollision = options;
    fdtCollision.fdtAddress = boot_contract::kLinuxImageEntry + 0x1000;
    ExpectPlanError(image, fdtCollision,
                    RiscvLinuxImageError::FdtOverlap,
                    "FDT overlap");

    auto insufficientFdt = options;
    insufficientFdt.fdtCapacity = options.fdtActualBytes - 1;
    ExpectPlanError(image, insufficientFdt,
                    RiscvLinuxImageError::FdtCapacityInsufficient,
                    "insufficient reserved FDT capacity");

    auto invalidFdtOptions = options;
    invalidFdtOptions.fdtValid = false;
    ExpectPlanError(image, invalidFdtOptions,
                    RiscvLinuxImageError::InvalidFdt,
                    "invalid FDT header or platform");
    auto invalidFdt = sourceFdt;
    if (!invalidFdt.empty()) invalidFdt[0] = 0;
    Expect(!LinuxValidatePlatformFdt(invalidFdt, false),
           "damaged FDT magic is rejected by the platform validator");

    auto disabledCpu = sourceFdt;
    const std::array<uint8_t, 5> okayString{{'o', 'k', 'a', 'y', 0}};
    const auto okayAt = std::search(disabledCpu.begin(), disabledCpu.end(),
                                    okayString.begin(), okayString.end());
    if (okayAt != disabledCpu.end()) {
        const std::array<uint8_t, 5> disabledString{{'f', 'a', 'i', 'l', 0}};
        std::copy(disabledString.begin(), disabledString.end(), okayAt);
    }
    const FdtTree disabledTree = ParseFdt(disabledCpu);
    uint64_t disabledHartId = UINT64_MAX;
    Expect(!LinuxValidatePlatformFdt(disabledCpu, false) &&
           !LinuxFdtBootHartEnabled(disabledTree, disabledHartId),
           "disabled hart 0 is rejected by the Linux FDT validator");
    auto disabledHartOptions = options;
    disabledHartOptions.bootHartEnabled = false;
    ExpectPlanError(image, disabledHartOptions,
                    RiscvLinuxImageError::InvalidBootHart,
                    "disabled boot hart");

    auto wrongPmp = pmpRegions;
    wrongPmp[0].config = 0x1BU; // S-mode RW without execute.
    auto wrongPmpOptions = options;
    wrongPmpOptions.pmpRegions = wrongPmp;
    ExpectPlanError(image, wrongPmpOptions,
                    RiscvLinuxImageError::PmpCoverageMissing,
                    "incorrect S-mode execute permission");

    auto pageTableCollisionOptions = options;
    std::array<RiscvLinuxReservedRange, 5> pageTableConflict{};
    std::copy(reservations.begin(), reservations.end(), pageTableConflict.begin());
    pageTableConflict[4] = {
        "active-page-table-window", validPlan.plan.loadAddress + 0x1000,
        0x1000, 0x1000,
    };
    pageTableCollisionOptions.reservedRanges = pageTableConflict;
    ExpectPlanError(image, pageTableCollisionOptions,
                    RiscvLinuxImageError::ImageReservationConflict,
                    "active page-table reservation collision");

    auto staleFwJump = options;
    staleFwJump.firmwareJumpAddress += 4;
    ExpectPlanError(image, staleFwJump,
                    RiscvLinuxImageError::FirmwareJumpMismatch,
                    "stale firmware FW_JUMP address");

    if (validPlan.ok() && validPlan.plan.effectiveSize <=
            options.fdtAddress - validPlan.plan.loadAddress - 64) {
        // The original pinned binary remains the only successful boot image.
        // This derived, header-only unit case never reaches the CPU execution
        // test; it verifies the format's zero-initialized runtime tail rule.
        auto zeroTailImage = image;
        const uint64_t largerFootprint = validPlan.plan.effectiveSize + 64;
        WriteLe64(zeroTailImage, 16, largerFootprint);
        const auto zeroTailPlan = planRiscvLinuxImage(zeroTailImage, options);
        Expect(zeroTailPlan.ok() && zeroTailPlan.plan.fileSize == image.size() &&
               zeroTailPlan.plan.effectiveSize == largerFootprint,
               "header-authorized zero-initialized runtime tail is planned separately");
    }

    RV64CPU cpu(PmpMachineProfile::StandardPmp16);
    std::vector<uint8_t> originalRam(static_cast<size_t>(boot_contract::kRamSize), 0xA5);
    cpu.loadProgram(boot_contract::kRamBase, originalRam);
    cpu.setPC(boot_contract::kRamBase + 0x1234);
    cpu.writeGR(10, 0x55667788ULL);
    std::array<uint8_t, 32> before{};
    const bool saved = cpu.readPhysical(boot_contract::kLinuxImageEntry,
                                        before.data(), before.size());
    auto rejectedLoadOptions = options;
    rejectedLoadOptions.firmwareJumpAddress += 4;
    const auto rejectedLoad = loadRiscvLinuxImage(image, cpu, rejectedLoadOptions);
    std::array<uint8_t, 32> after{};
    const bool readAfter = cpu.readPhysical(boot_contract::kLinuxImageEntry,
                                            after.data(), after.size());
    Expect(saved && readAfter && before == after &&
           rejectedLoad.error == RiscvLinuxImageError::FirmwareJumpMismatch &&
           cpu.getPC() == boot_contract::kRamBase + 0x1234 &&
           cpu.readGR(10) == 0x55667788ULL &&
           cpu.privilege() == PrivilegeMode::Machine &&
           cpu.machineMmioEventSequence() == 0,
           "failed placement leaves RAM, CPU state, and device state unchanged");

    if (validPlan.ok()) {
        const auto firstLoad = loadRiscvLinuxImage(image, cpu, options);
        const auto secondLoad = loadRiscvLinuxImage(image, cpu, options);
        std::vector<uint8_t> loadedBytes(image.size(), 0);
        const bool loadedRead = cpu.readPhysical(
            validPlan.plan.loadAddress, loadedBytes.data(), loadedBytes.size());
        Expect(firstLoad.ok() && secondLoad.ok() && loadedRead &&
               loadedBytes == image &&
               cpu.getPC() == boot_contract::kRamBase + 0x1234 &&
               cpu.readGR(10) == 0x55667788ULL &&
               cpu.privilege() == PrivilegeMode::Machine,
               "repeated loads remain deterministic and do not reset CPU state");
        const uint8_t fdtSentinel = 0x6C;
        uint8_t fdtAfter = 0;
        const bool fdtUnchanged =
            cpu.writePhysical(options.fdtAddress, &fdtSentinel, 1) &&
            cpu.readPhysical(options.fdtAddress, &fdtAfter, 1) &&
            fdtAfter == fdtSentinel;
        Expect(fdtUnchanged,
               "successful loading writes only the validated effective Linux Image range");
    }

    std::cout << "RISCV-18 Linux Image loader checks=" << g_checks
              << " failures=" << g_failures << '\n';
    return g_failures == 0 ? 0 : 1;
}

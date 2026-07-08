#include "VMManager.h"
#include "RawDiskDevice.h"
#include "IStorageDevice.h"
#include "ISO9660Parser.h"
#include "FATParser.h"
#include "PEParser.h"
#include "TestKernelHandler.h"
#include "IA64EfiHandoffLayout.h"
#include "memory.h"
#include "IA64ISAPlugin.h"
#include "logger.h"
#include "BootStageTrace.h"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_set>
#include <stdexcept>
#include <vector>

namespace ia64 {

namespace {
void DrawBootStatus(VMInstance* instance,
                    const char* line1,
                    const char* line2 = nullptr,
                    const char* line3 = nullptr,
                    uint32_t titleColor = 0xFF6EE7B7) {
    if (!instance || !instance->vm || !instance->vm->getFramebufferDevice()) {
        return;
    }

    FramebufferDevice* fb = instance->vm->getFramebufferDevice();
    // Use a clearly visible dark-blue background (not near-black)
    fb->Clear(0xFF1A1A3E);
    // Title: bright cyan/green
    fb->DrawText(32, 20, "GUIDEXOS IA-64 HYPERVISOR", titleColor, 2);
    // Separator line (draw as a row of dashes)
    fb->DrawText(32, 46, "------------------------", 0xFF505070, 2);
    if (line1) {
        fb->DrawText(32, 72, line1, 0xFFFFFFFF, 2);
    }
    if (line2) {
        fb->DrawText(32, 100, line2, 0xFFFFD166, 2);
    }
    if (line3) {
        fb->DrawText(32, 128, line3, 0xFF93C5FD, 2);
    }
    // Footer hint
    fb->DrawText(32, 460, "CHECK LOGS FOR DETAILS", 0xFF606060, 1);
}

IA64ISAPlugin* getActiveIa64Plugin(VMInstance* instance) {
    if (!instance || !instance->vm) {
        return nullptr;
    }

    CPU& cpu = instance->vm->getCPU();
    return dynamic_cast<IA64ISAPlugin*>(cpu.getISAPlugin());
}

std::string previewBytes(IMemory& memory, uint64_t address, size_t count);
std::string previewBytesRaw(const IMemory& memory, uint64_t address, size_t count);
std::string previewUtf16(IMemory& memory, uint64_t address);
std::string describeEfiDevicePath(IMemory& memory, uint64_t address);

bool shouldSeedLoaderLocalUtf16Probe() {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t valueLen = 0;
    if (_dupenv_s(&value, &valueLen, "GUIDEXOS_EFI_DIAG_SEED_LOADER_LOCAL_5E018") != 0 ||
        value == nullptr || *value == '\0') {
        if (value) {
            free(value);
        }
        return false;
    }
    const std::string envValue(value);
    free(value);
#else
    const char* rawValue = std::getenv("GUIDEXOS_EFI_DIAG_SEED_LOADER_LOCAL_5E018");
    if (!rawValue || *rawValue == '\0') {
        return false;
    }
    const std::string envValue(rawValue);
#endif
    return envValue != "0" &&
           envValue != "false" &&
           envValue != "off" &&
           envValue != "no";
}

void seedLoaderLocalUtf16Probe(VMInstance* instance) {
    if (!instance || !instance->vm || !shouldSeedLoaderLocalUtf16Probe()) {
        return;
    }

    static constexpr uint64_t kLoaderLocalProbeAddress = 0x5E018ULL;
    static const uint16_t kSeededPath[] = {
        '\\','E','F','I','\\','B','O','O','T','\\',
        'B','O','O','T','I','A','6','4','.','E','F','I',0
    };

    auto& memory = instance->vm->getMemory();
    const uint64_t seedEnd = kLoaderLocalProbeAddress + sizeof(kSeededPath);
    if (seedEnd > memory.GetTotalSize()) {
        LOG_WARN("[EFI-DIAG] Requested loader-local CHAR16 seed does not fit in guest memory");
        return;
    }

    memory.Write(kLoaderLocalProbeAddress,
                 reinterpret_cast<const uint8_t*>(kSeededPath),
                 sizeof(kSeededPath));

    std::ostringstream oss;
    oss << "[EFI-DIAG] Seeded loader-local CHAR16 buffer"
        << " addr=0x" << std::hex << kLoaderLocalProbeAddress
        << " bytes=" << previewUtf16(memory, kLoaderLocalProbeAddress);
    LOG_INFO(oss.str());
    BootStageTrace::Event("EFI_DIAG_SEED_LOADER_LOCAL_UTF16",
                          "addr=" + BootStageTrace::Hex(kLoaderLocalProbeAddress) +
                          " text=\"\\EFI\\BOOT\\BOOTIA64.EFI\"");
}

std::string getDiagLoadOptionsSeedText() {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t valueLen = 0;
    if (_dupenv_s(&value, &valueLen, "GUIDEXOS_EFI_DIAG_SEED_LOAD_OPTIONS") != 0 ||
        value == nullptr || *value == '\0') {
        if (value) {
            free(value);
        }
        return {};
    }
    const std::string envValue(value);
    free(value);
#else
    const char* rawValue = std::getenv("GUIDEXOS_EFI_DIAG_SEED_LOAD_OPTIONS");
    if (!rawValue || *rawValue == '\0') {
        return {};
    }
    const std::string envValue(rawValue);
#endif
    if (envValue == "0" ||
        envValue == "false" ||
        envValue == "False" ||
        envValue == "off" ||
        envValue == "Off" ||
        envValue == "no" ||
        envValue == "No") {
        return {};
    }
    if (envValue == "1" ||
        envValue == "true" ||
        envValue == "True" ||
        envValue == "on" ||
        envValue == "On" ||
        envValue == "yes" ||
        envValue == "Yes") {
        return "elilo-forced";
    }
    return envValue;
}

bool shouldEnableGpRelativeDataDiag() {
    return !getDiagLoadOptionsSeedText().empty();
}

void installGpRelativeDataWriteTrace(VMInstance* instance, const char* phase) {
    if (!instance || !instance->vm || !phase || !shouldEnableGpRelativeDataDiag()) {
        return;
    }

    auto* memory = dynamic_cast<Memory*>(&instance->vm->getMemory());
    if (!memory) {
        return;
    }

    static std::mutex tracedMutex;
    static std::unordered_set<const VirtualMachine*> tracedVms;
    {
        std::lock_guard<std::mutex> lock(tracedMutex);
        if (!tracedVms.insert(instance->vm.get()).second) {
            return;
        }
    }

    struct RangeSpec {
        const char* label;
        uint64_t start;
        uint64_t end;
    };

    const std::vector<RangeSpec> watchedRanges = {
        {"list_heads", 0x382D0ULL, 0x382E0ULL},
        {"gp_targets", 0x3D050ULL, 0x3D060ULL},
        {"common_options", 0x3FB80ULL, 0x40100ULL},
        {"runtime_ptr_12f000", 0x12F000ULL, 0x12F200ULL},
        {"runtime_ptr_137060", 0x137060ULL, 0x137200ULL},
    };

    std::ostringstream install;
    install << "[EFI-RANGE-DIAG] installed exact watches phase=\"" << phase << "\"";
    for (const auto& range : watchedRanges) {
        install << " " << range.label << "=0x" << std::hex << range.start
                << "..0x" << range.end;
    }
    LOG_INFO(install.str());

    auto formatBytes = [](uint64_t start, uint64_t end, const uint8_t* raw) {
        const uint64_t count = end > start ? end - start : 0;
        const uint64_t limit = std::min<uint64_t>(count, 32ULL);
        std::ostringstream bytes;
        bytes << std::hex << std::setfill('0');
        for (uint64_t i = 0; i < limit; ++i) {
            if (i != 0) {
                bytes << ' ';
            }
            bytes << std::setw(2)
                  << static_cast<unsigned>(raw[static_cast<size_t>(start + i)]);
        }
        if (count > limit) {
            if (limit != 0) {
                bytes << ' ';
            }
            bytes << "...";
        }
        return bytes.str();
    };

    for (const auto& range : watchedRanges) {
        memory->GetMMU().RegisterReadHook(
            [instance,
             memory,
             phaseLabel = std::string(phase),
             range,
             formatBytes](HookContext& context, uint8_t* /*data*/) {
                const uint64_t readEnd = context.address + static_cast<uint64_t>(context.size);
                if (readEnd <= range.start || context.address >= range.end) {
                    return;
                }

                const uint64_t overlapStart = std::max(context.address, range.start);
                const uint64_t overlapEnd = std::min(readEnd, range.end);
                if (overlapStart >= overlapEnd) {
                    return;
                }

                const uint8_t* raw = memory->GetRawData();
                std::ostringstream trace;
                trace << "[EFI-RANGE-READ] phase=\"" << phaseLabel << "\""
                      << " label=\"" << range.label << "\""
                      << " ip=0x" << std::hex << instance->vm->getIP()
                      << " address=0x" << context.address
                      << " size=0x" << context.size
                      << " overlap=0x" << overlapStart << "..0x" << overlapEnd
                      << " bytes=" << formatBytes(overlapStart, overlapEnd, raw)
                      << std::dec;
                std::cout << trace.str() << std::endl;
            });

        memory->GetMMU().RegisterWriteHook(
            [instance,
             memory,
             phaseLabel = std::string(phase),
             range,
             formatBytes](HookContext& context, uint8_t* data) {
                const uint64_t writeEnd = context.address + static_cast<uint64_t>(context.size);
                if (writeEnd <= range.start || context.address >= range.end) {
                    return;
                }

                const uint64_t overlapStart = std::max(context.address, range.start);
                const uint64_t overlapEnd = std::min(writeEnd, range.end);
                if (overlapStart >= overlapEnd) {
                    return;
                }

                const uint8_t* raw = memory->GetRawData();
                std::ostringstream beforeBytes;
                std::ostringstream afterBytes;
                beforeBytes << std::hex << std::setfill('0');
                afterBytes << std::hex << std::setfill('0');
                const uint64_t limit = std::min<uint64_t>(overlapEnd - overlapStart, 32ULL);
                for (uint64_t i = 0; i < limit; ++i) {
                    const uint64_t addr = overlapStart + i;
                    if (i != 0) {
                        beforeBytes << ' ';
                        afterBytes << ' ';
                    }
                    beforeBytes << std::setw(2)
                                << static_cast<unsigned>(raw[static_cast<size_t>(addr)]);
                    afterBytes << std::setw(2)
                               << static_cast<unsigned>(data[static_cast<size_t>(addr - context.address)]);
                }
                if (overlapEnd - overlapStart > limit) {
                    if (limit != 0) {
                        beforeBytes << ' ';
                        afterBytes << ' ';
                    }
                    beforeBytes << "...";
                    afterBytes << "...";
                }

                std::ostringstream trace;
                trace << "[EFI-RANGE-WRITE] phase=\"" << phaseLabel << "\""
                      << " label=\"" << range.label << "\""
                      << " ip=0x" << std::hex << instance->vm->getIP()
                      << " address=0x" << context.address
                      << " size=0x" << context.size
                      << " overlap=0x" << overlapStart << "..0x" << overlapEnd
                      << " before=" << beforeBytes.str()
                      << " after=" << afterBytes.str()
                      << std::dec;
                std::cout << trace.str() << std::endl;
            });
    }
}

void seedLoadedImageLoadOptions(VMInstance* instance,
                                uint64_t loadedImageProtocolAddr,
                                uint64_t loadOptionsAddr) {
    if (!instance || !instance->vm) {
        return;
    }

    const std::string seedText = getDiagLoadOptionsSeedText();
    if (seedText.empty()) {
        return;
    }

    auto& memory = instance->vm->getMemory();
    std::vector<uint16_t> utf16(seedText.size() + 1U, 0U);
    for (size_t i = 0; i < seedText.size(); ++i) {
        utf16[i] = static_cast<uint8_t>(seedText[i]);
    }

    const uint64_t seedBytes = static_cast<uint64_t>(utf16.size() * sizeof(uint16_t));
    if (loadOptionsAddr + seedBytes > memory.GetTotalSize()) {
        LOG_WARN("[EFI-DIAG] Requested LoadedImage.LoadOptions seed does not fit in guest memory");
        return;
    }

    memory.Write(loadOptionsAddr,
                 reinterpret_cast<const uint8_t*>(utf16.data()),
                 static_cast<size_t>(seedBytes));
    const uint32_t loadOptionsSizeBytes =
        static_cast<uint32_t>(seedText.size() * sizeof(uint16_t));
    memory.Write(loadedImageProtocolAddr + 0x30,
                 reinterpret_cast<const uint8_t*>(&loadOptionsSizeBytes),
                 sizeof(loadOptionsSizeBytes));
    memory.Write(loadedImageProtocolAddr + 0x38,
                 reinterpret_cast<const uint8_t*>(&loadOptionsAddr),
                 sizeof(loadOptionsAddr));

    std::ostringstream oss;
    oss << "[EFI-DIAG] Seeded LoadedImage.LoadOptions"
        << " addr=0x" << std::hex << loadOptionsAddr
        << " size=0x" << loadOptionsSizeBytes
        << " text=\"" << seedText << "\""
        << " utf16Seed=\"" << seedText << "\"";
    LOG_INFO(oss.str());
    BootStageTrace::Event("EFI_DIAG_SEED_LOAD_OPTIONS",
                          "addr=" + BootStageTrace::Hex(loadOptionsAddr) +
                          " size=" + BootStageTrace::Hex(loadOptionsSizeBytes) +
                          " text=\"" + seedText + "\"");
}

void installEfiBootMetadataWriteTrace(VMInstance* instance,
                                      const char* phase,
                                      uint64_t metadataAddr) {
    if (!instance || !instance->vm || !phase) {
        return;
    }

    auto* memory = dynamic_cast<Memory*>(&instance->vm->getMemory());
    if (!memory) {
        return;
    }

    static std::mutex tracedMutex;
    static std::unordered_set<const VirtualMachine*> tracedVms;
    {
        std::lock_guard<std::mutex> lock(tracedMutex);
        if (!tracedVms.insert(instance->vm.get()).second) {
            return;
        }
    }

    constexpr uint64_t EFI_BOOT_IMAGE_METADATA_SIZE = 24ULL;
    auto firstHit = std::make_shared<bool>(false);

    memory->GetMMU().RegisterWriteHook(
        [instance,
         phaseLabel = std::string(phase),
         firstHit,
         metadataAddr,
         metadataSize = EFI_BOOT_IMAGE_METADATA_SIZE](HookContext& context, uint8_t* data) {
            const uint64_t writeEnd = context.address + static_cast<uint64_t>(context.size);
            const uint64_t metadataEnd = metadataAddr + metadataSize;
            if (writeEnd <= metadataAddr || context.address >= metadataEnd) {
                return;
            }

            if (*firstHit) {
                return;
            }
            *firstHit = true;

            const uint64_t overlapStart = std::max(context.address, metadataAddr);
            const uint64_t overlapEnd = std::min(writeEnd, metadataEnd);
            std::ostringstream bytes;
            bytes << std::hex << std::setfill('0');
            for (uint64_t addr = overlapStart; addr < overlapEnd; ++addr) {
                if (addr != overlapStart) {
                    bytes << ' ';
                }
                bytes << std::setw(2)
                      << static_cast<unsigned>(data[static_cast<size_t>(addr - context.address)]);
            }

            std::cout << "[EFI-MILESTONE] " << phaseLabel
                      << " first metadata write observed"
                      << " ip=0x" << std::hex << instance->vm->getIP()
                      << " address=0x" << context.address
                      << " size=0x" << context.size
                      << " overlapBytes=" << bytes.str()
                      << std::dec << std::endl;
            BootStageTrace::Event("EFI_BOOT_IMAGE_METADATA_WRITE",
                "phase=\"" + phaseLabel + "\" ip=" + BootStageTrace::Hex(instance->vm->getIP()) +
                " address=" + BootStageTrace::Hex(context.address) +
                " size=" + BootStageTrace::Hex(static_cast<uint64_t>(context.size)) +
                " overlapBytes=" + bytes.str());
        });
}

void installEfiLoadedImageReadTrace(VMInstance* instance,
                                    const char* phase,
                                    uint64_t loadedImageAddress,
                                    uint64_t filePathAddress,
                                    uint64_t filePathSize) {
    if (!instance || !instance->vm || !phase) {
        return;
    }

    auto* memory = dynamic_cast<Memory*>(&instance->vm->getMemory());
    if (!memory) {
        return;
    }

    static std::mutex tracedMutex;
    static std::unordered_set<const VirtualMachine*> tracedVms;
    {
        std::lock_guard<std::mutex> lock(tracedMutex);
        if (!tracedVms.insert(instance->vm.get()).second) {
            return;
        }
    }

    constexpr uint64_t EFI_LOADED_IMAGE_PROTOCOL_SIZE = 0x70ULL;
    auto loadedImageFirstHit = std::make_shared<bool>(false);
    auto filePathFirstHit = std::make_shared<bool>(false);

    memory->GetMMU().RegisterReadHook(
        [instance,
         memory,
         phaseLabel = std::string(phase),
         loadedImageAddress,
         loadedImageSize = EFI_LOADED_IMAGE_PROTOCOL_SIZE,
         loadedImageFirstHit,
         filePathAddress,
         filePathSize,
         filePathFirstHit](HookContext& context, uint8_t* /*data*/) {
            const uint64_t readEnd = context.address + static_cast<uint64_t>(context.size);
            const uint64_t loadedImageEnd = loadedImageAddress + loadedImageSize;
            const uint64_t filePathEnd = filePathAddress + filePathSize;
            const bool overlapsLoadedImage =
                !(readEnd <= loadedImageAddress || context.address >= loadedImageEnd);
            const bool overlapsFilePath =
                filePathAddress != 0 &&
                !(readEnd <= filePathAddress || context.address >= filePathEnd);

            if (!overlapsLoadedImage && !overlapsFilePath) {
                return;
            }

            const bool shouldLogLoadedImage = overlapsLoadedImage && !*loadedImageFirstHit;
            const bool shouldLogFilePath = overlapsFilePath && !*filePathFirstHit;
            if (!shouldLogLoadedImage && !shouldLogFilePath) {
                return;
            }

            if (shouldLogLoadedImage) {
                *loadedImageFirstHit = true;
            }
            if (shouldLogFilePath) {
                *filePathFirstHit = true;
            }

            std::ostringstream trace;
            trace << "phase=\"" << phaseLabel << "\""
                  << " ip=0x" << std::hex << instance->vm->getIP()
                  << " address=0x" << context.address
                  << " size=0x" << context.size
                  << std::dec;
            if (shouldLogLoadedImage) {
                trace << " field=LoadedImageProtocol";
                uint64_t loadedImageFilePath = 0;
                try {
                    memory->Read(loadedImageAddress + 0x20,
                                 reinterpret_cast<uint8_t*>(&loadedImageFilePath),
                                 sizeof(loadedImageFilePath));
                } catch (const std::exception&) {
                    loadedImageFilePath = 0;
                }
                trace << " loadedImageFilePath=0x" << std::hex << loadedImageFilePath
                      << " loadedImageBytes=" << previewBytesRaw(*memory, loadedImageAddress, loadedImageSize);
            }
            if (shouldLogFilePath) {
                trace << " field=LoadedImageFilePath";
                trace << " filePathBytes="
                      << previewBytesRaw(*memory, filePathAddress, filePathSize);
            }

            std::cout << "[EFI-READ-TRACE] " << trace.str() << std::endl;
            BootStageTrace::Event("EFI_LOADED_IMAGE_READ", trace.str());
        });
}

uint64_t SetupMinimalEfiStack(VMInstance* instance, std::ostringstream& oss) {
    constexpr uint64_t EFI_STACK_SIZE = 0x40000ULL;   // 256 KiB
    constexpr uint64_t EFI_STACK_GUARD = 0x10000ULL;  // leave the top page range unused

    if (!instance || !instance->vm) {
        return 0;
    }

    const uint64_t memorySize = static_cast<uint64_t>(instance->vm->getMemory().GetTotalSize());
    if (memorySize <= EFI_STACK_SIZE + EFI_STACK_GUARD) {
        LOG_WARN("  Guest memory too small for EFI stack setup");
        return 0;
    }

    const uint64_t stackTop = (memorySize - EFI_STACK_GUARD) & ~15ULL;
    const uint64_t stackBase = stackTop - EFI_STACK_SIZE;
    const uint64_t zero = 0;

    // Touch the bottom and top of the stack window so bounds/MMU problems are
    // diagnosed during setup instead of as a host assertion later.
    instance->vm->getMemory().Write(stackBase, reinterpret_cast<const uint8_t*>(&zero), sizeof(zero));
    instance->vm->getMemory().Write(stackTop - sizeof(zero), reinterpret_cast<const uint8_t*>(&zero), sizeof(zero));
    instance->vm->writeGR(0, 12, stackTop);

    oss.str("");
    oss << "  EFI stack: 0x" << std::hex << stackBase << "-0x" << stackTop
        << " (r12=0x" << stackTop << ")" << std::dec;
    LOG_INFO(oss.str());

    return stackTop;
}

void WriteIa64Bundle(VMInstance* instance,
                     uint64_t address,
                     uint8_t templateId,
                     uint64_t slot0,
                     uint64_t slot1,
                     uint64_t slot2) {
    const uint64_t lo = static_cast<uint64_t>(templateId & 0x1F) |
                        ((slot0 & 0x1FFFFFFFFFFULL) << 5) |
                        ((slot1 & 0x3FFFFULL) << 46);
    const uint64_t hi = ((slot1 >> 18) & 0x7FFFFFULL) |
                        ((slot2 & 0x1FFFFFFFFFFULL) << 23);

    uint8_t bundle[16] = {};
    std::memcpy(bundle, &lo, sizeof(lo));
    std::memcpy(bundle + sizeof(lo), &hi, sizeof(hi));
    instance->vm->getMemory().Write(address, bundle, sizeof(bundle));
}

std::string previewBytes(IMemory& memory, uint64_t address, size_t count) {
    if (address == 0 || count == 0) {
        return {};
    }

    std::ostringstream bytes;
    bytes << std::hex;
    for (size_t i = 0; i < count; ++i) {
        uint8_t value = 0;
        try {
            memory.Read(address + i, &value, sizeof(value));
        } catch (const std::exception&) {
            if (i == 0) {
                return "<unreadable>";
            }
            bytes << " ...";
            break;
        }
        if (i != 0) {
            bytes << ' ';
        }
        if (value < 0x10) {
            bytes << '0';
        }
        bytes << static_cast<unsigned>(value);
    }
    return bytes.str();
}

std::string previewBytesRaw(const IMemory& memory, uint64_t address, size_t count) {
    if (address == 0 || count == 0) {
        return {};
    }

    const uint8_t* raw = memory.GetRawData();
    if (!raw) {
        return "<unreadable>";
    }

    const uint64_t totalSize = memory.GetTotalSize();
    if (address >= totalSize) {
        return "<unreadable>";
    }

    const uint64_t limit = std::min<uint64_t>(count, totalSize - address);
    std::ostringstream bytes;
    bytes << std::hex;
    for (uint64_t i = 0; i < limit; ++i) {
        if (i != 0) {
            bytes << ' ';
        }
        const int value = raw[static_cast<size_t>(address + i)];
        if (value < 0x10) {
            bytes << '0';
        }
        bytes << value;
    }
    if (limit < count) {
        if (limit != 0) {
            bytes << ' ';
        }
        bytes << "...";
    }
    return bytes.str();
}

std::string previewUtf16(IMemory& memory, uint64_t address) {
    if (address == 0) {
        return {};
    }

    std::string text;
    for (size_t i = 0; i < 80; ++i) {
        uint16_t ch = 0;
        try {
            memory.Read(address + i * sizeof(ch), reinterpret_cast<uint8_t*>(&ch), sizeof(ch));
        } catch (const std::exception&) {
            if (text.empty()) {
                return "<unreadable>";
            }
            break;
        }

        if (ch == 0) {
            break;
        }
        if (ch >= 0x20 && ch <= 0x7E) {
            text.push_back(static_cast<char>(ch));
        } else {
            text.push_back('?');
        }
    }

    return text;
}

std::string describeEfiDevicePath(IMemory& memory, uint64_t address) {
    if (address == 0) {
        return "null";
    }

    std::ostringstream oss;
    oss << "0x" << std::hex << address << " nodes=[";
    uint64_t nodeAddress = address;
    for (size_t nodeIndex = 0; nodeIndex < 8; ++nodeIndex) {
        uint8_t type = 0;
        uint8_t subtype = 0;
        uint16_t length = 0;
        try {
            memory.Read(nodeAddress, &type, sizeof(type));
            memory.Read(nodeAddress + 1, &subtype, sizeof(subtype));
            memory.Read(nodeAddress + 2, reinterpret_cast<uint8_t*>(&length), sizeof(length));
        } catch (const std::exception&) {
            if (nodeIndex != 0) {
                oss << ' ';
            }
            oss << "<unreadable@0x" << nodeAddress << ">";
            break;
        }

        if (nodeIndex != 0) {
            oss << ' ';
        }
        oss << "{type=0x" << std::hex << static_cast<unsigned>(type)
            << ",sub=0x" << static_cast<unsigned>(subtype)
            << ",len=0x" << length;
        if (type == 0x04 && subtype == 0x04 && length >= 4) {
            const std::string path = previewUtf16(memory, nodeAddress + 4);
            if (!path.empty()) {
                oss << ",path=\"" << path << "\"";
            }
        }
        oss << "}";

        if (type == 0x7F && subtype == 0xFF) {
            break;
        }
        if (length < 4) {
            oss << " <bad-length>";
            break;
        }
        nodeAddress += length;
    }
    oss << "]";
    return oss.str();
}
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

VMManager::VMManager()
    : vms_()
    , snapshots_()
    , defaultConfig_(VMConfiguration::createStandard("default"))
    , vmCounter_(0)
    , snapshotCounter_(0) {
    LOG_INFO("VMManager initialized");
}

VMManager::~VMManager() {
    LOG_INFO("VMManager shutting down...");
    stopAllVMs();
    vms_.clear();
    LOG_INFO("VMManager destroyed");
}

// ============================================================================
// VM Creation and Configuration
// ============================================================================

std::string VMManager::createVM(const VMConfiguration& config) {
    // Validate configuration
    std::string validationError;
    if (!config.validate(&validationError)) {
        LOG_ERROR("VM configuration validation failed: " + validationError);
        return "";
    }
    
    // Generate VM ID
    std::string vmId = generateVMId(config.name);
    
    LOG_INFO("Creating VM: " + config.name + " (ID: " + vmId + ")");
    
    try {
        // Create VM instance
        auto instance = std::make_unique<VMInstance>();
        
        // Initialize metadata
        instance->metadata.vmId = vmId;
        instance->metadata.name = config.name;
        instance->metadata.uuid = config.uuid.empty() ? vmId : config.uuid;
        instance->metadata.configuration = config;
        instance->metadata.setState(VMState::CREATED, "VM created");
        
        // Create VirtualMachine
        instance->vm = std::make_unique<VirtualMachine>(
            config.memory.memorySize,
            config.cpu.cpuCount,
            config.cpu.isaType
        );
        
        // Create storage devices
        if (!createStorageDevices(instance.get(), config.storageDevices)) {
            LOG_ERROR("Failed to create storage devices for VM: " + vmId);
            return "";
        }
        
        // Initialize VM
        if (!instance->vm->init()) {
            LOG_ERROR("Failed to initialize VM: " + vmId);
            return "";
        }
        
        instance->metadata.setState(VMState::STOPPED, "VM ready to start");
        
        // Store VM
        vms_[vmId] = std::move(instance);
        
        LOG_INFO("VM created successfully: " + vmId);
        return vmId;
        
    } catch (const std::exception& e) {
        LOG_ERROR("Exception creating VM: " + std::string(e.what()));
        return "";
    }
}

bool VMManager::deleteVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    // Cannot delete running VM
    if (instance->metadata.currentState == VMState::RUNNING ||
        instance->metadata.currentState == VMState::PAUSED) {
        LOG_ERROR("Cannot delete running VM: " + vmId);
        return false;
    }
    
    LOG_INFO("Deleting VM: " + vmId);
    
    // Delete snapshots
    snapshots_.erase(vmId);
    
    // Delete VM
    vms_.erase(vmId);
    
    LOG_INFO("VM deleted: " + vmId);
    return true;
}

VMConfiguration VMManager::getVMConfiguration(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMConfiguration();
    }
    return instance->metadata.configuration;
}

bool VMManager::updateVMConfiguration(const std::string& vmId, 
                                     const VMConfiguration& config) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    // VM must be stopped to update configuration
    if (instance->metadata.currentState != VMState::STOPPED) {
        LOG_ERROR("VM must be stopped to update configuration: " + vmId);
        return false;
    }
    
    // Validate new configuration
    std::string validationError;
    if (!config.validate(&validationError)) {
        LOG_ERROR("Configuration validation failed: " + validationError);
        return false;
    }
    
    instance->metadata.configuration = config;
    instance->metadata.modifiedTime = std::chrono::system_clock::now();
    
    LOG_INFO("VM configuration updated: " + vmId);
    return true;
}

// ============================================================================
// VM Lifecycle Management
// ============================================================================

bool VMManager::startVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canStart()) {
        LOG_ERROR("Cannot start VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Starting VM: " + vmId);
    
    try {
        instance->metadata.setState(VMState::STARTING, "Starting VM");
        
        // Reset VM if it was terminated
        if (instance->metadata.previousState == VMState::TERMINATED) {
            if (!instance->vm->reset()) {
                instance->metadata.setError("Failed to reset VM");
                return false;
            }
        }
        
        // Load bootloader from boot device if configured
        const auto& bootConfig = instance->metadata.configuration.boot;
        const uint64_t guestMemorySize =
            static_cast<uint64_t>(instance->vm->getMemory().GetTotalSize());
        EfiHandoffLayout efiLayout{};
        bool efiLayoutReady = false;
        auto ensureEfiLayout = [&]() -> const EfiHandoffLayout& {
            if (!efiLayoutReady) {
                if (!tryComputeEfiHandoffLayout(guestMemorySize, efiLayout)) {
                    std::ostringstream error;
                    error << "Guest memory too small for IA-64 EFI handoff region"
                          << " guestMemorySize=0x" << std::hex << guestMemorySize;
                    LOG_ERROR(error.str());
                    throw std::runtime_error(error.str());
                }
                efiLayoutReady = true;
                std::ostringstream layoutLog;
                layoutLog << "[EFI-MILESTONE] EFI handoff layout"
                          << " base=0x" << std::hex << efiLayout.base
                          << " end=0x" << efiLayout.end
                          << " loadedImage=0x" << efiLayout.loadedImageProtocolAddr
                          << " filePath=0x" << efiLayout.loadedImageFilePathAddr
                          << " loadOptions=0x" << efiLayout.loadedImageLoadOptionsAddr
                          << " bootImageMetadata=0x" << efiLayout.bootImageMetadataAddr
                          << " poolBase=0x" << efiLayout.poolBase
                          << " poolEnd=0x" << efiLayout.poolEnd
                          << " guestMemorySize=0x" << guestMemorySize;
                LOG_INFO(layoutLog.str());
            }
            return efiLayout;
        };
        
        // Handle direct boot mode (load kernel directly)
        if (bootConfig.directBoot && !bootConfig.kernelPath.empty()) {
            LOG_INFO("Direct boot mode enabled");
            LOG_INFO("  Kernel path: " + bootConfig.kernelPath);
            LOG_INFO("  Entry point: 0x" + std::to_string(bootConfig.entryPoint));
            
            // Load kernel file
            std::ifstream kernelFile(bootConfig.kernelPath, std::ios::binary | std::ios::ate);
            if (!kernelFile) {
                LOG_ERROR("Failed to open kernel file: " + bootConfig.kernelPath);
                instance->metadata.setError("Kernel file not found");
                return false;
            }
            
            // Get file size
            std::streamsize kernelSize = kernelFile.tellg();
            kernelFile.seekg(0, std::ios::beg);
            
            // Read kernel into memory
            std::vector<uint8_t> kernelData(kernelSize);
            if (!kernelFile.read(reinterpret_cast<char*>(kernelData.data()), kernelSize)) {
                LOG_ERROR("Failed to read kernel file");
                instance->metadata.setError("Failed to read kernel");
                return false;
            }
            
            LOG_INFO("  Kernel size: " + std::to_string(kernelSize) + " bytes");
            
            // Check if this is a test kernel
            if (TestKernelHandler::IsTestKernel(kernelData.data(), kernelData.size())) {
                LOG_INFO("? Detected test kernel");
                
                // Get framebuffer from VM (80x25 VGA text mode)
                uint16_t* framebuffer = instance->vm->getFramebuffer();
                if (framebuffer) {
                    if (TestKernelHandler::ExecuteTestKernel(kernelData.data(), 
                                                             kernelData.size(), 
                                                             framebuffer)) {
                        LOG_INFO("? Test kernel executed successfully!");
                        LOG_INFO("  Framebuffer updated with test pattern");
                        
                        // Don't set entry point for test kernels - they're already "executed"
                        // The VM will just idle
                        instance->vm->setEntryPoint(0xFFFFFFFF); // Invalid entry point (halt)
                    } else {
                        LOG_ERROR("Test kernel execution failed");
                        instance->metadata.setError("Test kernel execution failed");
                        return false;
                    }
                } else {
                    LOG_ERROR("Framebuffer not available");
                    instance->metadata.setError("Framebuffer not available");
                    return false;
                }
            } else {
                // Regular kernel - load into memory
                uint64_t loadAddress = bootConfig.entryPoint != 0 ? 
                                      bootConfig.entryPoint : 0x100000;
                
                LOG_INFO("  Loading kernel to address: 0x" + std::to_string(loadAddress));
                
                if (instance->vm->loadProgram(kernelData.data(), kernelData.size(), loadAddress)) {
                    instance->vm->setEntryPoint(loadAddress);
                    LOG_INFO("? Kernel loaded successfully");
                } else {
                    LOG_ERROR("Failed to load kernel into memory");
                    instance->metadata.setError("Failed to load kernel");
                    return false;
                }
            }
        }
        // Load from boot device if not direct boot
        else if (!bootConfig.bootDevice.empty() && !bootConfig.directBoot) {
            // Find the boot device
            IStorageDevice* bootDevice = nullptr;
            for (const auto& device : instance->storageDevices) {
                if (device->getDeviceId() == bootConfig.bootDevice) {
                    bootDevice = device.get();
                    break;
                }
            }
            
            if (bootDevice) {
                LOG_INFO("Loading bootloader from device: " + bootConfig.bootDevice);
                
                // Try to parse as ISO 9660 filesystem
                LOG_INFO("Attempting to parse ISO 9660 filesystem...");
                ISO9660Parser isoParser(bootDevice);
                
                if (isoParser.parse()) {
                    LOG_INFO("? ISO filesystem parsed successfully");
                    
                    // Find El Torito boot catalog
                    if (isoParser.findBootCatalog()) {
                        LOG_INFO("? El Torito boot catalog found");
                        
                        // Get EFI boot entry
                        const BootEntryInfo* efiEntry = isoParser.getEFIBootEntry();

                        // Capture a boot-image backing store up front so both the
                        // direct EFI-entry path and the embedded-boot-image fallback
                        // can publish the same media for FileProtocol.Open.
                        const char* bootImgPaths[] = {
                            "/boot/boot.img",
                            "/BOOT/BOOT.IMG",
                            "boot/boot.img",
                            "BOOT/BOOT.IMG",
                            "/boot.img",
                            "/BOOT.IMG"
                        };
                        std::vector<uint8_t> bootImageBackingStoreData;
                        bool bootImageBackingStoreFound = false;
                        std::string bootImageBackingStorePath;
                            for (const char* path : bootImgPaths) {
                                if (isoParser.extractFile(path, bootImageBackingStoreData)) {
                                    LOG_INFO("? Found boot image: " + std::string(path));
                                    bootImageBackingStoreFound = true;
                                    bootImageBackingStorePath = path;
                                    break;
                                }
                            }
                            if (!bootImageBackingStoreFound) {
                                const BootEntryInfo* bootImageEntry = isoParser.getBootImageEntry();
                                if (bootImageEntry && bootImageEntry->isBootable) {
                                    LOG_INFO("  Boot catalog boot image at LBA " +
                                             std::to_string(bootImageEntry->loadLBA) +
                                             " size=" + std::to_string(bootImageEntry->sectorCount) +
                                             " sectors");
                                    if (isoParser.readFile(bootImageEntry->loadLBA,
                                                           bootImageEntry->sectorCount,
                                                           bootImageBackingStoreData) > 0 &&
                                        !bootImageBackingStoreData.empty()) {
                                        bootImageBackingStoreFound = true;
                                        bootImageBackingStorePath = "El Torito boot catalog";
                                        LOG_INFO("? Found boot image via El Torito catalog");
                                    }
                                }
                            }

                        if (efiEntry) {
                            LOG_INFO("? EFI boot entry found");
                            LOG_INFO("  Loading from LBA: " + std::to_string(efiEntry->loadLBA));
                            LOG_INFO("  Size: " + std::to_string(efiEntry->sectorCount) + " sectors");
                            
                            // Extract EFI executable from boot image (FAT filesystem)
                            std::vector<uint8_t> efiExecutable;
                            if (isoParser.extractEFIExecutable(efiExecutable)) {
                                LOG_INFO("? EFI executable extracted: " + 
                                        std::to_string(efiExecutable.size()) + " bytes");
                                
                                // Parse PE/COFF format
                                guideXOS::PEParser peParser;
                                if (peParser.parse(efiExecutable.data(), efiExecutable.size())) {
                                    LOG_INFO("? PE/COFF image parsed successfully");
                                    
                                    if (!peParser.isIA64()) {
                                        LOG_ERROR("EFI executable is not for IA-64 architecture");
                                    } else if (!peParser.isEFI()) {
                                        LOG_WARN("Executable may not be an EFI application");
                                    } else {
                                        // Load PE image properly
                                        std::vector<uint8_t> imageBuffer;
                                        uint64_t loadAddress, entryPoint;
                                        
                                        if (peParser.loadImage(imageBuffer, loadAddress, entryPoint)) {
                                            LOG_INFO("? PE image prepared for loading");
                                            LOG_INFO("  Preferred load address: 0x" + std::to_string(loadAddress));
                                            LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
                                            
                                            // Load into VM memory at preferred address
                                            if (instance->vm->loadProgram(imageBuffer.data(), 
                                                                         imageBuffer.size(), 
                                                                         loadAddress)) {
                                                seedLoaderLocalUtf16Probe(instance);
                                                // Set entry point from PE header
                                                instance->vm->setEntryPoint(entryPoint);
                                                LOG_INFO("? EFI bootloader loaded successfully");
                                                LOG_INFO("  Load address: 0x" + std::to_string(loadAddress));
                                                LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            } else {
                                                LOG_ERROR("Failed to load EFI bootloader into memory");
                                            }
                                        } else {
                                            LOG_ERROR("Failed to prepare PE image for loading");
                                        }
                                    }
                                } else {
                                    LOG_ERROR("Failed to parse EFI executable as PE/COFF");
                                }
                            } else {
                                LOG_ERROR("Failed to extract EFI executable from boot image");
                            }
                        } else {
                            LOG_WARN("No EFI boot entry found in El Torito boot catalog");
                            LOG_INFO("Attempting to find EFI bootloader in ISO filesystem...");
                            
                            // First, list root directory to see what's available
                            LOG_INFO("Listing root directory contents...");
                            isoParser.listRootDirectory();
                            
                            // Fallback: Search for BOOTIA64.EFI in the ISO filesystem
                            const char* efiPaths[] = {
                                "/EFI/BOOT/BOOTIA64.EFI",
                                "EFI/BOOT/BOOTIA64.EFI",
                                "/efi/boot/bootia64.efi",
                                "BOOTIA64.EFI",  // Try just the filename in case it's in root
                                "/BOOT/BOOTIA64.EFI"
                            };
                            
                            std::vector<uint8_t> efiExecutable;
                            bool foundEFI = false;
                            
                            for (const char* path : efiPaths) {
                                LOG_INFO("Searching for: " + std::string(path));
                                if (isoParser.extractFile(path, efiExecutable)) {
                                    LOG_INFO("? Found EFI bootloader: " + std::string(path));
                                    foundEFI = true;
                                    break;
                                }
                            }
                            
                            if (foundEFI) {
                                LOG_INFO("? EFI bootloader extracted: " + 
                                        std::to_string(efiExecutable.size()) + " bytes");
                                
                                // Parse PE/COFF format
                                guideXOS::PEParser peParser;
                                if (peParser.parse(efiExecutable.data(), efiExecutable.size())) {
                                    LOG_INFO("? PE/COFF image parsed successfully");
                                    
                                    if (!peParser.isIA64()) {
                                        LOG_ERROR("EFI executable is not for IA-64 architecture");
                                    } else if (!peParser.isEFI()) {
                                        LOG_WARN("Executable may not be an EFI application");
                                    } else {
                                        // Load PE image properly
                                        std::vector<uint8_t> imageBuffer;
                                        uint64_t loadAddress, entryPoint;
                                        
                                        if (peParser.loadImage(imageBuffer, loadAddress, entryPoint)) {
                                            LOG_INFO("? PE image prepared for loading");
                                            LOG_INFO("  Preferred load address: 0x" + std::to_string(loadAddress));
                                            LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));
                                            LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
                                            
                                            // Load into VM memory at preferred address
                                            if (instance->vm->loadProgram(imageBuffer.data(), 
                                                                         imageBuffer.size(), 
                                                                         loadAddress)) {
                                                seedLoaderLocalUtf16Probe(instance);
                                                // Set entry point from PE header
                                                instance->vm->setEntryPoint(entryPoint);
                                                LOG_INFO("? EFI bootloader loaded successfully from filesystem");
                                                LOG_INFO("  Load address: 0x" + std::to_string(loadAddress));
                                                LOG_INFO("  Entry point: 0x" + std::to_string(entryPoint));

                                                if (bootImageBackingStoreFound && !bootImageBackingStoreData.empty()) {
                                                    LOG_INFO("? Boot image extracted: " +
                                                             std::to_string(bootImageBackingStoreData.size()) + " bytes");
                                                    {
                                                        std::ostringstream ctx;
                                                        ctx << "sourcePath=\"" << bootImageBackingStorePath << "\""
                                                            << " bytes=" << bootImageBackingStoreData.size();
                                                        BootStageTrace::Stage(30, "EFI boot image extracted", ctx.str());
                                                    }
                                                    LOG_INFO("  Parsing as FAT filesystem...");

                                                    guideXOS::FATParser fatParser;
                                                    if (fatParser.parse(bootImageBackingStoreData.data(),
                                                                        bootImageBackingStoreData.size())) {
                                                        LOG_INFO("? FAT filesystem parsed successfully");
                                                        BootStageTrace::Stage(40, "FAT image mounted",
                                                                              "sourcePath=\"" + bootImageBackingStorePath + "\" bytes=" +
                                                                              std::to_string(bootImageBackingStoreData.size()));
                                                        const EfiHandoffLayout& layout = ensureEfiLayout();
                                                        constexpr uint64_t EFI_BOOT_IMAGE_METADATA_SIGNATURE = 0x42465441494D4645ULL;
                                                        constexpr uint64_t EFI_BOOT_IMAGE_GUEST_BASE = 0x18000000ULL;
                                                        const uint64_t memorySizeForBootImage =
                                                            guestMemorySize;
                                                        if (EFI_BOOT_IMAGE_GUEST_BASE + bootImageBackingStoreData.size() < memorySizeForBootImage &&
                                                            EFI_BOOT_IMAGE_GUEST_BASE + bootImageBackingStoreData.size() < layout.base) {
                                                            instance->vm->getMemory().Write(
                                                                EFI_BOOT_IMAGE_GUEST_BASE,
                                                                bootImageBackingStoreData.data(),
                                                                bootImageBackingStoreData.size());
                                                            instance->vm->getMemory().Write(
                                                                layout.bootImageMetadataAddr,
                                                                reinterpret_cast<const uint8_t*>(&EFI_BOOT_IMAGE_METADATA_SIGNATURE),
                                                                sizeof(EFI_BOOT_IMAGE_METADATA_SIGNATURE));
                                                            instance->vm->getMemory().Write(
                                                                layout.bootImageMetadataAddr + 8,
                                                                reinterpret_cast<const uint8_t*>(&EFI_BOOT_IMAGE_GUEST_BASE),
                                                                sizeof(EFI_BOOT_IMAGE_GUEST_BASE));
                                                            const uint64_t bootImageSize =
                                                                static_cast<uint64_t>(bootImageBackingStoreData.size());
                                                            instance->vm->getMemory().Write(
                                                                layout.bootImageMetadataAddr + 16,
                                                                reinterpret_cast<const uint8_t*>(&bootImageSize),
                                                                sizeof(bootImageSize));
                                                            IA64ISAPlugin* ia64Plugin = getActiveIa64Plugin(instance);
                                                            if (ia64Plugin) {
                                                                ia64Plugin->setBootImageBackingStore(bootImageBackingStoreData);
                                                            }
                                                            LOG_INFO("[EFI-MILESTONE] Published read-only EFI boot image backing store"
                                                                     " base=0x18000000 size=0x" +
                                                                     std::to_string(bootImageBackingStoreData.size()) +
                                                                     " metadata=0x" + BootStageTrace::Hex(layout.bootImageMetadataAddr));
                                                            {
                                                                uint64_t metadataSignature = 0;
                                                                uint64_t metadataBase = 0;
                                                                uint64_t metadataSize = 0;
                                                                instance->vm->getMemory().Read(
                                                                    layout.bootImageMetadataAddr,
                                                                    reinterpret_cast<uint8_t*>(&metadataSignature),
                                                                    sizeof(metadataSignature));
                                                                instance->vm->getMemory().Read(
                                                                    layout.bootImageMetadataAddr + 8,
                                                                    reinterpret_cast<uint8_t*>(&metadataBase),
                                                                    sizeof(metadataBase));
                                                                    instance->vm->getMemory().Read(
                                                                        layout.bootImageMetadataAddr + 16,
                                                                        reinterpret_cast<uint8_t*>(&metadataSize),
                                                                        sizeof(metadataSize));
                                                                    std::ostringstream trace;
                                                                    trace << "[EFI-MILESTONE] publish-readback boot image metadata"
                                                                          << " vm=0x" << std::hex << reinterpret_cast<uintptr_t>(instance->vm.get())
                                                                          << " cpu=0x" << reinterpret_cast<uintptr_t>(&instance->vm->getCPU())
                                                                          << " memoryRef=0x" << reinterpret_cast<uintptr_t>(&instance->vm->getMemory())
                                                                          << " memoryObj=0x" << reinterpret_cast<uintptr_t>(dynamic_cast<Memory*>(&instance->vm->getMemory()))
                                                                          << " plugin=0x" << reinterpret_cast<uintptr_t>(ia64Plugin)
                                                                          << " bootImageBytes=0x" << bootImageBackingStoreData.size()
                                                                          << " signature=0x" << metadataSignature
                                                                          << " base=0x" << metadataBase
                                                                          << " size=0x" << metadataSize
                                                                          << " addr=0x" << layout.bootImageMetadataAddr;
                                                                    LOG_INFO(trace.str());
                                                            }
                                                            installEfiBootMetadataWriteTrace(instance,
                                                                                             "boot-image publication",
                                                                                             layout.bootImageMetadataAddr);
                                                            BootStageTrace::Event("EFI_BOOT_IMAGE_BACKING_STORE",
                                                                "base=0x18000000 size=" +
                                                                BootStageTrace::Hex(static_cast<uint64_t>(bootImageBackingStoreData.size())) +
                                                                " metadata=" + BootStageTrace::Hex(layout.bootImageMetadataAddr));
                                                        } else {
                                                            LOG_WARN("[EFI-MILESTONE] EFI boot image backing store not published; FileProtocol will report EFI_NO_MEDIA");
                                                            BootStageTrace::Event("EFI_BOOT_IMAGE_BACKING_STORE",
                                                                "published=false base=0x18000000 size=" +
                                                                BootStageTrace::Hex(static_cast<uint64_t>(bootImageBackingStoreData.size())) +
                                                                " metadata=" + BootStageTrace::Hex(layout.bootImageMetadataAddr));
                                                        }
                                                    } else {
                                                        LOG_WARN("[EFI-MILESTONE] EFI boot image backing store present but FAT parse failed; "
                                                                 "FileProtocol may still report EFI_NO_MEDIA");
                                                        BootStageTrace::Event("EFI_BOOT_IMAGE_BACKING_STORE",
                                                            "published=false base=0x18000000 size=" +
                                                            BootStageTrace::Hex(static_cast<uint64_t>(bootImageBackingStoreData.size())) +
                                                            " metadata=" + BootStageTrace::Hex(ensureEfiLayout().bootImageMetadataAddr));
                                                    }
                                                }
                                            } else {
                                                LOG_ERROR("Failed to load EFI bootloader into memory");
                                            }
                                        } else {
                                            LOG_ERROR("Failed to prepare PE image for loading");
                                        }
                                    }
                                } else {
                                    LOG_ERROR("Failed to parse EFI executable as PE/COFF");
                                }
                            } else {
                                LOG_ERROR("Could not find EFI bootloader in ISO filesystem");
                                LOG_INFO("Checking for boot image with embedded EFI bootloader...");
                                
                                // Try to find BOOT.IMG and search for EFI bootloader inside it
                                const char* bootImgPaths[] = {
                                    "/boot/boot.img",
                                    "/BOOT/BOOT.IMG",
                                    "boot/boot.img",
                                    "BOOT/BOOT.IMG",
                                    "/boot.img",
                                    "/BOOT.IMG"
                                };
                                
                                std::vector<uint8_t> bootImgData;
                                bool foundBootImg = false;
                                std::string bootImgPath;
                                
                                for (const char* path : bootImgPaths) {
                                    if (isoParser.extractFile(path, bootImgData)) {
                                        LOG_INFO("? Found boot image: " + std::string(path));
                                        foundBootImg = true;
                                        bootImgPath = path;
                                        break;
                                    }
                                }
                                if (!foundBootImg && bootImageBackingStoreFound && !bootImageBackingStoreData.empty()) {
                                    bootImgData = bootImageBackingStoreData;
                                    foundBootImg = true;
                                    bootImgPath = bootImageBackingStorePath;
                                    LOG_INFO("? Reusing El Torito catalog boot image");
                                }
                                
                                bool bootedFromImage = false;
                                
                                if (foundBootImg) {
                                    LOG_INFO("? Boot image extracted: " + 
                                            std::to_string(bootImgData.size()) + " bytes");
                                    {
                                        std::ostringstream ctx;
                                        ctx << "sourcePath=\"" << bootImgPath << "\""
                                            << " bytes=" << bootImgData.size();
                                        BootStageTrace::Stage(30, "EFI boot image extracted", ctx.str());
                                    }
                                    LOG_INFO("  Parsing as FAT filesystem...");
                                    
                                    // Parse boot image as FAT filesystem
                                    guideXOS::FATParser fatParser;
                                    if (fatParser.parse(bootImgData.data(), bootImgData.size())) {
                                        LOG_INFO("? FAT filesystem parsed successfully");
                                        BootStageTrace::Stage(40, "FAT image mounted",
                                                              "sourcePath=\"" + bootImgPath + "\" bytes=" +
                                                              std::to_string(bootImgData.size()));
                                        
                                        // Search for EFI bootloader inside the FAT image
                                        const char* efiPathsInFAT[] = {
                                            "/EFI/BOOT/BOOTIA64.EFI",
                                            "EFI/BOOT/BOOTIA64.EFI",
                                            "/efi/boot/bootia64.efi",
                                            "efi/boot/bootia64.efi"
                                        };
                                        
                                        guideXOS::FATFileInfo efiFileInfo;
                                        std::string foundEFIPath;
                                        
                                        for (const char* path : efiPathsInFAT) {
                                            LOG_INFO("  Searching in boot image: " + std::string(path));
                                            if (fatParser.findFile(path, efiFileInfo)) {
                                                LOG_INFO("  ?? Found EFI bootloader in boot image!");
                                                foundEFIPath = path;
                                                std::ostringstream ctx;
                                                ctx << "path=\"" << foundEFIPath << "\""
                                                    << " size=" << efiFileInfo.size
                                                    << " firstCluster=" << efiFileInfo.firstCluster
                                                    << " sourceImage=\"" << bootImgPath << "\"";
                                                BootStageTrace::Stage(50, "BOOTIA64.EFI found", ctx.str());
                                                break;
                                            }
                                        }
                                        
                                        if (!foundEFIPath.empty()) {
                                            // Extract EFI bootloader from FAT image
                                            if (fatParser.readFile(efiFileInfo, efiExecutable)) {
                                                LOG_INFO("??? EFI bootloader extracted from boot image: " + 
                                                        std::to_string(efiExecutable.size()) + " bytes");
                                                
                                                // Parse and load the EFI executable
                                                guideXOS::PEParser peParser;
                                                if (peParser.parse(efiExecutable.data(), efiExecutable.size())) {
                                                    LOG_INFO("? PE/COFF image parsed successfully");
                                                    
                                                    if (!peParser.isIA64()) {
                                                        LOG_ERROR("EFI executable is not for IA-64 architecture");
                                                    } else if (!peParser.isEFI()) {
                                                        LOG_WARN("Executable may not be an EFI application");
                                                    } else {
                                                        // Load PE image properly
                                                        std::vector<uint8_t> imageBuffer;
                                                        uint64_t loadAddress, entryPoint;
                                                        
                                                        if (peParser.loadImage(imageBuffer, loadAddress, entryPoint)) {
                                                            LOG_INFO("? PE image prepared for loading");
                                                            
                                                            // Use ostringstream for proper hex formatting
                                                            std::ostringstream oss;
                                                            oss << "  Preferred load address: 0x" << std::hex << loadAddress << std::dec;
                                                            LOG_INFO(oss.str());
                                                            
                                                            oss.str("");
                                                            oss << "  Entry point: 0x" << std::hex << entryPoint << std::dec;
                                                            LOG_INFO(oss.str());
                                                            
                                                            LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
                                                            
                                                            // Load into VM memory
                                                                if (instance->vm->loadProgram(imageBuffer.data(), 
                                                                                         imageBuffer.size(), 
                                                                                         loadAddress)) {
                                                                    installGpRelativeDataWriteTrace(instance,
                                                                                                   "boot-image load");
                                                                    seedLoaderLocalUtf16Probe(instance);
                                                                    const auto& peInfo = peParser.getImageInfo();
                                                                    if (peInfo.hasGlobalPointer && loadAddress == 0 &&
                                                                        peInfo.globalPointer >= imageBuffer.size()) {
                                                                        const uint64_t ia64EfiImageAliasBase =
                                                                            guideXOS::ComputeIa64EfiAliasBase(peInfo);
                                                                    size_t mirroredSections = 0;
                                                                    size_t mirroredBytes = 0;
                                                                    for (const auto& section : peInfo.sections) {
                                                                        if ((section.characteristics & guideXOS::IMAGE_SCN_MEM_EXECUTE) != 0) {
                                                                            continue;
                                                                        }
                                                                        const uint64_t sectionSize = std::max(
                                                                            section.virtualSize,
                                                                            static_cast<uint64_t>(section.rawDataSize));
                                                                        if (sectionSize == 0 ||
                                                                            section.virtualAddress >= imageBuffer.size()) {
                                                                            continue;
                                                                        }
                                                                        const uint64_t copySize = std::min<uint64_t>(
                                                                            sectionSize,
                                                                            imageBuffer.size() - section.virtualAddress);
                                                                        instance->vm->getMemory().Write(
                                                                            ia64EfiImageAliasBase + section.virtualAddress,
                                                                            imageBuffer.data() + section.virtualAddress,
                                                                            static_cast<size_t>(copySize));
                                                                        ++mirroredSections;
                                                                        mirroredBytes += static_cast<size_t>(copySize);
                                                                    }
                                                                    oss.str("");
                                                                    oss << "[EFI-MILESTONE] Mirrored IA-64 EFI non-executable sections at 0x"
                                                                        << std::hex << ia64EfiImageAliasBase
                                                                        << "-0x" << (ia64EfiImageAliasBase + imageBuffer.size())
                                                                        << " while keeping entry=0x" << entryPoint
                                                                        << "; GP=0x" << peInfo.globalPointer
                                                                        << " maps GP-relative data into the mirror for load-base diagnosis; "
                                                                        << "excluded executable .text so bad GP-relative text probes stay zero"
                                                                        << std::dec
                                                                        << " mirroredSections=" << mirroredSections
                                                                        << " mirroredBytes=" << mirroredBytes
                                                                        << std::dec;
                                                                    LOG_WARN(oss.str());
                                                                }

                                                                instance->vm->setEntryPoint(entryPoint);
                                                                if (peInfo.hasGlobalPointer) {
                                                                    instance->vm->writeGR(0, 1, peInfo.globalPointer);
                                                                    oss.str("");
                                                                    oss << "  Global pointer (r1): 0x" << std::hex
                                                                        << peInfo.globalPointer << std::dec;
                                                                    LOG_INFO(oss.str());
                                                                }

                                                                // Set up minimal EFI firmware environment
                                                                // BOOTIA64.EFI entry: efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
                                                                // These are passed in r32 (in0) and r33 (in1) per IA-64 ABI.
                                                                // Without valid pointers, early EFI code (e.g. st8 [r33]=r16)
                                                                // corrupts address 0 (the loaded image itself).
                                                                // Keep firmware handoff stubs away from the loaded image
                                                                // and early bootloader scratch space. The IA-64 stack is
                                                                // placed near the top of memory with a guard, so this page
                                                                // remains below it while staying outside low guest RAM.
                                                                const EfiHandoffLayout& layout = ensureEfiLayout();
                                                                const uint64_t EFI_STUB_ADDR = layout.base;
                                                                constexpr uint64_t EFI_IMAGE_HANDLE = 0x1ULL;   // dummy non-null handle
                                                                constexpr uint64_t EFI_IMAGE_DEVICE_HANDLE = 0x40ULL; // boot media handle advertised by LocateHandle/SimpleFS

                                                                // Write a minimal EFI System Table plus service-table
                                                                // headers. This is only enough for userland EFI code
                                                                // to pass non-null service table probes; service calls
                                                                // still trap/fail through normal unsupported paths.
                                                                {
                                                                    const uint64_t EFI_RUNTIME_SERVICES_ADDR = layout.runtimeServicesAddr;
                                                                    const uint64_t EFI_BOOT_SERVICES_ADDR = layout.bootServicesAddr;
                                                                    const uint64_t EFI_FIRMWARE_VENDOR_ADDR = layout.firmwareVendorAddr;
                                                                    const uint64_t EFI_BOOT_IMAGE_METADATA_ADDR = layout.bootImageMetadataAddr;
                                                                    constexpr uint64_t EFI_BOOT_IMAGE_METADATA_SIGNATURE = 0x42465441494D4645ULL;
                                                                    constexpr uint64_t EFI_BOOT_IMAGE_GUEST_BASE = 0x18000000ULL;
                                                                    const uint64_t EFI_OPEN_VOLUME_STUB_CODE_ADDR = layout.openVolumeStubCodeAddr;
                                                                    const uint64_t EFI_OPEN_VOLUME_STUB_DESC_ADDR = layout.openVolumeStubDescAddr;
                                                                    const uint64_t EFI_LOADED_IMAGE_PROTOCOL_ADDR = layout.loadedImageProtocolAddr;
                                                                    const uint64_t EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR = layout.simpleFileSystemProtocolAddr;
                                                                    const uint64_t EFI_ROOT_FILE_PROTOCOL_ADDR = layout.rootFileProtocolAddr;
                                                                    const uint64_t EFI_TEXT_OUTPUT_PROTOCOL_ADDR = layout.textOutputProtocolAddr;
                                                                    const uint64_t EFI_TEXT_OUTPUT_MODE_ADDR = layout.textOutputModeAddr;
                                                                    const uint64_t EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR = layout.textOutputStringStubCodeAddr;
                                                                    const uint64_t EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR = layout.textOutputStringStubDescAddr;
                                                                    const uint64_t EFI_FILE_OPEN_STUB_CODE_ADDR = layout.fileOpenStubCodeAddr;
                                                                    const uint64_t EFI_FILE_OPEN_STUB_DESC_ADDR = layout.fileOpenStubDescAddr;
                                                                    const uint64_t EFI_FILE_CLOSE_STUB_CODE_ADDR = layout.fileCloseStubCodeAddr;
                                                                    const uint64_t EFI_FILE_CLOSE_STUB_DESC_ADDR = layout.fileCloseStubDescAddr;
                                                                    const uint64_t EFI_FILE_READ_STUB_CODE_ADDR = layout.fileReadStubCodeAddr;
                                                                    const uint64_t EFI_FILE_READ_STUB_DESC_ADDR = layout.fileReadStubDescAddr;
                                                                    const uint64_t EFI_FILE_GET_POSITION_STUB_CODE_ADDR = layout.fileGetPositionStubCodeAddr;
                                                                    const uint64_t EFI_FILE_GET_POSITION_STUB_DESC_ADDR = layout.fileGetPositionStubDescAddr;
                                                                    const uint64_t EFI_FILE_SET_POSITION_STUB_CODE_ADDR = layout.fileSetPositionStubCodeAddr;
                                                                    const uint64_t EFI_FILE_SET_POSITION_STUB_DESC_ADDR = layout.fileSetPositionStubDescAddr;
                                                                    const uint64_t EFI_FILE_GET_INFO_STUB_CODE_ADDR = layout.fileGetInfoStubCodeAddr;
                                                                    const uint64_t EFI_FILE_GET_INFO_STUB_DESC_ADDR = layout.fileGetInfoStubDescAddr;
                                                                    const uint64_t EFI_LOCATE_HANDLE_STUB_CODE_ADDR = layout.locateHandleStubCodeAddr;
                                                                    const uint64_t EFI_LOCATE_HANDLE_STUB_DESC_ADDR = layout.locateHandleStubDescAddr;
                                                                    const uint64_t EFI_LOCATE_PROTOCOL_STUB_CODE_ADDR = layout.locateProtocolStubCodeAddr;
                                                                    const uint64_t EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR = layout.locateProtocolStubDescAddr;
                                                                    const uint64_t EFI_GET_MEMORY_MAP_STUB_CODE_ADDR = layout.getMemoryMapStubCodeAddr;
                                                                    const uint64_t EFI_GET_MEMORY_MAP_STUB_DESC_ADDR = layout.getMemoryMapStubDescAddr;
                                                                    const uint64_t EFI_EXIT_BOOT_SERVICES_STUB_CODE_ADDR = layout.exitBootServicesStubCodeAddr;
                                                                    const uint64_t EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR = layout.exitBootServicesStubDescAddr;
                                                                    const uint64_t EFI_LOAD_IMAGE_STUB_CODE_ADDR = layout.loadImageStubCodeAddr;
                                                                    const uint64_t EFI_LOAD_IMAGE_STUB_DESC_ADDR = layout.loadImageStubDescAddr;
                                                                    const uint64_t EFI_START_IMAGE_STUB_CODE_ADDR = layout.startImageStubCodeAddr;
                                                                    const uint64_t EFI_START_IMAGE_STUB_DESC_ADDR = layout.startImageStubDescAddr;
                                                                    const uint64_t EFI_LOADED_IMAGE_FILE_PATH_ADDR = layout.loadedImageFilePathAddr;
                                                                    const uint64_t EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR = layout.loadedImageLoadOptionsAddr;
                                                                    const uint64_t EFI_GET_VARIABLE_STUB_CODE_ADDR = layout.getVariableStubCodeAddr;
                                                                    const uint64_t EFI_GET_VARIABLE_STUB_DESC_ADDR = layout.getVariableStubDescAddr;
                                                                    const uint64_t EFI_ALLOCATE_POOL_STUB_CODE_ADDR = layout.allocatePoolStubCodeAddr;
                                                                    const uint64_t EFI_ALLOCATE_POOL_STUB_DESC_ADDR = layout.allocatePoolStubDescAddr;
                                                                    const uint64_t EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR = layout.handleProtocolStubCodeAddr;
                                                                    const uint64_t EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR = layout.handleProtocolStubDescAddr;
                                                                    const uint64_t EFI_UNSUPPORTED_STUB_CODE_ADDR = layout.unsupportedStubCodeAddr;
                                                                    const uint64_t EFI_UNSUPPORTED_STUB_DESC_ADDR = layout.unsupportedStubDescAddr;
                                                                    const uint64_t EFI_SUCCESS_STUB_CODE_ADDR = layout.successStubCodeAddr;
                                                                    const uint64_t EFI_SUCCESS_STUB_DESC_ADDR = layout.successStubDescAddr;
                                                                    constexpr uint64_t EFI_TABLE_SIGNATURE = 0x5453595320494249ULL; // "IBI SYST"
                                                                    constexpr uint64_t EFI_BOOT_SERVICES_SIGNATURE = 0x56524553544F4F42ULL; // "BOOTSERV"
                                                                    constexpr uint64_t EFI_RUNTIME_SERVICES_SIGNATURE = 0x56524553544E5552ULL; // "RUNTSERV"

                                                                    static const uint8_t systemStub[0x1000] = {};
                                                                    instance->vm->getMemory().Write(EFI_STUB_ADDR, systemStub, sizeof(systemStub));
                                                                    static const uint8_t protocolStubPage[0x1000] = {};
                                                                    instance->vm->getMemory().Write(EFI_STUB_ADDR + 0x1000ULL,
                                                                        protocolStubPage,
                                                                        sizeof(protocolStubPage));

                                                                    auto write32 = [&](uint64_t address, uint32_t value) {
                                                                        instance->vm->getMemory().Write(address, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
                                                                    };
                                                                    auto write16 = [&](uint64_t address, uint16_t value) {
                                                                        instance->vm->getMemory().Write(address, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
                                                                    };
                                                                    auto write8 = [&](uint64_t address, uint8_t value) {
                                                                        instance->vm->getMemory().Write(address, &value, sizeof(value));
                                                                    };
                                                                    auto write64 = [&](uint64_t address, uint64_t value) {
                                                                        instance->vm->getMemory().Write(address, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
                                                                    };
                                                                    auto logBootImageMetadata = [&](const char* phase) {
                                                                        uint64_t metadataSignature = 0;
                                                                        uint64_t metadataBase = 0;
                                                                        uint64_t metadataSize = 0;
                                                                        instance->vm->getMemory().Read(EFI_BOOT_IMAGE_METADATA_ADDR,
                                                                            reinterpret_cast<uint8_t*>(&metadataSignature),
                                                                            sizeof(metadataSignature));
                                                                        instance->vm->getMemory().Read(EFI_BOOT_IMAGE_METADATA_ADDR + 8,
                                                                            reinterpret_cast<uint8_t*>(&metadataBase),
                                                                            sizeof(metadataBase));
                                                                        instance->vm->getMemory().Read(EFI_BOOT_IMAGE_METADATA_ADDR + 16,
                                                                            reinterpret_cast<uint8_t*>(&metadataSize),
                                                                            sizeof(metadataSize));
                                                                        LOG_INFO(std::string("[EFI-MILESTONE] ") + phase +
                                                                                 " boot image metadata signature=0x" +
                                                                                 BootStageTrace::Hex(metadataSignature) +
                                                                                 " base=0x" + BootStageTrace::Hex(metadataBase) +
                                                                                 " size=0x" + BootStageTrace::Hex(metadataSize) +
                                                                                 " addr=0x" + BootStageTrace::Hex(EFI_BOOT_IMAGE_METADATA_ADDR));
                                                                    };

                                                                    write64(EFI_STUB_ADDR + 0x00, EFI_TABLE_SIGNATURE);
                                                                    write32(EFI_STUB_ADDR + 0x08, 0x00010010U); // EFI 1.10-style revision
                                                                    write32(EFI_STUB_ADDR + 0x0C, 0x78U);       // 64-bit EFI_SYSTEM_TABLE size
                                                                    write64(EFI_STUB_ADDR + 0x18, EFI_FIRMWARE_VENDOR_ADDR);
                                                                    write32(EFI_STUB_ADDR + 0x20, 1U);

                                                                    // 64-bit EFI_SYSTEM_TABLE layout.
                                                                    write64(EFI_STUB_ADDR + 0x38, EFI_IMAGE_HANDLE);
                                                                    write64(EFI_STUB_ADDR + 0x40, EFI_TEXT_OUTPUT_PROTOCOL_ADDR);
                                                                    write64(EFI_STUB_ADDR + 0x48, EFI_IMAGE_HANDLE);
                                                                    write64(EFI_STUB_ADDR + 0x50, EFI_TEXT_OUTPUT_PROTOCOL_ADDR);
                                                                    write64(EFI_STUB_ADDR + 0x58, EFI_RUNTIME_SERVICES_ADDR);
                                                                    write64(EFI_STUB_ADDR + 0x60, EFI_BOOT_SERVICES_ADDR);
                                                                    write64(EFI_STUB_ADDR + 0x68, 0ULL); // NumberOfTableEntries
                                                                    write64(EFI_STUB_ADDR + 0x70, 0ULL);

                                                                    write64(EFI_RUNTIME_SERVICES_ADDR + 0x00, EFI_RUNTIME_SERVICES_SIGNATURE);
                                                                    write32(EFI_RUNTIME_SERVICES_ADDR + 0x08, 0x00010010U);
                                                                    write32(EFI_RUNTIME_SERVICES_ADDR + 0x0C, 0x88U);

                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x00, EFI_BOOT_SERVICES_SIGNATURE);
                                                                    write32(EFI_BOOT_SERVICES_ADDR + 0x08, 0x00010010U);
                                                                    write32(EFI_BOOT_SERVICES_ADDR + 0x0C, 0x180U);

                                                                    // IA-64 function pointers are descriptors: [code, gp].
                                                                    // Use one tiny callable error stub for unimplemented EFI
                                                                    // service entries so zero pointers do not branch through
                                                                    // the loaded PE header.
                                                                    constexpr uint64_t ADD_R8_R0_MINUS_1 = 0x13fffcfe200ULL;
                                                                    constexpr uint64_t ADD_R8_R0_ZERO = 0x12000000200ULL;
                                                                    constexpr uint64_t NOP_I = 0x0ULL;
                                                                    constexpr uint64_t BR_RET_B0 = 0x108000100ULL;
                                                                    WriteIa64Bundle(instance,
                                                                                   EFI_UNSUPPORTED_STUB_CODE_ADDR,
                                                                                   0x10,
                                                                                   ADD_R8_R0_MINUS_1,
                                                                                   NOP_I,
                                                                                   BR_RET_B0);
                                                                    WriteIa64Bundle(instance,
                                                                                   EFI_SUCCESS_STUB_CODE_ADDR,
                                                                                   0x10,
                                                                                   ADD_R8_R0_ZERO,
                                                                                   NOP_I,
                                                                                   BR_RET_B0);
                                                                    WriteIa64Bundle(instance,
                                                                                   EFI_ALLOCATE_POOL_STUB_CODE_ADDR,
                                                                                   0x10,
                                                                                   ADD_R8_R0_ZERO,
                                                                                   NOP_I,
                                                                                   BR_RET_B0);
                                                                    WriteIa64Bundle(instance,
                                                                                   EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR,
                                                                                   0x10,
                                                                                   ADD_R8_R0_ZERO,
                                                                                   NOP_I,
                                                                                   BR_RET_B0);
                                                                    WriteIa64Bundle(instance,
                                                                                   EFI_OPEN_VOLUME_STUB_CODE_ADDR,
                                                                                   0x10,
                                                                                   ADD_R8_R0_ZERO,
                                                                                   NOP_I,
                                                                                   BR_RET_B0);
                                                                    WriteIa64Bundle(instance,
                                                                                   EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR,
                                                                                   0x10,
                                                                                   ADD_R8_R0_ZERO,
                                                                                   NOP_I,
                                                                                   BR_RET_B0);
                                                                     WriteIa64Bundle(instance,
                                                                                    EFI_GET_VARIABLE_STUB_CODE_ADDR,
                                                                                    0x10,
                                                                                    ADD_R8_R0_MINUS_1,
                                                                                    NOP_I,
                                                                                    BR_RET_B0);
                                                                    const uint64_t efiCallableStubs[] = {
                                                                        EFI_FILE_OPEN_STUB_CODE_ADDR,
                                                                        EFI_FILE_CLOSE_STUB_CODE_ADDR,
                                                                        EFI_FILE_READ_STUB_CODE_ADDR,
                                                                        EFI_FILE_GET_POSITION_STUB_CODE_ADDR,
                                                                        EFI_FILE_SET_POSITION_STUB_CODE_ADDR,
                                                                        EFI_FILE_GET_INFO_STUB_CODE_ADDR,
                                                                        EFI_LOCATE_HANDLE_STUB_CODE_ADDR,
                                                                        EFI_LOCATE_PROTOCOL_STUB_CODE_ADDR,
                                                                        EFI_GET_MEMORY_MAP_STUB_CODE_ADDR,
                                                                        EFI_EXIT_BOOT_SERVICES_STUB_CODE_ADDR,
                                                                        EFI_LOAD_IMAGE_STUB_CODE_ADDR,
                                                                        EFI_START_IMAGE_STUB_CODE_ADDR
                                                                    };
                                                                    for (uint64_t stubCode : efiCallableStubs) {
                                                                        WriteIa64Bundle(instance,
                                                                                       stubCode,
                                                                                       0x10,
                                                                                       ADD_R8_R0_ZERO,
                                                                                       NOP_I,
                                                                                       BR_RET_B0);
                                                                    }
                                                                    write64(EFI_UNSUPPORTED_STUB_DESC_ADDR, EFI_UNSUPPORTED_STUB_CODE_ADDR);
                                                                    write64(EFI_UNSUPPORTED_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    write64(EFI_GET_VARIABLE_STUB_DESC_ADDR, EFI_GET_VARIABLE_STUB_CODE_ADDR);
                                                                    write64(EFI_GET_VARIABLE_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    write64(EFI_SUCCESS_STUB_DESC_ADDR, EFI_SUCCESS_STUB_CODE_ADDR);
                                                                    write64(EFI_SUCCESS_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    write64(EFI_ALLOCATE_POOL_STUB_DESC_ADDR, EFI_ALLOCATE_POOL_STUB_CODE_ADDR);
                                                                    write64(EFI_ALLOCATE_POOL_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    write64(EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR, EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR);
                                                                    write64(EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    write64(EFI_OPEN_VOLUME_STUB_DESC_ADDR, EFI_OPEN_VOLUME_STUB_CODE_ADDR);
                                                                    write64(EFI_OPEN_VOLUME_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    write64(EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR,
                                                                            EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR);
                                                                    write64(EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR + 8,
                                                                            peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    auto writeDescriptor = [&](uint64_t descriptorAddress, uint64_t codeAddress) {
                                                                        write64(descriptorAddress, codeAddress);
                                                                        write64(descriptorAddress + 8,
                                                                                peInfo.hasGlobalPointer ? peInfo.globalPointer : EFI_STUB_ADDR);
                                                                    };
                                                                    writeDescriptor(EFI_FILE_OPEN_STUB_DESC_ADDR, EFI_FILE_OPEN_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_FILE_CLOSE_STUB_DESC_ADDR, EFI_FILE_CLOSE_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_FILE_READ_STUB_DESC_ADDR, EFI_FILE_READ_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_FILE_GET_POSITION_STUB_DESC_ADDR, EFI_FILE_GET_POSITION_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_FILE_SET_POSITION_STUB_DESC_ADDR, EFI_FILE_SET_POSITION_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_FILE_GET_INFO_STUB_DESC_ADDR, EFI_FILE_GET_INFO_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_LOCATE_HANDLE_STUB_DESC_ADDR, EFI_LOCATE_HANDLE_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR, EFI_LOCATE_PROTOCOL_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_GET_MEMORY_MAP_STUB_DESC_ADDR, EFI_GET_MEMORY_MAP_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR, EFI_EXIT_BOOT_SERVICES_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_LOAD_IMAGE_STUB_DESC_ADDR, EFI_LOAD_IMAGE_STUB_CODE_ADDR);
                                                                    writeDescriptor(EFI_START_IMAGE_STUB_DESC_ADDR, EFI_START_IMAGE_STUB_CODE_ADDR);

                                                                    for (uint64_t offset = 0x18; offset < 0x88; offset += 8) {
                                                                        write64(EFI_RUNTIME_SERVICES_ADDR + offset,
                                                                                EFI_UNSUPPORTED_STUB_DESC_ADDR);
                                                                    }
                                                                    // GetVariable should report no NVRAM variables instead of a
                                                                    // generic unsupported call. Bootloaders use it to probe boot
                                                                    // variables and can distinguish EFI_NOT_FOUND from a broken
                                                                    // firmware service.
                                                                    write64(EFI_RUNTIME_SERVICES_ADDR + 0x48,
                                                                            EFI_GET_VARIABLE_STUB_DESC_ADDR);
                                                                    for (uint64_t offset = 0x18; offset < 0x180; offset += 8) {
                                                                        write64(EFI_BOOT_SERVICES_ADDR + offset,
                                                                                EFI_UNSUPPORTED_STUB_DESC_ADDR);
                                                                    }
                                                                    // The current bootloader repeatedly calls the 64-bit
                                                                    // Boot Services entry at offset 0x100 while connecting
                                                                    // devices. Returning EFI_SUCCESS lets it continue to the
                                                                    // next userland instruction path without modeling devices.
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x38, EFI_GET_MEMORY_MAP_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x40, EFI_ALLOCATE_POOL_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x48, EFI_SUCCESS_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x98, EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x118, EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0xB0, EFI_LOCATE_HANDLE_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x138, EFI_LOCATE_HANDLE_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0xC8, EFI_LOAD_IMAGE_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0xD0, EFI_START_IMAGE_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0xE8, EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x100, EFI_SUCCESS_STUB_DESC_ADDR);
                                                                    write64(EFI_BOOT_SERVICES_ADDR + 0x140, EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR);

                                                                    const uint64_t memorySizeForBootImage =
                                                                        static_cast<uint64_t>(instance->vm->getMemory().GetTotalSize());
                                                                    if (!bootImgData.empty() &&
                                                                        EFI_BOOT_IMAGE_GUEST_BASE + bootImgData.size() < memorySizeForBootImage &&
                                                                        EFI_BOOT_IMAGE_GUEST_BASE + bootImgData.size() < EFI_STUB_ADDR) {
                                                                        instance->vm->getMemory().Write(
                                                                            EFI_BOOT_IMAGE_GUEST_BASE,
                                                                            bootImgData.data(),
                                                                            bootImgData.size());
                                                                        write64(EFI_BOOT_IMAGE_METADATA_ADDR, EFI_BOOT_IMAGE_METADATA_SIGNATURE);
                                                                        write64(EFI_BOOT_IMAGE_METADATA_ADDR + 8, EFI_BOOT_IMAGE_GUEST_BASE);
                                                                        write64(EFI_BOOT_IMAGE_METADATA_ADDR + 16,
                                                                                static_cast<uint64_t>(bootImgData.size()));
                                                                        IA64ISAPlugin* ia64Plugin = getActiveIa64Plugin(instance);
                                                                        if (ia64Plugin) {
                                                                            ia64Plugin->setBootImageBackingStore(bootImgData);
                                                                        }
                                                                        oss.str("");
                                                                        oss << "[EFI-MILESTONE] Published read-only EFI boot image backing store"
                                                                            << " base=0x" << std::hex << EFI_BOOT_IMAGE_GUEST_BASE
                                                                            << " size=0x" << bootImgData.size()
                                                                            << " metadata=0x" << EFI_BOOT_IMAGE_METADATA_ADDR
                                                                            << std::dec;
                                                                        LOG_INFO(oss.str());
                                                                        logBootImageMetadata("publish-readback");
                                                                        {
                                                                            std::ostringstream trace;
                                                                            trace << "[EFI-MILESTONE] publish-readback boot image metadata"
                                                                                  << " vm=0x" << std::hex << reinterpret_cast<uintptr_t>(instance->vm.get())
                                                                                  << " cpu=0x" << reinterpret_cast<uintptr_t>(&instance->vm->getCPU())
                                                                                  << " memoryRef=0x" << reinterpret_cast<uintptr_t>(&instance->vm->getMemory())
                                                                                  << " memoryObj=0x" << reinterpret_cast<uintptr_t>(dynamic_cast<Memory*>(&instance->vm->getMemory()))
                                                                                  << " plugin=0x" << reinterpret_cast<uintptr_t>(ia64Plugin)
                                                                                  << " bootImageBytes=0x" << bootImgData.size()
                                                                                  << " signature=0x" << EFI_BOOT_IMAGE_METADATA_SIGNATURE
                                                                                  << " base=0x" << EFI_BOOT_IMAGE_GUEST_BASE
                                                                                  << " size=0x" << bootImgData.size()
                                                                                  << " addr=0x" << EFI_BOOT_IMAGE_METADATA_ADDR;
                                                                            LOG_INFO(trace.str());
                                                                        }
                                                                        installEfiBootMetadataWriteTrace(instance,
                                                                                                         "boot-image publication",
                                                                                                         EFI_BOOT_IMAGE_METADATA_ADDR);
                                                                        BootStageTrace::Event("EFI_BOOT_IMAGE_BACKING_STORE",
                                                                            "base=" + BootStageTrace::Hex(EFI_BOOT_IMAGE_GUEST_BASE) +
                                                                            " size=" + BootStageTrace::Hex(static_cast<uint64_t>(bootImgData.size())) +
                                                                            " metadata=" + BootStageTrace::Hex(EFI_BOOT_IMAGE_METADATA_ADDR));
                                                                    } else {
                                                                        LOG_WARN("[EFI-MILESTONE] EFI boot image backing store not published; FileProtocol will report EFI_NO_MEDIA");
                                                                        BootStageTrace::Event("EFI_BOOT_IMAGE_BACKING_STORE",
                                                                            "published=false base=" + BootStageTrace::Hex(EFI_BOOT_IMAGE_GUEST_BASE) +
                                                                            " size=" + BootStageTrace::Hex(static_cast<uint64_t>(bootImgData.size())) +
                                                                            " metadata=" + BootStageTrace::Hex(EFI_BOOT_IMAGE_METADATA_ADDR));
                                                                    }

                                                                    static const uint16_t firmwareVendor[] = {
                                                                        'g','u','i','d','e','X','O','S',' ','H','y','p','e','r','v','i','s','o','r',0
                                                                    };
                                                                    instance->vm->getMemory().Write(EFI_FIRMWARE_VENDOR_ADDR,
                                                                        reinterpret_cast<const uint8_t*>(firmwareVendor),
                                                                        sizeof(firmwareVendor));

                                                                    // Do not advertise firmware configuration tables in the
                                                                    // Phase 2 userland handoff. A fake all-zero table is more
                                                                    // misleading than absence here because boot code may treat
                                                                    // the vendor payload as a real firmware structure.

                                                                    // Minimal EFI_LOADED_IMAGE_PROTOCOL for HandleProtocol.
                                                                    // This is the boot app's own image metadata, not platform
                                                                    // firmware, and keeps early userland EFI setup on the
                                                                    // success path without inventing ACPI/PCI devices.
                                                                    write32(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x00, 0x1000U);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x10, EFI_STUB_ADDR);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x18, EFI_IMAGE_DEVICE_HANDLE);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x20, EFI_LOADED_IMAGE_FILE_PATH_ADDR);
                                                                    LOG_INFO(std::string("[EFI-MILESTONE] LoadedImage.FilePath pointer written")
                                                                             + " field=0x" + BootStageTrace::Hex(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x20)
                                                                             + " value=0x" + BootStageTrace::Hex(EFI_LOADED_IMAGE_FILE_PATH_ADDR));
                                                                    // Provide a real empty UTF-16 load-options buffer so the
                                                                    // bootloader never has to chase a null pointer when it
                                                                    // probes argv-like state during early file/path setup.
                                                                    write32(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x30, 0U);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x38, EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x40, loadAddress);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x48, imageBuffer.size());
                                                                    write32(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x50, 2U);
                                                                    write32(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x54, 4U);
                                                                    write64(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x58, EFI_UNSUPPORTED_STUB_DESC_ADDR);

                                                                    // MEDIA_FILEPATH_DP("\\EFI\\BOOT\\BOOTIA64.EFI") + End.
                                                                    // The loaded-image file path lets bootloader userland derive
                                                                    // its boot directory before it starts probing SimpleFS.
                                                                    static const uint16_t loadedImagePath[] = {
                                                                        '\\','E','F','I','\\','B','O','O','T','\\',
                                                                        'B','O','O','T','I','A','6','4','.','E','F','I',0
                                                                    };
                                                                    const uint16_t filePathNodeLength =
                                                                        static_cast<uint16_t>(4 + sizeof(loadedImagePath));
                                                                    write8(EFI_LOADED_IMAGE_FILE_PATH_ADDR + 0x00, 0x04U);
                                                                    write8(EFI_LOADED_IMAGE_FILE_PATH_ADDR + 0x01, 0x04U);
                                                                    write16(EFI_LOADED_IMAGE_FILE_PATH_ADDR + 0x02,
                                                                            filePathNodeLength);
                                                                    instance->vm->getMemory().Write(EFI_LOADED_IMAGE_FILE_PATH_ADDR + 0x04,
                                                                        reinterpret_cast<const uint8_t*>(loadedImagePath),
                                                                        sizeof(loadedImagePath));
                                                                    const uint64_t filePathEndNode =
                                                                        EFI_LOADED_IMAGE_FILE_PATH_ADDR + filePathNodeLength;
                                                                    write8(filePathEndNode + 0x00, 0x7FU);
                                                                    write8(filePathEndNode + 0x01, 0xFFU);
                                                                    write16(filePathEndNode + 0x02, 0x04U);
                                                                    write16(EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR, 0U);
                                                                    seedLoadedImageLoadOptions(instance,
                                                                                              EFI_LOADED_IMAGE_PROTOCOL_ADDR,
                                                                                              EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR);

                                                                    uint32_t loadedImageRevision = 0;
                                                                    uint64_t loadedImageFilePath = 0;
                                                                    uint64_t loadedImageLoadOptions = 0;
                                                                    uint64_t loadedImageImageBase = 0;
                                                                    uint64_t loadedImageImageSize = 0;
                                                                    instance->vm->getMemory().Read(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x00,
                                                                        reinterpret_cast<uint8_t*>(&loadedImageRevision),
                                                                        sizeof(loadedImageRevision));
                                                                    instance->vm->getMemory().Read(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x20,
                                                                        reinterpret_cast<uint8_t*>(&loadedImageFilePath),
                                                                        sizeof(loadedImageFilePath));
                                                                    instance->vm->getMemory().Read(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x38,
                                                                        reinterpret_cast<uint8_t*>(&loadedImageLoadOptions),
                                                                        sizeof(loadedImageLoadOptions));
                                                                    instance->vm->getMemory().Read(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x40,
                                                                        reinterpret_cast<uint8_t*>(&loadedImageImageBase),
                                                                        sizeof(loadedImageImageBase));
                                                                    instance->vm->getMemory().Read(EFI_LOADED_IMAGE_PROTOCOL_ADDR + 0x48,
                                                                        reinterpret_cast<uint8_t*>(&loadedImageImageSize),
                                                                        sizeof(loadedImageImageSize));
                                                                    oss.str("");
                                                                    oss << "  LoadedImage verify: revision=0x" << std::hex << loadedImageRevision
                                                                        << " filePath=0x" << loadedImageFilePath
                                                                        << " loadOptions=0x" << loadedImageLoadOptions
                                                                        << " imageBase=0x" << loadedImageImageBase
                                                                        << " imageSize=0x" << loadedImageImageSize
                                                                        << " loadedImageFilePathAddr=0x" << EFI_LOADED_IMAGE_FILE_PATH_ADDR
                                                                        << " loadedImageFilePathNodeLength=0x" << filePathNodeLength
                                                                        << " loadedImageFilePathBytes=" << previewBytesRaw(instance->vm->getMemory(), EFI_LOADED_IMAGE_FILE_PATH_ADDR, static_cast<size_t>(filePathNodeLength) + 4ULL)
                                                                        << " loadedImageFilePathNodes=<omitted>"
                                                                        << " loadOptionsBytes=" << previewBytesRaw(instance->vm->getMemory(), EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR, 16);
                                                                    LOG_INFO(oss.str());
                                                                    BootStageTrace::Event("EFI_LOADED_IMAGE_VERIFY",
                                                                        "revision=" + BootStageTrace::Hex(loadedImageRevision) +
                                                                        " filePath=" + BootStageTrace::Hex(loadedImageFilePath) +
                                                                        " loadOptions=" + BootStageTrace::Hex(loadedImageLoadOptions) +
                                                                        " imageBase=" + BootStageTrace::Hex(loadedImageImageBase) +
                                                                        " imageSize=" + BootStageTrace::Hex(loadedImageImageSize));
                                                                    installEfiLoadedImageReadTrace(instance,
                                                                                                   "boot-image setup",
                                                                                                   EFI_LOADED_IMAGE_PROTOCOL_ADDR,
                                                                                                   EFI_LOADED_IMAGE_FILE_PATH_ADDR,
                                                                                                   static_cast<uint64_t>(filePathNodeLength) + 4ULL);

                                                                    // Minimal Simple Text Output protocol for BOOTIA64.EFI
                                                                    // status/diagnostic writes. This avoids bootloader printf
                                                                    // callbacks being built from null ConOut/StdErr pointers.
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x00,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // Reset
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x08,
                                                                            EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR); // OutputString
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x10,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // TestString
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x18,
                                                                            EFI_UNSUPPORTED_STUB_DESC_ADDR); // QueryMode
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x20,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // SetMode
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x28,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // SetAttribute
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x30,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // ClearScreen
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x38,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // SetCursorPosition
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x40,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR); // EnableCursor
                                                                    write64(EFI_TEXT_OUTPUT_PROTOCOL_ADDR + 0x48,
                                                                            EFI_TEXT_OUTPUT_MODE_ADDR);
                                                                    write32(EFI_TEXT_OUTPUT_MODE_ADDR + 0x00, 1U); // MaxMode
                                                                    write32(EFI_TEXT_OUTPUT_MODE_ADDR + 0x04, 0U); // Mode
                                                                    write32(EFI_TEXT_OUTPUT_MODE_ADDR + 0x08, 0x0FU); // Attribute
                                                                    write32(EFI_TEXT_OUTPUT_MODE_ADDR + 0x0C, 0U); // CursorColumn
                                                                    write32(EFI_TEXT_OUTPUT_MODE_ADDR + 0x10, 0U); // CursorRow
                                                                    write8(EFI_TEXT_OUTPUT_MODE_ADDR + 0x14, 1U); // CursorVisible

                                                                    // Minimal read-only Simple File System/File protocol.
                                                                    // File methods are intercepted in IA64ISAPlugin and backed
                                                                    // by the already extracted FAT boot image.
                                                                    write64(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR + 0x00, 0x00010000ULL);
                                                                    write64(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR + 0x08,
                                                                            EFI_OPEN_VOLUME_STUB_DESC_ADDR);
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x00, 0x00010000ULL);
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x08,
                                                                            EFI_FILE_OPEN_STUB_DESC_ADDR);   // Open
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x10,
                                                                            EFI_FILE_CLOSE_STUB_DESC_ADDR);  // Close
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x18,
                                                                            EFI_UNSUPPORTED_STUB_DESC_ADDR); // Delete
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x20,
                                                                            EFI_FILE_READ_STUB_DESC_ADDR);   // Read
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x28,
                                                                            EFI_UNSUPPORTED_STUB_DESC_ADDR); // Write
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x30,
                                                                            EFI_FILE_GET_POSITION_STUB_DESC_ADDR); // GetPosition
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x38,
                                                                            EFI_FILE_SET_POSITION_STUB_DESC_ADDR); // SetPosition
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x40,
                                                                            EFI_FILE_GET_INFO_STUB_DESC_ADDR); // GetInfo
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x48,
                                                                            EFI_UNSUPPORTED_STUB_DESC_ADDR); // SetInfo
                                                                    write64(EFI_ROOT_FILE_PROTOCOL_ADDR + 0x50,
                                                                            EFI_SUCCESS_STUB_DESC_ADDR);     // Flush

                                                                    uint64_t configTableCount = 0;
                                                                    instance->vm->getMemory().Read(EFI_STUB_ADDR + 0x68,
                                                                        reinterpret_cast<uint8_t*>(&configTableCount),
                                                                        sizeof(configTableCount));
                                                                    logBootImageMetadata("pre-handoff-readback");
                                                                    oss.str("");
                                                                    oss << "  EFI System Table stub at: 0x" << std::hex << EFI_STUB_ADDR
                                                                        << " (RuntimeServices=0x" << EFI_RUNTIME_SERVICES_ADDR
                                                                        << ", BootServices=0x" << EFI_BOOT_SERVICES_ADDR
                                                                        << ", UnsupportedService=0x" << EFI_UNSUPPORTED_STUB_DESC_ADDR
                                                                        << ", GetVariable=0x" << EFI_GET_VARIABLE_STUB_DESC_ADDR
                                                                        << ", SuccessService=0x" << EFI_SUCCESS_STUB_DESC_ADDR
                                                                        << ", AllocatePool=0x" << EFI_ALLOCATE_POOL_STUB_DESC_ADDR
                                                                        << ", HandleProtocol=0x" << EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR
                                                                        << ", OpenVolume=0x" << EFI_OPEN_VOLUME_STUB_DESC_ADDR
                                                                        << ", TextOutput=0x" << EFI_TEXT_OUTPUT_PROTOCOL_ADDR
                                                                        << ", LoadedImageFilePath=0x" << EFI_LOADED_IMAGE_FILE_PATH_ADDR
                                                                        << ", LoadOptionsPtr=0x" << EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR
                                                                        << ", LoadOptionsBuffer=0x" << EFI_LOADED_IMAGE_LOAD_OPTIONS_ADDR
                                                                        << ", ConfigTables=" << std::dec << configTableCount << ")";
                                                                    LOG_INFO(oss.str());
                                                                }

                                                                // Set EFI entry arguments: r32=ImageHandle, r33=SystemTable
                                                                instance->vm->writeGR(0, 32, EFI_IMAGE_HANDLE);
                                                                instance->vm->writeGR(0, 33, EFI_STUB_ADDR);
                                                                const uint64_t efiStackTop = SetupMinimalEfiStack(instance, oss);
                                                                oss.str("");
                                                                oss << "  Set r32=ImageHandle=0x" << std::hex << EFI_IMAGE_HANDLE
                                                                    << ", r33=EFI_SystemTable=0x" << EFI_STUB_ADDR;
                                                                LOG_INFO(oss.str());
                                                                {
                                                                    std::ostringstream ctx;
                                                                    ctx << "entryPoint=" << BootStageTrace::Hex(entryPoint)
                                                                        << " loadAddress=" << BootStageTrace::Hex(loadAddress)
                                                                        << " imageBase=" << BootStageTrace::Hex(peInfo.imageBase)
                                                                        << " imageSize=" << BootStageTrace::Hex(imageBuffer.size())
                                                                        << " r1_gp=" << BootStageTrace::Hex(peInfo.hasGlobalPointer ? peInfo.globalPointer : 0)
                                                                        << " r12_sp=" << BootStageTrace::Hex(efiStackTop)
                                                                        << " r32_ImageHandle=" << BootStageTrace::Hex(EFI_IMAGE_HANDLE)
                                                                        << " r33_SystemTable=" << BootStageTrace::Hex(EFI_STUB_ADDR)
                                                                        << " bootImage=\"" << bootImgPath << "\""
                                                                        << " efiPath=\"" << foundEFIPath << "\"";
                                                                    BootStageTrace::EventOnce("NATIVE_BUILD_STAMP",
                                                                        std::string("source=\"VMManager.cpp\" buildDate=\"") +
                                                                        __DATE__ + "\" buildTime=\"" + __TIME__ + "\"");
                                                                    BootStageTrace::Stage(90, "Initial CPU state prepared", ctx.str());
                                                                }

                                                                LOG_INFO("??? EFI bootloader loaded successfully from boot image!");
                                                                LOG_INFO("  Source: " + bootImgPath + " -> " + foundEFIPath);
                                                                
                                                                oss.str("");
                                                                oss << "  Load address: 0x" << std::hex << loadAddress << std::dec;
                                                                LOG_INFO(oss.str());
                                                                
                                                                oss.str("");
                                                                oss << "  Entry point: 0x" << std::hex << entryPoint << std::dec;
                                                                LOG_INFO(oss.str());

                                                                DrawBootStatus(instance,
                                                                               "EFI BOOTLOADER LOADED",
                                                                               "STARTING IA64 EXECUTION",
                                                                               "SEE LOGS FOR DECODE TRACE");
                                                                
                                                                bootedFromImage = true;
                                                            } else {
                                                                LOG_ERROR("Failed to load EFI bootloader into memory");
                                                                DrawBootStatus(instance,
                                                                               "BOOT FAILED",
                                                                               "EFI LOAD INTO MEMORY FAILED",
                                                                               "SEE NATIVE LOG");
                                                            }
                                                        } else {
                                                            LOG_ERROR("Failed to prepare PE image for loading");
                                                        }
                                                    }
                                                } else {
                                                    LOG_ERROR("Failed to parse EFI executable as PE/COFF");
                                                }
                                            } else {
                                                LOG_ERROR("Failed to read EFI file from boot image");
                                            }
                                        } else {
                                            LOG_WARN("No EFI bootloader found in boot image");
                                        }
                                    } else {
                                        LOG_WARN("Boot image is not a valid FAT filesystem");
                                    }
                                }
                                
                                // If we didn't boot from image, try other fallbacks
                                if (!bootedFromImage) {
                                    LOG_INFO("This ISO does not appear to have EFI boot support.");
                                    LOG_INFO("Attempting to find IA-64 kernel for direct boot...");
                                    
                                    // Try to find the IA-64 kernel in common Debian installer locations
                                    const char* kernelPaths[] = {
                                        "/install/vmlinuz",
                                        "/install/vmlinux",
                                        "/boot/vmlinuz",
                                        "/boot/vmlinux",
                                        "install/vmlinuz",
                                        "install/vmlinux",
                                        "boot/vmlinuz",
                                        "boot/vmlinux"
                                    };
                                    
                                    std::vector<uint8_t> kernelData;
                                    bool foundKernel = false;
                                    std::string foundPath;
                                    
                                    for (const char* path : kernelPaths) {
                                        LOG_INFO("Searching for kernel: " + std::string(path));
                                        if (isoParser.extractFile(path, kernelData)) {
                                            LOG_INFO("? Found kernel: " + std::string(path));
                                            foundKernel = true;
                                            foundPath = path;
                                            break;
                                        }
                                    }
                                    
                                    if (foundKernel) {
                                        LOG_INFO("? Kernel extracted: " + 
                                                std::to_string(kernelData.size()) + " bytes");
                                        LOG_INFO("  Path: " + foundPath);
                                        
                                        // TODO: Parse ELF kernel format and load properly
                                        // For now, attempt basic loading
                                        uint64_t kernelAddress = 0x100000;  // Standard kernel load address
                                        
                                        if (instance->vm->loadProgram(kernelData.data(), 
                                                                     kernelData.size(), 
                                                                     kernelAddress)) {
                                            instance->vm->setEntryPoint(kernelAddress);
                                            LOG_INFO("? Kernel loaded at 0x" + 
                                                    std::to_string(kernelAddress));
                                            LOG_WARN("Direct kernel boot attempted - this is experimental");
                                            LOG_WARN("The kernel may require additional setup (initrd, boot parameters, etc.)");
                                        } else {
                                            LOG_ERROR("Failed to load kernel into memory");
                                        }
                                    } else {
                                        LOG_WARN("No IA-64 kernel found in standard locations");
                                        LOG_INFO("Falling back to raw boot sector loading...");
                                        
                                        // Fallback: Load first 4KB as before
                                        std::vector<uint8_t> bootSector(4096);
                                        int64_t bytesRead = bootDevice->readBytes(0, bootSector.size(), bootSector.data());
                                        
                                        if (bytesRead > 0) {
                                            uint64_t bootAddress = bootConfig.entryPoint != 0 ? 
                                                                  bootConfig.entryPoint : 0x100000;
                                            LOG_INFO("Loading raw boot data to address: 0x" + 
                                                    std::to_string(bootAddress));
                                            LOG_WARN("WARNING: Loading x86 BIOS boot code on IA-64 architecture");
                                            LOG_WARN("This will NOT work. Please use an EFI-bootable IA-64 ISO or direct kernel boot.");
                                            
                                            if (instance->vm->loadProgram(bootSector.data(), 
                                                                         static_cast<size_t>(bytesRead), 
                                                                         bootAddress)) {
                                                instance->vm->setEntryPoint(bootAddress);
                                                LOG_INFO("Raw boot data loaded, entry point: 0x" + 
                                                        std::to_string(bootAddress));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    } else {
                        LOG_WARN("El Torito boot catalog not found");
                        LOG_INFO("This may not be a bootable ISO");
                    }
                } else {
                    LOG_WARN("Failed to parse ISO filesystem, trying raw boot sector...");
                    
                    // Fallback: Load first 4KB as raw boot sector
                    std::vector<uint8_t> bootSector(4096);
                    int64_t bytesRead = bootDevice->readBytes(0, bootSector.size(), bootSector.data());
                    
                    if (bytesRead > 0) {
                        uint64_t bootAddress = bootConfig.entryPoint != 0 ? 
                                              bootConfig.entryPoint : 0x100000;
                        LOG_INFO("Loading raw boot data to address: 0x" + 
                                std::to_string(bootAddress));
                        
                        if (instance->vm->loadProgram(bootSector.data(), 
                                                     static_cast<size_t>(bytesRead), 
                                                     bootAddress)) {
                            instance->vm->setEntryPoint(bootAddress);
                            LOG_INFO("Raw boot data loaded, entry point: 0x" + 
                                    std::to_string(bootAddress));
                        } else {
                            LOG_ERROR("Failed to load boot data into memory");
                        }
                    } else {
                        LOG_ERROR("Failed to read boot data from device");
                    }
                }
            } else {
                LOG_WARN("Boot device not found: " + bootConfig.bootDevice);
            }
        }
        
        instance->metadata.setState(VMState::RUNNING, "VM running");
        instance->metadata.startedTime = std::chrono::system_clock::now();
        
        LOG_INFO("VM started: " + vmId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception starting VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::stopVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canStop()) {
        LOG_ERROR("Cannot stop VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Stopping VM: " + vmId);
    
    try {
        instance->metadata.setState(VMState::STOPPING, "Stopping VM");
        
        instance->vm->halt();
        
        // Update resource usage before stopping
        updateResourceUsage(instance);
        
        instance->metadata.setState(VMState::STOPPED, "VM stopped");
        instance->metadata.stoppedTime = std::chrono::system_clock::now();
        
        LOG_INFO("VM stopped: " + vmId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception stopping VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::pauseVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canPause()) {
        LOG_ERROR("Cannot pause VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Pausing VM: " + vmId);
    
    instance->metadata.setState(VMState::PAUSING, "Pausing VM");
    
    // Update resource usage before pausing
    updateResourceUsage(instance);
    
    instance->metadata.setState(VMState::PAUSED, "VM paused");
    
    LOG_INFO("VM paused: " + vmId);
    return true;
}

bool VMManager::resumeVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!instance->metadata.canResume()) {
        LOG_ERROR("Cannot resume VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return false;
    }
    
    LOG_INFO("Resuming VM: " + vmId);
    
    instance->metadata.setState(VMState::RESUMING, "Resuming VM");
    instance->metadata.setState(VMState::RUNNING, "VM running");
    
    LOG_INFO("VM resumed: " + vmId);
    return true;
}

bool VMManager::resetVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    LOG_INFO("Resetting VM: " + vmId);
    
    try {
        VMState previousState = instance->metadata.currentState;
        
        if (!instance->vm->reset()) {
            instance->metadata.setError("Failed to reset VM");
            return false;
        }
        
        instance->metadata.resourceUsage.reset();
        instance->metadata.clearError();
        
        // Restore previous state if it was running
        if (previousState == VMState::RUNNING) {
            instance->metadata.setState(VMState::RUNNING, "VM reset and running");
        } else {
            instance->metadata.setState(VMState::STOPPED, "VM reset");
        }
        
        LOG_INFO("VM reset: " + vmId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception resetting VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::terminateVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    LOG_INFO("Terminating VM: " + vmId);
    
    instance->metadata.setState(VMState::TERMINATING, "Terminating VM");
    
    instance->vm->halt();
    updateResourceUsage(instance);
    
    instance->metadata.setState(VMState::TERMINATED, "VM terminated");
    instance->metadata.stoppedTime = std::chrono::system_clock::now();
    
    LOG_INFO("VM terminated: " + vmId);
    return true;
}

// ============================================================================
// VM Execution Control
// ============================================================================

uint64_t VMManager::runVM(const std::string& vmId, uint64_t maxCycles) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return 0;
    }
    
    if (instance->metadata.currentState != VMState::RUNNING) {
        LOG_ERROR("VM is not running: " + vmId);
        return 0;
    }
    
    try {
        uint64_t cyclesExecuted = instance->vm->run(maxCycles);

        if (cyclesExecuted == 0 || instance->vm->getState() == VMState::ERROR) {
            std::ostringstream oss;
            oss << "STOPPED AT IP=0x" << std::hex << instance->vm->getIP() << std::dec;
            const std::string stopLine = oss.str();

            // Show a clearly red boot-failure screen
            DrawBootStatus(instance,
                           "BOOT FAILED",
                           stopLine.c_str(),
                           "CHECK NATIVE LOG FOR [SKIP] WARNINGS",
                           0xFFFF4444);

            if (instance->vm->getState() == VMState::ERROR) {
                instance->metadata.setError("VM execution stopped; see native log for [SKIP] warnings");
            }
        } else {
            // Execution ran a full batch — show a "RUNNING" heartbeat on-screen
            // so the user can see the boot is progressing (updated each RunVM call)
            std::ostringstream oss;
            oss << "CYCLES: " << instance->metadata.resourceUsage.cyclesExecuted + cyclesExecuted;
            const std::string cyclesLine = oss.str();

            std::ostringstream ipOss;
            ipOss << "IP=0x" << std::hex << instance->vm->getIP() << std::dec;
            const std::string ipLine = ipOss.str();

            DrawBootStatus(instance,
                           "EFI BOOTING",
                           cyclesLine.c_str(),
                           ipLine.c_str(),
                           0xFF6EE7B7);
        }
        
        // Update resource usage
        instance->metadata.resourceUsage.cyclesExecuted += cyclesExecuted;
        
        return cyclesExecuted;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception running VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return 0;
    }
}

bool VMManager::stepVM(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (instance->metadata.currentState != VMState::RUNNING) {
        LOG_ERROR("VM is not running: " + vmId);
        return false;
    }
    
    try {
        bool result = instance->vm->step();
        
        instance->metadata.resourceUsage.instructionsExecuted++;
        
        return result;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception stepping VM: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

// ============================================================================
// Snapshot Management
// ============================================================================

std::string VMManager::snapshotVM(const std::string& vmId,
                                  const std::string& snapshotName,
                                  const std::string& description) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return "";
    }
    
    if (!instance->metadata.canSnapshot()) {
        LOG_ERROR("Cannot snapshot VM in current state: " + 
                 vmStateToString(instance->metadata.currentState));
        return "";
    }
    
    LOG_INFO("Creating snapshot for VM: " + vmId + " - " + snapshotName);
    
    try {
        VMState previousState = instance->metadata.currentState;
        instance->metadata.setState(VMState::SNAPSHOTTING, "Creating snapshot");
        
        // Generate snapshot ID
        std::string snapshotId = generateSnapshotId(snapshotName);
        
        // Create snapshot metadata
        VMSnapshot snapshot;
        snapshot.snapshotId = snapshotId;
        snapshot.name = snapshotName;
        snapshot.description = description;
        snapshot.createdTime = std::chrono::system_clock::now();
        snapshot.vmState = previousState;
        snapshot.memorySize = instance->metadata.configuration.memory.memorySize;
        snapshot.vmMetadata = instance->metadata;
        
        // Store snapshot
        snapshots_[vmId].push_back(snapshot);
        instance->metadata.snapshotCount = snapshots_[vmId].size();
        
        // Restore previous state
        instance->metadata.setState(previousState, "Snapshot created");
        
        LOG_INFO("Snapshot created: " + snapshotId);
        return snapshotId;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception creating snapshot: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return "";
    }
}

bool VMManager::restoreSnapshot(const std::string& vmId, const std::string& snapshotId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    // Find snapshot
    auto snapshotIt = snapshots_.find(vmId);
    if (snapshotIt == snapshots_.end()) {
        LOG_ERROR("No snapshots found for VM: " + vmId);
        return false;
    }
    
    auto& vmSnapshots = snapshotIt->second;
    auto it = std::find_if(vmSnapshots.begin(), vmSnapshots.end(),
        [&snapshotId](const VMSnapshot& s) { return s.snapshotId == snapshotId; });
    
    if (it == vmSnapshots.end()) {
        LOG_ERROR("Snapshot not found: " + snapshotId);
        return false;
    }
    
    LOG_INFO("Restoring snapshot: " + snapshotId + " for VM: " + vmId);
    
    try {
        instance->metadata.setState(VMState::RESTORING, "Restoring snapshot");
        
        // Restore metadata from snapshot
        const VMSnapshot& snapshot = *it;
        instance->metadata = snapshot.vmMetadata;
        instance->metadata.activeSnapshot = snapshotId;
        
        // Reset VM
        if (!instance->vm->reset()) {
            instance->metadata.setError("Failed to reset VM during restore");
            return false;
        }
        
        instance->metadata.setState(VMState::STOPPED, "Snapshot restored");
        
        LOG_INFO("Snapshot restored: " + snapshotId);
        return true;
        
    } catch (const std::exception& e) {
        instance->metadata.setError("Exception restoring snapshot: " + std::string(e.what()));
        LOG_ERROR(instance->metadata.lastError);
        return false;
    }
}

bool VMManager::deleteSnapshot(const std::string& vmId, const std::string& snapshotId) {
    auto snapshotIt = snapshots_.find(vmId);
    if (snapshotIt == snapshots_.end()) {
        LOG_ERROR("No snapshots found for VM: " + vmId);
        return false;
    }
    
    auto& vmSnapshots = snapshotIt->second;
    auto it = std::find_if(vmSnapshots.begin(), vmSnapshots.end(),
        [&snapshotId](const VMSnapshot& s) { return s.snapshotId == snapshotId; });
    
    if (it == vmSnapshots.end()) {
        LOG_ERROR("Snapshot not found: " + snapshotId);
        return false;
    }
    
    LOG_INFO("Deleting snapshot: " + snapshotId);
    
    vmSnapshots.erase(it);
    
    auto* instance = getVMInstance(vmId);
    if (instance) {
        instance->metadata.snapshotCount = vmSnapshots.size();
    }
    
    LOG_INFO("Snapshot deleted: " + snapshotId);
    return true;
}

std::vector<VMSnapshot> VMManager::listSnapshots(const std::string& vmId) const {
    auto it = snapshots_.find(vmId);
    if (it == snapshots_.end()) {
        return std::vector<VMSnapshot>();
    }
    return it->second;
}

// ============================================================================
// Storage Device Management
// ============================================================================

bool VMManager::attachStorage(const std::string& vmId, 
                              std::unique_ptr<IStorageDevice> device) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    if (!device) {
        LOG_ERROR("Invalid storage device");
        return false;
    }
    
    std::string deviceId = device->getDeviceId();
    LOG_INFO("Attaching storage device " + deviceId + " to VM: " + vmId);
    
    // Check for duplicate device ID
    for (const auto& existingDevice : instance->storageDevices) {
        if (existingDevice->getDeviceId() == deviceId) {
            LOG_ERROR("Storage device already attached: " + deviceId);
            return false;
        }
    }
    
    // Connect device
    if (!device->connect()) {
        LOG_ERROR("Failed to connect storage device: " + deviceId);
        return false;
    }
    
    instance->storageDevices.push_back(std::move(device));
    
    LOG_INFO("Storage device attached: " + deviceId);
    return true;
}

bool VMManager::detachStorage(const std::string& vmId, const std::string& deviceId) {
    auto* instance = getVMInstance(vmId);
    if (!instance) {
        LOG_ERROR("VM not found: " + vmId);
        return false;
    }
    
    LOG_INFO("Detaching storage device " + deviceId + " from VM: " + vmId);
    
    auto it = std::find_if(instance->storageDevices.begin(), 
                          instance->storageDevices.end(),
        [&deviceId](const std::unique_ptr<IStorageDevice>& dev) {
            return dev->getDeviceId() == deviceId;
        });
    
    if (it == instance->storageDevices.end()) {
        LOG_ERROR("Storage device not found: " + deviceId);
        return false;
    }
    
    (*it)->disconnect();
    instance->storageDevices.erase(it);
    
    LOG_INFO("Storage device detached: " + deviceId);
    return true;
}

StorageDeviceInfo VMManager::getStorageInfo(const std::string& vmId, 
                                            const std::string& deviceId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return StorageDeviceInfo();
    }
    
    for (const auto& device : instance->storageDevices) {
        if (device->getDeviceId() == deviceId) {
            return device->getInfo();
        }
    }
    
    return StorageDeviceInfo();
}

// ============================================================================
// VM Query and Status
// ============================================================================

VMMetadata VMManager::getVMMetadata(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMMetadata();
    }
    return instance->metadata;
}

VMState VMManager::getVMState(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMState::ERROR;
    }
    return instance->metadata.currentState;
}

VMResourceUsage VMManager::getVMResourceUsage(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance) {
        return VMResourceUsage();
    }
    return instance->metadata.resourceUsage;
}

bool VMManager::vmExists(const std::string& vmId) const {
    return vms_.find(vmId) != vms_.end();
}

std::vector<std::string> VMManager::listVMs() const {
    std::vector<std::string> vmIds;
    vmIds.reserve(vms_.size());
    
    for (const auto& pair : vms_) {
        vmIds.push_back(pair.first);
    }
    
    return vmIds;
}

std::vector<std::string> VMManager::listVMsByState(VMState state) const {
    std::vector<std::string> vmIds;
    
    for (const auto& pair : vms_) {
        if (pair.second->metadata.currentState == state) {
            vmIds.push_back(pair.first);
        }
    }
    
    return vmIds;
}

// ============================================================================
// Direct VM Access
// ============================================================================

VirtualMachine* VMManager::getVMDirect(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    return instance ? instance->vm.get() : nullptr;
}

const VirtualMachine* VMManager::getVMDirect(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    return instance ? instance->vm.get() : nullptr;
}

// ============================================================================
// Manager Configuration
// ============================================================================

size_t VMManager::stopAllVMs() {
    size_t count = 0;
    
    for (auto& pair : vms_) {
        if (pair.second->metadata.currentState == VMState::RUNNING ||
            pair.second->metadata.currentState == VMState::PAUSED) {
            if (stopVM(pair.first)) {
                count++;
            }
        }
    }
    
    return count;
}

std::string VMManager::getStatistics() const {
    std::ostringstream oss;
    
    oss << "VMManager Statistics:\n";
    oss << "  Total VMs: " << vms_.size() << "\n";
    
    size_t running = 0, stopped = 0, paused = 0, error = 0;
    for (const auto& pair : vms_) {
        switch (pair.second->metadata.currentState) {
            case VMState::RUNNING: running++; break;
            case VMState::STOPPED: stopped++; break;
            case VMState::PAUSED: paused++; break;
            case VMState::ERROR: error++; break;
            default: break;
        }
    }
    
    oss << "  Running: " << running << "\n";
    oss << "  Stopped: " << stopped << "\n";
    oss << "  Paused: " << paused << "\n";
    oss << "  Error: " << error << "\n";
    oss << "  Total Snapshots: " << snapshots_.size() << "\n";
    
    return oss.str();
}

// ============================================================================
// Internal Helpers
// ============================================================================

VMInstance* VMManager::getVMInstance(const std::string& vmId) {
    auto it = vms_.find(vmId);
    return it != vms_.end() ? it->second.get() : nullptr;
}

const VMInstance* VMManager::getVMInstance(const std::string& vmId) const {
    auto it = vms_.find(vmId);
    return it != vms_.end() ? it->second.get() : nullptr;
}

std::string VMManager::generateVMId(const std::string& vmName) {
    std::ostringstream oss;
    oss << vmName << "-" << std::setfill('0') << std::setw(6) << vmCounter_++;
    return oss.str();
}

std::string VMManager::generateSnapshotId(const std::string& snapshotName) {
    std::ostringstream oss;
    oss << snapshotName << "-" << std::setfill('0') << std::setw(6) 
        << snapshotCounter_++;
    return oss.str();
}

bool VMManager::canTransition(VMState from, VMState to) const {
    // Define valid state transitions
    // This can be expanded based on requirements
    
    switch (from) {
        case VMState::CREATED:
        case VMState::STOPPED:
            return to == VMState::STARTING || to == VMState::TERMINATED;
            
        case VMState::RUNNING:
            return to == VMState::PAUSING || to == VMState::STOPPING || 
                   to == VMState::SNAPSHOTTING || to == VMState::ERROR;
            
        case VMState::PAUSED:
            return to == VMState::RESUMING || to == VMState::STOPPING ||
                   to == VMState::SNAPSHOTTING;
            
        default:
            return true;  // Allow transitions from transitional states
    }
}

bool VMManager::createStorageDevices(VMInstance* instance,
                                    const std::vector<StorageConfiguration>& configs) {
    
    if (configs.empty()) {
        LOG_INFO("No storage devices to create");
        return true;
    }
    
    LOG_INFO("Creating " + std::to_string(configs.size()) + " storage device(s)");
    
    for (const auto& config : configs) {
        LOG_INFO("  Device ID: " + config.deviceId);
        LOG_INFO("  Type: " + config.deviceType);
        LOG_INFO("  Path: " + config.imagePath);
        LOG_INFO("  Read-only: " + std::string(config.readOnly ? "yes" : "no"));
        
        // Create appropriate storage device type
        std::unique_ptr<IStorageDevice> device;
        
        if (config.deviceType == "raw") {
            try {
                LOG_INFO("  Creating RawDiskDevice...");
                device = std::make_unique<RawDiskDevice>(
                    config.deviceId,
                    config.imagePath,
                    config.sizeBytes,
                    config.readOnly
                );
                LOG_INFO("  ? RawDiskDevice created");
            } catch (const std::exception& e) {
                LOG_ERROR("  ERROR creating RawDiskDevice: " + std::string(e.what()));
                return false;
            }
        } else {
            LOG_ERROR("Unsupported storage device type: " + config.deviceType);
            return false;
        }
        
        LOG_INFO("  Connecting storage device...");
        if (!device->connect()) {
            LOG_ERROR("  ERROR: Failed to connect storage device: " + config.deviceId);
            LOG_ERROR("  File path: " + config.imagePath);
            LOG_ERROR("  This usually means:");
            LOG_ERROR("    - File doesn't exist");
            LOG_ERROR("    - File is locked by another process");
            LOG_ERROR("    - Insufficient permissions");
            return false;
        }
        
        LOG_INFO("  ? Storage device connected successfully");
        instance->storageDevices.push_back(std::move(device));
    }
    
    LOG_INFO("? All storage devices created successfully");
    return true;
}

void VMManager::updateResourceUsage(VMInstance* instance) {
    if (!instance || !instance->vm) {
        return;
    }
    
    // Update memory usage
    instance->metadata.resourceUsage.memoryUsedBytes = 
        instance->metadata.configuration.memory.memorySize;
    
    // Other resource updates can be added here
}

// ============================================================================
// Console Output Access
// ============================================================================

std::vector<std::string> VMManager::getConsoleOutput(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::vector<std::string>();
    }
    return instance->vm->getConsoleOutput();
}

std::vector<std::string> VMManager::getConsoleOutput(const std::string& vmId, 
                                                     size_t startLine, 
                                                     size_t count) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::vector<std::string>();
    }
    return instance->vm->getConsoleOutput(startLine, count);
}

std::string VMManager::getRecentConsoleOutput(const std::string& vmId, 
                                              size_t maxBytes) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::string();
    }
    return instance->vm->getRecentConsoleOutput(maxBytes);
}

std::vector<std::string> VMManager::getConsoleOutputSince(const std::string& vmId, 
                                                          size_t lineNumber) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return std::vector<std::string>();
    }
    return instance->vm->getConsoleOutputSince(lineNumber);
}

size_t VMManager::getConsoleLineCount(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return 0;
    }
    return instance->vm->getConsoleLineCount();
}

uint64_t VMManager::getConsoleTotalBytes(const std::string& vmId) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return 0;
    }
    return instance->vm->getConsoleTotalBytes();
}

bool VMManager::clearConsoleOutput(const std::string& vmId) {
    auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return false;
    }
    instance->vm->clearConsoleOutput();
    return true;
}

// ============================================================================
// Framebuffer Access
// ============================================================================

bool VMManager::getFramebuffer(const std::string& vmId, 
                               uint8_t* buffer, 
                               size_t bufferSize,
                               size_t* width,
                               size_t* height) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return false;
    }
    
    const FramebufferDevice* fb = instance->vm->getFramebufferDevice();
    if (!fb) {
        return false;
    }
    
    const size_t fbWidth = fb->GetWidth();
    const size_t fbHeight = fb->GetHeight();
    const size_t fbSize = fbWidth * fbHeight * fb->GetBytesPerPixel();
    
    if (bufferSize < fbSize) {
        return false;
    }
    
    if (width) *width = fbWidth;
    if (height) *height = fbHeight;
    
    fb->CopyFramebuffer(buffer, bufferSize);
    return true;
}

bool VMManager::getFramebufferDimensions(const std::string& vmId,
                                        size_t* width,
                                        size_t* height) const {
    const auto* instance = getVMInstance(vmId);
    if (!instance || !instance->vm) {
        return false;
    }
    
    const FramebufferDevice* fb = instance->vm->getFramebufferDevice();
    if (!fb) {
        return false;
    }
    
    if (width) *width = fb->GetWidth();
    if (height) *height = fb->GetHeight();
    
    return true;
}

} // namespace ia64

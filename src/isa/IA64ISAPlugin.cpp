#include "IA64ISAPlugin.h"
#include "SyscallDispatcher.h"
#include "Profiler.h"
#include "memory.h"
#include "BootStageTrace.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <array>
#include <cctype>
#include <algorithm>
#include <iomanip>

namespace ia64 {

namespace {

struct CountedLoopTraceState {
    bool active = false;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t suppressed = 0;
};

CountedLoopTraceState g_countedLoopTrace;

constexpr uint64_t EFI_STATUS_SUCCESS = 0ULL;
constexpr uint64_t EFI_STATUS_INVALID_PARAMETER = 0x8000000000000002ULL;
constexpr uint64_t EFI_STATUS_UNSUPPORTED = 0x8000000000000003ULL;
constexpr uint64_t EFI_STATUS_BUFFER_TOO_SMALL = 0x8000000000000005ULL;
constexpr uint64_t EFI_STATUS_WRITE_PROTECTED = 0x8000000000000008ULL;
constexpr uint64_t EFI_STATUS_NOT_FOUND = 0x800000000000000EULL;
constexpr uint64_t EFI_STATUS_NO_MEDIA = 0x800000000000000CULL;
constexpr uint64_t EFI_HANDOFF_REGION_BASE = 0x1FE00000ULL;
constexpr uint64_t EFI_HANDOFF_REGION_END = 0x20000000ULL;
constexpr uint64_t EFI_RUNTIME_SERVICES_ADDR = EFI_HANDOFF_REGION_BASE + 0x400ULL;
constexpr uint64_t EFI_BOOT_SERVICES_ADDR = EFI_HANDOFF_REGION_BASE + 0x800ULL;
constexpr uint64_t EFI_BOOT_IMAGE_METADATA_ADDR = EFI_HANDOFF_REGION_BASE + 0x1D00ULL;
constexpr uint64_t EFI_BOOT_IMAGE_METADATA_SIGNATURE = 0x42465441494D4645ULL; // "EFMIATFB"
constexpr uint64_t EFI_BOOT_IMAGE_GUEST_BASE = 0x18000000ULL;
constexpr uint64_t EFI_OPEN_VOLUME_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xC80ULL;
constexpr uint64_t EFI_OPEN_VOLUME_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0xCC0ULL;
constexpr uint64_t EFI_LOADED_IMAGE_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0xD00ULL;
constexpr uint64_t EFI_GET_VARIABLE_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0xDC0ULL;
constexpr uint64_t EFI_ALLOCATE_POOL_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0xE40ULL;
constexpr uint64_t EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0xEC0ULL;
constexpr uint64_t EFI_UNSUPPORTED_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0xF40ULL;
constexpr uint64_t EFI_SUCCESS_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0xFC0ULL;
constexpr uint64_t EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0x1000ULL;
constexpr uint64_t EFI_ROOT_FILE_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0x1040ULL;
constexpr uint64_t EFI_TEXT_OUTPUT_PROTOCOL_ADDR = EFI_HANDOFF_REGION_BASE + 0x1200ULL;
constexpr uint64_t EFI_TEXT_OUTPUT_MODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1260ULL;
constexpr uint64_t EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1280ULL;
constexpr uint64_t EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x12C0ULL;
constexpr uint64_t EFI_FILE_OPEN_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1400ULL;
constexpr uint64_t EFI_FILE_OPEN_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x1440ULL;
constexpr uint64_t EFI_FILE_CLOSE_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1480ULL;
constexpr uint64_t EFI_FILE_CLOSE_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x14C0ULL;
constexpr uint64_t EFI_FILE_READ_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1500ULL;
constexpr uint64_t EFI_FILE_READ_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x1540ULL;
constexpr uint64_t EFI_FILE_GET_POSITION_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1580ULL;
constexpr uint64_t EFI_FILE_GET_POSITION_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x15C0ULL;
constexpr uint64_t EFI_FILE_SET_POSITION_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1600ULL;
constexpr uint64_t EFI_FILE_SET_POSITION_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x1640ULL;
constexpr uint64_t EFI_FILE_GET_INFO_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1680ULL;
constexpr uint64_t EFI_FILE_GET_INFO_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x16C0ULL;
constexpr uint64_t EFI_LOCATE_HANDLE_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1700ULL;
constexpr uint64_t EFI_LOCATE_HANDLE_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x1740ULL;
constexpr uint64_t EFI_LOCATE_PROTOCOL_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1780ULL;
constexpr uint64_t EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x17C0ULL;
constexpr uint64_t EFI_GET_MEMORY_MAP_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1800ULL;
constexpr uint64_t EFI_GET_MEMORY_MAP_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x1840ULL;
constexpr uint64_t EFI_EXIT_BOOT_SERVICES_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1880ULL;
constexpr uint64_t EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x18C0ULL;
constexpr uint64_t EFI_LOAD_IMAGE_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1900ULL;
constexpr uint64_t EFI_LOAD_IMAGE_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x1940ULL;
constexpr uint64_t EFI_START_IMAGE_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0x1980ULL;
constexpr uint64_t EFI_START_IMAGE_STUB_DESC_ADDR = EFI_HANDOFF_REGION_BASE + 0x19C0ULL;
constexpr uint64_t EFI_GET_VARIABLE_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xD80ULL;
constexpr uint64_t EFI_ALLOCATE_POOL_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xE00ULL;
constexpr uint64_t EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xE80ULL;
constexpr uint64_t EFI_UNSUPPORTED_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xF00ULL;
constexpr uint64_t EFI_SUCCESS_STUB_CODE_ADDR = EFI_HANDOFF_REGION_BASE + 0xF80ULL;
constexpr uint64_t EFI_POOL_BASE = EFI_HANDOFF_REGION_BASE + 0x2000ULL;
constexpr uint64_t EFI_POOL_END = EFI_HANDOFF_REGION_BASE + 0x80000ULL;
constexpr bool IA64_STRICT_RECOVERY = false;
constexpr uint64_t IA64_EXECUTABLE_IMAGE_BASE = 0x100000ULL;
constexpr uint64_t IA64_EXECUTABLE_IMAGE_END = 0x200000ULL;
using EfiGuid = std::array<uint8_t, 16>;
struct EfiSlotName {
    uint64_t offset;
    const char* name;
};

std::string cpuSummary(const CPUState& cpu) {
    std::ostringstream oss;
    oss << "ip=" << BootStageTrace::Hex(cpu.GetIP())
        << " r1_gp=" << BootStageTrace::Hex(cpu.GetGR(1))
        << " r8_ret=" << BootStageTrace::Hex(cpu.GetGR(8))
        << " r12_sp=" << BootStageTrace::Hex(cpu.GetGR(12))
        << " r32=" << BootStageTrace::Hex(cpu.GetGR(32))
        << " r33=" << BootStageTrace::Hex(cpu.GetGR(33))
        << " br0=" << BootStageTrace::Hex(cpu.GetBR(0))
        << " br6=" << BootStageTrace::Hex(cpu.GetBR(6))
        << " cfm=" << BootStageTrace::Hex(cpu.GetCFM())
        << " psr=" << BootStageTrace::Hex(cpu.GetPSR());
    return oss.str();
}

std::string formatHex(uint64_t value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << value;
    return oss.str();
}

void logScanLoopState(const char* phase, const CPUState& cpu, size_t slot,
                      const InstructionEx& instr) {
    std::cout << "[IA64-SCAN] phase=" << phase
              << " ip=" << formatHex(cpu.GetIP())
              << " slot=" << std::dec << slot
              << " instr=\"" << instr.GetDisassembly() << "\""
              << " r1=" << formatHex(cpu.GetGR(1))
              << " r8=" << formatHex(cpu.GetGR(8))
              << " r12=" << formatHex(cpu.GetGR(12))
              << " r16=" << formatHex(cpu.GetGR(16))
              << " r17=" << formatHex(cpu.GetGR(17))
              << " r18=" << formatHex(cpu.GetGR(18))
              << " r19=" << formatHex(cpu.GetGR(19))
              << " r20=" << formatHex(cpu.GetGR(20))
              << " r21=" << formatHex(cpu.GetGR(21))
              << " r22=" << formatHex(cpu.GetGR(22))
              << " r23=" << formatHex(cpu.GetGR(23))
              << " r24=" << formatHex(cpu.GetGR(24))
              << " r25=" << formatHex(cpu.GetGR(25))
              << " r26=" << formatHex(cpu.GetGR(26))
              << " r27=" << formatHex(cpu.GetGR(27))
              << " r32=" << formatHex(cpu.GetGR(32))
              << " r33=" << formatHex(cpu.GetGR(33))
              << " br0=" << formatHex(cpu.GetBR(0))
              << " br4=" << formatHex(cpu.GetBR(4))
              << " br6=" << formatHex(cpu.GetBR(6))
              << " p6=" << cpu.GetPR(6)
              << " p7=" << cpu.GetPR(7)
              << " p8=" << cpu.GetPR(8)
              << " cfm=" << formatHex(cpu.GetCFM())
              << std::dec << std::endl;
}

bool isBranchRegisterTargetInstruction(InstructionType type) {
    return type == InstructionType::BR_COND ||
           type == InstructionType::BR_CALL ||
           type == InstructionType::BR_RET;
}

constexpr EfiGuid EFI_LOADED_IMAGE_PROTOCOL_GUID = {{
    0xA1, 0x31, 0x1B, 0x5B, 0x62, 0x95, 0xD2, 0x11,
    0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
}};
constexpr EfiGuid EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID = {{
    0x22, 0x5B, 0x4E, 0x96, 0x59, 0x64, 0xD2, 0x11,
    0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
}};
constexpr EfiGuid EFI_FILE_INFO_GUID = {{
    0x92, 0x6E, 0x57, 0x09, 0x3F, 0x6D, 0xD2, 0x11,
    0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
}};
constexpr EfiGuid EFI_FILE_SYSTEM_INFO_GUID = {{
    0x93, 0x6E, 0x57, 0x09, 0x3F, 0x6D, 0xD2, 0x11,
    0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B
}};

uint64_t normalizeBranchEntryIP(uint64_t target) {
    return target & ~0xFULL;
}

bool tryResolveIa64FunctionDescriptor(IMemory& memory,
                                      uint64_t target,
                                      uint64_t& codePointer,
                                      uint64_t& descriptorGp) {
    if (target == 0 || (target & 0xFULL) != 0) {
        return false;
    }

    if (memory.GetTotalSize() != 0 && target >= memory.GetTotalSize()) {
        return false;
    }

    try {
        memory.Read(target, reinterpret_cast<uint8_t*>(&codePointer), sizeof(codePointer));
        memory.Read(target + 8, reinterpret_cast<uint8_t*>(&descriptorGp), sizeof(descriptorGp));
    } catch (const std::exception&) {
        return false;
    }

    if (codePointer == 0 || codePointer == target || (codePointer & 0xFULL) != 0) {
        return false;
    }

    if (memory.GetTotalSize() != 0 && codePointer >= memory.GetTotalSize()) {
        return false;
    }

    return true;
}

bool isZeroFilledEfiHandoffBundle(IMemory& memory, uint64_t target) {
    const uint64_t entryIP = normalizeBranchEntryIP(target);
    if (entryIP < EFI_HANDOFF_REGION_BASE || entryIP >= EFI_HANDOFF_REGION_END) {
        return false;
    }

    uint8_t bundle[16] = {};
    try {
        memory.Read(entryIP, bundle, sizeof(bundle));
    } catch (const std::exception&) {
        return false;
    }

    for (uint8_t byte : bundle) {
        if (byte != 0) {
            return false;
        }
    }

    return true;
}

template <size_t N>
const char* lookupEfiSlotName(uint64_t offset, const EfiSlotName (&slots)[N]) {
    for (const EfiSlotName& slot : slots) {
        if (slot.offset == offset) {
            return slot.name;
        }
    }
    return nullptr;
}

std::string describeEfiTableSlot(uint64_t address) {
    static constexpr EfiSlotName runtimeServices[] = {
        {0x18, "GetTime"}, {0x20, "SetTime"}, {0x28, "GetWakeupTime"},
        {0x30, "SetWakeupTime"}, {0x38, "SetVirtualAddressMap"},
        {0x40, "ConvertPointer"}, {0x48, "GetVariable"},
        {0x50, "GetNextVariableName"}, {0x58, "SetVariable"},
        {0x60, "GetNextHighMonotonicCount"}, {0x68, "ResetSystem"},
        {0x70, "UpdateCapsule"}, {0x78, "QueryCapsuleCapabilities"},
        {0x80, "QueryVariableInfo"}
    };
    static constexpr EfiSlotName bootServices[] = {
        {0x18, "RaiseTPL"}, {0x20, "RestoreTPL"}, {0x28, "AllocatePages"},
        {0x30, "FreePages"}, {0x38, "GetMemoryMap"}, {0x40, "AllocatePool"},
        {0x48, "FreePool"}, {0x50, "CreateEvent"}, {0x58, "SetTimer"},
        {0x60, "WaitForEvent"}, {0x68, "SignalEvent"}, {0x70, "CloseEvent"},
        {0x78, "CheckEvent"}, {0x80, "InstallProtocolInterface"},
        {0x88, "ReinstallProtocolInterface"}, {0x90, "UninstallProtocolInterface"},
        {0x98, "HandleProtocol"}, {0xA0, "Reserved"}, {0xA8, "RegisterProtocolNotify"},
        {0xB0, "LocateHandle"}, {0xB8, "LocateDevicePath"},
        {0xC0, "InstallConfigurationTable"}, {0xC8, "LoadImage"},
        {0xD0, "StartImage"}, {0xD8, "Exit"}, {0xE0, "UnloadImage"},
        {0xE8, "ExitBootServices"}, {0xF0, "GetNextMonotonicCount"},
        {0xF8, "Stall"}, {0x100, "SetWatchdogTimer"},
        {0x108, "ConnectController"}, {0x110, "DisconnectController"},
        {0x118, "OpenProtocol"}, {0x120, "CloseProtocol"},
        {0x128, "OpenProtocolInformation"}, {0x130, "ProtocolsPerHandle"},
        {0x138, "LocateHandleBuffer"}, {0x140, "LocateProtocol"},
        {0x148, "InstallMultipleProtocolInterfaces"},
        {0x150, "UninstallMultipleProtocolInterfaces"}, {0x158, "CalculateCrc32"},
        {0x160, "CopyMem"}, {0x168, "SetMem"}, {0x170, "CreateEventEx"}
    };
    static constexpr EfiSlotName simpleTextOut[] = {
        {0x00, "Reset"}, {0x08, "OutputString"}, {0x10, "TestString"},
        {0x18, "QueryMode"}, {0x20, "SetMode"}, {0x28, "SetAttribute"},
        {0x30, "ClearScreen"}, {0x38, "SetCursorPosition"},
        {0x40, "EnableCursor"}, {0x48, "Mode"}
    };
    static constexpr EfiSlotName simpleFs[] = {
        {0x00, "Revision"}, {0x08, "OpenVolume"}
    };
    static constexpr EfiSlotName fileProtocol[] = {
        {0x00, "Revision"}, {0x08, "Open"}, {0x10, "Close"},
        {0x18, "Delete"}, {0x20, "Read"}, {0x28, "Write"},
        {0x30, "GetPosition"}, {0x38, "SetPosition"}, {0x40, "GetInfo"},
        {0x48, "SetInfo"}, {0x50, "Flush"}
    };
    static constexpr EfiSlotName loadedImage[] = {
        {0x00, "Revision"}, {0x08, "ParentHandle"}, {0x10, "SystemTable"},
        {0x18, "DeviceHandle"}, {0x20, "FilePath"}, {0x30, "LoadOptionsSize"},
        {0x38, "LoadOptions"}, {0x40, "ImageBase"}, {0x48, "ImageSize"},
        {0x50, "ImageCodeType/ImageDataType"}, {0x58, "Unload"}
    };

    auto describeRange = [](uint64_t value,
                            uint64_t base,
                            uint64_t size,
                            const char* tableName,
                            const auto& slots) -> std::string {
        if (value < base || value >= base + size) {
            return {};
        }
        const uint64_t offset = value - base;
        std::ostringstream oss;
        oss << tableName << "+0x" << std::hex << offset;
        const char* name = lookupEfiSlotName(offset, slots);
        if (name != nullptr) {
            oss << " (" << name << ")";
        }
        return oss.str();
    };

    std::string description = describeRange(address, EFI_RUNTIME_SERVICES_ADDR, 0x88, "RuntimeServices", runtimeServices);
    if (!description.empty()) {
        return description;
    }
    description = describeRange(address, EFI_BOOT_SERVICES_ADDR, 0x180, "BootServices", bootServices);
    if (!description.empty()) {
        return description;
    }
    description = describeRange(address, EFI_LOADED_IMAGE_PROTOCOL_ADDR, 0x60, "LoadedImage", loadedImage);
    if (!description.empty()) {
        return description;
    }
    description = describeRange(address, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR, 0x10, "SimpleFileSystem", simpleFs);
    if (!description.empty()) {
        return description;
    }
    description = describeRange(address, EFI_ROOT_FILE_PROTOCOL_ADDR, 0x58, "FileProtocol", fileProtocol);
    if (!description.empty()) {
        return description;
    }
    description = describeRange(address, EFI_TEXT_OUTPUT_PROTOCOL_ADDR, 0x50, "SimpleTextOut", simpleTextOut);
    if (!description.empty()) {
        return description;
    }
    if (address >= EFI_TEXT_OUTPUT_MODE_ADDR && address < EFI_TEXT_OUTPUT_MODE_ADDR + 0x18) {
        std::ostringstream oss;
        oss << "SimpleTextOut.Mode+0x" << std::hex << (address - EFI_TEXT_OUTPUT_MODE_ADDR);
        return oss.str();
    }
    return {};
}

std::string describeEfiDescriptor(uint64_t address) {
    if (address == EFI_GET_VARIABLE_STUB_DESC_ADDR) {
        return "GetVariable descriptor";
    }
    if (address == EFI_ALLOCATE_POOL_STUB_DESC_ADDR) {
        return "AllocatePool descriptor";
    }
    if (address == EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR) {
        return "HandleProtocol descriptor";
    }
    if (address == EFI_OPEN_VOLUME_STUB_DESC_ADDR) {
        return "OpenVolume descriptor";
    }
    if (address == EFI_UNSUPPORTED_STUB_DESC_ADDR) {
        return "Unsupported descriptor";
    }
    if (address == EFI_SUCCESS_STUB_DESC_ADDR) {
        return "Success descriptor";
    }
    if (address == EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR) {
        return "SimpleTextOut.OutputString descriptor";
    }
    if (address == EFI_FILE_OPEN_STUB_DESC_ADDR) return "FileProtocol.Open descriptor";
    if (address == EFI_FILE_CLOSE_STUB_DESC_ADDR) return "FileProtocol.Close descriptor";
    if (address == EFI_FILE_READ_STUB_DESC_ADDR) return "FileProtocol.Read descriptor";
    if (address == EFI_FILE_GET_POSITION_STUB_DESC_ADDR) return "FileProtocol.GetPosition descriptor";
    if (address == EFI_FILE_SET_POSITION_STUB_DESC_ADDR) return "FileProtocol.SetPosition descriptor";
    if (address == EFI_FILE_GET_INFO_STUB_DESC_ADDR) return "FileProtocol.GetInfo descriptor";
    if (address == EFI_LOCATE_HANDLE_STUB_DESC_ADDR) return "BootServices.LocateHandle descriptor";
    if (address == EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR) return "BootServices.LocateProtocol descriptor";
    if (address == EFI_GET_MEMORY_MAP_STUB_DESC_ADDR) return "BootServices.GetMemoryMap descriptor";
    if (address == EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR) return "BootServices.ExitBootServices descriptor";
    if (address == EFI_LOAD_IMAGE_STUB_DESC_ADDR) return "BootServices.LoadImage descriptor";
    if (address == EFI_START_IMAGE_STUB_DESC_ADDR) return "BootServices.StartImage descriptor";
    return {};
}

uint64_t efiDescriptorForCodePointer(uint64_t codePointer) {
    if (codePointer == EFI_OPEN_VOLUME_STUB_CODE_ADDR) return EFI_OPEN_VOLUME_STUB_DESC_ADDR;
    if (codePointer == EFI_GET_VARIABLE_STUB_CODE_ADDR) return EFI_GET_VARIABLE_STUB_DESC_ADDR;
    if (codePointer == EFI_ALLOCATE_POOL_STUB_CODE_ADDR) return EFI_ALLOCATE_POOL_STUB_DESC_ADDR;
    if (codePointer == EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR) return EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR;
    if (codePointer == EFI_UNSUPPORTED_STUB_CODE_ADDR) return EFI_UNSUPPORTED_STUB_DESC_ADDR;
    if (codePointer == EFI_SUCCESS_STUB_CODE_ADDR) return EFI_SUCCESS_STUB_DESC_ADDR;
    if (codePointer == EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR) return EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR;
    if (codePointer == EFI_FILE_OPEN_STUB_CODE_ADDR) return EFI_FILE_OPEN_STUB_DESC_ADDR;
    if (codePointer == EFI_FILE_CLOSE_STUB_CODE_ADDR) return EFI_FILE_CLOSE_STUB_DESC_ADDR;
    if (codePointer == EFI_FILE_READ_STUB_CODE_ADDR) return EFI_FILE_READ_STUB_DESC_ADDR;
    if (codePointer == EFI_FILE_GET_POSITION_STUB_CODE_ADDR) return EFI_FILE_GET_POSITION_STUB_DESC_ADDR;
    if (codePointer == EFI_FILE_SET_POSITION_STUB_CODE_ADDR) return EFI_FILE_SET_POSITION_STUB_DESC_ADDR;
    if (codePointer == EFI_FILE_GET_INFO_STUB_CODE_ADDR) return EFI_FILE_GET_INFO_STUB_DESC_ADDR;
    if (codePointer == EFI_LOCATE_HANDLE_STUB_CODE_ADDR) return EFI_LOCATE_HANDLE_STUB_DESC_ADDR;
    if (codePointer == EFI_LOCATE_PROTOCOL_STUB_CODE_ADDR) return EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR;
    if (codePointer == EFI_GET_MEMORY_MAP_STUB_CODE_ADDR) return EFI_GET_MEMORY_MAP_STUB_DESC_ADDR;
    if (codePointer == EFI_EXIT_BOOT_SERVICES_STUB_CODE_ADDR) return EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR;
    if (codePointer == EFI_LOAD_IMAGE_STUB_CODE_ADDR) return EFI_LOAD_IMAGE_STUB_DESC_ADDR;
    if (codePointer == EFI_START_IMAGE_STUB_CODE_ADDR) return EFI_START_IMAGE_STUB_DESC_ADDR;
    return 0;
}

const char* efiStatusName(uint64_t status) {
    switch (status) {
        case EFI_STATUS_SUCCESS: return "EFI_SUCCESS";
        case EFI_STATUS_INVALID_PARAMETER: return "EFI_INVALID_PARAMETER";
        case EFI_STATUS_UNSUPPORTED: return "EFI_UNSUPPORTED";
        case EFI_STATUS_BUFFER_TOO_SMALL: return "EFI_BUFFER_TOO_SMALL";
        case EFI_STATUS_WRITE_PROTECTED: return "EFI_WRITE_PROTECTED";
        case EFI_STATUS_NO_MEDIA: return "EFI_NO_MEDIA";
        case EFI_STATUS_NOT_FOUND: return "EFI_NOT_FOUND";
        default: return "EFI_STATUS_UNKNOWN";
    }
}

std::string normalizeEfiPath(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.front() == '/') {
        path.erase(path.begin());
    }
    return path;
}

std::string joinEfiPath(const std::string& parent, const std::string& child) {
    std::string normalizedChild = normalizeEfiPath(child);
    if (normalizedChild.empty() || normalizedChild == ".") {
        return normalizeEfiPath(parent);
    }
    if (normalizedChild == "/") {
        return {};
    }
    std::string normalizedParent = normalizeEfiPath(parent);
    if (normalizedParent.empty()) {
        return normalizedChild;
    }
    if (normalizedParent.back() != '/') {
        normalizedParent.push_back('/');
    }
    return normalizedParent + normalizedChild;
}

std::vector<uint8_t> makeEfiFileInfoBuffer(const std::string& name,
                                           uint64_t fileSize,
                                           bool isDirectory) {
    std::vector<uint16_t> utf16Name;
    for (char c : name) {
        utf16Name.push_back(static_cast<uint16_t>(static_cast<unsigned char>(c)));
    }
    utf16Name.push_back(0);

    const uint64_t fixedSize = 80;
    const uint64_t totalSize = fixedSize + static_cast<uint64_t>(utf16Name.size() * sizeof(uint16_t));
    std::vector<uint8_t> buffer(static_cast<size_t>(totalSize), 0);
    auto write64 = [&](size_t offset, uint64_t value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    };
    write64(0, totalSize);
    write64(8, fileSize);
    write64(16, fileSize);
    write64(72, isDirectory ? 0x10ULL : 0x1ULL);
    std::memcpy(buffer.data() + fixedSize,
                utf16Name.data(),
                utf16Name.size() * sizeof(uint16_t));
    return buffer;
}

std::vector<uint8_t> makeEfiFileSystemInfoBuffer(uint64_t volumeSize) {
    static const uint16_t volumeLabel[] = { 'g','u','i','d','e','X','O','S','-','b','o','o','t',0 };
    const uint64_t fixedSize = 56;
    const uint64_t totalSize = fixedSize + sizeof(volumeLabel);
    std::vector<uint8_t> buffer(static_cast<size_t>(totalSize), 0);
    auto write64 = [&](size_t offset, uint64_t value) {
        std::memcpy(buffer.data() + offset, &value, sizeof(value));
    };
    write64(0, totalSize);
    buffer[8] = 1; // ReadOnly
    write64(16, volumeSize);
    write64(24, 0);
    write64(32, 512);
    std::memcpy(buffer.data() + fixedSize, volumeLabel, sizeof(volumeLabel));
    return buffer;
}

bool isLoadInstruction(InstructionType type) {
    switch (type) {
        case InstructionType::LD1:
        case InstructionType::LD2:
        case InstructionType::LD4:
        case InstructionType::LD8:
        case InstructionType::LD1_S:
        case InstructionType::LD2_S:
        case InstructionType::LD4_S:
        case InstructionType::LD8_S:
            return true;
        default:
            return false;
    }
}

bool isStoreInstruction(InstructionType type) {
    switch (type) {
        case InstructionType::ST1:
        case InstructionType::ST2:
        case InstructionType::ST4:
        case InstructionType::ST8:
            return true;
        default:
            return false;
    }
}

size_t storeSizeForInstruction(InstructionType type) {
    switch (type) {
        case InstructionType::ST1:
            return 1;
        case InstructionType::ST2:
            return 2;
        case InstructionType::ST4:
            return 4;
        case InstructionType::ST8:
            return 8;
        default:
            return 0;
    }
}

bool rangesOverlap(uint64_t leftStart, size_t leftSize, uint64_t rightStart, size_t rightSize) {
    if (leftSize == 0 || rightSize == 0) {
        return false;
    }
    const uint64_t leftEnd = leftStart > UINT64_MAX - static_cast<uint64_t>(leftSize)
        ? UINT64_MAX
        : leftStart + static_cast<uint64_t>(leftSize);
    const uint64_t rightEnd = rightStart > UINT64_MAX - static_cast<uint64_t>(rightSize)
        ? UINT64_MAX
        : rightStart + static_cast<uint64_t>(rightSize);
    return leftStart < rightEnd && rightStart < leftEnd;
}

bool isAdvancedLoadCheckInstruction(InstructionType type) {
    return type == InstructionType::CHK_A_NC ||
           type == InstructionType::CHK_A_CLR;
}

uint64_t readCallerOutputRegister(const CPUState& cpu, size_t index) {
    const size_t reg = 32 + cpu.GetSOL() + index;
    return reg < NUM_GENERAL_REGISTERS ? cpu.GetGR(reg) : 0;
}

bool readEfiGuid(IMemory& memory, uint64_t address, EfiGuid& guid) {
    if (address == 0) {
        return false;
    }

    try {
        memory.Read(address, guid.data(), guid.size());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool readGuestU8(IMemory& memory, uint64_t address, uint8_t& value) {
    try {
        memory.Read(address, &value, sizeof(value));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool readGuestU16(IMemory& memory, uint64_t address, uint16_t& value) {
    try {
        memory.Read(address, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool readGuestU32(IMemory& memory, uint64_t address, uint32_t& value) {
    try {
        memory.Read(address, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool readGuestU64(IMemory& memory, uint64_t address, uint64_t& value) {
    try {
        memory.Read(address, reinterpret_cast<uint8_t*>(&value), sizeof(value));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string formatEfiGuid(const EfiGuid& guid) {
    std::ostringstream oss;
    oss << std::hex;
    for (size_t i = 0; i < guid.size(); ++i) {
        if (i != 0) {
            oss << ':';
        }
        const int value = guid[i];
        if (value < 0x10) {
            oss << '0';
        }
        oss << value;
    }
    return oss.str();
}

bool isZeroEfiGuid(const EfiGuid& guid) {
    for (uint8_t byte : guid) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string readHexBytesPreview(IMemory& memory, uint64_t address, size_t count) {
    if (address == 0 || count == 0) {
        return {};
    }

    std::ostringstream oss;
    oss << std::hex;
    for (size_t i = 0; i < count; ++i) {
        uint8_t byte = 0;
        try {
            memory.Read(address + i, &byte, sizeof(byte));
        } catch (const std::exception&) {
            if (i == 0) {
                return "<unreadable>";
            }
            oss << " ...";
            break;
        }
        if (i != 0) {
            oss << ' ';
        }
        const int value = byte;
        if (value < 0x10) {
            oss << '0';
        }
        oss << value;
    }
    return oss.str();
}

std::string readUtf16CodeUnitPreview(IMemory& memory, uint64_t address, size_t count) {
    if (address == 0 || count == 0) {
        return {};
    }

    std::ostringstream oss;
    oss << std::hex;
    for (size_t i = 0; i < count; ++i) {
        uint16_t ch = 0;
        try {
            memory.Read(address + i * sizeof(ch),
                        reinterpret_cast<uint8_t*>(&ch),
                        sizeof(ch));
        } catch (const std::exception&) {
            if (i == 0) {
                return "<unreadable>";
            }
            oss << " ...";
            break;
        }
        if (i != 0) {
            oss << ' ';
        }
        if (ch < 0x1000) {
            oss << '0';
        }
        if (ch < 0x100) {
            oss << '0';
        }
        if (ch < 0x10) {
            oss << '0';
        }
        oss << ch;
        if (ch == 0) {
            break;
        }
    }
    return oss.str();
}

std::string readUtf16Preview(IMemory& memory, uint64_t address) {
    if (address == 0) {
        return {};
    }

    std::string preview;
    for (size_t i = 0; i < 80; ++i) {
        uint16_t ch = 0;
        try {
            memory.Read(address + i * sizeof(ch),
                        reinterpret_cast<uint8_t*>(&ch),
                        sizeof(ch));
        } catch (const std::exception&) {
            if (preview.empty()) {
                preview = "<unreadable>";
            }
            break;
        }

        if (ch == 0) {
            break;
        }
        if (ch == '\r') {
            preview += "\\r";
        } else if (ch == '\n') {
            preview += "\\n";
        } else if (ch >= 0x20 && ch <= 0x7E) {
            preview.push_back(static_cast<char>(ch));
        } else {
            preview.push_back('?');
        }
    }

    return preview;
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
        if (!readGuestU8(memory, nodeAddress, type) ||
            !readGuestU8(memory, nodeAddress + 1, subtype) ||
            !readGuestU16(memory, nodeAddress + 2, length)) {
            if (nodeIndex != 0) {
                oss << " ";
            }
            oss << "<unreadable@0x" << nodeAddress << ">";
            break;
        }

        if (nodeIndex != 0) {
            oss << " ";
        }
        oss << "{type=0x" << static_cast<unsigned>(type)
            << ",sub=0x" << static_cast<unsigned>(subtype)
            << ",len=0x" << length;
        if (type == 0x04 && subtype == 0x04 && length >= 4) {
            const std::string filePath = readUtf16Preview(memory, nodeAddress + 4);
            if (!filePath.empty()) {
                oss << ",path=\"" << filePath << "\"";
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

std::string describeLoadedImageProtocol(IMemory& memory, uint64_t address) {
    uint32_t revision = 0;
    uint64_t parentHandle = 0;
    uint64_t systemTable = 0;
    uint64_t deviceHandle = 0;
    uint64_t filePath = 0;
    uint32_t loadOptionsSize = 0;
    uint64_t loadOptions = 0;
    uint64_t imageBase = 0;
    uint64_t imageSize = 0;
    uint32_t imageCodeType = 0;
    uint32_t imageDataType = 0;
    uint64_t unload = 0;
    uint64_t ext60 = 0;
    uint64_t ext68 = 0;

    const bool readable =
        readGuestU32(memory, address + 0x00, revision) &&
        readGuestU64(memory, address + 0x08, parentHandle) &&
        readGuestU64(memory, address + 0x10, systemTable) &&
        readGuestU64(memory, address + 0x18, deviceHandle) &&
        readGuestU64(memory, address + 0x20, filePath) &&
        readGuestU32(memory, address + 0x30, loadOptionsSize) &&
        readGuestU64(memory, address + 0x38, loadOptions) &&
        readGuestU64(memory, address + 0x40, imageBase) &&
        readGuestU64(memory, address + 0x48, imageSize) &&
        readGuestU32(memory, address + 0x50, imageCodeType) &&
        readGuestU32(memory, address + 0x54, imageDataType) &&
        readGuestU64(memory, address + 0x58, unload);

    if (!readable) {
        std::ostringstream oss;
        oss << "LoadedImage{addr=0x" << std::hex << address
            << " unreadable}";
        return oss.str();
    }

    readGuestU64(memory, address + 0x60, ext60);
    readGuestU64(memory, address + 0x68, ext68);

    std::ostringstream oss;
    oss << "LoadedImage{addr=0x" << std::hex << address
        << " Revision=0x" << revision
        << " ParentHandle=0x" << parentHandle
        << " SystemTable=0x" << systemTable
        << " DeviceHandle=0x" << deviceHandle
        << " FilePath=0x" << filePath
        << " LoadOptionsSize=0x" << loadOptionsSize
        << " LoadOptions=0x" << loadOptions
        << " ImageBase=0x" << imageBase
        << " ImageSize=0x" << imageSize
        << " CodeType=0x" << imageCodeType
        << " DataType=0x" << imageDataType
        << " Unload=0x" << unload
        << " Ext60=0x" << ext60
        << " Ext68=0x" << ext68
        << " FilePathNodes=" << describeEfiDevicePath(memory, filePath);
    if (loadOptions != 0 && loadOptionsSize != 0) {
        oss << " LoadOptionsUtf16=\"" << readUtf16Preview(memory, loadOptions) << "\""
            << " LoadOptionsRaw=" << readHexBytesPreview(memory, loadOptions,
                                                         std::min<size_t>(loadOptionsSize, 32));
    } else {
        oss << " LoadOptions=empty";
    }
    oss << "}";
    return oss.str();
}

std::string describeSimpleFileSystemProtocol(IMemory& memory, uint64_t address) {
    uint64_t revision = 0;
    uint64_t openVolume = 0;
    std::ostringstream oss;
    oss << "SimpleFS{addr=0x" << std::hex << address;
    if (readGuestU64(memory, address + 0x00, revision) &&
        readGuestU64(memory, address + 0x08, openVolume)) {
        oss << " Revision=0x" << revision
            << " OpenVolume=0x" << openVolume;
    } else {
        oss << " unreadable";
    }
    oss << "}";
    return oss.str();
}

bool mirrorTextToVmConsole(IMemory& memory, const std::string& text, uint64_t& consoleAddress) {
    consoleAddress = 0;
    if (text.empty()) {
        return false;
    }

    const size_t memorySize = memory.GetTotalSize();
    if (memorySize <= 0x200) {
        return false;
    }

    consoleAddress = static_cast<uint64_t>(memorySize - 0x200);
    try {
        uint64_t cursor = consoleAddress;
        for (char ch : text) {
            const char out = ch == '\0' ? '?' : ch;
            memory.Write(cursor, reinterpret_cast<const uint8_t*>(&out), sizeof(out));
            ++cursor;
        }
        if (text.back() != '\n') {
            const char newline = '\n';
            memory.Write(cursor, reinterpret_cast<const uint8_t*>(&newline), sizeof(newline));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

const char* efiGlyphRows(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'B': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
        case 'C': return "01111" "10000" "10000" "10000" "10000" "10000" "01111";
        case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
        case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
        case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
        case 'G': return "01111" "10000" "10000" "10011" "10001" "10001" "01111";
        case 'H': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
        case 'I': return "11111" "00100" "00100" "00100" "00100" "00100" "11111";
        case 'J': return "00111" "00010" "00010" "00010" "10010" "10010" "01100";
        case 'K': return "10001" "10010" "10100" "11000" "10100" "10010" "10001";
        case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
        case 'M': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
        case 'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
        case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
        case 'P': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
        case 'Q': return "01110" "10001" "10001" "10001" "10101" "10010" "01101";
        case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
        case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
        case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
        case 'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
        case 'V': return "10001" "10001" "10001" "10001" "01010" "01010" "00100";
        case 'W': return "10001" "10001" "10001" "10101" "10101" "10101" "01010";
        case 'X': return "10001" "01010" "00100" "00100" "00100" "01010" "10001";
        case 'Y': return "10001" "01010" "00100" "00100" "00100" "00100" "00100";
        case 'Z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
        case '0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
        case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
        case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
        case '3': return "11110" "00001" "00001" "01110" "00001" "00001" "11110";
        case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
        case '5': return "11111" "10000" "10000" "11110" "00001" "00001" "11110";
        case '6': return "01111" "10000" "10000" "11110" "10001" "10001" "01110";
        case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
        case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
        case '9': return "01110" "10001" "10001" "01111" "00001" "00001" "11110";
        case ':': return "00000" "00100" "00100" "00000" "00100" "00100" "00000";
        case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
        case '-': return "00000" "00000" "00000" "11111" "00000" "00000" "00000";
        case '/': return "00001" "00010" "00010" "00100" "01000" "01000" "10000";
        case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
        default:  return "11111" "00001" "00010" "00100" "00100" "00000" "00100";
    }
}

bool mirrorTextToFramebuffer(IMemory& memory,
                             const std::string& text,
                             size_t lineIndex,
                             uint64_t& framebufferAddress) {
    framebufferAddress = 0xB8000000ULL;
    if (text.empty()) {
        return false;
    }

    constexpr size_t width = 640;
    constexpr size_t height = 480;
    constexpr size_t scale = 2;
    constexpr uint32_t bg = 0xFF101820U;
    constexpr uint32_t fg = 0xFF6EE7B7U;
    const size_t y = 168 + (lineIndex % 10) * 22;
    const std::string line = "EFI: " + text.substr(0, 70);

    auto writePixel = [&](size_t x, size_t py, uint32_t color) {
        if (x >= width || py >= height) {
            return;
        }
        const uint64_t address = framebufferAddress + ((py * width + x) * sizeof(color));
        memory.Write(address, reinterpret_cast<const uint8_t*>(&color), sizeof(color));
    };

    try {
        for (size_t py = y > 4 ? y - 4 : 0; py < y + 18 && py < height; ++py) {
            for (size_t x = 24; x < 616; ++x) {
                writePixel(x, py, bg);
            }
        }

        size_t cursorX = 32;
        for (char c : line) {
            const char* glyph = efiGlyphRows(c);
            for (size_t row = 0; row < 7; ++row) {
                for (size_t col = 0; col < 5; ++col) {
                    if (glyph[row * 5 + col] != '1') {
                        continue;
                    }
                    for (size_t sy = 0; sy < scale; ++sy) {
                        for (size_t sx = 0; sx < scale; ++sx) {
                            writePixel(cursorX + col * scale + sx,
                                       y + row * scale + sy,
                                       fg);
                        }
                    }
                }
            }
            cursorX += 6 * scale;
            if (cursorX >= 616) {
                break;
            }
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string readUtf16AsciiString(IMemory& memory, uint64_t address) {
    if (address == 0) {
        return {};
    }

    std::string value;
    for (size_t i = 0; i < 64; ++i) {
        uint16_t ch = 0;
        try {
            memory.Read(address + i * sizeof(ch),
                        reinterpret_cast<uint8_t*>(&ch),
                        sizeof(ch));
        } catch (const std::exception&) {
            return {};
        }

        if (ch == 0) {
            return value;
        }
        if (ch < 0x20 || ch > 0x7E) {
            return {};
        }
        value.push_back(static_cast<char>(ch));
    }

    return value;
}

void appendLe16(std::vector<uint8_t>& data, uint16_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void appendLe32(std::vector<uint8_t>& data, uint32_t value) {
    data.push_back(static_cast<uint8_t>(value & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

std::vector<uint8_t> makeBoot0000LoadOption() {
    static const uint16_t description[] = {
        'g','u','i','d','e','X','O','S',' ','I','A','-','6','4',' ','I','S','O',0
    };
    static const uint16_t bootPath[] = {
        '\\','E','F','I','\\','B','O','O','T','\\',
        'B','O','O','T','I','A','6','4','.','E','F','I',0
    };

    std::vector<uint8_t> path;
    path.push_back(0x04); // MEDIA_DEVICE_PATH
    path.push_back(0x04); // MEDIA_FILEPATH_DP
    appendLe16(path, static_cast<uint16_t>(4 + sizeof(bootPath)));
    const uint8_t* bootPathBytes = reinterpret_cast<const uint8_t*>(bootPath);
    path.insert(path.end(), bootPathBytes, bootPathBytes + sizeof(bootPath));
    path.push_back(0x7F); // END_DEVICE_PATH_TYPE
    path.push_back(0xFF); // END_ENTIRE_DEVICE_PATH_SUBTYPE
    appendLe16(path, 4);

    std::vector<uint8_t> option;
    appendLe32(option, 1U); // LOAD_OPTION_ACTIVE
    appendLe16(option, static_cast<uint16_t>(path.size()));
    const uint8_t* descBytes = reinterpret_cast<const uint8_t*>(description);
    option.insert(option.end(), descBytes, descBytes + sizeof(description));
    option.insert(option.end(), path.begin(), path.end());
    return option;
}

std::vector<uint8_t> makeAsciiVariableData(const char* value) {
    const size_t size = std::strlen(value) + 1;
    std::vector<uint8_t> data;
    data.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        data.push_back(static_cast<uint8_t>(value[i]));
    }
    return data;
}

bool isLanguageVariableProbe(const std::string& variableNameText, uint64_t currentIP) {
    return variableNameText == "PlatformLang" ||
           variableNameText == "PlatformLangCodes" ||
           (variableNameText == "P" && currentIP == 0x1b00ULL);
}

uint64_t alignUp(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void finishCountedLoopTraceIfActive() {
    if (g_countedLoopTrace.active && g_countedLoopTrace.suppressed > 0) {
        std::cout << "[TRACE] suppressed " << g_countedLoopTrace.suppressed
                  << " counted-loop instruction trace lines for range 0x"
                  << std::hex << g_countedLoopTrace.start << "-0x"
                  << g_countedLoopTrace.end << std::dec << std::endl;
    }

    g_countedLoopTrace = CountedLoopTraceState();
}

} // namespace

// ============================================================================
// IA64ISAState Implementation
// ============================================================================

IA64ISAState::IA64ISAState()
    : cpuState_()
    , currentBundle_()
    , currentSlot_(0)
    , bundleValid_(false)
    , predicateGroupSnapshot_()
    , pendingInterrupts_()
    , interruptVectorBase_(0) {
    predicateGroupSnapshot_.fill(false);
    predicateGroupSnapshot_[0] = true;
}

IA64ISAState::IA64ISAState(const CPUState& cpuState)
    : cpuState_(cpuState)
    , currentBundle_()
    , currentSlot_(0)
    , bundleValid_(false)
    , predicateGroupSnapshot_()
    , pendingInterrupts_()
    , interruptVectorBase_(0) {
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        predicateGroupSnapshot_[i] = cpuState_.GetPR(i);
    }
}

std::unique_ptr<ISAState> IA64ISAState::clone() const {
    auto cloned = std::make_unique<IA64ISAState>(cpuState_);
    cloned->currentBundle_ = currentBundle_;
    cloned->currentSlot_ = currentSlot_;
    cloned->bundleValid_ = bundleValid_;
    cloned->predicateGroupSnapshot_ = predicateGroupSnapshot_;
    cloned->pendingInterrupts_ = pendingInterrupts_;
    cloned->interruptVectorBase_ = interruptVectorBase_;
    return cloned;
}

void IA64ISAState::serialize(uint8_t* buffer) const {
    if (!buffer) {
        throw std::invalid_argument("Null buffer in serialize");
    }
    
    size_t offset = 0;
    
    // Serialize CPUState registers
    // General registers (128 * 8 bytes)
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; ++i) {
        uint64_t val = cpuState_.GetGR(i);
        std::memcpy(buffer + offset, &val, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }
    
    // Predicate registers (64 * 1 byte, padded)
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        uint8_t val = cpuState_.GetPR(i) ? 1 : 0;
        buffer[offset++] = val;
    }
    
    // Branch registers (8 * 8 bytes)
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) {
        uint64_t val = cpuState_.GetBR(i);
        std::memcpy(buffer + offset, &val, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }

    // Application registers (128 * 8 bytes) keep RSE-visible state intact
    for (size_t i = 0; i < NUM_APPLICATION_REGISTERS; ++i) {
        uint64_t val = cpuState_.GetAR(i);
        std::memcpy(buffer + offset, &val, sizeof(uint64_t));
        offset += sizeof(uint64_t);
    }
    
    // Special registers
    uint64_t ip = cpuState_.GetIP();
    std::memcpy(buffer + offset, &ip, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    uint64_t cfm = cpuState_.GetCFM();
    std::memcpy(buffer + offset, &cfm, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    uint64_t psr = cpuState_.GetPSR();
    std::memcpy(buffer + offset, &psr, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Runtime state
    std::memcpy(buffer + offset, &currentSlot_, sizeof(size_t));
    offset += sizeof(size_t);
    
    uint8_t valid = bundleValid_ ? 1 : 0;
    buffer[offset++] = valid;

    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        buffer[offset++] = predicateGroupSnapshot_[i] ? 1 : 0;
    }
    
    std::memcpy(buffer + offset, &interruptVectorBase_, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Pending interrupts
    size_t numInterrupts = pendingInterrupts_.size();
    std::memcpy(buffer + offset, &numInterrupts, sizeof(size_t));
    offset += sizeof(size_t);
    
    if (numInterrupts > 0) {
        std::memcpy(buffer + offset, pendingInterrupts_.data(), numInterrupts);
        offset += numInterrupts;
    }
}

void IA64ISAState::deserialize(const uint8_t* buffer) {
    if (!buffer) {
        throw std::invalid_argument("Null buffer in deserialize");
    }
    
    size_t offset = 0;
    
    // Deserialize CPUState registers
    // General registers
    for (size_t i = 0; i < NUM_GENERAL_REGISTERS; ++i) {
        uint64_t val;
        std::memcpy(&val, buffer + offset, sizeof(uint64_t));
        cpuState_.SetGR(i, val);
        offset += sizeof(uint64_t);
    }
    
    // Predicate registers
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        uint8_t val = buffer[offset++];
        cpuState_.SetPR(i, val != 0);
    }
    
    // Branch registers
    for (size_t i = 0; i < NUM_BRANCH_REGISTERS; ++i) {
        uint64_t val;
        std::memcpy(&val, buffer + offset, sizeof(uint64_t));
        cpuState_.SetBR(i, val);
        offset += sizeof(uint64_t);
    }

    // Application registers
    for (size_t i = 0; i < NUM_APPLICATION_REGISTERS; ++i) {
        uint64_t val;
        std::memcpy(&val, buffer + offset, sizeof(uint64_t));
        cpuState_.SetAR(i, val);
        offset += sizeof(uint64_t);
    }
    
    // Special registers
    uint64_t ip;
    std::memcpy(&ip, buffer + offset, sizeof(uint64_t));
    cpuState_.SetIP(ip);
    offset += sizeof(uint64_t);
    
    uint64_t cfm;
    std::memcpy(&cfm, buffer + offset, sizeof(uint64_t));
    cpuState_.SetCFM(cfm);
    offset += sizeof(uint64_t);
    
    uint64_t psr;
    std::memcpy(&psr, buffer + offset, sizeof(uint64_t));
    cpuState_.SetPSR(psr);
    offset += sizeof(uint64_t);
    
    // Runtime state
    std::memcpy(&currentSlot_, buffer + offset, sizeof(size_t));
    offset += sizeof(size_t);
    
    uint8_t valid = buffer[offset++];
    bundleValid_ = (valid != 0);

    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        predicateGroupSnapshot_[i] = buffer[offset++] != 0;
    }
    
    std::memcpy(&interruptVectorBase_, buffer + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    
    // Pending interrupts
    size_t numInterrupts;
    std::memcpy(&numInterrupts, buffer + offset, sizeof(size_t));
    offset += sizeof(size_t);
    
    pendingInterrupts_.clear();
    if (numInterrupts > 0) {
        pendingInterrupts_.resize(numInterrupts);
        std::memcpy(pendingInterrupts_.data(), buffer + offset, numInterrupts);
        offset += numInterrupts;
    }
}

size_t IA64ISAState::getStateSize() const {
    size_t size = 0;
    
    // CPUState
    size += NUM_GENERAL_REGISTERS * sizeof(uint64_t);  // GRs
    size += NUM_PREDICATE_REGISTERS;                   // PRs (padded to bytes)
    size += NUM_BRANCH_REGISTERS * sizeof(uint64_t);   // BRs
    size += NUM_APPLICATION_REGISTERS * sizeof(uint64_t); // ARs
    size += 3 * sizeof(uint64_t);                      // IP, CFM, PSR
    
    // Runtime state
    size += sizeof(size_t);      // currentSlot_
    size += 1;                   // bundleValid_
    size += NUM_PREDICATE_REGISTERS; // predicate group snapshot
    size += sizeof(uint64_t);    // interruptVectorBase_
    size += sizeof(size_t);      // pendingInterrupts_ count
    size += pendingInterrupts_.size();  // interrupt data
    
    return size;
}

std::string IA64ISAState::toString() const {
    std::stringstream ss;
    
    ss << "IA-64 CPU State:\n";
    ss << "  IP: 0x" << std::hex << cpuState_.GetIP() << std::dec << "\n";
    ss << "  CFM: 0x" << std::hex << cpuState_.GetCFM() << std::dec << "\n";
    ss << "  RSE: rsc=0x" << std::hex << cpuState_.GetRSC()
       << " bsp=0x" << cpuState_.GetBSP()
       << " bspstore=0x" << cpuState_.GetBSPSTORE()
       << " rnat=0x" << cpuState_.GetRNAT()
       << " pfs=0x" << cpuState_.GetPFS() << std::dec << "\n";
    ss << "  PSR: 0x" << std::hex << cpuState_.GetPSR() << std::dec << "\n";
    
    ss << "  General Registers:\n";
    for (size_t i = 0; i < 8; ++i) {
        ss << "    GR" << i << ": 0x" << std::hex << cpuState_.GetGR(i) << std::dec << "\n";
    }
    
    ss << "  Bundle State:\n";
    ss << "    Slot: " << currentSlot_ << "\n";
    ss << "    Valid: " << (bundleValid_ ? "yes" : "no") << "\n";
    
    ss << "  Interrupts:\n";
    ss << "    Pending: " << pendingInterrupts_.size() << "\n";
    ss << "    Vector Base: 0x" << std::hex << interruptVectorBase_ << std::dec << "\n";
    
    return ss.str();
}

void IA64ISAState::reset() {
    cpuState_.Reset();
    currentBundle_ = Bundle();
    currentSlot_ = 0;
    bundleValid_ = false;
    predicateGroupSnapshot_.fill(false);
    predicateGroupSnapshot_[0] = true;
    pendingInterrupts_.clear();
    interruptVectorBase_ = 0;
}

// ============================================================================
// IA64ISAPlugin Implementation
// ============================================================================

IA64ISAPlugin::IA64ISAPlugin(IDecoder& decoder)
    : decoder_(decoder)
    , state_()
    , syscallDispatcher_(nullptr)
    , profiler_(nullptr)
    , cachedInstruction_()
    , hasCachedInstruction_(false)
    , pendingCallInputs_()
    , efiPoolNext_(EFI_POOL_BASE)
    , efiTextOutputCalls_(0)
    , efiTextOutputMirrored_(0)
    , efiTextOutputFramebuffer_(0)
    , efiOpenVolumeCalls_(0)
    , efiSimpleFsProtocolReturns_(0)
    , efiGenericSuccessCalls_(0)
    , efiGenericUnsupportedCalls_(0)
    , efiZeroGuidProtocolCalls_(0)
    , efiHandleProtocolCalls_(0)
    , efiLocateHandleCalls_(0)
    , efiLocateProtocolCalls_(0)
    , efiGetMemoryMapCalls_(0)
    , efiExitBootServicesCalls_(0)
    , efiAllocatePoolCalls_(0)
    , efiFreePoolCalls_(0)
    , efiLoadImageCalls_(0)
    , efiStartImageCalls_(0)
    , efiFileOpenCalls_(0)
    , efiFileReadCalls_(0)
    , efiFileGetInfoCalls_(0)
    , efiFileCloseCalls_(0)
    , efiFileSetPositionCalls_(0)
    , efiFileGetPositionCalls_(0)
    , efiFirstSuccessfulFileOpen_(0)
    , efiTotalFileBytesRead_(0)
    , descriptorCallCount_(0)
    , gpSwitchCount_(0)
    , suspiciousGpCount_(0)
    , unknownRegionCallCount_(0)
    , recoveredLoadStoreCount_(0)
    , lastEfiCallName_()
    , lastEfiCallIP_(0)
    , lastDescriptorAddress_(0)
    , lastDescriptorCode_(0)
    , lastDescriptorGp_(0)
    , lastBranchTargets_()
    , recentInstructionSequenceRepeatCount_(0)
    , efiFileHandles_()
    , efiBootImage_()
    , efiBootFat_(nullptr)
    , callFrameStack_() {
}

IA64ISAPlugin::IA64ISAPlugin(IDecoder& decoder, 
                             SyscallDispatcher* syscallDispatcher,
                             Profiler* profiler)
    : decoder_(decoder)
    , state_()
    , syscallDispatcher_(syscallDispatcher)
    , profiler_(profiler)
    , cachedInstruction_()
    , hasCachedInstruction_(false)
    , pendingCallInputs_()
    , efiPoolNext_(EFI_POOL_BASE)
    , efiTextOutputCalls_(0)
    , efiTextOutputMirrored_(0)
    , efiTextOutputFramebuffer_(0)
    , efiOpenVolumeCalls_(0)
    , efiSimpleFsProtocolReturns_(0)
    , efiGenericSuccessCalls_(0)
    , efiGenericUnsupportedCalls_(0)
    , efiZeroGuidProtocolCalls_(0)
    , efiHandleProtocolCalls_(0)
    , efiLocateHandleCalls_(0)
    , efiLocateProtocolCalls_(0)
    , efiGetMemoryMapCalls_(0)
    , efiExitBootServicesCalls_(0)
    , efiAllocatePoolCalls_(0)
    , efiFreePoolCalls_(0)
    , efiLoadImageCalls_(0)
    , efiStartImageCalls_(0)
    , efiFileOpenCalls_(0)
    , efiFileReadCalls_(0)
    , efiFileGetInfoCalls_(0)
    , efiFileCloseCalls_(0)
    , efiFileSetPositionCalls_(0)
    , efiFileGetPositionCalls_(0)
    , efiFirstSuccessfulFileOpen_(0)
    , efiTotalFileBytesRead_(0)
    , descriptorCallCount_(0)
    , gpSwitchCount_(0)
    , suspiciousGpCount_(0)
    , unknownRegionCallCount_(0)
    , recoveredLoadStoreCount_(0)
    , lastEfiCallName_()
    , lastEfiCallIP_(0)
    , lastDescriptorAddress_(0)
    , lastDescriptorCode_(0)
    , lastDescriptorGp_(0)
    , lastBranchTargets_()
    , recentInstructionSequenceRepeatCount_(0)
    , efiFileHandles_()
    , efiBootImage_()
    , efiBootFat_(nullptr)
    , callFrameStack_() {
}

void IA64ISAPlugin::reset() {
    state_.reset();
    hasCachedInstruction_ = false;
    pendingCallInputs_.clear();
    recentInstructions_.clear();
    recentTrackedRegisterWrites_.clear();
    recentInstructionSequenceRepeatCount_ = 0;
    efiPoolNext_ = EFI_POOL_BASE;
    efiTextOutputCalls_ = 0;
    efiTextOutputMirrored_ = 0;
    efiTextOutputFramebuffer_ = 0;
    efiOpenVolumeCalls_ = 0;
    efiSimpleFsProtocolReturns_ = 0;
    efiGenericSuccessCalls_ = 0;
    efiGenericUnsupportedCalls_ = 0;
    efiZeroGuidProtocolCalls_ = 0;
    efiHandleProtocolCalls_ = 0;
    efiLocateHandleCalls_ = 0;
    efiLocateProtocolCalls_ = 0;
    efiGetMemoryMapCalls_ = 0;
    efiExitBootServicesCalls_ = 0;
    efiAllocatePoolCalls_ = 0;
    efiFreePoolCalls_ = 0;
    efiLoadImageCalls_ = 0;
    efiStartImageCalls_ = 0;
    efiFileOpenCalls_ = 0;
    efiFileReadCalls_ = 0;
    efiFileGetInfoCalls_ = 0;
    efiFileCloseCalls_ = 0;
    efiFileSetPositionCalls_ = 0;
    efiFileGetPositionCalls_ = 0;
    efiFirstSuccessfulFileOpen_ = 0;
    efiTotalFileBytesRead_ = 0;
    descriptorCallCount_ = 0;
    gpSwitchCount_ = 0;
    suspiciousGpCount_ = 0;
    unknownRegionCallCount_ = 0;
    recoveredLoadStoreCount_ = 0;
    lastEfiCallName_.clear();
    lastEfiCallIP_ = 0;
    lastDescriptorAddress_ = 0;
    lastDescriptorCode_ = 0;
    lastDescriptorGp_ = 0;
    lastBranchTargets_.clear();
    recentInstructionSequenceRepeatCount_ = 0;
    efiFileHandles_.clear();
    efiBootImage_.clear();
    efiBootFat_.reset();
    callFrameStack_.clear();
}

ISADecodeResult IA64ISAPlugin::decode(IMemory& memory) {
    ISADecodeResult result;
    
    try {
        // Fetch bundle if needed
        if (!state_.bundleValid_ || state_.currentSlot_ >= state_.currentBundle_.instructions.size()) {
            fetchBundle(memory);
            state_.currentSlot_ = 0;
        }
        
        // Check if we have instructions
        if (state_.currentBundle_.instructions.empty()) {
            result.valid = false;
            result.instructionAddress = state_.getCPUState().GetIP();
            result.instructionLength = 0;
            result.disassembly = "<invalid bundle>";
            result.internalData = nullptr;
            return result;
        }
        
        // Get current instruction from bundle
        const InstructionEx& instr = state_.currentBundle_.instructions[state_.currentSlot_];
        
        // Cache the instruction for execute()
        cachedInstruction_ = instr;
        hasCachedInstruction_ = true;
        
        // Fill in result
        result.valid = true;
        result.instructionAddress = state_.getCPUState().GetIP();
        result.instructionLength = 16;  // IA-64 bundles are 16 bytes (we advance by bundle)
        result.disassembly = instr.GetDisassembly();
        result.internalData = &cachedInstruction_;
        {
            std::ostringstream ctx;
            ctx << "ip=" << BootStageTrace::Hex(result.instructionAddress)
                << " slot=" << state_.currentSlot_
                << " type=" << static_cast<int>(instr.GetType())
                << " raw=" << BootStageTrace::Hex(instr.GetRawBits())
                << " disasm=\"" << result.disassembly << "\" "
                << cpuSummary(state_.getCPUState());
            BootStageTrace::Stage(110, "First instruction decoded", ctx.str());
        }
        
    } catch (const std::exception& e) {
        result.valid = false;
        result.instructionAddress = state_.getCPUState().GetIP();
        result.instructionLength = 0;
        result.disassembly = std::string("<decode error: ") + e.what() + ">";
        result.internalData = nullptr;
        BootStageTrace::EventOnce(
            "FIRST_FAULT_TRAP",
            "kind=decode ip=" + BootStageTrace::Hex(result.instructionAddress) +
            " reason=\"" + std::string(e.what()) + "\" " + cpuSummary(state_.getCPUState()));
    }
    
    return result;
}

size_t IA64ISAPlugin::getInstructionLength(IMemory& memory, uint64_t address) {
    // IA-64 bundles are always 16 bytes (128 bits)
    // Individual instructions within bundles are 41 bits each
    // For this interface, we return the bundle size
    return 16;
}

ISAExecutionResult IA64ISAPlugin::execute(IMemory& memory, const ISADecodeResult& decodeResult) {
    if (!decodeResult.valid) {
        return ISAExecutionResult::EXCEPTION;
    }
    
    if (!hasCachedInstruction_) {
        std::cerr << "Warning: execute() called without prior decode()\n";
        return ISAExecutionResult::EXCEPTION;
    }
    
    try {
        // Profile instruction execution
        if (isProfilingEnabled()) {
            profiler_->recordInstructionExecution(
                decodeResult.instructionAddress, 
                decodeResult.disassembly
            );
        }
        
        if (g_countedLoopTrace.active &&
            (decodeResult.instructionAddress < g_countedLoopTrace.start ||
             decodeResult.instructionAddress > g_countedLoopTrace.end)) {
            finishCountedLoopTraceIfActive();
        }

        const bool suppressCountedLoopTrace =
            g_countedLoopTrace.active &&
            decodeResult.instructionAddress >= g_countedLoopTrace.start &&
            decodeResult.instructionAddress <= g_countedLoopTrace.end &&
            state_.getCPUState().GetAR(65) > 1;

        if (suppressCountedLoopTrace) {
            ++g_countedLoopTrace.suppressed;
        } else {
            std::cout << "[IP=0x" << std::hex << decodeResult.instructionAddress << std::dec
                      << ", Slot=" << state_.currentSlot_ << "] "
                      << decodeResult.disassembly << std::endl;
        }

        if (decodeResult.instructionAddress == 0x36CC0ULL) {
            const uint8_t templateField = static_cast<uint8_t>(state_.currentBundle_.templateType);
            std::cout << "[IA64-BUNDLE] ip=" << formatHex(decodeResult.instructionAddress)
                      << " template=" << formatHex(static_cast<uint64_t>(templateField))
                      << std::endl;
            for (size_t i = 0; i < state_.currentBundle_.instructions.size(); ++i) {
                const InstructionEx& bundleInstr = state_.currentBundle_.instructions[i];
                std::cout << "[IA64-BUNDLE] slot=" << i
                          << " raw=" << formatHex(bundleInstr.GetRawBits())
                          << " type=" << static_cast<int>(bundleInstr.GetType())
                          << " disasm=" << bundleInstr.GetDisassembly()
                          << std::endl;
            }
        }
        const bool tracePostSimpleFsPath =
            (decodeResult.instructionAddress >= 0x1CD0ULL &&
             decodeResult.instructionAddress <= 0x1D70ULL) ||
            (decodeResult.instructionAddress >= 0x33C80ULL &&
             decodeResult.instructionAddress <= 0x33CC0ULL);
        if (tracePostSimpleFsPath) {
            const CPUState& cpu = state_.getCPUState();
            const uint8_t tracePredicate = cachedInstruction_.GetPredicate();
            const bool traceLivePredicate =
                (tracePredicate == 0) || cpu.GetPR(tracePredicate);
            std::cout << "[EFI-POST-SIMPLEFS] pre ip=0x" << std::hex
                      << decodeResult.instructionAddress
                      << " slot=" << std::dec << state_.currentSlot_
                      << " pred=p" << static_cast<int>(tracePredicate)
                      << " livePred=" << (traceLivePredicate ? "true" : "false")
                      << " p6=" << cpu.GetPR(6)
                      << " p7=" << cpu.GetPR(7)
                      << std::hex
                      << " r1=0x" << cpu.GetGR(1)
                      << " r8=0x" << cpu.GetGR(8)
                      << " r16=0x" << cpu.GetGR(16)
                      << " r17=0x" << cpu.GetGR(17)
                      << " r18=0x" << cpu.GetGR(18)
                      << " r20=0x" << cpu.GetGR(20)
                      << " r32=0x" << cpu.GetGR(32)
                      << " r36=0x" << cpu.GetGR(36)
                      << " r38=0x" << cpu.GetGR(38)
                      << " r61=0x" << cpu.GetGR(61)
                      << " b0=0x" << cpu.GetBR(0)
                      << " mem[r16]=" << readHexBytesPreview(memory, cpu.GetGR(16), 8)
                      << " mem[r32]=" << readHexBytesPreview(memory, cpu.GetGR(32), 16)
                      << " mem[r36]=" << readHexBytesPreview(memory, cpu.GetGR(36), 16)
                      << " mem[r38]=" << readHexBytesPreview(memory, cpu.GetGR(38), 8)
                      << " lastEfiCall=\"" << lastEfiCallName_ << "\""
                      << " lastEfiIP=0x" << lastEfiCallIP_
                      << std::dec << std::endl;
        }

        const bool traceBootLoopPath =
            (decodeResult.instructionAddress >= 0x36D60ULL &&
             decodeResult.instructionAddress <= 0x36ED0ULL);
        static bool bootLoopScanActive = false;
        static uint64_t bootLoopScanLastIP = 0;
        static size_t bootLoopScanLastSlot = 0;
        if (traceBootLoopPath) {
            const CPUState& cpu = state_.getCPUState();
            const uint8_t tracePredicate = cachedInstruction_.GetPredicate();
            const bool traceLivePredicate =
                (tracePredicate == 0) || cpu.GetPR(tracePredicate);
            std::ostringstream ctx;
            ctx << "ip=0x" << std::hex << decodeResult.instructionAddress
                << " slot=" << std::dec << state_.currentSlot_
                << " pred=p" << static_cast<int>(tracePredicate)
                << " livePred=" << (traceLivePredicate ? "true" : "false")
                << " p6=" << cpu.GetPR(6)
                << " p7=" << cpu.GetPR(7)
                << std::hex
                << " r19=0x" << cpu.GetGR(19)
                << " r20=0x" << cpu.GetGR(20)
                << " r21=0x" << cpu.GetGR(21)
                << " r22=0x" << cpu.GetGR(22)
                << " r23=0x" << cpu.GetGR(23)
                << " r24=0x" << cpu.GetGR(24)
                << " r25=0x" << cpu.GetGR(25)
                << " r33=0x" << cpu.GetGR(33)
                << " b0=0x" << cpu.GetBR(0)
                << " lastEfiCall=\"" << lastEfiCallName_ << "\""
                << " lastEfiIP=0x" << lastEfiCallIP_;
            if (!bootLoopScanActive) {
                bootLoopScanActive = true;
                std::ostringstream begin;
                begin << "startIP=" << BootStageTrace::Hex(decodeResult.instructionAddress)
                      << " slot=" << state_.currentSlot_
                      << " pred=p" << static_cast<int>(tracePredicate)
                      << " livePred=" << (traceLivePredicate ? "true" : "false")
                      << " r19=0x" << std::hex << cpu.GetGR(19)
                      << " r20=0x" << cpu.GetGR(20)
                      << " r21=0x" << cpu.GetGR(21)
                      << " r22=0x" << cpu.GetGR(22)
                      << " r23=0x" << cpu.GetGR(23)
                      << " r24=0x" << cpu.GetGR(24)
                      << " r25=0x" << cpu.GetGR(25)
                      << " r33=0x" << cpu.GetGR(33)
                      << " b0=0x" << cpu.GetBR(0)
                      << " lastEfiCall=\"" << lastEfiCallName_ << "\""
                      << " lastEfiIP=0x" << lastEfiCallIP_;
                BootStageTrace::Event("EFI_DYNAMIC_SCAN_BEGIN", begin.str());
            }
            bootLoopScanLastIP = decodeResult.instructionAddress;
            bootLoopScanLastSlot = state_.currentSlot_;
            BootStageTrace::EventOnce("EFI_DYNAMIC_SCAN_HOT_PATH", ctx.str());
        } else if (bootLoopScanActive) {
            bootLoopScanActive = false;
            std::ostringstream end;
            end << "endIP=" << BootStageTrace::Hex(bootLoopScanLastIP)
                << " endSlot=" << bootLoopScanLastSlot
                << " nextIP=" << BootStageTrace::Hex(decodeResult.instructionAddress)
                << " nextSlot=" << state_.currentSlot_;
            BootStageTrace::Event("EFI_DYNAMIC_SCAN_END", end.str());
        }

        if (cachedInstruction_.GetType() == InstructionType::UNKNOWN) {
            const std::string unknownSlot = FormatIA64UnknownSlot(
                decodeResult.instructionAddress,
                state_.currentSlot_,
                state_.currentBundle_.templateType,
                cachedInstruction_.GetUnit(),
                cachedInstruction_.GetRawBits(),
                false);
            BootStageTrace::EventOnce("FIRST_UNIMPLEMENTED_INSTRUCTION", unknownSlot);
            std::cerr << unknownSlot << std::endl;
            hasCachedInstruction_ = false;
            return ISAExecutionResult::EXCEPTION;
        }
        
        // Check for syscall
        if (cachedInstruction_.GetType() == InstructionType::BREAK &&
            cachedInstruction_.HasImmediate() &&
            cachedInstruction_.GetImmediate() == 0x100000) {
            
            if (syscallDispatcher_) {
                syscallDispatcher_->DispatchSyscall(state_.getCPUState(), memory);
            }
            
            // Advance to next instruction
            state_.currentSlot_++;
            if (state_.currentSlot_ >= state_.currentBundle_.instructions.size()) {
                state_.getCPUState().SetIP(state_.getCPUState().GetIP() + 16);
                state_.bundleValid_ = false;
            }
            
            hasCachedInstruction_ = false;
            return ISAExecutionResult::SYSCALL;
        }
        
        // Check for branch instructions
        bool isBranch = false;
        bool handledFirmwareCallStub = false;
        uint64_t branchTarget = 0;
        const uint64_t gpBeforeBranch = state_.getCPUState().GetGR(1);
        std::array<uint64_t, 8> branchRegistersBefore{};
        for (size_t i = 0; i < branchRegistersBefore.size(); ++i) {
            branchRegistersBefore[i] = state_.getCPUState().GetBR(i);
        }
        const uint8_t predicate = cachedInstruction_.GetPredicate();
        const bool snapshotPredicateTrue = (predicate == 0) || state_.predicateGroupSnapshot_[predicate];
        const bool livePredicateTrue = (predicate == 0) || state_.getCPUState().GetPR(predicate);
        const uint64_t currentIP = state_.getCPUState().GetIP();
        const uint64_t branchTargetValue = cachedInstruction_.HasBranchTarget()
            ? cachedInstruction_.GetBranchTarget()
            : 0;
        const uint64_t branchRegisterValue = isBranchRegisterTargetInstruction(cachedInstruction_.GetType())
            ? state_.getCPUState().GetBR(cachedInstruction_.GetSrc1())
            : 0;
        // Some boot code reaches a counted loop form through the conservative
        // br.call decoder. Treat a short backward br.call b5 as ar.lc-driven.
        const bool callLooksLikeCountedLoop =
            cachedInstruction_.GetType() == InstructionType::BR_CALL &&
            cachedInstruction_.GetDst() == 5 &&
            cachedInstruction_.HasBranchTarget() &&
            branchTargetValue <= currentIP &&
            (currentIP - branchTargetValue) <= 0x100;
        const bool branchInstruction =
            cachedInstruction_.GetType() == InstructionType::BR_COND ||
            cachedInstruction_.GetType() == InstructionType::BR_CALL ||
            cachedInstruction_.GetType() == InstructionType::BR_RET ||
            cachedInstruction_.GetType() == InstructionType::BR_CLOOP;
        switch (cachedInstruction_.GetType()) {
            case InstructionType::BR_COND:
                if (livePredicateTrue) {
                    branchTarget = cachedInstruction_.HasBranchTarget()
                        ? cachedInstruction_.GetBranchTarget()
                        : branchRegisterValue;
                    // The IA-64 EFI bootloader uses a small low-address thunk for
                    // Boot Services calls that decodes as br.cond b6, but it is
                    // followed by an EFI function that returns through b0. Link
                    // only this thunk-to-stub pattern so normal indirect branches
                    // keep their plain branch semantics.
                    if (!cachedInstruction_.HasBranchTarget() &&
                        cachedInstruction_.GetSrc1() == 6 &&
                        currentIP >= 0x1900 && currentIP < 0x1A00 &&
                        branchTarget >= 0x1FE00000ULL && branchTarget < 0x1FE01000ULL) {
                        state_.getCPUState().SetBR(0, currentIP + 16);
                        std::cout << "[IA64-BR] thunk br.cond b6 resolved via b6="
                                  << formatHex(branchTarget)
                                  << " callerIP=" << formatHex(currentIP)
                                  << std::endl;
                    }
                    isBranch = true;
                }
                break;
                
            case InstructionType::BR_CALL:
                if (callLooksLikeCountedLoop) {
                    if (livePredicateTrue && state_.getCPUState().GetAR(65) != 0) {
                        branchTarget = cachedInstruction_.GetBranchTarget();
                        isBranch = true;
                    }
                } else if (livePredicateTrue) {
                    branchTarget = cachedInstruction_.HasBranchTarget()
                        ? cachedInstruction_.GetBranchTarget()
                        : branchRegisterValue;
                    const uint64_t originalBranchTarget = branchTarget;
                    if (!cachedInstruction_.HasBranchTarget()) {
                        ++descriptorCallCount_;
                        const CPUState& cpu = state_.getCPUState();
                        const uint64_t oldGp = cpu.GetGR(1);
                        uint64_t descriptorCode = 0;
                        uint64_t descriptorGp = 0;
                        bool descriptorReadable = false;
                        bool resolvedViaDescriptor = false;
                        bool resolvedViaKnownStub = false;
                        uint64_t descriptorAddress = branchTarget;
                        // Resolve only from the actual branch target. Speculatively
                        // treating unrelated registers as function descriptors can
                        // mask bad control-flow values and create repeat loops.
                        if (branchTarget != 0) {
                            try {
                                if (tryResolveIa64FunctionDescriptor(memory, branchTarget,
                                                                     descriptorCode,
                                                                     descriptorGp)) {
                                    descriptorReadable = true;
                                    resolvedViaDescriptor = true;
                                    descriptorAddress = branchTarget;
                                    branchTarget = descriptorCode;
                                }
                            } catch (const std::exception&) {
                            }
                        }
                        if (!descriptorReadable) {
                            const uint64_t knownDescriptor = efiDescriptorForCodePointer(branchTarget);
                            if (knownDescriptor != 0) {
                                try {
                                    memory.Read(knownDescriptor, reinterpret_cast<uint8_t*>(&descriptorCode), sizeof(descriptorCode));
                                    memory.Read(knownDescriptor + 8, reinterpret_cast<uint8_t*>(&descriptorGp), sizeof(descriptorGp));
                                    descriptorAddress = knownDescriptor;
                                    descriptorReadable = true;
                                    resolvedViaKnownStub = true;
                                } catch (const std::exception&) {
                                    descriptorAddress = knownDescriptor;
                                    descriptorCode = 0;
                                    descriptorGp = 0;
                                }
                            }
                        }
                        if (descriptorReadable && descriptorGp != 0 && descriptorGp != oldGp) {
                            ++gpSwitchCount_;
                            state_.getCPUState().SetGR(1, descriptorGp);
                        }
                        // IA-64 callee prologues copy the caller GP back out of r38.
                        // Mirror the resolved GP there so the next frame keeps the
                        // right global pointer even when it re-materializes r1.
                        state_.getCPUState().SetGR(38, state_.getCPUState().GetGR(1));
                        if (descriptorReadable &&
                            descriptorGp >= memory.GetTotalSize() &&
                            descriptorGp < EFI_HANDOFF_REGION_BASE) {
                            ++suspiciousGpCount_;
                        }
                        const bool knownFirmwareTarget =
                            branchTarget >= EFI_HANDOFF_REGION_BASE && branchTarget < EFI_HANDOFF_REGION_END;
                        if (!knownFirmwareTarget && branchTarget >= memory.GetTotalSize()) {
                            ++unknownRegionCallCount_;
                        }
                        lastDescriptorAddress_ = descriptorAddress;
                        lastDescriptorCode_ = branchTarget;
                        lastDescriptorGp_ = descriptorReadable ? descriptorGp : 0;
                        const bool verboseIndirectCall =
                            descriptorCallCount_ <= 24 ||
                            !descriptorReadable ||
                            (descriptorCallCount_ % 128 == 0);
                        if (verboseIndirectCall) {
                            std::cout << "[IA64-DESC] indirect br.call callerIP=0x" << std::hex << currentIP
                                      << " srcBR=b" << std::dec << static_cast<int>(cachedInstruction_.GetSrc1())
                                      << " srcValue=0x" << std::hex << branchRegisterValue
                                      << " target=0x" << originalBranchTarget
                                      << " interpretedAs=";
                            if (resolvedViaDescriptor) {
                                std::cout << "descriptor";
                            } else if (resolvedViaKnownStub) {
                                std::cout << "known-stub-code";
                            } else if (branchTarget == 0) {
                                std::cout << "null";
                            } else {
                                std::cout << "invalid";
                            }
                            std::cout << " descriptor=0x" << descriptorAddress
                                      << " readable=" << (descriptorReadable ? "yes" : "no")
                                      << " descriptorCode=0x" << (descriptorReadable ? descriptorCode : 0)
                                      << " resolvedCode=0x" << branchTarget
                                      << " descriptorGp=0x" << descriptorGp
                                      << " oldR1=0x" << oldGp
                                      << " newR1=0x" << state_.getCPUState().GetGR(1)
                                      << " returnBR=b" << static_cast<int>(cachedInstruction_.GetDst())
                                      << " region=";
                            if (descriptorAddress >= EFI_HANDOFF_REGION_BASE && descriptorAddress < EFI_HANDOFF_REGION_END) {
                                std::cout << "efi-table";
                            } else if (descriptorAddress >= EFI_POOL_BASE && descriptorAddress < EFI_POOL_END) {
                                std::cout << "pool";
                            } else if (descriptorAddress >= 0x100000ULL && descriptorAddress < 0x200000ULL) {
                                std::cout << "mirrored-section";
                            } else if (descriptorAddress < memory.GetTotalSize()) {
                                std::cout << "image-or-guest";
                            } else {
                                std::cout << "unknown";
                            }
                            std::cout << std::dec << std::endl;
                        }
                    }
                    if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_ALLOCATE_POOL_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t size = readCallerOutputRegister(state_.getCPUState(), 1);
                        const uint64_t bufferOut = readCallerOutputRegister(state_.getCPUState(), 2);
                        ++efiAllocatePoolCalls_;
                        const uint64_t allocation = allocateEfiPool(memory, size);
                        if (allocation != 0 && bufferOut != 0) {
                            memory.Write(bufferOut,
                                         reinterpret_cast<const uint8_t*>(&allocation),
                                         sizeof(allocation));
                            state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                            std::cout << "[EFI-STUB] BootServices.AllocatePool size=0x"
                                      << std::hex << size << " -> 0x" << allocation
                                      << " via out=0x" << bufferOut << std::dec << std::endl;
                        } else {
                            state_.getCPUState().SetGR(8, static_cast<uint64_t>(-1));
                            std::cout << "[EFI-STUB] BootServices.AllocatePool failed size=0x"
                                      << std::hex << size << " out=0x" << bufferOut
                                      << " next=0x" << efiPoolNext_ << std::dec << std::endl;
                        }
                        logEfiServiceCall(memory, "BootServices.AllocatePool", currentIP,
                                          EFI_ALLOCATE_POOL_STUB_DESC_ADDR, originalBranchTarget,
                                          state_.getCPUState().GetGR(8));
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_GET_VARIABLE_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t variableName = readCallerOutputRegister(state_.getCPUState(), 0);
                        const uint64_t vendorGuid = readCallerOutputRegister(state_.getCPUState(), 1);
                        const uint64_t attributesOut = readCallerOutputRegister(state_.getCPUState(), 2);
                        const uint64_t dataSizeOut = readCallerOutputRegister(state_.getCPUState(), 3);
                        const uint64_t dataOut = readCallerOutputRegister(state_.getCPUState(), 4);
                        const std::string variableNameText = readUtf16AsciiString(memory, variableName);

                        std::vector<uint8_t> variableData;
                        if (variableNameText == "BootCurrent") {
                            appendLe16(variableData, 0);
                        } else if (variableNameText == "Boot0000") {
                            variableData = makeBoot0000LoadOption();
                        } else if (isLanguageVariableProbe(variableNameText, currentIP)) {
                            variableData = makeAsciiVariableData("en-US");
                        }

                        uint64_t providedSize = 0;
                        if (dataSizeOut != 0) {
                            try {
                                memory.Read(dataSizeOut,
                                            reinterpret_cast<uint8_t*>(&providedSize),
                                            sizeof(providedSize));
                            } catch (const std::exception&) {
                                providedSize = 0;
                            }
                        }

                        uint64_t status = EFI_STATUS_NOT_FOUND;
                        if (!variableData.empty() && dataSizeOut != 0) {
                            const uint64_t requiredSize = static_cast<uint64_t>(variableData.size());
                            memory.Write(dataSizeOut,
                                         reinterpret_cast<const uint8_t*>(&requiredSize),
                                         sizeof(requiredSize));
                            if (attributesOut != 0) {
                                const uint32_t attributes = 0x7U; // NV | BS | RT
                                memory.Write(attributesOut,
                                             reinterpret_cast<const uint8_t*>(&attributes),
                                             sizeof(attributes));
                            }
                            if (dataOut != 0 && providedSize >= requiredSize) {
                                memory.Write(dataOut, variableData.data(), variableData.size());
                                status = EFI_STATUS_SUCCESS;
                            } else {
                                status = EFI_STATUS_BUFFER_TOO_SMALL;
                            }
                        } else if (dataSizeOut != 0) {
                            const uint64_t zero = 0;
                            memory.Write(dataSizeOut,
                                         reinterpret_cast<const uint8_t*>(&zero),
                                         sizeof(zero));
                        }

                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        std::cout << "[EFI-STUB] RuntimeServices.GetVariable name=0x"
                                  << std::hex << variableName
                                  << " \"" << variableNameText << "\""
                                  << " guid=0x" << vendorGuid
                                  << " attrOut=0x" << attributesOut
                                  << " sizeOut=0x" << dataSizeOut
                                  << " dataOut=0x" << dataOut;
                        if (!variableData.empty()) {
                            std::cout << " required=0x" << variableData.size();
                        }
                        if (status == EFI_STATUS_SUCCESS) {
                            std::cout << " -> EFI_SUCCESS";
                        } else if (status == EFI_STATUS_BUFFER_TOO_SMALL) {
                            std::cout << " -> EFI_BUFFER_TOO_SMALL";
                        } else {
                            std::cout << " -> EFI_NOT_FOUND";
                        }
                        std::cout << std::dec << std::endl;
                        logEfiServiceCall(memory, "RuntimeServices.GetVariable", currentIP,
                                          EFI_GET_VARIABLE_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_HANDLE_PROTOCOL_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        ++efiHandleProtocolCalls_;
                        const uint64_t handle = readCallerOutputRegister(state_.getCPUState(), 0);
                        const uint64_t protocolGuid = readCallerOutputRegister(state_.getCPUState(), 1);
                        const uint64_t interfaceOut = readCallerOutputRegister(state_.getCPUState(), 2);
                        EfiGuid guid = {};
                        bool hasGuid = readEfiGuid(memory, protocolGuid, guid);
                        bool repairedZeroGuid = false;
                        const char* repairedGuidName = nullptr;
                        if (hasGuid && isZeroEfiGuid(guid) && protocolGuid != 0) {
                            const EfiGuid* expectedGuid = nullptr;
                            if (currentIP == 0x2EFD0ULL || currentIP == 0x1A50ULL) {
                                expectedGuid = &EFI_LOADED_IMAGE_PROTOCOL_GUID;
                                repairedGuidName = "LoadedImageProtocol";
                            } else if (currentIP == 0x1CC0ULL) {
                                expectedGuid = &EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
                                repairedGuidName = "SimpleFileSystemProtocol";
                            }
                            if (expectedGuid != nullptr) {
                                try {
                                    memory.Write(protocolGuid, expectedGuid->data(), expectedGuid->size());
                                    guid = *expectedGuid;
                                    repairedZeroGuid = true;
                                } catch (const std::exception&) {
                                    hasGuid = readEfiGuid(memory, protocolGuid, guid);
                                }
                            }
                        }
                        const bool zeroGuid = hasGuid && isZeroEfiGuid(guid);
                        if (zeroGuid) {
                            ++efiZeroGuidProtocolCalls_;
                        }
                        uint64_t protocolAddress = 0;
                        const char* protocolName = nullptr;
                        if (hasGuid && guid == EFI_LOADED_IMAGE_PROTOCOL_GUID) {
                            protocolAddress = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
                            protocolName = "LoadedImageProtocol";
                        } else if (hasGuid && guid == EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID) {
                            protocolAddress = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR;
                            protocolName = "SimpleFileSystemProtocol";
                        } else if (zeroGuid) {
                            if (currentIP == 0x2EFD0ULL || currentIP == 0x1A50ULL) {
                                protocolAddress = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
                                protocolName = "LoadedImageProtocol(zero-guid fallback)";
                            } else if (currentIP == 0x1CC0ULL) {
                                protocolAddress = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR;
                                protocolName = "SimpleFileSystemProtocol(zero-guid fallback)";
                            }
                        }
                        if (protocolAddress == EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR) {
                            ++efiSimpleFsProtocolReturns_;
                        }

                        if (interfaceOut != 0 && protocolAddress != 0) {
                            memory.Write(interfaceOut,
                                         reinterpret_cast<const uint8_t*>(&protocolAddress),
                                         sizeof(protocolAddress));
                            if (protocolAddress == EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR) {
                                uint64_t adjacentLoadedImage = 0;
                                const uint64_t adjacentLoadedImageSlot = interfaceOut + sizeof(uint64_t);
                                const bool haveAdjacentLoadedImage =
                                    readGuestU64(memory, adjacentLoadedImageSlot, adjacentLoadedImage);
                                if (!haveAdjacentLoadedImage ||
                                    adjacentLoadedImage == 0 ||
                                    adjacentLoadedImage == EFI_LOADED_IMAGE_PROTOCOL_ADDR) {
                                    adjacentLoadedImage = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
                                    memory.Write(adjacentLoadedImageSlot,
                                                 reinterpret_cast<const uint8_t*>(&adjacentLoadedImage),
                                                 sizeof(adjacentLoadedImage));
                                    std::cout << "[EFI-STUB] SimpleFS.HandleProtocol restored adjacent LoadedImage context"
                                              << " slot=0x" << std::hex << adjacentLoadedImageSlot
                                              << " value=0x" << adjacentLoadedImage
                                              << " interfaceOut=0x" << interfaceOut
                                              << std::dec << std::endl;
                                    BootStageTrace::Event("EFI_SIMPLEFS_CONTEXT_RESTORED",
                                        "slot=" + BootStageTrace::Hex(adjacentLoadedImageSlot) +
                                        " value=" + BootStageTrace::Hex(adjacentLoadedImage) +
                                        " interfaceOut=" + BootStageTrace::Hex(interfaceOut));
                                }
                            }
                        } else if (interfaceOut != 0) {
                            const uint64_t nullInterface = 0;
                            memory.Write(interfaceOut,
                                         reinterpret_cast<const uint8_t*>(&nullInterface),
                                         sizeof(nullInterface));
                        }
                        uint64_t interfaceOutValue = 0;
                        const bool hasInterfaceOutValue =
                            interfaceOut != 0 && readGuestU64(memory, interfaceOut, interfaceOutValue);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, protocolAddress != 0
                            ? EFI_STATUS_SUCCESS
                            : EFI_STATUS_NOT_FOUND);
                        std::cout << "[EFI-STUB] BootServices.HandleProtocol guid=0x"
                                  << std::hex << protocolGuid;
                        if (hasGuid) {
                            std::cout << " [" << formatEfiGuid(guid) << "]";
                        } else {
                            std::cout << " [unreadable]";
                        }
                        if (protocolAddress != 0) {
                            std::cout << " returned " << protocolName << "=0x"
                                      << protocolAddress << " via out=0x" << interfaceOut;
                        } else {
                            std::cout << " -> EFI_NOT_FOUND via out=0x" << interfaceOut;
                        }
                        if (interfaceOut != 0) {
                            std::cout << " outValue=";
                            if (hasInterfaceOutValue) {
                                std::cout << "0x" << interfaceOutValue;
                            } else {
                                std::cout << "unreadable";
                            }
                            std::cout << " outBytes="
                                      << readHexBytesPreview(memory, interfaceOut, 16);
                        }
                        std::cout << " handle=0x" << handle
                                  << " callsite=0x" << currentIP;
                        if (zeroGuid) {
                            const uint64_t currentGp = state_.getCPUState().GetGR(1);
                            const int64_t guidMinusGp =
                                static_cast<int64_t>(protocolGuid) - static_cast<int64_t>(currentGp);
                            std::cout << " ZERO_PROTOCOL_GUID"
                                      << " gp=0x" << currentGp
                                      << " guidMinusGp=" << std::dec << guidMinusGp << std::hex
                                      << " guidBytes="
                                      << readHexBytesPreview(memory, protocolGuid, guid.size())
                                      << " nextBytes="
                                      << readHexBytesPreview(memory, protocolGuid + guid.size(), guid.size());
                        } else if (repairedZeroGuid) {
                            std::cout << " repairedZeroGuidAs=" << repairedGuidName;
                        }
                        if (protocolAddress == EFI_LOADED_IMAGE_PROTOCOL_ADDR) {
                            std::cout << " " << describeLoadedImageProtocol(memory, protocolAddress);
                        } else if (protocolAddress == EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR) {
                            std::cout << " " << describeSimpleFileSystemProtocol(memory, protocolAddress)
                                      << " latestLoadedImage="
                                      << describeLoadedImageProtocol(memory, EFI_LOADED_IMAGE_PROTOCOL_ADDR)
                                      << " expectedSimpleFsTable=0x" << EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR
                                      << " simpleFsTableBytes32="
                                      << readHexBytesPreview(memory, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR, 32)
                                      << " expectedOpenVolumeDescriptor=0x" << EFI_OPEN_VOLUME_STUB_DESC_ADDR
                                      << " openVolumeDescriptorBytes32="
                                      << readHexBytesPreview(memory, EFI_OPEN_VOLUME_STUB_DESC_ADDR, 32)
                                      << " lastIndirectDescriptor=0x" << lastDescriptorAddress_
                                      << " lastIndirectDescriptorBytes32="
                                      << readHexBytesPreview(memory, lastDescriptorAddress_, 32);
                        }
                        std::cout << std::dec << std::endl;
                        logEfiServiceCall(memory, "BootServices.HandleProtocol", currentIP,
                                          EFI_HANDLE_PROTOCOL_STUB_DESC_ADDR, originalBranchTarget,
                                          state_.getCPUState().GetGR(8));
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_OPEN_VOLUME_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        ++efiOpenVolumeCalls_;
                        if (efiOpenVolumeCalls_ == 1) {
                            std::cout << "[EFI-MILESTONE] first OpenVolume call callerIP=0x"
                                      << std::hex << currentIP << std::dec << std::endl;
                        }
                        const uint64_t rootOut = readCallerOutputRegister(state_.getCPUState(), 1);
                        if (rootOut != 0) {
                            memory.Write(rootOut,
                                         reinterpret_cast<const uint8_t*>(&EFI_ROOT_FILE_PROTOCOL_ADDR),
                                         sizeof(EFI_ROOT_FILE_PROTOCOL_ADDR));
                        }
                        installFileProtocolTable(memory, EFI_ROOT_FILE_PROTOCOL_ADDR);
                        EfiFileHandle rootHandle;
                        rootHandle.protocolAddress = EFI_ROOT_FILE_PROTOCOL_ADDR;
                        rootHandle.path = "";
                        rootHandle.isDirectory = true;
                        if (ensureEfiBootFat(memory)) {
                            efiBootFat_->listDirectory("/", rootHandle.directoryEntries);
                        }
                        efiFileHandles_[EFI_ROOT_FILE_PROTOCOL_ADDR] = rootHandle;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, rootOut != 0
                            ? EFI_STATUS_SUCCESS
                            : EFI_STATUS_UNSUPPORTED);
                        std::cout << "[EFI-STUB] SimpleFileSystem.OpenVolume rootOut=0x"
                                  << std::hex << rootOut;
                        if (rootOut != 0) {
                            std::cout << " -> FileProtocol=0x" << EFI_ROOT_FILE_PROTOCOL_ADDR;
                        } else {
                            std::cout << " -> EFI_UNSUPPORTED";
                        }
                        std::cout << std::dec << std::endl;
                        logEfiServiceCall(memory, "SimpleFS.OpenVolume", currentIP,
                                          EFI_OPEN_VOLUME_STUB_DESC_ADDR, originalBranchTarget,
                                          state_.getCPUState().GetGR(8));
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_FILE_OPEN_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiFileOpen(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "FileProtocol.Open", currentIP,
                                          EFI_FILE_OPEN_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_FILE_READ_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiFileRead(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "FileProtocol.Read", currentIP,
                                          EFI_FILE_READ_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_FILE_GET_INFO_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiFileGetInfo(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "FileProtocol.GetInfo", currentIP,
                                          EFI_FILE_GET_INFO_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_FILE_CLOSE_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiFileClose(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "FileProtocol.Close", currentIP,
                                          EFI_FILE_CLOSE_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_FILE_GET_POSITION_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiFileGetPosition(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "FileProtocol.GetPosition", currentIP,
                                          EFI_FILE_GET_POSITION_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_FILE_SET_POSITION_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiFileSetPosition(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "FileProtocol.SetPosition", currentIP,
                                          EFI_FILE_SET_POSITION_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_LOCATE_HANDLE_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiLocateHandle(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "BootServices.LocateHandle", currentIP,
                                          EFI_LOCATE_HANDLE_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_LOCATE_PROTOCOL_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiLocateProtocol(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "BootServices.LocateProtocol", currentIP,
                                          EFI_LOCATE_PROTOCOL_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_GET_MEMORY_MAP_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t status = handleEfiGetMemoryMap(memory);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, status);
                        logEfiServiceCall(memory, "BootServices.GetMemoryMap", currentIP,
                                          EFI_GET_MEMORY_MAP_STUB_DESC_ADDR, originalBranchTarget, status);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_EXIT_BOOT_SERVICES_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        ++efiExitBootServicesCalls_;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-MILESTONE] ExitBootServices called image=0x"
                                  << std::hex << readCallerOutputRegister(state_.getCPUState(), 0)
                                  << " key=0x" << readCallerOutputRegister(state_.getCPUState(), 1)
                                  << std::dec << std::endl;
                        logEfiServiceCall(memory, "BootServices.ExitBootServices", currentIP,
                                          EFI_EXIT_BOOT_SERVICES_STUB_DESC_ADDR, originalBranchTarget,
                                          EFI_STATUS_SUCCESS);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        (branchTarget == EFI_LOAD_IMAGE_STUB_CODE_ADDR ||
                         branchTarget == EFI_START_IMAGE_STUB_CODE_ADDR)) {
                        handledFirmwareCallStub = true;
                        const bool loadImage = branchTarget == EFI_LOAD_IMAGE_STUB_CODE_ADDR;
                        if (loadImage) {
                            ++efiLoadImageCalls_;
                        } else {
                            ++efiStartImageCalls_;
                        }
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_UNSUPPORTED);
                        logEfiServiceCall(memory, loadImage ? "BootServices.LoadImage" : "BootServices.StartImage", currentIP,
                                          loadImage ? EFI_LOAD_IMAGE_STUB_DESC_ADDR : EFI_START_IMAGE_STUB_DESC_ADDR,
                                          originalBranchTarget, EFI_STATUS_UNSUPPORTED);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_TEXT_OUTPUT_STRING_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
                        const uint64_t stringAddress = readCallerOutputRegister(state_.getCPUState(), 1);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        ++efiTextOutputCalls_;
                        std::cout << "[EFI-STUB] SimpleTextOut.OutputString this=0x"
                                  << std::hex << thisProtocol
                                  << " string=0x" << stringAddress;
                        const std::string preview = readUtf16Preview(memory, stringAddress);
                        if (!preview.empty()) {
                            std::cout << " \"" << preview << "\"";
                        }
                        std::cout << " utf16="
                                  << readUtf16CodeUnitPreview(memory, stringAddress, 16);
                        uint64_t consoleAddress = 0;
                        if (mirrorTextToVmConsole(memory, preview, consoleAddress)) {
                            ++efiTextOutputMirrored_;
                            std::cout << " mirroredToConsole=0x" << consoleAddress;
                        } else {
                            std::cout << " consoleMirror=unavailable";
                        }
                        uint64_t framebufferAddress = 0;
                        if (mirrorTextToFramebuffer(memory, preview, efiTextOutputCalls_, framebufferAddress)) {
                            ++efiTextOutputFramebuffer_;
                            std::cout << " mirroredToFramebuffer=0x" << framebufferAddress;
                        } else {
                            std::cout << " framebufferMirror=unavailable";
                        }
                        {
                            std::ostringstream ctx;
                            ctx << "path=SimpleTextOut.OutputString"
                                << " this=" << BootStageTrace::Hex(thisProtocol)
                                << " stringAddress=" << BootStageTrace::Hex(stringAddress)
                                << " text=\"" << preview << "\""
                                << " consoleMirror=" << BootStageTrace::Hex(consoleAddress)
                                << " framebufferMirror=" << BootStageTrace::Hex(framebufferAddress)
                                << " calls=" << efiTextOutputCalls_;
                            BootStageTrace::Stage(140, "First console/GOP/serial output observed", ctx.str());
                            BootStageTrace::Event("VISIBLE_OUTPUT", ctx.str());
                        }
                        std::cout << " -> EFI_SUCCESS" << std::dec << std::endl;
                        logEfiServiceCall(memory, "SimpleTextOut.OutputString", currentIP,
                                          EFI_TEXT_OUTPUT_STRING_STUB_DESC_ADDR, originalBranchTarget,
                                          EFI_STATUS_SUCCESS);
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_SUCCESS_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        ++efiGenericSuccessCalls_;
                        const CPUState& cpu = state_.getCPUState();
                        // `r8` is the EFI return status here, not a descriptor pointer.
                        // Use the actual stub code target to describe/log the call.
                        const uint64_t descriptorCandidate =
                            efiDescriptorForCodePointer(originalBranchTarget);
                        const uint64_t slotCandidate = cpu.GetGR(9);
                        const uint64_t receiverCandidate = readCallerOutputRegister(cpu, 0);
                        const uint64_t r14Candidate = cpu.GetGR(14);
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-STUB] generic success service callsite=0x"
                                  << std::hex << currentIP
                                  << " target=0x" << originalBranchTarget
                                  << " descriptorCandidate=0x" << descriptorCandidate;
                        std::string description = describeEfiDescriptor(descriptorCandidate);
                        if (!description.empty()) {
                            std::cout << " [" << description << "]";
                        }
                        std::cout << " slotCandidate[r9]=0x" << slotCandidate;
                        description = describeEfiTableSlot(slotCandidate);
                        if (!description.empty()) {
                            std::cout << " [" << description << "]";
                        }
                        std::cout << " receiver[r32]=0x" << receiverCandidate;
                        description = describeEfiTableSlot(receiverCandidate);
                        if (!description.empty()) {
                            std::cout << " [" << description << "]";
                        }
                        std::cout << " r14=0x" << r14Candidate;
                        description = describeEfiTableSlot(r14Candidate);
                        if (!description.empty()) {
                            std::cout << " [" << description << "]";
                        }
                        std::cout << " -> EFI_SUCCESS" << std::dec << std::endl;
                        if (slotCandidate == EFI_BOOT_SERVICES_ADDR + 0x48) {
                            ++efiFreePoolCalls_;
                        }
                        logEfiServiceCall(memory, "EFI.GenericSuccess", currentIP,
                                          descriptorCandidate, originalBranchTarget,
                                          EFI_STATUS_SUCCESS);
                    } else if (!cachedInstruction_.HasBranchTarget() && branchTarget == 0) {
                        handledFirmwareCallStub = true;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-STUB] indirect br.call target is null; returning EFI_SUCCESS"
                                  << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        isZeroFilledEfiHandoffBundle(memory, branchTarget)) {
                        handledFirmwareCallStub = true;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_SUCCESS);
                        std::cout << "[EFI-STUB] indirect br.call target 0x"
                                  << std::hex << originalBranchTarget
                                  << " is zero-filled handoff memory; returning EFI_SUCCESS"
                                  << std::dec << std::endl;
                    } else if (!cachedInstruction_.HasBranchTarget() &&
                        branchTarget == EFI_UNSUPPORTED_STUB_CODE_ADDR) {
                        handledFirmwareCallStub = true;
                        ++efiGenericUnsupportedCalls_;
                        branchTarget = currentIP + 16;
                        state_.getCPUState().SetBR(cachedInstruction_.GetDst(), branchTarget);
                        state_.getCPUState().SetGR(8, EFI_STATUS_UNSUPPORTED);
                        std::cout << "[EFI-STUB] unsupported firmware service target 0x"
                                  << std::hex << originalBranchTarget
                                  << " -> EFI_UNSUPPORTED" << std::dec << std::endl;
                        logEfiServiceCall(memory, "EFI.Unsupported", currentIP,
                                          EFI_UNSUPPORTED_STUB_DESC_ADDR, originalBranchTarget,
                                          EFI_STATUS_UNSUPPORTED);
                    } else {
                        saveCallFrame(currentIP + 16);
                        captureCallOutputRegisters();
                    }
                    isBranch = true;
                }
                break;
                
            case InstructionType::BR_RET:
                if (livePredicateTrue) {
                    branchTarget = branchRegisterValue;
                    isBranch = true;
                }
                break;

            case InstructionType::BR_CLOOP:
                if (livePredicateTrue && cachedInstruction_.HasBranchTarget() &&
                    state_.getCPUState().GetAR(65) != 0) {
                    branchTarget = cachedInstruction_.GetBranchTarget();
                    isBranch = true;
                }
                break;
                
            default:
                break;
        }

        // Direct calls/branches can also land on IA-64 function descriptors.
        // Resolve those before stub dispatch so imported EFI entry points jump
        // to code rather than executing the descriptor payload as a bundle.
        if (isBranch && branchTarget != 0) {
            uint64_t resolvedCode = 0;
            uint64_t resolvedGp = 0;
            if (tryResolveIa64FunctionDescriptor(memory, branchTarget, resolvedCode, resolvedGp) &&
                resolvedCode != branchTarget) {
                const uint64_t oldGp = state_.getCPUState().GetGR(1);
                if (resolvedGp != 0 && resolvedGp != oldGp) {
                    ++gpSwitchCount_;
                    state_.getCPUState().SetGR(1, resolvedGp);
                }
                // Keep the caller GP mirrored in r38 for the next callee frame,
                // matching the indirect-call and fetch-bundle descriptor paths.
                state_.getCPUState().SetGR(38, state_.getCPUState().GetGR(1));
                lastDescriptorAddress_ = branchTarget;
                lastDescriptorCode_ = resolvedCode;
                lastDescriptorGp_ = resolvedGp;
                std::cout << "[IA64-DESC] direct branch target descriptor=0x" << std::hex
                          << branchTarget
                          << " resolvedCode=0x" << resolvedCode
                          << " descriptorGp=0x" << resolvedGp
                          << " oldR1=0x" << oldGp
                          << " newR1=0x" << state_.getCPUState().GetGR(1)
                          << std::dec << std::endl;
                branchTarget = resolvedCode;
            }
        }
        
        // Keep non-branch predicate execution on the instruction-group snapshot,
        // while branches use the current predicate state for boot-critical flow.
        if ((branchInstruction && livePredicateTrue) ||
            (!branchInstruction && snapshotPredicateTrue)) {
            if (callLooksLikeCountedLoop) {
                if (state_.getCPUState().GetAR(65) != 0) {
                    state_.getCPUState().SetAR(65, state_.getCPUState().GetAR(65) - 1);
                }
            } else {
                if (!handledFirmwareCallStub) {
                    executeInstruction(memory, cachedInstruction_, true);
                }
            }
            if (cachedInstruction_.GetType() == InstructionType::ALLOC) {
                applyPendingCallInputRegisters();
            }
        }

        // Handle branch after execution
        if (isBranch) {
            {
                const char* branchKind = "branch";
                if (cachedInstruction_.GetType() == InstructionType::BR_CALL) {
                    branchKind = "call";
                } else if (cachedInstruction_.GetType() == InstructionType::BR_RET) {
                    branchKind = "return";
                } else if (cachedInstruction_.GetType() == InstructionType::BR_COND) {
                    branchKind = "conditional";
                } else if (cachedInstruction_.GetType() == InstructionType::BR_CLOOP) {
                    branchKind = "counted-loop";
                }
                std::ostringstream ctx;
                ctx << "kind=" << branchKind
                    << " callerIP=" << BootStageTrace::Hex(currentIP)
                    << " slot=" << state_.currentSlot_
                    << " srcBR=b" << static_cast<int>(cachedInstruction_.GetSrc1())
                    << " srcValue=" << BootStageTrace::Hex(branchRegisterValue)
                    << " dstBR=b" << static_cast<int>(cachedInstruction_.GetDst())
                    << " target=" << BootStageTrace::Hex(branchTarget)
                    << " normalized=" << BootStageTrace::Hex(normalizeBranchEntryIP(branchTarget))
                    << " predicate=p" << static_cast<int>(predicate)
                    << " livePredicate=" << (livePredicateTrue ? "true" : "false")
                    << " oldR1=" << BootStageTrace::Hex(gpBeforeBranch)
                    << " newR1=" << BootStageTrace::Hex(state_.getCPUState().GetGR(1))
                    << " oldBRs=[";
                for (size_t i = 0; i < branchRegistersBefore.size(); ++i) {
                    if (i != 0) {
                        ctx << ',';
                    }
                    ctx << 'b' << i << '=' << BootStageTrace::Hex(branchRegistersBefore[i]);
                }
                ctx << "] newBRs=[";
                for (size_t i = 0; i < branchRegistersBefore.size(); ++i) {
                    if (i != 0) {
                        ctx << ',';
                    }
                    ctx << 'b' << i << '=' << BootStageTrace::Hex(state_.getCPUState().GetBR(i));
                }
                ctx << "]"
                    << " disasm=\"" << decodeResult.disassembly << "\" "
                    << cpuSummary(state_.getCPUState());
                BootStageTrace::Stage(120, "First branch/call observed", ctx.str());
                if (cachedInstruction_.GetType() == InstructionType::BR_CALL) {
                    BootStageTrace::EventOnce("FIRST_CALL_OBSERVED", ctx.str());
                }
            }
            if (cachedInstruction_.GetType() == InstructionType::BR_RET &&
                livePredicateTrue && branchTarget == 0) {
                std::cout << "[EFI-STUB] top-level br.ret b0 reached zero return address; halting EFI app"
                          << std::endl;
                std::cout << "[EFI-MILESTONE] EFI app returned before kernel handoff; "
                          << "ConsoleOut.OutputString calls=" << efiTextOutputCalls_
                          << ", mirroredToVmConsole=" << efiTextOutputMirrored_
                          << ", mirroredToFramebuffer=" << efiTextOutputFramebuffer_
                          << ", SimpleFS.OpenVolume calls=" << efiOpenVolumeCalls_
                          << ", SimpleFS.HandleProtocol returns=" << efiSimpleFsProtocolReturns_
                          << ", genericSuccessServices=" << efiGenericSuccessCalls_
                          << ", genericUnsupportedServices=" << efiGenericUnsupportedCalls_
                          << ", zeroGuidHandleProtocolCalls=" << efiZeroGuidProtocolCalls_
                          << ", HandleProtocol=" << efiHandleProtocolCalls_
                          << ", LocateHandle=" << efiLocateHandleCalls_
                          << ", LocateProtocol=" << efiLocateProtocolCalls_
                          << ", GetMemoryMap=" << efiGetMemoryMapCalls_
                          << ", ExitBootServices=" << efiExitBootServicesCalls_
                          << ", AllocatePool=" << efiAllocatePoolCalls_
                          << ", FreePool=" << efiFreePoolCalls_
                          << ", LoadImage=" << efiLoadImageCalls_
                          << ", StartImage=" << efiStartImageCalls_
                          << ", File.Open=" << efiFileOpenCalls_
                          << ", File.Read=" << efiFileReadCalls_
                          << ", File.GetInfo=" << efiFileGetInfoCalls_
                          << ", File.Close=" << efiFileCloseCalls_
                          << ", File.SetPosition=" << efiFileSetPositionCalls_
                          << ", File.GetPosition=" << efiFileGetPositionCalls_
                          << ", totalFileBytesRead=0x" << std::hex << efiTotalFileBytesRead_ << std::dec
                          << ", descriptorCalls=" << descriptorCallCount_
                          << ", gpSwitches=" << gpSwitchCount_
                          << ", suspiciousGP=" << suspiciousGpCount_
                          << ", unknownRegionCalls=" << unknownRegionCallCount_
                          << ", recoveredLoadStores=" << recoveredLoadStoreCount_
                          << ". Missing next milestone: kernel handoff; if OpenVolume remains 0, "
                          << "the loader is returning before firmware file/block I/O begins; "
                          << "if SimpleFS.HandleProtocol is nonzero while OpenVolume is zero, "
                          << "the fork is after protocol lookup and before volume open, usually "
                          << "LoadedImage FilePath/LoadOptions or device-path parsing; "
                          << "if zeroGuidHandleProtocolCalls is nonzero, protocol GUID data/addressing "
                          << "is suspect before device-path/file I/O."
                          << std::endl;
                state_.bundleValid_ = false;
                hasCachedInstruction_ = false;
                return ISAExecutionResult::HALT;
            }
            if (cachedInstruction_.GetType() == InstructionType::BR_RET && livePredicateTrue) {
                restoreCallFrame(branchTarget);
            }
            const uint64_t branchEntryIP = normalizeBranchEntryIP(branchTarget);
            rememberBranchTarget(branchEntryIP);
            if (efiExitBootServicesCalls_ > 0 &&
                branchEntryIP < memory.GetTotalSize() &&
                (branchEntryIP < EFI_HANDOFF_REGION_BASE || branchEntryIP >= EFI_HANDOFF_REGION_END) &&
                branchEntryIP >= 0x100000ULL) {
                {
                    std::ostringstream ctx;
                    ctx << "callerIP=" << BootStageTrace::Hex(currentIP)
                        << " target=" << BootStageTrace::Hex(branchEntryIP)
                        << " rawTarget=" << BootStageTrace::Hex(branchTarget)
                        << " lastEfiCall=\"" << lastEfiCallName_ << "\" "
                        << cpuSummary(state_.getCPUState());
                    BootStageTrace::Stage(180, "Kernel handoff attempted", ctx.str());
                }
                std::cout << "[EFI-MILESTONE] branch/call to non-firmware kernel entry candidate ip=0x"
                          << std::hex << currentIP << " target=0x" << branchEntryIP
                          << " lastEfiCall=" << lastEfiCallName_ << std::dec << std::endl;
            }
            if (callLooksLikeCountedLoop) {
                g_countedLoopTrace.active = true;
                g_countedLoopTrace.start = branchEntryIP;
                g_countedLoopTrace.end = currentIP;
            }
            const size_t memorySize = memory.GetTotalSize();
            const bool branchIntoSyntheticEfi =
                branchEntryIP >= EFI_HANDOFF_REGION_BASE && branchEntryIP < EFI_HANDOFF_REGION_END;
            const CPUState& cpu = state_.getCPUState();
            if (memorySize != 0 &&
                !branchIntoSyntheticEfi &&
                (branchEntryIP >= memorySize || branchEntryIP + 16 > memorySize)) {
                const char* branchKind = "branch";
                if (cachedInstruction_.GetType() == InstructionType::BR_CALL) {
                    branchKind = "br.call";
                } else if (cachedInstruction_.GetType() == InstructionType::BR_RET) {
                    branchKind = "br.ret";
                } else if (cachedInstruction_.GetType() == InstructionType::BR_COND) {
                    branchKind = "br.cond";
                }
                std::cerr << "[EFI-MILESTONE] IA-64 control-flow target outside guest memory before kernel handoff"
                          << " ip=0x" << std::hex << currentIP
                          << " slot=" << std::dec << state_.currentSlot_
                          << " kind=" << branchKind
                           << " branchTarget=0x" << std::hex << branchTarget
                           << " normalized=0x" << branchEntryIP
                           << " guestSize=0x" << memorySize
                           << " gp(r1)=0x" << cpu.GetGR(1)
                           << " r2=0x" << cpu.GetGR(2)
                           << " r8=0x" << cpu.GetGR(8)
                           << " r14=0x" << cpu.GetGR(14)
                           << " r33=0x" << cpu.GetGR(33)
                           << " br0=0x" << cpu.GetBR(0)
                           << " br6=0x" << cpu.GetBR(6)
                          << ". Missing next milestone: kernel handoff; suspect corrupt IA-64"
                          << " function-descriptor/control-flow data from PE/COFF loading, relocations,"
                          << " or GP-relative addressing rather than ConsoleOut/GOP."
                          << std::dec << "\n";
                state_.bundleValid_ = false;
                hasCachedInstruction_ = false;
                return ISAExecutionResult::EXCEPTION;
            }
            if (!branchIntoSyntheticEfi && memorySize != 0 && branchEntryIP >= memorySize) {
                std::cerr << "[FETCH-FAULT] IA-64 branch target left mapped guest memory"
                          << " ip=0x" << std::hex << currentIP
                          << " target=0x" << branchTarget
                          << " normalized=0x" << branchEntryIP
                          << " guestSize=0x" << memorySize
                          << " br0=0x" << cpu.GetBR(0)
                          << " br6=0x" << cpu.GetBR(6)
                          << std::dec << std::endl;
                state_.bundleValid_ = false;
                hasCachedInstruction_ = false;
                return ISAExecutionResult::EXCEPTION;
            }
            state_.getCPUState().SetIP(branchEntryIP);
            state_.bundleValid_ = false;
            state_.currentSlot_ = 0;
            capturePredicateGroupSnapshot();
            hasCachedInstruction_ = false;
            return ISAExecutionResult::CONTINUE;
        }

        if (callLooksLikeCountedLoop) {
            finishCountedLoopTraceIfActive();
        }
        
        // Advance to next instruction (non-branch)
        const size_t executedSlot = state_.currentSlot_;
        state_.currentSlot_++;
        if (executedSlot < state_.currentBundle_.stopAfterSlot.size() &&
            state_.currentBundle_.stopAfterSlot[executedSlot]) {
            capturePredicateGroupSnapshot();
        }
        if (state_.currentSlot_ >= state_.currentBundle_.instructions.size()) {
            state_.getCPUState().SetIP(state_.getCPUState().GetIP() + 16);
            state_.bundleValid_ = false;
            capturePredicateGroupSnapshot();
        }
        
        hasCachedInstruction_ = false;
        return ISAExecutionResult::CONTINUE;
        
    } catch (const std::exception& e) {
        std::cerr << "Execution error: " << e.what() << "\n";
        BootStageTrace::EventOnce(
            "FIRST_FAULT_TRAP",
            "kind=execute ip=" + BootStageTrace::Hex(state_.getCPUState().GetIP()) +
            " reason=\"" + std::string(e.what()) + "\" " + cpuSummary(state_.getCPUState()));
        hasCachedInstruction_ = false;
        return ISAExecutionResult::EXCEPTION;
    }
}

ISAExecutionResult IA64ISAPlugin::step(IMemory& memory) {
    // Service interrupts first
    servicePendingInterrupt(memory);
    
    // Decode and execute
    auto decodeResult = decode(memory);
    if (!decodeResult.valid) {
        return ISAExecutionResult::EXCEPTION;
    }
    
    return execute(memory, decodeResult);
}

void IA64ISAPlugin::setState(const ISAState& state) {
    const IA64ISAState* ia64State = dynamic_cast<const IA64ISAState*>(&state);
    if (!ia64State) {
        throw std::invalid_argument("State is not IA64ISAState");
    }
    
    state_ = *ia64State;
    hasCachedInstruction_ = false;
    pendingCallInputs_.clear();
    efiTextOutputCalls_ = 0;
    efiTextOutputMirrored_ = 0;
    efiTextOutputFramebuffer_ = 0;
    efiOpenVolumeCalls_ = 0;
    efiSimpleFsProtocolReturns_ = 0;
    efiGenericSuccessCalls_ = 0;
    efiGenericUnsupportedCalls_ = 0;
    efiZeroGuidProtocolCalls_ = 0;
    efiHandleProtocolCalls_ = 0;
    efiLocateHandleCalls_ = 0;
    efiLocateProtocolCalls_ = 0;
    efiGetMemoryMapCalls_ = 0;
    efiExitBootServicesCalls_ = 0;
    efiAllocatePoolCalls_ = 0;
    efiFreePoolCalls_ = 0;
    efiLoadImageCalls_ = 0;
    efiStartImageCalls_ = 0;
    efiFileOpenCalls_ = 0;
    efiFileReadCalls_ = 0;
    efiFileGetInfoCalls_ = 0;
    efiFileCloseCalls_ = 0;
    efiFileSetPositionCalls_ = 0;
    efiFileGetPositionCalls_ = 0;
    efiFirstSuccessfulFileOpen_ = 0;
    efiTotalFileBytesRead_ = 0;
    descriptorCallCount_ = 0;
    gpSwitchCount_ = 0;
    suspiciousGpCount_ = 0;
    unknownRegionCallCount_ = 0;
    recoveredLoadStoreCount_ = 0;
    lastEfiCallName_.clear();
    lastEfiCallIP_ = 0;
    lastDescriptorAddress_ = 0;
    lastDescriptorCode_ = 0;
    lastDescriptorGp_ = 0;
    lastBranchTargets_.clear();
    recentInstructionSequenceRepeatCount_ = 0;
    efiFileHandles_.clear();
    efiBootImage_.clear();
    efiBootFat_.reset();
    callFrameStack_.clear();
}

std::vector<uint8_t> IA64ISAPlugin::serialize_state() const {
    size_t stateSize = state_.getStateSize();
    std::vector<uint8_t> data(stateSize);
    state_.serialize(data.data());
    return data;
}

bool IA64ISAPlugin::deserialize_state(const std::vector<uint8_t>& data) {
    try {
        if (data.size() < state_.getStateSize()) {
            return false;
        }
        
        state_.deserialize(data.data());
        hasCachedInstruction_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

std::string IA64ISAPlugin::dumpState() const {
    return state_.toString();
}

std::string IA64ISAPlugin::disassemble(IMemory& memory, uint64_t address) {
    try {
        uint8_t bundleData[16];
        memory.Read(address, bundleData, 16);
        
        Bundle bundle = decoder_.DecodeBundleAt(bundleData, address);
        
        std::stringstream ss;
        ss << "Bundle at 0x" << std::hex << address << std::dec << ":\n";
        for (size_t i = 0; i < bundle.instructions.size(); ++i) {
            ss << "  [" << i << "] " << bundle.instructions[i].GetDisassembly() << "\n";
        }
        
        return ss.str();
    } catch (const std::exception& e) {
        return std::string("<disassembly error: ") + e.what() + ">";
    }
}

bool IA64ISAPlugin::hasFeature(const std::string& feature) const {
    // IA-64 features
    if (feature == "predication") return true;
    if (feature == "register_rotation") return true;
    if (feature == "register_stack") return true;
    if (feature == "bundles") return true;
    if (feature == "epic") return true;
    if (feature == "speculation") return false;  // Not yet implemented
    if (feature == "advanced_loads") return false;  // Not yet implemented
    
    return false;
}

// ============================================================================
// IA-64 Specific Methods
// ============================================================================

uint64_t IA64ISAPlugin::readGR(size_t index) const {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    
    // Apply rotation for stacked registers (GR32-GR127)
    size_t physical = applyRegisterRotation(index, 'G');
    return state_.getCPUState().GetGR(physical);
}

void IA64ISAPlugin::writeGR(size_t index, uint64_t value) {
    if (index >= NUM_GENERAL_REGISTERS) {
        throw std::out_of_range("General register index out of range");
    }
    
    // GR0 is hardwired to zero
    if (index == 0) {
        return;
    }
    
    size_t physical = applyRegisterRotation(index, 'G');
    state_.getCPUState().SetGR(physical, value);
}

bool IA64ISAPlugin::readPR(size_t index) const {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    
    size_t physical = applyRegisterRotation(index, 'P');
    return state_.getCPUState().GetPR(physical);
}

void IA64ISAPlugin::writePR(size_t index, bool value) {
    if (index >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register index out of range");
    }
    
    // PR0 is hardwired to true
    if (index == 0) {
        return;
    }
    
    size_t physical = applyRegisterRotation(index, 'P');
    state_.getCPUState().SetPR(physical, value);
}

uint64_t IA64ISAPlugin::readBR(size_t index) const {
    return state_.getCPUState().GetBR(index);
}

void IA64ISAPlugin::writeBR(size_t index, uint64_t value) {
    state_.getCPUState().SetBR(index, value);
}

uint64_t IA64ISAPlugin::getCFM() const {
    return state_.getCPUState().GetCFM();
}

void IA64ISAPlugin::setCFM(uint64_t value) {
    state_.getCPUState().SetCFM(value);
}

void IA64ISAPlugin::queueInterrupt(uint8_t vector) {
    state_.pendingInterrupts_.push_back(vector);
}

bool IA64ISAPlugin::hasPendingInterrupt() const {
    return !state_.pendingInterrupts_.empty();
}

void IA64ISAPlugin::setInterruptsEnabled(bool enabled) {
    // Set/clear interrupt enable bit in PSR
    uint64_t psr = state_.getCPUState().GetPSR();
    if (enabled) {
        psr |= (1ULL << 14);  // PSR.i bit
    } else {
        psr &= ~(1ULL << 14);
    }
    state_.getCPUState().SetPSR(psr);
}

bool IA64ISAPlugin::areInterruptsEnabled() const {
    uint64_t psr = state_.getCPUState().GetPSR();
    return (psr & (1ULL << 14)) != 0;  // PSR.i bit
}

void IA64ISAPlugin::setInterruptVectorBase(uint64_t baseAddress) {
    state_.interruptVectorBase_ = baseAddress;
}

uint64_t IA64ISAPlugin::getInterruptVectorBase() const {
    return state_.interruptVectorBase_;
}

bool IA64ISAPlugin::isAtBundleBoundary() const {
    return state_.currentSlot_ == 0 && !state_.bundleValid_;
}

size_t IA64ISAPlugin::getCurrentSlot() const {
    return state_.currentSlot_;
}

bool IA64ISAPlugin::isProfilingEnabled() const {
    return profiler_ != nullptr && profiler_->isEnabled();
}

// ============================================================================
// Internal Helper Methods
// ============================================================================

void IA64ISAPlugin::fetchBundle(IMemory& memory) {
    uint64_t ip = state_.getCPUState().GetIP();
    uint8_t bundleData[16];
    
    try {
        memory.Read(ip, bundleData, 16);

        uint64_t descriptorCode = 0;
        uint64_t descriptorGp = 0;
        if (tryResolveIa64FunctionDescriptor(memory, ip, descriptorCode, descriptorGp) &&
            descriptorCode != ip) {
            const uint64_t oldGp = state_.getCPUState().GetGR(1);
            if (descriptorGp != 0 && descriptorGp != oldGp) {
                state_.getCPUState().SetGR(1, descriptorGp);
            }
            state_.getCPUState().SetGR(38, state_.getCPUState().GetGR(1));
            state_.getCPUState().SetIP(descriptorCode);
            ip = descriptorCode;
            memory.Read(ip, bundleData, 16);
            std::cout << "[IA64-DESC] fetch bundle descriptor ip=0x" << std::hex
                      << state_.getCPUState().GetIP()
                      << " resolvedCode=0x" << descriptorCode
                      << " descriptorGp=0x" << descriptorGp
                      << " oldR1=0x" << oldGp
                      << " newR1=0x" << state_.getCPUState().GetGR(1)
                      << std::dec << std::endl;
        }

        state_.currentBundle_ = decoder_.DecodeBundleAt(bundleData, ip);
        state_.bundleValid_ = true;
        capturePredicateGroupSnapshot();
        std::ostringstream ctx;
        ctx << "bundleIP=" << BootStageTrace::Hex(ip)
            << " template=" << BootStageTrace::Hex(static_cast<uint64_t>(state_.currentBundle_.templateType))
            << " instructionCount=" << state_.currentBundle_.instructions.size()
            << " bytes=" << readHexBytesPreview(memory, ip, 16)
            << " " << cpuSummary(state_.getCPUState());
        BootStageTrace::Stage(100, "First bundle fetched", ctx.str());
    } catch (const std::exception& e) {
        const auto& cpu = state_.getCPUState();
        BootStageTrace::EventOnce(
            "FIRST_FAULT_TRAP",
            "kind=bundle_fetch ip=" + BootStageTrace::Hex(ip) +
            " reason=\"" + std::string(e.what()) + "\" " + cpuSummary(cpu));
        std::cerr << "[EFI-MILESTONE] Fatal IA-64 bundle fetch outside guest memory"
                  << " ip=0x" << std::hex << ip
                  << " br0=0x" << cpu.GetBR(0)
                  << " gp(r1)=0x" << cpu.GetGR(1)
                  << " r2=0x" << cpu.GetGR(2)
                  << " r8=0x" << cpu.GetGR(8)
                  << " slot=" << std::dec << state_.currentSlot_
                  << " reason=\"" << e.what() << "\". "
                  << "Halting instead of reusing the previous decoded bundle; "
                  << "suspect corrupted IA-64 control-flow data/function descriptor before kernel handoff.\n";
        state_.currentBundle_ = Bundle();
        state_.bundleValid_ = false;
    }
}

void IA64ISAPlugin::capturePredicateGroupSnapshot() {
    for (size_t i = 0; i < NUM_PREDICATE_REGISTERS; ++i) {
        state_.predicateGroupSnapshot_[i] = state_.getCPUState().GetPR(i);
    }
}

void IA64ISAPlugin::captureCallOutputRegisters() {
    pendingCallInputs_.clear();

    const uint8_t sof = state_.getCPUState().GetSOF();
    const uint8_t sol = state_.getCPUState().GetSOL();
    if (sof <= sol) {
        return;
    }

    const size_t outputCount = static_cast<size_t>(sof - sol);
    const size_t firstOutput = 32 + sol;
    for (size_t i = 0; i < outputCount && firstOutput + i < NUM_GENERAL_REGISTERS; ++i) {
        const uint64_t value = state_.getCPUState().GetGR(firstOutput + i);
        pendingCallInputs_.push_back(value);
        state_.getCPUState().SetGR(32 + i, value);
    }
}

void IA64ISAPlugin::applyPendingCallInputRegisters() {
    if (pendingCallInputs_.empty()) {
        return;
    }

    for (size_t i = 0; i < pendingCallInputs_.size() && 32 + i < NUM_GENERAL_REGISTERS; ++i) {
        state_.getCPUState().SetGR(32 + i, pendingCallInputs_[i]);
    }

    pendingCallInputs_.clear();
}

void IA64ISAPlugin::saveCallFrame(uint64_t returnAddress) {
    CallFrameSnapshot frame{};
    frame.cfm = state_.getCPUState().GetCFM();
    frame.returnAddress = returnAddress;
    for (size_t i = 0; i < frame.stackedRegisters.size(); ++i) {
        frame.stackedRegisters[i] = state_.getCPUState().GetGR(NUM_STATIC_GR + i);
    }
    callFrameStack_.push_back(frame);
}

void IA64ISAPlugin::restoreCallFrame(uint64_t branchTarget) {
    if (callFrameStack_.empty()) {
        return;
    }

    size_t matchIndex = callFrameStack_.size();
    while (matchIndex > 0) {
        --matchIndex;
        if (callFrameStack_[matchIndex].returnAddress == branchTarget) {
            break;
        }
    }
    if (matchIndex >= callFrameStack_.size() ||
        callFrameStack_[matchIndex].returnAddress != branchTarget) {
        return;
    }

    const CallFrameSnapshot frame = callFrameStack_[matchIndex];
    callFrameStack_.erase(callFrameStack_.begin() + matchIndex, callFrameStack_.end());

    for (size_t i = 0; i < frame.stackedRegisters.size(); ++i) {
        state_.getCPUState().SetGR(NUM_STATIC_GR + i, frame.stackedRegisters[i]);
    }
    state_.getCPUState().SetCFM(frame.cfm);
}

void IA64ISAPlugin::rememberBranchTarget(uint64_t target) {
    lastBranchTargets_.push_back(target);
    if (lastBranchTargets_.size() > 5) {
        lastBranchTargets_.erase(lastBranchTargets_.begin());
    }
}

void IA64ISAPlugin::recordRecentInstruction(uint64_t ip, size_t slot, const std::string& disasm) {
    recentInstructions_.push_back(RecentInstructionTrace{ip, slot, disasm});
    if (recentInstructions_.size() > 16) {
        recentInstructions_.pop_front();
    }

    constexpr size_t loopWindow = 6;
    if (recentInstructions_.size() >= loopWindow * 2) {
        const size_t start = recentInstructions_.size() - loopWindow * 2;
        bool repeated = true;
        for (size_t i = 0; i < loopWindow; ++i) {
            const auto& previous = recentInstructions_[start + i];
            const auto& current = recentInstructions_[start + loopWindow + i];
            if (previous.ip != current.ip ||
                previous.slot != current.slot ||
                previous.disasm != current.disasm) {
                repeated = false;
                break;
            }
        }

        if (repeated) {
            ++recentInstructionSequenceRepeatCount_;
            if (recentInstructionSequenceRepeatCount_ == 1 ||
                (recentInstructionSequenceRepeatCount_ % 32) == 0) {
                const auto& first = recentInstructions_[recentInstructions_.size() - loopWindow];
                const auto& last = recentInstructions_.back();
                std::cout << "[IA64-LOOP] repeated instruction window count="
                          << recentInstructionSequenceRepeatCount_
                          << " startIP=0x" << std::hex << first.ip
                          << " endIP=0x" << last.ip
                          << " sequence=";
                for (size_t i = recentInstructions_.size() - loopWindow; i < recentInstructions_.size(); ++i) {
                    const auto& entry = recentInstructions_[i];
                    if (i != recentInstructions_.size() - loopWindow) {
                        std::cout << " | ";
                    }
                    std::cout << "[ip=0x" << std::hex << entry.ip
                              << " slot=" << std::dec << entry.slot
                              << " \"" << entry.disasm << "\"]";
                }
                std::cout << std::dec << std::endl;
            }
        } else {
            recentInstructionSequenceRepeatCount_ = 0;
        }
    }
}

void IA64ISAPlugin::recordTrackedRegisterWrite(size_t reg, uint64_t value, uint64_t ip, size_t slot, const std::string& disasm) {
    if (reg != 16 && reg != 17) {
        return;
    }

    recentTrackedRegisterWrites_.push_back(RecentRegisterWriteTrace{reg, value, ip, slot, disasm});
    if (recentTrackedRegisterWrites_.size() > 16) {
        recentTrackedRegisterWrites_.pop_front();
    }
}

void IA64ISAPlugin::dumpRecentFaultContext(const CPUState& cpu, uint64_t ip, size_t slot, const InstructionEx& instr, uint64_t baseBefore) const {
    std::cerr << "[IA64-FAULT-HISTORY] ip=0x" << std::hex << ip
              << " slot=" << std::dec << slot
              << " disasm=\"" << instr.GetDisassembly() << "\""
              << " baseBefore=0x" << std::hex << baseBefore
              << " r1=0x" << cpu.GetGR(1)
              << " r12=0x" << cpu.GetGR(12)
              << " r14=0x" << cpu.GetGR(14)
              << " r16=0x" << cpu.GetGR(16)
              << " r17=0x" << cpu.GetGR(17)
              << " r18=0x" << cpu.GetGR(18)
              << " r32=0x" << cpu.GetGR(32)
              << " r33=0x" << cpu.GetGR(33)
              << " r34=0x" << cpu.GetGR(34)
              << " r35=0x" << cpu.GetGR(35)
              << std::dec << "\n";

    std::cerr << "[IA64-FAULT-HISTORY] recent-instructions:";
    if (recentInstructions_.empty()) {
        std::cerr << " <empty>";
    } else {
        for (const auto& entry : recentInstructions_) {
            std::cerr << " [ip=0x" << std::hex << entry.ip
                      << " slot=" << std::dec << entry.slot
                      << " \"" << entry.disasm << "\"]";
        }
    }
    std::cerr << std::dec << "\n";

    std::cerr << "[IA64-FAULT-HISTORY] recent-r16-r17-writes:";
    if (recentTrackedRegisterWrites_.empty()) {
        std::cerr << " <empty>";
    } else {
        for (const auto& entry : recentTrackedRegisterWrites_) {
            std::cerr << " [r" << std::dec << entry.reg
                      << "=0x" << std::hex << entry.value
                      << " ip=0x" << entry.ip
                      << " slot=" << std::dec << entry.slot
                      << " \"" << entry.disasm << "\"]";
        }
    }
    std::cerr << std::dec << "\n";

    std::ostringstream history;
    history << "ip=" << BootStageTrace::Hex(ip)
            << " slot=" << slot
            << " disasm=\"" << instr.GetDisassembly() << "\""
            << " baseBefore=" << BootStageTrace::Hex(baseBefore)
            << " r1=" << BootStageTrace::Hex(cpu.GetGR(1))
            << " r12=" << BootStageTrace::Hex(cpu.GetGR(12))
            << " recent-r16-r17-writes=";
    if (recentTrackedRegisterWrites_.empty()) {
        history << "<empty>";
    } else {
        history << "[";
        for (size_t i = 0; i < recentTrackedRegisterWrites_.size(); ++i) {
            if (i != 0) {
                history << ", ";
            }
            const auto& entry = recentTrackedRegisterWrites_[i];
            history << "r" << entry.reg
                    << "=0x" << std::hex << entry.value
                    << " ip=0x" << entry.ip
                    << " slot=" << std::dec << entry.slot
                    << " \"" << entry.disasm << "\"";
        }
        history << "]";
    }
    BootStageTrace::EventOnce("FIRST_FAULT_REGISTER_HISTORY", history.str());
}

uint64_t IA64ISAPlugin::allocateEfiPool(IMemory& memory, uint64_t size, uint64_t alignment) {
    const uint64_t allocation = alignUp(efiPoolNext_, alignment == 0 ? 16 : alignment);
    const uint64_t alignedSize = alignUp(size, 16);
    if (size == 0 || allocation > EFI_POOL_END || alignedSize > (EFI_POOL_END - allocation)) {
        return 0;
    }
    std::vector<uint8_t> zero(static_cast<size_t>(alignedSize), 0);
    memory.Write(allocation, zero.data(), zero.size());
    efiPoolNext_ = allocation + alignedSize;
    return allocation;
}

void IA64ISAPlugin::logEfiServiceCall(IMemory& memory,
                                      const char* serviceName,
                                      uint64_t callerIP,
                                      uint64_t descriptorAddress,
                                      uint64_t codePointer,
                                      uint64_t status) {
    CPUState& cpu = state_.getCPUState();
    lastEfiCallName_ = serviceName ? serviceName : "unknown";
    lastEfiCallIP_ = callerIP;
    lastDescriptorAddress_ = descriptorAddress;
    lastDescriptorCode_ = codePointer;
    lastDescriptorGp_ = cpu.GetGR(1);
    std::string descriptor = describeEfiDescriptor(descriptorAddress);
    if (descriptor.empty()) {
        descriptor = describeEfiTableSlot(descriptorAddress);
    }
    {
        std::ostringstream ctx;
        ctx << "service=\"" << lastEfiCallName_ << "\""
            << " callerIP=" << BootStageTrace::Hex(callerIP)
            << " descriptor=" << BootStageTrace::Hex(descriptorAddress)
            << " code=" << BootStageTrace::Hex(codePointer)
            << " gp=" << BootStageTrace::Hex(cpu.GetGR(1))
            << " args=[" << BootStageTrace::Hex(readCallerOutputRegister(cpu, 0))
            << "," << BootStageTrace::Hex(readCallerOutputRegister(cpu, 1))
            << "," << BootStageTrace::Hex(readCallerOutputRegister(cpu, 2))
            << "," << BootStageTrace::Hex(readCallerOutputRegister(cpu, 3))
            << "," << BootStageTrace::Hex(readCallerOutputRegister(cpu, 4))
            << "," << BootStageTrace::Hex(readCallerOutputRegister(cpu, 5))
            << "] status=" << BootStageTrace::Hex(status)
            << " statusName=\"" << efiStatusName(status) << "\"";
        if (!descriptor.empty()) {
            ctx << " descriptorName=\"" << descriptor << "\"";
        }
        BootStageTrace::Stage(130, "First EFI service call observed", ctx.str());
        BootStageTrace::Event("EFI_CALL", ctx.str());
        if (lastEfiCallName_ == "BootServices.GetMemoryMap") {
            BootStageTrace::Stage(160, "GetMemoryMap observed", ctx.str());
        } else if (lastEfiCallName_ == "BootServices.ExitBootServices") {
            BootStageTrace::Stage(170, "ExitBootServices attempted", ctx.str());
        }
    }
    std::cout << "[EFI-CALL] service=" << lastEfiCallName_
              << " callerIP=0x" << std::hex << callerIP
              << " descriptor=0x" << descriptorAddress
              << " code=0x" << codePointer
              << " gp(r1)=0x" << cpu.GetGR(1)
              << " args=[0x" << readCallerOutputRegister(cpu, 0)
              << ",0x" << readCallerOutputRegister(cpu, 1)
              << ",0x" << readCallerOutputRegister(cpu, 2)
              << ",0x" << readCallerOutputRegister(cpu, 3)
              << ",0x" << readCallerOutputRegister(cpu, 4)
              << ",0x" << readCallerOutputRegister(cpu, 5)
              << "] status=0x" << status << " (" << efiStatusName(status) << ")";
    if (!descriptor.empty()) {
        std::cout << " descriptorName=\"" << descriptor << "\"";
    }
    std::cout << " descriptorBytes=" << readHexBytesPreview(memory, descriptorAddress, 16)
              << std::dec << std::endl;
}

bool IA64ISAPlugin::ensureEfiBootFat(IMemory& memory) {
    if (efiBootFat_ && efiBootFat_->isValid()) {
        return true;
    }

    uint64_t signature = 0;
    uint64_t base = 0;
    uint64_t size = 0;
    try {
        memory.Read(EFI_BOOT_IMAGE_METADATA_ADDR, reinterpret_cast<uint8_t*>(&signature), sizeof(signature));
        memory.Read(EFI_BOOT_IMAGE_METADATA_ADDR + 8, reinterpret_cast<uint8_t*>(&base), sizeof(base));
        memory.Read(EFI_BOOT_IMAGE_METADATA_ADDR + 16, reinterpret_cast<uint8_t*>(&size), sizeof(size));
    } catch (const std::exception&) {
        return false;
    }
    if (signature != EFI_BOOT_IMAGE_METADATA_SIGNATURE || base == 0 || size == 0 || size > 64ULL * 1024ULL * 1024ULL) {
        return false;
    }

    efiBootImage_.assign(static_cast<size_t>(size), 0);
    try {
        memory.Read(base, efiBootImage_.data(), efiBootImage_.size());
    } catch (const std::exception&) {
        efiBootImage_.clear();
        return false;
    }

    efiBootFat_ = std::make_unique<guideXOS::FATParser>();
    if (!efiBootFat_->parse(efiBootImage_.data(), efiBootImage_.size())) {
        std::cout << "[EFI-FILE] boot image metadata present but FAT parse failed base=0x"
                  << std::hex << base << " size=0x" << size << std::dec << std::endl;
        efiBootFat_.reset();
        return false;
    }

    std::cout << "[EFI-MILESTONE] EFI FileProtocol backing FAT boot image is available"
              << " base=0x" << std::hex << base
              << " size=0x" << size << std::dec << std::endl;
    return true;
}

void IA64ISAPlugin::installFileProtocolTable(IMemory& memory, uint64_t protocolAddress) {
    auto write64 = [&](uint64_t offset, uint64_t value) {
        memory.Write(protocolAddress + offset, reinterpret_cast<const uint8_t*>(&value), sizeof(value));
    };
    write64(0x00, 0x00010000ULL);
    write64(0x08, EFI_FILE_OPEN_STUB_DESC_ADDR);
    write64(0x10, EFI_FILE_CLOSE_STUB_DESC_ADDR);
    write64(0x18, EFI_UNSUPPORTED_STUB_DESC_ADDR);
    write64(0x20, EFI_FILE_READ_STUB_DESC_ADDR);
    write64(0x28, EFI_UNSUPPORTED_STUB_DESC_ADDR);
    write64(0x30, EFI_FILE_GET_POSITION_STUB_DESC_ADDR);
    write64(0x38, EFI_FILE_SET_POSITION_STUB_DESC_ADDR);
    write64(0x40, EFI_FILE_GET_INFO_STUB_DESC_ADDR);
    write64(0x48, EFI_UNSUPPORTED_STUB_DESC_ADDR);
    write64(0x50, EFI_SUCCESS_STUB_DESC_ADDR);
}

uint64_t IA64ISAPlugin::handleEfiFileOpen(IMemory& memory) {
    ++efiFileOpenCalls_;
    const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t newHandleOut = readCallerOutputRegister(state_.getCPUState(), 1);
    const uint64_t fileNameAddress = readCallerOutputRegister(state_.getCPUState(), 2);
    const uint64_t openMode = readCallerOutputRegister(state_.getCPUState(), 3);
    const uint64_t attributes = readCallerOutputRegister(state_.getCPUState(), 4);
    const std::string fileName = readUtf16AsciiString(memory, fileNameAddress);
    std::cout << "[EFI-MILESTONE] possible kernel/config filename requested path=\""
              << fileName << "\" parent=0x" << std::hex << thisProtocol
              << " mode=0x" << openMode << " attrs=0x" << attributes << std::dec << std::endl;

    if (newHandleOut == 0 || (openMode & ~1ULL) != 0) {
        return (openMode & ~1ULL) != 0 ? EFI_STATUS_WRITE_PROTECTED : EFI_STATUS_INVALID_PARAMETER;
    }
    if (!ensureEfiBootFat(memory)) {
        const uint64_t nullHandle = 0;
        memory.Write(newHandleOut, reinterpret_cast<const uint8_t*>(&nullHandle), sizeof(nullHandle));
        return EFI_STATUS_NO_MEDIA;
    }

    auto parentIt = efiFileHandles_.find(thisProtocol);
    const std::string parentPath = parentIt != efiFileHandles_.end() ? parentIt->second.path : std::string();
    const std::string path = joinEfiPath(parentPath, fileName);

    guideXOS::FATFileInfo info{};
    std::vector<uint8_t> data;
    std::vector<guideXOS::FATFileInfo> entries;
    bool isDirectory = false;
    bool found = false;
    if (path.empty()) {
        found = efiBootFat_->listDirectory("/", entries);
        isDirectory = true;
    } else if (efiBootFat_->findFile(path, info)) {
        isDirectory = info.isDirectory;
        found = isDirectory
            ? efiBootFat_->listDirectory(path, entries)
            : efiBootFat_->readFile(info, data);
    }
    if (!found) {
        const uint64_t nullHandle = 0;
        memory.Write(newHandleOut, reinterpret_cast<const uint8_t*>(&nullHandle), sizeof(nullHandle));
        std::cout << "[EFI-FILE] Open path=\"" << path << "\" -> EFI_NOT_FOUND" << std::endl;
        return EFI_STATUS_NOT_FOUND;
    }

    const uint64_t handleAddress = allocateEfiPool(memory, 0x60);
    if (handleAddress == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    installFileProtocolTable(memory, handleAddress);
    memory.Write(newHandleOut, reinterpret_cast<const uint8_t*>(&handleAddress), sizeof(handleAddress));

    EfiFileHandle handle;
    handle.protocolAddress = handleAddress;
    handle.path = path;
    handle.isDirectory = isDirectory;
    handle.data = std::move(data);
    handle.directoryEntries = std::move(entries);
    efiFileHandles_[handleAddress] = std::move(handle);

    if (efiFirstSuccessfulFileOpen_ == 0) {
        efiFirstSuccessfulFileOpen_ = efiFileOpenCalls_;
        std::cout << "[EFI-MILESTONE] first successful file Open path=\"" << path
                  << "\" handle=0x" << std::hex << handleAddress << std::dec << std::endl;
    }
    std::cout << "[EFI-FILE] Open path=\"" << path << "\" -> handle=0x"
              << std::hex << handleAddress << std::dec
              << (isDirectory ? " [DIR]" : " [FILE]")
              << " size=" << efiFileHandles_[handleAddress].data.size()
              << " -> EFI_SUCCESS" << std::endl;
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiFileRead(IMemory& memory) {
    ++efiFileReadCalls_;
    const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t bufferSizeAddress = readCallerOutputRegister(state_.getCPUState(), 1);
    const uint64_t bufferAddress = readCallerOutputRegister(state_.getCPUState(), 2);
    if (bufferSizeAddress == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    uint64_t requested = 0;
    memory.Read(bufferSizeAddress, reinterpret_cast<uint8_t*>(&requested), sizeof(requested));
    auto it = efiFileHandles_.find(thisProtocol);
    if (it == efiFileHandles_.end()) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    EfiFileHandle& handle = it->second;

    if (handle.isDirectory) {
        if (handle.directoryIndex >= handle.directoryEntries.size()) {
            const uint64_t zero = 0;
            memory.Write(bufferSizeAddress, reinterpret_cast<const uint8_t*>(&zero), sizeof(zero));
            return EFI_STATUS_SUCCESS;
        }
        const auto& entry = handle.directoryEntries[handle.directoryIndex];
        std::vector<uint8_t> info = makeEfiFileInfoBuffer(entry.name, entry.size, entry.isDirectory);
        if (requested < info.size() || bufferAddress == 0) {
            const uint64_t required = static_cast<uint64_t>(info.size());
            memory.Write(bufferSizeAddress, reinterpret_cast<const uint8_t*>(&required), sizeof(required));
            return EFI_STATUS_BUFFER_TOO_SMALL;
        }
        memory.Write(bufferAddress, info.data(), info.size());
        const uint64_t actual = static_cast<uint64_t>(info.size());
        memory.Write(bufferSizeAddress, reinterpret_cast<const uint8_t*>(&actual), sizeof(actual));
        ++handle.directoryIndex;
        return EFI_STATUS_SUCCESS;
    }

    const uint64_t remaining = handle.position < handle.data.size()
        ? static_cast<uint64_t>(handle.data.size()) - handle.position
        : 0;
    const uint64_t actual = std::min(requested, remaining);
    if (actual > 0 && bufferAddress != 0) {
        memory.Write(bufferAddress, handle.data.data() + static_cast<size_t>(handle.position), static_cast<size_t>(actual));
        handle.position += actual;
        efiTotalFileBytesRead_ += actual;
        std::cout << "[EFI-MILESTONE] first Read over 0 bytes path=\"" << handle.path
                  << "\" bytes=0x" << std::hex << actual
                  << " totalFileBytes=0x" << efiTotalFileBytesRead_ << std::dec << std::endl;
        {
            std::ostringstream ctx;
            ctx << "service=FileProtocol.Read"
                << " handle=" << BootStageTrace::Hex(thisProtocol)
                << " path=\"" << handle.path << "\""
                << " buffer=" << BootStageTrace::Hex(bufferAddress)
                << " requested=" << BootStageTrace::Hex(requested)
                << " actual=" << BootStageTrace::Hex(actual)
                << " newPosition=" << BootStageTrace::Hex(handle.position)
                << " totalFileBytes=" << BootStageTrace::Hex(efiTotalFileBytesRead_);
            BootStageTrace::Stage(150, "First disk read after EFI entry observed", ctx.str());
            BootStageTrace::Event("EFI_FILE_READ", ctx.str());
        }
    }
    memory.Write(bufferSizeAddress, reinterpret_cast<const uint8_t*>(&actual), sizeof(actual));
    std::cout << "[EFI-FILE] Read handle=0x" << std::hex << thisProtocol
              << " path=\"" << handle.path << "\" requested=0x" << requested
              << " actual=0x" << actual << " position=0x" << handle.position
              << std::dec << " -> EFI_SUCCESS" << std::endl;
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiFileGetInfo(IMemory& memory) {
    ++efiFileGetInfoCalls_;
    const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t infoTypeAddress = readCallerOutputRegister(state_.getCPUState(), 1);
    const uint64_t bufferSizeAddress = readCallerOutputRegister(state_.getCPUState(), 2);
    const uint64_t bufferAddress = readCallerOutputRegister(state_.getCPUState(), 3);
    if (bufferSizeAddress == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    EfiGuid guid{};
    const bool hasGuid = readEfiGuid(memory, infoTypeAddress, guid);
    auto it = efiFileHandles_.find(thisProtocol);
    if (it == efiFileHandles_.end() || !hasGuid) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    const EfiFileHandle& handle = it->second;
    std::vector<uint8_t> info;
    if (guid == EFI_FILE_INFO_GUID) {
        const std::string name = handle.path.empty() ? std::string("\\") : handle.path.substr(handle.path.find_last_of("/\\") + 1);
        info = makeEfiFileInfoBuffer(name, static_cast<uint64_t>(handle.data.size()), handle.isDirectory);
    } else if (guid == EFI_FILE_SYSTEM_INFO_GUID) {
        info = makeEfiFileSystemInfoBuffer(static_cast<uint64_t>(efiBootImage_.size()));
    } else {
        return EFI_STATUS_UNSUPPORTED;
    }
    uint64_t provided = 0;
    memory.Read(bufferSizeAddress, reinterpret_cast<uint8_t*>(&provided), sizeof(provided));
    const uint64_t required = static_cast<uint64_t>(info.size());
    memory.Write(bufferSizeAddress, reinterpret_cast<const uint8_t*>(&required), sizeof(required));
    if (bufferAddress == 0 || provided < required) {
        return EFI_STATUS_BUFFER_TOO_SMALL;
    }
    memory.Write(bufferAddress, info.data(), info.size());
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiFileClose(IMemory& memory) {
    (void)memory;
    ++efiFileCloseCalls_;
    const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
    if (thisProtocol != EFI_ROOT_FILE_PROTOCOL_ADDR) {
        efiFileHandles_.erase(thisProtocol);
    }
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiFileGetPosition(IMemory& memory) {
    ++efiFileGetPositionCalls_;
    const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t positionOut = readCallerOutputRegister(state_.getCPUState(), 1);
    auto it = efiFileHandles_.find(thisProtocol);
    if (it == efiFileHandles_.end() || positionOut == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    memory.Write(positionOut, reinterpret_cast<const uint8_t*>(&it->second.position), sizeof(uint64_t));
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiFileSetPosition(IMemory& memory) {
    (void)memory;
    ++efiFileSetPositionCalls_;
    const uint64_t thisProtocol = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t position = readCallerOutputRegister(state_.getCPUState(), 1);
    auto it = efiFileHandles_.find(thisProtocol);
    if (it == efiFileHandles_.end()) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    if (position == 0xFFFFFFFFFFFFFFFFULL) {
        it->second.position = static_cast<uint64_t>(it->second.data.size());
    } else {
        it->second.position = std::min<uint64_t>(position, static_cast<uint64_t>(it->second.data.size()));
    }
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiLocateHandle(IMemory& memory) {
    ++efiLocateHandleCalls_;
    const uint64_t bufferSizeAddress = readCallerOutputRegister(state_.getCPUState(), 2);
    const uint64_t bufferAddress = readCallerOutputRegister(state_.getCPUState(), 3);
    const uint64_t required = sizeof(uint64_t) * 2;
    if (bufferSizeAddress == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    uint64_t provided = 0;
    memory.Read(bufferSizeAddress, reinterpret_cast<uint8_t*>(&provided), sizeof(provided));
    memory.Write(bufferSizeAddress, reinterpret_cast<const uint8_t*>(&required), sizeof(required));
    if (bufferAddress == 0 || provided < required) {
        return EFI_STATUS_BUFFER_TOO_SMALL;
    }
    const uint64_t handles[] = { 0x1ULL, 0x40ULL };
    memory.Write(bufferAddress, reinterpret_cast<const uint8_t*>(handles), sizeof(handles));
    return EFI_STATUS_SUCCESS;
}

uint64_t IA64ISAPlugin::handleEfiLocateProtocol(IMemory& memory) {
    ++efiLocateProtocolCalls_;
    const uint64_t protocolGuid = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t interfaceOut = readCallerOutputRegister(state_.getCPUState(), 2);
    if (interfaceOut == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    EfiGuid guid{};
    const bool hasGuid = readEfiGuid(memory, protocolGuid, guid);
    uint64_t protocolAddress = 0;
    if (hasGuid && guid == EFI_LOADED_IMAGE_PROTOCOL_GUID) {
        protocolAddress = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
    } else if (hasGuid && guid == EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID) {
        protocolAddress = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_ADDR;
    }
    memory.Write(interfaceOut, reinterpret_cast<const uint8_t*>(&protocolAddress), sizeof(protocolAddress));
    return protocolAddress != 0 ? EFI_STATUS_SUCCESS : EFI_STATUS_NOT_FOUND;
}

uint64_t IA64ISAPlugin::handleEfiGetMemoryMap(IMemory& memory) {
    ++efiGetMemoryMapCalls_;
    const uint64_t memoryMapSizeAddress = readCallerOutputRegister(state_.getCPUState(), 0);
    const uint64_t memoryMapAddress = readCallerOutputRegister(state_.getCPUState(), 1);
    const uint64_t mapKeyAddress = readCallerOutputRegister(state_.getCPUState(), 2);
    const uint64_t descriptorSizeAddress = readCallerOutputRegister(state_.getCPUState(), 3);
    const uint64_t descriptorVersionAddress = readCallerOutputRegister(state_.getCPUState(), 4);
    if (memoryMapSizeAddress == 0) {
        return EFI_STATUS_INVALID_PARAMETER;
    }
    constexpr uint64_t descriptorSize = 48;
    constexpr uint64_t descriptorVersion = 1;
    constexpr uint64_t requiredSize = descriptorSize * 3;
    uint64_t provided = 0;
    memory.Read(memoryMapSizeAddress, reinterpret_cast<uint8_t*>(&provided), sizeof(provided));
    memory.Write(memoryMapSizeAddress, reinterpret_cast<const uint8_t*>(&requiredSize), sizeof(requiredSize));
    if (descriptorSizeAddress != 0) {
        memory.Write(descriptorSizeAddress, reinterpret_cast<const uint8_t*>(&descriptorSize), sizeof(descriptorSize));
    }
    if (descriptorVersionAddress != 0) {
        memory.Write(descriptorVersionAddress, reinterpret_cast<const uint8_t*>(&descriptorVersion), sizeof(descriptorVersion));
    }
    if (mapKeyAddress != 0) {
        const uint64_t mapKey = 1;
        memory.Write(mapKeyAddress, reinterpret_cast<const uint8_t*>(&mapKey), sizeof(mapKey));
    }
    if (memoryMapAddress == 0 || provided < requiredSize) {
        return EFI_STATUS_BUFFER_TOO_SMALL;
    }
    auto writeDescriptor = [&](uint64_t address, uint32_t type, uint64_t physicalStart, uint64_t pages, uint64_t attributes) {
        memory.Write(address + 0x00, reinterpret_cast<const uint8_t*>(&type), sizeof(type));
        memory.Write(address + 0x08, reinterpret_cast<const uint8_t*>(&physicalStart), sizeof(physicalStart));
        memory.Write(address + 0x10, reinterpret_cast<const uint8_t*>(&physicalStart), sizeof(physicalStart));
        memory.Write(address + 0x18, reinterpret_cast<const uint8_t*>(&pages), sizeof(pages));
        memory.Write(address + 0x20, reinterpret_cast<const uint8_t*>(&attributes), sizeof(attributes));
    };
    writeDescriptor(memoryMapAddress, 7, 0x100000ULL, 0x17F00ULL, 0xFULL);
    writeDescriptor(memoryMapAddress + descriptorSize, 2, EFI_HANDOFF_REGION_BASE, 0x200ULL, 0xFULL);
    writeDescriptor(memoryMapAddress + descriptorSize * 2, 4, EFI_BOOT_IMAGE_GUEST_BASE, 0x2000ULL, 0xFULL);
    std::cout << "[EFI-MILESTONE] GetMemoryMap returned descriptors=3 size=0x"
              << std::hex << requiredSize << " key=1" << std::dec << std::endl;
    return EFI_STATUS_SUCCESS;
}

void IA64ISAPlugin::executeInstruction(IMemory& memory, const InstructionEx& instr, bool ignorePredicate) {
    try {
        CPUState& cpu = state_.getCPUState();
        const uint64_t currentIP = cpu.GetIP();
        const bool traceScanLoop =
            currentIP >= 0x36D90ULL && currentIP <= 0x36F10ULL;
        static uint64_t scanLoopTraceCount = 0;
        const bool emitScanLoopTrace =
            traceScanLoop && (scanLoopTraceCount < 64 || (scanLoopTraceCount % 256) == 0);
        if (emitScanLoopTrace) {
            logScanLoopState("pre", cpu, state_.currentSlot_, instr);
        }
        recordRecentInstruction(cpu.GetIP(), state_.currentSlot_, instr.GetDisassembly());
        static constexpr uint64_t EFI_POST_SIMPLEFS_OUT_WATCH_ADDR = 0x1FF93010ULL;
        static constexpr size_t EFI_POST_SIMPLEFS_OUT_WATCH_SIZE = 0x10;
        const size_t storeSize = storeSizeForInstruction(instr.GetType());
        const uint64_t storeAddress = isStoreInstruction(instr.GetType())
            ? cpu.GetGR(instr.GetSrc1())
            : 0;
        const bool watchPostSimpleFsOutStore =
            storeSize != 0 &&
            rangesOverlap(storeAddress, storeSize,
                          EFI_POST_SIMPLEFS_OUT_WATCH_ADDR,
                          EFI_POST_SIMPLEFS_OUT_WATCH_SIZE);
        if (watchPostSimpleFsOutStore) {
            std::cout << "[EFI-OUT-WATCH] pre store ip=0x" << std::hex << cpu.GetIP()
                      << " slot=" << std::dec << state_.currentSlot_
                      << " type=\"" << instr.GetDisassembly() << "\""
                      << " target=0x" << std::hex << storeAddress
                      << " size=0x" << storeSize
                      << " baseReg=r" << std::dec << static_cast<int>(instr.GetSrc1())
                      << " dstReg=r" << static_cast<int>(instr.GetDst())
                      << " src2Reg=r" << static_cast<int>(instr.GetSrc2())
                      << std::hex
                      << " r1=0x" << cpu.GetGR(1)
                      << " r12=0x" << cpu.GetGR(12)
                      << " r32=0x" << cpu.GetGR(32)
                      << " r36=0x" << cpu.GetGR(36)
                      << " r37=0x" << cpu.GetGR(37)
                      << " r65=0x" << cpu.GetGR(65)
                      << " r66=0x" << cpu.GetGR(66)
                      << " watchBytes="
                      << readHexBytesPreview(memory, EFI_POST_SIMPLEFS_OUT_WATCH_ADDR,
                                             EFI_POST_SIMPLEFS_OUT_WATCH_SIZE)
                      << " lastEfiCall=\"" << lastEfiCallName_ << "\""
                      << " lastEfiIP=0x" << lastEfiCallIP_
                      << std::dec << std::endl;
        }
        const uint64_t loadedImageSlotCandidate = cpu.GetGR(36);
        uint64_t loadedImageSlotValue = 0;
        const bool postSimpleFsLoadedImageLoad =
            cpu.GetIP() == 0x1D70ULL &&
            state_.currentSlot_ == 0 &&
            instr.GetType() == InstructionType::LD8 &&
            instr.GetDst() == 20 &&
            instr.GetSrc1() == 36 &&
            lastEfiCallName_ == "BootServices.HandleProtocol" &&
            loadedImageSlotCandidate == EFI_POST_SIMPLEFS_OUT_WATCH_ADDR + sizeof(uint64_t) &&
            readGuestU64(memory, loadedImageSlotCandidate, loadedImageSlotValue) &&
            loadedImageSlotValue == 0;
        if (postSimpleFsLoadedImageLoad) {
            loadedImageSlotValue = EFI_LOADED_IMAGE_PROTOCOL_ADDR;
            memory.Write(loadedImageSlotCandidate,
                         reinterpret_cast<const uint8_t*>(&loadedImageSlotValue),
                         sizeof(loadedImageSlotValue));
            std::cout << "[EFI-STUB] restored LoadedImage before post-SimpleFS dereference"
                      << " ip=0x" << std::hex << cpu.GetIP()
                      << " slot=" << std::dec << state_.currentSlot_
                      << " address=0x" << std::hex << loadedImageSlotCandidate
                      << " value=0x" << loadedImageSlotValue
                      << " lastEfiIP=0x" << lastEfiCallIP_
                      << std::dec << std::endl;
            BootStageTrace::Event("EFI_POST_SIMPLEFS_LOADED_IMAGE_RESTORED",
                "ip=" + BootStageTrace::Hex(cpu.GetIP()) +
                " slot=" + std::to_string(state_.currentSlot_) +
                " address=" + BootStageTrace::Hex(loadedImageSlotCandidate) +
                " value=" + BootStageTrace::Hex(loadedImageSlotValue) +
                " lastEfiCall=\"" + lastEfiCallName_ + "\"" +
                " lastEfiIP=" + BootStageTrace::Hex(lastEfiCallIP_));
        }
        instr.Execute(state_.getCPUState(), memory, ignorePredicate);
        if (traceScanLoop && emitScanLoopTrace) {
            logScanLoopState("post", state_.getCPUState(), state_.currentSlot_, instr);
            ++scanLoopTraceCount;
        } else if (traceScanLoop) {
            ++scanLoopTraceCount;
        }
        if (instr.GetDst() == 16 || instr.GetDst() == 17) {
            recordTrackedRegisterWrite(instr.GetDst(), state_.getCPUState().GetGR(instr.GetDst()),
                                       cpu.GetIP(), state_.currentSlot_, instr.GetDisassembly());
        }
        if (watchPostSimpleFsOutStore) {
            std::cout << "[EFI-OUT-WATCH] post store ip=0x" << std::hex << cpu.GetIP()
                      << " slot=" << std::dec << state_.currentSlot_
                      << " type=\"" << instr.GetDisassembly() << "\""
                      << " target=0x" << std::hex << storeAddress
                      << " watchBytes="
                      << readHexBytesPreview(memory, EFI_POST_SIMPLEFS_OUT_WATCH_ADDR,
                                             EFI_POST_SIMPLEFS_OUT_WATCH_SIZE)
                      << std::dec << std::endl;
        }
        if (isAdvancedLoadCheckInstruction(instr.GetType())) {
            std::cout << "[ALAT-STUB] " << instr.GetDisassembly()
                      << " treated as success; ALAT tracking is not implemented"
                      << std::endl;
        }
    } catch (const std::exception& e) {
        CPUState& cpu = state_.getCPUState();
        const uint64_t ip = cpu.GetIP();
        const uint64_t baseBefore = cpu.GetGR(instr.GetSrc1());
        {
            std::ostringstream ctx;
            ctx << "kind=instruction_execute"
                << " ip=" << BootStageTrace::Hex(ip)
                << " slot=" << state_.currentSlot_
                << " targetAddress=" << BootStageTrace::Hex(baseBefore)
                << " disasm=\"" << instr.GetDisassembly() << "\""
                << " reason=\"" << e.what() << "\" "
                << cpuSummary(cpu);
            BootStageTrace::EventOnce("FIRST_FAULT_TRAP", ctx.str());
            BootStageTrace::EventOnce("FIRST_MEMORY_ACCESS_OUTSIDE_NORMAL_RANGE", ctx.str());
        }

        std::cerr << "Error executing instruction at IP=0x" << std::hex << ip
                  << " Slot=" << std::dec << state_.currentSlot_
                  << " raw=0x" << std::hex << instr.GetRawBits()
                  << " disasm=\"" << instr.GetDisassembly() << "\": "
                  << e.what() << std::dec << "\n";
        ++recoveredLoadStoreCount_;
        std::cerr << "[IA64-MEM-RECOVERY] ip=0x" << std::hex << ip
                  << " type=\"" << instr.GetDisassembly() << "\""
                  << " targetAddress=0x" << baseBefore
                  << " targetRegister=r" << std::dec << static_cast<int>(instr.GetDst())
                  << " gp(r1)=0x" << std::hex << cpu.GetGR(1)
                  << " lastEfiCall=\"" << lastEfiCallName_ << "\""
                  << " lastEfiIP=0x" << lastEfiCallIP_
                  << " lastDescriptor=0x" << lastDescriptorAddress_
                  << " lastDescriptorCode=0x" << lastDescriptorCode_
                  << " lastDescriptorGp=0x" << lastDescriptorGp_
                  << " branchHistory=[";
        for (size_t i = 0; i < lastBranchTargets_.size(); ++i) {
            if (i != 0) {
                std::cerr << ",";
            }
            std::cerr << "0x" << lastBranchTargets_[i];
        }
        std::cerr << "] addressHint=";
        if ((baseBefore & 0x00FF000000000000ULL) != 0 &&
            ((baseBefore & 0xFFFFULL) >= 0x20 || ((baseBefore >> 16) & 0xFFFFULL) >= 0x20)) {
            std::cerr << "looks-like-packed-utf16";
        } else if (baseBefore < cpu.GetGR(1) + 0x100000ULL && baseBefore + 0x100000ULL > cpu.GetGR(1)) {
            std::cerr << "gp-relative-near";
        } else if (baseBefore >= EFI_HANDOFF_REGION_BASE && baseBefore < EFI_HANDOFF_REGION_END) {
            std::cerr << "efi-table-or-pool-relative";
        } else if (baseBefore >= 0x100000ULL && baseBefore < 0x200000ULL) {
            std::cerr << "mirrored-section-relative";
        } else if (baseBefore < memory.GetTotalSize()) {
            std::cerr << "image-or-guest-relative";
        } else {
            std::cerr << "totally-invalid";
        }
        std::cerr << " strictRecovery=" << (IA64_STRICT_RECOVERY ? "on" : "off")
                  << std::dec << "\n";
        dumpRecentFaultContext(cpu, ip, state_.currentSlot_, instr, baseBefore);
        if (IA64_STRICT_RECOVERY) {
            throw;
        }

        if (isLoadInstruction(instr.GetType())) {
            if (ip >= 0x6700 && ip < 0x6840) {
                const uint64_t gp = cpu.GetGR(1);
                const uint64_t r33 = cpu.GetGR(33);
                const int64_t r33MinusGp =
                    static_cast<int64_t>(r33) - static_cast<int64_t>(gp);
                const bool r33InDiagnosticMirror = r33 >= 0x100000ULL && r33 < 0x200000ULL;
                const uint64_t r33MirrorRva = r33InDiagnosticMirror ? r33 - 0x100000ULL : 0;
                std::cerr << "[EFI-MILESTONE] IA-64 function-descriptor data load failed before"
                          << " SimpleFS.OpenVolume/kernel handoff"
                          << " ip=0x" << std::hex << ip
                          << " slot=" << std::dec << state_.currentSlot_
                          << " dst=r" << static_cast<int>(instr.GetDst())
                          << " srcBase=r" << static_cast<int>(instr.GetSrc1())
                          << " baseBefore=0x" << std::hex << baseBefore
                          << " gp(r1)=0x" << gp
                          << " r2=0x" << cpu.GetGR(2)
                          << " r8=0x" << cpu.GetGR(8)
                          << " r14=0x" << cpu.GetGR(14)
                          << " r33=0x" << r33
                          << " r33MinusGp=" << std::dec << r33MinusGp << std::hex
                          << " r35=0x" << cpu.GetGR(35)
                          << " br0=0x" << cpu.GetBR(0)
                          << " br6=0x" << cpu.GetBR(6);
                if (r33InDiagnosticMirror) {
                    std::cerr << " r33MirrorRva=0x" << r33MirrorRva;
                    if (r33MirrorRva >= 0x1000ULL && r33MirrorRva < 0x37110ULL) {
                        std::cerr << " r33MirrorSectionHint=.text";
                    } else if (r33MirrorRva >= 0x38000ULL && r33MirrorRva < 0x38540ULL) {
                        std::cerr << " r33MirrorSectionHint=.sdata";
                    } else if (r33MirrorRva >= 0x39000ULL && r33MirrorRva < 0x56390ULL) {
                        std::cerr << " r33MirrorSectionHint=.data";
                    }
                }
                std::cerr << " r33Bytes=" << readHexBytesPreview(memory, r33, 16);
                std::cerr << ". r14 was loaded from r33 and is now the corrupt descriptor pointer;"
                          << " suspect PE/COFF relocation or GP-relative data placement."
                          << std::dec << "\n";
                throw;
            }
            cpu.SetGR(instr.GetDst(), 0);
            bool sanitizedBase = false;
            if (instr.HasImmediate()) {
                cpu.SetGR(instr.GetSrc1(), static_cast<uint64_t>(static_cast<int64_t>(instr.GetImmediate())));
                sanitizedBase = true;
            } else if (instr.GetSrc2() != 0) {
                cpu.SetGR(instr.GetSrc1(), cpu.GetGR(instr.GetSrc2()));
                sanitizedBase = true;
            } else if (instr.GetSrc1() != instr.GetDst()) {
                cpu.SetGR(instr.GetSrc1(), 0);
                sanitizedBase = true;
            }

            std::cerr << "Recovered failed IA-64 load by zeroing r"
                      << static_cast<int>(instr.GetDst())
                      << (sanitizedBase ? ", sanitizing the base register, and continuing.\n"
                                        : " and continuing.\n");
        } else {
            std::cerr << "Treating as NOP and continuing...\n";
        }
    }
}

size_t IA64ISAPlugin::applyRegisterRotation(size_t logicalReg, char regType) const {
    switch (regType) {
        case 'G':  // General register
            if (logicalReg < 32) {
                return logicalReg;
            } else {
                uint8_t rrb = getGRRotationBase();
                size_t rotatingSize = 96;
                size_t offset = (logicalReg - 32 + rrb) % rotatingSize;
                return 32 + offset;
            }
            
        case 'F':  // Floating-point register
            if (logicalReg < 32) {
                return logicalReg;
            } else {
                uint8_t rrb = getFRRotationBase();
                size_t rotatingSize = 96;
                size_t offset = (logicalReg - 32 + rrb) % rotatingSize;
                return 32 + offset;
            }
            
        case 'P':  // Predicate register
            if (logicalReg < 16) {
                return logicalReg;
            } else {
                uint8_t rrb = getPRRotationBase();
                size_t rotatingSize = 48;
                size_t offset = (logicalReg - 16 + rrb) % rotatingSize;
                return 16 + offset;
            }
            
        default:
            return logicalReg;
    }
}

bool IA64ISAPlugin::checkPredicate(size_t predicateReg) const {
    if (predicateReg >= NUM_PREDICATE_REGISTERS) {
        throw std::out_of_range("Predicate register out of range");
    }
    
    size_t physical = applyRegisterRotation(predicateReg, 'P');
    return state_.getCPUState().GetPR(physical);
}

void IA64ISAPlugin::servicePendingInterrupt(IMemory& memory) {
    if (!hasPendingInterrupt() || !areInterruptsEnabled()) {
        return;
    }
    
    // Get the first pending interrupt
    uint8_t vector = state_.pendingInterrupts_.front();
    state_.pendingInterrupts_.erase(state_.pendingInterrupts_.begin());
    
    // Calculate interrupt handler address
    uint64_t handlerAddress = state_.interruptVectorBase_ + (vector * 16);
    
    // In a real implementation, we would:
    // 1. Save current IP to IIP (Interruption Instruction Pointer)
    // 2. Save PSR to IPSR (Interruption PSR)
    // 3. Set IP to handler address
    // 4. Disable interrupts
    // 5. Switch to privileged mode
    
    std::cout << "Servicing interrupt vector " << static_cast<int>(vector)
              << " at handler 0x" << std::hex << handlerAddress << std::dec << "\n";
    
    // For now, just jump to the handler
    state_.getCPUState().SetIP(handlerAddress);
    state_.bundleValid_ = false;
}

uint8_t IA64ISAPlugin::getGRRotationBase() const {
    return state_.getCPUState().GetRRB_GR();
}

uint8_t IA64ISAPlugin::getFRRotationBase() const {
    return state_.getCPUState().GetRRB_FR();
}

uint8_t IA64ISAPlugin::getPRRotationBase() const {
    return state_.getCPUState().GetRRB_PR();
}

// ============================================================================
// Factory Functions
// ============================================================================

std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder) {
    return std::make_unique<IA64ISAPlugin>(decoder);
}

std::unique_ptr<IISA> createIA64ISA(IDecoder& decoder,
                                     SyscallDispatcher* syscallDispatcher,
                                     Profiler* profiler) {
    return std::make_unique<IA64ISAPlugin>(decoder, syscallDispatcher, profiler);
}

} // namespace ia64

#include "VMManager.h"
#include "IA64ISAPlugin.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string isoPath;
    uint64_t cycles = 2'000'000;
    uint64_t memoryMiB = 512;
    uint64_t keyAfterCycles = 300'000;
    struct InputKey {
        uint16_t scanCode = 0;
        uint16_t unicodeChar = 0;
        std::string name;
    };
    std::vector<InputKey> keys;
};

bool parseUnsigned(const char* text, uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') {
        return false;
    }
    value = static_cast<uint64_t>(parsed);
    return true;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool parseInputKey(const std::string& text, Options::InputKey& key) {
    const std::string name = lowercase(text);
    if (name == "enter" || name == "return" || name == "cr") {
        key = {0, 0x000D, "enter"};
        return true;
    }
    if (name == "lf" || name == "linefeed") {
        key = {0, 0x000A, "linefeed"};
        return true;
    }
    if (name == "up") {
        key = {0x0001, 0, "up"};
        return true;
    }
    if (name == "down") {
        key = {0x0002, 0, "down"};
        return true;
    }
    if (name == "left") {
        key = {0x0004, 0, "left"};
        return true;
    }
    if (name == "right") {
        key = {0x0003, 0, "right"};
        return true;
    }
    if (name == "backspace") {
        key = {0, 0x0008, "backspace"};
        return true;
    }
    if (name == "escape" || name == "esc") {
        key = {0, 0x001B, "escape"};
        return true;
    }
    if (text.size() == 1) {
        key = {0, static_cast<uint16_t>(static_cast<unsigned char>(text[0])), text};
        return true;
    }
    return false;
}

bool parseOptions(int argc, char** argv, Options& options) {
    if (argc < 2) {
        return false;
    }

    options.isoPath = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.cycles)) {
                return false;
            }
        } else if (argument == "--memory-mib" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.memoryMiB)) {
                return false;
            }
        } else if (argument == "--key-after-cycles" && i + 1 < argc) {
            if (!parseUnsigned(argv[++i], options.keyAfterCycles)) {
                return false;
            }
        } else if (argument == "--key" && i + 1 < argc) {
            Options::InputKey key;
            if (!parseInputKey(argv[++i], key)) {
                return false;
            }
            options.keys.push_back(key);
        } else if (argument == "--help" || argument == "-h") {
            return false;
        } else {
            return false;
        }
    }
    return !options.isoPath.empty() && options.cycles > 0 && options.memoryMiB >= 1;
}

void printUsage() {
    std::cout << "Usage: ia64_iso_matrix <iso-path> [--cycles N] [--memory-mib N] "
                 "[--key NAME]... [--key-after-cycles N]\n"
              << "Defaults: --cycles 2000000 --memory-mib 512 "
                 "--key-after-cycles 300000\n"
              << "Keys: enter, linefeed, up, down, left, right, backspace, escape, or one character\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return argc >= 2 ? 2 : 0;
    }

    if (std::getenv("GUIDEXOS_MATRIX_QUIET") != nullptr) {
        std::cout.setstate(std::ios_base::failbit);
    }

    try {
        ia64::Logger::getInstance().setLogLevel(ia64::LogLevel::INFO);

        ia64::VMManager manager;
        ia64::VMConfiguration config =
            ia64::VMConfiguration::createMinimal("ia64-iso-matrix");
        config.memory.memorySize =
            static_cast<size_t>(options.memoryMiB) * 1024ULL * 1024ULL;
        config.boot.bootDevice = "disk0";

        ia64::StorageConfiguration storage("disk0", options.isoPath);
        storage.readOnly = true;
        config.addStorageDevice(storage);

        std::cout << "[IA64-MATRIX] iso=\"" << options.isoPath << "\""
                  << " memoryMiB=" << options.memoryMiB
                  << " cycles=" << options.cycles << std::endl;

        const std::string vmId = manager.createVM(config);
        if (vmId.empty()) {
            std::cerr << "[IA64-MATRIX] createVM failed" << std::endl;
            return 1;
        }
        if (!manager.startVM(vmId)) {
            std::cerr << "[IA64-MATRIX] startVM failed vmId=" << vmId << std::endl;
            return 1;
        }

        auto reportRuntime = [&](const char* phase) {
            const ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
            if (vm == nullptr) {
                return;
            }
            std::cout << "[IA64-MATRIX] phase=" << phase
                      << " ip=0x" << std::hex << vm->getIP(0) << std::dec
                      << std::endl;
            std::cerr << "[IA64-MATRIX] phase=" << phase
                      << " ip=0x" << std::hex << vm->getIP(0) << std::dec
                      << std::endl;
        };

        uint64_t executed = 0;
        if (options.keys.empty()) {
            executed = manager.runVM(vmId, options.cycles);
        } else {
            if (options.keyAfterCycles >= options.cycles) {
                std::cerr << "[IA64-MATRIX] --key-after-cycles must be less than --cycles when keys are scripted\n";
                return 2;
            }

            const uint64_t warmupExecuted = manager.runVM(vmId, options.keyAfterCycles);
            executed += warmupExecuted;
            reportRuntime("before-input");
            if (warmupExecuted != options.keyAfterCycles) {
                std::cerr << "[IA64-MATRIX] VM stopped before scripted input boundary"
                          << " requested=" << options.keyAfterCycles
                          << " executed=" << warmupExecuted << std::endl;
                return 1;
            }

            ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
            ia64::CPUContext* context = vm == nullptr ? nullptr : vm->getCPUContext(0);
            ia64::IA64ISAPlugin* plugin = context == nullptr
                ? nullptr
                : dynamic_cast<ia64::IA64ISAPlugin*>(context->isaPlugin.get());
            if (plugin == nullptr) {
                std::cerr << "[IA64-MATRIX] IA-64 input queue is unavailable\n";
                return 1;
            }
            for (const auto& key : options.keys) {
                plugin->enqueueEfiInputKey(key.scanCode, key.unicodeChar);
                std::cout << "[IA64-MATRIX] queued key=" << key.name
                          << " scan=0x" << std::hex << key.scanCode
                          << " unicode=0x" << key.unicodeChar << std::dec << std::endl;
            }

            executed += manager.runVM(vmId, options.cycles - options.keyAfterCycles);
        }
        reportRuntime("final");
        const ia64::VMMetadata metadata = manager.getVMMetadata(vmId);
        const ia64::VMResourceUsage usage = manager.getVMResourceUsage(vmId);

        const ia64::VirtualMachine* vm = manager.getVMDirect(vmId);
        const ia64::CPUContext* context = vm == nullptr ? nullptr : vm->getCPUContext(0);
        const ia64::IA64ISAPlugin* plugin = context == nullptr
            ? nullptr
            : dynamic_cast<const ia64::IA64ISAPlugin*>(context->isaPlugin.get());
        if (plugin != nullptr) {
            const auto summary = plugin->getEfiTraceSummary();
            std::cerr << "[IA64-MATRIX] EFI-summary"
                      << " SimpleTextOut=" << summary.textOutputCalls
                      << " OpenVolume=" << summary.openVolumeCalls
                      << " File.Open=" << summary.fileOpenCalls
                      << " File.Read=" << summary.fileReadCalls
                      << " File.GetInfo=" << summary.fileGetInfoCalls
                      << " File.Close=" << summary.fileCloseCalls
                      << " File.GetPosition=" << summary.fileGetPositionCalls
                      << " File.SetPosition=" << summary.fileSetPositionCalls
                      << " LoadImage=" << summary.loadImageCalls
                      << " StartImage=" << summary.startImageCalls
                      << " ExitBootServices=" << summary.exitBootServicesCalls
                      << " ReadKeyStroke=" << summary.readKeyStrokeCalls
                      << " (NOT_READY=" << summary.readKeyStrokeNotReadyCalls
                      << " SUCCESS=" << summary.readKeyStrokeSuccessCalls << ')'
                      << " WaitForEvent=" << summary.waitForEventCalls
                      << " CheckEvent=" << summary.checkEventCalls
                      << " totalFileBytesRead=0x" << std::hex << summary.totalFileBytesRead
                      << std::dec << std::endl;
            for (size_t i = 0; i < summary.openFilePaths.size(); ++i) {
                std::cerr << "[IA64-MATRIX] open-file path=\"" << summary.openFilePaths[i]
                          << "\" position=0x" << std::hex << summary.openFilePositions[i]
                          << " size=0x" << summary.openFileSizes[i] << std::dec << std::endl;
            }
        }

        std::cout << "[IA64-MATRIX] vmId=" << vmId
                  << " cyclesExecuted=" << executed
                  << " usageCycles=" << usage.cyclesExecuted
                  << " state=" << ia64::vmStateToString(metadata.currentState)
                  << " error=\"" << metadata.lastError << "\""
                  << std::endl;
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[IA64-MATRIX] exception: " << exception.what() << std::endl;
        return 1;
    }
}

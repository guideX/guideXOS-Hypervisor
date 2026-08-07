#include "VMManager.h"
#include "logger.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

struct Options {
    std::string isoPath;
    uint64_t cycles = 2'000'000;
    uint64_t memoryMiB = 512;
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
        } else if (argument == "--help" || argument == "-h") {
            return false;
        } else {
            return false;
        }
    }
    return !options.isoPath.empty() && options.cycles > 0 && options.memoryMiB >= 1;
}

void printUsage() {
    std::cout << "Usage: ia64_iso_matrix <iso-path> [--cycles N] [--memory-mib N]\n"
              << "Defaults: --cycles 2000000 --memory-mib 512\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parseOptions(argc, argv, options)) {
        printUsage();
        return argc >= 2 ? 2 : 0;
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

        const uint64_t executed = manager.runVM(vmId, options.cycles);
        const ia64::VMMetadata metadata = manager.getVMMetadata(vmId);
        const ia64::VMResourceUsage usage = manager.getVMResourceUsage(vmId);

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

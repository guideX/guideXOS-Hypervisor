#include <iostream>
#include <vector>
#include <cstdint>
#include <string>
#include <iomanip>
#include "VirtualMachine.h"
#include "logger.h"

using namespace ia64;

// Parse command line arguments
struct DebugConfig {
    bool verbose;
    bool trace;
    int maxCycles;
    
    DebugConfig() : verbose(false), trace(false), maxCycles(100) {}
};

DebugConfig parseArgs(int argc, char* argv[]) {
    DebugConfig config;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--debug" || arg == "-d") {
            config.verbose = true;
        } else if (arg == "--trace" || arg == "-t") {
            config.trace = true;
        } else if (arg == "--cycles" || arg == "-c") {
            if (i + 1 < argc) {
                config.maxCycles = std::atoi(argv[++i]);
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "IA-64 Emulator Debug Harness\n";
            std::cout << "============================\n\n";
            std::cout << "Usage: debug_harness [options]\n\n";
            std::cout << "Options:\n";
            std::cout << "  --debug, -d         Enable debug logging (instruction trace)\n";
            std::cout << "  --trace, -t         Enable trace logging (register changes, memory access)\n";
            std::cout << "  --cycles N, -c N    Run for N cycles (default: 100)\n";
            std::cout << "  --help, -h          Show this help message\n\n";
            std::exit(0);
        }
    }
    
    return config;
}

// Create a hardcoded test program
std::vector<uint8_t> createTestProgram() {
    std::vector<uint8_t> program;
    
    // IA-64 bundle format: 128 bits (16 bytes)
    // [template:5][slot2:41][slot1:41][slot0:41]
    // Little-endian byte ordering
    
    // For this demo, we'll create several NOP bundles
    // Template 0x00 = MII (Memory, Integer, Integer)
    // NOP instruction encoding varies by slot type
    
    // Bundle 0: Three NOPs
    program.insert(program.end(), {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });
    
    // Bundle 1: Three NOPs
    program.insert(program.end(), {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });
    
    // Bundle 2: Three NOPs
    program.insert(program.end(), {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });
    
    // Bundle 3: Three NOPs
    program.insert(program.end(), {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    });
    
    return program;
}

void printHeader() {
    std::cout << "======================================\n";
    std::cout << "  IA-64 Emulator Debug Harness\n";
    std::cout << "  Windows Edition\n";
    std::cout << "======================================\n\n";
}

void printProgramInfo(const std::vector<uint8_t>& program, uint64_t loadAddr) {
    std::cout << "Program Information:\n";
    std::cout << "-------------------\n";
    std::cout << "  Size: " << program.size() << " bytes\n";
    std::cout << "  Bundles: " << (program.size() / 16) << "\n";
    std::cout << "  Load address: 0x" << std::hex << std::setw(16) 
              << std::setfill('0') << loadAddr << std::dec << "\n\n";
}

void printInitialState(const VirtualMachine& vm) {
    std::cout << "Initial VM State:\n";
    std::cout << "-----------------\n";
    std::cout << "  IP: 0x" << std::hex << std::setw(16) << std::setfill('0') 
              << vm.getIP() << std::dec << "\n";
    std::cout << "  GR0: 0x" << std::hex << std::setw(16) << std::setfill('0') 
              << vm.readGR(0) << std::dec << " (should always be 0)\n\n";
}

void printExecutionSummary(int cyclesExecuted, uint64_t finalIP) {
    std::cout << "\n======================================\n";
    std::cout << "Execution Summary:\n";
    std::cout << "======================================\n";
    std::cout << "  Cycles executed: " << cyclesExecuted << "\n";
    std::cout << "  Final IP: 0x" << std::hex << std::setw(16) 
              << std::setfill('0') << finalIP << std::dec << "\n\n";
}

void printFinalState(const VirtualMachine& vm) {
    std::cout << "Final VM State:\n";
    std::cout << "================\n";
    vm.dump();
}

int main(int argc, char* argv[]) {
    try {
        printHeader();
        
        // Parse command line arguments
        DebugConfig config = parseArgs(argc, argv);
        
        // Configure logging
        Logger& logger = Logger::getInstance();
        if (config.trace) {
            logger.setLogLevel(LogLevel::TRACE);
            LOG_INFO("Log level set to TRACE");
        } else if (config.verbose) {
            logger.setLogLevel(LogLevel::DEBUG);
            LOG_INFO("Log level set to DEBUG");
        } else {
            logger.setLogLevel(LogLevel::INFO);
        }
        
        // Initialize Virtual Machine (16MB)
        LOG_INFO("Initializing Virtual Machine (16MB)...");
        VirtualMachine vm(16 * 1024 * 1024);
        
        // Initialize VM
        if (!vm.init()) {
            LOG_ERROR("Failed to initialize VM");
            return 1;
        }
        
        // Create test program
        LOG_INFO("Creating test program...");
        std::vector<uint8_t> program = createTestProgram();
        
        // Load program into VM memory
        uint64_t loadAddress = 0x100000;  // 1MB offset
        LOG_INFO("Loading program into VM memory...");
        if (!vm.loadProgram(program.data(), program.size(), loadAddress)) {
            LOG_ERROR("Failed to load program");
            return 1;
        }
        
        printProgramInfo(program, loadAddress);
        
        // Set entry point
        vm.setEntryPoint(loadAddress);
        
        printInitialState(vm);
        
        // Execute
        std::cout << "======================================\n";
        std::cout << "Beginning Execution\n";
        std::cout << "======================================\n\n";
        
        LOG_INFO("Starting execution loop...");
        
        int cyclesExecuted = 0;
        bool shouldContinue = true;
        
        while (cyclesExecuted < config.maxCycles && shouldContinue) {
            // Log cycle start
            if (logger.getLogLevel() >= LogLevel::DEBUG) {
                std::cout << "\n--- Cycle " << (cyclesExecuted + 1) << " ---\n";
            }
            
            uint64_t ipBefore = vm.getIP();
            
            // Execute one step
            try {
                shouldContinue = vm.step();
                cyclesExecuted++;
                
                // Log IP change
                uint64_t ipAfter = vm.getIP();
                if (logger.getLogLevel() >= LogLevel::DEBUG) {
                    std::stringstream ss;
                    ss << "IP: 0x" << std::hex << std::setw(16) << std::setfill('0') 
                       << ipBefore << " -> 0x" << std::setw(16) << std::setfill('0') 
                       << ipAfter << std::dec;
                    LOG_DEBUG(ss.str());
                }
                
            } catch (const std::exception& e) {
                LOG_ERROR(std::string("Execution error: ") + e.what());
                break;
            }
        }
        
        if (cyclesExecuted >= config.maxCycles) {
            LOG_INFO("Maximum cycles reached");
        }
        if (!shouldContinue) {
            LOG_INFO("VM halted");
        }
        
        printExecutionSummary(cyclesExecuted, vm.getIP());
        printFinalState(vm);
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL ERROR] " << e.what() << "\n";
        return 1;
    }
}

#include <iostream>
#include <vector>
#include <cstdint>
#include <string>
#include <iomanip>
#include "cpu.h"
#include "cpu_state.h"
#include "decoder.h"
#include "memory.h"
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

void printInitialState(const CPU& cpu) {
    std::cout << "Initial CPU State:\n";
    std::cout << "-----------------\n";
    std::cout << "  IP: 0x" << std::hex << std::setw(16) << std::setfill('0') 
              << cpu.getIP() << std::dec << "\n";
    std::cout << "  GR0: 0x" << std::hex << std::setw(16) << std::setfill('0') 
              << cpu.readGR(0) << std::dec << " (should always be 0)\n";
    std::cout << "  PR0: " << (cpu.readPR(0) ? "true" : "false") 
              << " (should always be true)\n\n";
}

void printExecutionSummary(int cyclesExecuted, uint64_t finalIP) {
    std::cout << "\n======================================\n";
    std::cout << "Execution Summary:\n";
    std::cout << "======================================\n";
    std::cout << "  Cycles executed: " << cyclesExecuted << "\n";
    std::cout << "  Final IP: 0x" << std::hex << std::setw(16) 
              << std::setfill('0') << finalIP << std::dec << "\n\n";
}

void printFinalState(const CPU& cpu) {
    std::cout << "Final CPU State:\n";
    std::cout << "================\n";
    cpu.dump();
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
        
        // Initialize memory system (16MB)
        LOG_INFO("Initializing memory system (16MB)...");
        Memory memory(16 * 1024 * 1024);
        
        // Initialize decoder
        LOG_INFO("Initializing instruction decoder...");
        InstructionDecoder decoder;
        
        // Initialize CPU
        LOG_INFO("Initializing CPU...");
        CPU cpu(memory, decoder);
        
        // Create test program
        LOG_INFO("Creating test program...");
        std::vector<uint8_t> program = createTestProgram();
        
        // Load program into memory
        uint64_t loadAddress = 0x100000;  // 1MB offset
        LOG_INFO("Loading program into memory...");
        memory.Write(loadAddress, program.data(), program.size());
        
        printProgramInfo(program, loadAddress);
        
        // Set instruction pointer
        cpu.setIP(loadAddress);
        
        printInitialState(cpu);
        
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
            
            uint64_t ipBefore = cpu.getIP();
            
            // Execute one step
            try {
                shouldContinue = cpu.step();
                cyclesExecuted++;
                
                // Log IP change
                uint64_t ipAfter = cpu.getIP();
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
            LOG_INFO("CPU halted");
        }
        
        printExecutionSummary(cyclesExecuted, cpu.getIP());
        printFinalState(cpu);
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n[FATAL ERROR] " << e.what() << "\n";
        return 1;
    }
}

/**
 * IA-64 Hypervisor - Web UI Example
 * 
 * This example demonstrates how to start the web-based VM management UI
 * along with the REST API server.
 * 
 * Usage:
 *   webui_example.exe [api_port] [webui_port]
 * 
 * Default ports:
 *   API: 8080
 *   Web UI: 8090
 * 
 * After starting, open your web browser to:
 *   http://localhost:8090
 */

#include "VMManager.h"
#include "RestAPIServer.h"
#include "WebUIServer.h"
#include "logger.h"
#include "VMConfiguration.h"
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

using namespace ia64;
using namespace ia64::api;

void printBanner() {
    std::cout << "============================================\n";
    std::cout << "  IA-64 Hypervisor - Web Management UI\n";
    std::cout << "============================================\n\n";
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [api_port] [webui_port]\n";
    std::cout << "\nDefault ports:\n";
    std::cout << "  API Server:  8080\n";
    std::cout << "  Web UI:      8090\n";
    std::cout << "\nExample:\n";
    std::cout << "  " << programName << " 8080 8090\n";
    std::cout << std::endl;
}

void printServerInfo(const std::string& apiUrl, const std::string& webUrl) {
    std::cout << "Servers started successfully!\n\n";
    std::cout << "REST API Server: " << apiUrl << "\n";
    std::cout << "Web UI Server:   " << webUrl << "\n\n";
    std::cout << "Open your web browser to:\n";
    std::cout << "  " << webUrl << "\n\n";
    std::cout << "API Documentation:\n";
    std::cout << "  " << apiUrl << "/api/v1/health\n";
    std::cout << "  " << apiUrl << "/api/v1/vms\n\n";
    std::cout << "Press Ctrl+C to stop the servers...\n";
    std::cout << "============================================\n" << std::endl;
}

void createSampleVMs(VMManager& vmManager) {
    std::cout << "Creating sample VMs...\n";
    
    // Create a minimal VM
    VMConfiguration minimalConfig = VMConfiguration::createMinimal("minimal-vm");
    minimalConfig.memory.memorySize = 128 * 1024 * 1024;  // 128 MB
    std::string vm1 = vmManager.createVM(minimalConfig);
    if (!vm1.empty()) {
        std::cout << "  Created VM: minimal-vm (ID: " << vm1.substr(0, 8) << "...)\n";
    }
    
    // Create a standard VM
    VMConfiguration standardConfig = VMConfiguration::createStandard("standard-vm", 512);
    std::string vm2 = vmManager.createVM(standardConfig);
    if (!vm2.empty()) {
        std::cout << "  Created VM: standard-vm (ID: " << vm2.substr(0, 8) << "...)\n";
    }
    
    // Create a server-like VM  
    VMConfiguration serverConfig = VMConfiguration::createStandard("server-vm", 2048);
    serverConfig.cpu.cpuCount = 4;
    std::string vm3 = vmManager.createVM(serverConfig);
    if (!vm3.empty()) {
        std::cout << "  Created VM: server-vm (ID: " << vm3.substr(0, 8) << "...)\n";
    }
    
    std::cout << "\nSample VMs created. You can manage them from the web UI.\n\n";
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    uint16_t apiPort = 8080;
    uint16_t webPort = 8090;
    bool createSamples = true;
    
    if (argc > 1) {
        if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            printBanner();
            printUsage(argv[0]);
            return 0;
        }
        
        try {
            apiPort = static_cast<uint16_t>(std::stoi(argv[1]));
        } catch (...) {
            std::cerr << "Error: Invalid API port number\n";
            return 1;
        }
    }
    
    if (argc > 2) {
        try {
            webPort = static_cast<uint16_t>(std::stoi(argv[2]));
        } catch (...) {
            std::cerr << "Error: Invalid Web UI port number\n";
            return 1;
        }
    }
    
    if (argc > 3) {
        if (std::string(argv[3]) == "--no-samples") {
            createSamples = false;
        }
    }
    
    printBanner();
    
    // Create VM Manager
    VMManager vmManager;
    
    // Create sample VMs if requested
    if (createSamples) {
        createSampleVMs(vmManager);
    }
    
    // Create API Server
    RestAPIServer apiServer(vmManager, apiPort);
    
    // Create Web UI Server
    WebUIServer webServer(webPort, "./web");
    webServer.setCORSEnabled(true);
    webServer.setAPIUrl("http://localhost:" + std::to_string(apiPort));
    
    // Start servers
    std::cout << "Starting servers...\n";
    
    if (!apiServer.start()) {
        std::cerr << "Error: Failed to start API server on port " << apiPort << "\n";
        std::cerr << "Make sure the port is not already in use.\n";
        return 1;
    }
    
    if (!webServer.start()) {
        std::cerr << "Error: Failed to start Web UI server on port " << webPort << "\n";
        std::cerr << "Make sure the port is not already in use.\n";
        apiServer.stop();
        return 1;
    }
    
    // Print server information
    printServerInfo(apiServer.getEndpoint(), webServer.getEndpoint());
    
    // Keep running until interrupted
    try {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // Check if servers are still running
            if (!apiServer.isRunning() || !webServer.isRunning()) {
                std::cerr << "\nError: One or more servers stopped unexpectedly\n";
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "\nException: " << e.what() << "\n";
    }
    
    // Cleanup
    std::cout << "\nStopping servers...\n";
    webServer.stop();
    apiServer.stop();
    
    std::cout << "Servers stopped. Goodbye!\n";
    
    return 0;
}

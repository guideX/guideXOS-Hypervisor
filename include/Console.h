#pragma once

#include "IODevice.h"
#include "ConsoleOutputBuffer.h"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iosfwd>
#include <string>
#include <memory>

namespace ia64 {

class VirtualConsole : public IMemoryMappedDevice {
public:
    static constexpr uint64_t kDefaultBaseAddress = 0xFFFF0000ULL;
    static constexpr size_t kRegisterSize = 8;
    static constexpr uint64_t kDataRegisterOffset = 0;
    static constexpr uint64_t kStatusRegisterOffset = 4;
    static constexpr uint64_t kFlushRegisterOffset = 5;
    static constexpr size_t kDefaultScrollbackLines = 10000;

    explicit VirtualConsole(uint64_t baseAddress = kDefaultBaseAddress, 
                           std::ostream& output = std::cout,
                           size_t scrollbackLines = kDefaultScrollbackLines);
    ~VirtualConsole() override;

    uint64_t GetBaseAddress() const override { return baseAddress_; }
    size_t GetSize() const override { return kRegisterSize; }
    bool Read(uint64_t address, uint8_t* data, size_t size) const override;
    bool Write(uint64_t address, const uint8_t* data, size_t size) override;

    void WriteChar(char value);
    void Flush();
    const std::string& GetBuffer() const { return buffer_; }
    
    // Console output access methods
    std::vector<std::string> getAllOutputLines() const;
    std::vector<std::string> getOutputLines(size_t startLine, size_t count = 0) const;
    std::string getRecentOutput(size_t maxBytes) const;
    std::vector<std::string> getOutputSince(size_t lineNumber) const;
    size_t getOutputLineCount() const;
    uint64_t getTotalBytesWritten() const;
    void clearOutput();

private:
    uint64_t baseAddress_;
    std::ostream* output_;
    std::string buffer_;
    std::unique_ptr<ConsoleOutputBuffer> outputBuffer_;
};

} // namespace ia64

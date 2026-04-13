#include "Console.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace ia64 {

VirtualConsole::VirtualConsole(uint64_t baseAddress, std::ostream& output, size_t scrollbackLines)
    : baseAddress_(baseAddress),
      output_(&output),
      outputBuffer_(std::make_unique<ConsoleOutputBuffer>(scrollbackLines)) {

}

VirtualConsole::~VirtualConsole() {
    Flush();
}

bool VirtualConsole::Read(uint64_t address, uint8_t* data, size_t size) const {
    if (data == nullptr || !HandlesRange(address, size)) {
        return false;
    }

    std::memset(data, 0, size);

    for (size_t index = 0; index < size; ++index) {
        const uint64_t offset = (address - baseAddress_) + index;
        if (offset == kStatusRegisterOffset) {
            data[index] = static_cast<uint8_t>(buffer_.size() & 0xFFU);
        }
    }

    return true;
}

bool VirtualConsole::Write(uint64_t address, const uint8_t* data, size_t size) {
    if (data == nullptr || !HandlesRange(address, size)) {
        return false;
    }

    for (size_t index = 0; index < size; ++index) {
        const uint64_t offset = (address - baseAddress_) + index;
        if (offset == kDataRegisterOffset) {
            WriteChar(static_cast<char>(data[index]));
        } else if (offset == kFlushRegisterOffset && data[index] != 0) {
            Flush();
        }
    }

    return true;
}

void VirtualConsole::WriteChar(char value) {
    buffer_.push_back(value);
    if (outputBuffer_) {
        outputBuffer_->appendChar(value);
    }
    if (value == '\n') {
        Flush();
    }
}

void VirtualConsole::Flush() {
    if (output_ == nullptr || buffer_.empty()) {
        return;
    }

    (*output_) << buffer_;
    output_->flush();
    buffer_.clear();
}

std::vector<std::string> VirtualConsole::getAllOutputLines() const {
    if (!outputBuffer_) {
        return std::vector<std::string>();
    }
    return outputBuffer_->getAllLines();
}

std::vector<std::string> VirtualConsole::getOutputLines(size_t startLine, size_t count) const {
    if (!outputBuffer_) {
        return std::vector<std::string>();
    }
    return outputBuffer_->getLines(startLine, count);
}

std::string VirtualConsole::getRecentOutput(size_t maxBytes) const {
    if (!outputBuffer_) {
        return std::string();
    }
    return outputBuffer_->getRecentOutput(maxBytes);
}

std::vector<std::string> VirtualConsole::getOutputSince(size_t lineNumber) const {
    if (!outputBuffer_) {
        return std::vector<std::string>();
    }
    return outputBuffer_->getLinesSince(lineNumber);
}

size_t VirtualConsole::getOutputLineCount() const {
    if (!outputBuffer_) {
        return 0;
    }
    return outputBuffer_->getLineCount();
}

uint64_t VirtualConsole::getTotalBytesWritten() const {
    if (!outputBuffer_) {
        return 0;
    }
    return outputBuffer_->getTotalBytesWritten();
}

void VirtualConsole::clearOutput() {
    if (outputBuffer_) {
        outputBuffer_->clear();
    }
}

void VirtualConsole::Reset() {
    buffer_.clear();
    if (outputBuffer_) {
        outputBuffer_->clear();
    }
}

} // namespace ia64

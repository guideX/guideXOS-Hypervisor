#include "Console.h"

#include <cstring>
#include <iostream>

namespace ia64 {

VirtualConsole::VirtualConsole(uint64_t baseAddress, std::ostream& output)
    : baseAddress_(baseAddress),
      output_(&output) {
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

} // namespace ia64

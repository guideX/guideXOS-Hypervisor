#include "memory.h"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cassert>

namespace ia64 {

Memory::Memory(size_t size)
    : data_(size, 0)
{
    if (size == 0) {
        throw std::invalid_argument("Memory size cannot be zero");
    }
}

void Memory::loadBuffer(uint64_t address, const uint8_t* buffer, size_t size) {
    if (size == 0) {
        return; // Nothing to load
    }
    
    if (buffer == nullptr) {
        throw std::invalid_argument("Buffer pointer cannot be null");
    }
    
    checkBounds(address, size);
    std::memcpy(&data_[address], buffer, size);
}

void Memory::loadBuffer(uint64_t address, const std::vector<uint8_t>& buffer) {
    loadBuffer(address, buffer.data(), buffer.size());
}

void Memory::Read(uint64_t address, uint8_t* dest, size_t size) const {
    if (size == 0) {
        return; // Nothing to read
    }
    
    if (dest == nullptr) {
        throw std::invalid_argument("Destination pointer cannot be null");
    }
    
    checkBounds(address, size);
    std::memcpy(dest, &data_[address], size);
}

void Memory::Write(uint64_t address, const uint8_t* src, size_t size) {
    if (size == 0) {
        return; // Nothing to write
    }
    
    if (src == nullptr) {
        throw std::invalid_argument("Source pointer cannot be null");
    }
    
    checkBounds(address, size);
    std::memcpy(&data_[address], src, size);
}

void Memory::Clear() {
    std::fill(data_.begin(), data_.end(), static_cast<uint8_t>(0));
}

void Memory::checkBounds(uint64_t address, size_t size) const {
    // Debug mode: use assertions for fast checks
    assert(address < data_.size() && "Memory address out of bounds");
    assert(address + size <= data_.size() && "Memory access exceeds bounds");
    
    // Release mode: throw exceptions for safety
    if (address >= data_.size()) {
        std::ostringstream oss;
        oss << "Memory address 0x" << std::hex << address 
            << " out of bounds (size: 0x" << data_.size() << ")";
        throw std::out_of_range(oss.str());
    }
    
    if (address + size > data_.size()) {
        std::ostringstream oss;
        oss << "Memory access at 0x" << std::hex << address 
            << " with size 0x" << size 
            << " exceeds bounds (max: 0x" << data_.size() << ")";
        throw std::out_of_range(oss.str());
    }
}

} // namespace ia64

#include "decoder.h"

#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct PeSection {
    uint32_t virtualAddress = 0;
    uint32_t virtualSize = 0;
    uint32_t rawPointer = 0;
    uint32_t rawSize = 0;
};

uint16_t read16(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 2 > data.size()) {
        throw std::runtime_error("unexpected EOF reading u16");
    }
    return static_cast<uint16_t>(data[offset] | (static_cast<uint16_t>(data[offset + 1]) << 8));
}

uint32_t read32(const std::vector<uint8_t>& data, size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("unexpected EOF reading u32");
    }
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path);
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint64_t rvaToFileOffset(const std::vector<uint8_t>& pe, uint32_t rva, std::vector<PeSection>& sections) {
    if (pe.size() < 0x100) {
        throw std::runtime_error("file too small for PE");
    }
    if (read16(pe, 0) != 0x5A4D) {
        throw std::runtime_error("missing MZ header");
    }
    const uint32_t peOffset = read32(pe, 0x3C);
    if (peOffset + 0x18 > pe.size()) {
        throw std::runtime_error("invalid PE header offset");
    }
    if (read32(pe, peOffset) != 0x00004550) {
        throw std::runtime_error("missing PE signature");
    }
    const uint16_t numberOfSections = read16(pe, peOffset + 6);
    const uint16_t optionalHeaderSize = read16(pe, peOffset + 20);
    const size_t sectionTable = peOffset + 24 + optionalHeaderSize;
    if (sectionTable + static_cast<size_t>(numberOfSections) * 40 > pe.size()) {
        throw std::runtime_error("section table truncated");
    }

    sections.clear();
    sections.reserve(numberOfSections);
    for (uint16_t i = 0; i < numberOfSections; ++i) {
        const size_t off = sectionTable + static_cast<size_t>(i) * 40;
        PeSection sec{};
        sec.virtualSize = read32(pe, off + 8);
        sec.virtualAddress = read32(pe, off + 12);
        sec.rawSize = read32(pe, off + 16);
        sec.rawPointer = read32(pe, off + 20);
        sections.push_back(sec);
    }

    for (const auto& sec : sections) {
        const uint32_t span = sec.virtualSize != 0 ? sec.virtualSize : sec.rawSize;
        if (rva >= sec.virtualAddress && rva < sec.virtualAddress + span) {
            return static_cast<uint64_t>(sec.rawPointer) + (rva - sec.virtualAddress);
        }
    }
    throw std::runtime_error("RVA not found in any section");
}

std::string hexBytes(const uint8_t* data, size_t size) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        if (i != 0) {
            oss << ' ';
        }
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::cerr << "usage: codex_ia64_dump <bootia64.efi> [rva_start] [rva_end]\n";
            return 1;
        }

        const std::string path = argv[1];
        const uint32_t rvaStart = argc >= 3 ? static_cast<uint32_t>(std::stoul(argv[2], nullptr, 0)) : 0x36E70U;
        const uint32_t rvaEnd = argc >= 4 ? static_cast<uint32_t>(std::stoul(argv[3], nullptr, 0)) : 0x36F10U;

        const std::vector<uint8_t> pe = readFile(path);
        std::vector<PeSection> sections;
        std::cout << "file=" << path << " size=0x" << std::hex << pe.size() << std::dec << "\n";
        const uint64_t fileOffset = rvaToFileOffset(pe, rvaStart, sections);
        std::cout << "rvaStart=0x" << std::hex << rvaStart << " fileOffset=0x" << fileOffset << std::dec << "\n";

        ia64::InstructionDecoder decoder;
        for (uint32_t rva = rvaStart; rva <= rvaEnd; rva += 16) {
            const uint64_t offset = rvaToFileOffset(pe, rva, sections);
            if (offset + 16 > pe.size()) {
                throw std::runtime_error("bundle extends past file end");
            }
            uint8_t bundle[16];
            std::copy_n(pe.begin() + static_cast<std::ptrdiff_t>(offset), 16, bundle);
            auto decoded = decoder.DecodeBundleAt(bundle, rva);
            std::cout << "bundle rva=0x" << std::hex << rva
                      << " offset=0x" << offset
                      << " template=0x" << static_cast<unsigned>(static_cast<uint8_t>(decoded.templateType))
                      << " stop=" << decoded.hasStop
                      << " bytes=" << hexBytes(bundle, 16)
                      << std::dec << "\n";
            for (size_t slot = 0; slot < decoded.instructions.size(); ++slot) {
                const auto& instr = decoded.instructions[slot];
                std::cout << "  slot" << slot
                          << " type=" << static_cast<int>(instr.GetType())
                          << " pred=p" << static_cast<unsigned>(instr.GetPredicate())
                          << " disasm=" << instr.GetDisassembly()
                          << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}

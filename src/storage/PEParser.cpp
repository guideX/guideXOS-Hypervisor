#include "PEParser.h"
#include "PEParser.h"
#include "logger.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <iomanip>

using ia64::LOG_ERROR;
using ia64::LOG_INFO;
using ia64::LOG_WARN;

namespace guideXOS {

PEParser::PEParser()
    : imageData_(nullptr)
    , imageSize_(0)
    , valid_(false) {
}

PEParser::~PEParser() {
}

bool PEParser::parse(const uint8_t* data, size_t size) {
    if (!data || size < sizeof(DOSHeader)) {
        LOG_ERROR("Invalid PE image data");
        return false;
    }
    
    imageData_ = data;
    imageSize_ = size;
    
    if (!parseDOSHeader()) {
        LOG_ERROR("Failed to parse DOS header");
        return false;
    }
    
    if (!parsePEHeader()) {
        LOG_ERROR("Failed to parse PE header");
        return false;
    }
    
    if (!parseCOFFHeader()) {
        LOG_ERROR("Failed to parse COFF header");
        return false;
    }
    
    if (!parseOptionalHeader()) {
        LOG_ERROR("Failed to parse optional header");
        return false;
    }
    
    if (!parseSections()) {
        LOG_ERROR("Failed to parse sections");
        return false;
    }
    
    valid_ = true;
    LOG_INFO("PE/COFF image parsed successfully");
    
    // Use std::ostringstream for proper hex formatting
    std::ostringstream oss;
    oss << "  Machine: 0x" << std::hex << imageInfo_.machine << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Subsystem: " + std::to_string(imageInfo_.subsystem));
    
    oss.str("");
    oss << "  Image base: 0x" << std::hex << imageInfo_.imageBase << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Entry point: 0x" << std::hex << imageInfo_.entryPoint << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Sections: " + std::to_string(imageInfo_.sections.size()));
    
    return true;
}

bool PEParser::parseDOSHeader() {
    if (imageSize_ < sizeof(DOSHeader)) {
        return false;
    }
    
    std::memcpy(&dosHeader_, imageData_, sizeof(DOSHeader));
    
    // Check MZ signature
    if (dosHeader_.e_magic != 0x5A4D) { // "MZ"
        LOG_ERROR("Invalid DOS header magic (expected MZ)");
        return false;
    }
    
    // Validate PE header offset
    if (dosHeader_.e_lfanew == 0 || dosHeader_.e_lfanew >= imageSize_) {
        LOG_ERROR("Invalid PE header offset");
        return false;
    }
    
    return true;
}

bool PEParser::parsePEHeader() {
    uint32_t peOffset = dosHeader_.e_lfanew;
    
    if (peOffset + sizeof(PEHeader) > imageSize_) {
        return false;
    }
    
    std::memcpy(&peHeader_, imageData_ + peOffset, sizeof(PEHeader));
    
    // Check PE signature
    if (peHeader_.signature != 0x00004550) { // "PE\0\0"
        LOG_ERROR("Invalid PE signature");
        return false;
    }
    
    return true;
}

bool PEParser::parseCOFFHeader() {
    uint32_t coffOffset = dosHeader_.e_lfanew + sizeof(PEHeader);
    
    if (coffOffset + sizeof(COFFHeader) > imageSize_) {
        return false;
    }
    
    std::memcpy(&coffHeader_, imageData_ + coffOffset, sizeof(COFFHeader));
    
    imageInfo_.machine = coffHeader_.machine;
    
    LOG_INFO("COFF Header:");
    LOG_INFO("  Machine: 0x" + std::to_string(coffHeader_.machine));
    LOG_INFO("  Number of sections: " + std::to_string(coffHeader_.numberOfSections));
    LOG_INFO("  Optional header size: " + std::to_string(coffHeader_.sizeOfOptionalHeader));
    
    return true;
}

bool PEParser::parseOptionalHeader() {
    uint32_t optOffset = dosHeader_.e_lfanew + sizeof(PEHeader) + sizeof(COFFHeader);
    
    if (coffHeader_.sizeOfOptionalHeader == 0) {
        LOG_WARN("No optional header present");
        return true;
    }
    
    if (optOffset + sizeof(PEOptionalHeader64) > imageSize_) {
        return false;
    }
    
    std::memcpy(&optionalHeader_, imageData_ + optOffset, sizeof(PEOptionalHeader64));
    
    // Validate magic (0x10B for PE32, 0x20B for PE32+)
    if (optionalHeader_.magic != 0x20B && optionalHeader_.magic != 0x10B) {
        LOG_ERROR("Invalid optional header magic: 0x" + std::to_string(optionalHeader_.magic));
        return false;
    }
    
    imageInfo_.imageBase = optionalHeader_.imageBase;
    imageInfo_.entryPoint = optionalHeader_.imageBase + optionalHeader_.addressOfEntryPoint;
    imageInfo_.subsystem = optionalHeader_.subsystem;
    imageInfo_.sectionAlignment = optionalHeader_.sectionAlignment;
    imageInfo_.fileAlignment = optionalHeader_.fileAlignment;
    imageInfo_.sizeOfImage = optionalHeader_.sizeOfImage;
    imageInfo_.sizeOfHeaders = optionalHeader_.sizeOfHeaders;
    
    LOG_INFO("Optional Header:");
    
    std::ostringstream oss;
    oss << "  Magic: 0x" << std::hex << optionalHeader_.magic << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Entry point RVA: 0x" << std::hex << optionalHeader_.addressOfEntryPoint << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Image base: 0x" << std::hex << optionalHeader_.imageBase << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Section alignment: " + std::to_string(optionalHeader_.sectionAlignment));
    LOG_INFO("  Subsystem: " + std::to_string(optionalHeader_.subsystem));
    
    return true;
}

bool PEParser::parseSections() {
    uint32_t sectionOffset = dosHeader_.e_lfanew + sizeof(PEHeader) + 
                             sizeof(COFFHeader) + coffHeader_.sizeOfOptionalHeader;
    
    imageInfo_.sections.clear();
    
    for (uint16_t i = 0; i < coffHeader_.numberOfSections; i++) {
        if (sectionOffset + sizeof(PESectionHeader) > imageSize_) {
            LOG_ERROR("Section header extends beyond image size");
            return false;
        }
        
        PESectionHeader sectionHeader;
        std::memcpy(&sectionHeader, imageData_ + sectionOffset, sizeof(PESectionHeader));
        
        PESectionInfo section;
        section.name = std::string(sectionHeader.name, 
                                   std::find(sectionHeader.name, sectionHeader.name + 8, '\0'));
        section.virtualAddress = sectionHeader.virtualAddress;
        section.virtualSize = sectionHeader.virtualSize;
        section.rawDataOffset = sectionHeader.pointerToRawData;
        section.rawDataSize = sectionHeader.sizeOfRawData;
        section.characteristics = sectionHeader.characteristics;
        
        imageInfo_.sections.push_back(section);
        
        LOG_INFO("Section " + std::to_string(i) + ": " + section.name);
        LOG_INFO("  Virtual address: 0x" + std::to_string(section.virtualAddress));
        LOG_INFO("  Virtual size: " + std::to_string(section.virtualSize));
        LOG_INFO("  Raw data offset: 0x" + std::to_string(section.rawDataOffset));
        LOG_INFO("  Raw data size: " + std::to_string(section.rawDataSize));
        
        sectionOffset += sizeof(PESectionHeader);
    }
    
    return true;
}

bool PEParser::loadImage(std::vector<uint8_t>& imageBuffer, uint64_t& loadAddress, 
                        uint64_t& entryPoint) {
    if (!valid_) {
        LOG_ERROR("PE image not parsed or invalid");
        return false;
    }
    
    // Allocate buffer for entire image
    imageBuffer.clear();
    imageBuffer.resize(imageInfo_.sizeOfImage, 0);
    
    // Copy headers
    if (imageInfo_.sizeOfHeaders > imageSize_) {
        LOG_ERROR("Headers size exceeds image size");
        return false;
    }
    
    std::memcpy(imageBuffer.data(), imageData_, imageInfo_.sizeOfHeaders);
    LOG_INFO("Copied " + std::to_string(imageInfo_.sizeOfHeaders) + " bytes of headers");
    
    // Copy each section
    for (const auto& section : imageInfo_.sections) {
        if (section.rawDataSize == 0) {
            LOG_INFO("Section " + section.name + " has no raw data (BSS section)");
            continue;
        }
        
        if (section.rawDataOffset + section.rawDataSize > imageSize_) {
            LOG_ERROR("Section " + section.name + " data extends beyond image size");
            return false;
        }
        
        if (section.virtualAddress + section.rawDataSize > imageInfo_.sizeOfImage) {
            LOG_ERROR("Section " + section.name + " extends beyond allocated image size");
            return false;
        }
        
        // Copy section data to its virtual address
        std::memcpy(imageBuffer.data() + section.virtualAddress,
                   imageData_ + section.rawDataOffset,
                   section.rawDataSize);
        
        LOG_INFO("Loaded section " + section.name + ": " + 
                std::to_string(section.rawDataSize) + " bytes at RVA 0x" + 
                std::to_string(section.virtualAddress));
    }
    
    loadAddress = imageInfo_.imageBase;
    entryPoint = imageInfo_.entryPoint;
    
    LOG_INFO("PE image loaded successfully");
    
    // Use std::ostringstream for proper hex formatting
    std::ostringstream oss;
    oss << "  Load address: 0x" << std::hex << loadAddress << std::dec;
    LOG_INFO(oss.str());
    
    oss.str("");
    oss << "  Entry point: 0x" << std::hex << entryPoint << std::dec;
    LOG_INFO(oss.str());
    
    LOG_INFO("  Image size: " + std::to_string(imageBuffer.size()) + " bytes");
    
    return true;
}

} // namespace guideXOS

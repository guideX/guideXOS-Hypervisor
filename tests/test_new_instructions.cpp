#include "decoder.h"
#include "cpu_state.h"
#include "memory.h"
#include "ISO9660Parser.h"
#include "FATParser.h"
#include "IStorageDevice.h"
#include <iostream>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <string>
#include <vector>
#include <stdexcept>

using namespace ia64;

// Test helper forward declarations
void assert_equal(const char* name, uint64_t expected, uint64_t actual);
void assert_true(const char* name, bool condition);
void assert_string(const char* name, const std::string& expected, const std::string& actual);

namespace {

uint64_t build_mov_to_pr_slot(uint8_t sourceRegister,
                              uint64_t mask17,
                              uint8_t qualifyingPredicate = 0) {
    const uint64_t encodedImm16 = (mask17 & 0x1FFFFULL) >> 1;
    return (3ULL << 33) |
           (static_cast<uint64_t>(sourceRegister) << 13) |
           ((encodedImm16 & 0x7FULL) << 6) |
           (((encodedImm16 >> 7) & 0xFFULL) << 24) |
           (((encodedImm16 >> 15) & 0x1ULL) << 36) |
           (qualifyingPredicate & 0x3FULL);
}

class MemoryStorageDevice : public IStorageDevice {
public:
    explicit MemoryStorageDevice(std::vector<uint8_t> data, uint32_t blockSize = 2048)
        : data_(std::move(data)), blockSize_(blockSize) {}

    StorageDeviceInfo getInfo() const override {
        StorageDeviceInfo info;
        info.deviceId = "memory-storage";
        info.type = StorageDeviceType::MEMORY_BACKED;
        info.sizeBytes = data_.size();
        info.blockSize = blockSize_;
        info.connected = true;
        return info;
    }

    std::string getDeviceId() const override { return "memory-storage"; }
    uint64_t getSize() const override { return data_.size(); }
    uint32_t getBlockSize() const override { return blockSize_; }
    bool isReadOnly() const override { return true; }
    bool isConnected() const override { return true; }

    int64_t readBlocks(uint64_t blockNumber, uint64_t blockCount, uint8_t* buffer) override {
        const uint64_t offset = blockNumber * blockSize_;
        const uint64_t size = blockCount * blockSize_;
        if (!buffer || offset + size > data_.size()) {
            return -1;
        }
        std::memcpy(buffer, data_.data() + offset, static_cast<size_t>(size));
        return static_cast<int64_t>(blockCount);
    }

    int64_t writeBlocks(uint64_t, uint64_t, const uint8_t*) override { return -1; }
    bool flush() override { return true; }
    bool connect() override { return true; }
    void disconnect() override {}

private:
    std::vector<uint8_t> data_;
    uint32_t blockSize_;
};

void write_le16(std::vector<uint8_t>& buffer, size_t offset, uint16_t value) {
    buffer[offset] = static_cast<uint8_t>(value & 0xff);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

void write_le32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value) {
    buffer[offset] = static_cast<uint8_t>(value & 0xff);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    buffer[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xff);
    buffer[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

std::vector<uint8_t> makeFatImageWithBootLoader() {
    std::vector<uint8_t> image(8 * 512, 0);
    auto* boot = reinterpret_cast<guideXOS::FATBootSector*>(image.data());
    boot->bytesPerSector = 512;
    boot->sectorsPerCluster = 1;
    boot->reservedSectors = 1;
    boot->numFATs = 1;
    boot->rootEntryCount = 16;
    boot->totalSectors16 = 8;
    boot->mediaType = 0xF8;
    boot->sectorsPerFAT = 1;
    boot->bootSignature = 0x29;
    std::memcpy(boot->fileSystemType, "FAT16   ", 8);

    auto* fat = image.data() + 512;
    fat[0] = 0xF8;
    fat[1] = 0xFF;
    fat[2] = 0xFF;
    fat[3] = 0xFF;

    auto* root = reinterpret_cast<guideXOS::FATDirectoryEntry*>(image.data() + 1024);
    std::memcpy(root[0].filename, "EFI     ", 8);
    std::memcpy(root[0].extension, "   ", 3);
    root[0].attributes = guideXOS::ATTR_DIRECTORY;
    root[0].firstClusterLow = 2;
    root[1].filename[0] = 0x00;

    auto* efiDir = image.data() + 1536;
    auto* efiEntry = reinterpret_cast<guideXOS::FATDirectoryEntry*>(efiDir);
    std::memcpy(efiEntry[0].filename, "BOOT     ", 8);
    std::memcpy(efiEntry[0].extension, "   ", 3);
    efiEntry[0].attributes = guideXOS::ATTR_DIRECTORY;
    efiEntry[0].firstClusterLow = 3;
    efiEntry[1].filename[0] = 0x00;

    auto* bootDir = image.data() + 2048;
    auto* bootEntry = reinterpret_cast<guideXOS::FATDirectoryEntry*>(bootDir);
    std::memcpy(bootEntry[0].filename, "BOOTIA64", 8);
    std::memcpy(bootEntry[0].extension, "EFI", 3);
    bootEntry[0].attributes = guideXOS::ATTR_ARCHIVE;
    bootEntry[0].firstClusterLow = 4;
    bootEntry[0].fileSize = 8;
    bootEntry[1].filename[0] = 0x00;

    auto* data = image.data() + 2560;
    std::memcpy(data, "BOOTIA64", 8);
    return image;
}

void write_lfn_ascii_entry(uint8_t* entry, const char* name) {
    std::memset(entry, 0xFF, 32);
    entry[0] = 0x41; // Last (and only) long-name fragment.
    entry[11] = guideXOS::ATTR_LONG_NAME;
    entry[12] = 0;
    entry[13] = 0;
    entry[26] = 0;
    entry[27] = 0;

    const size_t length = std::strlen(name);
    const size_t offsets[] = {
        1, 3, 5, 7, 9,
        14, 16, 18, 20, 22, 24,
        28, 30,
    };
    for (size_t index = 0; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        const uint16_t codeUnit = index < length
            ? static_cast<uint8_t>(name[index])
            : (index == length ? 0 : 0xFFFF);
        entry[offsets[index]] = static_cast<uint8_t>(codeUnit & 0xFF);
        entry[offsets[index] + 1] = static_cast<uint8_t>(codeUnit >> 8);
    }
}

std::vector<uint8_t> makeIsoWithDirectBootLoader() {
    std::vector<uint8_t> image(32 * 2048, 0);
    auto* pvd = image.data() + 16 * 2048;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    write_le16(image, 16 * 2048 + 128, 2048);
    write_le32(image, 16 * 2048 + 80, 32);
    auto* rootRecord = pvd + 156;
    rootRecord[0] = 34;
    write_le32(image, 16 * 2048 + 156 + 2, 20);
    write_le32(image, 16 * 2048 + 156 + 10, 2048);
    rootRecord[25] = 2;
    rootRecord[32] = 1;

    auto* rootDir = image.data() + 20 * 2048;
    rootDir[0] = 48;
    write_le32(image, 20 * 2048 + 2, 21);
    write_le32(image, 20 * 2048 + 10, 8);
    rootDir[25] = 0;
    rootDir[32] = 14;
    std::memcpy(rootDir + 33, "BOOTIA64.EFI;1", 14);
    rootDir[48] = 0;

    std::memcpy(image.data() + 21 * 2048, "BOOTIA64", 8);
    return image;
}

std::vector<uint8_t> makeElToritoImageWithFatBootLoader(uint8_t platformId = 0xEF) {
    std::vector<uint8_t> image(64 * 2048, 0);
    auto* pvd = image.data() + 16 * 2048;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    write_le16(image, 16 * 2048 + 128, 2048);
    write_le32(image, 16 * 2048 + 80, 64);
    auto* rootRecord = pvd + 156;
    rootRecord[0] = 34;
    write_le32(image, 16 * 2048 + 156 + 2, 32);
    write_le32(image, 16 * 2048 + 156 + 10, 2048);
    rootRecord[25] = 2;
    rootRecord[32] = 1;

    auto* bootRecord = image.data() + 17 * 2048;
    bootRecord[0] = 0;
    std::memcpy(bootRecord + 1, "CD001", 5);
    bootRecord[6] = 1;
    std::memcpy(bootRecord + 7, "EL TORITO SPECIFICATION", 23);
    write_le32(image, 17 * 2048 + 71, 18);

    auto* validation = image.data() + 18 * 2048;
    validation[0] = 1;
    validation[1] = platformId;
    validation[30] = 0x55;
    validation[31] = 0xAA;
    auto* entry = image.data() + 18 * 2048 + 32;
    entry[0] = 0x88;
    entry[1] = 0;
    write_le16(image, 18 * 2048 + 34, 0);
    write_le16(image, 18 * 2048 + 38, 2);
    write_le32(image, 18 * 2048 + 40, 19);

    auto fat = makeFatImageWithBootLoader();
    std::memcpy(image.data() + 19 * 2048, fat.data(), fat.size());
    return image;
}

void test_iso_boot_media_direct_path() {
    std::cout << "Testing ISO9660 direct boot-media lookup..." << std::endl;

    auto image = makeIsoWithDirectBootLoader();
    MemoryStorageDevice device(std::move(image), 2048);
    ISO9660Parser parser(&device);

    assert_true("ISO parse should succeed", parser.parse());
    std::vector<uint8_t> executable;
    assert_true("ISO bootloader should be found", parser.extractEFIExecutable(executable));
    assert_true("ISO bootloader should not be empty", !executable.empty());
    assert_true("Direct ISO path should be reported", parser.getLastBootMediaDiagnostics().find("BOOTIA64.EFI found at path") != std::string::npos);

    std::cout << "  ? ISO direct boot-media lookup passed" << std::endl;
}

void test_fat_boot_media_lookup() {
    std::cout << "Testing FAT boot-media lookup..." << std::endl;

    auto image = makeFatImageWithBootLoader();
    guideXOS::FATParser fat;
    assert_true("FAT parse should succeed", fat.parse(image.data(), image.size()));
    guideXOS::FATFileInfo info{};
    assert_true("FAT path should resolve", fat.findFile("/EFI/BOOT/BOOTIA64.EFI", info));
    assert_true("FAT path should be file", !info.isDirectory);
    assert_true("FAT file should have data", info.size == 8);
    std::vector<uint8_t> data;
    assert_true("FAT file should read", fat.readFile(info, data));
    assert_true("FAT data should match", data.size() == 8 && std::memcmp(data.data(), "BOOTIA64", 8) == 0);

    auto longNameImage = makeFatImageWithBootLoader();
    auto* longNameBootDir = longNameImage.data() + 2048;
    std::memmove(longNameBootDir + 64, longNameBootDir, sizeof(guideXOS::FATDirectoryEntry));
    std::memset(longNameBootDir + 32, 0, sizeof(guideXOS::FATDirectoryEntry));
    write_lfn_ascii_entry(longNameBootDir, "elilo.conf");
    auto* longNameEntry = reinterpret_cast<guideXOS::FATDirectoryEntry*>(longNameBootDir + 32);
    std::memcpy(longNameEntry->filename, "ELILO~1 ", 8);
    std::memcpy(longNameEntry->extension, "CON", 3);
    longNameEntry->attributes = guideXOS::ATTR_ARCHIVE;
    longNameEntry->firstClusterLow = 5;
    longNameEntry->fileSize = 4;
    longNameBootDir[96] = 0;
    std::memcpy(longNameImage.data() + 3072, "CONF", 4);

    guideXOS::FATParser longNameFat;
    assert_true("FAT long-name image should parse",
                longNameFat.parse(longNameImage.data(), longNameImage.size()));
    guideXOS::FATFileInfo longNameInfo{};
    assert_true("FAT long filename should resolve",
                longNameFat.findFile("/EFI/BOOT/elilo.conf", longNameInfo));
    assert_true("FAT long filename should preserve its name",
                longNameInfo.name == "elilo.conf");
    std::vector<uint8_t> longNameData;
    assert_true("FAT long filename should read",
                longNameFat.readFile(longNameInfo, longNameData));
    assert_true("FAT long filename data should match",
                longNameData.size() == 4 && std::memcmp(longNameData.data(), "CONF", 4) == 0);

    std::cout << "  ? FAT boot-media lookup passed" << std::endl;
}

void test_el_torito_fat_boot_media_lookup() {
    std::cout << "Testing El Torito EFI boot-image lookup..." << std::endl;

    auto image = makeElToritoImageWithFatBootLoader();
    MemoryStorageDevice device(std::move(image), 2048);
    ISO9660Parser parser(&device);

    assert_true("ISO parse should succeed", parser.parse());
    assert_true("Boot catalog should parse", parser.findBootCatalog());
    std::vector<uint8_t> executable;
    assert_true("Boot image should yield EFI loader", parser.extractEFIExecutable(executable));
    assert_true("Boot image should not be empty", !executable.empty());
    assert_true("Boot image diagnostic should mention BOOTIA64.EFI", parser.getLastBootMediaDiagnostics().find("BOOTIA64.EFI found at path") != std::string::npos);

    std::cout << "  ? El Torito EFI boot-image lookup passed" << std::endl;
}

void test_el_torito_x86_boot_image_lookup() {
    std::cout << "Testing El Torito x86 boot-image fallback lookup..." << std::endl;

    auto image = makeElToritoImageWithFatBootLoader(0x00);
    MemoryStorageDevice device(std::move(image), 2048);
    ISO9660Parser parser(&device);

    assert_true("ISO parse should succeed", parser.parse());
    assert_true("Boot catalog should parse", parser.findBootCatalog());
    std::vector<uint8_t> executable;
    assert_true("x86 El Torito boot image should yield EFI loader", parser.extractEFIExecutable(executable));
    assert_true("x86 El Torito boot image should not be empty", !executable.empty());
    assert_true("x86 boot image diagnostic should mention BOOTIA64.EFI",
                parser.getLastBootMediaDiagnostics().find("BOOTIA64.EFI found at path") != std::string::npos);

    std::cout << "  ? El Torito x86 boot-image fallback lookup passed" << std::endl;
}

} // namespace

// Test helper
void assert_equal(const char* name, uint64_t expected, uint64_t actual);
void assert_true(const char* name, bool condition);
void assert_string(const char* name, const std::string& expected, const std::string& actual);

void assert_equal(const char* name, uint64_t expected, uint64_t actual) {
    if (expected != actual) {
        std::cerr << "TEST FAILED: " << name << std::endl;
        std::cerr << "  Expected: 0x" << std::hex << expected << std::dec << std::endl;
        std::cerr << "  Actual:   0x" << std::hex << actual << std::dec << std::endl;
        exit(1);
    }
}

void assert_true(const char* name, bool condition) {
    if (!condition) {
        std::cerr << "TEST FAILED: " << name << " - condition is false" << std::endl;
        exit(1);
    }
}

void assert_string(const char* name, const std::string& expected, const std::string& actual) {
    if (expected != actual) {
        std::cerr << "TEST FAILED: " << name << std::endl;
        std::cerr << "  Expected: " << expected << std::endl;
        std::cerr << "  Actual:   " << actual << std::endl;
        exit(1);
    }
}

uint64_t build_tbit_z_slot(uint8_t qp, uint8_t p1, uint8_t p2, uint8_t r3, uint8_t pos) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(p1) & 0x3F) << 6) |
           ((static_cast<uint64_t>(pos) & 0x3F) << 14) |
           ((static_cast<uint64_t>(r3) & 0x7F) << 20) |
           ((static_cast<uint64_t>(p2) & 0x3F) << 27) |
           (5ULL << 37);
}

uint64_t build_tnat_z_slot(uint8_t qp, uint8_t p1, uint8_t p2, uint8_t r3) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(p1) & 0x3F) << 6) |
           (1ULL << 13) |
           ((static_cast<uint64_t>(r3) & 0x7F) << 20) |
           ((static_cast<uint64_t>(p2) & 0x3F) << 27) |
           (5ULL << 37);
}

uint64_t build_mov_from_ip_slot(uint8_t qp, uint8_t r1) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(r1) & 0x7F) << 6) |
           (0x30ULL << 27);
}

uint64_t build_mov_from_pr_slot(uint8_t qp, uint8_t r1) {
    return (static_cast<uint64_t>(qp) & 0x3F) |
           ((static_cast<uint64_t>(r1) & 0x7F) << 6) |
           (0x33ULL << 27);
}

uint64_t build_addp4_imm14_slot(uint8_t destination, int16_t immediate,
                                uint8_t base, uint8_t predicate = 0) {
    const uint16_t encoded = static_cast<uint16_t>(immediate) & 0x3FFFU;
    return (static_cast<uint64_t>(predicate & 0x3F)) |
           ((static_cast<uint64_t>(destination & 0x7F)) << 6) |
           ((static_cast<uint64_t>(encoded & 0x7F)) << 13) |
           ((static_cast<uint64_t>(base & 0x7F)) << 20) |
           ((static_cast<uint64_t>((encoded >> 7) & 0x3F)) << 27) |
           (static_cast<uint64_t>(0x3) << 34) |
           (static_cast<uint64_t>((encoded >> 13) & 0x1) << 36) |
           (static_cast<uint64_t>(0x8) << 37);
}

// Test CMP instructions
void test_compare_instructions() {
    std::cout << "Testing compare instructions..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Test CMP.EQ
    cpu.SetGR(1, 100);
    cpu.SetGR(2, 100);
    cpu.SetGR(3, 50);
    
    InstructionEx cmp_eq(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_eq.SetOperands4(1, 1, 2, 2);  // p1, p2 = r1, r2 (100 == 100)
    cmp_eq.Execute(cpu, memory);
    
    assert_true("CMP.EQ: p1 should be true", cpu.GetPR(1));
    assert_true("CMP.EQ: p2 should be false", !cpu.GetPR(2));
    
    // Test CMP.LT signed
    cpu.SetGR(4, static_cast<uint64_t>(-10));  // Negative number
    cpu.SetGR(5, 5);
    
    InstructionEx cmp_lt(InstructionType::CMP_LT, UnitType::I_UNIT);
    cmp_lt.SetOperands4(3, 4, 5, 4);  // p3, p4 = r4, r5 (-10 < 5)
    cmp_lt.Execute(cpu, memory);
    
    assert_true("CMP.LT: p3 should be true (signed)", cpu.GetPR(3));
    assert_true("CMP.LT: p4 should be false", !cpu.GetPR(4));
    
    // Test CMP.LTU unsigned
    InstructionEx cmp_ltu(InstructionType::CMP_LTU, UnitType::I_UNIT);
    cmp_ltu.SetOperands4(5, 4, 5, 6);  // p5, p6 = r4, r5 (unsigned)
    cmp_ltu.Execute(cpu, memory);
    
    assert_true("CMP.LTU: p5 should be false (unsigned)", !cpu.GetPR(5));
    assert_true("CMP.LTU: p6 should be true", cpu.GetPR(6));

    InstructionEx cmp_and(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_and.SetOperands4(20, 1, 3, 21);
    cmp_and.SetCompareCompleter(CompareCompleter::AND);
    cpu.SetPR(20, true);
    cpu.SetPR(21, true);
    cmp_and.Execute(cpu, memory);
    assert_true("CMP.EQ.AND should clear p20 when result is false", !cpu.GetPR(20));
    assert_true("CMP.EQ.AND should clear p21 when result is false", !cpu.GetPR(21));

    InstructionEx cmp_or(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_or.SetOperands4(22, 1, 2, 23);
    cmp_or.SetCompareCompleter(CompareCompleter::OR);
    cmp_or.Execute(cpu, memory);
    assert_true("CMP.EQ.OR should set p22 when result is true", cpu.GetPR(22));
    assert_true("CMP.EQ.OR should set p23 when result is true", cpu.GetPR(23));

    InstructionEx cmp_unc(InstructionType::CMP_EQ, UnitType::I_UNIT);
    cmp_unc.SetPredicate(31);
    cmp_unc.SetOperands4(24, 1, 2, 25);
    cmp_unc.SetCompareCompleter(CompareCompleter::UNC);
    cpu.SetPR(24, true);
    cpu.SetPR(25, true);
    cmp_unc.Execute(cpu, memory);
    assert_true("CMP.EQ.UNC should clear p24 when qp is false", !cpu.GetPR(24));
    assert_true("CMP.EQ.UNC should clear p25 when qp is false", !cpu.GetPR(25));
    
    std::cout << "  ? Compare instructions passed" << std::endl;
}

void test_compare_ne_decoder() {
    std::cout << "Testing compare-ne decoder mapping..." << std::endl;

    InstructionDecoder decoder;
    InstructionEx cmp_ne = decoder.DecodeSlot(0x1a801300180ULL, UnitType::I_UNIT, 0x36e70);

    assert_true("CMP.NE raw slot should decode as CMP_NE",
                cmp_ne.GetType() == InstructionType::CMP_NE);
    assert_equal("CMP.NE destination predicate", 6, cmp_ne.GetDst());
    assert_equal("CMP.NE lhs register", 0, cmp_ne.GetSrc1());
    assert_equal("CMP.NE rhs register", 19, cmp_ne.GetSrc2());
    assert_equal("CMP.NE complement predicate", 0, cmp_ne.GetSrc3());

    CPUState cpu;
    Memory memory(1024 * 1024);

    cpu.SetGR(19, 3);
    cmp_ne.Execute(cpu, memory);
    assert_true("CMP.NE should set p6 while r19 is non-zero", cpu.GetPR(6));

    cpu.SetGR(19, 0);
    cmp_ne.Execute(cpu, memory);
    assert_true("CMP.NE should clear p6 when r19 reaches zero", !cpu.GetPR(6));

    std::cout << "  ? Compare-ne decoder mapping passed" << std::endl;
}

void test_latest_boot_log_blockers() {
    std::cout << "Testing latest boot-log raw instructions..." << std::endl;

    InstructionDecoder decoder;

    InstructionEx cmp_ltu = decoder.DecodeSlot(0x1a031b34000ULL, UnitType::I_UNIT, 0x36ec0);
    assert_true("Boot raw cmp.ltu should decode", cmp_ltu.GetType() == InstructionType::CMP_LTU);
    assert_equal("Boot cmp.ltu p1 decode", 0, cmp_ltu.GetDst());
    assert_equal("Boot cmp.ltu lhs register", 26, cmp_ltu.GetSrc1());
    assert_equal("Boot cmp.ltu rhs register", 27, cmp_ltu.GetSrc2());
    assert_equal("Boot cmp.ltu p2 decode", 6, cmp_ltu.GetSrc3());
    assert_string("Boot cmp.ltu disassembly",
                  "cmp.ltu p0, p6 = r26, r27",
                  cmp_ltu.GetDisassembly());

    CPUState cpu;
    Memory memory(1024 * 1024);

    InstructionEx fc = decoder.DecodeSlot(0x2182000000ULL, UnitType::M_UNIT, 0x1e100);
    assert_true("ELILO raw fc should decode", fc.GetType() == InstructionType::FC);
    assert_equal("ELILO fc qualifying predicate", 0, fc.GetPredicate());
    assert_equal("ELILO fc source register", 32, fc.GetSrc1());
    assert_equal("ELILO fc has no destination register", 0, fc.GetDst());
    assert_equal("ELILO fc has no second source register", 0, fc.GetSrc2());
    assert_string("ELILO fc disassembly", "fc r32", fc.GetDisassembly());

    cpu.SetAR(65, 0x1234);
    cpu.SetAR(66, 0x5678);
    cpu.SetCFM(0xabcde);
    cpu.SetIP(0x1e100);
    cpu.SetGR(32, 0x4006);
    cpu.SetGR(20, 0x1122334455667788ULL);
    fc.Execute(cpu, memory);
    assert_equal("fc must preserve its address register", 0x4006, cpu.GetGR(32));
    assert_equal("fc must preserve unrelated GR state", 0x1122334455667788ULL, cpu.GetGR(20));
    assert_equal("fc must preserve ar.lc", 0x1234, cpu.GetAR(65));
    assert_equal("fc must preserve ar.ec", 0x5678, cpu.GetAR(66));
    assert_equal("fc must preserve CFM", 0xabcde, cpu.GetCFM());
    assert_equal("fc must preserve IP", 0x1e100, cpu.GetIP());

    InstructionEx falseFc = fc;
    falseFc.SetPredicate(1);
    cpu.SetPR(1, false);
    cpu.SetGR(32, 0x5007);
    falseFc.Execute(cpu, memory);
    assert_equal("false-predicated fc must preserve its address register", 0x5007, cpu.GetGR(32));
    cpu.SetPR(1, true);

    CPUState userCpu;
    Memory unmappedMemory(0x2000);
    userCpu.SetPSR(3ULL << 32);
    userCpu.SetGR(32, 0x1000);
    unmappedMemory.GetMMU().ClearPageTable();
    bool fcFaulted = false;
    try {
        fc.Execute(userCpu, unmappedMemory);
    } catch (const std::exception&) {
        fcFaulted = true;
    }
    assert_true("user-mode fc must validate its translated read address", fcFaulted);

    InstructionEx syncI = decoder.DecodeSlot(0x198000000ULL, UnitType::M_UNIT, 0x1e120);
    assert_true("ELILO raw sync.i should decode", syncI.GetType() == InstructionType::SYNC_I);
    assert_equal("sync.i qualifying predicate", 0, syncI.GetPredicate());
    assert_string("sync.i disassembly", "sync.i", syncI.GetDisassembly());
    syncI.Execute(cpu, memory);

    InstructionEx srlzI = decoder.DecodeSlot(0x188000000ULL, UnitType::M_UNIT, 0x1e120);
    assert_true("ELILO raw srlz.i should decode", srlzI.GetType() == InstructionType::SRLZ_I);
    assert_equal("srlz.i qualifying predicate", 0, srlzI.GetPredicate());
    assert_string("srlz.i disassembly", "srlz.i", srlzI.GetDisassembly());
    srlzI.Execute(cpu, memory);

    // Exact Linux entry instruction at guest address 0x047f7b80.
    // Binutils disassembles raw 0x38180000 as: rsm 0x6000.
    InstructionEx rsm = decoder.DecodeSlot(0x38180000ULL, UnitType::M_UNIT, 0x047f7b80);
    assert_true("Linux entry rsm should decode", rsm.GetType() == InstructionType::RSM);
    assert_equal("Linux entry rsm qualifying predicate", 0, rsm.GetPredicate());
    assert_equal("Linux entry rsm immediate", 0x6000, rsm.GetImmediate());
    assert_string("Linux entry rsm disassembly", "rsm 0x6000", rsm.GetDisassembly());

    cpu.SetPSR((1ULL << 13) | (1ULL << 14) | (1ULL << 17) | (1ULL << 27) |
               (1ULL << 32));
    rsm.Execute(cpu, memory);
    assert_true("Linux entry rsm should clear PSR.IC", (cpu.GetPSR() & (1ULL << 13)) == 0);
    assert_true("Linux entry rsm should clear PSR.I", (cpu.GetPSR() & (1ULL << 14)) == 0);
    assert_true("Linux entry rsm should preserve PSR.DT", (cpu.GetPSR() & (1ULL << 17)) != 0);
    assert_true("Linux entry rsm should preserve upper PSR", (cpu.GetPSR() & (1ULL << 32)) != 0);

    // Exact Linux entry return-from-interruption instruction at 0x047f7e4c.
    // Retained Binutils 2.19.1 identifies raw 0x40000000 as unpredicated rfi.
    InstructionEx rfi = decoder.DecodeSlot(0x40000000ULL, UnitType::B_UNIT, 0x047f7e40);
    assert_true("Linux entry rfi should decode", rfi.GetType() == InstructionType::RFI);
    assert_equal("Linux entry rfi qualifying predicate", 0, rfi.GetPredicate());
    assert_string("Linux entry rfi disassembly", "rfi", rfi.GetDisassembly());
    CPUState rfiCpu;
    rfiCpu.SetCR(16, 0x1010084a2008ULL);
    rfiCpu.SetCR(19, 0xa0000001007f7e50ULL);
    rfi.Execute(rfiCpu, memory);
    assert_equal("rfi should restore IPSR into PSR", 0x1010084a2008ULL, rfiCpu.GetPSR());
    assert_equal("rfi should restore IIP as a bundle address",
                 0xa0000001007f7e50ULL & ~0xFULL, rfiCpu.GetIP());

    cpu.SetGR(26, 1);
    cpu.SetGR(27, 2);
    cmp_ltu.Execute(cpu, memory);
    assert_true("Boot cmp.ltu should clear complement when true", !cpu.GetPR(6));

    cpu.SetGR(26, 3);
    cpu.SetGR(27, 2);
    cmp_ltu.Execute(cpu, memory);
    assert_true("Boot cmp.ltu should set complement when false", cpu.GetPR(6));

    InstructionEx cmp_eq = decoder.DecodeSlot(0x1d048a10280ULL, UnitType::I_UNIT, 0x36ed0);
    assert_true("Boot raw cmp.eq should decode", cmp_eq.GetType() == InstructionType::CMP_EQ);
    assert_equal("Boot cmp.eq p1 decode", 10, cmp_eq.GetDst());
    assert_equal("Boot cmp.eq lhs register", 8, cmp_eq.GetSrc1());
    assert_equal("Boot cmp.eq rhs register", 10, cmp_eq.GetSrc2());
    assert_equal("Boot cmp.eq p2 decode", 9, cmp_eq.GetSrc3());
    assert_string("Boot cmp.eq disassembly",
                  "cmp.eq p10, p9 = r8, r10",
                  cmp_eq.GetDisassembly());

    InstructionEx cmp_eq_m_unit = decoder.DecodeSlot(0x1d048a10280ULL, UnitType::M_UNIT, 0x36ed0);
    assert_true("Boot raw cmp.eq should decode in M-unit slot",
                cmp_eq_m_unit.GetType() == InstructionType::CMP_EQ);
    assert_string("Boot M-unit cmp.eq disassembly",
                  "cmp.eq p10, p9 = r8, r10",
                  cmp_eq_m_unit.GetDisassembly());

    const uint8_t efiEntryBundle[16] = {
        0x00, 0x10, 0x19, 0x08, 0x80, 0x05, 0x30, 0x02,
        0x00, 0x62, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00
    };
    const uint64_t expectedSlot0 = 0x2c0040c880ULL;
    const uint64_t expectedSlot1 = 0x1880008c0ULL;
    const uint64_t expectedSlot2 = 0x8000000ULL;

    uint64_t low = 0;
    uint64_t high = 0;
    for (int i = 0; i < 8; ++i) {
        low |= static_cast<uint64_t>(efiEntryBundle[i]) << (i * 8);
        high |= static_cast<uint64_t>(efiEntryBundle[i + 8]) << (i * 8);
    }
    const uint64_t slot0 = (low >> 5) & 0x1FFFFFFFFFFULL;
    const uint64_t slot1 = ((low >> 46) | ((high & 0x7FFFFFFULL) << 18)) & 0x1FFFFFFFFFFULL;
    const uint64_t slot2 = (high >> 23) & 0x1FFFFFFFFFFULL;
    assert_equal("EFI entry bundle slot0 extraction", expectedSlot0, slot0);
    assert_equal("EFI entry bundle slot1 extraction", expectedSlot1, slot1);
    assert_equal("EFI entry bundle slot2 extraction", expectedSlot2, slot2);

    Bundle bundle = decoder.DecodeBundleAt(efiEntryBundle, 0x1000);
    assert_true("EFI entry bundle should decode as MII", bundle.templateType == TemplateType::MII);
    assert_true("EFI entry bundle should have three instructions", bundle.instructions.size() == 3);
    assert_true("EFI entry bundle slot 0 should decode as alloc",
                bundle.instructions[0].GetType() == InstructionType::ALLOC);
    assert_true("EFI entry bundle slot 1 should decode as mov from branch",
                bundle.instructions[1].GetType() == InstructionType::MOV_FROM_BR);
    assert_true("EFI entry bundle slot 2 should decode as nop",
                bundle.instructions[2].GetType() == InstructionType::NOP);
    assert_string("EFI entry bundle slot 0 disassembly",
                  "alloc r34 = ar.pfs, 6, 4, 0",
                  bundle.instructions[0].GetDisassembly());
    assert_string("EFI entry bundle slot 1 disassembly",
                  "mov r35 = b0",
                  bundle.instructions[1].GetDisassembly());
    assert_string("EFI entry bundle slot 2 disassembly",
                  "nop",
                  bundle.instructions[2].GetDisassembly());

    cpu.SetGR(8, 0x1234);
    cpu.SetGR(10, 0x1234);
    cmp_eq.Execute(cpu, memory);
    assert_true("Boot cmp.eq should set p10 when true", cpu.GetPR(10));
    assert_true("Boot cmp.eq should clear p9 when true", !cpu.GetPR(9));

    cpu.SetGR(10, 0x5678);
    cmp_eq.Execute(cpu, memory);
    assert_true("Boot cmp.eq should clear p10 when false", !cpu.GetPR(10));
    assert_true("Boot cmp.eq should set p9 when false", cpu.GetPR(9));

    InstructionEx getf_sig = decoder.DecodeSlot(0x8708014540ULL, UnitType::M_UNIT, 0x36ee0);
    assert_true("Boot raw getf.sig should decode", getf_sig.GetType() == InstructionType::GETF_SIG);
    assert_equal("Boot getf.sig destination register", 21, getf_sig.GetDst());
    assert_equal("Boot getf.sig source FP register", 10, getf_sig.GetSrc1());
    assert_string("Boot getf.sig disassembly",
                  "getf.sig r21 = f10",
                  getf_sig.GetDisassembly());

    uint8_t fr10[16] = {};
    const uint64_t significand = 0x0123456789abcdefULL;
    for (int i = 0; i < 8; ++i) {
        fr10[i] = static_cast<uint8_t>((significand >> (i * 8)) & 0xff);
    }
    cpu.SetFR(10, fr10);
    getf_sig.Execute(cpu, memory);
    assert_equal("Boot getf.sig should copy significand bytes", significand, cpu.GetGR(21));

    InstructionEx setf_sig = decoder.DecodeSlot(0xC708032280ULL, UnitType::M_UNIT, 0x36ec0);
    assert_true("Boot raw setf.sig should decode", setf_sig.GetType() == InstructionType::SETF_SIG);
    assert_equal("Boot setf.sig destination FP register", 10, setf_sig.GetDst());
    assert_equal("Boot setf.sig source general register", 25, setf_sig.GetSrc1());
    assert_string("Boot setf.sig disassembly",
                  "setf.sig f10 = r25",
                  setf_sig.GetDisassembly());

    const uint64_t setfValue = 0x0123456789abcdefULL;
    cpu.SetGR(25, setfValue);
    setf_sig.Execute(cpu, memory);
    uint8_t setfResult[16] = {};
    cpu.GetFR(10, setfResult);
    uint64_t setfSignificand = 0;
    uint64_t setfSignAndExponent = 0;
    for (int i = 0; i < 8; ++i) {
        setfSignificand |= static_cast<uint64_t>(setfResult[i]) << (i * 8);
        setfSignAndExponent |= static_cast<uint64_t>(setfResult[8 + i]) << (i * 8);
    }
    assert_equal("setf.sig should copy the complete integer significand",
                 setfValue, setfSignificand);
    assert_equal("setf.sig should use the integer-format exponent",
                 0x1003EULL, setfSignAndExponent);

    cpu.SetGRNaT(25, true);
    setf_sig.Execute(cpu, memory);
    cpu.GetFR(10, setfResult);
    uint64_t setfNatSignAndExponent = 0;
    for (int i = 0; i < 8; ++i) {
        setfNatSignAndExponent |= static_cast<uint64_t>(setfResult[8 + i]) << (i * 8);
    }
    assert_equal("setf.sig should produce FP NaTVal for a GR NaT source",
                 0x1FFFEULL, setfNatSignAndExponent);
    cpu.SetGRNaT(25, false);

    std::memset(setfResult, 0xA5, sizeof(setfResult));
    cpu.SetFR(10, setfResult);
    cpu.SetPR(1, false);
    InstructionEx predicatedSetf = setf_sig;
    predicatedSetf.SetPredicate(1);
    predicatedSetf.Execute(cpu, memory);
    uint8_t predicatedSetfResult[16] = {};
    cpu.GetFR(10, predicatedSetfResult);
    assert_true("false-predicated setf.sig should preserve its destination",
                std::memcmp(setfResult, predicatedSetfResult, sizeof(setfResult)) == 0);
    cpu.SetPR(1, true);

    InstructionEx xma_l = decoder.DecodeSlot(0x1d048a10280ULL, UnitType::F_UNIT, 0x36ed0);
    assert_true("Boot raw F-unit xma.l should decode", xma_l.GetType() == InstructionType::XMA);
    assert_equal("Boot xma.l destination FP register", 10, xma_l.GetDst());
    assert_equal("Boot xma.l f3 source FP register", 10, xma_l.GetSrc1());
    assert_equal("Boot xma.l f4 source FP register", 9, xma_l.GetSrc2());
    assert_equal("Boot xma.l f2 source FP register", 8, xma_l.GetSrc3());
    assert_string("Boot xma.l disassembly",
                  "xma.l f10 = f10, f9, f8",
                  xma_l.GetDisassembly());

    uint8_t fr8[16] = {};
    uint8_t fr9[16] = {};
    uint8_t fr10_operands[16] = {};
    const uint64_t addend = 5;
    const uint64_t multiplicand = 3;
    const uint64_t multiplier = 7;
    for (int i = 0; i < 8; ++i) {
        fr8[i] = static_cast<uint8_t>((addend >> (i * 8)) & 0xff);
        fr9[i] = static_cast<uint8_t>((multiplier >> (i * 8)) & 0xff);
        fr10_operands[i] = static_cast<uint8_t>((multiplicand >> (i * 8)) & 0xff);
    }
    cpu.SetFR(8, fr8);
    cpu.SetFR(9, fr9);
    cpu.SetFR(10, fr10_operands);
    xma_l.Execute(cpu, memory);
    getf_sig.Execute(cpu, memory);
    assert_equal("Boot xma.l should compute signed low product plus addend", 26, cpu.GetGR(21));

    const InstructionEx xma_h = decoder.DecodeSlot(0x1dc48a10280ULL, UnitType::F_UNIT, 0x36ed0);
    const InstructionEx xma_hu = decoder.DecodeSlot(0x1d848a10280ULL, UnitType::F_UNIT, 0x36ed0);
    assert_true("Boot raw xma.h should decode", xma_h.GetType() == InstructionType::XMA_H);
    assert_true("Boot raw xma.hu should decode", xma_hu.GetType() == InstructionType::XMA_HU);
    assert_string("Boot xma.h disassembly",
                  "xma.h f10 = f10, f9, f8",
                  xma_h.GetDisassembly());
    assert_string("Boot xma.hu disassembly",
                  "xma.hu f10 = f10, f9, f8",
                  xma_hu.GetDisassembly());

    uint8_t frNegativeOne[16] = {};
    uint8_t frTwo[16] = {};
    uint8_t frZero[16] = {};
    std::memset(frNegativeOne, 0xff, sizeof(frNegativeOne));
    frNegativeOne[8] = 0;
    frNegativeOne[9] = 0;
    frTwo[0] = 2;
    cpu.SetFR(10, frNegativeOne); // f3 = -1
    cpu.SetFR(9, frTwo);          // f4 = 2
    cpu.SetFR(8, frZero);         // f2 = 0
    xma_h.Execute(cpu, memory);
    uint8_t xmaHighResult[16] = {};
    cpu.GetFR(10, xmaHighResult);
    uint64_t xmaHighSignificand = 0;
    for (int i = 0; i < 8; ++i) {
        xmaHighSignificand |= static_cast<uint64_t>(xmaHighResult[i]) << (i * 8);
    }
    assert_equal("Boot xma.h should use signed high-half multiplication",
                 0xffffffffffffffffULL, xmaHighSignificand);

    xma_hu.Execute(cpu, memory);
    cpu.GetFR(10, xmaHighResult);
    xmaHighSignificand = 0;
    for (int i = 0; i < 8; ++i) {
        xmaHighSignificand |= static_cast<uint64_t>(xmaHighResult[i]) << (i * 8);
    }
    assert_equal("Boot xma.hu should use unsigned high-half multiplication",
                 1, xmaHighSignificand);

    uint8_t natVal[16] = {};
    natVal[8] = 0xfe;
    natVal[9] = 0xff;
    natVal[10] = 0x01;
    cpu.SetFR(8, natVal);
    xma_l.Execute(cpu, memory);
    uint8_t natResult[16] = {};
    cpu.GetFR(10, natResult);
    assert_true("Boot xma.l should propagate a source NaTVal",
                std::memcmp(natVal, natResult, sizeof(natVal)) == 0);

    uint8_t sentinel[16] = {};
    sentinel[0] = 0xa5;
    cpu.SetFR(10, sentinel);
    cpu.SetFR(8, fr8);
    cpu.SetPR(1, false);
    InstructionEx predicated_xma = xma_l;
    predicated_xma.SetPredicate(1);
    predicated_xma.Execute(cpu, memory);
    uint8_t predicatedResult[16] = {};
    cpu.GetFR(10, predicatedResult);
    assert_true("False-predicated xma.l should preserve its destination",
                std::memcmp(sentinel, predicatedResult, sizeof(sentinel)) == 0);

    const uint64_t immediateAddp4Raw = 0x11df80fe940ULL;
    InstructionEx immediateAddp4 = decoder.DecodeSlot(
        immediateAddp4Raw, UnitType::M_UNIT, 0xf6e0);
    assert_true("Boot raw immediate addp4 should decode",
                immediateAddp4.GetType() == InstructionType::ADDP4);
    assert_equal("Immediate addp4 predicate", 0, immediateAddp4.GetPredicate());
    assert_equal("Immediate addp4 destination", 37, immediateAddp4.GetDst());
    assert_equal("Immediate addp4 base register", 0, immediateAddp4.GetSrc1());
    assert_true("Immediate addp4 should retain its immediate form",
                immediateAddp4.HasImmediate());
    assert_equal("Immediate addp4 raw immediate", 0x3fff, immediateAddp4.GetImmediate() & 0x3fff);
    assert_true("Immediate addp4 should sign-extend imm14",
                static_cast<int64_t>(immediateAddp4.GetImmediate()) == -1);
    assert_string("Immediate addp4 disassembly",
                  "addp4 r37 = -1, r0",
                  immediateAddp4.GetDisassembly());

    cpu.SetGR(0, 0);
    cpu.SetGR(37, 0xaaaaaaaaaaaaaaaaULL);
    immediateAddp4.Execute(cpu, memory);
    assert_equal("Immediate addp4 negative value should use low-32 arithmetic",
                 0xffffffffULL, cpu.GetGR(37));

    const InstructionEx positiveAddp4 = decoder.DecodeSlot(
        build_addp4_imm14_slot(37, 5, 14), UnitType::M_UNIT, 0xf6e0);
    assert_true("Positive immediate addp4 should decode",
                positiveAddp4.GetType() == InstructionType::ADDP4);
    assert_equal("Positive immediate addp4 base register", 14, positiveAddp4.GetSrc1());
    assert_true("Positive immediate addp4 value should be 5",
                static_cast<int64_t>(positiveAddp4.GetImmediate()) == 5);
    assert_string("Positive immediate addp4 disassembly",
                  "addp4 r37 = 5, r14",
                  positiveAddp4.GetDisassembly());
    cpu.SetGR(14, 0xc000000080000003ULL);
    positiveAddp4.Execute(cpu, memory);
    assert_equal("Immediate addp4 should preserve base pointer region bits",
                 0x4000000080000008ULL, cpu.GetGR(37));

    const InstructionEx falseImmediateAddp4 = decoder.DecodeSlot(
        build_addp4_imm14_slot(37, 5, 14, 1), UnitType::M_UNIT, 0xf6e0);
    cpu.SetPR(1, false);
    cpu.SetGR(37, 0xfeedfaceULL);
    falseImmediateAddp4.Execute(cpu, memory);
    assert_equal("False-predicated immediate addp4 should preserve destination",
                 0xfeedfaceULL, cpu.GetGR(37));

    cpu.SetPR(1, true);
    cpu.SetGRNaT(14, true);
    positiveAddp4.Execute(cpu, memory);
    assert_true("Immediate addp4 should propagate base NaT",
                cpu.GetGRNaT(37));
    cpu.SetGRNaT(14, false);

    InstructionEx mov_to_br = decoder.DecodeSlot(0xe0014a000ULL, UnitType::I_UNIT, 0x30700);
    assert_true("Boot raw mov-to-branch should decode", mov_to_br.GetType() == InstructionType::MOV_TO_BR);
    assert_equal("Boot mov-to-branch destination branch register", 0, mov_to_br.GetDst());
    assert_equal("Boot mov-to-branch source general register", 37, mov_to_br.GetSrc1());
    assert_string("Boot mov-to-branch disassembly",
                  "mov b0 = r37",
                  mov_to_br.GetDisassembly());

    cpu.SetGR(37, 0x123456789abcdef0ULL);
    mov_to_br.Execute(cpu, memory);
    assert_equal("Boot mov-to-branch should restore b0", 0x123456789abcdef0ULL, cpu.GetBR(0));

    InstructionEx shladd_scale5 = decoder.DecodeSlot(0x10088e1c200ULL, UnitType::I_UNIT, 0xa100);
    assert_true("Boot raw shladd scale-5 should decode",
                shladd_scale5.GetType() == InstructionType::SHLADD);
    assert_equal("Boot shladd scale-5 destination", 8, shladd_scale5.GetDst());
    assert_equal("Boot shladd scale-5 source", 14, shladd_scale5.GetSrc1());
    assert_equal("Boot shladd scale-5 addend", 14, shladd_scale5.GetSrc2());
    assert_equal("Boot shladd scale-5 count", 2, shladd_scale5.GetImmediate());
    assert_string("Boot shladd scale-5 disassembly",
                  "shladd r8 = r14, 2, r14",
                  shladd_scale5.GetDisassembly());

    cpu.SetGR(14, 7);
    shladd_scale5.Execute(cpu, memory);
    assert_equal("Boot shladd scale-5 should compute index * 5", 35, cpu.GetGR(8));

    InstructionEx shladd_scale40 = decoder.DecodeSlot(0x10091110400ULL, UnitType::I_UNIT, 0xa110);
    assert_true("Boot raw shladd scale-40 should decode",
                shladd_scale40.GetType() == InstructionType::SHLADD);
    assert_equal("Boot shladd scale-40 destination", 16, shladd_scale40.GetDst());
    assert_equal("Boot shladd scale-40 source", 8, shladd_scale40.GetSrc1());
    assert_equal("Boot shladd scale-40 addend", 17, shladd_scale40.GetSrc2());
    assert_equal("Boot shladd scale-40 count", 3, shladd_scale40.GetImmediate());
    assert_string("Boot shladd scale-40 disassembly",
                  "shladd r16 = r8, 3, r17",
                  shladd_scale40.GetDisassembly());

    cpu.SetGR(8, 35);
    cpu.SetGR(17, 0x1000);
    shladd_scale40.Execute(cpu, memory);
    assert_equal("Boot shladd scale-40 should compute base + index * 40", 0x1118, cpu.GetGR(16));

    // Exact Debian DVD/netinst ELILO blocker: I7 fixed-count SHL.
    InstructionEx shl_fixed = decoder.DecodeSlot(0xeca0042840ULL, UnitType::I_UNIT, 0x27e80);
    assert_true("Debian fixed-count SHL should decode",
                shl_fixed.GetType() == InstructionType::SHL);
    assert_equal("Debian fixed-count SHL destination", 33, shl_fixed.GetDst());
    assert_equal("Debian fixed-count SHL source", 33, shl_fixed.GetSrc1());
    assert_true("Debian fixed-count SHL should carry an immediate",
                shl_fixed.HasImmediate());
    assert_equal("Debian fixed-count SHL count", 0, shl_fixed.GetImmediate());
    assert_string("Debian fixed-count SHL disassembly",
                  "shl r33 = r33, 0",
                  shl_fixed.GetDisassembly());

    cpu.SetGR(33, 0x123456789abcdef0ULL);
    shl_fixed.Execute(cpu, memory);
    assert_equal("Debian fixed-count SHL should preserve count-zero source",
                 0x123456789abcdef0ULL,
                 cpu.GetGR(33));

    // IA-64 major-5 DEP.Z alias used by the authentic ELILO descriptor-index
    // calculation.  Historical Binutils decodes this as shl r20=r19,32;
    // treating it as EXTR corrupts the second fops descriptor after the
    // configuration file is closed.
    InstructionEx shl_major5 = decoder.DecodeSlot(0xa6f9f26500ULL, UnitType::I_UNIT, 0x5940);
    assert_true("Major-5 fixed-count SHL should decode",
                shl_major5.GetType() == InstructionType::SHL);
    assert_equal("Major-5 fixed-count SHL destination", 20, shl_major5.GetDst());
    assert_equal("Major-5 fixed-count SHL source", 19, shl_major5.GetSrc1());
    assert_true("Major-5 fixed-count SHL should carry an immediate",
                shl_major5.HasImmediate());
    assert_equal("Major-5 fixed-count SHL count", 32, shl_major5.GetImmediate());
    assert_string("Major-5 fixed-count SHL disassembly",
                  "shl r20 = r19, 32",
                  shl_major5.GetDisassembly());

    cpu.SetGR(19, 0x12345678ULL);
    shl_major5.Execute(cpu, memory);
    assert_equal("Major-5 fixed-count SHL should shift by 32",
                 0x1234567800000000ULL,
                 cpu.GetGR(20));

    // Exact authentic Debian find_kernel_memory instruction at 0x98d0.
    // Historical Binutils decodes this major-5 I7 form as
    // "shl r10=r24,12".  Decoding it as EXTR makes the apparent
    // conventional descriptor end look too small and causes ELILO to reject
    // the descriptor even though the EFI map is valid.
    InstructionEx shlMemoryMapEnd = decoder.DecodeSlot(0xa79b330280ULL,
                                                        UnitType::I_UNIT,
                                                        0x98d0);
    assert_true("ELILO memory-map SHL should decode",
                shlMemoryMapEnd.GetType() == InstructionType::SHL);
    assert_equal("ELILO memory-map SHL destination", 10,
                 shlMemoryMapEnd.GetDst());
    assert_equal("ELILO memory-map SHL source", 24,
                 shlMemoryMapEnd.GetSrc1());
    assert_true("ELILO memory-map SHL should carry an immediate",
                shlMemoryMapEnd.HasImmediate());
    assert_equal("ELILO memory-map SHL count", 12,
                 shlMemoryMapEnd.GetImmediate());
    assert_string("ELILO memory-map SHL disassembly",
                  "shl r10 = r24, 12",
                  shlMemoryMapEnd.GetDisassembly());

    cpu.SetGR(24, 0x17f00);
    shlMemoryMapEnd.Execute(cpu, memory);
    assert_equal("ELILO memory-map SHL should compute pages byte length",
                 0x17f00000ULL,
                 cpu.GetGR(10));

    // Exact authentic ELILO memcpy_long prologue instructions at 0x28146 and
    // 0x2814c.  These are the fixed-count SHLs that align the source words
    // before the first bulk copy.  Decoding either one as EXTR makes the
    // source word assembly lose the byte-alignment shift and propagates a
    // zero into the decompressed output.
    InstructionEx memcpySourceShift = decoder.DecodeSlot(0xa7e3c28500ULL,
                                                          UnitType::I_UNIT,
                                                          0x28140);
    assert_true("ELILO memcpy source SHL should decode",
                memcpySourceShift.GetType() == InstructionType::SHL);
    assert_equal("ELILO memcpy source SHL destination", 20,
                 memcpySourceShift.GetDst());
    assert_equal("ELILO memcpy source SHL source", 20,
                 memcpySourceShift.GetSrc1());
    assert_true("ELILO memcpy source SHL should carry an immediate",
                memcpySourceShift.HasImmediate());
    assert_equal("ELILO memcpy source SHL count", 3,
                 memcpySourceShift.GetImmediate());
    assert_string("ELILO memcpy source SHL disassembly",
                  "shl r20 = r20, 3",
                  memcpySourceShift.GetDisassembly());
    cpu.SetGR(20, 1);
    memcpySourceShift.Execute(cpu, memory);
    assert_equal("ELILO memcpy source SHL should shift by three",
                 8,
                 cpu.GetGR(20));

    InstructionEx memcpyDestinationShift = decoder.DecodeSlot(0xa7e3c2c580ULL,
                                                               UnitType::I_UNIT,
                                                               0x28140);
    assert_true("ELILO memcpy destination SHL should decode",
                memcpyDestinationShift.GetType() == InstructionType::SHL);
    assert_equal("ELILO memcpy destination SHL destination", 22,
                 memcpyDestinationShift.GetDst());
    assert_equal("ELILO memcpy destination SHL source", 22,
                 memcpyDestinationShift.GetSrc1());
    assert_true("ELILO memcpy destination SHL should carry an immediate",
                memcpyDestinationShift.HasImmediate());
    assert_equal("ELILO memcpy destination SHL count", 3,
                 memcpyDestinationShift.GetImmediate());
    assert_string("ELILO memcpy destination SHL disassembly",
                  "shl r22 = r22, 3",
                  memcpyDestinationShift.GetDisassembly());
    cpu.SetGR(22, 1);
    memcpyDestinationShift.Execute(cpu, memory);
    assert_equal("ELILO memcpy destination SHL should shift by three",
                 8,
                 cpu.GetGR(22));

    // Exact authentic ELILO memcpy_long alignment dispatch at 0x281e6.
    // This I7 fixed-count SHL is encoded in the major-5 DEP.Z alias space;
    // decoding it as EXTR leaves the loop target at COPY(0,1) instead of the
    // required COPY(16,0) path.
    InstructionEx memcpyLoopDispatchShift = decoder.DecodeSlot(
        0xa7cb924480ULL, UnitType::I_UNIT, 0x281e0);
    assert_true("ELILO memcpy loop-dispatch SHL should decode",
                memcpyLoopDispatchShift.GetType() == InstructionType::SHL);
    assert_equal("ELILO memcpy loop-dispatch SHL destination", 18,
                 memcpyLoopDispatchShift.GetDst());
    assert_equal("ELILO memcpy loop-dispatch SHL source", 18,
                 memcpyLoopDispatchShift.GetSrc1());
    assert_true("ELILO memcpy loop-dispatch SHL should carry an immediate",
                memcpyLoopDispatchShift.HasImmediate());
    assert_equal("ELILO memcpy loop-dispatch SHL count", 6,
                 memcpyLoopDispatchShift.GetImmediate());
    assert_string("ELILO memcpy loop-dispatch SHL disassembly",
                  "shl r18 = r18, 6",
                  memcpyLoopDispatchShift.GetDisassembly());
    cpu.SetGR(18, 2);
    memcpyLoopDispatchShift.Execute(cpu, memory);
    assert_equal("ELILO memcpy loop-dispatch SHL should select 16-byte path",
                 0x80,
                 cpu.GetGR(18));

    // Exact authentic ELILO huft_build instruction at 0x21f00.  Historical
    // Binutils decodes this raw slot as "shr.u r14=r25,r23".  The encoded
    // source/count fields are reversed relative to SHL.
    InstructionEx debianShr = decoder.DecodeSlot(0xf20192e380ULL,
                                                  UnitType::I_UNIT,
                                                  0x21f00);
    assert_true("Authentic ELILO variable SHR should decode",
                debianShr.GetType() == InstructionType::SHR);
    assert_equal("Authentic ELILO SHR destination", 14, debianShr.GetDst());
    assert_equal("Authentic ELILO SHR source", 25, debianShr.GetSrc1());
    assert_equal("Authentic ELILO SHR count", 23, debianShr.GetSrc2());
    assert_string("Authentic ELILO SHR disassembly",
                  "shr r14 = r25, r23",
                  debianShr.GetDisassembly());

    cpu.SetGR(25, 2);
    cpu.SetGR(23, 0);
    debianShr.Execute(cpu, memory);
    assert_equal("Authentic ELILO SHR count-zero result", 2, cpu.GetGR(14));

    cpu.SetGR(25, 0x8000000000000000ULL);
    cpu.SetGR(23, 63);
    debianShr.Execute(cpu, memory);
    assert_equal("Authentic ELILO SHR logical sign-bit result", 1,
                 cpu.GetGR(14));

    // Exact authentic ELILO repeat-index update at 0x25730/0x259d0.
    // Historical Binutils decodes this A1 slot as "add r40=r40,r16,1".
    const uint64_t rawAddP1 = 0x10009050a00ULL;
    InstructionEx debianAddP1 = decoder.DecodeSlot(rawAddP1,
                                                    UnitType::I_UNIT,
                                                    0x25730);
    assert_true("Authentic ELILO three-input ADD should decode",
                debianAddP1.GetType() == InstructionType::ADD_P1);
    assert_equal("Authentic ELILO ADD destination", 40, debianAddP1.GetDst());
    assert_equal("Authentic ELILO ADD source 1", 40, debianAddP1.GetSrc1());
    assert_equal("Authentic ELILO ADD source 2", 16, debianAddP1.GetSrc2());
    assert_string("Authentic ELILO ADD disassembly",
                  "add r40 = r40, r16, 1",
                  debianAddP1.GetDisassembly());

    cpu.SetGR(40, 93);
    cpu.SetGR(16, 2);
    debianAddP1.Execute(cpu, memory);
    assert_equal("Authentic ELILO ADD plus-one result", 96, cpu.GetGR(40));

    InstructionEx ordinaryAdd = decoder.DecodeSlot(rawAddP1 & ~(1ULL << 27),
                                                     UnitType::I_UNIT,
                                                     0x25730);
    assert_true("Ordinary A1 ADD should remain distinct",
                ordinaryAdd.GetType() == InstructionType::ADD);
    cpu.SetGR(40, 93);
    cpu.SetGR(16, 2);
    ordinaryAdd.Execute(cpu, memory);
    assert_equal("Ordinary A1 ADD result", 95, cpu.GetGR(40));

    InstructionEx zxt4_return = decoder.DecodeSlot(0x90800200ULL, UnitType::I_UNIT, 0x34b10);
    assert_true("Boot raw zxt4 should decode", zxt4_return.GetType() == InstructionType::ZXT4);
    assert_equal("Boot zxt4 destination", 8, zxt4_return.GetDst());
    assert_equal("Boot zxt4 source", 8, zxt4_return.GetSrc1());
    assert_string("Boot zxt4 disassembly",
                  "zxt4 r8 = r8",
                  zxt4_return.GetDisassembly());

    cpu.SetGR(8, 0xffffffff80000001ULL);
    zxt4_return.Execute(cpu, memory);
    assert_equal("Boot zxt4 should clear high 32 bits", 0x80000001ULL, cpu.GetGR(8));

    InstructionEx mov_i_from_ar = decoder.DecodeSlot(0x15c3400000ULL, UnitType::I_UNIT, 0x42000);
    assert_true("Boot raw mov.i from AR should decode",
                mov_i_from_ar.GetType() == InstructionType::MOV_FROM_AR);
    assert_equal("Boot mov.i from AR destination", 0, mov_i_from_ar.GetDst());
    assert_equal("Boot mov.i from AR source application register", 52, mov_i_from_ar.GetSrc1());
    assert_string("Boot mov.i from AR disassembly",
                  "mov r0 = ar.52",
                  mov_i_from_ar.GetDisassembly());

    cpu.SetAR(52, 0x1122334455667788ULL);
    mov_i_from_ar.Execute(cpu, memory);
    assert_equal("Boot mov.i from AR should leave r0 unchanged", 0ULL, cpu.GetGR(0));

    InstructionEx mov_m_from_ar = decoder.DecodeSlot(0x2112400ac0ULL, UnitType::M_UNIT, 0x32a60);
    assert_true("Boot raw mov.m from AR should decode",
                mov_m_from_ar.GetType() == InstructionType::MOV_FROM_AR);
    assert_equal("Boot mov.m from AR destination", 43, mov_m_from_ar.GetDst());
    assert_equal("Boot mov.m from AR source application register", 36, mov_m_from_ar.GetSrc1());
    assert_string("Boot mov.m from AR disassembly",
                  "mov r43 = ar.36",
                  mov_m_from_ar.GetDisassembly());

    cpu.SetAR(36, 0x0123456789abcdefULL);
    cpu.SetGRNaT(43, true);
    mov_m_from_ar.Execute(cpu, memory);
    assert_equal("Boot mov.m from AR should copy application register", 0x0123456789abcdefULL, cpu.GetGR(43));
    assert_true("Boot mov.m from AR should clear destination NaT", !cpu.GetGRNaT(43));

    InstructionEx or_imm = decoder.DecodeSlot(0x10170e0e440ULL, UnitType::M_UNIT, 0x32590);
    assert_true("Boot raw OR immediate should decode", or_imm.GetType() == InstructionType::OR_IMM);
    assert_equal("Boot OR immediate destination", 17, or_imm.GetDst());
    assert_equal("Boot OR immediate source", 14, or_imm.GetSrc2());
    assert_equal("Boot OR immediate value", 7, or_imm.GetImmediate());
    assert_string("Boot OR immediate disassembly",
                  "or r17 = 7, r14",
                  or_imm.GetDisassembly());

    cpu.SetGR(14, 0x12340);
    or_imm.Execute(cpu, memory);
    assert_equal("Boot OR immediate should set low immediate bits", 0x12347, cpu.GetGR(17));

    InstructionEx zxt2_value = decoder.DecodeSlot(0x88800fc0ULL, UnitType::I_UNIT, 0x31c50);
    assert_true("Boot raw zxt2 should decode", zxt2_value.GetType() == InstructionType::ZXT2);
    assert_equal("Boot zxt2 destination", 63, zxt2_value.GetDst());
    assert_equal("Boot zxt2 source", 8, zxt2_value.GetSrc1());
    assert_string("Boot zxt2 disassembly",
                  "zxt2 r63 = r8",
                  zxt2_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffffffff807fULL);
    zxt2_value.Execute(cpu, memory);
    assert_equal("Boot zxt2 should keep low 16 bits", 0x807fULL, cpu.GetGR(63));

    InstructionEx sxt1_value = decoder.DecodeSlot(0xa0800200ULL, UnitType::I_UNIT, 0x31c60);
    assert_true("Boot raw sxt1 should decode", sxt1_value.GetType() == InstructionType::SXT1);
    assert_equal("Boot sxt1 destination", 8, sxt1_value.GetDst());
    assert_equal("Boot sxt1 source", 8, sxt1_value.GetSrc1());
    assert_string("Boot sxt1 disassembly",
                  "sxt1 r8 = r8",
                  sxt1_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffffffffff80ULL);
    sxt1_value.Execute(cpu, memory);
    assert_equal("Boot sxt1 should sign-extend byte", 0xffffffffffffff80ULL, cpu.GetGR(8));

    InstructionEx sxt2_value = decoder.DecodeSlot(0xa8800200ULL, UnitType::I_UNIT, 0x31c70);
    assert_true("Boot raw sxt2 should decode", sxt2_value.GetType() == InstructionType::SXT2);
    assert_equal("Boot sxt2 destination", 8, sxt2_value.GetDst());
    assert_equal("Boot sxt2 source", 8, sxt2_value.GetSrc1());
    assert_string("Boot sxt2 disassembly",
                  "sxt2 r8 = r8",
                  sxt2_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffffffff8001ULL);
    sxt2_value.Execute(cpu, memory);
    assert_equal("Boot sxt2 should sign-extend halfword", 0xffffffffffff8001ULL, cpu.GetGR(8));

    InstructionEx sxt4_value = decoder.DecodeSlot(0xb0800200ULL, UnitType::I_UNIT, 0x31c80);
    assert_true("Boot raw sxt4 should decode", sxt4_value.GetType() == InstructionType::SXT4);
    assert_equal("Boot sxt4 destination", 8, sxt4_value.GetDst());
    assert_equal("Boot sxt4 source", 8, sxt4_value.GetSrc1());
    assert_string("Boot sxt4 disassembly",
                  "sxt4 r8 = r8",
                  sxt4_value.GetDisassembly());

    cpu.SetGR(8, 0xffffffff80000001ULL);
    sxt4_value.Execute(cpu, memory);
    assert_equal("Boot sxt4 should sign-extend word", 0xffffffff80000001ULL, cpu.GetGR(8));

    InstructionEx cloop = decoder.DecodeSlot(0xb1ffffc140ULL, UnitType::B_UNIT, 0xa120);
    assert_true("Boot raw counted-loop branch should decode",
                cloop.GetType() == InstructionType::BR_CLOOP);
    assert_equal("Boot counted-loop target", 0xa100, cloop.GetBranchTarget());
    assert_string("Boot counted-loop disassembly",
                  "br.cloop 0xa100",
                  cloop.GetDisassembly());

    cpu.SetAR(65, 2);
    cloop.Execute(cpu, memory);
    assert_equal("br.cloop should decrement ar.lc when nonzero", 1, cpu.GetAR(65));

    // Gentoo's raw back-edge is major opcode 4, btype=5.  The architecture
    // defines this as an unpredicated IP-relative br.cloop.
    InstructionEx gentooCloop = decoder.DecodeSlot(0x8000026140ULL,
                                                    UnitType::B_UNIT,
                                                    0x16f80);
    assert_true("Gentoo raw back-edge should decode as br.cloop",
                gentooCloop.GetType() == InstructionType::BR_CLOOP);
    assert_equal("Gentoo counted-loop predicate", 0, gentooCloop.GetPredicate());
    assert_equal("Gentoo counted-loop target", 0x170b0, gentooCloop.GetBranchTarget());
    assert_string("Gentoo counted-loop disassembly",
                  "br.cloop 0x170b0",
                  gentooCloop.GetDisassembly());

    InstructionEx chk_a_clr = decoder.DecodeSlot(0xa00018280ULL, UnitType::M_UNIT, 0xeb30);
    assert_true("Boot raw chk.a.clr should decode",
                chk_a_clr.GetType() == InstructionType::CHK_A_CLR);
    assert_equal("Boot chk.a.clr checked register", 10, chk_a_clr.GetDst());
    assert_equal("Boot chk.a.clr recovery target", 0xebf0, chk_a_clr.GetBranchTarget());
    assert_string("Boot chk.a.clr disassembly",
                  "chk.a.clr r10, 0xebf0",
                  chk_a_clr.GetDisassembly());

    cpu.SetGR(10, 0x1122334455667788ULL);
    chk_a_clr.Execute(cpu, memory);
    assert_equal("chk.a.clr stub should leave checked register unchanged",
                 0x1122334455667788ULL, cpu.GetGR(10));

    InstructionEx load_options_chars = decoder.DecodeSlot(0xa5f2104846ULL, UnitType::I_UNIT, 0x86b0);
    assert_true("Boot raw load-options byte-to-char extract should decode",
                load_options_chars.GetType() == InstructionType::EXTR);
    assert_equal("Boot load-options extract destination", 33, load_options_chars.GetDst());
    assert_equal("Boot load-options extract source", 33, load_options_chars.GetSrc1());
    assert_equal("Boot load-options extract position", 1, load_options_chars.GetImmediate() & 0x3f);
    assert_equal("Boot load-options extract encoded length", 62, load_options_chars.GetImmediate() >> 6);
    assert_string("Boot load-options extract disassembly",
                  "extr r33 = r33, 1, 63",
                  load_options_chars.GetDisassembly());

    cpu.SetGR(2, 1);
    cpu.SetGR(33, 2);
    load_options_chars.Execute(cpu, memory, true);
    assert_equal("Boot load-options extract should ignore stale r2 and halve byte count",
                 1, cpu.GetGR(33));

    InstructionEx shrp = decoder.DecodeSlot(0xadf2104846ULL, UnitType::I_UNIT, 0x86b0);
    assert_true("Boot raw shrp should decode",
                shrp.GetType() == InstructionType::SHRP);
    assert_equal("Boot shrp destination", 33, shrp.GetDst());
    assert_equal("Boot shrp high source", 2, shrp.GetSrc1());
    assert_equal("Boot shrp low source", 33, shrp.GetSrc2());
    assert_equal("Boot shrp count", 62, shrp.GetImmediate());
    assert_string("Boot shrp disassembly",
                  "shrp r33 = r2, r33, 62",
                  shrp.GetDisassembly());

    cpu.SetGR(2, 0x0123456789abcdefULL);
    cpu.SetGR(33, 0xf000000000000000ULL);
    shrp.Execute(cpu, memory, true);
    assert_equal("shrp should concatenate high:low and keep shifted low half",
                 0x048d159e26af37bfULL, cpu.GetGR(33));

    InstructionEx loop_cmp = decoder.DecodeSlot(0x1a03a11e180ULL, UnitType::I_UNIT, 0x86c0);
    assert_true("Loop cmp.ltu should decode as register compare",
                loop_cmp.GetType() == InstructionType::CMP_LTU);
    assert_equal("Loop cmp.ltu p1 decode", 6, loop_cmp.GetDst());
    assert_equal("Loop cmp.ltu lhs register", 15, loop_cmp.GetSrc1());
    assert_equal("Loop cmp.ltu rhs register", 33, loop_cmp.GetSrc2());
    assert_equal("Loop cmp.ltu p2 decode", 7, loop_cmp.GetSrc3());
    assert_true("Loop cmp.ltu should not be immediate", !loop_cmp.HasImmediate());
    assert_string("Loop cmp.ltu disassembly",
                  "cmp.ltu p6, p7 = r15, r33",
                  loop_cmp.GetDisassembly());

    cpu.SetGR(15, 3);
    cpu.SetGR(33, 4);
    loop_cmp.Execute(cpu, memory);
    assert_true("Loop cmp.ltu should keep p6 true while index is below bound", cpu.GetPR(6));
    assert_true("Loop cmp.ltu should clear p7 while index is below bound", !cpu.GetPR(7));

    cpu.SetGR(15, 4);
    cpu.SetGR(33, 4);
    loop_cmp.Execute(cpu, memory);
    assert_true("Loop cmp.ltu should clear p6 at loop bound", !cpu.GetPR(6));
    assert_true("Loop cmp.ltu should set p7 at loop bound", cpu.GetPR(7));

    InstructionEx next_cmp = decoder.DecodeSlot(0x1a0521202c0ULL, UnitType::I_UNIT, 0x86d0);
    assert_true("Loop next cmp.ltu should decode",
                next_cmp.GetType() == InstructionType::CMP_LTU);
    assert_equal("Loop next cmp.ltu p1 decode", 11, next_cmp.GetDst());
    assert_equal("Loop next cmp.ltu lhs register", 16, next_cmp.GetSrc1());
    assert_equal("Loop next cmp.ltu rhs register", 33, next_cmp.GetSrc2());
    assert_equal("Loop next cmp.ltu p2 decode", 10, next_cmp.GetSrc3());
    assert_string("Loop next cmp.ltu disassembly",
                  "cmp.ltu p11, p10 = r16, r33",
                  next_cmp.GetDisassembly());

    InstructionEx space_cmp = decoder.DecodeSlot(0x1ce30e411c0ULL, UnitType::I_UNIT, 0x86e0);
    assert_true("Loop space compare should decode as cmp4.ne",
                space_cmp.GetType() == InstructionType::CMP4_NE);
    assert_true("Loop space compare should use or.andcm completer",
                space_cmp.GetCompareCompleter() == CompareCompleter::OR_ANDCM);
    assert_equal("Loop space compare p1 decode", 7, space_cmp.GetDst());
    assert_equal("Loop space compare source register", 14, space_cmp.GetSrc2());
    assert_equal("Loop space compare immediate", 32, space_cmp.GetImmediate());
    assert_string("Loop space compare disassembly",
                  "cmp4.ne.or.andcm p7, p6 = 32, r14",
                  space_cmp.GetDisassembly());

    cpu.SetPR(6, true);
    cpu.SetPR(7, false);
    cpu.SetGR(14, 32);
    space_cmp.Execute(cpu, memory);
    assert_true("cmp4.ne.or.andcm should leave p6 true for a space", cpu.GetPR(6));
    assert_true("cmp4.ne.or.andcm should leave p7 false for a space", !cpu.GetPR(7));

    cpu.SetPR(6, true);
    cpu.SetPR(7, false);
    cpu.SetGR(14, 'A');
    space_cmp.Execute(cpu, memory);
    assert_true("cmp4.ne.or.andcm should clear p6 for non-space", !cpu.GetPR(6));
    assert_true("cmp4.ne.or.andcm should set p7 for non-space", cpu.GetPR(7));

    InstructionEx nul_cmp = decoder.DecodeSlot(0x1cc40e00240ULL, UnitType::I_UNIT, 0x86e0);
    assert_true("Loop null compare should decode as cmp4.eq",
                nul_cmp.GetType() == InstructionType::CMP4_EQ);
    assert_equal("Loop null compare p1 decode", 9, nul_cmp.GetDst());
    assert_equal("Loop null compare source register", 14, nul_cmp.GetSrc2());
    assert_equal("Loop null compare immediate", 0, nul_cmp.GetImmediate());
    assert_string("Loop null compare disassembly",
                  "cmp4.eq p9, p8 = 0, r14",
                  nul_cmp.GetDisassembly());

    std::cout << "  ? Latest boot-log raw instructions passed" << std::endl;
}

void test_ia64_region_register_moves() {
    std::cout << "Testing IA-64 indirect region-register moves..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact Linux kernel entry instruction at guest address 0x047f7bb0.
    // Retained Binutils 2.19.1 disassembles raw 0x2080200200 as
    // "mov r8=rr[r2]" in the MMI bundle following the entry rsm/srlz.i.
    const uint64_t rawFromRR = 0x2080200200ULL;
    const InstructionEx fromRR = decoder.DecodeSlot(
        rawFromRR, UnitType::M_UNIT, 0x047f7bb0);
    assert_true("Linux entry mov-from-RR should decode",
                fromRR.GetType() == InstructionType::MOV_FROM_RR);
    assert_equal("mov-from-RR destination register", 8, fromRR.GetDst());
    assert_equal("mov-from-RR selector register", 2, fromRR.GetSrc1());
    assert_equal("mov-from-RR qualifying predicate", 0, fromRR.GetPredicate());
    assert_string("mov-from-RR disassembly",
                  "mov r8 = rr[r2]",
                  fromRR.GetDisassembly());

    cpu.SetGR(2, 3ULL << 61);
    cpu.SetRR(3, 0x123456789abcdef0ULL);
    fromRR.Execute(cpu, memory);
    assert_equal("mov-from-RR should select with GR bits 63:61",
                 0x123456789abcdef0ULL,
                 cpu.GetGR(8));

    // Exact adjacent Linux instruction at guest address 0x047f7cb6.
    // Its raw slot is 0x2000220000 and Binutils disassembles it as
    // "mov rr[r2]=r16".
    const uint64_t rawToRR = 0x2000220000ULL;
    const InstructionEx toRR = decoder.DecodeSlot(
        rawToRR, UnitType::M_UNIT, 0x047f7cb0);
    assert_true("Linux entry mov-to-RR should decode",
                toRR.GetType() == InstructionType::MOV_TO_RR);
    assert_equal("mov-to-RR selector register", 2, toRR.GetDst());
    assert_equal("mov-to-RR source register", 16, toRR.GetSrc1());
    assert_equal("mov-to-RR qualifying predicate", 0, toRR.GetPredicate());
    assert_string("mov-to-RR disassembly",
                  "mov rr[r2] = r16",
                  toRR.GetDisassembly());

    cpu.SetGR(2, 5ULL << 61);
    cpu.SetGR(16, 0xfedcba9876543210ULL);
    toRR.Execute(cpu, memory);
    assert_equal("mov-to-RR should select with GR bits 63:61",
                 0xfedcba9876543210ULL,
                 cpu.GetRR(5));

    std::cout << "  ? IA-64 indirect region-register moves passed" << std::endl;
}

void test_ia64_control_register_moves() {
    std::cout << "Testing IA-64 indirect control-register moves..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact Linux kernel entry instruction at guest address 0x047f7db0.
    // Retained Binutils 2.19.1 disassembles raw 0x2161524000 as
    // "mov cr.itir=r18"; cr.itir is indirect selector 21.
    const uint64_t rawToCR = 0x2161524000ULL;
    const InstructionEx toCR = decoder.DecodeSlot(
        rawToCR, UnitType::M_UNIT, 0x047f7db0);
    assert_true("Linux entry mov-to-CR should decode",
                toCR.GetType() == InstructionType::MOV_TO_CR);
    assert_equal("mov-to-CR selector", 21, toCR.GetDst());
    assert_equal("mov-to-CR source register", 18, toCR.GetSrc1());
    assert_equal("mov-to-CR qualifying predicate", 0, toCR.GetPredicate());
    assert_string("mov-to-CR disassembly",
                  "mov cr[r21] = r18",
                  toCR.GetDisassembly());

    cpu.SetGR(18, 0x123456789abcdef0ULL);
    toCR.Execute(cpu, memory);
    assert_equal("mov-to-CR should copy the source value",
                 0x123456789abcdef0ULL,
                 cpu.GetCR(21));

    // The from-form uses x6=0x24 and the same cr3 selector field.  This raw
    // encoding is the exact inverse form of the Linux M32 instruction above,
    // with r9 as the general-register destination.
    const uint64_t rawFromCR = 0x2121524240ULL;
    const InstructionEx fromCR = decoder.DecodeSlot(
        rawFromCR, UnitType::M_UNIT, 0x047f7db0);
    assert_true("indirect mov-from-CR should decode",
                fromCR.GetType() == InstructionType::MOV_FROM_CR);
    assert_equal("mov-from-CR destination register", 9, fromCR.GetDst());
    assert_equal("mov-from-CR selector", 21, fromCR.GetSrc1());
    assert_string("mov-from-CR disassembly",
                  "mov r9 = cr[r21]",
                  fromCR.GetDisassembly());

    cpu.SetCR(21, 0xfedcba9876543210ULL);
    fromCR.Execute(cpu, memory);
    assert_equal("mov-from-CR should copy the selected control register",
                 0xfedcba9876543210ULL,
                 cpu.GetGR(9));

    std::cout << "  ? IA-64 indirect control-register moves passed" << std::endl;
}

void test_ia64_translation_register_inserts() {
    std::cout << "Testing IA-64 translation-register inserts..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact Linux kernel entry instructions at guest addresses 0x047f7df6
    // and 0x047f7e00.  Retained Binutils 2.19.1 disassembles these raw slots
    // as itr.i itr[r16]=r18 and itr.d dtr[r16]=r18.
    const uint64_t rawItrI = 0x2079024000ULL;
    const InstructionEx itrI = decoder.DecodeSlot(
        rawItrI, UnitType::M_UNIT, 0x047f7df0);
    assert_true("Linux entry itr.i should decode",
                itrI.GetType() == InstructionType::ITR_I);
    assert_equal("itr.i selector register", 16, itrI.GetDst());
    assert_equal("itr.i physical-address register", 18, itrI.GetSrc1());
    assert_equal("itr.i qualifying predicate", 0, itrI.GetPredicate());
    assert_string("itr.i disassembly",
                  "itr.i itr[r16] = r18",
                  itrI.GetDisassembly());

    const uint64_t rawItrD = 0x2071024000ULL;
    const InstructionEx itrD = decoder.DecodeSlot(
        rawItrD, UnitType::M_UNIT, 0x047f7e00);
    assert_true("Linux entry itr.d should decode",
                itrD.GetType() == InstructionType::ITR_D);
    assert_equal("itr.d selector register", 16, itrD.GetDst());
    assert_equal("itr.d physical-address register", 18, itrD.GetSrc1());
    assert_string("itr.d disassembly",
                  "itr.d dtr[r16] = r18",
                  itrD.GetDisassembly());

    cpu.SetGR(16, 0);
    cpu.SetGR(18, 0x12345000ULL);
    cpu.SetCR(21, 0x68ULL);
    cpu.SetRR(5, 0xfeedfaceULL);
    cpu.SetCR(20, (5ULL << 61) | 0x1000ULL);
    itrI.Execute(cpu, memory);
    assert_true("itr.i should mark the selected ITR valid",
                cpu.GetITR(0).valid);
    assert_equal("itr.i should retain the physical-address operand",
                 0x12345000ULL, cpu.GetITR(0).physicalAddress);
    assert_equal("itr.i should retain CR.IFA",
                 (5ULL << 61) | 0x1000ULL,
                 cpu.GetITR(0).virtualAddress);
    assert_equal("itr.i should retain CR.ITIR",
                 0x68ULL, cpu.GetITR(0).itir);
    assert_equal("itr.i should retain the selected RR",
                 0xfeedfaceULL, cpu.GetITR(0).regionValue);

    itrD.Execute(cpu, memory);
    assert_true("itr.d should mark the selected DTR valid",
                cpu.GetDTR(0).valid);
    assert_equal("itr.d should retain the physical-address operand",
                 0x12345000ULL, cpu.GetDTR(0).physicalAddress);
    assert_equal("itr.d should retain CR.IFA",
                 (5ULL << 61) | 0x1000ULL,
                 cpu.GetDTR(0).virtualAddress);

    std::cout << "  ? IA-64 translation-register inserts passed" << std::endl;
}

void test_memory_bounds_throw() {
    std::cout << "Testing memory bounds diagnostics..." << std::endl;

    Memory memory(0x1000);
    uint64_t value = 0;
    bool threw = false;

    try {
        memory.Read(0x1000, reinterpret_cast<uint8_t*>(&value), sizeof(value));
    } catch (const std::out_of_range& ex) {
        threw = std::string(ex.what()).find("out of bounds") != std::string::npos;
    }
    assert_true("Out-of-range read should throw instead of asserting", threw);

    threw = false;
    try {
        memory.Read(0xffc, reinterpret_cast<uint8_t*>(&value), sizeof(value));
    } catch (const std::out_of_range& ex) {
        threw = std::string(ex.what()).find("exceeds bounds") != std::string::npos;
    }
    assert_true("Overlapping read should throw instead of asserting", threw);

    std::cout << "  ? Memory bounds diagnostics passed" << std::endl;
}

void test_application_register_moves() {
    std::cout << "Testing application register moves..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    InstructionEx mov_to_pfs = decoder.DecodeSlot(0x15404c000ULL, UnitType::M_UNIT, 0x306e0);
    assert_true("Boot raw mov-to-ar.pfs should decode in M-unit",
                mov_to_pfs.GetType() == InstructionType::MOV_TO_AR);
    assert_equal("mov-to-ar.pfs application register", 64, mov_to_pfs.GetDst());
    assert_equal("mov-to-ar.pfs source register", 38, mov_to_pfs.GetSrc1());
    assert_string("mov-to-ar.pfs disassembly",
                  "mov ar.pfs = r38",
                  mov_to_pfs.GetDisassembly());

    cpu.SetCFM(0x2200);
    cpu.SetGR(38, 0x12345);
    mov_to_pfs.Execute(cpu, memory);
    assert_equal("mov-to-ar.pfs should not change CFM", 0x2200, cpu.GetCFM());
    assert_equal("mov-to-ar.pfs should update AR storage", 0x12345, cpu.GetAR(64));

    InstructionEx mov_from_pfs(InstructionType::MOV_FROM_AR, UnitType::I_UNIT);
    mov_from_pfs.SetOperands(37, 64, 0);
    cpu.SetAR(64, 0x45678);
    mov_from_pfs.Execute(cpu, memory);
    assert_equal("mov-from-ar.pfs should read AR.PFS", 0x45678, cpu.GetGR(37));
    assert_equal("mov-from-ar.pfs should leave CFM independent", 0x2200, cpu.GetCFM());

    InstructionEx mov_to_pfs_i = decoder.DecodeSlot(0x15404a000ULL, UnitType::I_UNIT, 0x35400);
    assert_true("Boot raw mov-to-ar.pfs should decode in I-unit",
                mov_to_pfs_i.GetType() == InstructionType::MOV_TO_AR);
    assert_equal("mov-to-ar.pfs I-unit source register", 37, mov_to_pfs_i.GetSrc1());

    InstructionEx authentic_mov_to_pr = decoder.DecodeSlot(
        0x60002c700ULL, UnitType::I_UNIT, 0x2f0c0);
    assert_true("Authentic memcpy_long mov-to-predicate should decode",
                authentic_mov_to_pr.GetType() == InstructionType::MOV_TO_PR);
    assert_equal("Authentic mov-to-predicate source register", 22,
                 authentic_mov_to_pr.GetSrc1());
    assert_equal("Authentic mov-to-predicate mask", 0x38,
                 authentic_mov_to_pr.GetImmediate());
    assert_string("Authentic mov-to-predicate disassembly",
                  "mov pr = r22, 0x38",
                  authentic_mov_to_pr.GetDisassembly());

    cpu.SetGR(22, 0);
    cpu.SetPR(3, false);
    cpu.SetPR(4, false);
    cpu.SetPR(5, true);
    authentic_mov_to_pr.Execute(cpu, memory);
    assert_true("Authentic mov-to-predicate should keep PR0 true", cpu.GetPR(0));
    assert_true("Authentic mov-to-predicate should clear PR3", !cpu.GetPR(3));
    assert_true("Authentic mov-to-predicate should clear PR4", !cpu.GetPR(4));
    assert_true("Authentic mov-to-predicate should clear PR5", !cpu.GetPR(5));

    InstructionEx zero_mask_mov_to_pr = decoder.DecodeSlot(
        build_mov_to_pr_slot(0, 0), UnitType::I_UNIT, 0x2f0c0);
    assert_true("Zero-mask mov-to-predicate should decode",
                zero_mask_mov_to_pr.GetType() == InstructionType::MOV_TO_PR);
    assert_equal("Zero-mask mov-to-predicate immediate", 0,
                 zero_mask_mov_to_pr.GetImmediate());

    InstructionEx patterned_mov_to_pr = decoder.DecodeSlot(
        build_mov_to_pr_slot(37, 0x1234), UnitType::I_UNIT, 0x2f0c0);
    assert_equal("Patterned mov-to-predicate source register", 37,
                 patterned_mov_to_pr.GetSrc1());
    assert_equal("Patterned mov-to-predicate mask", 0x1234,
                 patterned_mov_to_pr.GetImmediate());

    InstructionEx sign_boundary_mov_to_pr = decoder.DecodeSlot(
        build_mov_to_pr_slot(63, 0x1fffe), UnitType::I_UNIT, 0x2f0c0);
    assert_equal("Sign-boundary mov-to-predicate source register", 63,
                 sign_boundary_mov_to_pr.GetSrc1());
    assert_equal("Sign-boundary mov-to-predicate mask",
                 0xfffffffffffffffeULL,
                 sign_boundary_mov_to_pr.GetImmediate());

    InstructionEx predicated_mov_to_pr = decoder.DecodeSlot(
        build_mov_to_pr_slot(22, 0x38, 1), UnitType::I_UNIT, 0x2f0c0);
    cpu.SetGR(22, (1ULL << 3) | (1ULL << 4) | (1ULL << 5));
    cpu.SetPR(1, false);
    cpu.SetPR(3, false);
    cpu.SetPR(4, false);
    cpu.SetPR(5, false);
    predicated_mov_to_pr.Execute(cpu, memory);
    assert_true("False-qualified mov-to-predicate should not write PR3",
                !cpu.GetPR(3));
    cpu.SetPR(1, true);
    predicated_mov_to_pr.Execute(cpu, memory);
    assert_true("True-qualified mov-to-predicate should write PR3", cpu.GetPR(3));
    assert_true("True-qualified mov-to-predicate should write PR4", cpu.GetPR(4));
    assert_true("True-qualified mov-to-predicate should write PR5", cpu.GetPR(5));

    InstructionEx mov_to_pr = decoder.DecodeSlot(0x16ff04bfc0ULL, UnitType::I_UNIT, 0x2f0c0);
    assert_true("Boot raw mov-to-predicate should decode",
                mov_to_pr.GetType() == InstructionType::MOV_TO_PR);
    assert_equal("mov-to-predicate source register", 37, mov_to_pr.GetSrc1());
    assert_equal("mov-to-predicate mask", 0xfffffffffffffffeULL, mov_to_pr.GetImmediate());
    assert_string("mov-to-predicate disassembly",
                  "mov pr = r37, 0xfffffffffffffffe",
                  mov_to_pr.GetDisassembly());

    cpu.SetGR(37, (1ULL << 1) | (1ULL << 16) | (1ULL << 63));
    cpu.SetPR(2, true);
    cpu.SetPR(10, true);
    mov_to_pr.Execute(cpu, memory);
    assert_true("mov-to-predicate should keep PR0 true", cpu.GetPR(0));
    assert_true("mov-to-predicate should set PR1 from source bit", cpu.GetPR(1));
    assert_true("mov-to-predicate should clear PR2 from source bit", !cpu.GetPR(2));
    assert_true("mov-to-predicate should clear PR10 from source bit", !cpu.GetPR(10));
    assert_true("mov-to-predicate should set rotating PR16", cpu.GetPR(16));
    assert_true("mov-to-predicate should set high rotating predicate", cpu.GetPR(63));

    InstructionEx mov_from_ip = decoder.DecodeSlot(build_mov_from_ip_slot(0, 11), UnitType::I_UNIT, 0x2f000);
    assert_true("mov from ip should decode", mov_from_ip.GetType() == InstructionType::MOV_FROM_IP);
    assert_equal("mov from ip destination register", 11, mov_from_ip.GetDst());
    assert_string("mov from ip disassembly",
                  "mov r11 = ip",
                  mov_from_ip.GetDisassembly());

    cpu.SetIP(0x123456789abcdef0ULL);
    mov_from_ip.Execute(cpu, memory);
    assert_equal("mov from ip should copy the current IP", 0x123456789abcdef0ULL, cpu.GetGR(11));

    InstructionEx mov_from_pr = decoder.DecodeSlot(build_mov_from_pr_slot(0, 12), UnitType::I_UNIT, 0x2f010);
    assert_true("mov from pr should decode", mov_from_pr.GetType() == InstructionType::MOV_FROM_PR);
    assert_equal("mov from pr destination register", 12, mov_from_pr.GetDst());
    assert_string("mov from pr disassembly",
                  "mov r12 = pr",
                  mov_from_pr.GetDisassembly());

    cpu.SetPR(1, true);
    cpu.SetPR(16, true);
    cpu.SetPR(63, true);
    mov_from_pr.Execute(cpu, memory);
    assert_equal("mov from pr should pack predicate bits into a GR",
                 0x8000000000010003ULL, cpu.GetGR(12));

    InstructionEx filler_m_nop = decoder.DecodeSlot(0x2b86ULL, UnitType::M_UNIT, 0x42008);
    assert_true("Final-loop predicated M nop should decode",
                filler_m_nop.GetType() == InstructionType::NOP);
    assert_equal("Final-loop M nop predicate", 6, filler_m_nop.GetPredicate());

    InstructionEx filler_i_nop = decoder.DecodeSlot(0x0ULL, UnitType::I_UNIT, 0x42008);
    assert_true("Final-loop zero I nop should decode",
                filler_i_nop.GetType() == InstructionType::NOP);

    std::cout << "  ? Application register moves passed" << std::endl;
}

void test_test_instructions() {
    std::cout << "Testing test-bit/test-NaT instructions..." << std::endl;

    CPUState cpu;
    Memory memory(1024 * 1024);

    cpu.SetGR(10, 0x20);

    InstructionEx tbit_z(InstructionType::TBIT_Z, UnitType::I_UNIT);
    tbit_z.SetOperands4(1, 10, 0, 2);
    tbit_z.SetImmediate(5);
    tbit_z.Execute(cpu, memory);
    assert_true("TBIT.Z: p1 should be false when selected bit is one", !cpu.GetPR(1));
    assert_true("TBIT.Z: p2 should be true when selected bit is one", cpu.GetPR(2));

    InstructionEx tbit_nz(InstructionType::TBIT_NZ, UnitType::I_UNIT);
    tbit_nz.SetOperands4(3, 10, 0, 4);
    tbit_nz.SetImmediate(5);
    tbit_nz.Execute(cpu, memory);
    assert_true("TBIT.NZ: p3 should be true when selected bit is one", cpu.GetPR(3));
    assert_true("TBIT.NZ: p4 should be false when selected bit is one", !cpu.GetPR(4));

    cpu.SetGR(11, 0x1234);
    cpu.SetGRNaT(11, false);

    InstructionEx tnat_z(InstructionType::TNAT_Z, UnitType::I_UNIT);
    tnat_z.SetOperands4(5, 11, 0, 6);
    tnat_z.Execute(cpu, memory);
    assert_true("TNAT.Z: p5 should be true for non-NaT register", cpu.GetPR(5));
    assert_true("TNAT.Z: p6 should be false for non-NaT register", !cpu.GetPR(6));

    cpu.SetGRNaT(11, true);
    InstructionEx tnat_nz(InstructionType::TNAT_NZ, UnitType::I_UNIT);
    tnat_nz.SetOperands4(7, 11, 0, 8);
    tnat_nz.Execute(cpu, memory);
    assert_true("TNAT.NZ: p7 should be true for NaT register", cpu.GetPR(7));
    assert_true("TNAT.NZ: p8 should be false for NaT register", !cpu.GetPR(8));

    InstructionDecoder decoder;
    InstructionEx decoded_tbit = decoder.DecodeSlot(build_tbit_z_slot(0, 9, 10, 10, 5),
                                                    UnitType::I_UNIT, 0);
    assert_true("TBIT.Z slot should decode", decoded_tbit.GetType() == InstructionType::TBIT_Z);
    assert_equal("TBIT.Z p1 decode", 9, decoded_tbit.GetDst());
    assert_equal("TBIT.Z source decode", 10, decoded_tbit.GetSrc1());
    assert_equal("TBIT.Z p2 decode", 10, decoded_tbit.GetSrc3());
    assert_equal("TBIT.Z position decode", 5, decoded_tbit.GetImmediate());

    InstructionEx decoded_tnat = decoder.DecodeSlot(build_tnat_z_slot(0, 11, 12, 11),
                                                    UnitType::I_UNIT, 0);
    assert_true("TNAT.Z slot should decode", decoded_tnat.GetType() == InstructionType::TNAT_Z);
    assert_equal("TNAT.Z p1 decode", 11, decoded_tnat.GetDst());
    assert_equal("TNAT.Z source decode", 11, decoded_tnat.GetSrc1());
    assert_equal("TNAT.Z p2 decode", 12, decoded_tnat.GetSrc3());

    // Exact Debian ELILO instruction at bundle-relative IP 0x9ea0.
    // Binutils disassembles raw 0xb230e001c0 as:
    //   (p0) tbit.z.or.andcm p7,p6=r14,0
    const uint64_t eliloParallelTbit = 0xb230e001c0ULL;
    InstructionEx decoded_parallel_tbit = decoder.DecodeSlot(
        eliloParallelTbit, UnitType::I_UNIT, 0x9ea6);
    assert_true("ELILO parallel TBIT should decode",
                decoded_parallel_tbit.GetType() == InstructionType::TBIT_Z);
    assert_equal("ELILO parallel TBIT qualifying predicate", 0,
                 decoded_parallel_tbit.GetPredicate());
    assert_equal("ELILO parallel TBIT p1", 7, decoded_parallel_tbit.GetDst());
    assert_equal("ELILO parallel TBIT source", 14, decoded_parallel_tbit.GetSrc1());
    assert_equal("ELILO parallel TBIT p2", 6, decoded_parallel_tbit.GetSrc3());
    assert_equal("ELILO parallel TBIT position", 0,
                 decoded_parallel_tbit.GetImmediate());
    assert_true("ELILO parallel TBIT completer",
                decoded_parallel_tbit.GetCompareCompleter() ==
                    CompareCompleter::OR_ANDCM);
    assert_string("ELILO parallel TBIT disassembly",
                  "tbit.z.or.andcm p7, p6 = r14, 0",
                  decoded_parallel_tbit.GetDisassembly());

    cpu.SetGR(14, 0x1);
    cpu.SetPR(7, false);
    cpu.SetPR(6, true);
    decoded_parallel_tbit.Execute(cpu, memory);
    assert_true("ELILO parallel TBIT false result preserves p7", !cpu.GetPR(7));
    assert_true("ELILO parallel TBIT false result preserves p6", cpu.GetPR(6));

    cpu.SetGR(14, 0x0);
    decoded_parallel_tbit.Execute(cpu, memory);
    assert_true("ELILO parallel TBIT true result sets p7", cpu.GetPR(7));
    assert_true("ELILO parallel TBIT true result clears p6", !cpu.GetPR(6));

    std::cout << "  ? Test instructions passed" << std::endl;
}

// Test bitwise operations
void test_bitwise_operations() {
    std::cout << "Testing bitwise operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    cpu.SetGR(1, 0xFF00);
    cpu.SetGR(2, 0x0F0F);
    
    // Test AND
    InstructionEx and_insn(InstructionType::AND, UnitType::I_UNIT);
    and_insn.SetOperands(3, 1, 2);
    and_insn.Execute(cpu, memory);
    assert_equal("AND", 0x0F00, cpu.GetGR(3));
    
    // Test OR
    InstructionEx or_insn(InstructionType::OR, UnitType::I_UNIT);
    or_insn.SetOperands(4, 1, 2);
    or_insn.Execute(cpu, memory);
    assert_equal("OR", 0xFF0F, cpu.GetGR(4));
    
    // Test XOR
    InstructionEx xor_insn(InstructionType::XOR, UnitType::I_UNIT);
    xor_insn.SetOperands(5, 1, 2);
    xor_insn.Execute(cpu, memory);
    assert_equal("XOR", 0xF00F, cpu.GetGR(5));
    
    // Test ANDCM (AND complement)
    InstructionEx andcm_insn(InstructionType::ANDCM, UnitType::I_UNIT);
    andcm_insn.SetOperands(6, 1, 2);
    andcm_insn.Execute(cpu, memory);
    assert_equal("ANDCM", 0xF000, cpu.GetGR(6));
    
    std::cout << "  ? Bitwise operations passed" << std::endl;
}

void test_ia64_immediate_andcm() {
    std::cout << "Testing IA-64 A3 immediate ANDCM encoding and execution..." << std::endl;

    InstructionDecoder decoder;
    Memory memory(1024 * 1024);

    // Binutils opcodes/ia64-opc-a.c uses:
    //   OpX2aVeX4X2b(8, 0, 0, 0xb, 1), {R1, IMM8, R3}
    // IMM8 is signed, with imm7a in bits 13:19 and s in bit 36.
    auto makeRaw = [](uint8_t qp, uint8_t dst, uint8_t src, int immediate) {
        assert_true("ANDCM immediate must fit signed 8-bit field",
                    immediate >= -128 && immediate <= 127);
        const uint8_t encoded = static_cast<uint8_t>(immediate);
        uint64_t raw = static_cast<uint64_t>(qp & 0x3f) |
                       (static_cast<uint64_t>(dst & 0x7f) << 6) |
                       (static_cast<uint64_t>(encoded & 0x7f) << 13) |
                       (static_cast<uint64_t>(src & 0x7f) << 20) |
                       (1ULL << 27) | (0xbULL << 29) | (8ULL << 37);
        raw |= static_cast<uint64_t>((encoded >> 7) & 0x1) << 36;
        return raw;
    };

    // Authentic Debian /linux instruction at IP 0x25620, slot 1.  Binutils
    // disassembles this raw slot as: andcm r9=-1,r32.
    const uint64_t authenticRaw = 0x1116a0fe240ULL;
    InstructionEx authentic = decoder.DecodeSlot(authenticRaw, UnitType::I_UNIT, 0x25620);
    assert_true("authentic immediate ANDCM should decode",
                authentic.GetType() == InstructionType::ANDCM_IMM);
    assert_equal("authentic ANDCM destination", 9, authentic.GetDst());
    assert_equal("authentic ANDCM source register", 32, authentic.GetSrc2());
    assert_equal("authentic ANDCM predicate", 0, authentic.GetPredicate());
    assert_equal("authentic ANDCM immediate", static_cast<uint64_t>(-1),
                 authentic.GetImmediate());
    assert_string("authentic ANDCM disassembly",
                  "andcm r9 = -1, r32", authentic.GetDisassembly());

    CPUState cpu;
    cpu.SetGR(9, 0x0123456789abcdefULL);
    cpu.SetGR(32, 0);
    authentic.Execute(cpu, memory);
    assert_equal("authentic ANDCM result", 0xffffffffffffffffULL, cpu.GetGR(9));

    // These raw slots are the assembled fixtures from
    // .codex_tmp/andcm-immediate-fixtures.s, cross-checked with:
    //   .codex_tmp/ia64_toolchains/build-binutils/binutils/.libs/objdump.exe -d -Mintel .codex_tmp/andcm-immediate-fixtures.o
    struct Fixture {
        uint64_t raw;
        uint8_t dst;
        uint8_t src;
        int immediate;
        const char* disassembly;
    };
    const Fixture fixtures[] = {
        {0x1016a100280ULL, 10, 33, 0, "andcm r10 = 0, r33"},
        {0x1016a2022c0ULL, 11, 34, 1, "andcm r11 = 1, r34"},
        {0x1016a3fe300ULL, 12, 35, 127, "andcm r12 = 127, r35"},
        {0x1116a400340ULL, 13, 36, -128, "andcm r13 = -128, r36"},
        {0x1116a5fc386ULL, 14, 37, -2, "andcm r14 = -2, r37"},
    };
    for (const Fixture& fixture : fixtures) {
        InstructionEx instruction = decoder.DecodeSlot(
            fixture.raw, UnitType::I_UNIT, 0);
        assert_true("assembled immediate ANDCM should decode",
                    instruction.GetType() == InstructionType::ANDCM_IMM);
        assert_equal("assembled ANDCM destination", fixture.dst, instruction.GetDst());
        assert_equal("assembled ANDCM source register", fixture.src, instruction.GetSrc2());
        assert_equal("assembled ANDCM immediate",
                     static_cast<uint64_t>(static_cast<int64_t>(fixture.immediate)),
                     instruction.GetImmediate());
        assert_string("assembled ANDCM disassembly",
                      fixture.disassembly, instruction.GetDisassembly());
    }

    // A non-commutative check: imm & ~source differs from source & ~imm.
    InstructionEx nontrivial = decoder.DecodeSlot(
        makeRaw(0, 20, 21, 0x55), UnitType::I_UNIT, 0);
    cpu.SetGR(21, 0xaa);
    nontrivial.Execute(cpu, memory);
    assert_equal("ANDCM immediate operand order", 0x55, cpu.GetGR(20));

    // A false qualifying predicate suppresses the write.
    InstructionEx predicated = decoder.DecodeSlot(
        makeRaw(6, 22, 23, -1), UnitType::I_UNIT, 0);
    cpu.SetGR(22, 0xfeedfacecafebeefULL);
    cpu.SetGR(23, 0);
    cpu.SetPR(6, false);
    predicated.Execute(cpu, memory);
    assert_equal("ANDCM immediate false predicate preserves destination",
                 0xfeedfacecafebeefULL, cpu.GetGR(22));

    // The nearby x2b=0 encoding is AND immediate, not ANDCM immediate.
    InstructionEx adjacentAnd = decoder.DecodeSlot(
        0x101621fe280ULL, UnitType::I_UNIT, 0);
    assert_true("adjacent AND immediate remains distinct",
                adjacentAnd.GetType() == InstructionType::AND_IMM);
    assert_string("adjacent AND immediate disassembly",
                  "and r10 = 127, r33", adjacentAnd.GetDisassembly());

    // Writes to r0 remain architecturally suppressed.
    InstructionEx writeR0 = decoder.DecodeSlot(
        makeRaw(0, 0, 24, 1), UnitType::I_UNIT, 0);
    cpu.SetGR(24, 0);
    writeR0.Execute(cpu, memory);
    assert_equal("ANDCM immediate destination r0 remains zero", 0, cpu.GetGR(0));

    std::cout << "  ? IA-64 A3 immediate ANDCM encoding and execution passed" << std::endl;
}

// Test subtract operations
void test_subtract_operations() {
    std::cout << "Testing subtract operations..." << std::endl;

    CPUState cpu;
    Memory memory(1024 * 1024);

    cpu.SetGR(19, 9);
    cpu.SetGR(20, 3);

    InstructionEx sub_reg(InstructionType::SUB, UnitType::I_UNIT);
    sub_reg.SetOperands(19, 19, 20);
    sub_reg.Execute(cpu, memory);
    assert_equal("SUB register", 6, cpu.GetGR(19));

    cpu.SetGR(19, 9);
    InstructionEx sub_imm(InstructionType::SUB_IMM, UnitType::I_UNIT);
    sub_imm.SetOperands(19, 0, 19);
    sub_imm.SetImmediate(3);
    sub_imm.Execute(cpu, memory);
    assert_equal("SUB immediate", static_cast<uint64_t>(-6), cpu.GetGR(19));

    std::cout << "  ? Subtract operations passed" << std::endl;
}

void test_ia64_immediate_sub_raw_encoding() {
    std::cout << "Testing IA-64 A3 immediate SUB raw encoding..." << std::endl;

    InstructionDecoder decoder;
    Memory memory(1024 * 1024);

    const uint64_t rawSub = 0x10129110407ULL;
    InstructionEx actual = decoder.DecodeSlot(rawSub, UnitType::I_UNIT, 0x2c1e0);
    assert_true("raw immediate SUB should decode",
                actual.GetType() == InstructionType::SUB_IMM);
    assert_equal("raw immediate SUB predicate", 7, actual.GetPredicate());
    assert_equal("raw immediate SUB destination", 16, actual.GetDst());
    assert_equal("raw immediate SUB source r3", 17, actual.GetSrc2());
    assert_equal("raw immediate SUB immediate", 8, actual.GetImmediate());
    assert_string("raw immediate SUB disassembly",
                  "sub r16 = 8, r17",
                  actual.GetDisassembly());

    CPUState cpu;
    cpu.SetPR(7, true);
    cpu.SetGR(17, 0x32);
    actual.Execute(cpu, memory);
    assert_equal("raw immediate SUB result is imm minus r3",
                 static_cast<uint64_t>(-0x2a), cpu.GetGR(16));

    cpu.SetGR(16, 0x1122334455667788ULL);
    cpu.SetPR(7, false);
    actual.Execute(cpu, memory);
    assert_equal("raw immediate SUB respects predicate",
                 0x1122334455667788ULL, cpu.GetGR(16));

    auto makeRaw = [](uint8_t qp, uint8_t dst, uint8_t src, int immediate) {
        const uint8_t encoded = static_cast<uint8_t>(immediate);
        uint64_t raw = static_cast<uint64_t>(qp & 0x3f) |
                       (static_cast<uint64_t>(dst & 0x7f) << 6) |
                       (static_cast<uint64_t>(encoded & 0x7f) << 13) |
                       (static_cast<uint64_t>(src & 0x7f) << 20) |
                       (1ULL << 27) | (9ULL << 29) | (8ULL << 37);
        raw |= static_cast<uint64_t>((encoded >> 7) & 0x1) << 36;
        return raw;
    };

    auto executeBoundary = [&](const char* name,
                                uint8_t dst,
                                uint8_t src,
                                int immediate,
                                uint64_t sourceValue,
                                uint64_t expected) {
        InstructionEx instruction = decoder.DecodeSlot(
            makeRaw(0, dst, src, immediate), UnitType::I_UNIT, 0);
        assert_true(name, instruction.GetType() == InstructionType::SUB_IMM);
        assert_equal("immediate SUB boundary source", src, instruction.GetSrc2());
        assert_equal("immediate SUB boundary sign extension",
                     static_cast<uint64_t>(static_cast<int64_t>(immediate)),
                     instruction.GetImmediate());
        cpu.SetGR(src, sourceValue);
        cpu.SetGR(dst, 0);
        instruction.Execute(cpu, memory);
        assert_equal(name, expected, cpu.GetGR(dst));
    };

    executeBoundary("immediate SUB zero / 0-minus-x", 5, 6, 0, 5,
                     static_cast<uint64_t>(-5));
    executeBoundary("immediate SUB maximum positive immediate", 7, 8, 127, 1, 126);
    executeBoundary("immediate SUB minimum negative immediate", 9, 10, -128, 0,
                    static_cast<uint64_t>(-128));

    InstructionEx writeR0 = decoder.DecodeSlot(makeRaw(0, 0, 11, 1), UnitType::I_UNIT, 0);
    cpu.SetGR(11, 9);
    writeR0.Execute(cpu, memory);
    assert_equal("immediate SUB destination r0 remains zero", 0, cpu.GetGR(0));

    std::cout << "  ? IA-64 A3 immediate SUB raw encoding passed" << std::endl;
}

void test_ia64_sub_minus_one_raw_encoding() {
    std::cout << "Testing IA-64 A1 three-input SUB raw encoding..." << std::endl;

    InstructionDecoder decoder;
    Memory memory(1024 * 1024);

    // Authentic gzip huft_build instruction at IP 0x219e0, slot 2.
    const uint64_t rawSubM1 = 0x10020e20640ULL;
    InstructionEx subM1 = decoder.DecodeSlot(rawSubM1, UnitType::I_UNIT, 0x219e0);
    assert_true("raw SUB ...,1 should decode",
                subM1.GetType() == InstructionType::SUB_M1);
    assert_equal("raw SUB ...,1 destination", 25, subM1.GetDst());
    assert_equal("raw SUB ...,1 source 1", 16, subM1.GetSrc1());
    assert_equal("raw SUB ...,1 source 2", 14, subM1.GetSrc2());
    assert_string("raw SUB ...,1 disassembly",
                  "sub r25 = r16, r14, 1",
                  subM1.GetDisassembly());

    CPUState cpu;
    cpu.SetGR(16, 7);
    cpu.SetGR(14, 2);
    subM1.Execute(cpu, memory);
    assert_equal("raw SUB ...,1 result", 4, cpu.GetGR(25));

    const uint64_t rawSub = rawSubM1 | (1ULL << 27);
    InstructionEx plainSub = decoder.DecodeSlot(rawSub, UnitType::I_UNIT, 0x219e0);
    assert_true("ordinary raw SUB should remain distinct",
                plainSub.GetType() == InstructionType::SUB);
    cpu.SetGR(25, 0);
    plainSub.Execute(cpu, memory);
    assert_equal("ordinary raw SUB result", 5, cpu.GetGR(25));

    std::cout << "  ? IA-64 A1 three-input SUB raw encoding passed" << std::endl;
}

// Test shift operations
void test_shift_operations() {
    std::cout << "Testing shift operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    cpu.SetGR(1, 0x12345678);
    cpu.SetGR(2, 4);
    
    // Test SHL
    InstructionEx shl(InstructionType::SHL, UnitType::I_UNIT);
    shl.SetOperands(3, 1, 2);
    shl.Execute(cpu, memory);
    assert_equal("SHL", 0x123456780ULL, cpu.GetGR(3));
    
    // Test SHR (logical)
    InstructionEx shr(InstructionType::SHR, UnitType::I_UNIT);
    shr.SetOperands(4, 1, 2);
    shr.Execute(cpu, memory);
    assert_equal("SHR", 0x01234567ULL, cpu.GetGR(4));
    
    // Test SHRA (arithmetic)
    cpu.SetGR(5, 0x8000000000000000ULL);  // Negative number
    cpu.SetGR(6, 4);
    InstructionEx shra(InstructionType::SHRA, UnitType::I_UNIT);
    shra.SetOperands(7, 5, 6);
    shra.Execute(cpu, memory);
    assert_equal("SHRA", 0xF800000000000000ULL, cpu.GetGR(7));
    
    // Test SHLADD
    cpu.SetGR(8, 10);
    cpu.SetGR(9, 100);
    InstructionEx shladd(InstructionType::SHLADD, UnitType::I_UNIT);
    shladd.SetOperands(10, 8, 9);
    shladd.SetImmediate(2);  // Shift by 2
    shladd.Execute(cpu, memory);
    assert_equal("SHLADD", 140, cpu.GetGR(10));  // (10 << 2) + 100 = 40 + 100
    
    std::cout << "  ? Shift operations passed" << std::endl;
}

// Test extract/deposit operations
void test_extract_deposit() {
    std::cout << "Testing extract/deposit operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Test ZXT (zero extend)
    cpu.SetGR(1, 0xFFFFFFFFFFFFFF80ULL);
    
    InstructionEx zxt1(InstructionType::ZXT1, UnitType::I_UNIT);
    zxt1.SetOperands(2, 1, 0);
    zxt1.Execute(cpu, memory);
    assert_equal("ZXT1", 0x80ULL, cpu.GetGR(2));
    
    InstructionEx zxt2(InstructionType::ZXT2, UnitType::I_UNIT);
    zxt2.SetOperands(3, 1, 0);
    zxt2.Execute(cpu, memory);
    assert_equal("ZXT2", 0xFF80ULL, cpu.GetGR(3));
    
    InstructionEx zxt4(InstructionType::ZXT4, UnitType::I_UNIT);
    zxt4.SetOperands(4, 1, 0);
    zxt4.Execute(cpu, memory);
    assert_equal("ZXT4", 0xFFFFFF80ULL, cpu.GetGR(4));
    
    // Test SXT (sign extend)
    cpu.SetGR(5, 0x80);  // Negative byte
    
    InstructionEx sxt1(InstructionType::SXT1, UnitType::I_UNIT);
    sxt1.SetOperands(6, 5, 0);
    sxt1.Execute(cpu, memory);
    assert_equal("SXT1", 0xFFFFFFFFFFFFFF80ULL, cpu.GetGR(6));
    
    cpu.SetGR(7, 0x8000);  // Negative word
    InstructionEx sxt2(InstructionType::SXT2, UnitType::I_UNIT);
    sxt2.SetOperands(8, 7, 0);
    sxt2.Execute(cpu, memory);
    assert_equal("SXT2", 0xFFFFFFFFFFFF8000ULL, cpu.GetGR(8));
    
    std::cout << "  ? Extract/deposit operations passed" << std::endl;
}

// Test memory operations
void test_memory_operations() {
    std::cout << "Testing memory operations..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    uint64_t base_addr = 0x1000;
    cpu.SetGR(1, base_addr);
    
    // Test ST1/LD1
    cpu.SetGR(2, 0x42);
    InstructionEx st1(InstructionType::ST1, UnitType::M_UNIT);
    st1.SetOperands(1, 2, 0);
    st1.Execute(cpu, memory);
    
    InstructionEx ld1(InstructionType::LD1, UnitType::M_UNIT);
    ld1.SetOperands(3, 1, 0);
    ld1.Execute(cpu, memory);
    assert_equal("LD1/ST1", 0x42, cpu.GetGR(3));
    
    // Test ST2/LD2
    cpu.SetGR(1, base_addr + 0x10);
    cpu.SetGR(4, 0x1234);
    InstructionEx st2(InstructionType::ST2, UnitType::M_UNIT);
    st2.SetOperands(1, 4, 0);
    st2.Execute(cpu, memory);
    
    InstructionEx ld2(InstructionType::LD2, UnitType::M_UNIT);
    ld2.SetOperands(5, 1, 0);
    ld2.Execute(cpu, memory);
    assert_equal("LD2/ST2", 0x1234, cpu.GetGR(5));
    
    // Test ST4/LD4
    cpu.SetGR(1, base_addr + 0x20);
    cpu.SetGR(6, 0x12345678);
    InstructionEx st4(InstructionType::ST4, UnitType::M_UNIT);
    st4.SetOperands(1, 6, 0);
    st4.Execute(cpu, memory);
    
    InstructionEx ld4(InstructionType::LD4, UnitType::M_UNIT);
    ld4.SetOperands(7, 1, 0);
    ld4.Execute(cpu, memory);
    assert_equal("LD4/ST4", 0x12345678, cpu.GetGR(7));
    
    // Test ST8/LD8
    cpu.SetGR(1, base_addr + 0x30);
    cpu.SetGR(8, 0x123456789ABCDEF0ULL);
    InstructionEx st8(InstructionType::ST8, UnitType::M_UNIT);
    st8.SetOperands(1, 8, 0);
    st8.Execute(cpu, memory);
    
    InstructionEx ld8(InstructionType::LD8, UnitType::M_UNIT);
    ld8.SetOperands(9, 1, 0);
    ld8.Execute(cpu, memory);
    assert_equal("LD8/ST8", 0x123456789ABCDEF0ULL, cpu.GetGR(9));
    
    std::cout << "  ? Memory operations passed" << std::endl;
}

// Test predicated execution
void test_predicated_execution() {
    std::cout << "Testing predicated execution..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    cpu.SetGR(1, 100);
    cpu.SetGR(2, 200);
    
    // Set predicate registers
    cpu.SetPR(1, true);
    cpu.SetPR(2, false);
    
    // Test with true predicate
    InstructionEx add1(InstructionType::ADD, UnitType::I_UNIT);
    add1.SetPredicate(1);
    add1.SetOperands(3, 1, 2);
    add1.Execute(cpu, memory);
    assert_equal("Predicated ADD (true)", 300, cpu.GetGR(3));
    
    // Test with false predicate
    InstructionEx add2(InstructionType::ADD, UnitType::I_UNIT);
    add2.SetPredicate(2);
    add2.SetOperands(4, 1, 2);
    add2.Execute(cpu, memory);
    assert_equal("Predicated ADD (false)", 0, cpu.GetGR(4));  // Should not execute
    
    std::cout << "  ? Predicated execution passed" << std::endl;
}

// Test ALLOC instruction
void test_alloc_instruction() {
    std::cout << "Testing ALLOC instruction..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Set initial CFM
    cpu.SetCFM(0x12345678);
    
    // ALLOC: sof=10, sol=5, sor=2
    // immediate = (sor << 14) | (sol << 7) | sof
    uint64_t imm = (2ULL << 14) | (5ULL << 7) | 10ULL;
    
    InstructionEx alloc(InstructionType::ALLOC, UnitType::I_UNIT);
    alloc.SetOperands(10, 0, 0);  // r10 = ar.pfs
    alloc.SetImmediate(imm);
    alloc.Execute(cpu, memory);
    
    // Check saved CFM
    assert_equal("ALLOC: saved CFM", 0x12345678, cpu.GetGR(10));
    assert_equal("ALLOC: ar.pfs should preserve previous frame state", 0x12345678, cpu.GetPFS());
    assert_equal("ALLOC: ar.pfs alias should update with CFM",
                 (2ULL << 14) | (5ULL << 7) | 10ULL,
                 cpu.GetRSEState().pfs);
    
    // Check new CFM fields
    uint64_t new_cfm = cpu.GetCFM();
    assert_equal("ALLOC: new SOF", 10, new_cfm & 0x7F);
    assert_equal("ALLOC: new SOL", 5, (new_cfm >> 7) & 0x7F);
    assert_equal("ALLOC: new SOR", 2, (new_cfm >> 14) & 0xF);
    assert_equal("ALLOC: explicit RSE SOF should track CFM", 10, cpu.GetRSEState().sof);
    assert_equal("ALLOC: explicit RSE SOL should track CFM", 5, cpu.GetRSEState().sol);
    assert_equal("ALLOC: explicit RSE SOR should track CFM", 2, cpu.GetRSEState().sor);
    
    std::cout << "  ? ALLOC instruction passed" << std::endl;
}

void test_rse_state_aliases() {
    std::cout << "Testing RSE state aliases..." << std::endl;

    CPUState cpu;

    cpu.SetRSC(0x11);
    cpu.SetBSP(0x80000000000ULL);
    cpu.SetBSPSTORE(0x80000000020ULL);
    cpu.SetRNAT(0xdeadbeef);
    cpu.SetPFS(0x1234 | (static_cast<uint64_t>(7) << 7) | (static_cast<uint64_t>(1) << 14));

    assert_equal("RSE: RSC alias", 0x11, cpu.GetRSC());
    assert_equal("RSE: BSP alias", 0x80000000000ULL, cpu.GetBSP());
    assert_equal("RSE: BSPSTORE alias", 0x80000000020ULL, cpu.GetBSPSTORE());
    assert_equal("RSE: RNAT alias", 0xdeadbeef, cpu.GetRNAT());
    assert_equal("RSE: PFS alias", 0x1234 | (static_cast<uint64_t>(7) << 7) | (static_cast<uint64_t>(1) << 14), cpu.GetPFS());
    assert_equal("RSE: explicit CFM remains independent from PFS", 0, cpu.GetCFM());
    assert_equal("RSE: frame size fields updated", 0x34, cpu.GetRSEState().sof);
    assert_equal("RSE: local size fields updated", 0x27, cpu.GetRSEState().sol);
    assert_equal("RSE: rotating size fields updated", 0x01, cpu.GetRSEState().sor);

    std::cout << "  ? RSE state aliases passed" << std::endl;
}

void test_ia64_flushrs() {
    std::cout << "Testing IA-64 M0 flushrs decoding and execution..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact authentic ELILO encoding at IP 0x28640.  Historical Binutils
    // identifies this M0 syllable as flushrs: major=0, x3=0, x4=0xc, x2=0.
    const uint64_t rawFlushrs = 0x60000000ULL;
    const InstructionEx flushrs = decoder.DecodeSlot(
        rawFlushrs, UnitType::M_UNIT, 0x28640);
    assert_true("authentic flushrs should decode",
                flushrs.GetType() == InstructionType::FLUSHRS);
    assert_equal("flushrs predicate", 0, flushrs.GetPredicate());
    assert_true("flushrs has no immediate", !flushrs.HasImmediate());
    assert_string("flushrs disassembly", "flushrs", flushrs.GetDisassembly());

    cpu.SetRSC(0x3);
    cpu.SetBSP(0x1000);
    cpu.SetBSPSTORE(0x0f80);
    cpu.SetRNAT(0x55);
    cpu.SetCFM(0x183);
    flushrs.Execute(cpu, memory);

    assert_equal("flushrs should advance BSPSTORE to BSP",
                 0x1000, cpu.GetBSPSTORE());
    assert_equal("flushrs should preserve BSP", 0x1000, cpu.GetBSP());
    assert_equal("flushrs should preserve RSC", 0x3, cpu.GetRSC());
    assert_equal("flushrs should preserve RNAT", 0x55, cpu.GetRNAT());
    assert_equal("flushrs should preserve CFM", 0x183, cpu.GetCFM());

    // The encoding is NO_PRED in Binutils.  A nonzero qp field is therefore
    // not another flushrs variant and must remain unsupported.
    const InstructionEx invalidPredicated = decoder.DecodeSlot(
        rawFlushrs | 1ULL, UnitType::M_UNIT, 0x28640);
    assert_true("predicated flushrs encoding should remain unknown",
                invalidPredicated.GetType() == InstructionType::UNKNOWN);

    const InstructionEx adjacentLoadrs = decoder.DecodeSlot(
        0x50000000ULL, UnitType::M_UNIT, 0x28640);
    assert_true("adjacent loadrs encoding should not alias flushrs",
                adjacentLoadrs.GetType() == InstructionType::UNKNOWN);

    cpu.SetBSP(0x2000);
    cpu.SetBSPSTORE(0x1800);
    cpu.SetPR(1, false);
    InstructionEx manuallyPredicated = flushrs;
    manuallyPredicated.SetPredicate(1);
    manuallyPredicated.Execute(cpu, memory);
    assert_equal("false predicate should nullify flushrs",
                 0x1800, cpu.GetBSPSTORE());

    std::cout << "  ? IA-64 flushrs decoding and execution passed" << std::endl;
}

void test_ia64_invala() {
    std::cout << "Testing IA-64 M0 invala decoding and ALAT invalidation..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact authentic ELILO encoding at IP 0x28810.  Historical Binutils
    // identifies this M24 complete-form syllable as invala:
    // major=0, x3=0, x4=0, x2=1.
    const uint64_t rawInvala = 0x80000000ULL;
    const InstructionEx invala = decoder.DecodeSlot(
        rawInvala, UnitType::M_UNIT, 0x28810);
    assert_true("authentic invala should decode",
                invala.GetType() == InstructionType::INVALA);
    assert_equal("invala predicate", 0, invala.GetPredicate());
    assert_true("invala has no immediate", !invala.HasImmediate());
    assert_string("invala disassembly", "invala", invala.GetDisassembly());

    // M24 is predicatable.  The same opcode with qp=1 must remain invala,
    // unlike the NO_PRED flushrs encoding.
    const InstructionEx predicatedInvala = decoder.DecodeSlot(
        rawInvala | 1ULL, UnitType::M_UNIT, 0x28810);
    assert_true("predicated invala should decode",
                predicatedInvala.GetType() == InstructionType::INVALA);
    assert_equal("predicated invala qp", 1, predicatedInvala.GetPredicate());

    const InstructionEx flushrs = decoder.DecodeSlot(
        0x60000000ULL, UnitType::M_UNIT, 0x28640);
    const InstructionEx loadrs = decoder.DecodeSlot(
        0x50000000ULL, UnitType::M_UNIT, 0x28640);
    assert_true("invala must not alias flushrs",
                invala.GetType() != flushrs.GetType());
    assert_true("adjacent loadrs encoding should remain unsupported",
                loadrs.GetType() == InstructionType::UNKNOWN);

    // Use nontrivial RSE and stacked-register state to make sure the ALAT
    // invalidation is not incorrectly implemented as an RSE reset.
    cpu.SetRSC(0x3);
    cpu.SetBSP(0x1000);
    cpu.SetBSPSTORE(0x0f80);
    cpu.SetRNAT(0x300905a4dULL);
    cpu.SetPFS(0x3);
    cpu.SetCFM(0x183);
    cpu.SetGR(32, 0x1122334455667788ULL);
    cpu.SetGRNaT(32, true);
    cpu.SetPR(1, false);
    invala.Execute(cpu, memory);

    // invala has no RSE side effects; all modeled state must be preserved.
    assert_equal("invala preserves RSC", 0x3, cpu.GetRSC());
    assert_equal("invala preserves BSP", 0x1000, cpu.GetBSP());
    assert_equal("invala preserves BSPSTORE", 0x0f80, cpu.GetBSPSTORE());
    assert_equal("invala preserves RNAT", 0x300905a4dULL, cpu.GetRNAT());
    assert_equal("invala preserves PFS", 0x3, cpu.GetPFS());
    assert_equal("invala preserves CFM", 0x183, cpu.GetCFM());
    assert_equal("invala preserves stacked register", 0x1122334455667788ULL,
                 cpu.GetGR(32));
    assert_true("invala preserves stacked-register NaT", cpu.GetGRNaT(32));

    // A false qualifying predicate nullifies invala and likewise leaves the
    // RSE state untouched.
    InstructionEx manuallyPredicated = invala;
    manuallyPredicated.SetPredicate(1);
    manuallyPredicated.Execute(cpu, memory);
    assert_equal("false predicate should nullify invala",
                 0x0f80, cpu.GetBSPSTORE());

    std::cout << "  ? IA-64 invala decoding and execution passed" << std::endl;
}

void test_alloc_invalid_frame_size_fails_safe() {
    std::cout << "Testing ALLOC invalid frame sizes..." << std::endl;

    CPUState cpu;
    Memory memory(1024 * 1024);
    cpu.SetCFM(0x100);

    InstructionEx alloc(InstructionType::ALLOC, UnitType::I_UNIT);
    alloc.SetOperands(10, 0, 0);
    alloc.SetImmediate((2ULL << 14) | (12ULL << 7) | 10ULL);

    bool threw = false;
    try {
        alloc.Execute(cpu, memory);
    } catch (const std::out_of_range& ex) {
        threw = std::string(ex.what()).find("ALLOC frame size invalid") != std::string::npos;
    }

    assert_true("ALLOC should reject sol > sof", threw);
    assert_equal("ALLOC invalid frame should leave CFM unchanged", 0x100, cpu.GetCFM());
    assert_equal("ALLOC invalid frame should leave ar.pfs unchanged", 0, cpu.GetPFS());

    std::cout << "  ? ALLOC invalid frame sizes fail safely" << std::endl;
}

// Test 32-bit compare instructions
void test_cmp4_instructions() {
    std::cout << "Testing CMP4 (32-bit compare) instructions..." << std::endl;
    
    CPUState cpu;
    Memory memory(1024 * 1024);
    
    // Use values that differ in upper 32 bits
    cpu.SetGR(1, 0x1000000000000064ULL);  // Upper bits differ
    cpu.SetGR(2, 0x2000000000000064ULL);  // Upper bits differ
    
    // CMP4 should only compare lower 32 bits
    InstructionEx cmp4_eq(InstructionType::CMP4_EQ, UnitType::I_UNIT);
    cmp4_eq.SetOperands4(1, 1, 2, 2);
    cmp4_eq.Execute(cpu, memory);
    
    assert_true("CMP4.EQ: p1 should be true (lower 32 bits equal)", cpu.GetPR(1));
    assert_true("CMP4.EQ: p2 should be false", !cpu.GetPR(2));
    
    std::cout << "  ? CMP4 instructions passed" << std::endl;
}

void test_ia64_unsigned_fixed_truncate_modulus_sequence() {
    std::cout << "Testing IA-64 FCVT.FXU.TRUNC modulus sequence..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    const InstructionEx convert = decoder.DecodeSlot(
        0x4d8014280ULL, UnitType::F_UNIT, 0x370e0);
    assert_true("raw FCVT.FXU.TRUNC should decode",
                convert.GetType() == InstructionType::FCVT_FXU);
    assert_equal("FCVT.FXU.TRUNC destination FP register", 10, convert.GetDst());
    assert_equal("FCVT.FXU.TRUNC source FP register", 10, convert.GetSrc1());
    assert_string("FCVT.FXU.TRUNC disassembly",
                  "fcvt.fxu.trunc.s1 f10 = f10",
                  convert.GetDisassembly());

    auto setFloatingValue = [&cpu](uint8_t reg, uint64_t significand,
                                   uint64_t signAndExponent) {
        uint8_t bytes[16] = {};
        for (int i = 0; i < 8; ++i) {
            bytes[i] = static_cast<uint8_t>((significand >> (i * 8)) & 0xff);
            bytes[8 + i] = static_cast<uint8_t>((signAndExponent >> (i * 8)) & 0xff);
        }
        cpu.SetFR(reg, bytes);
    };

    // 52.5 in register format: 0xd2 * 2^(0x10004 - 0x1003e) = 52.5.
    setFloatingValue(10, 0xd200000000000000ULL, 0x10004ULL);
    convert.Execute(cpu, memory);

    uint8_t converted[16] = {};
    cpu.GetFR(10, converted);
    uint64_t convertedSignificand = 0;
    uint64_t convertedSignAndExponent = 0;
    for (int i = 0; i < 8; ++i) {
        convertedSignificand |= static_cast<uint64_t>(converted[i]) << (i * 8);
        convertedSignAndExponent |= static_cast<uint64_t>(converted[8 + i]) << (i * 8);
    }
    assert_equal("FCVT.FXU.TRUNC should discard the fractional part", 0x34,
                 convertedSignificand);
    assert_equal("FCVT.FXU.TRUNC should produce integer-format exponent",
                 0x1003E, convertedSignAndExponent);

    const InstructionEx xma = decoder.DecodeSlot(
        0x1d048a1c280ULL, UnitType::F_UNIT, 0x370f0);
    assert_true("raw XMA.L modulus step should decode",
                xma.GetType() == InstructionType::XMA);
    assert_string("XMA.L modulus-step disassembly",
                  "xma.l f10 = f10, f9, f14",
                  xma.GetDisassembly());

    setFloatingValue(9, 0xfffffffffffffff6ULL, 0x1003EULL); // -10
    setFloatingValue(14, 525, 0x1003EULL);                  // original dividend
    xma.Execute(cpu, memory);
    cpu.GetFR(10, converted);
    convertedSignificand = 0;
    convertedSignAndExponent = 0;
    for (int i = 0; i < 8; ++i) {
        convertedSignificand |= static_cast<uint64_t>(converted[i]) << (i * 8);
        convertedSignAndExponent |= static_cast<uint64_t>(converted[8 + i]) << (i * 8);
    }
    assert_equal("XMA.L modulus step should compute dividend minus quotient*divisor",
                 5, convertedSignificand);
    assert_equal("XMA.L modulus step should retain integer-format exponent",
                 0x1003E, convertedSignAndExponent);

    std::cout << "  ? IA-64 FCVT.FXU.TRUNC modulus sequence passed" << std::endl;
}

void test_ia64_unknown_slot_formatter() {
    std::cout << "Testing IA-64 unknown-slot formatter..." << std::endl;

    const std::string msg = FormatIA64UnknownSlot(
        0x36e70,
        1,
        TemplateType::MII,
        UnitType::I_UNIT,
        0x1ULL,
        false);

    assert_true("unknown-slot formatter should include IP",
                msg.find("IP=0x36e70") != std::string::npos);
    assert_true("unknown-slot formatter should include slot index",
                msg.find("slot=1") != std::string::npos);
    assert_true("unknown-slot formatter should include template value",
                msg.find("template=0x0(MII)") != std::string::npos);
    assert_true("unknown-slot formatter should include slot type",
                msg.find("slotType=I") != std::string::npos);
    assert_true("unknown-slot formatter should include raw syllable",
                msg.find("raw41=0x1") != std::string::npos);
    assert_true("unknown-slot formatter should include major opcode",
                msg.find("major=0x") != std::string::npos);
    assert_true("unknown-slot formatter should include path",
                msg.find("path=normal") != std::string::npos);
    assert_true("unknown-slot formatter should include decoder family",
                msg.find("decoder=I-type") != std::string::npos);

    std::cout << "  ? IA-64 unknown-slot formatter passed" << std::endl;
}

void test_ia64_br_ctop_state_machine() {
    std::cout << "Testing IA-64 br.ctop state machine..." << std::endl;

    InstructionDecoder decoder;
    InstructionEx ctop = decoder.DecodeSlot(0x95ffffe1c0ULL,
                                            UnitType::B_UNIT,
                                            0x28290);
    assert_true("Authentic br.ctop should decode",
                ctop.GetType() == InstructionType::BR_CTOP);
    assert_equal("Authentic br.ctop must be unpredicated", 0, ctop.GetPredicate());
    assert_equal("Authentic br.ctop target", 0x28280, ctop.GetBranchTarget());
    assert_string("Authentic br.ctop disassembly",
                  "br.ctop 0x28280",
                  ctop.GetDisassembly());

    CPUState cpu;
    Memory memory(4096);
    cpu.SetCFM(0x111a3ULL);

    // Prolog/kernel phase: LC is decremented, EC is preserved, PR63 is
    // written before one rotation, and the top branch is taken.
    cpu.SetAR(65, 2);
    cpu.SetAR(66, 4);
    const ModuloLoopResult kernel = cpu.ExecuteBrCTop();
    assert_true("br.ctop LC>0 should branch", kernel.branchTaken);
    assert_true("br.ctop LC>0 should rotate", kernel.rotated);
    assert_equal("br.ctop LC>0 decrements LC", 1, cpu.GetAR(65));
    assert_equal("br.ctop LC>0 preserves EC", 4, cpu.GetAR(66));
    assert_equal("br.ctop LC>0 decrements RRB.GR", 31, cpu.GetRRB_GR());
    assert_equal("br.ctop LC>0 decrements RRB.FR", 95, cpu.GetRRB_FR());
    assert_equal("br.ctop LC>0 decrements RRB.PR", 47, cpu.GetRRB_PR());
    assert_true("br.ctop PR63 physical bit is set", cpu.GetPRPhysical(63));
    assert_true("br.ctop rotation exposes PR63 as logical PR16",
                cpu.GetPR(16) == cpu.GetPRPhysical(63));

    // First epilog phase: EC is decremented and the top branch remains taken.
    cpu.SetAR(65, 0);
    cpu.SetAR(66, 3);
    const ModuloLoopResult epilog = cpu.ExecuteBrCTop();
    assert_true("br.ctop EC>1 should branch", epilog.branchTaken);
    assert_true("br.ctop EC>1 should rotate", epilog.rotated);
    assert_equal("br.ctop EC>1 leaves LC zero", 0, cpu.GetAR(65));
    assert_equal("br.ctop EC>1 decrements EC", 2, cpu.GetAR(66));

    // Final epilog stage: EC reaches zero, the final stage rotates, and the
    // top branch falls through.
    cpu.SetAR(66, 1);
    const uint64_t cfmBeforeFinal = cpu.GetCFM();
    const ModuloLoopResult finalStage = cpu.ExecuteBrCTop();
    assert_true("br.ctop EC==1 should fall through", !finalStage.branchTaken);
    assert_true("br.ctop EC==1 should rotate", finalStage.rotated);
    assert_equal("br.ctop EC==1 decrements EC to zero", 0, cpu.GetAR(66));
    assert_true("br.ctop EC==1 changes RRBs", cpu.GetCFM() != cfmBeforeFinal);

    // Fully drained: PR63 is cleared but LC, EC, and all RRBs remain stable.
    const uint64_t cfmBeforeDrained = cpu.GetCFM();
    const ModuloLoopResult drained = cpu.ExecuteBrCTop();
    assert_true("br.ctop LC=EC=0 should fall through", !drained.branchTaken);
    assert_true("br.ctop LC=EC=0 should not rotate", !drained.rotated);
    assert_equal("br.ctop drained LC remains zero", 0, cpu.GetAR(65));
    assert_equal("br.ctop drained EC remains zero", 0, cpu.GetAR(66));
    assert_equal("br.ctop drained CFM remains stable", cfmBeforeDrained, cpu.GetCFM());

    // br.cexit has the same LC/EC/rotation state transition but the opposite
    // branch sense: it exits on the drained phases and falls through while a
    // kernel or epilog stage still has work to execute.
    CPUState cexitCpu;
    cexitCpu.SetCFM(0x111a3ULL);
    cexitCpu.SetAR(65, 1);
    cexitCpu.SetAR(66, 2);
    const ModuloLoopResult cexitKernel = cexitCpu.ExecuteBrCExit();
    assert_true("br.cexit LC>0 should fall through", !cexitKernel.branchTaken);
    assert_equal("br.cexit LC>0 decrements LC", 0, cexitCpu.GetAR(65));
    assert_equal("br.cexit LC>0 preserves EC", 2, cexitCpu.GetAR(66));
    cexitCpu.SetAR(66, 1);
    const ModuloLoopResult cexitFinal = cexitCpu.ExecuteBrCExit();
    assert_true("br.cexit EC==1 should branch", cexitFinal.branchTaken);
    assert_equal("br.cexit EC==1 drains EC", 0, cexitCpu.GetAR(66));
    const ModuloLoopResult cexitDrained = cexitCpu.ExecuteBrCExit();
    assert_true("br.cexit LC=EC=0 should branch", cexitDrained.branchTaken);
    assert_true("br.cexit drained stage should not rotate", !cexitDrained.rotated);

    std::cout << "  ? IA-64 br.ctop state machine passed" << std::endl;
}

void test_ia64_rotating_register_mapping() {
    std::cout << "Testing IA-64 rotating register mapping..." << std::endl;

    CPUState cpu;
    cpu.SetCFM(0x111a3ULL); // 32 rotating GRs, all RRBs initially zero.
    for (size_t i = 0; i < 32; ++i) {
        cpu.SetGRPhysical(32 + i, 0x1000 + i);
    }
    for (size_t i = 0; i < 48; ++i) {
        cpu.SetPRPhysical(16 + i, (i & 1) != 0);
    }
    for (size_t i = 0; i < 96; ++i) {
        uint8_t value[16] = {};
        value[0] = static_cast<uint8_t>(i);
        cpu.SetFRPhysical(32 + i, value);
    }
    cpu.SetGR(31, 0x3131);
    cpu.SetGRPhysical(64, 0x6464);

    assert_equal("logical GR32 initially maps to physical GR32",
                 0x1000, cpu.GetGR(32));
    assert_true("logical PR17 initially maps to physical PR17",
                cpu.GetPR(17));
    uint8_t initialFR[16] = {};
    cpu.GetFR(32, initialFR);
    assert_equal("logical FR32 initially maps to physical FR32", 0, initialFR[0]);

    cpu.RotateRegisters();
    assert_equal("first rotation sets RRB.PR to 47", 47, cpu.GetRRB_PR());
    assert_true("physical PR63 retains its patterned value", cpu.GetPRPhysical(63));
    assert_equal("rotated logical GR32 maps through RRB.GR",
                 0x101f, cpu.GetGR(32));
    assert_equal("GR outside SOR remains static", 0x6464, cpu.GetGR(64));
    assert_equal("static GR31 remains static", 0x3131, cpu.GetGR(31));
    assert_true("rotated logical PR16 maps through RRB.PR", cpu.GetPR(16));
    uint8_t rotatedFR[16] = {};
    cpu.GetFR(32, rotatedFR);
    assert_equal("rotated logical FR32 wraps to physical FR127", 95, rotatedFR[0]);

    for (size_t i = 0; i < 31; ++i) {
        cpu.RotateRegisters();
    }
    assert_equal("RRB.GR wraps after the configured GR region", 0, cpu.GetRRB_GR());
    assert_equal("RRB.FR tracks its independent 96-register phase", 64, cpu.GetRRB_FR());
    assert_equal("RRB.PR wraps after 48 rotations", 16, cpu.GetRRB_PR());
    for (size_t i = 0; i < 64; ++i) {
        cpu.RotateRegisters();
    }
    assert_equal("RRB.FR wraps after 96 rotations", 0, cpu.GetRRB_FR());
    assert_equal("RRB.PR wraps after 48 rotations", 0, cpu.GetRRB_PR());
    assert_true("PR0 remains hardwired true", cpu.GetPR(0));

    std::cout << "  ? IA-64 rotating register mapping passed" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "IA-64 Instruction Set Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    
    try {
        test_compare_instructions();
        test_compare_ne_decoder();
        test_latest_boot_log_blockers();
        test_ia64_region_register_moves();
        test_ia64_control_register_moves();
        test_ia64_translation_register_inserts();
        test_iso_boot_media_direct_path();
        test_fat_boot_media_lookup();
        test_el_torito_fat_boot_media_lookup();
        test_memory_bounds_throw();
        test_application_register_moves();
        test_test_instructions();
        test_bitwise_operations();
        test_ia64_immediate_andcm();
        test_subtract_operations();
        test_ia64_immediate_sub_raw_encoding();
        test_ia64_sub_minus_one_raw_encoding();
        test_shift_operations();
        test_extract_deposit();
        test_memory_operations();
        test_predicated_execution();
        test_alloc_instruction();
        test_rse_state_aliases();
        test_ia64_flushrs();
        test_ia64_invala();
        test_alloc_invalid_frame_size_fails_safe();
        test_cmp4_instructions();
        test_ia64_unsigned_fixed_truncate_modulus_sequence();
        test_ia64_unknown_slot_formatter();
        test_ia64_br_ctop_state_machine();
        test_ia64_rotating_register_mapping();
        
        std::cout << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "? ALL TESTS PASSED" << std::endl;
        std::cout << "========================================" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "TEST SUITE FAILED: " << e.what() << std::endl;
        return 1;
    }
}

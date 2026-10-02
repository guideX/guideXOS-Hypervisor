#include "decoder.h"
#include "cpu_state.h"
#include "IA64AddressTranslation.h"
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

void installAuthenticKernelImageDtr(CPUState& cpu) {
    constexpr uint64_t rr5 = 0x539ULL;
    constexpr uint64_t kernelVma = 0xA000000100000000ULL;
    constexpr uint64_t kernelImageTte = 0x10000004000661ULL;
    constexpr uint64_t kernelImageItir = 0x68ULL;
    cpu.SetRR(5, rr5);
    cpu.SetDTR(0, kernelImageTte, kernelVma, kernelImageItir, rr5);
}

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

    InstructionEx linuxFc = decoder.DecodeSlot(0x2182200000ULL, UnitType::M_UNIT, 0x4a062b0);
    assert_true("Linux raw fc should decode", linuxFc.GetType() == InstructionType::FC);
    assert_equal("Linux fc source register", 34, linuxFc.GetSrc1());
    assert_string("Linux fc disassembly", "fc r34", linuxFc.GetDisassembly());

    Memory kernelFcMemory(128 * 1024 * 1024);
    CPUState kernelFcCpu;
    kernelFcCpu.SetPSR(1ULL << 17);
    installAuthenticKernelImageDtr(kernelFcCpu);
    kernelFcCpu.SetGR(34, 0xA00000010004D4F1ULL);
    linuxFc.Execute(kernelFcCpu, kernelFcMemory);
    assert_equal("Linux canonical fc must preserve its address register",
                 0xA00000010004D4F1ULL, kernelFcCpu.GetGR(34));

    InstructionEx linuxDep = decoder.DecodeSlot(0x8112C1CB00ULL, UnitType::I_UNIT, 0x4a08dc0);
    assert_true("Linux raw register dep should decode", linuxDep.GetType() == InstructionType::DEP);
    assert_equal("Linux dep destination register", 44, linuxDep.GetDst());
    assert_equal("Linux dep source register", 14, linuxDep.GetSrc1());
    assert_equal("Linux dep merge register", 44, linuxDep.GetSrc2());
    assert_equal("Linux dep position", 61, linuxDep.GetImmediate() & 0x3F);
    assert_equal("Linux dep encoded length", 2,
                 (linuxDep.GetImmediate() >> 6) & 0x3F);
    assert_string("Linux register dep disassembly",
                  "dep r44 = r14, r44, 61, 3",
                  linuxDep.GetDisassembly());

    CPUState linuxDepCpu;
    linuxDepCpu.SetGR(14, 5);
    linuxDepCpu.SetGR(44, 0x123456789ABCDEF0ULL);
    linuxDep.Execute(linuxDepCpu, kernelFcMemory);
    assert_equal("Linux register dep should merge the high three bits",
                 0xB23456789ABCDEF0ULL, linuxDepCpu.GetGR(44));

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

    const uint8_t frontierBundleBytes[16] = {
        0x03, 0xD0, 0x00, 0x48, 0x18, 0x10, 0x60, 0x03,
        0x90, 0x00, 0x42, 0x20, 0xE3, 0xD2, 0x30, 0x80
    };
    const Bundle frontierBundle = decoder.DecodeBundleAt(
        frontierBundleBytes, 0x4A5E3B0ULL);
    assert_equal("frontier bundle template bits", 0x03,
                 static_cast<uint8_t>(frontierBundle.templateType));
    assert_true("frontier template 0x03 maps slots M/I/I",
                frontierBundle.templateType == TemplateType::MI_I_STOP &&
                frontierBundle.instructions.size() == 3 &&
                frontierBundle.instructions[0].GetUnit() == UnitType::M_UNIT &&
                frontierBundle.instructions[1].GetUnit() == UnitType::I_UNIT &&
                frontierBundle.instructions[2].GetUnit() == UnitType::I_UNIT);
    const InstructionEx frontierLoad = frontierBundle.instructions[0];
    assert_equal("frontier raw slot", 0x80C2400680ULL,
                 frontierLoad.GetRawBits());
    assert_true("frontier slot independently decodes as LD8",
                frontierLoad.GetType() == InstructionType::LD8 &&
                frontierLoad.GetUnit() == UnitType::M_UNIT);
    assert_equal("frontier load predicate is p0", 0,
                 frontierLoad.GetPredicate());
    assert_equal("frontier LD8 destination", 26, frontierLoad.GetDst());
    assert_equal("frontier LD8 base register", 36, frontierLoad.GetSrc1());
    assert_equal("frontier LD8 has no index/update register", 0,
                 frontierLoad.GetSrc2());
    assert_true("frontier LD8 is non-updating and has no immediate",
                !frontierLoad.HasRegisterUpdate() && !frontierLoad.HasImmediate());
    assert_string("frontier LD8 disassembly", "ld8 r26 = [r36]",
                  frontierLoad.GetDisassembly());

    // Exact Linux entry instruction at guest address 0x040d3ba6.
    // Retained Binutils 2.19.1 identifies raw 0x180000006 as (p06) srlz.d.
    InstructionEx srlzD = decoder.DecodeSlot(0x180000006ULL, UnitType::M_UNIT, 0x040d3ba0);
    assert_true("Linux entry srlz.d should decode", srlzD.GetType() == InstructionType::SRLZ_D);
    assert_equal("Linux entry srlz.d qualifying predicate", 6, srlzD.GetPredicate());
    assert_string("Linux entry srlz.d disassembly", "srlz.d", srlzD.GetDisassembly());
    CPUState srlzDCpu;
    srlzDCpu.SetPR(6, true);
    srlzDCpu.SetPSR(0x1234000000000000ULL | 0x4000ULL);
    srlzDCpu.SetIP(0x040d3ba0);
    srlzD.Execute(srlzDCpu, memory);
    assert_equal("srlz.d should preserve PSR", 0x1234000000004000ULL, srlzDCpu.GetPSR());
    assert_equal("srlz.d should preserve IP", 0x040d3ba0ULL, srlzDCpu.GetIP());

    // Exact Linux instruction at physical IP 0x041069d0. Retained Binutils
    // 2.19.1 identifies raw 0x8088e14440 as cmpxchg4.acq r17=[r14],r10,ar.ccv.
    InstructionEx cmpxchg = decoder.DecodeSlot(0x8088e14440ULL,
                                                UnitType::M_UNIT,
                                                0x041069d0);
    assert_true("Linux cmpxchg4.acq should decode",
                cmpxchg.GetType() == InstructionType::CMPXCHG4_ACQ);
    assert_equal("Linux cmpxchg destination register", 17, cmpxchg.GetDst());
    assert_equal("Linux cmpxchg address register", 14, cmpxchg.GetSrc1());
    assert_equal("Linux cmpxchg store register", 10, cmpxchg.GetSrc2());
    assert_equal("Linux cmpxchg qualifying predicate", 0, cmpxchg.GetPredicate());
    assert_string("Linux cmpxchg disassembly",
                  "cmpxchg4.acq r17 = [r14], r10, ar.ccv",
                  cmpxchg.GetDisassembly());

    Memory cmpxchgMemory(0x2000);
    const uint64_t cmpxchgAddress = 0x400;
    cmpxchgMemory.write<uint32_t>(cmpxchgAddress, 0x11223344U);
    CPUState cmpxchgCpu;
    cmpxchgCpu.SetGR(14, cmpxchgAddress);
    cmpxchgCpu.SetGR(10, 0xAABBCCDDU);
    cmpxchgCpu.SetAR(32, 0x11223344U);
    cmpxchg.Execute(cmpxchgCpu, cmpxchgMemory);
    assert_equal("cmpxchg should return the old 32-bit value",
                 0x11223344U, cmpxchgCpu.GetGR(17));
    assert_equal("cmpxchg should store on equal compare value",
                 0xAABBCCDDU, cmpxchgMemory.read<uint32_t>(cmpxchgAddress));

    cmpxchgMemory.write<uint32_t>(cmpxchgAddress, 0x55667788U);
    cmpxchgCpu.SetAR(32, 0x01020304U);
    cmpxchg.Execute(cmpxchgCpu, cmpxchgMemory);
    assert_equal("cmpxchg should return the failed compare value",
                 0x55667788U, cmpxchgCpu.GetGR(17));
    assert_equal("cmpxchg should not store on a failed compare",
                 0x55667788U, cmpxchgMemory.read<uint32_t>(cmpxchgAddress));

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

    // Exact Linux entry instruction at guest address 0x040d3b96.
    // Retained Binutils 2.19.1 identifies raw 0x30100006 as (p06) ssm 0x4000.
    InstructionEx ssm = decoder.DecodeSlot(0x30100006ULL, UnitType::M_UNIT, 0x040d3b90);
    assert_true("Linux entry ssm should decode", ssm.GetType() == InstructionType::SSM);
    assert_equal("Linux entry ssm qualifying predicate", 6, ssm.GetPredicate());
    assert_equal("Linux entry ssm immediate", 0x4000, ssm.GetImmediate());
    assert_string("Linux entry ssm disassembly", "ssm 0x4000", ssm.GetDisassembly());

    CPUState ssmCpu;
    ssmCpu.SetPSR((1ULL << 13) | (1ULL << 27) | (1ULL << 32));
    ssmCpu.SetPR(6, true);
    ssm.Execute(ssmCpu, memory);
    assert_true("Linux entry ssm should set PSR.IC", (ssmCpu.GetPSR() & (1ULL << 13)) != 0);
    assert_true("Linux entry ssm should set PSR 0x4000", (ssmCpu.GetPSR() & 0x4000) != 0);
    assert_true("Linux entry ssm should preserve upper PSR", (ssmCpu.GetPSR() & (1ULL << 32)) != 0);

    ssmCpu.SetPSR(0);
    ssmCpu.SetPR(6, false);
    ssm.Execute(ssmCpu, memory);
    assert_equal("false-predicated ssm should preserve PSR", 0, ssmCpu.GetPSR());

    // Exact Linux entry return-from-interruption instruction at 0x047f7e4c.
    // Retained Binutils 2.19.1 identifies raw 0x40000000 as unpredicated rfi.
    InstructionEx rfi = decoder.DecodeSlot(0x40000000ULL, UnitType::B_UNIT, 0x047f7e40);
    assert_true("Linux entry rfi should decode", rfi.GetType() == InstructionType::RFI);
    assert_equal("Linux entry rfi qualifying predicate", 0, rfi.GetPredicate());
    assert_string("Linux entry rfi disassembly", "rfi", rfi.GetDisassembly());
    CPUState rfiCpu;
    constexpr uint64_t rfiPsr = 0x1010084a2008ULL | IA64_PSR_BN_MASK;
    rfiCpu.SetGRPhysical(16, 0x1616ULL);
    rfiCpu.SetGRPhysical(NUM_GENERAL_REGISTERS, 0xB016ULL);
    rfiCpu.SetCR(16, rfiPsr);
    rfiCpu.SetCR(19, 0xa0000001007f7e50ULL);
    rfi.Execute(rfiCpu, memory);
    assert_equal("rfi should restore IPSR into PSR", rfiPsr, rfiCpu.GetPSR());
    assert_equal("rfi should reactivate the interrupted static GR bank",
                 0xB016ULL, rfiCpu.GetGR(16));
    assert_equal("rfi should restore IIP as a bundle address",
                 0xa0000001007f7e50ULL & ~0xFULL, rfiCpu.GetIP());

    // The authentic kernel's first post-rfi data reference uses its canonical
    // region-5 direct-map address.  The flat replay image places that same
    // object at physical 0x04cbc1d0.
    const uint64_t kernelVirtualData = 0xa000000100cbc1d0ULL;
    const uint64_t kernelPhysicalData = 0x04cbc1d0ULL;
    const uint64_t kernelDataValue = 0xa000000100cd55b0ULL;
    Memory kernelMemory(0x20000000);
    kernelMemory.write<uint64_t>(kernelPhysicalData, kernelDataValue);

    const uint64_t frontierDtrTte =
        0x04000000ULL | (1ULL << 52) | (1ULL << 6) | (1ULL << 5) |
        1ULL | (3ULL << 9);
    const uint64_t frontierDtrItir = (26ULL << 2) | (5ULL << 8);
    const uint64_t frontierDtrVirtualBase = 0xa000000100000000ULL;
    const uint64_t frontierDtrPhysicalOffset = 0x00d00000ULL;
    const uint64_t frontierDtrPhysicalAddress =
        0x04000000ULL + frontierDtrPhysicalOffset;
    const uint64_t frontierLoadValue = 0xFEDCBA9876543210ULL;
    kernelMemory.write<uint64_t>(frontierDtrPhysicalAddress, frontierLoadValue);
    CPUState frontierDtrCpu;
    frontierDtrCpu.SetPSR(1ULL << 17);
    frontierDtrCpu.SetRR(5, 0x539ULL);
    frontierDtrCpu.SetDTR(0, frontierDtrTte, frontierDtrVirtualBase,
                          frontierDtrItir, 0x539ULL);
    frontierDtrCpu.SetGR(36, frontierDtrVirtualBase + frontierDtrPhysicalOffset);
    frontierDtrCpu.SetGR(26, 0x1122334455667788ULL);
    frontierLoad.Execute(frontierDtrCpu, kernelMemory);
    assert_equal("exact frontier LD8 returns all 64 bits through the DTR",
                 frontierLoadValue, frontierDtrCpu.GetGR(26));
    assert_equal("exact frontier LD8 has no base update",
                 frontierDtrVirtualBase + frontierDtrPhysicalOffset,
                 frontierDtrCpu.GetGR(36));

    InstructionEx kernelLoad(InstructionType::LD8, UnitType::M_UNIT);
    kernelLoad.SetOperands(16, 2);
    CPUState kernelCpu;
    kernelCpu.SetPSR(1ULL << 17);
    installAuthenticKernelImageDtr(kernelCpu);
    kernelCpu.SetGR(2, kernelVirtualData);
    kernelLoad.Execute(kernelCpu, kernelMemory);
    assert_equal("kernel direct-map load should translate canonical address",
                 kernelDataValue, kernelCpu.GetGR(16));

    // The post-EFI kernel also references the same flat RAM through the
    // region-6 uncached alias observed at the first strict replay frontier.
    const uint64_t uncachedVirtualData = 0xc0000001008e1610ULL;
    const uint64_t uncachedPhysicalData = 0x008e1610ULL;
    const uint64_t uncachedDataValue = 0x5aULL;
    kernelMemory.write<uint8_t>(uncachedPhysicalData, uncachedDataValue);
    CPUState uncachedCpu;
    uncachedCpu.SetPSR(1ULL << 17);
    uncachedCpu.SetGR(2, uncachedVirtualData);
    kernelLoad.Execute(uncachedCpu, kernelMemory);
    assert_equal("kernel direct-map load should translate region-6 alias",
                 uncachedDataValue, uncachedCpu.GetGR(16));

    // Exact Linux entry translation instruction at 0x047f8150.  Retained
    // Binutils 2.19.1 identifies raw 0x20f02000c0 as tpa r3=r2.
    InstructionEx tpa = decoder.DecodeSlot(0x20f02000c0ULL, UnitType::M_UNIT, 0x047f8150);
    assert_true("Linux entry tpa should decode", tpa.GetType() == InstructionType::TPA);
    assert_equal("Linux entry tpa qualifying predicate", 0, tpa.GetPredicate());
    assert_equal("Linux entry tpa destination register", 3, tpa.GetDst());
    assert_equal("Linux entry tpa source register", 2, tpa.GetSrc1());
    assert_string("Linux entry tpa disassembly", "tpa r3 = r2", tpa.GetDisassembly());
    CPUState tpaCpu;
    installAuthenticKernelImageDtr(tpaCpu);
    tpaCpu.SetGR(2, 0xa0000001009fe510ULL);
    tpa.Execute(tpaCpu, kernelMemory);
    assert_equal("tpa should translate the kernel direct-map pointer",
                 0x049fe510ULL, tpaCpu.GetGR(3));
    assert_true("tpa result should not be NaT", !tpaCpu.GetGRNaT(3));

    // Exact Linux timekeeping instruction at physical IP 0x04039200.
    // Retained Binutils 2.19.1 identifies raw 0x2112c00380 as
    // mov.m r14=ar.itc.  The CPU core advances AR.ITC once per successful
    // instruction, so consecutive architectural reads must be monotonic.
    InstructionEx readItc = decoder.DecodeSlot(0x2112c00380ULL,
                                                UnitType::M_UNIT,
                                                0x04039200);
    assert_true("Linux ar.itc read should decode",
                readItc.GetType() == InstructionType::MOV_FROM_AR);
    assert_equal("Linux ar.itc destination register", 14, readItc.GetDst());
    assert_equal("Linux ar.itc application register", 44, readItc.GetSrc1());
    assert_string("Linux ar.itc disassembly", "mov r14 = ar.itc",
                  readItc.GetDisassembly());
    CPUState itcCpu;
    itcCpu.SetAR(44, 100);
    readItc.Execute(itcCpu, kernelMemory);
    assert_equal("ar.itc read should return the current counter", 100,
                 itcCpu.GetGR(14));
    itcCpu.AdvanceITC();
    readItc.Execute(itcCpu, kernelMemory);
    assert_equal("ar.itc should advance between CPU steps", 101,
                 itcCpu.GetGR(14));

    // The same Linux image's per-CPU PT_LOAD is mapped at physical
    // 0x04b80000.  The timing helper reads offset 0x478 through its
    // canonical region-7 address.
    const uint64_t perCpuVirtualAddress = 0xfffffffffffc0478ULL;
    const uint64_t perCpuPhysicalAddress = 0x04b80478ULL;
    kernelMemory.write<uint64_t>(perCpuPhysicalAddress, 0x1122334455667788ULL);
    CPUState perCpuLoadCpu;
    perCpuLoadCpu.SetPSR(1ULL << 17);
    perCpuLoadCpu.SetGR(2, perCpuVirtualAddress);
    InstructionEx perCpuLoad(InstructionType::LD8, UnitType::M_UNIT);
    perCpuLoad.SetOperands(15, 2);
    perCpuLoad.Execute(perCpuLoadCpu, kernelMemory);
    assert_equal("kernel per-CPU load should translate region-7 address",
                 0x1122334455667788ULL, perCpuLoadCpu.GetGR(15));

    // EFI boot parameters are passed through IA-64's region-7 __va() alias.
    // The flat replay image keeps that object at its physical address.
    const uint64_t region7VirtualAddress = 0xe00000001fd93008ULL;
    const uint64_t region7PhysicalAddress = 0x1fd93008ULL;
    const uint64_t region7Value = 0x5453595320494249ULL;
    kernelMemory.write<uint64_t>(region7PhysicalAddress, region7Value);
    CPUState region7LoadCpu;
    region7LoadCpu.SetPSR(1ULL << 17);
    region7LoadCpu.SetGR(2, region7VirtualAddress);
    InstructionEx region7Load(InstructionType::LD8, UnitType::M_UNIT);
    region7Load.SetOperands(14, 2);
    region7Load.Execute(region7LoadCpu, kernelMemory);
    assert_equal("EFI region-7 load should translate __va address",
                 region7Value, region7LoadCpu.GetGR(14));

    CPUState perCpuTpaCpu;
    perCpuTpaCpu.SetGR(2, perCpuVirtualAddress);
    tpa.Execute(perCpuTpaCpu, kernelMemory);
    assert_equal("tpa should translate the per-CPU pointer",
                 perCpuPhysicalAddress, perCpuTpaCpu.GetGR(3));

    // Exact Linux instruction at physical IP 0x048296a0.  Retained Binutils
    // 2.19.1 identifies raw 0x848a0060c0 as fetchadd4.acq r3=[r32],1.
    InstructionEx fetchadd = decoder.DecodeSlot(0x848a0060c0ULL,
                                                UnitType::M_UNIT,
                                                0x048296a0);
    assert_true("Linux fetchadd4.acq should decode",
                fetchadd.GetType() == InstructionType::FETCHADD4_ACQ);
    assert_equal("Linux fetchadd destination register", 3, fetchadd.GetDst());
    assert_equal("Linux fetchadd address register", 32, fetchadd.GetSrc1());
    assert_equal("Linux fetchadd increment", 1, fetchadd.GetImmediate());
    assert_string("Linux fetchadd disassembly",
                  "fetchadd4.acq r3 = [r32], 1",
                  fetchadd.GetDisassembly());

    const uint64_t fetchaddVirtualAddress = 0xa000000100004000ULL;
    const uint64_t fetchaddPhysicalAddress = 0x04004000ULL;
    kernelMemory.write<uint32_t>(fetchaddPhysicalAddress, 0x12345678U);
    CPUState fetchaddCpu;
    fetchaddCpu.SetPSR(1ULL << 17);
    installAuthenticKernelImageDtr(fetchaddCpu);
    fetchaddCpu.SetGR(32, fetchaddVirtualAddress);
    fetchadd.Execute(fetchaddCpu, kernelMemory);
    assert_equal("fetchadd should return the old zero-extended value",
                 0x12345678ULL, fetchaddCpu.GetGR(3));
    assert_equal("fetchadd should add one to the memory value",
                 0x12345679ULL,
                 kernelMemory.read<uint32_t>(fetchaddPhysicalAddress));
    assert_true("fetchadd result should not be NaT", !fetchaddCpu.GetGRNaT(3));

    // The authentic continuation's next M17-family use is the negative
    // INC3 form fetchadd4.acq r14=[r32],-1. INC3's sign is bit 15; the
    // adjacent M-ordering bit 36 remains zero for this acquire encoding.
    constexpr uint64_t fetchaddNegativeRaw = 0x848A00E380ULL;
    const InstructionEx fetchaddNegative = decoder.DecodeSlot(
        fetchaddNegativeRaw, UnitType::M_UNIT, 0x04827790);
    assert_true("Linux negative fetchadd4.acq should decode",
                fetchaddNegative.GetType() == InstructionType::FETCHADD4_ACQ);
    assert_equal("negative fetchadd destination", 14,
                 fetchaddNegative.GetDst());
    assert_equal("negative fetchadd address register", 32,
                 fetchaddNegative.GetSrc1());
    assert_equal("negative fetchadd INC3", static_cast<uint64_t>(-1),
                 fetchaddNegative.GetImmediate());
    assert_string("negative fetchadd disassembly",
                  "fetchadd4.acq r14 = [r32], -1",
                  fetchaddNegative.GetDisassembly());

    constexpr uint64_t fetchaddInc3Base = 0x848A0060C0ULL & ~(7ULL << 13);
    constexpr int32_t expectedInc3[8] = {16, 8, 4, 1, -16, -8, -4, -1};
    for (uint64_t encoded = 0; encoded < 8; ++encoded) {
        const uint64_t raw = fetchaddInc3Base |
            ((encoded & 0x3ULL) << 13) | ((encoded >> 2) << 15);
        const InstructionEx decodedInc3 =
            decoder.DecodeInstruction(raw, UnitType::M_UNIT);
        assert_true("all M17 INC3 encodings remain fetchadd4.acq",
                    decodedInc3.GetType() == InstructionType::FETCHADD4_ACQ);
        assert_equal("M17 signed INC3 table",
                     static_cast<uint64_t>(static_cast<int64_t>(expectedInc3[encoded])),
                     decodedInc3.GetImmediate());
    }

    const uint64_t fetchaddNegativeVirtual = 0xE000000000A14050ULL;
    const uint64_t fetchaddNegativePhysical = 0x00A14050ULL;
    kernelMemory.write<uint32_t>(fetchaddNegativePhysical, 3U);
    CPUState fetchaddNegativeCpu;
    fetchaddNegativeCpu.SetPSR(1ULL << 17);
    fetchaddNegativeCpu.SetGR(32, fetchaddNegativeVirtual);
    fetchaddNegative.Execute(fetchaddNegativeCpu, kernelMemory);
    assert_equal("negative fetchadd returns the old zero-extended value",
                 3, fetchaddNegativeCpu.GetGR(14));
    assert_equal("negative fetchadd decrements mapped memory", 2,
                 kernelMemory.read<uint32_t>(fetchaddNegativePhysical));

    // Exact Linux kernel instruction at physical IP 0x048274e0.  Retained
    // Binutils 2.19.1 identifies raw 0x858a006380 as fetchadd4.rel
    // r14=[r32],1.
    InstructionEx fetchaddRel = decoder.DecodeSlot(0x858a006380ULL,
                                                   UnitType::M_UNIT,
                                                   0x048274e0);
    assert_true("Linux fetchadd4.rel should decode",
                fetchaddRel.GetType() == InstructionType::FETCHADD4_REL);
    assert_equal("Linux release fetchadd destination register", 14,
                 fetchaddRel.GetDst());
    assert_equal("Linux release fetchadd address register", 32,
                 fetchaddRel.GetSrc1());
    assert_equal("Linux release fetchadd increment", 1,
                 fetchaddRel.GetImmediate());
    assert_string("Linux release fetchadd disassembly",
                  "fetchadd4.rel r14 = [r32], 1",
                  fetchaddRel.GetDisassembly());

    const uint64_t fetchaddRelVirtualAddress = 0xa000000100004100ULL;
    const uint64_t fetchaddRelPhysicalAddress = 0x04004100ULL;
    kernelMemory.write<uint32_t>(fetchaddRelPhysicalAddress, 0xABCDEF01U);
    CPUState fetchaddRelCpu;
    fetchaddRelCpu.SetPSR(1ULL << 17);
    installAuthenticKernelImageDtr(fetchaddRelCpu);
    fetchaddRelCpu.SetGR(32, fetchaddRelVirtualAddress);
    fetchaddRel.Execute(fetchaddRelCpu, kernelMemory);
    assert_equal("release fetchadd should return the old zero-extended value",
                 0xABCDEF01ULL, fetchaddRelCpu.GetGR(14));
    assert_equal("release fetchadd should add one to the memory value",
                 0xABCDEF02ULL,
                 kernelMemory.read<uint32_t>(fetchaddRelPhysicalAddress));
    assert_true("release fetchadd result should not be NaT",
                !fetchaddRelCpu.GetGRNaT(14));

    // Exact Linux instruction at physical IP 0x043e8b56.  Retained Binutils
    // 2.19.1 identifies raw 0xae120fc4c0 as dep r19=0,r32,0,3.  The CPOS6b
    // field is encoded as 63-position and IMM1 is in bit 36.
    InstructionEx depImmediate = decoder.DecodeSlot(0xae120fc4c0ULL,
                                                     UnitType::I_UNIT,
                                                     0x043e8b56);
    assert_true("Linux dep immediate should decode",
                depImmediate.GetType() == InstructionType::DEP);
    assert_equal("Linux dep destination register", 19, depImmediate.GetDst());
    assert_equal("Linux dep base register", 32, depImmediate.GetSrc2());
    assert_equal("Linux dep position", 0, depImmediate.GetImmediate() & 0x3F);
    assert_equal("Linux dep encoded length", 2,
                 (depImmediate.GetImmediate() >> 6) & 0x3F);
    assert_string("Linux dep disassembly",
                  "dep r19 = 0, r32, 0, 3",
                  depImmediate.GetDisassembly());

    CPUState depCpu;
    depCpu.SetGR(19, 0xdeadbeefdeadbeefULL);
    depCpu.SetGR(32, 0xa0000001009674a0ULL);
    depImmediate.Execute(depCpu, kernelMemory);
    assert_equal("Linux dep should preserve the source address except field",
                 0xa0000001009674a0ULL, depCpu.GetGR(19));

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

    // M19 GETF.EXP: x6=0x1D, qp/r1/f2 fields, architectural sign/exponent
    // extraction, NaT propagation, destination clearing, and predication.
    const uint64_t getfExpRaw = 0x0874800C080ULL;
    InstructionEx getf_exp = decoder.DecodeSlot(getfExpRaw, UnitType::M_UNIT, 0x4800500);
    assert_true("Boot raw getf.exp should decode", getf_exp.GetType() == InstructionType::GETF_EXP);
    assert_equal("Boot getf.exp destination register", 2, getf_exp.GetDst());
    assert_equal("Boot getf.exp source FP register", 6, getf_exp.GetSrc1());
    assert_equal("Boot getf.exp qualifying predicate", 0, getf_exp.GetPredicate());
    assert_string("Boot getf.exp disassembly",
                  "getf.exp r2 = f6",
                  getf_exp.GetDisassembly());

    auto setFloatingRegisterFormat = [&](uint64_t signAndExponent, uint64_t significand,
                                         bool natVal = false) {
        uint8_t value[16] = {};
        for (int i = 0; i < 8; ++i) {
            value[i] = static_cast<uint8_t>(significand >> (i * 8));
            value[8 + i] = static_cast<uint8_t>(signAndExponent >> (i * 8));
        }
        if (natVal) {
            std::memset(value, 0, sizeof(value));
            value[8] = 0xFE;
            value[9] = 0xFF;
            value[10] = 0x01;
        }
        cpu.SetFR(6, value);
    };

    constexpr uint64_t getfExpExponent = 0x12345ULL;
    setFloatingRegisterFormat(getfExpExponent, 0x8000000000000000ULL);
    cpu.SetGR(2, UINT64_MAX);
    cpu.SetGRNaT(2, true);
    getf_exp.Execute(cpu, memory);
    assert_equal("getf.exp normal value returns exponent field",
                 getfExpExponent, cpu.GetGR(2));
    assert_true("getf.exp clears destination upper bits", (cpu.GetGR(2) >> 18) == 0);
    assert_true("getf.exp clears destination NaT for non-NaT source", !cpu.GetGRNaT(2));

    setFloatingRegisterFormat((1ULL << 17) | getfExpExponent, 0x8000000000000000ULL);
    cpu.SetGR(2, 0);
    getf_exp.Execute(cpu, memory);
    assert_equal("getf.exp preserves exponent and maps sign to bit 17",
                 (1ULL << 17) | getfExpExponent, cpu.GetGR(2));

    setFloatingRegisterFormat(0, 0x8000000000000000ULL);
    getf_exp.Execute(cpu, memory);
    assert_equal("getf.exp accepts exponent field zero", 0, cpu.GetGR(2));

    setFloatingRegisterFormat(0x1FFFFULL, 0x8000000000000000ULL);
    getf_exp.Execute(cpu, memory);
    assert_equal("getf.exp preserves the maximum 17-bit exponent", 0x1FFFFULL, cpu.GetGR(2));

    setFloatingRegisterFormat(0x1FFFEULL, 0, true);
    cpu.SetGR(2, UINT64_MAX);
    cpu.SetGRNaT(2, false);
    getf_exp.Execute(cpu, memory);
    assert_equal("getf.exp NaTVal keeps architectural field result", 0x1FFFEULL, cpu.GetGR(2));
    assert_true("getf.exp propagates source NaTVal to destination GR NaT", cpu.GetGRNaT(2));

    setFloatingRegisterFormat(getfExpExponent, 0x8000000000000000ULL);
    cpu.SetGR(2, 0xFEDCBA9876543210ULL);
    cpu.SetGRNaT(2, true);
    cpu.SetPR(1, false);
    InstructionEx predicatedGetfExp = getf_exp;
    predicatedGetfExp.SetPredicate(1);
    predicatedGetfExp.Execute(cpu, memory);
    assert_equal("false-predicated getf.exp preserves destination", 0xFEDCBA9876543210ULL, cpu.GetGR(2));
    assert_true("false-predicated getf.exp preserves destination NaT", cpu.GetGRNaT(2));
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

    // Linux's IA-64 udelay path uses SETF.SIG -> XMA.L -> GETF.SIG to form
    // start + usecs * cyc_per_usec. Exercise the authentic integer-format FP
    // exponent and operand values used by that path, rather than only raw
    // significands with zeroed exponent fields.
    const InstructionEx timebaseXma = decoder.DecodeSlot(
        0x1d038910180ULL, UnitType::F_UNIT, 0x4039240);
    assert_true("Authentic udelay XMA.L slot should decode",
                timebaseXma.GetType() == InstructionType::XMA);
    assert_equal("Authentic udelay XMA.L destination", 6, timebaseXma.GetDst());
    assert_equal("Authentic udelay XMA.L multiplicand f9", 9, timebaseXma.GetSrc1());
    assert_equal("Authentic udelay XMA.L multiplier f7", 7, timebaseXma.GetSrc2());
    assert_equal("Authentic udelay XMA.L addend f8", 8, timebaseXma.GetSrc3());

    const auto setIntegerFormat = [&cpu](uint8_t reg, uint64_t value) {
        uint8_t format[16] = {};
        for (int i = 0; i < 8; ++i) {
            format[i] = static_cast<uint8_t>(value >> (i * 8));
        }
        format[8] = 0x3e;
        format[9] = 0x00;
        format[10] = 0x01;
        cpu.SetFR(reg, format);
    };
    constexpr uint64_t itcStart = 0x2e665b0bULL;
    setIntegerFormat(8, itcStart);
    setIntegerFormat(9, 1000);
    setIntegerFormat(7, 18);
    timebaseXma.Execute(cpu, memory);
    InstructionEx getfTimebase = getf_sig;
    getfTimebase.SetOperands(14, 6, 0);
    getfTimebase.Execute(cpu, memory);
    assert_equal("Authentic udelay XMA.L should add the usec-to-ITC delta",
                 itcStart + 18000, cpu.GetGR(14));

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

    // Linux's early virtual-HPT setup uses the X2b=2 encoding of variable
    // SHR.  The ELF disassembly identifies raw 0xf221636588 as
    // "shr r22=r22,r27" at physical address 0x4000050.
    const InstructionEx kernelShr = decoder.DecodeSlot(
        0xF221636588ULL, UnitType::I_UNIT, 0x4000050ULL);
    assert_true("Linux X2b=2 variable SHR should decode",
                kernelShr.GetType() == InstructionType::SHR);
    assert_equal("Linux variable SHR destination", 22, kernelShr.GetDst());
    assert_equal("Linux variable SHR source", 22, kernelShr.GetSrc1());
    assert_equal("Linux variable SHR count register", 27, kernelShr.GetSrc2());
    assert_string("Linux variable SHR disassembly", "shr r22 = r22, r27",
                  kernelShr.GetDisassembly());
    cpu.SetPR(8, true);
    cpu.SetGR(22, 0xFEDCBA9876543210ULL);
    cpu.SetGR(27, 12);
    kernelShr.Execute(cpu, memory);
    assert_equal("Linux variable SHR executes as logical right shift",
                 0x000FEDCBA9876543ULL, cpu.GetGR(22));

    // The Linux VHPT refill vector uses this table-indexed TBIT.Z.UNC form.
    const InstructionEx kernelTbit = decoder.DecodeSlot(
        0xA0513812C7ULL, UnitType::I_UNIT, 0x4000100ULL);
    assert_true("Linux table-indexed TBIT.Z.UNC should decode",
                kernelTbit.GetType() == InstructionType::TBIT_Z);
    assert_equal("Linux TBIT.Z.UNC predicate", 7, kernelTbit.GetPredicate());
    assert_equal("Linux TBIT.Z.UNC first predicate destination", 11,
                 kernelTbit.GetDst());
    assert_equal("Linux TBIT.Z.UNC source register", 19,
                 kernelTbit.GetSrc1());
    assert_equal("Linux TBIT.Z.UNC second predicate destination", 10,
                 kernelTbit.GetSrc3());
    assert_equal("Linux TBIT.Z.UNC bit position", 32,
                 kernelTbit.GetImmediate());
    assert_true("Linux TBIT.Z.UNC should retain its unc completer",
                kernelTbit.GetCompareCompleter() == CompareCompleter::UNC);
    cpu.SetPR(7, true);
    cpu.SetGR(19, 0);
    kernelTbit.Execute(cpu, memory);
    assert_true("Linux TBIT.Z.UNC zero bit should set p11", cpu.GetPR(11));
    assert_true("Linux TBIT.Z.UNC zero bit should clear p10", !cpu.GetPR(10));

    // Linux's MOVL patcher uses predicate-paired variable SHRs when the
    // address shift is below 64. Preserve the encoded p6 so this path remains
    // nullified when the complementary p7 path is selected.
    const InstructionEx patchShrMask = decoder.DecodeSlot(
        0xF20211E846ULL, UnitType::I_UNIT, 0x4012F76ULL);
    const InstructionEx patchShrValue = decoder.DecodeSlot(
        0xF20221E3C6ULL, UnitType::I_UNIT, 0x4012F7CULL);
    assert_true("Linux MOVL patch-mask SHR decodes",
                patchShrMask.GetType() == InstructionType::SHR);
    assert_equal("Linux MOVL patch-mask SHR predicate", 6,
                 patchShrMask.GetPredicate());
    assert_equal("Linux MOVL patch-value SHR predicate", 6,
                 patchShrValue.GetPredicate());
    cpu.SetPR(6, false);
    cpu.SetGR(33, 0xFFFF7F000000000ULL);
    cpu.SetGR(15, 0x202900000000000ULL);
    patchShrMask.Execute(cpu, memory);
    patchShrValue.Execute(cpu, memory);
    assert_equal("false p6 patch-mask SHR leaves the MOVL mask intact",
                 0xFFFF7F000000000ULL, cpu.GetGR(33));
    assert_equal("false p6 patch-value SHR leaves the MOVL value intact",
                 0x202900000000000ULL, cpu.GetGR(15));
    cpu.SetPR(6, true);

    // IA-64 variable shift counts use only the low six bits. Linux's
    // ia64_patch_imm64 relies on wrapped counts when patching the X-slot of a
    // long MOVL bundle.
    InstructionEx wrappedShl(InstructionType::SHL, UnitType::I_UNIT);
    wrappedShl.SetOperands(14, 25, 23);
    cpu.SetGR(25, 1);
    cpu.SetGR(23, 0xFFFFFFFFFFFFFFE9ULL); // low six bits are 41.
    wrappedShl.Execute(cpu, memory);
    assert_equal("variable SHL wraps negative count to its low six bits",
                 1ULL << 41, cpu.GetGR(14));
    cpu.SetGR(23, 64);
    wrappedShl.Execute(cpu, memory);
    assert_equal("variable SHL count 64 wraps to zero", 1, cpu.GetGR(14));

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

    // Exact Linux kernel instruction at physical IP 0x408ac60.
    // Retained Binutils disassembles raw 0x2128000bc0 as "mov r47=psr".
    const uint64_t rawFromPsr = 0x2128000bc0ULL;
    const InstructionEx fromPsr = decoder.DecodeSlot(
        rawFromPsr, UnitType::M_UNIT, 0x0408ac60);
    assert_true("Linux mov-from-PSR should decode",
                fromPsr.GetType() == InstructionType::MOV_FROM_PSR);
    assert_equal("mov-from-PSR destination register", 47, fromPsr.GetDst());
    assert_equal("mov-from-PSR qualifying predicate", 0, fromPsr.GetPredicate());
    assert_string("mov-from-PSR disassembly",
                  "mov r47 = psr",
                  fromPsr.GetDisassembly());

    cpu.SetPSR(0x1010084a2008ULL);
    fromPsr.Execute(cpu, memory);
    assert_equal("mov-from-PSR should copy the processor status register",
                 0x1010084a2008ULL,
                 cpu.GetGR(47));

    // Exact Linux instruction at canonical VMA 0xA000000100012BA0.
    // Retained Binutils disassembles raw 0x216804e000 as "mov psr.l=r39".
    const uint64_t rawToPsr = 0x216804e000ULL;
    const InstructionEx toPsr = decoder.DecodeSlot(
        rawToPsr, UnitType::M_UNIT, 0x04012ba0);
    assert_true("Linux mov-to-PSR should decode",
                toPsr.GetType() == InstructionType::MOV_TO_PSR);
    assert_equal("mov-to-PSR source register", 39, toPsr.GetSrc1());
    assert_equal("mov-to-PSR qualifying predicate", 0, toPsr.GetPredicate());
    assert_string("mov-to-PSR disassembly",
                  "mov psr.l = r39",
                  toPsr.GetDisassembly());

    cpu.SetPSR(0xA5A500001010084AULL);
    cpu.SetGR(39, 0xFFFFFFFF12345678ULL);
    toPsr.Execute(cpu, memory);
    assert_equal("mov-to-PSR should replace only PSR.l",
                 0xA5A5000012345678ULL,
                 cpu.GetPSR());

    // Exact post-A5D Linux instruction at canonical VMA
    // 0xA00000010080DDC0.  Retained Binutils disassembles raw
    // 0x20B80003C0 as "mov r15=cpuid[r0]".
    const uint64_t rawFromCpuid = 0x20b80003c0ULL;
    const InstructionEx fromCpuid = decoder.DecodeSlot(
        rawFromCpuid, UnitType::M_UNIT, 0x0480ddc0);
    assert_true("Linux mov-from-CPUID should decode",
                fromCpuid.GetType() == InstructionType::MOV_FROM_CPUID);
    assert_equal("mov-from-CPUID destination register", 15, fromCpuid.GetDst());
    assert_equal("mov-from-CPUID selector register", 0, fromCpuid.GetSrc1());
    assert_equal("mov-from-CPUID qualifying predicate", 0, fromCpuid.GetPredicate());
    assert_string("mov-from-CPUID disassembly",
                  "mov r15 = cpuid[r0]",
                  fromCpuid.GetDisassembly());

    fromCpuid.Execute(cpu, memory);
    assert_equal("mov-from-CPUID should return the vendor low word",
                 0x49656e69756e6547ULL,
                 cpu.GetGR(15));

    InstructionEx indexedCpuid(InstructionType::MOV_FROM_CPUID, UnitType::M_UNIT);
    indexedCpuid.SetOperands(10, 11, 0);
    cpu.SetGR(11, 0x102ULL); // selector uses only GR[11]{7:0}: CPUID[2]
    indexedCpuid.Execute(cpu, memory);
    assert_equal("mov-from-CPUID should use the low selector byte", 0, cpu.GetGR(10));

    cpu.SetGR(10, 0xfeedfaceULL);
    indexedCpuid.SetPredicate(1);
    cpu.SetPR(1, false);
    indexedCpuid.Execute(cpu, memory);
    assert_equal("false-predicated mov-from-CPUID preserves its destination",
                 0xfeedfaceULL,
                 cpu.GetGR(10));

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

void test_ia64_vhpt_instructions() {
    std::cout << "Testing IA-64 VHPT and TLB refill instructions..." << std::endl;

    InstructionDecoder decoder;
    constexpr uint64_t itcDAddress = 0xA000000100000130ULL;
    const uint8_t itcDBundleBytes[16] = {
        0x6A, 0x01, 0x48, 0x00, 0x2E, 0x04,
        0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x04, 0x00
    };
    const Bundle itcDBundle = decoder.DecodeBundleAt(itcDBundleBytes, itcDAddress);
    assert_true("Linux VHPT handler itc.d bundle should decode",
                itcDBundle.instructions.size() == 3 &&
                itcDBundle.instructions[0].GetType() == InstructionType::ITC_D);
    const InstructionEx& itcD = itcDBundle.instructions[0];
    assert_equal("itc.d source TTE register", 18, itcD.GetSrc1());
    assert_equal("Linux itc.d qualifying predicate", 11, itcD.GetPredicate());

    constexpr uint64_t thashAddress = 0xA000000100002010ULL;
    const uint8_t thashBundleBytes[16] = {
        0x00, 0x88, 0x00, 0x20, 0x1A, 0x04,
        0xD0, 0x01, 0x00, 0x62, 0x00, 0xE0,
        0x03, 0x00, 0xCC, 0x00
    };
    const Bundle thashBundle = decoder.DecodeBundleAt(thashBundleBytes, thashAddress);
    assert_true("Linux VHPT thash bundle should decode",
                thashBundle.instructions.size() == 3 &&
                thashBundle.instructions[0].GetType() == InstructionType::THASH);
    assert_equal("thash destination register", 17,
                 thashBundle.instructions[0].GetDst());
    assert_equal("thash virtual-address source register", 16,
                 thashBundle.instructions[0].GetSrc1());

    constexpr uint64_t ptcAddress = 0xA000000100000190ULL;
    const uint8_t ptcBundleBytes[16] = {
        0xD1, 0x00, 0x6C, 0x20, 0x09, 0x04,
        0xF0, 0xFF, 0xC0, 0xBF, 0x05, 0x00,
        0x00, 0x00, 0x20, 0x00
    };
    const Bundle ptcBundle = decoder.DecodeBundleAt(ptcBundleBytes, ptcAddress);
    assert_true("Linux VHPT-handler ptc.l bundle should decode",
                ptcBundle.instructions.size() == 3 &&
                ptcBundle.instructions[0].GetType() == InstructionType::PTC_L);
    assert_equal("ptc.l invalidation address register", 16,
                 ptcBundle.instructions[0].GetSrc1());
    assert_equal("ptc.l invalidation page-size register", 27,
                 ptcBundle.instructions[0].GetSrc2());

    constexpr uint64_t rr5 = 0x539ULL;
    constexpr uint64_t pta = ((1ULL << 61) - (1ULL << 50)) | (50ULL << 2) | 1ULL;
    constexpr uint64_t virtualAddress = 0xA0007FFFFFC80000ULL;
    constexpr uint64_t pageFlags = (1ULL << 52) | (1ULL << 6) | (1ULL << 5) |
                                   1ULL | (3ULL << 9);
    CPUState cpu;
    Memory memory(0x100000, false);
    cpu.SetPSR(1ULL << 17);
    cpu.SetRR(5, rr5);
    cpu.SetCR(8, pta);
    cpu.SetCR(20, virtualAddress);
    cpu.SetCR(21, (5ULL << 8) | (14ULL << 2));
    cpu.SetPR(11, true);
    cpu.SetGR(18, 0x3C000ULL | pageFlags);
    itcD.Execute(cpu, memory);
    assert_true("itc.d installs a valid data-TLB entry", cpu.GetDTLB(0).valid);
    assert_equal("itc.d records the guest TTE", 0x1000000003C661ULL,
                 cpu.GetDTLB(0).physicalAddress);
    assert_equal("itc.d records the faulting VA page", virtualAddress,
                 cpu.GetDTLB(0).virtualAddress);
    assert_equal("itc.d records RID and page size", 0x538ULL,
                 cpu.GetDTLB(0).itir);
    assert_equal("short-format thash uses the live PTA/RR", 0xBFFC000FFFFFF900ULL,
                 ComputeIA64ShortVhptAddress(cpu, virtualAddress));

    memory.write<uint64_t>(0x3C123ULL, 0xFEDCBA9876543210ULL);
    const IA64AddressTranslator translator;
    const IA64AddressTranslation cached = translator.TranslateDataAddress(
        cpu, memory, virtualAddress + 0x123ULL, MemoryAccessType::READ);
    assert_true("itc.d entry satisfies a later DTLB lookup",
                cached.mechanism == IA64TranslationMechanism::DATA_TLB);
    assert_equal("itc.d entry translates to the guest physical PPN",
                 0x3C123ULL, cached.physicalAddress);
    uint64_t cachedValue = 0;
    translator.ReadData(cpu, memory, virtualAddress + 0x123ULL,
                        reinterpret_cast<uint8_t*>(&cachedValue), sizeof(cachedValue));
    assert_equal("DTLB hit reads the translated guest physical data",
                 0xFEDCBA9876543210ULL, cachedValue);

    cpu.SetGR(16, virtualAddress);
    thashBundle.instructions[0].Execute(cpu, memory);
    assert_equal("executed thash writes CR.PTA-derived IHA", 0xBFFC000FFFFFF900ULL,
                 cpu.GetGR(17));

    std::cout << "  ? VHPT hash and ITC.D refill instructions passed" << std::endl;
}

void test_ia64_data_address_translation() {
    std::cout << "Testing IA-64 data address translation..." << std::endl;

    constexpr uint64_t psrDt = (1ULL << 17) | (1ULL << 13);
    constexpr uint64_t rr5 = 0x539ULL; // RID 5, PS 14, VHPT enabled.
    constexpr uint64_t shortVhptPta =
        ((1ULL << 61) - (1ULL << 50)) | (50ULL << 2) | 1ULL;
    constexpr uint64_t pageFlags =
        (1ULL << 52) | (1ULL << 6) | (1ULL << 5) | 1ULL | (3ULL << 9);
    constexpr uint64_t region5 = 5ULL << 61;

    // Exact authentic Linux bundle at raw IP 0x4a5e3b0. The raw 41-bit slot
    // is independently extracted from the full 128-bit bundle, whose template
    // is 0x03 (MII with an internal and bundle-end stop).
    const uint8_t frontierBundleBytes[16] = {
        0x03, 0xD0, 0x00, 0x48, 0x18, 0x10, 0x60, 0x03,
        0x90, 0x00, 0x42, 0x20, 0xE3, 0xD2, 0x30, 0x80
    };
    InstructionDecoder decoder;
    // Exact MLX bundle from the kernel's nested-DTLB vector. LOAD_PHYSICAL
    // will patch this canonical swapper_pg_dir address at runtime; before that
    // patch, MOVL must retain the full 64-bit symbol address.
    const uint8_t nestedVectorMovlBundle[16] = {
        0x04, 0x00, 0x00, 0x00, 0x30, 0x80, 0x00, 0x01,
        0x00, 0x00, 0x20, 0x63, 0x02, 0x90, 0x02, 0x6A
    };
    const Bundle nestedVectorMovl = decoder.DecodeBundleAt(
        nestedVectorMovlBundle, 0x4001450ULL);
    assert_true("nested-DTLB LOAD_PHYSICAL bundle decodes as MLX MOVL",
                nestedVectorMovl.instructions.size() == 2 &&
                nestedVectorMovl.instructions[1].GetType() == InstructionType::MOVL);
    assert_equal("nested-DTLB swapper_pg_dir MOVL keeps its canonical immediate",
                 0xA000000100B44000ULL,
                 nestedVectorMovl.instructions[1].GetImmediate());

    const Bundle frontierBundle = decoder.DecodeBundleAt(
        frontierBundleBytes, 0x4A5E3B0ULL);
    assert_equal("frontier bundle template", 0x03,
                 static_cast<uint8_t>(frontierBundle.templateType));
    assert_true("frontier bundle has three slots",
                frontierBundle.instructions.size() == 3);
    const InstructionEx& frontierLoad = frontierBundle.instructions[0];
    assert_equal("frontier slot raw 41-bit instruction", 0x80C2400680ULL,
                 frontierLoad.GetRawBits());
    assert_true("frontier slot is an M-unit LD8",
                frontierLoad.GetUnit() == UnitType::M_UNIT &&
                frontierLoad.GetType() == InstructionType::LD8);
    assert_equal("frontier predicate", 0, frontierLoad.GetPredicate());
    assert_equal("frontier destination", 26, frontierLoad.GetDst());
    assert_equal("frontier base register", 36, frontierLoad.GetSrc1());
    assert_equal("frontier has no register operand update", 0,
                 frontierLoad.GetSrc2());
    assert_true("frontier has no immediate update",
                !frontierLoad.HasImmediate() && !frontierLoad.HasRegisterUpdate());
    assert_string("frontier instruction disassembly",
                  "ld8 r26 = [r36]", frontierLoad.GetDisassembly());

    // A DTR hit is an independently testable IA-64 translation mechanism.
    // The TTE has P/A/D, kernel RWX rights, and a write-back memory attribute.
    Memory trMemory(0x100000, false);
    const uint64_t trVaBase = region5 + 0x100000ULL;
    constexpr uint64_t trPaBase = 0x20000ULL;
    constexpr uint64_t trItir = (14ULL << 2) | (5ULL << 8);
    trMemory.write<uint64_t>(trPaBase + 0x123ULL, 0xFEDCBA9876543210ULL);
    CPUState trCpu;
    trCpu.SetPSR(psrDt);
    trCpu.SetRR(5, rr5);
    trCpu.SetCR(8, shortVhptPta);
    trCpu.SetDTR(0, trPaBase | pageFlags, trVaBase, trItir, rr5);
    const IA64AddressTranslator translator;
    const IA64AddressTranslation trResult = translator.TranslateDataAddress(
        trCpu, trMemory, trVaBase + 0x123ULL, MemoryAccessType::READ);
    assert_true("DTR supplies the selected translation",
                trResult.mechanism == IA64TranslationMechanism::TRANSLATION_REGISTER);
    assert_equal("DTR physical address", trPaBase + 0x123ULL,
                 trResult.physicalAddress);
    assert_equal("DTR page size", 0x4000ULL, trResult.pageSize);
    assert_equal("DTR RID", 5, trResult.regionId);
    assert_equal("DTR memory attribute", 0, trResult.memoryAttribute);
    assert_equal("DTR access rights", 3, trResult.accessRights);

    // Execute the exact frontier LD8 through a valid DTR and use a value with
    // its sign bit set to prove the complete 64-bit bit pattern is preserved.
    trCpu.SetGR(36, trVaBase + 0x123ULL);
    trCpu.SetGR(26, 0x1122334455667788ULL);
    frontierLoad.Execute(trCpu, trMemory);
    assert_equal("frontier-family LD8 returns the full 64-bit value",
                 0xFEDCBA9876543210ULL, trCpu.GetGR(26));
    assert_equal("non-updating LD8 preserves its base register",
                 trVaBase + 0x123ULL, trCpu.GetGR(36));

    InstructionEx translatedStore(InstructionType::ST8, UnitType::M_UNIT);
    translatedStore.SetOperands(36, 26, 0);
    trCpu.SetGR(26, 0x8877665544332211ULL);
    translatedStore.Execute(trCpu, trMemory);
    assert_equal("store uses the same translated DTR page",
                 0x8877665544332211ULL,
                 trMemory.read<uint64_t>(trPaBase + 0x123ULL));

    // Reject a DTR with a different RID, and verify a page-size boundary does
    // not let the entry cover the next 16-KiB page.
    CPUState otherRidCpu;
    otherRidCpu.SetPSR(psrDt);
    otherRidCpu.SetRR(5, 0x639ULL); // RID 6, same PS and VHPT enable.
    otherRidCpu.SetCR(8, shortVhptPta);
    otherRidCpu.SetDTR(0, trPaBase | pageFlags, trVaBase, trItir, rr5);
    bool wrongRidRejected = false;
    try {
        translator.TranslateDataAddress(otherRidCpu, trMemory,
            trVaBase + 0x123ULL, MemoryAccessType::READ);
    } catch (const IA64TranslationFault& fault) {
        wrongRidRejected = fault.GetKind() == IA64TranslationFaultKind::VHPT_MISS &&
                           fault.GetVectorOffset() == 0;
    }
    assert_true("different RID cannot hit the DTR", wrongRidRejected);

    bool pageBoundaryMiss = false;
    try {
        translator.TranslateDataAddress(trCpu, trMemory,
            trVaBase + 0x4000ULL, MemoryAccessType::READ);
    } catch (const IA64TranslationFault& fault) {
        pageBoundaryMiss = fault.GetKind() == IA64TranslationFaultKind::VHPT_MISS &&
                           fault.GetVectorOffset() == 0;
    }
    assert_true("DTR match stops exactly at its page-size boundary", pageBoundaryMiss);

    CPUState readOnlyCpu;
    readOnlyCpu.SetPSR(psrDt);
    readOnlyCpu.SetRR(5, rr5);
    readOnlyCpu.SetCR(8, shortVhptPta);
    readOnlyCpu.SetDTR(0, trPaBase | (pageFlags & ~(7ULL << 9)),
                       trVaBase, trItir, rr5);
    bool writeRightsFault = false;
    try {
        translator.TranslateDataAddress(readOnlyCpu, trMemory,
            trVaBase + 0x123ULL, MemoryAccessType::WRITE);
    } catch (const IA64TranslationFault& fault) {
        writeRightsFault = fault.GetKind() == IA64TranslationFaultKind::ACCESS_RIGHTS &&
                           fault.GetVectorOffset() == 0x5300;
    }
    assert_true("read-only TTE rejects a store with data-access-rights vector",
                writeRightsFault);

    // Exercise the short-format VHPT using only CR.PTA, RR, and the cached TLB
    // mapping for the guest-owned VHPT page. Two neighboring PTEs map to
    // non-contiguous physical frames so an 8-byte access crosses translations.
    Memory pageTableMemory(0x200000, false);
    constexpr uint64_t vhptBackingPa = 0x8000;
    constexpr uint64_t firstDataPa = 0x10000;
    constexpr uint64_t secondDataPa = 0x14000;
    constexpr uint64_t virtualCrossingBase = region5 + 0x18000ULL;
    CPUState walkCpu;
    walkCpu.SetPSR(psrDt);
    walkCpu.SetRR(5, rr5);
    walkCpu.SetCR(8, shortVhptPta);
    const uint64_t firstCrossingVa = virtualCrossingBase + 0x3FFCULL;
    const uint64_t secondCrossingVa = firstCrossingVa + 4;
    const uint64_t firstHashAddress =
        ComputeIA64ShortVhptAddress(walkCpu, firstCrossingVa);
    const uint64_t secondHashAddress =
        ComputeIA64ShortVhptAddress(walkCpu, secondCrossingVa);
    const uint64_t vhptPageVa = firstHashAddress & ~0x3FFFULL;
    assert_equal("adjacent 8-byte access PTEs share a VHPT backing page",
                 vhptPageVa, secondHashAddress & ~0x3FFFULL);
    const uint64_t firstPtePa = vhptBackingPa + (firstHashAddress & 0x3FFFULL);
    const uint64_t secondPtePa = vhptBackingPa + (secondHashAddress & 0x3FFFULL);
    pageTableMemory.write<uint64_t>(firstPtePa, firstDataPa | pageFlags);
    pageTableMemory.write<uint64_t>(secondPtePa, secondDataPa | pageFlags);
    pageTableMemory.write<uint32_t>(firstDataPa + 0x3FFC, 0x44332211U);
    pageTableMemory.write<uint32_t>(secondDataPa, 0x88776655U);

    const uint64_t rr5Itir = (5ULL << 8) | (14ULL << 2);
    walkCpu.InsertDTLB(vhptBackingPa | pageFlags, vhptPageVa, rr5Itir, rr5);
    const IA64AddressTranslation walked = translator.TranslateDataAddress(
        walkCpu, pageTableMemory, firstCrossingVa, MemoryAccessType::READ);
    assert_true("region-5 page-table walk is selected",
                walked.mechanism == IA64TranslationMechanism::SHORT_VHPT);
    assert_equal("walked PTE maps to the first physical frame",
                 firstDataPa + 0x3FFC, walked.physicalAddress);
    assert_equal("walked mapping uses the RR page size", 0x4000, walked.pageSize);
    assert_equal("walked mapping retains RID 5", 5, walked.regionId);
    assert_equal("walked PTE is write-back", 0, walked.memoryAttribute);
    const IA64AddressTranslation refilled = translator.TranslateDataAddress(
        walkCpu, pageTableMemory, firstCrossingVa, MemoryAccessType::READ);
    assert_true("short VHPT lookup fills a subsequent DTLB hit",
                refilled.mechanism == IA64TranslationMechanism::DATA_TLB);

    uint64_t crossingValue = 0;
    translator.ReadData(walkCpu, pageTableMemory,
                        firstCrossingVa,
                        reinterpret_cast<uint8_t*>(&crossingValue),
                        sizeof(crossingValue));
    assert_equal("8-byte load crosses translations without assuming contiguous PAs",
                 0x8877665544332211ULL, crossingValue);
    const uint64_t crossingStore = 0x0102030405060708ULL;
    translator.WriteData(walkCpu, pageTableMemory,
                         firstCrossingVa,
                         reinterpret_cast<const uint8_t*>(&crossingStore),
                         sizeof(crossingStore));
    assert_equal("cross-page store updates its first physical frame",
                 0x05060708U,
                 pageTableMemory.read<uint32_t>(firstDataPa + 0x3FFC));
    assert_equal("cross-page store updates its second physical frame",
                 0x01020304U, pageTableMemory.read<uint32_t>(secondDataPa));

    // Region 7 remains the documented identity-mapped kernel region.  Region
    // 5 with no matching DTR or valid page-table root must instead miss; its
    // region bits are never stripped into flat physical RAM.
    CPUState region7Cpu;
    region7Cpu.SetPSR(psrDt);
    region7Cpu.SetRR(7, 0x760ULL);
    const IA64AddressTranslation region7Translation = translator.TranslateDataAddress(
        region7Cpu, trMemory, 0xE000000000001234ULL, MemoryAccessType::READ);
    assert_true("region 7 retains identity translation",
                region7Translation.mechanism == IA64TranslationMechanism::REGION7_IDENTITY);
    assert_equal("region 7 maps the region offset", 0x1234,
                 region7Translation.physicalAddress);

    CPUState region5MissCpu;
    region5MissCpu.SetPSR(psrDt);
    region5MissCpu.SetRR(5, rr5);
    region5MissCpu.SetCR(8, shortVhptPta);
    bool region5Miss = false;
    try {
        translator.TranslateDataAddress(region5MissCpu, trMemory,
            region5 + 0x1234, MemoryAccessType::READ);
    } catch (const IA64TranslationFault& fault) {
        region5Miss = fault.GetKind() == IA64TranslationFaultKind::VHPT_MISS &&
                      fault.GetVectorOffset() == 0 &&
                      fault.GetHashAddress() ==
                          ComputeIA64ShortVhptAddress(region5MissCpu, region5 + 0x1234);
    }
    assert_true("unmapped VHPT page raises the guest VHPT translation vector", region5Miss);

    // TPA is a non-access reference. With VHPT disabled and interruption
    // collection off, IA-64 routes its miss through the nested DTLB vector.
    CPUState nestedTpaCpu;
    nestedTpaCpu.SetPSR(1ULL << 17);
    nestedTpaCpu.SetRR(0, 0x39ULL);
    bool nestedTpaMiss = false;
    try {
        translator.TranslateDataAddress(nestedTpaCpu, trMemory, 0,
                                        MemoryAccessType::READ, true);
    } catch (const IA64TranslationFault& fault) {
        nestedTpaMiss = fault.GetKind() == IA64TranslationFaultKind::NESTED_TLB_MISS &&
                        fault.GetAccessType() == MemoryAccessType::NON_ACCESS &&
                        fault.GetVectorOffset() == 0x1400;
    }
    assert_true("TPA without a VHPT raises a nested DTLB miss when IC is clear",
                nestedTpaMiss);

    // A PTE miss is reported architecturally before a guest physical OOB can
    // occur, and a failed LD8 leaves its destination untouched.
    Memory missingPageTableMemory(0x20000, false);
    CPUState missingPageCpu;
    missingPageCpu.SetPSR(psrDt);
    missingPageCpu.SetRR(5, rr5);
    missingPageCpu.SetCR(8, shortVhptPta);
    missingPageCpu.SetGR(36, region5 + 0x1234);
    missingPageCpu.SetGR(26, 0xAABBCCDDEEFF0011ULL);
    const uint64_t missingHash = ComputeIA64ShortVhptAddress(
        missingPageCpu, region5 + 0x1234);
    const uint64_t missingHashPage = missingHash & ~0x3FFFULL;
    missingPageCpu.InsertDTLB(vhptBackingPa | pageFlags,
                              missingHashPage, rr5Itir, rr5);
    bool pageNotPresent = false;
    try {
        frontierLoad.Execute(missingPageCpu, missingPageTableMemory);
    } catch (const IA64TranslationFault& fault) {
        pageNotPresent = fault.GetKind() == IA64TranslationFaultKind::PAGE_NOT_PRESENT &&
                         fault.GetVectorOffset() == 0x5000;
    }
    assert_true("missing short-format VHPT entry produces page-not-present fault",
                pageNotPresent);
    assert_equal("faulting LD8 preserves its destination register",
                 0xAABBCCDDEEFF0011ULL, missingPageCpu.GetGR(26));

    std::cout << "  ? IA-64 DTR/VHPT region-5 translations passed" << std::endl;
}

void test_ia64_mf_memory_fence() {
    std::cout << "Testing IA-64 M24 memory-fence decoding..." << std::endl;

    InstructionDecoder decoder;
    const InstructionEx mf = decoder.DecodeSlot(
        0x110000000ULL, UnitType::M_UNIT, 0x040D2460ULL);
    assert_true("authentic M24 slot decodes as mf",
                mf.GetType() == InstructionType::MF);
    assert_equal("mf qualifying predicate", 0, mf.GetPredicate());
    assert_string("mf disassembly", "mf", mf.GetDisassembly());

    const InstructionEx mfAcceptance = decoder.DecodeSlot(
        0x118000000ULL, UnitType::M_UNIT, 0x040D24B0ULL);
    assert_true("M24 acceptance-form slot decodes as mf.a",
                mfAcceptance.GetType() == InstructionType::MF_A);
    assert_string("mf.a disassembly", "mf.a", mfAcceptance.GetDisassembly());

    Memory memory(0x1000, false);
    memory.write<uint64_t>(0x100, 0x1122334455667788ULL);
    CPUState cpu;
    cpu.SetGR(32, 0x100);
    mf.Execute(cpu, memory);
    mfAcceptance.Execute(cpu, memory);
    assert_equal("mf and mf.a do not alter coherent guest memory",
                 0x1122334455667788ULL, memory.read<uint64_t>(0x100));

    std::cout << "  ? IA-64 M24 memory-fence decoding passed" << std::endl;
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

void test_ia64_lfetch_nonfaulting_prefetch() {
    std::cout << "Testing IA-64 non-faulting lfetch semantics..." << std::endl;

    // This is the exact slot word at the investigated Linux memset site:
    // lfetch.nt1 [r32].  It has no destination register and must not perform
    // a flat-memory read, even when the virtual alias is outside guest RAM.
    InstructionDecoder decoder;
    const InstructionEx lfetch =
        decoder.DecodeInstruction(0xcb12000000ULL, UnitType::M_UNIT);
    assert_true("lfetch decodes as its own instruction",
                lfetch.GetType() == InstructionType::LFETCH);
    assert_string("lfetch.nt1 disassembly",
                  "lfetch.nt1 [r32]",
                  lfetch.GetDisassembly());

    CPUState cpu;
    cpu.SetGR(32, 0xe000000100000000ULL);
    class CountingMemory final : public Memory {
    public:
        using Memory::Memory;

        mutable size_t readCount = 0;

        void Read(uint64_t address, uint8_t* dest, size_t size) const override {
            ++readCount;
            Memory::Read(address, dest, size);
        }
    } memory(1);
    lfetch.Execute(cpu, memory);
    assert_equal("non-faulting lfetch leaves its base register unchanged",
                 0xe000000100000000ULL,
                 cpu.GetGR(32));
    assert_equal("non-faulting lfetch performs no memory read", size_t(0), memory.readCount);

    std::cout << "  ? IA-64 non-faulting lfetch passed (memoryReads="
              << memory.readCount << ")" << std::endl;
}

void test_ia64_floating_stores_and_spill() {
    std::cout << "Testing IA-64 floating stores and stf.spill semantics..." << std::endl;

    constexpr uint64_t stfSpillF0Raw = 0xEEC9800000ULL;
    constexpr uint64_t region7RamAlias = 0xE000000000000000ULL;
    const std::vector<uint8_t> authenticBundle = {
        0x11, 0x00, 0x00, 0x30, 0xd9, 0x1d,
        0x00, 0x00, 0x00, 0x02, 0x00, 0xa0,
        0x00, 0x00, 0x00, 0x42,
    };

    InstructionDecoder decoder;
    const Bundle decodedBundle = decoder.DecodeBundleAt(
        authenticBundle.data(), 0xA0000001003E8960ULL);
    assert_true("frontier bundle is the MIB-stop template",
                decodedBundle.templateType == TemplateType::MIB_STOP);
    assert_true("frontier bundle has three slots", decodedBundle.instructions.size() == 3);
    const InstructionEx& frontier = decodedBundle.instructions[0];
    assert_true("exact frontier raw word decodes as STF_SPILL",
                frontier.GetType() == InstructionType::STF_SPILL);
    assert_true("frontier slot is an M-unit operation",
                frontier.GetUnit() == UnitType::M_UNIT);
    assert_equal("frontier raw instruction bits", stfSpillF0Raw,
                 frontier.GetRawBits());
    assert_equal("frontier base register", 24, frontier.GetDst());
    assert_equal("frontier source is floating register f0", 0,
                 frontier.GetSrc1());
    assert_true("frontier has an immediate base update", frontier.HasImmediate());
    assert_equal("frontier immediate", 128, frontier.GetImmediate());
    assert_string("frontier operation disassembly",
                  "stf.spill [r24] = f0, 128",
                  frontier.GetDisassembly());

    const uint64_t stf8F0Raw =
        (stfSpillF0Raw & ~(0x3FULL << 30)) | (0x31ULL << 30);
    const InstructionEx stf8 = decoder.DecodeInstruction(stf8F0Raw, UnitType::M_UNIT);
    assert_true("adjacent M10 x6=0x31 decodes as STF8, not an integer store",
                stf8.GetType() == InstructionType::STF8);
    assert_equal("STF8 source remains a floating register", 0, stf8.GetSrc1());
    assert_equal("STF8 immediate update", 128, stf8.GetImmediate());
    const uint64_t stf8F7Raw =
        (stf8F0Raw & ~(0x7FULL << 13)) | (7ULL << 13);
    const InstructionEx stf8F7 =
        decoder.DecodeInstruction(stf8F7Raw, UnitType::M_UNIT);
    assert_true("STF8 source-field variant remains STF8",
                stf8F7.GetType() == InstructionType::STF8);
    assert_equal("STF8 source-field variant selects f7", 7, stf8F7.GetSrc1());

    const uint64_t adjacentSubopcodeRaw =
        (stfSpillF0Raw & ~(0x3FULL << 30)) | (0x3AULL << 30);
    const InstructionEx adjacentSubopcode =
        decoder.DecodeInstruction(adjacentSubopcodeRaw, UnitType::M_UNIT);
    assert_true("neighboring x6=0x3a is not broadened into STF_SPILL or ST8",
                adjacentSubopcode.GetType() == InstructionType::UNKNOWN);

    const uint64_t st8SpillRaw =
        (stfSpillF0Raw & ~(0xFULL << 37)) | (0x5ULL << 37);
    const InstructionEx st8Spill =
        decoder.DecodeInstruction(st8SpillRaw, UnitType::M_UNIT);
    assert_true("M5 x6=0x3b remains in the distinct integer st8 family",
                st8Spill.GetType() == InstructionType::ST8);
    assert_equal("M5 st8 source is its GPR operand", 0, st8Spill.GetSrc1());

    constexpr uint64_t ldfFillRaw = 0xE6C2040280ULL;
    const InstructionEx ldfFill =
        decoder.DecodeInstruction(ldfFillRaw, UnitType::M_UNIT);
    assert_true("M10 x6=0x1b decodes as LDF_FILL",
                ldfFill.GetType() == InstructionType::LDF_FILL);
    assert_equal("LDF_FILL floating destination", 10, ldfFill.GetDst());
    assert_equal("LDF_FILL base register", 32, ldfFill.GetSrc1());
    assert_true("LDF_FILL has a post-increment immediate", ldfFill.HasImmediate());
    assert_equal("LDF_FILL immediate", 32, ldfFill.GetImmediate());
    assert_string("LDF_FILL disassembly",
                  "ldf.fill f10 = [r32], 32",
                  ldfFill.GetDisassembly());

    class CountingMemory final : public Memory {
    public:
        using Memory::Memory;

        std::vector<std::pair<uint64_t, size_t>> writes;
        mutable std::vector<std::pair<uint64_t, size_t>> reads;

        void Write(uint64_t address, const uint8_t* source, size_t size) override {
            writes.emplace_back(address, size);
            Memory::Write(address, source, size);
        }

        void Read(uint64_t address, uint8_t* destination, size_t size) const override {
            reads.emplace_back(address, size);
            Memory::Read(address, destination, size);
        }
    } memory(0x6000);

    CPUState cpu;
    cpu.SetPSR(1ULL << 17);
    uint8_t architecturalF0[16] = {};
    cpu.GetFR(0, architecturalF0);
    assert_true("architectural f0 has fixed +0 register-format contents",
                std::all_of(architecturalF0, architecturalF0 + sizeof(architecturalF0),
                            [](uint8_t value) { return value == 0; }));

    // Execute the exact Linux slot.  The region-7 alias is normalized only
    // for the memory transaction; the architectural base remains virtual.
    cpu.SetGR(24, region7RamAlias + 0x1000);
    frontier.Execute(cpu, memory);
    assert_equal("exact STF_SPILL performs one memory write",
                 1, memory.writes.size());
    assert_equal("exact STF_SPILL effective physical address", 0x1000,
                 memory.writes[0].first);
    assert_equal("exact STF_SPILL writes exactly 16 bytes", 16,
                 memory.writes[0].second);
    uint8_t stored[16] = {};
    memory.Read(0x1000, stored, sizeof(stored));
    assert_true("exact STF_SPILL writes f0's 16 zero bytes",
                std::memcmp(stored, architecturalF0, sizeof(stored)) == 0);
    assert_equal("exact STF_SPILL commits r24 post-increment after the store",
                 region7RamAlias + 0x1080, cpu.GetGR(24));

    // Change only the encoded FP source to f7.  A distinct GPR value proves
    // the execution path reads the FR bank, and the 16 bytes remain opaque.
    const uint64_t stfSpillF7Raw =
        (stfSpillF0Raw & ~(0x7FULL << 13)) | (7ULL << 13);
    const InstructionEx stfSpillF7 =
        decoder.DecodeInstruction(stfSpillF7Raw, UnitType::M_UNIT);
    assert_true("source-field variant remains STF_SPILL",
                stfSpillF7.GetType() == InstructionType::STF_SPILL);
    assert_equal("source-field variant selects f7", 7, stfSpillF7.GetSrc1());
    uint8_t floatingRegister[16] = {};
    for (size_t i = 0; i < sizeof(floatingRegister); ++i) {
        floatingRegister[i] = static_cast<uint8_t>(0x30 + i);
    }
    cpu.SetFR(7, floatingRegister);
    cpu.SetGR(7, 0x8877665544332211ULL);
    cpu.SetGR(24, region7RamAlias + 0x2000);
    stfSpillF7.Execute(cpu, memory);
    assert_equal("f7 spill adds one additional memory transaction",
                 2, memory.writes.size());
    assert_equal("f7 spill writes from its pre-update address", 0x2000,
                 memory.writes[1].first);
    assert_equal("f7 spill transaction is 16 bytes", 16,
                 memory.writes[1].second);
    std::memset(stored, 0, sizeof(stored));
    memory.Read(0x2000, stored, sizeof(stored));
    assert_true("f7 spill stores the exact FR register representation",
                std::memcmp(stored, floatingRegister, sizeof(stored)) == 0);
    assert_equal("f7 spill does not use or modify the GPR source lookalike",
                 0x8877665544332211ULL, cpu.GetGR(7));
    assert_equal("f7 spill commits r24 + 128", region7RamAlias + 0x2080,
                 cpu.GetGR(24));

    // NaTVal is legal for stf.spill: the instruction preserves the raw FR
    // bits and must not perform normal-store NaT consumption.
    uint8_t natVal[16] = {};
    natVal[8] = 0xfe;
    natVal[9] = 0xff;
    natVal[10] = 0x01;
    cpu.SetFR(7, natVal);
    cpu.SetGR(24, region7RamAlias + 0x3000);
    stfSpillF7.Execute(cpu, memory);
    assert_equal("NaTVal stf.spill also performs one 16-byte write",
                 3, memory.writes.size());
    assert_equal("NaTVal spill address", 0x3000, memory.writes[2].first);
    assert_equal("NaTVal spill width", 16, memory.writes[2].second);
    std::memset(stored, 0, sizeof(stored));
    memory.Read(0x3000, stored, sizeof(stored));
    assert_true("stf.spill preserves NaTVal register bits verbatim",
                std::memcmp(stored, natVal, sizeof(stored)) == 0);

    // The neighboring stf8 M10 operation stores only the 64-bit significand.
    cpu.SetFR(7, floatingRegister);
    cpu.SetGR(24, region7RamAlias + 0x4000);
    stf8F7.Execute(cpu, memory);
    assert_equal("stf8 performs one 8-byte store", 4, memory.writes.size());
    assert_equal("stf8 starts at the pre-update base", 0x4000,
                 memory.writes[3].first);
    assert_equal("stf8 access width", 8, memory.writes[3].second);
    uint8_t storedSignificand[8] = {};
    memory.Read(0x4000, storedSignificand, sizeof(storedSignificand));
    assert_true("stf8 stores the FR significand bytes, not a GPR value",
                std::memcmp(storedSignificand, floatingRegister,
                            sizeof(storedSignificand)) == 0);
    assert_equal("stf8 commits its immediate update", region7RamAlias + 0x4080,
                 cpu.GetGR(24));

    // The nine-bit M10 displacement is signed across s:i:imm7a.  Here the
    // encoded value is -1, and address calculation precedes the post-update.
    const uint64_t negativeUpdateRaw = stfSpillF0Raw |
        (1ULL << 36) | (0x7FULL << 6);
    const InstructionEx negativeUpdate =
        decoder.DecodeInstruction(negativeUpdateRaw, UnitType::M_UNIT);
    assert_true("negative-displacement encoding remains STF_SPILL",
                negativeUpdate.GetType() == InstructionType::STF_SPILL);
    assert_true("M10 sign-extends its 9-bit update immediate",
                static_cast<int64_t>(negativeUpdate.GetImmediate()) == -1);
    cpu.SetGR(24, region7RamAlias + 0x5100);
    negativeUpdate.Execute(cpu, memory);
    assert_equal("negative update stores at the original base", 0x5100,
                 memory.writes[4].first);
    assert_equal("negative update still writes 16 bytes", 16,
                 memory.writes[4].second);
    assert_equal("negative update decrements the base only after storing",
                 region7RamAlias + 0x50ff, cpu.GetGR(24));

    cpu.SetGR(24, region7RamAlias + 0x5ff8);
    const uint64_t faultingBase = cpu.GetGR(24);
    bool storeFaulted = false;
    try {
        frontier.Execute(cpu, memory);
    } catch (const std::out_of_range&) {
        storeFaulted = true;
    }
    assert_true("out-of-bounds STF_SPILL raises the memory fault", storeFaulted);
    assert_equal("faulting STF_SPILL does not commit its base update",
                 faultingBase, cpu.GetGR(24));
    assert_equal("faulting STF_SPILL attempts one exact 16-byte transaction",
                 16, memory.writes.back().second);

    // ldf.fill is the architectural inverse of the opaque 16-byte spill and
    // appears on the authentic continuation before the memset spill loop.
    memory.loadBuffer(0x2000, natVal, sizeof(natVal));
    cpu.SetGR(32, region7RamAlias + 0x2000);
    const size_t readsBeforeFill = memory.reads.size();
    ldfFill.Execute(cpu, memory);
    assert_equal("ldf.fill performs one 16-byte guest-memory read",
                 readsBeforeFill + 1, memory.reads.size());
    assert_equal("ldf.fill reads from the pre-update base", 0x2000,
                 memory.reads.back().first);
    assert_equal("ldf.fill read width", 16, memory.reads.back().second);
    uint8_t restoredFloatingRegister[16] = {};
    cpu.GetFR(10, restoredFloatingRegister);
    assert_true("ldf.fill restores all register-format bits including NaTVal",
                std::memcmp(restoredFloatingRegister, natVal,
                            sizeof(restoredFloatingRegister)) == 0);
    assert_equal("ldf.fill commits its +32 base update",
                 region7RamAlias + 0x2020, cpu.GetGR(32));

    const uint64_t ldfFillNegativeRaw = ldfFillRaw |
        (1ULL << 36) | (1ULL << 27) | (0x7FULL << 13);
    const InstructionEx ldfFillNegative =
        decoder.DecodeInstruction(ldfFillNegativeRaw, UnitType::M_UNIT);
    assert_true("LDF_FILL sign-extends the M10 imm9b displacement",
                static_cast<int64_t>(ldfFillNegative.GetImmediate()) == -1);
    memory.loadBuffer(0x3000, floatingRegister, sizeof(floatingRegister));
    cpu.SetGR(32, region7RamAlias + 0x3000);
    const size_t readsBeforeNegativeFill = memory.reads.size();
    ldfFillNegative.Execute(cpu, memory);
    assert_equal("negative LDF_FILL reads before updating the base", 0x3000,
                 memory.reads[readsBeforeNegativeFill].first);
    assert_equal("negative LDF_FILL update is applied after read",
                 region7RamAlias + 0x2fff, cpu.GetGR(32));

    std::cout << "  ? IA-64 floating stores and stf.spill passed" << std::endl;
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

    // Exact Linux kernel instruction at bundle IP 0x43e8780, slot 2.
    // Binutils disassembles raw 0xa05950d309 as:
    //   (p9) tbit.z.unc p12,p11=r21,3
    const uint64_t authenticLinuxTbitZUnc = 0xa05950d309ULL;
    InstructionEx decoded_linux_tbit_unc = decoder.DecodeSlot(
        authenticLinuxTbitZUnc, UnitType::I_UNIT, 0x43e8780);
    assert_true("Authentic Linux TBIT.Z.UNC should decode",
                decoded_linux_tbit_unc.GetType() == InstructionType::TBIT_Z);
    assert_equal("Authentic Linux TBIT.Z.UNC qualifying predicate", 9,
                 decoded_linux_tbit_unc.GetPredicate());
    assert_equal("Authentic Linux TBIT.Z.UNC p1", 12,
                 decoded_linux_tbit_unc.GetDst());
    assert_equal("Authentic Linux TBIT.Z.UNC source", 21,
                 decoded_linux_tbit_unc.GetSrc1());
    assert_equal("Authentic Linux TBIT.Z.UNC p2", 11,
                 decoded_linux_tbit_unc.GetSrc3());
    assert_equal("Authentic Linux TBIT.Z.UNC position", 3,
                 decoded_linux_tbit_unc.GetImmediate());
    assert_true("Authentic Linux TBIT.Z.UNC completer",
                decoded_linux_tbit_unc.GetCompareCompleter() ==
                    CompareCompleter::UNC);
    assert_string("Authentic Linux TBIT.Z.UNC disassembly",
                  "tbit.z.unc p12, p11 = r21, 3",
                  decoded_linux_tbit_unc.GetDisassembly());

    CPUState linuxTbitUncCpu;
    linuxTbitUncCpu.SetPR(9, true);
    linuxTbitUncCpu.SetGR(21, 0);
    decoded_linux_tbit_unc.Execute(linuxTbitUncCpu, memory);
    assert_true("Authentic Linux TBIT.Z.UNC sets p12 for zero bit",
                linuxTbitUncCpu.GetPR(12));
    assert_true("Authentic Linux TBIT.Z.UNC clears p11 for zero bit",
                !linuxTbitUncCpu.GetPR(11));

    InstructionEx decoded_tnat = decoder.DecodeSlot(build_tnat_z_slot(0, 11, 12, 11),
                                                    UnitType::I_UNIT, 0);
    assert_true("TNAT.Z slot should decode", decoded_tnat.GetType() == InstructionType::TNAT_Z);
    assert_equal("TNAT.Z p1 decode", 11, decoded_tnat.GetDst());
    assert_equal("TNAT.Z source decode", 11, decoded_tnat.GetSrc1());
    assert_equal("TNAT.Z p2 decode", 12, decoded_tnat.GetSrc3());

    // Exact Linux kernel instruction at physical IP 0x43e8be0.
    // Binutils disassembles raw 0xb0017031c0 as:
    //   tnat.nz.and p7,p0=r23
    const uint64_t authenticParallelTnat = 0xb0017031c0ULL;
    InstructionEx decoded_parallel_tnat = decoder.DecodeSlot(
        authenticParallelTnat, UnitType::I_UNIT, 0x43e8be0);
    assert_true("Authentic parallel TNAT should decode",
                decoded_parallel_tnat.GetType() == InstructionType::TNAT_NZ);
    assert_equal("Authentic parallel TNAT qualifying predicate", 0,
                 decoded_parallel_tnat.GetPredicate());
    assert_equal("Authentic parallel TNAT p1", 7,
                 decoded_parallel_tnat.GetDst());
    assert_equal("Authentic parallel TNAT source", 23,
                 decoded_parallel_tnat.GetSrc1());
    assert_equal("Authentic parallel TNAT p2", 0,
                 decoded_parallel_tnat.GetSrc3());
    assert_true("Authentic parallel TNAT completer",
                decoded_parallel_tnat.GetCompareCompleter() ==
                    CompareCompleter::AND);
    assert_string("Authentic parallel TNAT disassembly",
                  "tnat.nz.and p7, p0 = r23",
                  decoded_parallel_tnat.GetDisassembly());

    cpu.SetPR(7, true);
    cpu.SetPR(0, true);
    cpu.SetGRNaT(23, true);
    decoded_parallel_tnat.Execute(cpu, memory);
    assert_true("TNAT.NZ.AND true preserves p7", cpu.GetPR(7));
    assert_true("TNAT.NZ.AND true preserves hardwired p0", cpu.GetPR(0));

    cpu.SetPR(7, true);
    cpu.SetPR(0, true);
    cpu.SetGRNaT(23, false);
    decoded_parallel_tnat.Execute(cpu, memory);
    assert_true("TNAT.NZ.AND false clears p7", !cpu.GetPR(7));
    assert_true("TNAT.NZ.AND false preserves hardwired p0", cpu.GetPR(0));

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

void test_popcnt_instruction() {
    std::cout << "Testing POPCNT..." << std::endl;

    InstructionDecoder decoder;
    Memory memory(1024 * 1024);
    CPUState cpu;

    // IA-64 OpZaZbVeX2aX2bX2c(7,0,1,0,1,1,2), popcnt r8=r14.
    const uint64_t rawPopcnt =
        (7ULL << 37) | (1ULL << 33) | (1ULL << 34) |
        (1ULL << 28) | (2ULL << 30) | (8ULL << 6) | (14ULL << 20);
    InstructionEx popcnt = decoder.DecodeSlot(rawPopcnt, UnitType::I_UNIT, 0x3f1310);
    assert_true("POPCNT raw encoding should decode",
                popcnt.GetType() == InstructionType::POPCNT);
    assert_equal("POPCNT destination", 8, popcnt.GetDst());
    assert_equal("POPCNT source", 14, popcnt.GetSrc1());
    assert_string("POPCNT disassembly", "popcnt r8 = r14", popcnt.GetDisassembly());

    cpu.SetGR(14, 0x00000000FFFFFFFFULL);
    popcnt.Execute(cpu, memory);
    assert_equal("POPCNT should count all set low bits", 32, cpu.GetGR(8));

    cpu.SetPR(1, false);
    cpu.SetGR(8, 0xfeedfaceULL);
    popcnt.SetPredicate(1);
    popcnt.Execute(cpu, memory);
    assert_equal("False-predicated POPCNT should preserve destination",
                 0xfeedfaceULL, cpu.GetGR(8));

    std::cout << "  ? POPCNT passed" << std::endl;
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

void test_ia64_virtual_ip_observation() {
    std::cout << "Testing IA-64 virtual IP reads and call links..." << std::endl;

    CPUState cpu;
    Memory memory(1024 * 1024);
    constexpr uint64_t kernelPhysicalIP = 0x04009F00ULL;
    constexpr uint64_t kernelVirtualIP = 0xA000000100009F00ULL;
    cpu.SetIP(kernelPhysicalIP);
    cpu.SetPSR(1ULL << 36);

    InstructionEx movFromIp(InstructionType::MOV_FROM_IP, UnitType::I_UNIT);
    movFromIp.SetOperands(15, 0, 0);
    movFromIp.Execute(cpu, memory);
    assert_equal("mov r15=ip should expose the active kernel virtual IP",
                 kernelVirtualIP, cpu.GetGR(15));

    InstructionEx call(InstructionType::BR_CALL, UnitType::B_UNIT);
    call.SetOperands(0, 0, 0);
    call.Execute(cpu, memory);
    assert_equal("br.call should save a virtual return IP while IT is enabled",
                 kernelVirtualIP + 16, cpu.GetBR(0));

    cpu.SetPSR(0);
    cpu.SetIP(kernelPhysicalIP);
    call.Execute(cpu, memory);
    assert_equal("physical-mode br.call should preserve the physical return IP",
                 kernelPhysicalIP + 16, cpu.GetBR(0));

    std::cout << "  ? IA-64 IP reads and call links reflect translation mode" << std::endl;
}

void test_ia64_bspstore_write() {
    std::cout << "Testing IA-64 BSPSTORE write and dirty-partition preservation..." << std::endl;

    CPUState cpu;
    Memory memory(1024 * 1024);
    InstructionEx writeBspstore(InstructionType::MOV_TO_AR, UnitType::M_UNIT);
    writeBspstore.SetOperands(18, 2, 0);

    cpu.SetRSC(0);
    cpu.SetBSP(0);
    cpu.SetBSPSTORE(0);
    cpu.SetGR(2, 0xA000000100BC0CE7ULL);
    writeBspstore.Execute(cpu, memory);
    assert_equal("BSPSTORE write should ignore low address bits",
                 0xA000000100BC0CE0ULL, cpu.GetBSPSTORE());
    assert_equal("BSPSTORE write with empty dirty partition should move BSP",
                 0xA000000100BC0CE0ULL, cpu.GetBSP());

    // From slot zero, 63 dirty registers occupy 64 backing-store words because
    // the RSE inserts an RNAT collection after each 63-register group.
    cpu.SetBSPSTORE(0x1000);
    cpu.SetBSP(0x1200);
    cpu.SetGR(2, 0x2007);
    writeBspstore.Execute(cpu, memory);
    assert_equal("BSPSTORE write should skip the RNAT collection word",
                 0x2200, cpu.GetBSP());
    assert_equal("BSPSTORE write should store the aligned new pointer",
                 0x2000, cpu.GetBSPSTORE());

    std::cout << "  ? IA-64 BSPSTORE write preserves the dirty RSE partition" << std::endl;
}

void test_ia64_flushrs() {
    std::cout << "Testing IA-64 M0 flushrs decoding and execution..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact Linux entry instruction immediately before loadrs.  Retained
    // Binutils identifies raw 0x141000000 as mov.m ar.rsc=0.
    const uint64_t rawMovRsc = 0x141000000ULL;
    const InstructionEx movRsc = decoder.DecodeSlot(
        rawMovRsc, UnitType::M_UNIT, 0x047f81f0);
    assert_true("authentic mov.m ar.rsc should decode",
                movRsc.GetType() == InstructionType::MOV_TO_AR);
    assert_equal("mov.m ar.rsc predicate", 0, movRsc.GetPredicate());
    assert_equal("mov.m ar.rsc destination", 16, movRsc.GetDst());
    assert_equal("mov.m ar.rsc immediate", 0, movRsc.GetImmediate());
    assert_string("mov.m ar.rsc disassembly", "mov.m ar.rsc = 0", movRsc.GetDisassembly());
    cpu.SetRSC(0x3);
    movRsc.Execute(cpu, memory);
    assert_equal("mov.m ar.rsc should update AR.RSC", 0, cpu.GetRSC());

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
    assert_true("adjacent loadrs encoding should decode",
                adjacentLoadrs.GetType() == InstructionType::LOADRS);
    assert_equal("loadrs predicate", 0, adjacentLoadrs.GetPredicate());
    assert_string("loadrs disassembly", "loadrs", adjacentLoadrs.GetDisassembly());
    cpu.SetRSC(0);
    cpu.SetCFM(0x183);
    adjacentLoadrs.Execute(cpu, memory);
    assert_equal("loadrs with zero count should preserve RSC", 0, cpu.GetRSC());

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

void test_ia64_brp_hint() {
    std::cout << "Testing IA-64 B7 branch-predict hint decoding and execution..." << std::endl;

    InstructionDecoder decoder;
    CPUState cpu;
    Memory memory(1024 * 1024);

    // Exact Linux kernel syllable at physical IP 0x440d80c.  Retained
    // Binutils identifies raw 0xe800000048 as brp.loop.imp.  BRP has no
    // qualifying predicate and no architectural state effect.
    const uint64_t rawBrp = 0xe800000048ULL;
    const InstructionEx brp = decoder.DecodeSlot(rawBrp, UnitType::B_UNIT, 0x440d80c);
    assert_true("authentic brp.loop.imp should decode",
                brp.GetType() == InstructionType::BRP);
    assert_equal("brp raw bits", rawBrp, brp.GetRawBits());
    assert_string("brp disassembly", "brp.loop.imp", brp.GetDisassembly());

    cpu.SetIP(0x440d80c);
    cpu.SetGR(32, 0x1122334455667788ULL);
    cpu.SetBR(6, 0x123450ULL);
    brp.Execute(cpu, memory);
    assert_equal("brp should preserve IP", 0x440d80c, cpu.GetIP());
    assert_equal("brp should preserve GR state", 0x1122334455667788ULL, cpu.GetGR(32));
    assert_equal("brp should preserve branch-register state", 0x123450ULL, cpu.GetBR(6));

    std::cout << "  ? B7 branch-predict hint passed" << std::endl;
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
    assert_true("adjacent loadrs encoding should remain distinct from flushrs",
                loadrs.GetType() == InstructionType::LOADRS);

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

void test_ia64_static_register_banks() {
    std::cout << "Testing IA-64 static register banks..." << std::endl;

    InstructionDecoder decoder;
    InstructionEx bsw0 = decoder.DecodeSlot(0x60000000ULL, UnitType::B_UNIT, 0x1000);
    InstructionEx bsw1 = decoder.DecodeSlot(0x68000000ULL, UnitType::B_UNIT, 0x1000);
    assert_true("bsw.0 should decode", bsw0.GetType() == InstructionType::BSW);
    assert_true("bsw.1 should decode", bsw1.GetType() == InstructionType::BSW);
    assert_equal("bsw.0 is unpredicated", 0, bsw0.GetPredicate());
    assert_equal("bsw.1 is unpredicated", 0, bsw1.GetPredicate());
    assert_equal("bsw.0 selects bank zero", 0, bsw0.GetImmediate());
    assert_equal("bsw.1 selects bank one", 1, bsw1.GetImmediate());
    assert_string("bsw.0 disassembly", "bsw.0", bsw0.GetDisassembly());
    assert_string("bsw.1 disassembly", "bsw.1", bsw1.GetDisassembly());

    CPUState cpu;
    cpu.SetGRPhysical(16, 0x1616ULL);
    cpu.SetGRPhysical(NUM_GENERAL_REGISTERS, 0xB016ULL);
    cpu.SetGRNaTPhysical(NUM_GENERAL_REGISTERS, true);
    cpu.SetPSR((1ULL << 17) | (1ULL << 32));
    assert_equal("bank zero is selected after reset", 0x1616ULL, cpu.GetGR(16));

    Memory memory(0x1000);
    bsw1.Execute(cpu, memory);
    assert_equal("bsw.1 selects the alternate bank", 0xB016ULL, cpu.GetGR(16));
    assert_true("bsw.1 selects the alternate bank NaT bit", cpu.GetGRNaT(16));
    assert_true("bsw.1 preserves unrelated PSR bits",
                (cpu.GetPSR() & ((1ULL << 17) | (1ULL << 32))) ==
                    ((1ULL << 17) | (1ULL << 32)));
    cpu.SetGR(16, 0xB117ULL);
    cpu.SetGRNaT(16, true);

    bsw0.Execute(cpu, memory);
    assert_equal("bsw.0 restores bank-zero contents", 0x1616ULL, cpu.GetGR(16));
    assert_true("bsw.0 leaves the inactive bank's NaT bit intact",
                cpu.GetGRNaTPhysical(NUM_GENERAL_REGISTERS));
    bsw1.Execute(cpu, memory);
    assert_equal("alternate bank writes survive a bank switch", 0xB117ULL, cpu.GetGR(16));

    std::cout << "  ? IA-64 static register banks passed" << std::endl;
}

void test_ia64_banked_context_survives_interruption_return() {
    std::cout << "Testing IA-64 banked static-register context across an RFI..." << std::endl;

    CPUState cpu;
    Memory memory(0x1000);

    // The checkpointed/loader context lives in bank zero while the kernel's
    // interrupted context lives in the alternate bank selected by PSR.bn.
    cpu.SetGRPhysical(16, 0x0000'0000'0000'0016ULL);
    cpu.SetGRPhysical(17, 0x0000'0000'0000'0017ULL);
    cpu.SetGRPhysical(NUM_GENERAL_REGISTERS + 0, 0xB000'0000'0000'0016ULL);
    cpu.SetGRPhysical(NUM_GENERAL_REGISTERS + 1, 0xB000'0000'0000'0017ULL);

    // Run the loader-style prologue with bank zero active and modify bank zero.
    cpu.SetPSR(0);
    assert_equal("bank zero is active before the bank switch",
                 0x0000'0000'0000'0016ULL, cpu.GetGR(16));
    cpu.SetGR(16, 0x0000'0000'0000'00AAULL);
    cpu.SetGR(17, 0x0000'0000'0000'00BBULL);
    assert_equal("bank zero records the loader writes",
                 0x0000'0000'0000'00AAULL, cpu.GetGR(16));

    // rfi reactivates the interrupted bank from IPSR.bn and must expose the
    // alternate bank, not the loader's modified bank zero.
    const uint64_t interruptedPsr = IA64_PSR_BN_MASK;
    cpu.SetCR(16, interruptedPsr);
    cpu.SetCR(19, 0x4000ULL);
    InstructionEx rfi(InstructionType::RFI, UnitType::B_UNIT);
    rfi.Execute(cpu, memory);
    assert_true("rfi switches to the interrupted static bank",
                (cpu.GetPSR() & IA64_PSR_BN_MASK) != 0);
    assert_equal("banked r16 survives an interruption return",
                 0xB000'0000'0000'0016ULL, cpu.GetGR(16));
    assert_equal("banked r17 survives an interruption return",
                 0xB000'0000'0000'0017ULL, cpu.GetGR(17));

    // Returning to bank zero must still observe the loader's values.
    cpu.SetPSR(0);
    assert_equal("bank zero retains the loader value after the return",
                 0x0000'0000'0000'00AAULL, cpu.GetGR(16));

    std::cout << "  ? IA-64 banked static context survives an interruption return"
              << std::endl;
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
        test_ia64_vhpt_instructions();
        test_ia64_data_address_translation();
        test_ia64_mf_memory_fence();
        test_iso_boot_media_direct_path();
        test_fat_boot_media_lookup();
        test_el_torito_fat_boot_media_lookup();
        test_memory_bounds_throw();
        test_ia64_lfetch_nonfaulting_prefetch();
        test_ia64_floating_stores_and_spill();
        test_application_register_moves();
        test_test_instructions();
        test_bitwise_operations();
        test_ia64_immediate_andcm();
        test_subtract_operations();
        test_ia64_immediate_sub_raw_encoding();
        test_ia64_sub_minus_one_raw_encoding();
        test_shift_operations();
        test_popcnt_instruction();
        test_extract_deposit();
        test_memory_operations();
        test_predicated_execution();
        test_alloc_instruction();
        test_rse_state_aliases();
        test_ia64_virtual_ip_observation();
        test_ia64_bspstore_write();
        test_ia64_flushrs();
        test_ia64_brp_hint();
        test_ia64_invala();
        test_alloc_invalid_frame_size_fails_safe();
        test_cmp4_instructions();
        test_ia64_unsigned_fixed_truncate_modulus_sequence();
        test_ia64_unknown_slot_formatter();
        test_ia64_br_ctop_state_machine();
        test_ia64_rotating_register_mapping();
        test_ia64_static_register_banks();
        test_ia64_banked_context_survives_interruption_return();
        
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

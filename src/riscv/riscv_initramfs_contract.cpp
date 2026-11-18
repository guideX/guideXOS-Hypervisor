#include "riscv/riscv_initramfs_contract.h"

#include "riscv_cpu.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <string_view>

namespace riscv {
namespace {

constexpr uint32_t kModeTypeMask = 0170000U;
constexpr uint32_t kModeDirectory = 0040000U;
constexpr uint32_t kModeCharacter = 0020000U;
constexpr uint32_t kModeRegular = 0100000U;
constexpr uint32_t kPmpRead = 0x01U;
constexpr uint32_t kPmpWrite = 0x02U;
constexpr uint32_t kPmpExecute = 0x04U;
constexpr std::array<uint32_t, 64> kSha256RoundConstants{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

struct ParsedArchive {
    std::vector<RiscvNewcEntry> entries;
    uint64_t initEntry = 0;
};

bool ValidateElf(std::span<const uint8_t> elf, uint64_t& entry,
                 std::string& detail);

bool CheckedEnd(uint64_t start, uint64_t size, uint64_t& end) {
    if (size == 0 || size > std::numeric_limits<uint64_t>::max() - start)
        return false;
    end = start + size;
    return true;
}

bool Aligned(uint64_t value, uint64_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1ULL)) == 0 &&
        (value & (alignment - 1ULL)) == 0;
}

bool Overlap(uint64_t leftStart, uint64_t leftEnd,
             uint64_t rightStart, uint64_t rightEnd) {
    return leftStart < rightEnd && rightStart < leftEnd;
}

uint32_t ReadLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t ReadLe64(const uint8_t* bytes) {
    uint64_t result = 0;
    for (size_t index = 0; index < 8; ++index)
        result |= static_cast<uint64_t>(bytes[index]) << (index * 8);
    return result;
}

uint16_t ReadLe16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0]) |
        static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t RotateRight(uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32U - amount));
}

std::string Sha256(std::span<const uint8_t> data) {
    std::array<uint32_t, 8> state{{
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    }};
    const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8ULL;
    const size_t paddedSize = ((data.size() + 9U + 63U) / 64U) * 64U;
    std::vector<uint8_t> padded;
    padded.reserve(paddedSize);
    padded.insert(padded.end(), data.begin(), data.end());
    padded.push_back(0x80);
    padded.resize(paddedSize - 8U, 0);
    for (int shift = 56; shift >= 0; shift -= 8)
        padded.push_back(static_cast<uint8_t>(bitLength >> shift));

    for (size_t block = 0; block < padded.size(); block += 64U) {
        std::array<uint32_t, 64> words{};
        for (size_t index = 0; index < 16; ++index) {
            const uint8_t* item = padded.data() + block + index * 4U;
            words[index] = (static_cast<uint32_t>(item[0]) << 24) |
                (static_cast<uint32_t>(item[1]) << 16) |
                (static_cast<uint32_t>(item[2]) << 8) | item[3];
        }
        for (size_t index = 16; index < words.size(); ++index) {
            const uint32_t x = words[index - 15];
            const uint32_t y = words[index - 2];
            const uint32_t s0 = RotateRight(x, 7) ^ RotateRight(x, 18) ^ (x >> 3);
            const uint32_t s1 = RotateRight(y, 17) ^ RotateRight(y, 19) ^ (y >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (size_t index = 0; index < words.size(); ++index) {
            const uint32_t upper1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^
                                    RotateRight(e, 25);
            const uint32_t choose = (e & f) ^ (~e & g);
            const uint32_t t1 = h + upper1 + choose + kSha256RoundConstants[index] +
                                words[index];
            const uint32_t upper0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^
                                    RotateRight(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = upper0 + majority;
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (const uint32_t word : state) output << std::setw(8) << word;
    return output.str();
}

bool HashesMatch(std::string_view expected, std::string_view actual) {
    if (expected.size() != actual.size()) return false;
    for (size_t index = 0; index < expected.size(); ++index) {
        const auto upper = [](char value) {
            return value >= 'a' && value <= 'f' ? static_cast<char>(value - 'a' + 'A') : value;
        };
        if (upper(expected[index]) != upper(actual[index])) return false;
    }
    return true;
}

bool ReadHexField(std::span<const uint8_t> bytes, size_t offset,
                  uint32_t& value) {
    if (offset > bytes.size() || bytes.size() - offset < 8) return false;
    value = 0;
    for (size_t index = 0; index < 8; ++index) {
        const uint8_t byte = bytes[offset + index];
        uint32_t digit = 0;
        if (byte >= '0' && byte <= '9') digit = byte - '0';
        else if (byte >= 'a' && byte <= 'f') digit = byte - 'a' + 10;
        else if (byte >= 'A' && byte <= 'F') digit = byte - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

bool ParseNewc(std::span<const uint8_t> archive, ParsedArchive& parsed,
               std::string& detail) {
    size_t cursor = 0;
    uint32_t expectedInode = 1;
    while (cursor < archive.size()) {
        if (archive.size() - cursor < 110 ||
            std::string_view(reinterpret_cast<const char*>(archive.data() + cursor), 6) !=
                "070701") {
            detail = "invalid newc magic or truncated 110-byte header";
            return false;
        }
        std::array<uint32_t, 13> fields{};
        for (size_t index = 0; index < fields.size(); ++index) {
            if (!ReadHexField(archive, cursor + 6 + index * 8, fields[index])) {
                detail = "newc header contains a non-hexadecimal field";
                return false;
            }
        }
        const uint32_t inode = fields[0];
        const uint32_t mode = fields[1];
        const uint32_t uid = fields[2];
        const uint32_t gid = fields[3];
        const uint32_t links = fields[4];
        const uint32_t mtime = fields[5];
        const uint32_t fileSize = fields[6];
        const uint32_t deviceMajor = fields[7];
        const uint32_t deviceMinor = fields[8];
        const uint32_t rdevMajor = fields[9];
        const uint32_t rdevMinor = fields[10];
        const uint32_t nameSize = fields[11];
        const uint32_t check = fields[12];
        if (inode != expectedInode++ || uid != 0 || gid != 0 || mtime != 0 ||
            deviceMajor != 0 || deviceMinor != 0 || check != 0 || nameSize < 2) {
            detail = "newc deterministic inode, ownership, time, check, or name metadata is invalid";
            return false;
        }
        const uint64_t nameStart64 = static_cast<uint64_t>(cursor) + 110ULL;
        const uint64_t nameEnd64 = nameStart64 + nameSize;
        if (nameEnd64 > archive.size() || archive[static_cast<size_t>(nameEnd64 - 1)] != 0) {
            detail = "newc pathname is truncated or lacks a NUL terminator";
            return false;
        }
        const size_t nameStart = static_cast<size_t>(nameStart64);
        const size_t nameEnd = static_cast<size_t>(nameEnd64);
        const size_t dataStart = (nameEnd + 3U) & ~size_t{3U};
        if (dataStart < nameEnd || dataStart > archive.size() ||
            fileSize > archive.size() - dataStart) {
            detail = "newc pathname padding or file-data extent overflows the archive";
            return false;
        }
        const size_t dataEnd = dataStart + fileSize;
        const size_t next = (dataEnd + 3U) & ~size_t{3U};
        if (next < dataEnd || next > archive.size()) {
            detail = "newc file-data padding exceeds the archive";
            return false;
        }
        RiscvNewcEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(archive.data() + nameStart),
                          nameSize - 1U);
        entry.inode = inode;
        entry.mode = mode;
        entry.uid = uid;
        entry.gid = gid;
        entry.links = links;
        entry.mtime = mtime;
        entry.rdevMajor = rdevMajor;
        entry.rdevMinor = rdevMinor;
        entry.data.assign(archive.begin() + static_cast<std::ptrdiff_t>(dataStart),
                          archive.begin() + static_cast<std::ptrdiff_t>(dataEnd));
        if (entry.name == "init") {
            if (!ValidateElf(entry.data, parsed.initEntry, detail)) return false;
        } else if (entry.name == "bin/child") {
            uint64_t childEntry = 0;
            if (!ValidateElf(entry.data, childEntry, detail)) {
                detail = "/bin/child " + detail;
                return false;
            }
            if (childEntry != 0x20000ULL) {
                detail = "/bin/child ELF entry must be the independent 0x20000 image";
                return false;
            }
        }
        parsed.entries.push_back(std::move(entry));
        cursor = next;
        if (parsed.entries.back().name == "TRAILER!!!") {
            if (cursor != archive.size()) {
                detail = "bytes follow the newc TRAILER!!! entry";
                return false;
            }
            return true;
        }
    }
    detail = "newc archive has no TRAILER!!! entry";
    return false;
}

bool ValidateElf(std::span<const uint8_t> elf, uint64_t& entry,
                 std::string& detail) {
    if (elf.size() < 64 || elf[0] != 0x7f || elf[1] != 'E' ||
        elf[2] != 'L' || elf[3] != 'F' || elf[4] != 2 || elf[5] != 1 ||
        elf[6] != 1 || elf[7] != 0 || elf[8] != 0) {
        detail = "/init is not ELF64 little-endian System V ABI";
        return false;
    }
    if (ReadLe16(elf.data() + 16) != 2 || ReadLe16(elf.data() + 18) != 243 ||
        ReadLe32(elf.data() + 20) != 1 || ReadLe16(elf.data() + 52) != 64 ||
        ReadLe16(elf.data() + 54) != 56 || ReadLe32(elf.data() + 48) & 1U) {
        detail = "/init ELF type, RISC-V machine, version, header, or ABI flags are invalid";
        return false;
    }
    entry = ReadLe64(elf.data() + 24);
    const uint64_t phoff = ReadLe64(elf.data() + 32);
    const uint16_t phnum = ReadLe16(elf.data() + 56);
    if (phoff > elf.size() ||
        static_cast<uint64_t>(phnum) > (elf.size() - phoff) / 56ULL) {
        detail = "/init program-header table is truncated";
        return false;
    }
    bool entryExecutable = false;
    bool loadFound = false;
    for (uint16_t index = 0; index < phnum; ++index) {
        const uint8_t* header = elf.data() + static_cast<size_t>(phoff +
            static_cast<uint64_t>(index) * 56ULL);
        const uint32_t type = ReadLe32(header);
        const uint32_t flags = ReadLe32(header + 4);
        const uint64_t offset = ReadLe64(header + 8);
        const uint64_t vaddr = ReadLe64(header + 16);
        const uint64_t fileSize = ReadLe64(header + 32);
        const uint64_t memSize = ReadLe64(header + 40);
        const uint64_t alignment = ReadLe64(header + 48);
        if (type == 3 || type == 2) {
            detail = "/init has a dynamic interpreter or dynamic segment";
            return false;
        }
        if (type != 1) continue;
        if (fileSize > memSize || offset > elf.size() || fileSize > elf.size() - offset ||
            memSize > std::numeric_limits<uint64_t>::max() - vaddr ||
            (alignment > 1 && ((alignment & (alignment - 1ULL)) != 0 ||
             ((vaddr - offset) & (alignment - 1ULL)) != 0))) {
            detail = "/init PT_LOAD extent is invalid or overflows";
            return false;
        }
        loadFound = true;
        if ((flags & kPmpExecute) != 0 && vaddr <= entry &&
            entry < vaddr + fileSize) entryExecutable = true;
    }
    if (!loadFound || !entryExecutable) {
        detail = "/init entrypoint is outside an executable PT_LOAD segment";
        return false;
    }
    return true;
}

bool ValidateRequiredEntries(const ParsedArchive& parsed, std::string& detail) {
    static constexpr std::array<std::string_view, 6> processNames{
        ".", "dev", "dev/console", "dev/ttyS0", "init", "TRAILER!!!"};
    static constexpr std::array<std::string_view, 8> execveNames{
        ".", "bin", "dev", "dev/console", "dev/ttyS0", "init",
        "bin/child", "TRAILER!!!"};
    const bool processLayout = parsed.entries.size() == processNames.size() &&
        std::equal(processNames.begin(), processNames.end(), parsed.entries.begin(),
            [](std::string_view name, const RiscvNewcEntry& entry) {
                return name == entry.name;
            });
    const bool execveLayout = parsed.entries.size() == execveNames.size() &&
        std::equal(execveNames.begin(), execveNames.end(), parsed.entries.begin(),
            [](std::string_view name, const RiscvNewcEntry& entry) {
                return name == entry.name;
            });
    if (!processLayout && !execveLayout) {
        detail = "initramfs paths must match the accepted minimal process or two-executable layout";
        return false;
    }
    for (size_t index = 0; index < parsed.entries.size(); ++index) {
        const auto& entry = parsed.entries[index];
        if (entry.inode != index + 1U) {
            detail = "initramfs entries have duplicate or non-deterministic inode numbers";
            return false;
        }
    }
    const auto& root = parsed.entries[0];
    const size_t devIndex = execveLayout ? 2U : 1U;
    const size_t consoleIndex = devIndex + 1U;
    const size_t serialIndex = devIndex + 2U;
    const size_t initIndex = devIndex + 3U;
    const size_t childIndex = initIndex + 1U;
    const size_t trailerIndex = parsed.entries.size() - 1U;
    const auto& dev = parsed.entries[devIndex];
    const auto& console = parsed.entries[consoleIndex];
    const auto& serial = parsed.entries[serialIndex];
    const auto& init = parsed.entries[initIndex];
    const auto& trailer = parsed.entries[trailerIndex];
    if (root.mode != (kModeDirectory | 0755U) || root.links != 2 || !root.data.empty() ||
        dev.mode != (kModeDirectory | 0755U) || dev.links != 2 || !dev.data.empty() ||
        console.mode != (kModeCharacter | 0600U) || console.links != 1 ||
        console.rdevMajor != 5 || console.rdevMinor != 1 || !console.data.empty() ||
        serial.mode != (kModeCharacter | 0600U) || serial.links != 1 ||
        serial.rdevMajor != 4 || serial.rdevMinor != 64 || !serial.data.empty() ||
        root.rdevMajor != 0 || root.rdevMinor != 0 || dev.rdevMajor != 0 ||
        dev.rdevMinor != 0 || init.mode != (kModeRegular | 0755U) ||
        init.links != 1 || init.data.empty() || init.rdevMajor != 0 ||
        init.rdevMinor != 0 || trailer.mode != 0 || trailer.links != 1 ||
        !trailer.data.empty() || trailer.rdevMajor != 0 || trailer.rdevMinor != 0) {
        detail = "initramfs file type, mode, link count, or console/serial device metadata is invalid";
        return false;
    }
    if (execveLayout) {
        const auto& bin = parsed.entries[1];
        const auto& child = parsed.entries[childIndex];
        if (bin.mode != (kModeDirectory | 0755U) || bin.links != 2 || !bin.data.empty() ||
            child.mode != (kModeRegular | 0755U) || child.links != 1 ||
            child.data.empty() || child.rdevMajor != 0 || child.rdevMinor != 0) {
            detail = "two-executable initramfs requires /bin and executable static /bin/child";
            return false;
        }
    }
    for (const auto& entry : parsed.entries) {
        if (entry.uid != 0 || entry.gid != 0 || entry.mtime != 0) {
            detail = "initramfs uid/gid/timestamp must be deterministic zero values";
            return false;
        }
    }
    return true;
}

RiscvInitramfsResult Fail(RiscvInitramfsError error, std::string detail) {
    RiscvInitramfsResult result;
    result.error = error;
    result.detail = std::move(detail);
    return result;
}

} // namespace

const char* RiscvInitramfsErrorToString(RiscvInitramfsError error) {
    switch (error) {
    case RiscvInitramfsError::None: return "none";
    case RiscvInitramfsError::InvalidArchive: return "invalid uncompressed newc archive";
    case RiscvInitramfsError::InvalidContents: return "invalid initramfs contents";
    case RiscvInitramfsError::InvalidElf: return "invalid static RV64 ELF /init";
    case RiscvInitramfsError::InvalidPlacement: return "invalid initramfs physical placement";
    case RiscvInitramfsError::ArithmeticOverflow: return "initramfs range arithmetic overflow";
    case RiscvInitramfsError::OutsideRam: return "initramfs range is outside guest RAM";
    case RiscvInitramfsError::RangeOverlap: return "initramfs overlaps a protected physical range";
    case RiscvInitramfsError::PmpCoverageMissing: return "initramfs is outside accessible PMP RAM";
    case RiscvInitramfsError::MemoryAccessFailure: return "guest physical memory access failed";
    case RiscvInitramfsError::AllocationFailure: return "initramfs staging allocation failed";
    }
    return "unknown initramfs error";
}

RiscvInitramfsResult planRiscvInitramfs(
    std::span<const uint8_t> archive,
    const RiscvInitramfsLoadOptions& options) {
    if (archive.empty() || options.ramSize == 0 ||
        !Aligned(options.archiveAddress, options.requiredAlignment)) {
        return Fail(RiscvInitramfsError::InvalidPlacement,
                    "archive or RAM is empty, or the start address is misaligned");
    }
    const std::string archiveHash = Sha256(archive);
    if (!options.expectedArchiveSha256.empty() &&
        !HashesMatch(options.expectedArchiveSha256, archiveHash)) {
        return Fail(RiscvInitramfsError::InvalidArchive,
                    "archive SHA-256 differs from the pinned RISCV-26 manifest");
    }
    uint64_t archiveEnd = 0;
    uint64_t ramEnd = 0;
    if (!CheckedEnd(options.archiveAddress, archive.size(), archiveEnd) ||
        !CheckedEnd(options.ramBase, options.ramSize, ramEnd)) {
        return Fail(RiscvInitramfsError::ArithmeticOverflow,
                    "archive or RAM exclusive end overflows uint64_t");
    }
    if (options.archiveAddress < options.ramBase || archiveEnd > ramEnd) {
        return Fail(RiscvInitramfsError::OutsideRam,
                    "archive half-open range is not contained in guest RAM");
    }
    for (const auto& range : options.protectedRanges) {
        uint64_t protectedEnd = 0;
        if (!CheckedEnd(range.base, range.size, protectedEnd)) {
            return Fail(RiscvInitramfsError::ArithmeticOverflow,
                        "protected range is empty or its exclusive end overflows");
        }
        if (Overlap(options.archiveAddress, archiveEnd, range.base, protectedEnd)) {
            return Fail(RiscvInitramfsError::RangeOverlap,
                        "archive overlaps protected range " + std::string(range.name));
        }
    }
    const bool pmpCovered = std::any_of(options.pmpRegions.begin(), options.pmpRegions.end(),
        [&](const RiscvInitramfsPmpRegion& region) {
            return region.active && region.lowerBound <= options.archiveAddress &&
                region.upperBound >= archiveEnd &&
                (region.config & (kPmpRead | kPmpWrite | kPmpExecute)) ==
                    (kPmpRead | kPmpWrite | kPmpExecute);
        });
    if (!pmpCovered) {
        return Fail(RiscvInitramfsError::PmpCoverageMissing,
                    "no active PMP region grants read/write/execute over the archive");
    }

    ParsedArchive parsed;
    std::string detail;
    try {
        if (!ParseNewc(archive, parsed, detail))
            return Fail(RiscvInitramfsError::InvalidArchive, std::move(detail));
    } catch (const std::bad_alloc&) {
        return Fail(RiscvInitramfsError::AllocationFailure,
                    "allocation failed while parsing newc entries");
    }
    if (!ValidateRequiredEntries(parsed, detail))
        return Fail(RiscvInitramfsError::InvalidContents, std::move(detail));

    RiscvInitramfsResult result;
    result.plan.address = options.archiveAddress;
    result.plan.length = archive.size();
    result.plan.endExclusive = archiveEnd;
    result.plan.elfEntry = parsed.initEntry;
    result.plan.archiveSha256 = archiveHash;
    result.plan.entries = std::move(parsed.entries);
    return result;
}

RiscvInitramfsResult loadRiscvInitramfs(
    std::span<const uint8_t> archive, RV64CPU& cpu,
    const RiscvInitramfsLoadOptions& options) {
    RiscvInitramfsResult result = planRiscvInitramfs(archive, options);
    if (!result.ok()) return result;
    std::vector<uint8_t> previous;
    try {
        previous.resize(archive.size());
    } catch (const std::bad_alloc&) {
        return Fail(RiscvInitramfsError::AllocationFailure,
                    "allocation failed while staging the destination backup");
    }
    if (!cpu.readPhysical(result.plan.address, previous.data(), previous.size())) {
        return Fail(RiscvInitramfsError::MemoryAccessFailure,
                    "could not read the destination range before commit");
    }
    if (!cpu.writePhysical(result.plan.address, archive.data(), archive.size())) {
        const bool restored = cpu.writePhysical(result.plan.address,
                                                previous.data(), previous.size());
        if (!restored) {
            return Fail(RiscvInitramfsError::MemoryAccessFailure,
                        "archive write and destination rollback both failed");
        }
        return Fail(RiscvInitramfsError::MemoryAccessFailure,
                    "archive write failed; original guest bytes were restored");
    }
    return result;
}

} // namespace riscv

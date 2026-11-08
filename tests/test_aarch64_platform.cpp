#include "aarch64/AArch64DeviceTree.h"
#include "aarch64/AArch64GICv3.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64MMU.h"
#include "aarch64/AArch64PL011.h"
#include "aarch64/AArch64VirtualPlatform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace aarch64;

namespace {

int checks = 0;
int failures = 0;
std::filesystem::path phase13rEvidenceDirectory;

void check(bool value, const char* expression, unsigned line) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "AARCH64-13 FAILED line " << line << ": " << expression << '\n';
    }
}

void checkEq(uint64_t expected, uint64_t actual, const char* expression,
             unsigned line) {
    ++checks;
    if (expected != actual) {
        ++failures;
        std::cerr << "AARCH64-13 FAILED line " << line << ": " << expression
                  << " expected=0x" << std::hex << expected << " actual=0x"
                  << actual << std::dec << '\n';
    }
}

#define CHECK(value) check((value), #value, __LINE__)
#define CHECK_EQ(expected, actual) checkEq((expected), (actual), #expected " == " #actual, __LINE__)

uint32_t be32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

uint64_t be64(const uint8_t* bytes) {
    return (uint64_t(be32(bytes)) << 32) | be32(bytes + 4);
}

struct FdtNode {
    std::string path;
    std::map<std::string, std::vector<uint8_t>> properties;
};

struct RawFdtProperty {
    unsigned nodeOrdinal = 0;
    std::string name;
    std::size_t lengthField = 0;
    std::size_t nameOffsetField = 0;
    std::size_t valueOffset = 0;
    uint32_t length = 0;
};

// Test-only location index for deterministic malformed-DTB controls. Guest
// discovery itself is executed by the A64 words below and does not call this.
std::vector<RawFdtProperty> rawFdtProperties(const std::vector<uint8_t>& blob) {
    std::vector<RawFdtProperty> properties;
    if (blob.size() < 40 || be32(blob.data()) != 0xD00DFEEDu) return properties;
    const std::size_t structStart = be32(blob.data() + 8);
    const std::size_t structEnd = structStart + be32(blob.data() + 36);
    const std::size_t stringsStart = be32(blob.data() + 12);
    const std::size_t stringsEnd = stringsStart + be32(blob.data() + 32);
    if (structStart > blob.size() || structEnd > blob.size() || stringsEnd > blob.size()) return properties;
    std::size_t cursor = structStart;
    unsigned nodeOrdinal = 0;
    while (cursor + 4 <= structEnd) {
        const uint32_t token = be32(blob.data() + cursor);
        cursor += 4;
        if (token == 1) {
            ++nodeOrdinal;
            while (cursor < structEnd && blob[cursor] != 0) ++cursor;
            if (cursor == structEnd) break;
            ++cursor;
            cursor = (cursor + 3u) & ~std::size_t{3};
        } else if (token == 2 || token == 4) {
            continue;
        } else if (token == 3) {
            if (cursor + 8 > structEnd) break;
            const uint32_t length = be32(blob.data() + cursor);
            const uint32_t nameOffset = be32(blob.data() + cursor + 4);
            const std::size_t lengthField = cursor;
            const std::size_t nameOffsetField = cursor + 4;
            const std::size_t valueOffset = cursor + 8;
            cursor = valueOffset;
            if (length > structEnd - cursor || nameOffset >= stringsEnd - stringsStart) break;
            std::size_t nameEnd = stringsStart + nameOffset;
            while (nameEnd < stringsEnd && blob[nameEnd] != 0) ++nameEnd;
            if (nameEnd == stringsEnd) break;
            properties.push_back({nodeOrdinal,
                std::string(reinterpret_cast<const char*>(blob.data() + stringsStart + nameOffset),
                            nameEnd - (stringsStart + nameOffset)),
                lengthField, nameOffsetField, valueOffset, length});
            cursor = (cursor + length + 3u) & ~std::size_t{3};
        } else if (token == 9) {
            break;
        } else {
            break;
        }
    }
    return properties;
}

const RawFdtProperty* findRawProperty(const std::vector<RawFdtProperty>& properties,
                                      unsigned nodeOrdinal, const std::string& name) {
    const auto found = std::find_if(properties.begin(), properties.end(),
        [&](const RawFdtProperty& property) {
            return property.nodeOrdinal == nodeOrdinal && property.name == name;
        });
    return found == properties.end() ? nullptr : &*found;
}

void putBe32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

std::size_t findFdtString(const std::vector<uint8_t>& blob, const std::string& wanted) {
    const std::size_t stringsStart = be32(blob.data() + 12);
    const std::size_t stringsBytes = be32(blob.data() + 32);
    for (std::size_t at = 0; at + wanted.size() < stringsBytes; ++at) {
        if (std::equal(wanted.begin(), wanted.end(), blob.begin() +
                static_cast<std::ptrdiff_t>(stringsStart + at)) &&
            blob[stringsStart + at + wanted.size()] == 0) return at;
    }
    throw std::logic_error("test could not find FDT property string: " + wanted);
}

// A small independent FDT-v17 reader used as a qualification oracle. It does
// not call the production writer or use the writer's string/structure helpers.
class IndependentFdtReader final {
public:
    bool parse(const std::vector<uint8_t>& blob, std::string& error) {
        nodes_.clear();
        error.clear();
        if (blob.size() < 40 || be32(blob.data()) != 0xD00DFEEDu) {
            error = "bad or truncated FDT header";
            return false;
        }
        const uint32_t total = be32(blob.data() + 4);
        const uint32_t offStruct = be32(blob.data() + 8);
        const uint32_t offStrings = be32(blob.data() + 12);
        const uint32_t offReservations = be32(blob.data() + 16);
        const uint32_t version = be32(blob.data() + 20);
        const uint32_t lastCompatible = be32(blob.data() + 24);
        const uint32_t stringsBytes = be32(blob.data() + 32);
        const uint32_t structBytes = be32(blob.data() + 36);
        if (total != blob.size() || version != 17 || lastCompatible != 16 ||
            (offStruct & 3u) != 0 || (offReservations & 7u) != 0 ||
            offStruct < 40 || offStrings < 40 || offReservations < 40 ||
            offStruct > total || structBytes > total - offStruct ||
            offStrings > total || stringsBytes > total - offStrings) {
            error = "invalid FDT version, total size, alignment, or section bounds";
            return false;
        }
        const uint64_t structEnd = uint64_t(offStruct) + structBytes;
        const uint64_t stringsEnd = uint64_t(offStrings) + stringsBytes;
        if (offStruct < stringsEnd && offStrings < structEnd) {
            error = "FDT structure and strings sections overlap";
            return false;
        }
        if (structEnd > total || stringsEnd > total) {
            error = "FDT section extends beyond totalsize";
            return false;
        }
        uint32_t reservationCursor = offReservations;
        bool reservationEndSeen = false;
        while (reservationCursor <= total && total - reservationCursor >= 16) {
            const uint64_t address = be64(blob.data() + reservationCursor);
            const uint64_t size = be64(blob.data() + reservationCursor + 8);
            reservationCursor += 16;
            if (address == 0 && size == 0) {
                reservationEndSeen = true;
                break;
            }
            if (size == 0 || size > UINT64_MAX - address) {
                error = "invalid FDT memory reservation entry";
                return false;
            }
        }
        if (!reservationEndSeen || reservationCursor > offStruct ||
            reservationCursor > offStrings) {
            error = "missing or overlapping FDT reservation terminator";
            return false;
        }

        const std::size_t structStart = offStruct;
        const std::size_t structLimit = static_cast<std::size_t>(structEnd);
        const std::size_t stringsStart = offStrings;
        const std::size_t stringsLimit = static_cast<std::size_t>(stringsEnd);
        std::vector<std::string> parents;
        std::size_t cursor = structStart;
        bool rootSeen = false;
        bool endSeen = false;
        while (cursor < structLimit) {
            if (structLimit - cursor < 4) {
                error = "truncated FDT structure token";
                return false;
            }
            const uint32_t token = be32(blob.data() + cursor);
            cursor += 4;
            if (token == 1) { // FDT_BEGIN_NODE
                std::size_t nameEnd = cursor;
                while (nameEnd < structLimit && blob[nameEnd] != 0) ++nameEnd;
                if (nameEnd == structLimit) {
                    error = "unterminated FDT node name";
                    return false;
                }
                std::string name(reinterpret_cast<const char*>(blob.data() + cursor), nameEnd - cursor);
                cursor = (nameEnd + 4u) & ~std::size_t{3};
                if (cursor > structLimit) {
                    error = "FDT node name padding exceeds structure section";
                    return false;
                }
                std::string path;
                if (parents.empty()) {
                    if (rootSeen || !name.empty()) {
                        error = "FDT root node is duplicated or has a name";
                        return false;
                    }
                    rootSeen = true;
                    path = "/";
                } else {
                    path = parents.back() == "/" ? "/" + name : parents.back() + "/" + name;
                }
                if (nodes_.find(path) != nodes_.end()) {
                    error = "duplicate FDT node path";
                    return false;
                }
                nodes_.emplace(path, FdtNode{path, {}});
                parents.push_back(path);
            } else if (token == 2) { // FDT_END_NODE
                if (parents.empty()) {
                    error = "unbalanced FDT_END_NODE";
                    return false;
                }
                parents.pop_back();
            } else if (token == 3) { // FDT_PROP
                if (parents.empty() || structLimit - cursor < 8) {
                    error = "property outside a node or truncated property header";
                    return false;
                }
                const uint32_t length = be32(blob.data() + cursor);
                const uint32_t nameOffset = be32(blob.data() + cursor + 4);
                cursor += 8;
                if (length > structLimit - cursor || nameOffset >= stringsBytes) {
                    error = "property value or name offset exceeds its FDT block";
                    return false;
                }
                std::size_t propertyNameEnd = stringsStart + nameOffset;
                while (propertyNameEnd < stringsLimit && blob[propertyNameEnd] != 0)
                    ++propertyNameEnd;
                if (propertyNameEnd == stringsLimit) {
                    error = "FDT property name is not NUL terminated";
                    return false;
                }
                const std::string propertyName(
                    reinterpret_cast<const char*>(blob.data() + stringsStart + nameOffset),
                    propertyNameEnd - (stringsStart + nameOffset));
                const std::vector<uint8_t> value(blob.begin() + static_cast<std::ptrdiff_t>(cursor),
                    blob.begin() + static_cast<std::ptrdiff_t>(cursor + length));
                auto& properties = nodes_.at(parents.back()).properties;
                if (propertyName.empty() || !properties.emplace(propertyName, value).second) {
                    error = "empty or duplicate FDT property name";
                    return false;
                }
                cursor = (cursor + length + 3u) & ~std::size_t{3};
                if (cursor > structLimit) {
                    error = "FDT property padding exceeds structure section";
                    return false;
                }
            } else if (token == 9) { // FDT_END
                if (!parents.empty() || !rootSeen) {
                    error = "FDT_END precedes root closure";
                    return false;
                }
                endSeen = true;
                if (cursor != structLimit) {
                    error = "unexpected bytes follow FDT_END";
                    return false;
                }
                break;
            } else {
                error = "unsupported or malformed FDT structure token";
                return false;
            }
        }
        if (!rootSeen || !endSeen || !parents.empty()) {
            error = "FDT structure is incomplete";
            return false;
        }
        return true;
    }

    const FdtNode* node(const std::string& path) const {
        const auto found = nodes_.find(path);
        return found == nodes_.end() ? nullptr : &found->second;
    }

    const std::vector<uint8_t>* property(const std::string& path,
                                        const std::string& name) const {
        const FdtNode* found = node(path);
        if (found == nullptr) return nullptr;
        const auto prop = found->properties.find(name);
        return prop == found->properties.end() ? nullptr : &prop->second;
    }

private:
    std::map<std::string, FdtNode> nodes_;
};

bool propertyCellEquals(const IndependentFdtReader& reader,
                        const std::string& path, const std::string& name,
                        uint32_t expected) {
    const auto* value = reader.property(path, name);
    return value != nullptr && value->size() == 4 && be32(value->data()) == expected;
}

bool propertyStringEquals(const IndependentFdtReader& reader,
                          const std::string& path, const std::string& name,
                          const std::string& expected) {
    const auto* value = reader.property(path, name);
    return value != nullptr && value->size() == expected.size() + 1 &&
           std::equal(expected.begin(), expected.end(), value->begin()) &&
           value->back() == 0;
}

bool writeArtifact(const std::filesystem::path& path,
                   const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

bool writeTextArtifact(const std::filesystem::path& path,
                       const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return file.good();
}

std::vector<uint32_t> discoveryGuest() {
    // A64 words independently checked through Cpu::decodeWord and executed by
    // the production CPU. The guest reads the generated FDT via X0, checks a
    // part of the magic, and records its result in X3.
    return {0xB9400001u, // LDR W1, [X0]
            0x12003C22u, // AND W2, W1, #0xffff
            0x7137405Fu, // CMP W2, #0x0dd0
            0x54000061u, // B.NE fail (index 6)
            0x52800023u, // MOVZ W3, #1
            0x14000002u, // B done (index 7)
            0x52800003u, // fail: MOVZ W3, #0
            0xD503201Fu}; // done: NOP
}

uint32_t movzX(unsigned rd, uint16_t immediate, unsigned half = 0) {
    return 0xD2800000u | ((half & 3u) << 21) |
           (uint32_t(immediate) << 5) | (rd & 31u);
}

uint32_t movzW(unsigned rd, uint16_t immediate) {
    return 0x52800000u | (uint32_t(immediate) << 5) | (rd & 31u);
}

uint32_t movkX(unsigned rd, uint16_t immediate, unsigned half) {
    return 0xF2800000u | ((half & 3u) << 21) |
           (uint32_t(immediate) << 5) | (rd & 31u);
}

uint32_t strW(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xB9000000u | ((offset / 4u) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}

std::vector<uint32_t> uartGuest(uint64_t uartBase) {
    std::vector<uint32_t> words;
    words.push_back(movzX(10, static_cast<uint16_t>(uartBase), 0));
    for (unsigned half = 1; half < 4; ++half) {
        const uint16_t part = static_cast<uint16_t>(uartBase >> (half * 16));
        if (part != 0) words.push_back(movkX(10, part, half));
    }
    words.push_back(movzW(11, 0x70));
    words.push_back(strW(11, 10, AArch64PL011::UARTLCR_H));
    words.push_back(movzW(11, 0x301));
    words.push_back(strW(11, 10, AArch64PL011::UARTCR));
    words.push_back(movzW(11, 'O'));
    words.push_back(strW(11, 10, AArch64PL011::UARTDR));
    words.push_back(movzW(11, 'K'));
    words.push_back(strW(11, 10, AArch64PL011::UARTDR));
    words.push_back(0xD503201Fu);
    return words;
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> image;
    image.reserve(words.size() * 4);
    for (uint32_t word : words) {
        image.push_back(static_cast<uint8_t>(word));
        image.push_back(static_cast<uint8_t>(word >> 8));
        image.push_back(static_cast<uint8_t>(word >> 16));
        image.push_back(static_cast<uint8_t>(word >> 24));
    }
    return image;
}

// This narrow fixture builder emits only the instruction forms needed by the
// integration guest: bounded branches, loads/stores, arithmetic and moves,
// system registers, exception return, REV, and barriers. It is not a general
// assembler; encoders are intentionally local to this test guest.
class PlatformGuestBuilder final {
public:
    enum class BranchKind { B, BCond, Cbz, Cbnz };
    struct Fixup { std::size_t at; std::string label; BranchKind kind; unsigned value; };

    void label(const std::string& name) {
        labels_[name] = words_.size();
    }
    void word(uint32_t value) { words_.push_back(value); }
    void branch(const std::string& name) { fixups_.push_back({words_.size(), name, BranchKind::B, 0}); word(0); }
    void branchCond(unsigned condition, const std::string& name) {
        fixups_.push_back({words_.size(), name, BranchKind::BCond, condition}); word(0);
    }
    void cbz(unsigned reg, const std::string& name) {
        fixups_.push_back({words_.size(), name, BranchKind::Cbz, reg}); word(0);
    }
    void cbnz(unsigned reg, const std::string& name) {
        fixups_.push_back({words_.size(), name, BranchKind::Cbnz, reg}); word(0);
    }
    std::size_t labelOffset(const std::string& name) const {
        return labels_.at(name) * 4;
    }
    std::vector<uint32_t> finish() const {
        std::vector<uint32_t> result = words_;
        for (const Fixup& fixup : fixups_) {
            const auto target = labels_.find(fixup.label);
            if (target == labels_.end()) throw std::logic_error("unbound guest label: " + fixup.label);
            const int64_t delta = static_cast<int64_t>(target->second) - static_cast<int64_t>(fixup.at);
            if (fixup.kind == BranchKind::B) {
                if (delta < -(1 << 25) || delta >= (1 << 25)) throw std::logic_error("guest B branch out of range");
                result[fixup.at] = 0x14000000u | (static_cast<uint32_t>(delta) & 0x03FFFFFFu);
            } else {
                if (delta < -(1 << 18) || delta >= (1 << 18)) throw std::logic_error("guest conditional branch out of range");
                const uint32_t imm19 = (static_cast<uint32_t>(delta) & 0x7FFFFu) << 5;
                if (fixup.kind == BranchKind::BCond)
                    result[fixup.at] = 0x54000000u | imm19 | (fixup.value & 15u);
                else
                    result[fixup.at] = (fixup.kind == BranchKind::Cbz ? 0x34000000u : 0x35000000u) |
                        imm19 | (fixup.value & 31u);
            }
        }
        return result;
    }
private:
    std::vector<uint32_t> words_;
    std::map<std::string, std::size_t> labels_;
    std::vector<Fixup> fixups_;
};

constexpr unsigned kCondEq = 0;
constexpr unsigned kCondNe = 1;
constexpr unsigned kCondLo = 3;
constexpr unsigned kCondHi = 8;

uint32_t addW(unsigned rd, unsigned rn, unsigned imm) {
    return 0x11000000u | ((imm & 0xFFFu) << 10) | ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t addX(unsigned rd, unsigned rn, unsigned imm) {
    return 0x91000000u | ((imm & 0xFFFu) << 10) | ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t subsW(unsigned rd, unsigned rn, unsigned imm) {
    return 0x71000000u | ((imm & 0xFFFu) << 10) | ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t cmpW(unsigned rn, unsigned imm, bool shift12 = false) {
    return 0x7100001Fu | (shift12 ? 0x00400000u : 0u) |
           ((imm & 0xFFFu) << 10) | ((rn & 31u) << 5);
}
uint32_t ldrW(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xB9400000u | (((offset / 4u) & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}
uint32_t strX(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xF9000000u | (((offset / 8u) & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}
uint32_t ldrX(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0xF9400000u | (((offset / 8u) & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}
uint32_t stpXPre(unsigned rt, unsigned rt2, int offset) {
    return 0xA9800000u | ((static_cast<uint32_t>(offset / 8) & 0x7Fu) << 15) |
           ((rt2 & 31u) << 10) | (31u << 5) | (rt & 31u);
}
uint32_t ldpXPost(unsigned rt, unsigned rt2, unsigned offset) {
    return 0xA8C00000u | (((offset / 8u) & 0x7Fu) << 15) |
           ((rt2 & 31u) << 10) | (31u << 5) | (rt & 31u);
}
uint32_t movkW(unsigned rd, uint16_t imm, unsigned half = 1) {
    return 0x72800000u | ((half & 1u) << 21) |
           (static_cast<uint32_t>(imm) << 5) | (rd & 31u);
}
uint32_t ldrB(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0x39400000u | ((offset & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}
uint32_t revW(unsigned rd, unsigned rn) {
    return 0x5AC00800u | ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t addXShift12(unsigned rd, unsigned rn, unsigned imm) {
    return 0x91400000u | ((imm & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rd & 31u);
}
uint32_t strB(unsigned rt, unsigned rn, unsigned offset = 0) {
    return 0x39000000u | ((offset & 0xFFFu) << 10) |
           ((rn & 31u) << 5) | (rt & 31u);
}
uint16_t guestSyskey(uint8_t op0, uint8_t op1, uint8_t crn,
                     uint8_t crm, uint8_t op2) {
    return static_cast<uint16_t>((op0 << 14) | (op1 << 11) |
                                 (crn << 7) | (crm << 3) | op2);
}
uint32_t guestMrs(uint16_t key, unsigned rt) {
    return 0xD5200000u | (uint32_t((key >> 14) & 3u) << 19) |
           (uint32_t((key >> 11) & 7u) << 16) |
           (uint32_t((key >> 7) & 15u) << 12) |
           (uint32_t((key >> 3) & 15u) << 8) |
           (uint32_t(key & 7u) << 5) | (rt & 31u);
}
uint32_t guestMsr(uint16_t key, unsigned rt) {
    return 0xD5000000u | (uint32_t((key >> 14) & 3u) << 19) |
           (uint32_t((key >> 11) & 7u) << 16) |
           (uint32_t((key >> 7) & 15u) << 12) |
           (uint32_t((key >> 3) & 15u) << 8) |
           (uint32_t(key & 7u) << 5) | (rt & 31u);
}

void emitGuestConstantX(PlatformGuestBuilder& guest, unsigned rd, uint64_t value) {
    guest.word(movzX(rd, static_cast<uint16_t>(value), 0));
    for (unsigned half = 1; half < 4; ++half) {
        const uint16_t part = static_cast<uint16_t>(value >> (half * 16));
        if (part != 0) guest.word(movkX(rd, part, half));
    }
}

void emitGuestConstantW(PlatformGuestBuilder& guest, unsigned rd, uint32_t value) {
    guest.word(movzW(rd, static_cast<uint16_t>(value)));
    const uint16_t upper = static_cast<uint16_t>(value >> 16);
    if (upper != 0) guest.word(movkW(rd, upper));
}

struct IntegratedGuestOffsets {
    std::size_t mmuEnabled = 0;
    std::size_t timerWait = 0;
    std::size_t uartWait = 0;
    std::size_t completion = 0;
};

void emitIntegratedIrqHandler(PlatformGuestBuilder& handler) {
    handler.word(stpXPre(0, 1, -16));
    handler.word(stpXPre(9, 10, -16));
    handler.word(guestMrs(gicv3_sysreg::kIar1El1, 0));
    handler.word(cmpW(0, AArch64GICv3::kPhysicalTimerPpi));
    handler.branchCond(kCondEq, "timer_irq");
    handler.word(cmpW(0, 33)); handler.branchCond(kCondEq, "uart_irq");
    handler.word(cmpW(0, 34)); handler.branchCond(kCondEq, "uart_irq");
    handler.branch("eoi");
    handler.label("timer_irq");
    handler.word(movzX(1, 0));
    handler.word(guestMsr(timer_sysreg::kCntpCtlEl0, 1));
    handler.word(movzX(9, 0x3000, 1));
    handler.word(movzW(1, 1));
    handler.word(strW(1, 9, 0x40));
    handler.branch("eoi");
    handler.label("uart_irq");
    handler.word(movzX(10, 0x2000, 1));
    handler.word(ldrW(1, 10, AArch64PL011::UARTDR));
    handler.word(movzX(9, 0x3000, 1));
    handler.word(strW(1, 9, 0x44));
    handler.label("eoi");
    handler.word(guestMsr(gicv3_sysreg::kEoir1El1, 0));
    handler.word(ldpXPost(9, 10, 16));
    handler.word(ldpXPost(0, 1, 16));
    handler.word(0xD69F03E0u); // ERET
}

void emitIntegratedGicSetup(PlatformGuestBuilder& g,
                            const AArch64PlatformConfig& layout) {
    // The single-PE platform profile assigns the architectural physical and
    // virtual timer PPIs 30/27; bind the parsed three-cell FDT values to the
    // guest's corresponding GIC configuration before using those bit masks.
    g.word(cmpW(26, AArch64GICv3::kPhysicalTimerPpi));
    g.branchCond(kCondNe, "fail");
    g.word(cmpW(27, AArch64GICv3::kVirtualTimerPpi));
    g.branchCond(kCondNe, "fail");
    emitGuestConstantW(g, 11, kPstateI);
    g.word(guestMsr(guestSyskey(3, 3, 4, 2, 1), 11)); // mask IRQ during setup
    emitGuestConstantX(g, 11, layout.exceptionVectorBase);
    g.word(guestMsr(guestSyskey(3, 0, 12, 0, 0), 11)); // VBAR_EL1

    g.word(addX(12, 24, 0x14));
    g.word(movzW(11, 0)); g.word(strW(11, 12)); // wake the one Redistributor
    g.word(addXShift12(12, 24, 0x10)); g.word(addX(12, 12, 0x80));
    emitGuestConstantW(g, 11, 1u << AArch64GICv3::kPhysicalTimerPpi);
    g.word(strW(11, 12)); // IGROUPR0: Group 1 PPI 30
    g.word(addX(12, 12, 0x80)); g.word(strW(11, 12)); // ISENABLER0
    g.word(addXShift12(12, 24, 0x10)); g.word(addX(12, 12, 0x41E));
    g.word(movzW(11, 0x20)); g.word(strB(11, 12)); // IPRIORITYR30

    g.word(cmpW(25, 33)); g.branchCond(kCondEq, "uart_spi_33");
    g.word(cmpW(25, 34)); g.branchCond(kCondEq, "uart_spi_34");
    g.branch("fail");
    g.label("uart_spi_33"); g.word(movzW(11, 2)); g.branch("uart_spi_ready");
    g.label("uart_spi_34"); g.word(movzW(11, 4));
    g.label("uart_spi_ready");
    g.word(addX(12, 23, 0x84)); g.word(strW(11, 12)); // IGROUPR SPI33/34
    g.word(addX(12, 23, 0x104)); g.word(strW(11, 12)); // ISENABLER
    g.word(cmpW(25, 33)); g.branchCond(kCondEq, "uart_pri_33");
    g.word(addX(12, 23, 0x422)); g.branch("uart_pri_ready");
    g.label("uart_pri_33"); g.word(addX(12, 23, 0x421));
    g.label("uart_pri_ready");
    g.word(movzW(11, 0x20)); g.word(strB(11, 12)); // SPI priority
    g.word(cmpW(25, 33)); g.branchCond(kCondEq, "uart_route_33");
    g.word(addXShift12(12, 23, 6)); g.word(addX(12, 12, 0x110));
    g.branch("uart_route_ready");
    g.label("uart_route_33");
    g.word(addXShift12(12, 23, 6)); g.word(addX(12, 12, 0x108));
    g.label("uart_route_ready");
    g.word(strX(31, 12)); // IROUTER affinity 0
    g.word(addX(12, 23, 0)); g.word(movzW(11, 0x12));
    g.word(strW(11, 12)); // Distributor Group 1 enable, DS/ARE RAO/WI

    g.word(movzW(11, 1)); g.word(guestMsr(gicv3_sysreg::kSreEl1, 11));
    g.word(movzW(11, 0xFF)); g.word(guestMsr(gicv3_sysreg::kPmrEl1, 11));
    g.word(movzW(11, 0)); g.word(guestMsr(gicv3_sysreg::kBpr1El1, 11));
    g.word(movzW(11, 1)); g.word(guestMsr(gicv3_sysreg::kIgrpEn1El1, 11));

    // The guest itself installs the complete bounded IRQ vector at VBAR+0x280.
    PlatformGuestBuilder handler;
    emitIntegratedIrqHandler(handler);
    const std::vector<uint32_t> handlerWords = handler.finish();
    emitGuestConstantX(g, 12, layout.exceptionVectorBase + 0x280);
    for (std::size_t index = 0; index < handlerWords.size(); ++index) {
        emitGuestConstantW(g, 11, handlerWords[index]);
        g.word(strW(11, 12, static_cast<unsigned>(index * 4)));
    }
}

void emitGuestPageTablesAndEnableMmu(PlatformGuestBuilder& g,
                                    const AArch64PlatformConfig& layout) {
    const uint64_t root = layout.pageTableBase;
    const uint64_t l1 = root + 0x1000;
    const uint64_t identityL3 = root + 0x2000;
    const uint64_t gicL3 = root + 0x3000;
    const uint64_t uartL3 = root + 0x4000;
    const uint64_t dataL3 = root + 0x5000;
    const uint64_t gicAlias = 0x21000000;
    const uint64_t uartAlias = 0x20000000;
    const uint64_t dataAlias = 0x30000000;
    auto tableStore = [&](unsigned tableReg, unsigned offset, uint64_t value) {
        emitGuestConstantX(g, 11, value);
        g.word(strX(11, tableReg, offset));
    };

    emitGuestConstantX(g, 4, root);
    emitGuestConstantX(g, 5, l1);
    emitGuestConstantX(g, 7, identityL3);
    tableStore(4, 0, l1 | 3u);
    tableStore(5, 0, identityL3 | 3u);
    tableStore(5, static_cast<unsigned>((gicAlias >> 21) * 8), gicL3 | 3u);
    tableStore(5, static_cast<unsigned>((uartAlias >> 21) * 8), uartL3 | 3u);
    tableStore(5, static_cast<unsigned>((dataAlias >> 21) * 8), dataL3 | 3u);
    const uint64_t ramBlockCount = layout.ramBytes / 0x200000;
    for (uint64_t block = 1; block < ramBlockCount && block < 512; ++block)
        tableStore(5, static_cast<unsigned>(block * 8),
                   (block * 0x200000) | 0x701u); // Normal-memory 2 MiB block.

    // Identity map the first 2 MiB of normal guest RAM, including code,
    // vectors, DTB, guest data and the page tables constructed above.
    emitGuestConstantX(g, 8, identityL3);
    for (unsigned page = 0; page < 512; ++page) {
        emitGuestConstantX(g, 11, (uint64_t(page) << 12) | 0x703u);
        g.word(strX(11, 8, page * 8));
    }

    // Alias GICD and GICR under 0x21000000 with Device-nGnRnE page entries.
    emitGuestConstantX(g, 8, gicL3);
    for (unsigned page = 0; page < 16; ++page) {
        g.word(addXShift12(11, 23, page));
        g.word(addX(11, 11, 0x707)); // AttrIdx=1 Device-nGnRnE, AF, inner shareable.
        g.word(strX(11, 8, page * 8));
    }
    const uint64_t gicrOffsetPages = (layout.gicrBase - layout.gicdBase) / 4096;
    for (unsigned page = 0; page < 32; ++page) {
        g.word(addXShift12(11, 24, page));
        g.word(addX(11, 11, 0x707));
        g.word(strX(11, 8, static_cast<unsigned>((gicrOffsetPages + page) * 8)));
    }

    // The same UART is accessed after enabling translation through a
    // non-identity virtual alias and retains its DTB-discovered physical base.
    emitGuestConstantX(g, 8, uartL3);
    g.word(addX(11, 10, 0x707));
    g.word(strX(11, 8));
    emitGuestConstantX(g, 8, dataL3);
    emitGuestConstantX(g, 11, layout.guestDataBase + 0x703);
    g.word(strX(11, 8));

    emitGuestConstantX(g, 11, 0xFF); // MAIR: Attr0 Normal WB, Attr1 Device-nGnRnE.
    g.word(guestMsr(guestSyskey(3, 0, 10, 2, 0), 11));
    emitGuestConstantX(g, 11, 25ULL | (25ULL << 16) | (2ULL << 30));
    g.word(guestMsr(guestSyskey(3, 0, 2, 0, 2), 11)); // 39-bit VA, 40-bit PA, 4 KiB.
    emitGuestConstantX(g, 11, root);
    g.word(guestMsr(guestSyskey(3, 0, 2, 0, 0), 11));
    emitGuestConstantX(g, 11, 1);
    g.word(guestMsr(guestSyskey(3, 0, 1, 0, 0), 11)); // SCTLR_EL1.M
    g.word(0xD5033A9Fu); // DSB ISHST
    g.word(0xD5033FDFu); // ISB
}

void emitCopyW(PlatformGuestBuilder& guest, unsigned rd, unsigned rn) {
    guest.word(addX(rd, rn, 0)); // Wn writes have already zero-extended the source.
}
void emitBoundedDecrement(PlatformGuestBuilder& guest, unsigned amountReg,
                          unsigned boundReg, const std::string& loop,
                          const std::string& finished, const std::string& fail) {
    guest.label(loop);
    guest.cbz(amountReg, finished);
    guest.cbz(boundReg, fail);
    guest.word(subsW(amountReg, amountReg, 1));
    guest.word(subsW(boundReg, boundReg, 1));
    guest.branch(loop);
}
void emitPointerAdvance(PlatformGuestBuilder& guest, unsigned pointerReg,
                        unsigned offsetReg, const std::string& loop,
                        const std::string& finished) {
    guest.label(loop);
    guest.cbz(offsetReg, finished);
    guest.word(addX(pointerReg, pointerReg, 1));
    guest.word(subsW(offsetReg, offsetReg, 1));
    guest.branch(loop);
}
void emitBoundedStringPointerAdvance(PlatformGuestBuilder& guest,
                                     unsigned pointerReg, unsigned offsetReg,
                                     unsigned remainingReg,
                                     const std::string& loop,
                                     const std::string& finished,
                                     const std::string& fail) {
    guest.label(loop);
    guest.cbz(offsetReg, finished);
    guest.cbz(remainingReg, fail);
    guest.word(addX(pointerReg, pointerReg, 1));
    guest.word(subsW(offsetReg, offsetReg, 1));
    guest.word(subsW(remainingReg, remainingReg, 1));
    guest.branch(loop);
}

void emitPropertyNameMatch(PlatformGuestBuilder& guest, const std::string& name,
                           const std::string& matched, const std::string& next) {
    guest.word(cmpW(13, static_cast<unsigned>(name.size() + 1)));
    guest.branchCond(kCondLo, next);
    for (std::size_t i = 0; i < name.size(); ++i) {
        guest.word(ldrB(14, 11, static_cast<unsigned>(i)));
        guest.word(cmpW(14, static_cast<uint8_t>(name[i])));
        guest.branchCond(kCondNe, next);
    }
    guest.word(ldrB(14, 11, static_cast<unsigned>(name.size())));
    guest.word(cmpW(14, 0));
    guest.branchCond(kCondNe, next);
    guest.branch(matched);
}

void emitCellCheck(PlatformGuestBuilder& guest, unsigned offset, unsigned expected,
                   const std::string& fail) {
    guest.word(ldrW(14, 1, offset));
    guest.word(revW(14, 14));
    guest.word(cmpW(14, expected));
    guest.branchCond(kCondNe, fail);
}

std::vector<uint32_t> integratedDiscoveryGuest(std::size_t& completionOffset,
                                               std::size_t& failOffset,
                                               bool includePlatformIntegration = false,
                                               IntegratedGuestOffsets* stages = nullptr,
                                               const AArch64PlatformConfig& layout = {}) {
    // This bounded FDT-v17 guest follows the generated platform profile:
    // it checks the header and section bounds, consumes every structure token,
    // resolves property names through the strings block, and validates the
    // generated node compatibles/cells/interrupt relationship before using
    // the UART reg value. Node order is part of this deliberately narrow
    // writer/guest contract; no hardware address or SPI value is embedded.
    PlatformGuestBuilder g;

    g.label("entry");
    for (unsigned i = 0; i < 4; ++i) {
        g.word(ldrB(1, 0, i));
        g.word(cmpW(1, std::array<unsigned, 4>{0xD0, 0x0D, 0xFE, 0xED}[i]));
        g.branchCond(kCondNe, "fail");
    }
    g.word(ldrW(17, 0, 4)); g.word(revW(17, 17));
    g.word(cmpW(17, 40)); g.branchCond(kCondLo, "fail");
    g.word(cmpW(17, 16, true)); g.branchCond(kCondHi, "fail");
    g.word(ldrW(6, 0, 20)); g.word(revW(6, 6));
    g.word(cmpW(6, 17)); g.branchCond(kCondNe, "fail");
    g.word(ldrW(6, 0, 24)); g.word(revW(6, 6));
    g.word(cmpW(6, 16)); g.branchCond(kCondNe, "fail");

    g.word(ldrW(3, 0, 8)); g.word(revW(3, 3));
    g.word(cmpW(3, 40)); g.branchCond(kCondLo, "fail");
    g.word(cmpW(3, 16, true)); g.branchCond(kCondHi, "fail");
    g.word(ldrW(2, 0, 36)); g.word(revW(2, 2));
    g.cbz(2, "fail");
    g.word(cmpW(2, 16, true)); g.branchCond(kCondHi, "fail");
    g.word(ldrW(5, 0, 12)); g.word(revW(5, 5));
    g.word(cmpW(5, 40)); g.branchCond(kCondLo, "fail");
    g.word(cmpW(5, 16, true)); g.branchCond(kCondHi, "fail");
    g.word(ldrW(4, 0, 32)); g.word(revW(4, 4));
    g.word(cmpW(4, 16, true)); g.branchCond(kCondHi, "fail");

    // Check structure and strings sections fit within totalsize using guest
    // decrement loops, so no register-to-register compare is needed.
    emitCopyW(g, 18, 17); emitCopyW(g, 19, 3);
    emitBoundedDecrement(g, 19, 18, "struct_offset_check", "struct_offset_ok", "fail");
    g.label("struct_offset_ok");
    emitCopyW(g, 19, 2);
    emitBoundedDecrement(g, 19, 18, "struct_size_check", "struct_bounds_ok", "fail");
    g.label("struct_bounds_ok");
    emitCopyW(g, 18, 17); emitCopyW(g, 19, 5);
    emitBoundedDecrement(g, 19, 18, "strings_offset_check", "strings_offset_ok", "fail");
    g.label("strings_offset_ok");
    emitCopyW(g, 19, 4);
    emitBoundedDecrement(g, 19, 18, "strings_size_check", "strings_bounds_ok", "fail");
    g.label("strings_bounds_ok");
    emitCopyW(g, 18, 5); emitCopyW(g, 19, 3);
    emitBoundedDecrement(g, 19, 18, "section_order_offset", "section_order_start", "fail");
    g.label("section_order_start");
    emitCopyW(g, 19, 2);
    emitBoundedDecrement(g, 19, 18, "section_order_size", "section_order_ok", "fail");
    g.label("section_order_ok");

    // Materialize the two checked section pointers with bounded byte walks.
    g.word(addX(1, 0, 0)); emitCopyW(g, 12, 3);
    emitPointerAdvance(g, 1, 12, "make_structure_pointer", "structure_pointer_ready");
    g.label("structure_pointer_ready");
    g.word(addX(3, 0, 0)); emitCopyW(g, 12, 5);
    emitPointerAdvance(g, 3, 12, "make_strings_pointer", "strings_pointer_ready");
    g.label("strings_pointer_ready");

    g.word(movzW(5, 0)); // generated-profile node ordinal
    g.word(movzW(7, 0)); // bounded depth
    g.word(movzW(17, 0)); // discovered root #address-cells
    g.word(movzW(18, 0)); // discovered root #size-cells
    g.word(movzW(19, 0)); // timer compatible + interrupt-parent seen
    g.word(movzW(20, 0)); // root interrupt-parent + GIC compatible seen
    g.word(movzW(10, 0)); // UART base; required before FDT_END
    g.word(movzW(15, 0)); // UART aperture
    g.word(movzW(21, 0)); // RAM base
    g.word(movzW(22, 0)); // RAM size
    g.word(movzW(23, 0)); // GICD base
    g.word(movzW(24, 0)); // GICR base
    g.word(movzW(25, 0)); // UART architectural INTID
    g.word(movzW(26, 0)); // physical timer PPI INTID
    g.word(movzW(27, 0)); // virtual timer PPI INTID
    g.word(movzW(28, 0)); // GIC phandle
    g.word(movzW(29, 0)); // GIC #interrupt-cells
    g.word(movzW(30, 0)); // UART-compatible seen marker

    g.label("next_token");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    g.word(ldrW(6, 1)); g.word(revW(6, 6));
    g.word(addX(1, 1, 4)); g.word(subsW(2, 2, 4));
    g.word(cmpW(6, 1)); g.branchCond(kCondEq, "begin_node");
    g.word(cmpW(6, 2)); g.branchCond(kCondEq, "end_node");
    g.word(cmpW(6, 3)); g.branchCond(kCondEq, "property");
    g.word(cmpW(6, 4)); g.branchCond(kCondEq, "next_token"); // FDT_NOP
    g.word(cmpW(6, 9)); g.branchCond(kCondEq, "fdt_end");
    g.branch("fail");

    g.label("begin_node");
    g.word(addW(5, 5, 1));
    g.word(addW(7, 7, 1)); g.word(cmpW(7, 8)); g.branchCond(kCondHi, "fail");
    g.label("node_name");
    g.word(cmpW(2, 1)); g.branchCond(kCondLo, "fail");
    g.word(ldrB(6, 1)); g.word(addX(1, 1, 1)); g.word(subsW(2, 2, 1));
    g.cbnz(6, "node_name");
    g.word(movzW(16, 3));
    g.branch("align_token");

    g.label("end_node");
    g.word(subsW(7, 7, 1)); g.branchCond(kCondLo, "fail");
    g.branch("next_token");

    g.label("align_token");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    g.word(ldrW(6, 1)); g.word(revW(6, 6));
    for (const auto& [token, target] : std::array<std::pair<unsigned, const char*>, 5>{{
             {1, "next_token"}, {2, "next_token"}, {3, "next_token"},
             {4, "next_token"}, {9, "next_token"}}}) {
        g.word(cmpW(6, token)); g.branchCond(kCondEq, target);
    }
    g.word(ldrB(6, 1)); g.word(cmpW(6, 0)); g.branchCond(kCondNe, "fail");
    g.word(addX(1, 1, 1)); g.word(subsW(2, 2, 1));
    g.word(subsW(16, 16, 1)); g.branchCond(kCondLo, "fail");
    g.branch("align_token");

    g.label("property");
    g.word(cmpW(2, 8)); g.branchCond(kCondLo, "fail");
    g.word(ldrW(8, 1)); g.word(revW(8, 8));
    g.word(addX(1, 1, 4)); g.word(subsW(2, 2, 4));
    g.word(ldrW(9, 1)); g.word(revW(9, 9));
    g.word(addX(1, 1, 4)); g.word(subsW(2, 2, 4));
    g.word(addX(11, 3, 0)); emitCopyW(g, 12, 9); emitCopyW(g, 13, 4);
    emitBoundedStringPointerAdvance(g, 11, 12, 13,
        "property_name_offset", "property_name_at", "fail");
    g.label("property_name_at");
    g.branch("try_address_cells");

    g.label("try_address_cells");
    emitPropertyNameMatch(g, "#address-cells", "address_cells", "try_size_cells");
    g.label("try_size_cells");
    emitPropertyNameMatch(g, "#size-cells", "size_cells", "try_compatible");
    g.label("try_compatible");
    emitPropertyNameMatch(g, "compatible", "compatible", "try_reg");
    g.label("try_reg");
    emitPropertyNameMatch(g, "reg", "reg", "try_interrupts");
    g.label("try_interrupts");
    emitPropertyNameMatch(g, "interrupts", "interrupts", "try_phandle");
    g.label("try_phandle");
    emitPropertyNameMatch(g, "phandle", "phandle", "try_interrupt_cells");
    g.label("try_interrupt_cells");
    emitPropertyNameMatch(g, "#interrupt-cells", "interrupt_cells", "try_interrupt_parent");
    g.label("try_interrupt_parent");
    emitPropertyNameMatch(g, "interrupt-parent", "interrupt_parent", "unknown_property");

    g.label("address_cells");
    g.word(cmpW(5, 1)); g.branchCond(kCondNe, "skip_value");
    g.word(cmpW(8, 4)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 2, "fail");
    g.word(addW(17, 14, 0)); g.branch("skip_value");

    g.label("size_cells");
    g.word(cmpW(5, 1)); g.branchCond(kCondNe, "skip_value");
    g.word(cmpW(8, 4)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 2, "fail");
    g.word(addW(18, 14, 0)); g.branch("skip_value");

    g.label("compatible");
    g.word(cmpW(5, 7)); g.branchCond(kCondEq, "compatible_gic");
    g.word(cmpW(5, 8)); g.branchCond(kCondEq, "compatible_timer");
    g.word(cmpW(5, 9)); g.branchCond(kCondEq, "compatible_uart");
    g.branch("skip_value");
    g.label("compatible_gic");
    g.word(cmpW(8, 11)); g.branchCond(kCondNe, "fail");
    for (unsigned i = 0; i < 10; ++i) {
        const std::string expected = "arm,gic-v3";
        g.word(ldrB(14, 1, i)); g.word(cmpW(14, static_cast<uint8_t>(expected[i])));
        g.branchCond(kCondNe, "fail");
    }
    g.word(ldrB(14, 1, 10)); g.word(cmpW(14, 0)); g.branchCond(kCondNe, "fail");
    g.word(addW(20, 20, 1));
    g.branch("skip_value");
    g.label("compatible_timer");
    g.word(cmpW(8, 16)); g.branchCond(kCondNe, "fail");
    for (unsigned i = 0; i < 15; ++i) {
        const std::string expected = "arm,armv8-timer";
        g.word(ldrB(14, 1, i)); g.word(cmpW(14, static_cast<uint8_t>(expected[i])));
        g.branchCond(kCondNe, "fail");
    }
    g.word(ldrB(14, 1, 15)); g.word(cmpW(14, 0)); g.branchCond(kCondNe, "fail");
    g.word(addW(19, 19, 1));
    g.branch("skip_value");
    g.label("compatible_uart");
    g.word(cmpW(8, 24)); g.branchCond(kCondNe, "fail");
    for (unsigned i = 0; i < 9; ++i) {
        const std::string expected = "arm,pl011";
        g.word(ldrB(14, 1, i)); g.word(cmpW(14, static_cast<uint8_t>(expected[i])));
        g.branchCond(kCondNe, "fail");
    }
    g.word(ldrB(14, 1, 9)); g.word(cmpW(14, 0)); g.branchCond(kCondNe, "fail");
    const std::string secondCompat = "arm,primecell";
    for (unsigned i = 0; i < secondCompat.size(); ++i) {
        g.word(ldrB(14, 1, 10 + i)); g.word(cmpW(14, static_cast<uint8_t>(secondCompat[i])));
        g.branchCond(kCondNe, "fail");
    }
    g.word(ldrB(14, 1, 23)); g.word(cmpW(14, 0)); g.branchCond(kCondNe, "fail");
    g.word(movzW(30, 1)); g.branch("skip_value");

    g.label("reg");
    g.word(cmpW(5, 6)); g.branchCond(kCondEq, "memory_reg");
    g.word(cmpW(5, 7)); g.branchCond(kCondEq, "gic_reg");
    g.word(cmpW(5, 9)); g.branchCond(kCondEq, "uart_reg");
    g.branch("skip_value");
    g.label("memory_reg");
    g.word(cmpW(8, 16)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 16)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 0, "fail"); emitCellCheck(g, 8, 0, "fail");
    g.word(ldrW(14, 1, 4)); g.word(revW(14, 14)); g.word(addW(21, 14, 0));
    g.word(ldrW(14, 1, 12)); g.word(revW(14, 14)); g.word(addW(22, 14, 0));
    g.branch("skip_value");
    g.label("gic_reg");
    g.word(cmpW(8, 32)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 32)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 0, "fail"); emitCellCheck(g, 8, 0, "fail");
    emitCellCheck(g, 16, 0, "fail"); emitCellCheck(g, 24, 0, "fail");
    g.word(ldrW(14, 1, 4)); g.word(revW(14, 14)); g.word(addW(23, 14, 0));
    g.word(ldrW(14, 1, 20)); g.word(revW(14, 14)); g.word(addW(24, 14, 0));
    g.branch("skip_value");
    g.label("uart_reg");
    g.word(cmpW(8, 16)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 16)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 0, "fail"); emitCellCheck(g, 8, 0, "fail");
    g.word(ldrW(14, 1, 4)); g.word(revW(14, 14)); g.word(addW(10, 14, 0));
    g.word(ldrW(14, 1, 12)); g.word(revW(14, 14)); g.word(addW(15, 14, 0));
    g.branch("skip_value");

    g.label("interrupts");
    g.word(cmpW(5, 8)); g.branchCond(kCondEq, "timer_interrupts");
    g.word(cmpW(5, 9)); g.branchCond(kCondEq, "uart_interrupts");
    g.branch("skip_value");
    g.label("timer_interrupts");
    g.word(cmpW(8, 24)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 24)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 1, "fail"); emitCellCheck(g, 8, 4, "fail");
    emitCellCheck(g, 12, 1, "fail"); emitCellCheck(g, 20, 4, "fail");
    g.word(ldrW(14, 1, 4)); g.word(revW(14, 14));
    g.word(cmpW(14, 15)); g.branchCond(kCondHi, "fail");
    g.word(addW(26, 14, 16));
    g.word(ldrW(14, 1, 16)); g.word(revW(14, 14));
    g.word(cmpW(14, 15)); g.branchCond(kCondHi, "fail");
    g.word(addW(27, 14, 16)); g.branch("skip_value");
    g.label("uart_interrupts");
    g.word(cmpW(8, 12)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 12)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 0, "fail"); emitCellCheck(g, 8, 4, "fail");
    g.word(ldrW(14, 1, 4)); g.word(revW(14, 14));
    g.word(cmpW(14, 31)); g.branchCond(kCondHi, "fail");
    g.word(addW(25, 14, 32)); g.branch("skip_value");

    g.label("phandle");
    g.word(cmpW(5, 7)); g.branchCond(kCondNe, "skip_value");
    g.word(cmpW(8, 4)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 1, "fail"); g.word(movzW(28, 1)); g.branch("skip_value");
    g.label("interrupt_cells");
    g.word(cmpW(5, 7)); g.branchCond(kCondNe, "skip_value");
    g.word(cmpW(8, 4)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 3, "fail"); g.word(movzW(29, 3)); g.branch("skip_value");
    g.label("interrupt_parent");
    g.word(cmpW(5, 1)); g.branchCond(kCondEq, "interrupt_parent_value");
    g.word(cmpW(5, 8)); g.branchCond(kCondEq, "interrupt_parent_value");
    g.word(cmpW(5, 9)); g.branchCond(kCondNe, "skip_value");
    g.label("interrupt_parent_value");
    g.word(cmpW(8, 4)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(2, 4)); g.branchCond(kCondLo, "fail");
    emitCellCheck(g, 0, 1, "fail");
    g.word(cmpW(5, 1)); g.branchCond(kCondEq, "root_parent_seen");
    g.word(cmpW(28, 1)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(5, 8)); g.branchCond(kCondEq, "timer_parent_seen");
    g.word(addW(30, 30, 1)); g.branch("skip_value");
    g.label("timer_parent_seen"); g.word(addW(19, 19, 1)); g.branch("skip_value");
    g.label("root_parent_seen"); g.word(addW(20, 20, 1));
    g.branch("skip_value");

    // A recognized but irrelevant property is still bounded and skipped.
    g.label("unknown_property");
    g.label("name_scan");
    g.cbz(13, "fail");
    g.word(ldrB(14, 11)); g.cbz(14, "skip_value");
    g.word(addX(11, 11, 1)); g.word(subsW(13, 13, 1)); g.branch("name_scan");

    g.label("skip_value");
    g.label("skip_value_loop");
    g.cbz(8, "property_padding");
    g.cbz(2, "fail");
    g.word(addX(1, 1, 1)); g.word(subsW(8, 8, 1)); g.word(subsW(2, 2, 1));
    g.branch("skip_value_loop");
    g.label("property_padding");
    g.word(movzW(16, 3)); g.branch("align_token");

    g.label("fdt_end");
    g.cbnz(7, "fail");
    g.cbnz(2, "fail");
    g.cbz(10, "fail"); g.cbz(15, "fail");
    g.cbz(22, "fail"); g.cbz(23, "fail"); g.cbz(24, "fail");
    g.cbz(25, "fail"); g.cbz(26, "fail"); g.cbz(27, "fail");
    g.word(cmpW(17, 2)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(18, 2)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(19, 2)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(20, 2)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(28, 1)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(29, 3)); g.branchCond(kCondNe, "fail");
    g.word(cmpW(30, 2)); g.branchCond(kCondNe, "fail");
    g.branch("success");

    g.label("success");
    g.word(movzW(3, 1));
    g.word(movzW(11, 0x70)); g.word(strW(11, 10, AArch64PL011::UARTLCR_H));
    g.word(movzW(11, 0x301)); g.word(strW(11, 10, AArch64PL011::UARTCR));
    const std::string banner = "guideXOS Hypervisor AArch64\nFDT hardware discovery OK\n";
    for (std::size_t i = 0; i < banner.size(); ++i) {
        const std::string poll = "uart_poll_" + std::to_string(i);
        g.label(poll);
        g.word(ldrW(12, 10, AArch64PL011::UARTFR));
        g.word(0x121B018Du); // AND W13,W12,#0x20 (PL011 TXFF)
        g.cbnz(13, poll);
        g.word(movzW(11, static_cast<uint8_t>(banner[i])));
        g.word(strW(11, 10, AArch64PL011::UARTDR));
    }
    if (includePlatformIntegration) {
        emitIntegratedGicSetup(g, layout);
        emitGuestPageTablesAndEnableMmu(g, layout);
        g.label("mmu_enabled");

        // Exercise a Normal-memory VA and both Device aliases while the
        // guest-owned Stage-1 translation remains enabled.
        emitGuestConstantX(g, 9, 0x30000000);
        emitGuestConstantX(g, 11, 0x1122334455667788ULL);
        g.word(strX(11, 9, 0x48));
        g.word(ldrX(12, 9, 0x48));
        emitGuestConstantX(g, 9, 0x21000000);
        g.word(ldrW(12, 9, 4)); // GICD_TYPER through its translated alias.
        g.word(cmpW(12, 1)); g.branchCond(kCondNe, "fail");

        // Use the already guest-initialized UART through its translated alias
        // and set a one-byte RX threshold for deterministic input injection.
        emitGuestConstantX(g, 9, 0x20000000);
        g.word(movzW(11, 0x300)); g.word(strW(11, 9, AArch64PL011::UARTCR));
        g.word(movzW(11, 0x60)); g.word(strW(11, 9, AArch64PL011::UARTLCR_H));
        g.word(movzW(11, 0x301)); g.word(strW(11, 9, AArch64PL011::UARTCR));
        g.word(movzW(11, 0)); g.word(strW(11, 9, AArch64PL011::UARTIFLS));

        g.word(movzW(11, 10000));
        g.word(guestMsr(timer_sysreg::kCntpTvalEl0, 11));
        g.word(movzW(11, 1));
        g.word(guestMsr(timer_sysreg::kCntpCtlEl0, 11));
        g.word(movzW(11, 0));
        g.word(guestMsr(guestSyskey(3, 3, 4, 2, 1), 11)); // Clear PSTATE.I.
        g.label("timer_wait");
        emitGuestConstantX(g, 8, 0x30000000);
        g.word(ldrW(12, 8, 0x40));
        g.cbz(12, "timer_wait");

        emitGuestConstantX(g, 9, 0x20000000);
        g.word(movzW(11, 0x10));
        g.word(strW(11, 9, AArch64PL011::UARTIMSC));
        g.label("uart_wait");
        emitGuestConstantX(g, 8, 0x30000000);
        g.word(ldrW(12, 8, 0x44));
        g.cbz(12, "uart_wait");
        g.branch("done");
    } else {
        g.branch("done");
    }
    g.label("fail");
    failOffset = g.labelOffset("fail");
    g.word(movzW(3, 0)); g.branch("done");
    g.label("done");
    completionOffset = g.labelOffset("done");
    g.word(0xD503201Fu);
    if (stages != nullptr) {
        stages->mmuEnabled = g.labelOffset("mmu_enabled");
        stages->timerWait = g.labelOffset("timer_wait");
        stages->uartWait = g.labelOffset("uart_wait");
        stages->completion = completionOffset;
    }
    return g.finish();
}

void testGuestFdtDiscoveryAndDynamicUart() {
    std::size_t completionOffset = 0;
    std::size_t failOffset = 0;
    const std::vector<uint32_t> words = integratedDiscoveryGuest(completionOffset, failOffset);
    const std::vector<uint8_t> image = wordsToBytes(words);
    const std::string banner = "guideXOS Hypervisor AArch64\nFDT hardware discovery OK\n";
    CHECK(completionOffset < image.size());
    for (uint32_t word : words) CHECK(Cpu::decodeWord(word, 0x10000).valid);

    auto execute = [&](AArch64VirtualPlatform& platform) {
        const auto& config = platform.config();
        std::string error;
        CHECK(platform.loadRawGuestImage(image, config.guestEntry, &error));
        std::vector<uint8_t> guestDataBefore(config.guestDataBytes);
        platform.ram().Read(config.guestDataBase, guestDataBefore.data(), guestDataBefore.size());
        const uint64_t completionPc = config.guestEntry + completionOffset;
        uint64_t instructions = 0;
        bool guestOk = true;
        while (platform.cpu().cpu().pc() != completionPc &&
               instructions < config.maxGuestInstructions) {
            const uint64_t oldPc = platform.cpu().cpu().pc();
            const Fault fault = platform.cpu().cpu().step(platform.physicalBus());
            ++instructions;
            if (platform.cpu().cpu().pc() == config.guestEntry + failOffset) {
                const Cpu& core = platform.cpu().cpu();
                std::cerr << "guest parser rejection at pc=0x" << std::hex << oldPc
                          << " word=0x" << words[(oldPc - config.guestEntry) / 4]
                          << " regs={x1:0x" << core.readX(1) << ",x2:0x" << core.readX(2)
                          << ",x3:0x" << core.readX(3) << ",x4:0x" << core.readX(4)
                          << ",x5:0x" << core.readX(5) << ",x6:0x" << core.readX(6)
                          << ",x7:0x" << core.readX(7) << ",x8:0x" << core.readX(8)
                          << ",x9:0x" << core.readX(9) << ",x10:0x" << core.readX(10)
                          << ",x11:0x" << core.readX(11) << ",x12:0x" << core.readX(12)
                          << ",x13:0x" << core.readX(13) << "}\n" << std::dec;
                const std::size_t branchIndex = static_cast<std::size_t>((oldPc - config.guestEntry) / 4);
                for (std::size_t at = branchIndex > 6 ? branchIndex - 6 : 0;
                     at <= branchIndex; ++at)
                    std::cerr << "  [" << at << "] 0x" << std::hex << words[at] << std::dec << '\n';
            }
            if (fault != Fault::None) {
                guestOk = false;
                error = "guest fault " + std::to_string(static_cast<unsigned>(fault)) +
                    " at PC " + std::to_string(platform.cpu().cpu().pc());
                break;
            }
            if (!platform.uart().pendingTransmitBytes().empty()) {
                std::size_t progressed = 0;
                if (!platform.uart().advanceTransmit(1, progressed, &error) || progressed != 1) {
                    guestOk = false;
                    break;
                }
            }
        }
        if (platform.cpu().cpu().pc() != completionPc) {
            const Cpu& core = platform.cpu().cpu();
            const std::size_t at = static_cast<std::size_t>((core.pc() - config.guestEntry) / 4);
            std::cerr << "guest parser timeout pc=0x" << std::hex << core.pc()
                      << " word=0x" << (at < words.size() ? words[at] : 0)
                      << " x1=0x" << core.readX(1) << " x2=0x" << core.readX(2)
                      << " x3=0x" << core.readX(3) << " x5=0x" << core.readX(5)
                      << " x6=0x" << core.readX(6) << " x7=0x" << core.readX(7)
                      << " x8=0x" << core.readX(8) << " x9=0x" << core.readX(9)
                      << " x10=0x" << core.readX(10) << " x12=0x" << core.readX(12)
                      << " x13=0x" << core.readX(13) << " output="
                      << platform.uart().outputBytes().size() << std::dec << '\n';
        }
        CHECK(guestOk);
        CHECK_EQ(completionPc, platform.cpu().cpu().pc());
        CHECK(instructions < config.maxGuestInstructions);
        CHECK_EQ(1, platform.cpu().cpu().readX(3));
        CHECK_EQ(config.ramBase, platform.cpu().cpu().readX(21));
        CHECK_EQ(config.ramBytes, platform.cpu().cpu().readX(22));
        CHECK_EQ(config.gicdBase, platform.cpu().cpu().readX(23));
        CHECK_EQ(config.gicrBase, platform.cpu().cpu().readX(24));
        CHECK_EQ(config.uartSpi, platform.cpu().cpu().readX(25));
        CHECK_EQ(config.physicalTimerPpi, platform.cpu().cpu().readX(26));
        CHECK_EQ(config.virtualTimerPpi, platform.cpu().cpu().readX(27));
        CHECK_EQ(1, platform.cpu().cpu().readX(28));
        CHECK_EQ(3, platform.cpu().cpu().readX(29));
        CHECK_EQ(config.uartBase, platform.cpu().cpu().readX(10));
        CHECK_EQ(config.uartApertureBytes, platform.cpu().cpu().readX(15));
        CHECK(platform.cpu().cpu().exceptionLevel() == ExceptionLevel::EL1);
        CHECK_EQ(0, platform.cpu().cpu().tlb().validEntryCount());

        const std::vector<uint8_t> output = platform.uart().outputBytes();
        CHECK(output.size() == banner.size());
        CHECK(std::equal(output.begin(), output.end(), banner.begin(), banner.end()));
        CHECK(platform.uart().pendingTransmitBytes().empty());
        std::vector<uint8_t> guestDataAfter(config.guestDataBytes);
        platform.ram().Read(config.guestDataBase, guestDataAfter.data(), guestDataAfter.size());
        CHECK(guestDataAfter == guestDataBefore);

        uint8_t registerBytes[4]{};
        CHECK(platform.uart().readMmio(AArch64PL011::UARTLCR_H, registerBytes, 4) ==
              AArch64MmioAccessResult::Success);
        CHECK_EQ(0x70, uint32_t(registerBytes[0]) | (uint32_t(registerBytes[1]) << 8) |
                      (uint32_t(registerBytes[2]) << 16) | (uint32_t(registerBytes[3]) << 24));
        CHECK(platform.uart().readMmio(AArch64PL011::UARTCR, registerBytes, 4) ==
              AArch64MmioAccessResult::Success);
        CHECK_EQ(0x301, uint32_t(registerBytes[0]) | (uint32_t(registerBytes[1]) << 8) |
                       (uint32_t(registerBytes[2]) << 16) | (uint32_t(registerBytes[3]) << 24));
        return instructions;
    };

    AArch64VirtualPlatform first;
    const uint64_t firstInstructions = execute(first);
    AArch64PlatformConfig alternateConfig = first.config();
    alternateConfig.uartBase = 0x09100000;
    alternateConfig.uartSpi = 34;
    alternateConfig.timerFrequencyHz = 2'000'000;
    AArch64VirtualPlatform alternate(alternateConfig);
    CHECK(alternate.deviceTreeBlob() != first.deviceTreeBlob());
    const uint64_t alternateInstructions = execute(alternate);
    CHECK_EQ(firstInstructions, alternateInstructions);
    std::cout << "AARCH64-13R guest discovery: guest_bytes=" << image.size()
              << " guest_words=" << words.size()
              << " configA_uart=0x" << std::hex << first.cpu().cpu().readX(10)
              << " configA_spi=" << first.cpu().cpu().readX(25)
              << " configB_uart=0x" << alternate.cpu().cpu().readX(10)
              << " configB_spi=" << alternate.cpu().cpu().readX(25)
              << std::dec << " instructions_each=" << firstInstructions
              << " final_pc=0x" << std::hex << first.cpu().cpu().pc() << std::dec << '\n';
}

bool integratedGuestStep(AArch64VirtualPlatform& platform,
                         std::uint64_t& steps, std::string& error) {
    Cpu& core = platform.cpu().cpu();
    const Fault fault = core.step(platform.physicalBus());
    ++steps;
    if (fault != Fault::None) {
        error = "guest fault " + std::to_string(static_cast<unsigned>(fault)) +
            " at PC " + std::to_string(core.pc()) + ": " + core.lastError();
        return false;
    }
    if (!platform.uart().pendingTransmitBytes().empty()) {
        std::size_t transmitted = 0;
        if (!platform.uart().advanceTransmit(1, transmitted, &error) || transmitted != 1) {
            if (error.empty()) error = "PL011 transmit progress did not consume one queued byte";
            return false;
        }
    }
    return true;
}

bool integratedGuestRunToPc(AArch64VirtualPlatform& platform, uint64_t target,
                            uint64_t limit, std::uint64_t& steps,
                            std::string& error) {
    while (platform.cpu().cpu().pc() != target && steps < limit) {
        if (platform.cpu().cpu().pc() == platform.config().guestEntry +
                platform.config().guestImageCapacity) break;
        if (!integratedGuestStep(platform, steps, error)) return false;
    }
    if (platform.cpu().cpu().pc() != target) {
        if (error.empty()) error = "guest did not reach requested bounded PC";
        return false;
    }
    return true;
}

bool integratedGuestRunToAck(AArch64VirtualPlatform& platform, uint32_t intid,
                             uint64_t limit, std::uint64_t& steps,
                             std::string& error) {
    const uint64_t ackPc = platform.config().exceptionVectorBase + 0x28C;
    while ((platform.cpu().cpu().pc() != ackPc ||
            platform.cpu().cpu().readX(0) != intid) && steps < limit) {
        if (platform.cpu().cpu().pc() == platform.config().guestEntry +
                platform.config().guestImageCapacity) break;
        if (!integratedGuestStep(platform, steps, error)) return false;
    }
    if (platform.cpu().cpu().pc() != ackPc ||
        platform.cpu().cpu().readX(0) != intid) {
        if (error.empty()) error = "guest did not acknowledge expected IRQ in its bounded vector";
        return false;
    }
    return true;
}

bool integratedGuestRunSteps(AArch64VirtualPlatform& platform, uint64_t count,
                             std::uint64_t& steps, std::string& error) {
    for (uint64_t index = 0; index < count; ++index)
        if (!integratedGuestStep(platform, steps, error)) return false;
    return true;
}

uint64_t guestMemoryU64(const AArch64VirtualPlatform& platform, uint64_t address) {
    uint8_t bytes[8]{};
    const_cast<ia64::Memory&>(platform.ram()).Read(address, bytes, sizeof(bytes));
    uint64_t value = 0;
    for (int index = 7; index >= 0; --index) value = (value << 8) | bytes[index];
    return value;
}

void testIntegratedGuestIrqMmuAndSnapshots() {
    AArch64PlatformConfig config;
    IntegratedGuestOffsets stages;
    std::size_t completionOffset = 0;
    std::size_t failOffset = 0;
    const std::vector<uint32_t> words = integratedDiscoveryGuest(
        completionOffset, failOffset, true, &stages, config);
    const std::vector<uint8_t> image = wordsToBytes(words);
    CHECK(image.size() <= config.guestImageCapacity);
    CHECK(stages.mmuEnabled != 0 && stages.timerWait != 0 && stages.uartWait != 0);
    for (std::size_t index = 0; index < words.size(); ++index) {
        const Decoded decoded = Cpu::decodeWord(words[index], config.guestEntry + index * 4);
        CHECK(decoded.valid);
        if (!decoded.valid) std::cerr << "invalid integrated word index=" << index
            << " pc=0x" << std::hex << (config.guestEntry + index * 4)
            << " word=0x" << words[index] << " decode=" << decoded.disassembly
            << std::dec << '\n';
    }

    PlatformGuestBuilder vectorCheck;
    emitIntegratedIrqHandler(vectorCheck);
    const auto vectorWords = vectorCheck.finish();
    for (std::size_t index = 0; index < vectorWords.size(); ++index) {
        const Decoded decoded = Cpu::decodeWord(vectorWords[index], 0x20280 + index * 4);
        CHECK(decoded.valid);
        if (!decoded.valid) std::cerr << "invalid integrated vector index=" << index
            << " pc=0x" << std::hex << (0x20280 + index * 4)
            << " word=0x" << vectorWords[index] << " decode=" << decoded.disassembly
            << std::dec << '\n';
    }

    AArch64VirtualPlatform source(config);
    std::string error;
    CHECK(source.loadRawGuestImage(image, config.guestEntry, &error));
    CHECK_EQ(config.guestEntry, source.cpu().cpu().pc());
    CHECK(source.cpu().cpu().exceptionLevel() == ExceptionLevel::EL1);
    CHECK_EQ(config.dtbBase, source.cpu().cpu().readX(0));
    uint64_t sourceSteps = 0;
    const uint64_t mmuPc = config.guestEntry + stages.mmuEnabled;
    const uint64_t timerWaitPc = config.guestEntry + stages.timerWait;
    const uint64_t uartWaitPc = config.guestEntry + stages.uartWait;
    const uint64_t completionPc = config.guestEntry + completionOffset;
    const uint64_t vectorPc = config.exceptionVectorBase + 0x280;
    const bool reachedMmu = integratedGuestRunToPc(source, mmuPc,
        config.maxGuestInstructions, sourceSteps, error);
    CHECK(reachedMmu);
    if (!reachedMmu) {
        const Cpu& core = source.cpu().cpu();
        const uint64_t faultPc = core.elrEl1();
        const std::size_t faultIndex = faultPc >= config.guestEntry
            ? static_cast<std::size_t>((faultPc - config.guestEntry) / 4) : words.size();
        std::cerr << "integrated pre-MMU stop pc=0x" << std::hex << core.pc()
            << " word=0x" << guestMemoryU64(source, core.pc()) << " elr=0x"
            << faultPc << " instruction=0x"
            << (faultIndex < words.size() ? words[faultIndex] : 0)
            << " x10=0x" << core.readX(10) << " x11=0x" << core.readX(11)
            << " x12=0x" << core.readX(12) << " x23=0x" << core.readX(23)
            << " esr=0x" << core.esrEl1() << " far=0x" << core.farEl1()
            << std::dec << " error=" << error << '\n';
        const uint64_t root = core.effectiveTtbr0El1() & ~uint64_t{0xFFF};
        const auto readRam64 = [&](uint64_t at) {
            uint8_t bytes[8]{};
            source.ram().Read(at, bytes, sizeof(bytes));
            uint64_t value = 0;
            for (int byte = 7; byte >= 0; --byte) value = (value << 8) | bytes[byte];
            return value;
        };
        const uint64_t level2 = readRam64(root) & ~uint64_t{0xFFF};
        const uint64_t identityL3 = readRam64(level2) & ~uint64_t{0xFFF};
        const uint64_t identityPte = readRam64(
            identityL3 + ((faultPc >> 12) & 511u) * 8u);
        const MMUResult diagnosticMap = AArch64MMU::translate(source.physicalBus(),
            faultPc, 1, MMUAccess::Instruction, core.effectiveTcrEl1(),
            core.effectiveTtbr0El1(), core.effectiveTtbr1El1(),
            core.effectiveMairEl1(), false);
        std::cerr << "translation diagnostic ttbr0=0x" << std::hex << core.effectiveTtbr0El1()
            << " tcr=0x" << core.effectiveTcrEl1() << " root[0]=0x" << readRam64(root)
            << " level2=0x" << level2 << " level2[0]=0x" << readRam64(level2)
            << " identityL3=0x" << identityL3 << " pte=0x" << identityPte
            << " map_ok=" << diagnosticMap.success << " map_pa=0x" << diagnosticMap.physicalAddress
            << " map_fault=" << static_cast<unsigned>(diagnosticMap.fault)
            << " map_detail=" << diagnosticMap.detail << std::dec << '\n';
        return;
    }
    CHECK((source.cpu().cpu().sctlrEl1() & 1u) != 0);
    CHECK_EQ(config.exceptionVectorBase, source.cpu().cpu().vbarEl1());
    CHECK(integratedGuestRunToPc(source, timerWaitPc, config.maxGuestInstructions,
                                 sourceSteps, error));
    CHECK(source.cpu().cpu().tlb().validEntryCount() != 0);
    CHECK_EQ(1, source.cpu().cpu().readX(3));

    const auto translate = [&](uint64_t va, MMUAccess access) {
        const Cpu& core = source.cpu().cpu();
        return AArch64MMU::translate(source.physicalBus(), va, 1, access,
            core.effectiveTcrEl1(), core.effectiveTtbr0El1(),
            core.effectiveTtbr1El1(), core.effectiveMairEl1(),
            (core.effectiveSctlrEl1() & (1u << 19)) != 0);
    };
    const MMUResult codeMap = translate(config.guestEntry, MMUAccess::Instruction);
    CHECK(codeMap.success && codeMap.physicalAddress == config.guestEntry);
    const MMUResult uartMap = translate(0x20000000, MMUAccess::Write);
    CHECK(uartMap.success && uartMap.physicalAddress == config.uartBase);
    CHECK_EQ(1, uartMap.attributes.attributeIndex);
    CHECK_EQ(0, uartMap.attributes.memoryAttribute);
    const MMUResult gicMap = translate(0x21000000, MMUAccess::Read);
    CHECK(gicMap.success && gicMap.physicalAddress == config.gicdBase);
    CHECK_EQ(1, gicMap.attributes.attributeIndex);
    const MMUResult dataMap = translate(0x30000048, MMUAccess::Write);
    CHECK(dataMap.success && dataMap.physicalAddress == config.guestDataBase + 0x48);
    CHECK_EQ(0, dataMap.attributes.attributeIndex);
    const MMUResult vectorMap = translate(vectorPc, MMUAccess::Instruction);
    CHECK(vectorMap.success && vectorMap.physicalAddress == vectorPc);

    CHECK(integratedGuestRunSteps(source, 99, sourceSteps, error));
    const uint64_t countdownPc = source.cpu().cpu().pc();
    const uint64_t countdownCounter = source.timer().counter();
    const uint64_t countdownCompare = source.timer().physicalComparator();
    CHECK(countdownCompare > countdownCounter);
    std::vector<uint8_t> countdownSnapshot;
    CHECK(source.captureSnapshot(countdownSnapshot, &error));
    CHECK_EQ(countdownCounter, source.timer().counter()); // capture does not advance virtual time
    CHECK(!countdownSnapshot.empty());

    auto expectIncompatibleSnapshotAtomic = [&](const char* name,
            const std::function<void(AArch64PlatformConfig&)>& change) {
        AArch64PlatformConfig incompatibleConfig = config;
        change(incompatibleConfig);
        AArch64VirtualPlatform destination(incompatibleConfig);
        std::string restoreError;
        CHECK(destination.loadRawGuestImage(image, incompatibleConfig.guestEntry,
                                            &restoreError));
        const std::vector<uint8_t> cpuBefore = destination.cpu().serialize_state();
        const std::vector<uint8_t> ramBefore(destination.ram().GetRawData(),
            destination.ram().GetRawData() + destination.ram().GetTotalSize());
        const uint64_t timerCounterBefore = destination.timer().counter();
        const uint64_t timerCompareBefore = destination.timer().physicalComparator();
        const uint32_t timerControlBefore = destination.timer().physicalControl();
        const std::vector<uint8_t> uartOutputBefore = destination.uart().outputBytes();
        const std::vector<uint8_t> uartTxBefore = destination.uart().pendingTransmitBytes();
        const std::vector<uint8_t> uartRxBefore = destination.uart().pendingReceiveBytes();
        const uint64_t pendingPpiBefore = destination.gic().pending(AArch64GICv3::kPhysicalTimerPpi);
        const uint64_t activePpiBefore = destination.gic().active(AArch64GICv3::kPhysicalTimerPpi);
        const uint64_t pendingSpiBefore = destination.gic().pending(incompatibleConfig.uartSpi);
        const uint64_t activeSpiBefore = destination.gic().active(incompatibleConfig.uartSpi);
        const std::size_t devicesBefore = destination.devices().deviceCount();
        const std::size_t sourcesBefore = destination.devices().interruptSourceCount();
        std::array<AArch64InterruptSourceState, 3> sourceBefore{};
        for (std::size_t index = 0; index < sourceBefore.size(); ++index)
            CHECK(destination.devices().queryInterruptSource(201 + index,
                sourceBefore[index], &restoreError));

        CHECK(!destination.restoreSnapshot(countdownSnapshot, &restoreError));
        CHECK(!restoreError.empty());
        CHECK(destination.cpu().serialize_state() == cpuBefore);
        CHECK(std::equal(ramBefore.begin(), ramBefore.end(),
                         destination.ram().GetRawData()));
        CHECK_EQ(timerCounterBefore, destination.timer().counter());
        CHECK_EQ(timerCompareBefore, destination.timer().physicalComparator());
        CHECK_EQ(timerControlBefore, destination.timer().physicalControl());
        CHECK(destination.uart().outputBytes() == uartOutputBefore);
        CHECK(destination.uart().pendingTransmitBytes() == uartTxBefore);
        CHECK(destination.uart().pendingReceiveBytes() == uartRxBefore);
        CHECK_EQ(pendingPpiBefore, destination.gic().pending(AArch64GICv3::kPhysicalTimerPpi));
        CHECK_EQ(activePpiBefore, destination.gic().active(AArch64GICv3::kPhysicalTimerPpi));
        CHECK_EQ(pendingSpiBefore, destination.gic().pending(incompatibleConfig.uartSpi));
        CHECK_EQ(activeSpiBefore, destination.gic().active(incompatibleConfig.uartSpi));
        CHECK_EQ(devicesBefore, destination.devices().deviceCount());
        CHECK_EQ(sourcesBefore, destination.devices().interruptSourceCount());
        for (std::size_t index = 0; index < sourceBefore.size(); ++index) {
            AArch64InterruptSourceState after{};
            CHECK(destination.devices().queryInterruptSource(201 + index, after,
                &restoreError));
            CHECK_EQ(sourceBefore[index].id, after.id);
            CHECK_EQ(sourceBefore[index].ownerDeviceId, after.ownerDeviceId);
            CHECK_EQ(static_cast<unsigned>(sourceBefore[index].mode),
                     static_cast<unsigned>(after.mode));
            CHECK_EQ(sourceBefore[index].asserted, after.asserted);
            CHECK_EQ(sourceBefore[index].pending, after.pending);
        }
        if (!restoreError.empty())
            std::cerr << "snapshot identity control " << name << ": " << restoreError << '\n';
    };
    expectIncompatibleSnapshotAtomic("RAM geometry", [](auto& changed) {
        changed.ramBytes = 32ULL * 1024ULL * 1024ULL;
    });
    expectIncompatibleSnapshotAtomic("UART base", [](auto& changed) {
        changed.uartBase = 0x09100000;
    });
    expectIncompatibleSnapshotAtomic("UART SPI", [](auto& changed) {
        changed.uartSpi = 34;
    });
    expectIncompatibleSnapshotAtomic("GIC register regions", [](auto& changed) {
        changed.gicdBase = 0x08200000;
        changed.gicrBase = 0x082A0000;
    });
    expectIncompatibleSnapshotAtomic("timer frequency", [](auto& changed) {
        changed.timerFrequencyHz = 2'000'000;
    });

    AArch64VirtualPlatform countdownRestore(config);
    CHECK(countdownRestore.loadRawGuestImage(image, config.guestEntry, &error));
    CHECK(countdownRestore.restoreSnapshot(countdownSnapshot, &error));
    CHECK_EQ(countdownPc, countdownRestore.cpu().cpu().pc());
    CHECK_EQ(countdownCounter, countdownRestore.timer().counter());
    CHECK_EQ(countdownCompare, countdownRestore.timer().physicalComparator());
    CHECK_EQ(0, countdownRestore.cpu().cpu().tlb().validEntryCount());

    const uint64_t sourceCountdownStart = sourceSteps;
    const uint64_t restoreCountdownStart = uint64_t{0};
    const bool timerAcked = integratedGuestRunToAck(source, config.physicalTimerPpi,
        sourceSteps + 30000, sourceSteps, error);
    CHECK(timerAcked);
    if (!timerAcked) {
        std::cerr << "timer IRQ diagnostic pc=0x" << std::hex << source.cpu().cpu().pc()
            << " counter=0x" << source.timer().counter()
            << " compare=0x" << source.timer().physicalComparator()
            << " ctl=0x" << source.timer().physicalControl()
            << " ppi_pending=" << source.gic().pending(config.physicalTimerPpi)
            << " ppi_enabled=" << source.gic().enabled(config.physicalTimerPpi)
            << " ppi_active=" << source.gic().active(config.physicalTimerPpi)
            << " irq=" << source.gic().irqOutputAsserted()
            << " pstate=0x" << source.cpu().cpu().pstate()
            << std::dec << " error=" << error << '\n';
        return;
    }
    const uint64_t sourceTimerContinuation = sourceSteps - sourceCountdownStart;
    const uint64_t timerExpiry = source.timer().counter();
    const uint64_t timerInterruptedPc = source.cpu().cpu().elrEl1();
    CHECK_EQ(vectorPc + 12, source.cpu().cpu().pc());
    CHECK_EQ(config.physicalTimerPpi, source.cpu().cpu().readX(0));
    CHECK(timerInterruptedPc >= timerWaitPc && timerInterruptedPc < uartWaitPc);
    CHECK_EQ(0, timerInterruptedPc & 3u);
    CHECK(timerExpiry >= countdownCompare);
    CHECK_EQ(0x5, source.cpu().cpu().spsrEl1() & 0x1Fu);
    CHECK_EQ(0, source.cpu().cpu().spsrEl1() & kPstateI);
    CHECK((source.cpu().cpu().pstate() & kPstateI) != 0);
    CHECK(source.gic().active(config.physicalTimerPpi));
    CHECK(source.gic().pending(config.physicalTimerPpi));
    CHECK(integratedGuestRunToPc(source, uartWaitPc, config.maxGuestInstructions,
                                 sourceSteps, error));
    CHECK(!source.gic().active(config.physicalTimerPpi));
    CHECK(!source.gic().pending(config.physicalTimerPpi));
    CHECK_EQ(0, source.timer().physicalControl() & 1u);

    uint64_t countdownRestoreSteps = restoreCountdownStart;
    CHECK(integratedGuestRunToAck(countdownRestore, config.physicalTimerPpi,
        30000, countdownRestoreSteps, error));
    const uint64_t restoredTimerContinuation = countdownRestoreSteps - restoreCountdownStart;
    CHECK_EQ(sourceTimerContinuation, restoredTimerContinuation);
    CHECK_EQ(timerExpiry, countdownRestore.timer().counter());
    CHECK_EQ(timerInterruptedPc, countdownRestore.cpu().cpu().elrEl1());
    CHECK_EQ(source.cpu().cpu().spsrEl1(), countdownRestore.cpu().cpu().spsrEl1());
    CHECK(integratedGuestRunToPc(countdownRestore, uartWaitPc,
        config.maxGuestInstructions, countdownRestoreSteps, error));
    CHECK_EQ(source.cpu().cpu().pc(), countdownRestore.cpu().cpu().pc());

    const std::vector<uint8_t> input{'Z'};
    std::size_t accepted = 0;
    CHECK(source.uart().injectReceive(input, accepted, &error));
    CHECK_EQ(1, accepted);
    accepted = 0;
    CHECK(countdownRestore.uart().injectReceive(input, accepted, &error));
    CHECK_EQ(1, accepted);
    const uint64_t sourceBeforeUartAck = sourceSteps;
    const bool uartAcked = integratedGuestRunToAck(source, config.uartSpi,
        config.maxGuestInstructions, sourceSteps, error);
    CHECK(uartAcked);
    if (!uartAcked) {
        AArch64InterruptSourceState state{};
        source.devices().queryInterruptSource(203, state, &error);
        uint8_t uartBytes[4]{};
        source.uart().readMmio(AArch64PL011::UARTMIS, uartBytes, 4);
        const uint32_t uartMis = uint32_t(uartBytes[0]) | (uint32_t(uartBytes[1]) << 8) |
            (uint32_t(uartBytes[2]) << 16) | (uint32_t(uartBytes[3]) << 24);
        source.uart().readMmio(AArch64PL011::UARTIMSC, uartBytes, 4);
        const uint32_t uartImsc = uint32_t(uartBytes[0]) | (uint32_t(uartBytes[1]) << 8) |
            (uint32_t(uartBytes[2]) << 16) | (uint32_t(uartBytes[3]) << 24);
        source.gic().readMmio(0x84, uartBytes, 4);
        const uint32_t group = uint32_t(uartBytes[0]) | (uint32_t(uartBytes[1]) << 8) |
            (uint32_t(uartBytes[2]) << 16) | (uint32_t(uartBytes[3]) << 24);
        source.gic().readMmio(0x104, uartBytes, 4);
        const uint32_t enable = uint32_t(uartBytes[0]) | (uint32_t(uartBytes[1]) << 8) |
            (uint32_t(uartBytes[2]) << 16) | (uint32_t(uartBytes[3]) << 24);
        std::cerr << "UART IRQ diagnostic pc=0x" << std::hex << source.cpu().cpu().pc()
            << " counter=0x" << source.timer().counter()
            << " spi_pending=" << source.gic().pending(config.uartSpi)
            << " spi_enabled=" << source.gic().enabled(config.uartSpi)
            << " spi_active=" << source.gic().active(config.uartSpi)
            << " irq=" << source.gic().irqOutputAsserted()
            << " pstate=0x" << source.cpu().cpu().pstate()
            << " uart_mis=0x" << uartMis << " uart_imsc=0x" << uartImsc
            << " group=0x" << group << " enable=0x" << enable
            << " source=" << state.asserted
            << " rx_count=" << source.uart().pendingReceiveBytes().size()
            << std::dec << " error=" << error << '\n';
        return;
    }
    const uint64_t sourceUartContinuation = sourceSteps - sourceBeforeUartAck;
    CHECK_EQ(vectorPc + 12, source.cpu().cpu().pc());
    CHECK_EQ(config.uartSpi, source.cpu().cpu().readX(0));
    const uint64_t interruptedPc = source.cpu().cpu().elrEl1();
    CHECK_EQ(uartWaitPc, interruptedPc);
    CHECK_EQ(0x5, source.cpu().cpu().spsrEl1() & 0x1Fu);
    CHECK(source.gic().active(config.uartSpi));
    CHECK(source.gic().pending(config.uartSpi)); // RX level remains asserted until guest LDR.
    CHECK_EQ(1, source.uart().pendingReceiveBytes().size());
    AArch64InterruptSourceState uartSource{};
    CHECK(source.devices().queryInterruptSource(203, uartSource, &error));
    CHECK(uartSource.asserted);

    std::vector<uint8_t> activeSnapshot;
    CHECK(source.captureSnapshot(activeSnapshot, &error));
    const uint64_t activeSnapshotPc = source.cpu().cpu().pc();
    CHECK_EQ(vectorPc + 12, activeSnapshotPc);
    CHECK(!activeSnapshot.empty());
    CHECK(source.gic().active(config.uartSpi));

    const uint64_t restoreBeforeUartAck = countdownRestoreSteps;
    uint64_t restoreUartSteps = restoreBeforeUartAck;
    CHECK(integratedGuestRunToAck(countdownRestore, config.uartSpi,
        config.maxGuestInstructions, restoreUartSteps, error));
    const uint64_t restoredUartContinuation = restoreUartSteps - restoreBeforeUartAck;
    CHECK_EQ(sourceUartContinuation, restoredUartContinuation);
    CHECK_EQ(interruptedPc, countdownRestore.cpu().cpu().elrEl1());

    AArch64VirtualPlatform activeRestore(config);
    CHECK(activeRestore.loadRawGuestImage(image, config.guestEntry, &error));
    CHECK(activeRestore.restoreSnapshot(activeSnapshot, &error));
    CHECK_EQ(activeSnapshotPc, activeRestore.cpu().cpu().pc());
    CHECK_EQ(config.uartSpi, activeRestore.cpu().cpu().readX(0));
    CHECK_EQ(interruptedPc, activeRestore.cpu().cpu().elrEl1());
    CHECK_EQ(0, activeRestore.cpu().cpu().tlb().validEntryCount());
    CHECK(activeRestore.gic().active(config.uartSpi));
    CHECK(activeRestore.gic().pending(config.uartSpi));
    CHECK_EQ(1, activeRestore.uart().pendingReceiveBytes().size());

    const uint64_t sourceAfterActiveSnapshot = sourceSteps;
    CHECK(integratedGuestRunToPc(source, completionPc,
        config.maxGuestInstructions, sourceSteps, error));
    const uint64_t sourceActiveContinuation = sourceSteps - sourceAfterActiveSnapshot;
    std::uint64_t activeRestoreSteps = 0;
    CHECK(integratedGuestRunToPc(activeRestore, completionPc,
        config.maxGuestInstructions, activeRestoreSteps, error));
    CHECK_EQ(sourceActiveContinuation, activeRestoreSteps);
    const uint64_t countdownBeforeFinal = countdownRestoreSteps;
    CHECK(integratedGuestRunToPc(countdownRestore, completionPc,
        config.maxGuestInstructions, countdownRestoreSteps, error));
    CHECK_EQ(sourceUartContinuation, restoredUartContinuation);
    CHECK_EQ(source.cpu().cpu().pc(), activeRestore.cpu().cpu().pc());
    CHECK_EQ(source.cpu().cpu().pc(), countdownRestore.cpu().cpu().pc());
    CHECK_EQ(source.cpu().cpu().readX(3), activeRestore.cpu().cpu().readX(3));
    CHECK_EQ(1, source.cpu().cpu().readX(3));
    CHECK_EQ(1, activeRestore.cpu().cpu().readX(3));
    CHECK_EQ(1, countdownRestore.cpu().cpu().readX(3));
    CHECK_EQ(0x1122334455667788ULL,
             guestMemoryU64(source, config.guestDataBase + 0x48));
    CHECK_EQ('Z', uint32_t(guestMemoryU64(source, config.guestDataBase + 0x44)));
    CHECK_EQ(0x1122334455667788ULL,
             guestMemoryU64(activeRestore, config.guestDataBase + 0x48));
    CHECK_EQ('Z', uint32_t(guestMemoryU64(activeRestore, config.guestDataBase + 0x44)));
    CHECK_EQ(guestMemoryU64(source, config.guestDataBase + 0x48),
             guestMemoryU64(countdownRestore, config.guestDataBase + 0x48));
    CHECK_EQ(guestMemoryU64(source, config.guestDataBase + 0x44),
             guestMemoryU64(countdownRestore, config.guestDataBase + 0x44));
    CHECK_EQ(source.timer().counter(), activeRestore.timer().counter());
    CHECK_EQ(source.timer().counter(), countdownRestore.timer().counter());
    CHECK_EQ(source.gic().active(config.uartSpi), false);
    CHECK_EQ(source.gic().pending(config.uartSpi), false);
    CHECK_EQ(activeRestore.gic().active(config.uartSpi), false);
    CHECK_EQ(activeRestore.gic().pending(config.uartSpi), false);
    CHECK(source.uart().pendingReceiveBytes().empty());
    CHECK(activeRestore.uart().pendingReceiveBytes().empty());
    const std::string banner = "guideXOS Hypervisor AArch64\nFDT hardware discovery OK\n";
    const std::vector<uint8_t> sourceOutput = source.uart().outputBytes();
    const std::vector<uint8_t> activeOutput = activeRestore.uart().outputBytes();
    const std::vector<uint8_t> countdownOutput = countdownRestore.uart().outputBytes();
    CHECK(std::string(sourceOutput.begin(), sourceOutput.end()) == banner);
    CHECK(sourceOutput == activeOutput);
    CHECK(sourceOutput == countdownOutput);
    CHECK_EQ(sourceOutput.size(), activeOutput.size());
    const auto uartRegister = [&](AArch64VirtualPlatform& platform,
                                  uint64_t offset) {
        uint8_t bytes[4]{};
        const auto result = platform.uart().readMmio(offset, bytes, sizeof(bytes));
        CHECK(result == AArch64MmioAccessResult::Success);
        return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
               (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    };
    CHECK_EQ(0x60, uartRegister(source, AArch64PL011::UARTLCR_H));
    CHECK_EQ(0x301, uartRegister(source, AArch64PL011::UARTCR));
    CHECK_EQ(0, uartRegister(source, AArch64PL011::UARTIFLS));
    CHECK_EQ(0x10, uartRegister(source, AArch64PL011::UARTIMSC));
    CHECK_EQ(0, uartRegister(source, AArch64PL011::UARTMIS));
    CHECK_EQ(0, source.cpu().cpu().pstate() & kPstateI);
    CHECK_EQ(1, source.cpu().cpu().sctlrEl1() & 1u);
    CHECK_EQ(0, source.gic().active(config.physicalTimerPpi));
    CHECK_EQ(0, source.gic().pending(config.physicalTimerPpi));
    CHECK_EQ(0, countdownRestore.gic().active(config.physicalTimerPpi));
    CHECK_EQ(0, countdownRestore.gic().pending(config.physicalTimerPpi));
    CHECK_EQ(0, countdownRestore.gic().active(config.uartSpi));
    CHECK_EQ(0, countdownRestore.gic().pending(config.uartSpi));
    CHECK(countdownRestoreSteps > countdownBeforeFinal);

    // The exact same byte-for-byte guest image must rediscover and operate a
    // second supported UART base/SPI pair without rebuilding guest constants.
    AArch64PlatformConfig alternateConfig = config;
    alternateConfig.uartBase = 0x09100000;
    alternateConfig.uartSpi = 34;
    alternateConfig.timerFrequencyHz = 2'000'000;
    AArch64VirtualPlatform alternate(alternateConfig);
    CHECK(alternate.loadRawGuestImage(image, alternateConfig.guestEntry, &error));
    std::uint64_t alternateSteps = 0;
    const uint64_t alternateMmuPc = alternateConfig.guestEntry + stages.mmuEnabled;
    const uint64_t alternateTimerWaitPc = alternateConfig.guestEntry + stages.timerWait;
    const uint64_t alternateUartWaitPc = alternateConfig.guestEntry + stages.uartWait;
    const uint64_t alternateCompletionPc = alternateConfig.guestEntry + completionOffset;
    bool alternateReady = integratedGuestRunToPc(alternate, alternateMmuPc,
        alternateConfig.maxGuestInstructions, alternateSteps, error);
    CHECK(alternateReady);
    if (!alternateReady) return;
    CHECK_EQ(alternateConfig.uartBase, alternate.cpu().cpu().readX(10));
    CHECK_EQ(alternateConfig.uartSpi, alternate.cpu().cpu().readX(25));
    CHECK_EQ(alternateConfig.gicdBase, alternate.cpu().cpu().readX(23));
    CHECK_EQ(alternateConfig.gicrBase, alternate.cpu().cpu().readX(24));
    alternateReady = integratedGuestRunToPc(alternate, alternateTimerWaitPc,
        alternateConfig.maxGuestInstructions, alternateSteps, error);
    CHECK(alternateReady);
    if (!alternateReady) return;
    const bool alternateTimerAck = integratedGuestRunToAck(alternate,
        alternateConfig.physicalTimerPpi,
        alternateSteps + 20000, alternateSteps, error);
    CHECK(alternateTimerAck);
    if (!alternateTimerAck) return;
    CHECK(alternate.gic().active(alternateConfig.physicalTimerPpi));
    alternateReady = integratedGuestRunToPc(alternate, alternateUartWaitPc,
        alternateConfig.maxGuestInstructions, alternateSteps, error);
    CHECK(alternateReady);
    if (!alternateReady) return;
    accepted = 0;
    CHECK(alternate.uart().injectReceive(input, accepted, &error));
    CHECK_EQ(1, accepted);
    const bool alternateUartAck = integratedGuestRunToAck(alternate,
        alternateConfig.uartSpi, alternateConfig.maxGuestInstructions,
        alternateSteps, error);
    CHECK(alternateUartAck);
    if (!alternateUartAck) return;
    CHECK_EQ(alternateConfig.uartSpi, alternate.cpu().cpu().readX(0));
    CHECK(alternate.gic().active(alternateConfig.uartSpi));
    alternateReady = integratedGuestRunToPc(alternate, alternateCompletionPc,
        alternateConfig.maxGuestInstructions, alternateSteps, error);
    CHECK(alternateReady);
    if (!alternateReady) return;
    CHECK_EQ(1, alternate.cpu().cpu().readX(3));
    CHECK(alternate.uart().pendingReceiveBytes().empty());
    CHECK(!alternate.gic().active(alternateConfig.uartSpi));
    CHECK(!alternate.gic().pending(alternateConfig.uartSpi));
    CHECK_EQ(1, alternate.cpu().cpu().sctlrEl1() & 1u);
    const std::vector<uint8_t> alternateOutput = alternate.uart().outputBytes();
    CHECK(std::string(alternateOutput.begin(), alternateOutput.end()) == banner);

    if (!phase13rEvidenceDirectory.empty()) {
        std::ostringstream guestWordDump;
        for (std::size_t index = 0; index < words.size(); ++index)
            guestWordDump << index << " 0x" << std::hex << words[index] << std::dec << '\n';
        std::ostringstream manifest;
        manifest << "AARCH64-13R integrated guest fixture\n"
                 << "Generator: integratedDiscoveryGuest() in tests/test_aarch64_platform.cpp\n"
                 << "Execution: production AArch64 Cpu::step through AArch64PhysicalBus\n"
                 << "Initial PC: 0x" << std::hex << config.guestEntry << std::dec << '\n'
                 << "Instruction words: " << words.size() << '\n'
                 << "Binary bytes: " << image.size() << '\n'
                 << "Assembler: not used; bounded local A64 fixture encoders\n";
        std::ostringstream summary;
        summary << "AARCH64-13R integrated platform evidence\n"
                << "Config A entry=0x" << std::hex << config.guestEntry
                << " dtb=0x" << config.dtbBase << " UART=0x" << config.uartBase
                << " SPI=" << std::dec << config.uartSpi
                << " GICD=0x" << std::hex << config.gicdBase
                << " GICR=0x" << config.gicrBase << std::dec << '\n'
                << "Config B reuses identical guest bytes; UART=0x" << std::hex
                << alternateConfig.uartBase << " SPI=" << std::dec
                << alternateConfig.uartSpi << " timer_frequency_hz="
                << alternateConfig.timerFrequencyHz << '\n'
                << "Initial EL: EL1\n"
                << "MMU enable PC: 0x" << std::hex << mmuPc << '\n'
                << "Timer wait PC: 0x" << timerWaitPc << '\n'
                << "Timer vector PC: 0x" << vectorPc << '\n'
                << "Timer interrupted PC (ELR_EL1): 0x" << timerInterruptedPc << '\n'
                << "Timer INTID: " << std::dec << config.physicalTimerPpi << '\n'
                << "Timer comparator: 0x" << std::hex << countdownCompare << '\n'
                << "Timer counter at acknowledged vector: 0x" << timerExpiry << '\n'
                << "Timer IRQ continuation instructions: " << std::dec
                << sourceTimerContinuation << '\n'
                << "UART wait/interrupted PC: 0x" << std::hex << uartWaitPc << '\n'
                << "UART vector acknowledge PC: 0x" << activeSnapshotPc << '\n'
                << "UART SPI: " << std::dec << config.uartSpi << " received byte: Z\n"
                << "Countdown snapshot boundary PC: 0x" << std::hex << countdownPc << '\n'
                << "Countdown snapshot bytes: " << std::dec << countdownSnapshot.size() << '\n'
                << "Active-handler snapshot boundary PC: 0x" << std::hex
                << activeSnapshotPc << '\n'
                << "Active-handler snapshot bytes: " << std::dec << activeSnapshot.size() << '\n'
                << "Active-handler continuation instructions: " << activeRestoreSteps << '\n'
                << "Config A final PC: 0x" << std::hex << source.cpu().cpu().pc() << '\n'
                << "Config A total retired instructions: " << std::dec << sourceSteps << '\n'
                << "Config B final PC: 0x" << std::hex << alternate.cpu().cpu().pc() << '\n'
                << "Config B total retired instructions: " << std::dec << alternateSteps << '\n'
                << "Config A exact UART output bytes: " << sourceOutput.size() << '\n'
                << "Success register X3: " << source.cpu().cpu().readX(3) << '\n';
        CHECK(writeArtifact(phase13rEvidenceDirectory / "integrated_guest.bin", image));
        CHECK(writeArtifact(phase13rEvidenceDirectory / "config_a.dtb",
                            source.deviceTreeBlob()));
        CHECK(writeArtifact(phase13rEvidenceDirectory / "config_b.dtb",
                            alternate.deviceTreeBlob()));
        CHECK(writeArtifact(phase13rEvidenceDirectory / "countdown.snapshot",
                            countdownSnapshot));
        CHECK(writeArtifact(phase13rEvidenceDirectory / "active_handler.snapshot",
                            activeSnapshot));
        CHECK(writeArtifact(phase13rEvidenceDirectory / "uart_output.bin", sourceOutput));
        CHECK(writeTextArtifact(phase13rEvidenceDirectory / "integrated_guest.words.txt",
                                guestWordDump.str()));
        CHECK(writeTextArtifact(phase13rEvidenceDirectory / "integrated_guest_manifest.txt",
                                manifest.str()));
        CHECK(writeTextArtifact(phase13rEvidenceDirectory / "execution_summary.txt",
                                summary.str()));
    }

    std::cout << "AARCH64-13R integrated guest: bytes=" << image.size()
              << " words=" << words.size() << " initial_pc=0x" << std::hex
              << config.guestEntry << " mmu_pc=0x" << mmuPc
              << " timer_wait_pc=0x" << timerWaitPc
              << " timer_vector_pc=0x" << vectorPc
              << " uart_vector_pc=0x" << activeSnapshotPc
              << " interrupted_pc=0x" << interruptedPc
              << " final_pc=0x" << source.cpu().cpu().pc()
              << " timer_compare=0x" << countdownCompare
              << " timer_expiry=0x" << timerExpiry << std::dec
              << " timer_continuation_steps=" << sourceTimerContinuation
              << " uart_irq_steps=" << sourceUartContinuation
              << " active_restore_steps=" << activeRestoreSteps
              << " alternate_uart=0x" << std::hex << alternateConfig.uartBase
              << " alternate_spi=" << std::dec << alternateConfig.uartSpi
              << " alternate_steps=" << alternateSteps
              << " snapshot_countdown_bytes=" << countdownSnapshot.size()
              << " snapshot_active_bytes=" << activeSnapshot.size()
              << " total_guest_steps=" << sourceSteps << '\n';

    const uint64_t alternateFinalPc = alternate.cpu().cpu().pc();
    const uint64_t alternateFinalCounter = alternate.timer().counter();
    CHECK(source.reset(&error));
    CHECK_EQ(config.guestEntry, source.cpu().cpu().pc());
    CHECK_EQ(config.dtbBase, source.cpu().cpu().readX(0));
    CHECK_EQ(0, source.cpu().cpu().readX(3));
    CHECK_EQ(0, source.cpu().cpu().sctlrEl1() & 1u);
    CHECK_EQ(0, source.timer().counter());
    CHECK(!source.gic().pending(config.physicalTimerPpi));
    CHECK(!source.gic().active(config.physicalTimerPpi));
    CHECK(!source.gic().pending(config.uartSpi));
    CHECK(!source.gic().active(config.uartSpi));
    CHECK(source.uart().outputBytes() == sourceOutput); // reset preserves output history.
    CHECK(source.uart().pendingReceiveBytes().empty());
    CHECK_EQ(alternateFinalPc, alternate.cpu().cpu().pc());
    CHECK_EQ(alternateFinalCounter, alternate.timer().counter());
    CHECK(alternate.uart().outputBytes() == alternateOutput);
}

void testGuestFdtParserNegativeControls() {
    std::size_t completionOffset = 0;
    std::size_t failOffset = 0;
    const std::vector<uint8_t> image = wordsToBytes(
        integratedDiscoveryGuest(completionOffset, failOffset));
    AArch64VirtualPlatform platform;
    const std::vector<uint8_t> valid = platform.deviceTreeBlob();
    const auto properties = rawFdtProperties(valid);
    const RawFdtProperty* rootAddressCells = findRawProperty(properties, 1, "#address-cells");
    const RawFdtProperty* rootSizeCells = findRawProperty(properties, 1, "#size-cells");
    const RawFdtProperty* anyProperty = findRawProperty(properties, 1, "#address-cells");
    const RawFdtProperty* gicCompatible = findRawProperty(properties, 7, "compatible");
    const RawFdtProperty* gicInterruptCells = findRawProperty(properties, 7, "#interrupt-cells");
    const RawFdtProperty* timerInterrupts = findRawProperty(properties, 8, "interrupts");
    const RawFdtProperty* uartCompatible = findRawProperty(properties, 9, "compatible");
    const RawFdtProperty* uartReg = findRawProperty(properties, 9, "reg");
    const RawFdtProperty* uartInterrupts = findRawProperty(properties, 9, "interrupts");
    const RawFdtProperty* uartParent = findRawProperty(properties, 9, "interrupt-parent");
    CHECK(rootAddressCells != nullptr && rootSizeCells != nullptr && anyProperty != nullptr);
    CHECK(gicCompatible != nullptr && gicInterruptCells != nullptr && timerInterrupts != nullptr);
    CHECK(uartCompatible != nullptr && uartReg != nullptr && uartInterrupts != nullptr &&
          uartParent != nullptr);
    if (rootAddressCells == nullptr || rootSizeCells == nullptr || anyProperty == nullptr ||
        gicCompatible == nullptr || gicInterruptCells == nullptr || timerInterrupts == nullptr ||
        uartCompatible == nullptr || uartReg == nullptr || uartInterrupts == nullptr ||
        uartParent == nullptr) return;

    std::vector<std::pair<std::string, std::vector<uint8_t>>> cases;
    auto add = [&](const std::string& name, const std::function<void(std::vector<uint8_t>&)>& mutate) {
        std::vector<uint8_t> bad = valid;
        mutate(bad);
        cases.emplace_back(name, std::move(bad));
    };
    add("invalid magic", [](auto& bad) { bad[0] = 0; });
    add("truncated header", [](auto& bad) { putBe32(bad, 4, 39); });
    add("invalid totalsize", [](auto& bad) { putBe32(bad, 4, 0x10001); });
    add("structure block beyond totalsize", [](auto& bad) {
        putBe32(bad, 8, be32(bad.data() + 4) - 2);
    });
    add("strings block beyond totalsize", [](auto& bad) {
        putBe32(bad, 12, be32(bad.data() + 4) - 1);
    });
    add("invalid property string offset", [=](auto& bad) {
        putBe32(bad, anyProperty->nameOffsetField, 0xFFFFFFFFu);
    });
    add("property length overflow", [=](auto& bad) {
        putBe32(bad, uartReg->lengthField, 0xFFFFFFFFu);
    });
    add("missing UART compatible node", [=](auto& bad) { bad[uartCompatible->valueOffset] = 'x'; });
    add("missing GIC compatible node", [=](auto& bad) { bad[gicCompatible->valueOffset] = 'x'; });
    add("missing timer interrupts property", [=](auto& bad) {
        putBe32(bad, timerInterrupts->nameOffsetField,
                static_cast<uint32_t>(findFdtString(valid, "interrupt-names")));
    });
    add("invalid phandle relationship", [=](auto& bad) {
        putBe32(bad, uartParent->valueOffset, 2);
    });
    add("unsupported interrupt cell count", [=](auto& bad) {
        putBe32(bad, gicInterruptCells->valueOffset, 2);
    });
    add("invalid timer interrupt specifier", [=](auto& bad) {
        putBe32(bad, timerInterrupts->valueOffset, 2);
    });
    add("unsupported UART interrupt flags", [=](auto& bad) {
        putBe32(bad, uartInterrupts->valueOffset + 8, 0);
    });
    add("invalid address cell count", [=](auto& bad) {
        putBe32(bad, rootAddressCells->valueOffset, 1);
    });
    add("invalid size cell count", [=](auto& bad) {
        putBe32(bad, rootSizeCells->valueOffset, 1);
    });
    add("missing final FDT_END", [](auto& bad) {
        const std::size_t end = be32(bad.data() + 8) + be32(bad.data() + 36) - 4;
        putBe32(bad, end, 4); // FDT_NOP
    });
    add("invalid node alignment", [](auto& bad) {
        bad[be32(bad.data() + 8) + 5] = 1;
    });
    add("invalid property alignment", [=](auto& bad) {
        const RawFdtProperty* compatible = findRawProperty(properties, 1, "compatible");
        bad[compatible->valueOffset + compatible->length] = 1;
    });

    // Replace the structure with nine nested nodes. The guest bounds depth at
    // eight and must take its normal failure path before reaching the end.
    std::vector<uint8_t> excessiveDepth;
    const std::size_t structureOffset = be32(valid.data() + 8);
    std::vector<uint8_t> nested;
    for (unsigned depth = 0; depth < 9; ++depth) {
        nested.insert(nested.end(), {0, 0, 0, 1, 0}); // FDT_BEGIN_NODE + empty name.
        while ((nested.size() & 3u) != 0) nested.push_back(0);
    }
    const std::size_t oldStringsOffset = be32(valid.data() + 12);
    const std::size_t stringsBytes = be32(valid.data() + 32);
    const std::size_t newStringsOffset = structureOffset + nested.size();
    excessiveDepth.assign(newStringsOffset + stringsBytes, 0);
    std::copy(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(structureOffset),
              excessiveDepth.begin());
    std::copy(nested.begin(), nested.end(),
              excessiveDepth.begin() + static_cast<std::ptrdiff_t>(structureOffset));
    std::copy(valid.begin() + static_cast<std::ptrdiff_t>(oldStringsOffset),
              valid.begin() + static_cast<std::ptrdiff_t>(oldStringsOffset + stringsBytes),
              excessiveDepth.begin() + static_cast<std::ptrdiff_t>(newStringsOffset));
    putBe32(excessiveDepth, 4, static_cast<uint32_t>(excessiveDepth.size()));
    putBe32(excessiveDepth, 12, static_cast<uint32_t>(newStringsOffset));
    putBe32(excessiveDepth, 36, static_cast<uint32_t>(nested.size()));
    cases.emplace_back("excessive traversal depth", std::move(excessiveDepth));

    const uint64_t completionPc = platform.config().guestEntry + completionOffset;
    std::size_t rejected = 0;
    for (const auto& [name, bad] : cases) {
        std::string error;
        CHECK(bad.size() <= platform.config().dtbMaxBytes);
        CHECK(platform.loadRawGuestImage(image, platform.config().guestEntry, &error));
        platform.ram().Write(platform.config().dtbBase, bad.data(), bad.size());
        uint64_t instructions = 0;
        Fault fault = Fault::None;
        while (platform.cpu().cpu().pc() != completionPc && instructions < 100'000) {
            fault = platform.cpu().cpu().step(platform.physicalBus());
            ++instructions;
            if (fault != Fault::None) break;
        }
        CHECK(fault == Fault::None);
        CHECK_EQ(completionPc, platform.cpu().cpu().pc());
        CHECK_EQ(0, platform.cpu().cpu().readX(3));
        CHECK(platform.uart().outputBytes().empty());
        CHECK(instructions < 100'000);
        if (platform.cpu().cpu().readX(3) == 0 && fault == Fault::None &&
            platform.cpu().cpu().pc() == completionPc && platform.uart().outputBytes().empty())
            ++rejected;
        else
            std::cerr << "guest parser did not reject: " << name << '\n';
    }
    CHECK_EQ(cases.size(), rejected);
    std::cout << "AARCH64-13R guest parser negative controls: " << rejected
              << "/" << cases.size() << " rejected through bounded guest path\n";
}

void testIndependentFdtAndConsistency() {
    AArch64VirtualPlatform platform;
    const auto& config = platform.config();
    const auto& blob = platform.deviceTreeBlob();
    std::string error;
    IndependentFdtReader reader;
    CHECK(reader.parse(blob, error));
    if (!error.empty()) std::cerr << "FDT parser: " << error << '\n';
    CHECK_EQ(AArch64DeviceTree::kMagic, be32(blob.data()));
    CHECK_EQ(17, be32(blob.data() + 20));
    CHECK_EQ(blob.size(), be32(blob.data() + 4));
    CHECK(blob.size() <= config.dtbMaxBytes);

    const std::string serialPath = "/serial@9000000";
    CHECK(reader.node("/") != nullptr);
    CHECK(reader.node("/chosen") != nullptr);
    CHECK(reader.node("/aliases") != nullptr);
    CHECK(reader.node("/cpus/cpu@0") != nullptr);
    CHECK(reader.node("/memory@0") != nullptr);
    CHECK(reader.node("/interrupt-controller@8000000") != nullptr);
    CHECK(reader.node("/timer") != nullptr);
    CHECK(reader.node(serialPath) != nullptr);
    CHECK(propertyStringEquals(reader, "/chosen", "stdout-path", serialPath));
    CHECK(propertyStringEquals(reader, "/aliases", "serial0", serialPath));
    CHECK(propertyStringEquals(reader, "/cpus/cpu@0", "device_type", "cpu"));
    CHECK(propertyCellEquals(reader, "/cpus", "#address-cells", 1));
    CHECK(propertyCellEquals(reader, "/cpus", "#size-cells", 0));
    CHECK(propertyCellEquals(reader, "/interrupt-controller@8000000", "#interrupt-cells", 3));
    CHECK(propertyCellEquals(reader, "/interrupt-controller@8000000", "phandle", 1));
    CHECK(reader.node("/interrupt-controller@8000000/gic-its") == nullptr);

    const auto* memoryReg = reader.property("/memory@0", "reg");
    CHECK(memoryReg != nullptr && memoryReg->size() == 16);
    if (memoryReg != nullptr && memoryReg->size() == 16) {
        CHECK_EQ(config.ramBase, be64(memoryReg->data()));
        CHECK_EQ(config.ramBytes, be64(memoryReg->data() + 8));
    }
    const auto* gicReg = reader.property("/interrupt-controller@8000000", "reg");
    CHECK(gicReg != nullptr && gicReg->size() == 32);
    if (gicReg != nullptr && gicReg->size() == 32) {
        CHECK_EQ(config.gicdBase, be64(gicReg->data()));
        CHECK_EQ(config.gicdApertureBytes, be64(gicReg->data() + 8));
        CHECK_EQ(config.gicrBase, be64(gicReg->data() + 16));
        CHECK_EQ(config.gicrApertureBytes, be64(gicReg->data() + 24));
    }
    const auto* uartReg = reader.property(serialPath, "reg");
    CHECK(uartReg != nullptr && uartReg->size() == 16);
    if (uartReg != nullptr && uartReg->size() == 16) {
        CHECK_EQ(config.uartBase, be64(uartReg->data()));
        CHECK_EQ(config.uartApertureBytes, be64(uartReg->data() + 8));
    }
    const auto* uartIrq = reader.property(serialPath, "interrupts");
    CHECK(uartIrq != nullptr && uartIrq->size() == 12);
    if (uartIrq != nullptr && uartIrq->size() == 12) {
        CHECK_EQ(0, be32(uartIrq->data()));
        CHECK_EQ(config.uartSpi - 32, be32(uartIrq->data() + 4));
        CHECK_EQ(4, be32(uartIrq->data() + 8));
    }
    const auto* timerIrq = reader.property("/timer", "interrupts");
    CHECK(timerIrq != nullptr && timerIrq->size() == 24);
    if (timerIrq != nullptr && timerIrq->size() == 24) {
        CHECK_EQ(1, be32(timerIrq->data()));
        CHECK_EQ(14, be32(timerIrq->data() + 4));
        CHECK_EQ(4, be32(timerIrq->data() + 8));
        CHECK_EQ(1, be32(timerIrq->data() + 12));
        CHECK_EQ(11, be32(timerIrq->data() + 16));
        CHECK_EQ(4, be32(timerIrq->data() + 20));
    }
    CHECK(propertyCellEquals(reader, "/timer", "interrupt-parent", 1));
    CHECK(propertyCellEquals(reader, serialPath, "interrupt-parent", 1));
    CHECK(propertyCellEquals(reader, serialPath, "reg-io-width", 4));

    std::vector<uint8_t> installed(blob.size());
    platform.ram().Read(config.dtbBase, installed.data(), installed.size());
    CHECK(installed == blob);

    AArch64PlatformConfig changed = config;
    changed.uartBase = 0x09100000;
    changed.uartSpi = 34;
    changed.timerFrequencyHz = 2'000'000;
    AArch64VirtualPlatform second(changed);
    IndependentFdtReader secondReader;
    CHECK(secondReader.parse(second.deviceTreeBlob(), error));
    CHECK(second.deviceTreeBlob() != blob);
    CHECK_EQ(changed.uartBase, second.uart().physicalBase());
    CHECK_EQ(changed.timerFrequencyHz, second.timer().frequencyHz());
    CHECK(secondReader.node("/serial@9100000") != nullptr);
    const auto* changedIrq = secondReader.property("/serial@9100000", "interrupts");
    CHECK(changedIrq != nullptr && be32(changedIrq->data() + 4) == 2);
}

void testMalformedFdtControls() {
    AArch64VirtualPlatform platform;
    const std::vector<uint8_t> valid = platform.deviceTreeBlob();
    IndependentFdtReader reader;
    std::string error;
    CHECK(reader.parse(valid, error));

    std::vector<uint8_t> bad = valid;
    bad.resize(12);
    CHECK(!reader.parse(bad, error));
    bad = valid;
    bad[0] = 0;
    CHECK(!reader.parse(bad, error));
    bad = valid;
    bad[8] = 0xFF; bad[9] = 0xFF; bad[10] = 0xFF; bad[11] = 0xFF;
    CHECK(!reader.parse(bad, error));
    bad = valid;
    bad[20] = 0; bad[21] = 0; bad[22] = 0; bad[23] = 16;
    CHECK(!reader.parse(bad, error));
    bad = valid;
    const uint32_t structureOffset = be32(bad.data() + 8);
    bad[structureOffset + 4] = 0xFF; // malformed root node name termination/padding
    CHECK(!reader.parse(bad, error));
}

void testConfigAndImageRejection() {
    AArch64PlatformConfig config;
    std::string error;
    CHECK(validateAArch64PlatformConfig(config, &error));
    AArch64PlatformConfig invalid = config;
    invalid.ramBase = 0x40000000;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.ramBytes = 0x01000001;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.guestDataBase = invalid.guestImageBase;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.uartBase = UINT64_MAX - 0xFFF;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.gicrBase = invalid.gicdBase;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.uartSpi = 31;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.cpuProfile = 2;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.physicalTimerPpi = 29;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.guestEntry = invalid.guestImageBase + 2;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));
    invalid = config; invalid.dtbBase = invalid.guestImageBase;
    CHECK(!validateAArch64PlatformConfig(invalid, &error));

    bool constructorRejected = false;
    try { AArch64VirtualPlatform rejected(invalid); }
    catch (const std::invalid_argument&) { constructorRejected = true; }
    CHECK(constructorRejected);

    AArch64VirtualPlatform platform;
    const std::vector<uint8_t> image = wordsToBytes(discoveryGuest());
    CHECK(!platform.loadRawGuestImage({}, config.guestEntry, &error));
    CHECK(!platform.loadRawGuestImage(image, config.guestEntry + 2, &error));
    CHECK(!platform.loadRawGuestImage(std::vector<uint8_t>(config.guestImageCapacity + 4),
                                      config.guestEntry, &error));
    CHECK(platform.cpu().cpu().pc() == config.guestEntry);
}

void testGuestDtbReadAndSnapshotReplay() {
    const AArch64PlatformConfig config;
    AArch64VirtualPlatform source(config);
    const std::vector<uint32_t> words = discoveryGuest();
    const std::vector<uint8_t> image = wordsToBytes(words);
    for (uint32_t word : words) CHECK(Cpu::decodeWord(word, config.guestImageBase).valid);
    CHECK(source.loadRawGuestImage(image, config.guestEntry));
    CHECK_EQ(config.dtbBase, source.cpu().cpu().readX(0));
    CHECK_EQ(config.guestEntry, source.cpu().cpu().pc());
    CHECK_EQ(0x3C5, source.cpu().cpu().pstate());
    CHECK_EQ(config.stackBase + config.stackBytes, source.cpu().cpu().spEl1());
    CHECK_EQ(config.exceptionVectorBase, source.cpu().cpu().vbarEl1());
    CHECK(source.cpu().cpu().exceptionLevel() == ExceptionLevel::EL1);
    CHECK_EQ(0, source.cpu().cpu().sctlrEl1() & 1u);
    CHECK_EQ(0, source.devices().deviceCount() - 3);
    CHECK_EQ(3, source.devices().interruptSourceCount());

    std::vector<uint8_t> snapshot;
    std::string error;
    CHECK(source.captureSnapshot(snapshot, &error));
    CHECK(!snapshot.empty());

    AArch64VirtualPlatform restored(config);
    CHECK(restored.loadRawGuestImage(image, config.guestEntry));
    CHECK(restored.restoreSnapshot(snapshot, &error));
    AArch64PlatformRunResult sourceResult;
    AArch64PlatformRunResult restoredResult;
    const uint64_t kCompletion = config.guestEntry + 8 * 4;
    CHECK(source.runUntilPc(kCompletion, 64, sourceResult, &error));
    CHECK(restored.runUntilPc(kCompletion, 64, restoredResult, &error));
    CHECK(sourceResult.completed && restoredResult.completed);
    CHECK_EQ(7, sourceResult.instructions);
    CHECK_EQ(sourceResult.instructions, restoredResult.instructions);
    CHECK_EQ(0xEDFE0DD0u, source.cpu().cpu().readW(1));
    CHECK_EQ(0x0DD0, source.cpu().cpu().readW(2));
    CHECK_EQ(1, source.cpu().cpu().readW(3));
    CHECK_EQ(source.cpu().cpu().pc(), restored.cpu().cpu().pc());
    CHECK_EQ(source.cpu().cpu().readX(1), restored.cpu().cpu().readX(1));
    CHECK_EQ(source.timer().counter(), restored.timer().counter());
    CHECK_EQ(kCompletion, sourceResult.currentPc);
    CHECK_EQ(0, sourceResult.exceptions);
    CHECK_EQ(0, sourceResult.interrupts);

    AArch64PlatformConfig incompatibleConfig = config;
    incompatibleConfig.uartSpi = 34;
    AArch64VirtualPlatform incompatible(incompatibleConfig);
    CHECK(incompatible.loadRawGuestImage(image, config.guestEntry));
    const uint64_t beforePc = incompatible.cpu().cpu().pc();
    uint8_t beforeByte = 0, afterByte = 0;
    incompatible.ram().Read(config.guestImageBase, &beforeByte, 1);
    CHECK(!incompatible.restoreSnapshot(snapshot, &error));
    CHECK_EQ(beforePc, incompatible.cpu().cpu().pc());
    incompatible.ram().Read(config.guestImageBase, &afterByte, 1);
    CHECK_EQ(beforeByte, afterByte);

    CHECK(source.reset(&error));
    CHECK_EQ(config.guestEntry, source.cpu().cpu().pc());
    CHECK_EQ(config.dtbBase, source.cpu().cpu().readX(0));
    CHECK_EQ(0, source.cpu().cpu().readX(1));
    AArch64PlatformRunResult rebootResult;
    CHECK(source.runUntilPc(kCompletion, 64, rebootResult, &error));
    CHECK_EQ(sourceResult.instructions, rebootResult.instructions);
    CHECK_EQ(0xEDFE0DD0u, source.cpu().cpu().readW(1));
}

void testGuestPl011OutputAndFaultDiagnostics() {
    AArch64VirtualPlatform platform;
    const auto words = uartGuest(platform.config().uartBase);
    const auto image = wordsToBytes(words);
    std::string error;
    CHECK(platform.loadRawGuestImage(image, platform.config().guestEntry, &error));
    AArch64PlatformRunResult result;
    const uint64_t completion = platform.config().guestEntry + words.size() * 4;
    CHECK(platform.runUntilPc(completion, 64, result, &error));
    CHECK_EQ(words.size(), result.instructions);
    const auto queued = platform.uart().pendingTransmitBytes();
    CHECK(queued.size() == 2 && queued[0] == 'O' && queued[1] == 'K');
    std::size_t transmitted = 0;
    CHECK(platform.uart().advanceTransmit(2, transmitted, &error));
    CHECK_EQ(2, transmitted);
    const auto output = platform.uart().outputBytes();
    CHECK(output.size() == 2 && output[0] == 'O' && output[1] == 'K');

    const std::vector<uint8_t> invalidInstruction{0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(platform.loadRawGuestImage(invalidInstruction, platform.config().guestEntry, &error));
    AArch64PlatformRunResult faultResult;
    CHECK(!platform.runUntilPc(platform.config().guestEntry + 4, 8, faultResult, &error));
    CHECK(faultResult.lastFault == Fault::UnallocatedEncoding ||
          faultResult.lastFault == Fault::UnsupportedFeature);
    CHECK(!error.empty());
    CHECK_EQ(platform.config().guestEntry, faultResult.currentPc);
}

void testOneHundredLifecycleCycles() {
    AArch64PlatformConfig config;
    config.ramBytes = 2ULL * 1024ULL * 1024ULL;
    config.stackBase = 0x001F0000;
    const auto image = wordsToBytes(discoveryGuest());
    uint64_t completedCycles = 0;
    const uint64_t completion = config.guestEntry + 8 * 4;
    for (uint64_t cycle = 0; cycle < 100; ++cycle) {
        std::string error;
        AArch64VirtualPlatform source(config);
        CHECK(source.loadRawGuestImage(image, config.guestEntry, &error));
        std::vector<uint8_t> snapshot;
        CHECK(source.captureSnapshot(snapshot, &error));
        AArch64VirtualPlatform restored(config);
        CHECK(restored.loadRawGuestImage(image, config.guestEntry, &error));
        CHECK(restored.restoreSnapshot(snapshot, &error));
        AArch64PlatformRunResult result;
        CHECK(restored.runUntilPc(completion, 64, result, &error));
        CHECK_EQ(7, result.instructions);
        CHECK_EQ(1, restored.cpu().cpu().readW(3));
        CHECK(restored.reset(&error));
        CHECK_EQ(config.guestEntry, restored.cpu().cpu().pc());
        ++completedCycles;
    }
    CHECK_EQ(100, completedCycles);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write-13r-artifacts") {
        try {
            phase13rEvidenceDirectory = std::filesystem::path(argv[2]);
            std::filesystem::create_directories(phase13rEvidenceDirectory);
            testIntegratedGuestIrqMmuAndSnapshots();
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "AARCH64-13R artifact generation exception: "
                      << exception.what() << '\n';
        }
        std::cout << "AARCH64-13R artifact checks=" << checks
                  << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--integrated-platform-only") {
        try {
            testIntegratedGuestIrqMmuAndSnapshots();
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "AARCH64-13R integrated guest exception: " << exception.what() << '\n';
        }
        std::cout << "AARCH64-13R integrated platform checks=" << checks
                  << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--integrated-guest-only") {
        try {
            testGuestFdtDiscoveryAndDynamicUart();
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "AARCH64-13R guest exception: " << exception.what() << '\n';
        }
        std::cout << "AARCH64-13R guest checks=" << checks << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--guest-parser-negative-only") {
        try {
            testGuestFdtParserNegativeControls();
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "AARCH64-13R parser negative exception: " << exception.what() << '\n';
        }
        std::cout << "AARCH64-13R parser negative checks=" << checks
                  << " failures=" << failures << '\n';
        return failures == 0 ? 0 : 1;
    }
    if (argc == 3 && std::string(argv[1]) == "--write-artifacts") {
        try {
            const std::filesystem::path directory(argv[2]);
            std::filesystem::create_directories(directory);
            AArch64VirtualPlatform platform;
            const std::vector<uint8_t> discovery = wordsToBytes(discoveryGuest());
            const std::vector<uint8_t> uart = wordsToBytes(uartGuest(platform.config().uartBase));
            std::vector<uint8_t> snapshot;
            std::string error;
            if (!platform.loadRawGuestImage(discovery, platform.config().guestEntry, &error) ||
                !platform.captureSnapshot(snapshot, &error)) {
                std::cerr << "failed to prepare AARCH64-13 artifacts: " << error << '\n';
                return 2;
            }
            if (!writeArtifact(directory / "platform_discovery_guest.bin", discovery) ||
                !writeArtifact(directory / "platform_uart_guest.bin", uart) ||
                !writeArtifact(directory / "platform_guest.dtb",
                               platform.deviceTreeBlob()) ||
                !writeArtifact(directory / "platform_entry.snapshot", snapshot)) {
                std::cerr << "failed to write AARCH64-13 guest artifacts\n";
                return 2;
            }
            std::cout << "discovery_guest_bytes=" << discovery.size()
                      << " uart_guest_bytes=" << uart.size()
                      << " dtb_bytes=" << platform.deviceTreeBlob().size()
                      << " snapshot_bytes=" << snapshot.size() << '\n';
            return 0;
        } catch (const std::exception& exception) {
            std::cerr << "artifact generation failed: " << exception.what() << '\n';
            return 2;
        }
    }
    try {
        testIndependentFdtAndConsistency();
        testGuestFdtDiscoveryAndDynamicUart();
        testGuestFdtParserNegativeControls();
        testIntegratedGuestIrqMmuAndSnapshots();
        testMalformedFdtControls();
        testConfigAndImageRejection();
        testGuestDtbReadAndSnapshotReplay();
        testGuestPl011OutputAndFaultDiagnostics();
        testOneHundredLifecycleCycles();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "AARCH64-13 unexpected exception: " << exception.what() << '\n';
    }
    std::cout << "AARCH64-13 checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

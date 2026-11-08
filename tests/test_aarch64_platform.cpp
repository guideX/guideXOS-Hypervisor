#include "aarch64/AArch64DeviceTree.h"
#include "aarch64/AArch64GICv3.h"
#include "aarch64/AArch64GenericTimer.h"
#include "aarch64/AArch64PL011.h"
#include "aarch64/AArch64VirtualPlatform.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace aarch64;

namespace {

int checks = 0;
int failures = 0;

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

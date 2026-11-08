#include "aarch64/AArch64DeviceTree.h"

#include "aarch64/AArch64VirtualPlatform.h"

#include <limits>
#include <map>
#include <stdexcept>

namespace aarch64 {
namespace {

constexpr uint32_t kBeginNode = 1;
constexpr uint32_t kEndNode = 2;
constexpr uint32_t kProperty = 3;
constexpr uint32_t kEnd = 9;
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kReservationBytes = 16;

void setError(std::string* error, const std::string& value) {
    if (error != nullptr) *error = value;
}

void appendBe32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void appendBe64(std::vector<uint8_t>& out, uint64_t value) {
    appendBe32(out, static_cast<uint32_t>(value >> 32));
    appendBe32(out, static_cast<uint32_t>(value));
}

void align4(std::vector<uint8_t>& out) {
    while ((out.size() & 3u) != 0) out.push_back(0);
}

std::vector<uint8_t> beCells(std::initializer_list<uint32_t> cells) {
    std::vector<uint8_t> bytes;
    bytes.reserve(cells.size() * 4);
    for (uint32_t cell : cells) appendBe32(bytes, cell);
    return bytes;
}

std::vector<uint8_t> beAddressSize(uint64_t address, uint64_t size) {
    std::vector<uint8_t> bytes;
    bytes.reserve(16);
    appendBe64(bytes, address);
    appendBe64(bytes, size);
    return bytes;
}

std::vector<uint8_t> stringBytes(const std::string& value) {
    std::vector<uint8_t> bytes(value.begin(), value.end());
    bytes.push_back(0);
    return bytes;
}

std::vector<uint8_t> stringList(std::initializer_list<std::string> strings) {
    std::vector<uint8_t> bytes;
    for (const std::string& value : strings) {
        bytes.insert(bytes.end(), value.begin(), value.end());
        bytes.push_back(0);
    }
    return bytes;
}

class Writer final {
public:
    void beginNode(const std::string& name) {
        appendBe32(structure_, kBeginNode);
        structure_.insert(structure_.end(), name.begin(), name.end());
        structure_.push_back(0);
        align4(structure_);
    }

    void endNode() { appendBe32(structure_, kEndNode); }

    void finish() { appendBe32(structure_, kEnd); }

    void property(const std::string& name, const std::vector<uint8_t>& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("FDT property exceeds the version-17 length field");
        appendBe32(structure_, kProperty);
        appendBe32(structure_, static_cast<uint32_t>(value.size()));
        appendBe32(structure_, stringOffset(name));
        structure_.insert(structure_.end(), value.begin(), value.end());
        align4(structure_);
    }

    void cell(const std::string& name, uint32_t value) {
        property(name, beCells({value}));
    }

    const std::vector<uint8_t>& structure() const noexcept { return structure_; }
    const std::vector<uint8_t>& strings() const noexcept { return strings_; }

private:
    uint32_t stringOffset(const std::string& name) {
        const auto found = offsets_.find(name);
        if (found != offsets_.end()) return found->second;
        if (strings_.size() > std::numeric_limits<uint32_t>::max() - name.size() - 1)
            throw std::length_error("FDT strings block exceeds the version-17 offset field");
        const uint32_t offset = static_cast<uint32_t>(strings_.size());
        strings_.insert(strings_.end(), name.begin(), name.end());
        strings_.push_back(0);
        offsets_.emplace(name, offset);
        return offset;
    }

    std::vector<uint8_t> structure_;
    std::vector<uint8_t> strings_;
    std::map<std::string, uint32_t> offsets_;
};

std::string hexAddress(uint64_t value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    do {
        result.insert(result.begin(), kHex[value & 0xfu]);
        value >>= 4;
    } while (value != 0);
    return result;
}

void appendHeaderWord(std::vector<uint8_t>& out, uint32_t value) {
    appendBe32(out, value);
}

} // namespace

bool AArch64DeviceTree::generate(const AArch64PlatformConfig& config,
                                 std::vector<uint8_t>& blob,
                                 std::string* error) {
    blob.clear();
    if (!validateAArch64PlatformConfig(config, error)) return false;

    try {
        Writer writer;
        const std::string serialPath = "/serial@" + hexAddress(config.uartBase);

        writer.beginNode("");
        writer.cell("#address-cells", 2);
        writer.cell("#size-cells", 2);
        writer.property("compatible", stringBytes("guidexos,aarch64-virtual-platform"));
        writer.property("model", stringBytes("guideXOS AArch64 virtual platform v1"));
        writer.cell("interrupt-parent", 1);

        writer.beginNode("chosen");
        writer.property("stdout-path", stringBytes(serialPath));
        writer.endNode();

        writer.beginNode("aliases");
        writer.property("serial0", stringBytes(serialPath));
        writer.endNode();

        writer.beginNode("cpus");
        writer.cell("#address-cells", 1);
        writer.cell("#size-cells", 0);
        writer.beginNode("cpu@0");
        writer.property("device_type", stringBytes("cpu"));
        writer.property("compatible", stringBytes("arm,armv8"));
        writer.cell("reg", 0);
        writer.endNode();
        writer.endNode();

        writer.beginNode("memory@" + hexAddress(config.ramBase));
        writer.property("device_type", stringBytes("memory"));
        writer.property("reg", beAddressSize(config.ramBase, config.ramBytes));
        writer.endNode();

        writer.beginNode("interrupt-controller@" + hexAddress(config.gicdBase));
        writer.property("compatible", stringBytes("arm,gic-v3"));
        writer.property("reg", [&] {
            std::vector<uint8_t> value;
            value.reserve(32);
            appendBe64(value, config.gicdBase);
            appendBe64(value, config.gicdApertureBytes);
            appendBe64(value, config.gicrBase);
            appendBe64(value, config.gicrApertureBytes);
            return value;
        }());
        writer.property("interrupt-controller", {});
        writer.cell("#interrupt-cells", 3);
        writer.cell("#address-cells", 2);
        writer.cell("#size-cells", 2);
        writer.cell("phandle", 1);
        writer.endNode();

        writer.beginNode("timer");
        writer.property("compatible", stringBytes("arm,armv8-timer"));
        // The current binding order for a non-secure EL1 profile is physical,
        // then virtual. PPI numbers are relative to the PPI class.
        writer.property("interrupts", beCells({
            1, config.physicalTimerPpi - 16, 4,
            1, config.virtualTimerPpi - 16, 4}));
        writer.property("interrupt-names", stringList({"phys", "virt"}));
        writer.property("interrupt-parent", beCells({1}));
        writer.endNode();

        writer.beginNode(serialPath.substr(1));
        writer.property("compatible", stringList({"arm,pl011", "arm,primecell"}));
        writer.property("reg", beAddressSize(config.uartBase, config.uartApertureBytes));
        writer.property("interrupts", beCells({0, config.uartSpi - 32, 4}));
        writer.property("interrupt-parent", beCells({1}));
        writer.cell("reg-io-width", 4);
        writer.property("status", stringBytes("okay"));
        writer.endNode();

        writer.endNode();
        writer.finish();

        const std::vector<uint8_t>& structure = writer.structure();
        const std::vector<uint8_t>& strings = writer.strings();
        const uint64_t reservationOffset = kHeaderBytes;
        const uint64_t structureOffset = reservationOffset + kReservationBytes;
        const uint64_t stringsOffset = structureOffset + structure.size();
        const uint64_t totalSize = stringsOffset + strings.size();
        if (totalSize > config.dtbMaxBytes ||
            totalSize > std::numeric_limits<uint32_t>::max() ||
            structure.size() > std::numeric_limits<uint32_t>::max() ||
            strings.size() > std::numeric_limits<uint32_t>::max()) {
            setError(error, "generated DTB exceeds the configured bounded blob size");
            return false;
        }

        blob.reserve(static_cast<std::size_t>(totalSize));
        appendHeaderWord(blob, kMagic);
        appendHeaderWord(blob, static_cast<uint32_t>(totalSize));
        appendHeaderWord(blob, static_cast<uint32_t>(structureOffset));
        appendHeaderWord(blob, static_cast<uint32_t>(stringsOffset));
        appendHeaderWord(blob, static_cast<uint32_t>(reservationOffset));
        appendHeaderWord(blob, kVersion);
        appendHeaderWord(blob, 16); // last compatible format version
        appendHeaderWord(blob, 0);  // boot_cpuid_phys: sole CPU has affinity 0
        appendHeaderWord(blob, static_cast<uint32_t>(strings.size()));
        appendHeaderWord(blob, static_cast<uint32_t>(structure.size()));
        blob.resize(static_cast<std::size_t>(structureOffset), 0); // empty reservation map
        blob.insert(blob.end(), structure.begin(), structure.end());
        blob.insert(blob.end(), strings.begin(), strings.end());
        if (blob.size() != totalSize) {
            blob.clear();
            setError(error, "generated DTB layout size is inconsistent");
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& exception) {
        blob.clear();
        setError(error, std::string("unable to generate bounded FDT: ") + exception.what());
        return false;
    } catch (...) {
        blob.clear();
        setError(error, "unable to generate bounded FDT");
        return false;
    }
}

} // namespace aarch64

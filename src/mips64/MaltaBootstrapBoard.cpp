#include "Mips64MaltaBootstrap.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ia64 {
namespace {

constexpr uint64_t kFpgaWindowSize = 0x900;
constexpr uint64_t kUartWindowSize = 8 * 8;
constexpr uint64_t kStatusOffset = 0x208;
constexpr uint64_t kSwitchOffset = 0x200;
constexpr uint64_t kJumperOffset = 0x210;
constexpr uint64_t kLedBarOffset = 0x408;
constexpr uint64_t kAsciiWordOffset = 0x410;
constexpr uint64_t kAsciiPosition0Offset = 0x418;
constexpr uint64_t kSoftResetOffset = 0x500;
constexpr uint64_t kBreakResetOffset = 0x508;

uint32_t encI(uint8_t op, uint8_t rs, uint8_t rt, uint16_t imm) {
    return (uint32_t(op) << 26) | (uint32_t(rs) << 21) |
           (uint32_t(rt) << 16) | imm;
}

uint32_t encR(uint8_t rs, uint8_t rt, uint8_t rd, uint8_t shamt,
              uint8_t funct) {
    return (uint32_t(rs) << 21) | (uint32_t(rt) << 16) |
           (uint32_t(rd) << 11) | (uint32_t(shamt) << 6) | funct;
}

using GuestWords = std::vector<uint32_t>;

void loadFpgaAddress(GuestWords& words, uint8_t target, uint16_t offset) {
    words.push_back(encI(0x0F, 0, target, 0xBF00)); // CKSEG1 prefix.
    words.push_back(encI(0x0D, target, target, offset));
}

void loadUartAddress(GuestWords& words, uint8_t target, uint16_t offset) {
    words.push_back(encI(0x0F, 0, target, 0xBF00));
    words.push_back(encI(0x0D, target, target, offset));
}

void emitByteStore(GuestWords& words, uint8_t base, uint8_t valueReg,
                   uint16_t offset = 0) {
    words.push_back(encI(0x28, base, valueReg, offset)); // sb
}

void emitImmediateByte(GuestWords& words, uint8_t reg, uint8_t value) {
    words.push_back(encI(0x09, 0, reg, value)); // addiu reg,zero,imm
}

GuestWords makeBootstrapWords() {
    constexpr uint8_t t0 = 8;
    constexpr uint8_t t1 = 9;
    constexpr uint8_t s0 = 16;
    GuestWords words;

    // Read and retain the native-endian FPGA board status.
    loadFpgaAddress(words, t0, static_cast<uint16_t>(kStatusOffset));
    words.push_back(encI(0x23, t0, s0, 0)); // lw $s0, STATUS($t0)

    // Drive all eight Malta LEDs with a recognizable byte.
    loadFpgaAddress(words, t0, static_cast<uint16_t>(kLedBarOffset));
    emitImmediateByte(words, t1, 0xA5);
    words.push_back(encI(0x2B, t0, t1, 0)); // sw $t1, LEDBAR($t0)

    // ASCIIWORD displays eight hexadecimal digits; ASCIIPOS0-7 then replace
    // those digits with the board label "guideXOS".
    loadFpgaAddress(words, t0, static_cast<uint16_t>(kAsciiWordOffset));
    words.push_back(encI(0x0F, 0, t1, 0x424F)); // "BOOT"
    words.push_back(encI(0x0D, t1, t1, 0x4F54));
    words.push_back(encI(0x2B, t0, t1, 0));
    loadFpgaAddress(words, t0, static_cast<uint16_t>(kAsciiPosition0Offset));
    constexpr std::array<uint8_t, 8> label{{'g', 'u', 'i', 'd', 'e', 'X', 'O', 'S'}};
    for (size_t i = 0; i < label.size(); ++i) {
        emitImmediateByte(words, t1, label[i]);
        words.push_back(encI(0x2B, t0, t1, static_cast<uint16_t>(i * 8)));
    }

    // Initialize the CBUS NS16550 through its real byte-spaced registers.
    loadUartAddress(words, t0, 0x0918); // LCR
    emitImmediateByte(words, t1, 0x80); // DLAB=1
    emitByteStore(words, t0, t1);
    loadUartAddress(words, t0, 0x0900); // DLL while DLAB=1
    emitImmediateByte(words, t1, 1);
    emitByteStore(words, t0, t1);
    loadUartAddress(words, t0, 0x0908); // DLM while DLAB=1
    emitImmediateByte(words, t1, 0);
    emitByteStore(words, t0, t1);
    loadUartAddress(words, t0, 0x0918);
    emitImmediateByte(words, t1, 3); // 8 data bits, one stop bit, no parity.
    emitByteStore(words, t0, t1);

    // Each byte below is emitted by a guest SB to the UART THR. There is no
    // host-side print routine, callback, or pre-populated console output.
    loadUartAddress(words, t0, 0x0900);
    constexpr char bootMessage[] = "guideXOS MIPS64 Malta bootstrap OK";
    for (char ch : std::string(bootMessage)) {
        emitImmediateByte(words, t1, static_cast<uint8_t>(ch));
        emitByteStore(words, t0, t1);
    }

    // Hand off through JR to the separately host-loaded CKSEG0 RAM payload.
    words.push_back(encI(0x0F, 0, t1, 0x8000));
    words.push_back(encI(0x0D, t1, t1, 0x1000));
    words.push_back(encR(t1, 0, 0, 0, 0x08)); // jr $t1
    words.push_back(0); // architectural branch-delay slot
    return words;
}

GuestWords makePayloadWords() {
    constexpr uint8_t t0 = 8;
    constexpr uint8_t t1 = 9;
    GuestWords words;

    loadUartAddress(words, t0, 0x0900);
    constexpr char payloadMessage[] = "MIPS64 RAM payload running";
    for (char ch : std::string(payloadMessage)) {
        emitImmediateByte(words, t1, static_cast<uint8_t>(ch));
        emitByteStore(words, t0, t1);
    }

    // Publish guest completion only after the payload has emitted its text.
    words.push_back(encI(0x0F, 0, t0, 0x8000));
    words.push_back(encI(0x0D, t0, t0, 0x2000));
    words.push_back(encI(0x0F, 0, t1, 0x4758));
    words.push_back(encI(0x0D, t1, t1, 0x4F53)); // "GXOS"
    words.push_back(encI(0x2B, t0, t1, 0));
    words.push_back(encI(0x04, 0, 0, 0xFFFF)); // beq $zero,$zero,self
    words.push_back(0); // branch-delay slot
    return words;
}

std::vector<uint8_t> encodeWords(const GuestWords& words,
                                mips64::Mips64Endianness endianness,
                                size_t imageSize) {
    if (words.empty() || words.size() * 4 > imageSize) {
        throw std::invalid_argument("MIPS64 Malta guest fixture does not fit its image window");
    }
    std::vector<uint8_t> bytes(imageSize, 0); // zero is the encoded NOP.
    const bool little = endianness == mips64::Mips64Endianness::Little;
    for (size_t index = 0; index < words.size(); ++index) {
        const uint32_t word = words[index];
        for (unsigned byte = 0; byte < 4; ++byte) {
            const unsigned shift = 8 * (little ? byte : 3 - byte);
            bytes[index * 4 + byte] = static_cast<uint8_t>(word >> shift);
        }
    }
    return bytes;
}

bool overlaps(uint64_t aBase, uint64_t aSize, uint64_t bBase, uint64_t bSize) {
    return aBase < bBase + bSize && bBase < aBase + aSize;
}

void verifyNoOverlap(const PhysicalAddressMap& bus, uint64_t base, uint64_t size) {
    for (const PhysicalRegionInfo& region : bus.Regions()) {
        if (overlaps(base, size, region.base, region.size)) {
            std::ostringstream message;
            message << "Malta profile mapping at 0x" << std::hex << base
                    << " overlaps an existing physical region at 0x" << region.base;
            throw PhysicalBusError(PhysicalBusErrorCode::Overlap, message.str());
        }
    }
}

uint32_t readWord(const uint8_t* source, bool little) noexcept {
    uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) {
        const unsigned shift = 8 * (little ? byte : 3 - byte);
        value |= uint32_t(source[byte]) << shift;
    }
    return value;
}

void writeWord(uint32_t value, uint8_t* destination, bool little) noexcept {
    for (unsigned byte = 0; byte < 4; ++byte) {
        const unsigned shift = 8 * (little ? byte : 3 - byte);
        destination[byte] = static_cast<uint8_t>(value >> shift);
    }
}

}  // namespace

MaltaFpgaRegisters::MaltaFpgaRegisters(
    mips64::Mips64Endianness endianness, uint8_t switches, uint8_t jumpers,
    std::shared_ptr<MaltaResetMailbox> resetMailbox)
    : endianness_(endianness), switches_(switches), jumpers_(jumpers),
      resetMailbox_(std::move(resetMailbox)) {
    if (!resetMailbox_) {
        throw std::invalid_argument("Malta FPGA reset mailbox cannot be null");
    }
    Reset();
}

uint32_t MaltaFpgaRegisters::DecodeWord(const uint8_t* source) const noexcept {
    return readWord(source, endianness_ == mips64::Mips64Endianness::Little);
}

void MaltaFpgaRegisters::EncodeWord(uint32_t value, uint8_t* destination) const noexcept {
    writeWord(value, destination, endianness_ == mips64::Mips64Endianness::Little);
}

uint32_t MaltaFpgaRegisters::ReadRegister(uint64_t offset) const {
    switch (offset) {
        case kSwitchOffset: return switches_;
        case kStatusOffset:
            return endianness_ == mips64::Mips64Endianness::Big ? 0x12U : 0x10U;
        case kJumperOffset: return jumpers_;
        case kLedBarOffset: return ledBar_;
        case kSoftResetOffset: return 0;
        case kBreakResetOffset: return breakReset_;
        default:
            if (offset >= kAsciiPosition0Offset && offset <= 0x450 &&
                ((offset - kAsciiPosition0Offset) % 8) == 0) {
                throw PhysicalBusError(PhysicalBusErrorCode::PermissionDenied,
                                       "Malta FPGA ASCII display registers are write-only");
            }
            if (offset == kAsciiWordOffset) {
                throw PhysicalBusError(PhysicalBusErrorCode::PermissionDenied,
                                       "Malta FPGA ASCIIWORD is write-only");
            }
            throw PhysicalBusError(PhysicalBusErrorCode::Unmapped,
                                   "unsupported Malta FPGA register read");
    }
}

void MaltaFpgaRegisters::WriteRegister(uint64_t offset, uint32_t value) {
    switch (offset) {
        case kSwitchOffset:  // Hardware switches are guest read-only.
        case kJumperOffset:  // Jumper state is guest read-only.
        case kStatusOffset:  // Board identification is guest read-only.
            return;
        case kLedBarOffset:
            ledBar_ = static_cast<uint8_t>(value & 0xFF);
            return;
        case kAsciiWordOffset: {
            constexpr char digits[] = "0123456789ABCDEF";
            for (size_t index = 0; index < asciiDisplay_.size(); ++index) {
                const unsigned shift = static_cast<unsigned>((7 - index) * 4);
                asciiDisplay_[index] = digits[(value >> shift) & 0xF];
            }
            return;
        }
        case kSoftResetOffset:
            if (value == 0x42 && !resetMailbox_->pending.exchange(true)) {
                resetMailbox_->requests.fetch_add(1);
            }
            return;
        case kBreakResetOffset:
            breakReset_ = static_cast<uint8_t>(value & 0xFF);
            return;
        default:
            if (offset >= kAsciiPosition0Offset && offset <= 0x450 &&
                ((offset - kAsciiPosition0Offset) % 8) == 0) {
                asciiDisplay_[(offset - kAsciiPosition0Offset) / 8] =
                    static_cast<char>(value & 0xFF);
                return;
            }
            throw PhysicalBusError(PhysicalBusErrorCode::Unmapped,
                                   "unsupported Malta FPGA register write");
    }
}

void MaltaFpgaRegisters::Read(uint64_t offset, uint8_t* destination, size_t size) const {
    if (destination == nullptr || size != 4 || (offset & 3) != 0) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "Malta FPGA requires aligned 32-bit register reads");
    }
    EncodeWord(ReadRegister(offset), destination);
}

void MaltaFpgaRegisters::Write(uint64_t offset, const uint8_t* source, size_t size) {
    if (source == nullptr || size != 4 || (offset & 3) != 0) {
        throw PhysicalBusError(PhysicalBusErrorCode::InvalidWidth,
                               "Malta FPGA requires aligned 32-bit register writes");
    }
    WriteRegister(offset, DecodeWord(source));
}

void MaltaFpgaRegisters::Reset() {
    ledBar_ = 0;
    breakReset_ = 0x0A;
    asciiDisplay_.fill(' ');
    resetMailbox_->pending.store(false);
}

std::map<std::string, uint64_t> MaltaFpgaRegisters::InspectState() const {
    return {{"SWITCH", switches_}, {"STATUS", endianness_ == mips64::Mips64Endianness::Big ? 0x12U : 0x10U},
            {"JMPRS", jumpers_}, {"LEDBAR", ledBar_}, {"BRKRES", breakReset_},
            {"SOFTRES_pending", ResetPending()}, {"SOFTRES_requests", ResetRequestCount()}};
}

bool MaltaFpgaRegisters::ResetPending() const noexcept {
    return resetMailbox_->pending.load();
}

uint64_t MaltaFpgaRegisters::ResetRequestCount() const noexcept {
    return resetMailbox_->requests.load();
}

MaltaFpgaRegisters::Snapshot MaltaFpgaRegisters::CaptureSnapshot() const noexcept {
    Snapshot snapshot;
    snapshot.endianness = endianness_;
    snapshot.switches = switches_;
    snapshot.jumpers = jumpers_;
    snapshot.ledBar = ledBar_;
    snapshot.asciiDisplay = asciiDisplay_;
    snapshot.breakReset = breakReset_;
    snapshot.resetPending = resetMailbox_->pending.load();
    snapshot.resetRequestCount = resetMailbox_->requests.load();
    return snapshot;
}

bool MaltaFpgaRegisters::ValidateSnapshot(const Snapshot& snapshot) const noexcept {
    return snapshot.version == 1 && snapshot.endianness == endianness_ &&
           snapshot.switches == switches_ && snapshot.jumpers == jumpers_ &&
           (!snapshot.resetPending || snapshot.resetRequestCount != 0);
}

void MaltaFpgaRegisters::RestoreSnapshot(const Snapshot& snapshot) noexcept {
    ledBar_ = snapshot.ledBar;
    asciiDisplay_ = snapshot.asciiDisplay;
    breakReset_ = snapshot.breakReset;
    resetMailbox_->requests.store(snapshot.resetRequestCount);
    resetMailbox_->pending.store(snapshot.resetPending);
}

std::vector<uint8_t> MaltaBootstrapBoard::BuildDemoBootRom(
    mips64::Mips64Endianness endianness) {
    return encodeWords(makeBootstrapWords(), endianness,
                       static_cast<size_t>(kMaltaBootImageWindow));
}

std::vector<uint8_t> MaltaBootstrapBoard::BuildDemoRamPayload(
    mips64::Mips64Endianness endianness) {
    return encodeWords(makePayloadWords(), endianness, 256);
}

MaltaBootstrapBoard::MaltaBootstrapBoard(PhysicalAddressMap& bus,
                                         const MaltaBootstrapConfig& config)
    : config_(config), consoleSink_(std::make_shared<GuestConsoleSink>()),
      resetMailbox_(std::make_shared<MaltaResetMailbox>()) {
    if (config_.endianness != mips64::Mips64Endianness::Little &&
        config_.endianness != mips64::Mips64Endianness::Big) {
        throw std::invalid_argument("unsupported MIPS64 Malta endianness");
    }
    if (config_.bootRom.empty()) {
        if (!config_.useBuiltInDemoRom) {
            throw std::invalid_argument("MIPS64 Malta profile requires a non-empty boot ROM image");
        }
        bootRom_ = BuildDemoBootRom(config_.endianness);
    } else {
        if (config_.bootRom.size() > kMaltaBootImageWindow ||
            (config_.bootRom.size() & 3) != 0) {
            throw std::invalid_argument("MIPS64 Malta ROM must be word-aligned and fit the 4 MiB window");
        }
        bootRom_.assign(static_cast<size_t>(kMaltaBootImageWindow), 0);
        std::copy(config_.bootRom.begin(), config_.bootRom.end(), bootRom_.begin());
    }
    const uint64_t flashSize = config_.mapFlashMirror ? kMaltaBootImageWindow : 0;
    if (flashSize != 0) verifyNoOverlap(bus, kMaltaFlashBase, flashSize);
    verifyNoOverlap(bus, kMaltaResetRomBase, kMaltaBootImageWindow);
    verifyNoOverlap(bus, kMaltaFpgaBase, kFpgaWindowSize);
    verifyNoOverlap(bus, kMaltaUartBase, kUartWindowSize);

    uart_ = std::make_shared<Ns16550Uart>(consoleSink_, 3);
    fpga_ = std::make_shared<MaltaFpgaRegisters>(
        config_.endianness, config_.switches, config_.jumpers, resetMailbox_);

    std::vector<uint64_t> registered;
    try {
        if (config_.mapFlashMirror) {
            bus.RegisterRom(kMaltaFlashBase, bootRom_);
            registered.push_back(kMaltaFlashBase);
        }
        bus.RegisterRom(kMaltaResetRomBase, bootRom_);
        registered.push_back(kMaltaResetRomBase);
        bus.RegisterMmio(kMaltaFpgaBase, kFpgaWindowSize, fpga_, {4});
        registered.push_back(kMaltaFpgaBase);
        bus.RegisterMmio(kMaltaUartBase, kUartWindowSize, uart_, {1});
        registered.push_back(kMaltaUartBase);
    } catch (...) {
        for (auto it = registered.rbegin(); it != registered.rend(); ++it) {
            bus.RemoveRegion(*it);
        }
        throw;
    }
}

bool MaltaBootstrapBoard::ConsumeSoftwareResetRequest() noexcept {
    return resetMailbox_->pending.exchange(false);
}

bool MaltaBootstrapBoard::HasSoftwareResetRequest() const noexcept {
    return resetMailbox_->pending.load();
}

uint64_t MaltaBootstrapBoard::SoftwareResetRequestCount() const noexcept {
    return resetMailbox_->requests.load();
}

}  // namespace ia64

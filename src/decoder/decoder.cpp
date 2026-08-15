#include "decoder.h"
#include "decoder.h"
#include "ia64_decoders.h"
#include "ia64_formats.h"
#include "ia64_opcodes.h"
#include "cpu_state.h"
#include "memory.h"
#include <sstream>
#include <iomanip>
#include <iostream>
#include <cstring>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <limits>

namespace ia64 {

const char* DescribeIA64Template(TemplateType templateType) {
    switch (templateType) {
        case TemplateType::MII: return "MII";
        case TemplateType::MII_STOP: return "MII_STOP";
        case TemplateType::MI_I: return "MI_I";
        case TemplateType::MI_I_STOP: return "MI_I_STOP";
        case TemplateType::MLX: return "MLX";
        case TemplateType::MLX_STOP: return "MLX_STOP";
        case TemplateType::MMI: return "MMI";
        case TemplateType::MMI_STOP: return "MMI_STOP";
        case TemplateType::M_MI: return "M_MI";
        case TemplateType::M_MI_STOP: return "M_MI_STOP";
        case TemplateType::MFI: return "MFI";
        case TemplateType::MFI_STOP: return "MFI_STOP";
        case TemplateType::MMF: return "MMF";
        case TemplateType::MMF_STOP: return "MMF_STOP";
        case TemplateType::MIB: return "MIB";
        case TemplateType::MIB_STOP: return "MIB_STOP";
        case TemplateType::MBB: return "MBB";
        case TemplateType::MBB_STOP: return "MBB_STOP";
        case TemplateType::BBB: return "BBB";
        case TemplateType::BBB_STOP: return "BBB_STOP";
        case TemplateType::MMB: return "MMB";
        case TemplateType::MMB_STOP: return "MMB_STOP";
        case TemplateType::MFB: return "MFB";
        case TemplateType::MFB_STOP: return "MFB_STOP";
        case TemplateType::INVALID:
        default:
            return "INVALID";
    }
}

const char* DescribeIA64SlotType(UnitType unitType) {
    switch (unitType) {
        case UnitType::M_UNIT: return "M";
        case UnitType::I_UNIT: return "I";
        case UnitType::F_UNIT: return "F";
        case UnitType::B_UNIT: return "B";
        case UnitType::L_UNIT: return "L";
        case UnitType::X_UNIT: return "X";
        case UnitType::INVALID:
        default:
            return "?";
    }
}

const char* DescribeIA64DecoderFamily(UnitType unitType, uint8_t majorOpcode) {
    switch (unitType) {
        case UnitType::M_UNIT:
            return (majorOpcode <= 0x7) ? "M-type" : "A-type";
        case UnitType::I_UNIT:
            return (majorOpcode <= 0x7) ? "I-type" : "A-type";
        case UnitType::F_UNIT:
            return "F-type";
        case UnitType::B_UNIT:
            return "B-type";
        case UnitType::L_UNIT:
            return "L-unit";
        case UnitType::X_UNIT:
            return "X-type";
        case UnitType::INVALID:
        default:
            return "unknown";
    }
}

uint8_t ExtractIA64MajorOpcode(uint64_t slotBits) {
    return static_cast<uint8_t>((slotBits >> 37) & 0x0F);
}

std::string FormatIA64UnknownSlot(uint64_t bundleIP,
                                  size_t slotIndex,
                                  TemplateType templateType,
                                  UnitType unitType,
                                  uint64_t rawBits,
                                  bool fallbackPath) {
    const uint64_t raw41 = rawBits & 0x1FFFFFFFFFFULL;
    const uint8_t major = ExtractIA64MajorOpcode(raw41);
    const uint8_t x3 = static_cast<uint8_t>((raw41 >> 33) & 0x7);
    const uint8_t x6 = static_cast<uint8_t>((raw41 >> 27) & 0x3F);

    std::ostringstream oss;
    oss << "UNKNOWN IA64 SLOT: IP=0x" << std::hex << bundleIP
        << ", slot=" << std::dec << slotIndex
        << ", template=0x" << std::hex << static_cast<unsigned>(static_cast<uint8_t>(templateType))
        << "(" << DescribeIA64Template(templateType) << ")"
        << ", slotType=" << DescribeIA64SlotType(unitType)
        << ", raw41=0x" << raw41
        << ", major=0x" << static_cast<unsigned>(major)
        << ", x3=0x" << static_cast<unsigned>(x3)
        << ", x6=0x" << static_cast<unsigned>(x6)
        << ", path=" << (fallbackPath ? "fallback" : "normal")
        << ", decoder=" << DescribeIA64DecoderFamily(unitType, major);
    return oss.str();
}

// InstructionEx implementation
InstructionEx::InstructionEx() 
    : type_(InstructionType::NOP)
    , unit_(UnitType::I_UNIT)
    , rawBits_(0)
    , predicate_(0)
    , predicate2_(0)
    , dst_(0)
    , src1_(0)
    , src2_(0)
    , src3_(0)
    , immediate_(0)
    , hasImmediate_(false)
    , branchTarget_(0)
    , hasBranchTarget_(false)
    , compareCompleter_(CompareCompleter::NORMAL)
{}

InstructionEx::InstructionEx(InstructionType type, UnitType unit)
    : type_(type)
    , unit_(unit)
    , rawBits_(0)
    , predicate_(0)
    , predicate2_(0)
    , dst_(0)
    , src1_(0)
    , src2_(0)
    , src3_(0)
    , immediate_(0)
    , hasImmediate_(false)
    , branchTarget_(0)
    , hasBranchTarget_(false)
    , compareCompleter_(CompareCompleter::NORMAL)
{}

void InstructionEx::SetOperands(uint8_t dst, uint8_t src1, uint8_t src2) {
    dst_ = dst;
    src1_ = src1;
    src2_ = src2;
}

void InstructionEx::SetOperands4(uint8_t dst, uint8_t src1, uint8_t src2, uint8_t src3) {
    dst_ = dst;
    src1_ = src1;
    src2_ = src2;
    src3_ = src3;
}

void InstructionEx::SetImmediate(uint64_t imm) {
    immediate_ = imm;
    hasImmediate_ = true;
}

void InstructionEx::SetBranchTarget(uint64_t target) {
    branchTarget_ = target;
    hasBranchTarget_ = true;
}

// Helper function to check if predicate is true
static bool CheckPredicate(const CPUState& cpu, uint8_t predReg) {
    if (predReg == 0) {
        return true;  // PR0 is always true
    }
    return cpu.GetPR(predReg);
}

// Helper for sign extension
static int64_t SignExtend(uint64_t value, int bits) {
    uint64_t sign_bit = 1ULL << (bits - 1);
    if (value & sign_bit) {
        uint64_t mask = ~((1ULL << bits) - 1);
        return static_cast<int64_t>(value | mask);
    }
    return static_cast<int64_t>(value);
}

static uint64_t DecodeMovPrRotImmediate(uint64_t slotBits) {
    // I24: mov pr.rot = imm44. The 28-bit encoded immediate is split across
    // the I-format payload and sign-extended before the low 16 bits are restored.
    const uint64_t imm27 = (slotBits >> 6) & 0x7FFFFFFULL;
    const uint64_t sign = (slotBits >> 36) & 0x1ULL;
    const uint64_t imm28 = imm27 | (sign << 27);
    return static_cast<uint64_t>(SignExtend(imm28, 28)) << 16;
}

struct UInt128Parts {
    uint64_t lo;
    uint64_t hi;
};

static UInt128Parts MultiplyUnsigned64(uint64_t left, uint64_t right) {
    const uint64_t leftLo = left & 0xFFFFFFFFULL;
    const uint64_t leftHi = left >> 32;
    const uint64_t rightLo = right & 0xFFFFFFFFULL;
    const uint64_t rightHi = right >> 32;
    const uint64_t p0 = leftLo * rightLo;
    const uint64_t p1 = leftLo * rightHi;
    const uint64_t p2 = leftHi * rightLo;
    const uint64_t p3 = leftHi * rightHi;

    UInt128Parts result{p0, p3};
    const uint64_t middle = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) +
                            (p2 & 0xFFFFFFFFULL);
    result.lo = (p0 & 0xFFFFFFFFULL) | (middle << 32);
    result.hi += (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    return result;
}

static UInt128Parts MultiplySigned64(uint64_t left, uint64_t right) {
    UInt128Parts result = MultiplyUnsigned64(left, right);
    if ((left >> 63) != 0) {
        result.hi -= right;
    }
    if ((right >> 63) != 0) {
        result.hi -= left;
    }
    return result;
}

static UInt128Parts AddUInt128(UInt128Parts left, uint64_t right) {
    const uint64_t oldLo = left.lo;
    left.lo += right;
    if (left.lo < oldLo) {
        ++left.hi;
    }
    return left;
}

static uint64_t ReadLittleEndian64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(value); ++i) {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

static void WriteLittleEndian64(uint8_t* bytes, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); ++i) {
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

static bool IsNatVal(const uint8_t* bytes) {
    const uint64_t significand = ReadLittleEndian64(bytes);
    const uint64_t signAndExponent = ReadLittleEndian64(bytes + 8);
    return significand == 0 &&
           (signAndExponent & 0x3FFFFULL) == 0x1FFFEULL;
}

static void WriteNatVal(uint8_t* bytes) {
    std::memset(bytes, 0, 16);
    WriteLittleEndian64(bytes + 8, 0x1FFFEULL);
}

// F1 arithmetic works on the IA-64 register format rather than the host
// floating-point format.  A small fixed-width integer is sufficient here:
// the exact product is at most 128 bits and the addend can be aligned with it
// for every result that can affect a 64-bit register-format significand.
struct WideInteger {
    static constexpr size_t kLimbCount = 12;
    std::array<uint32_t, kLimbCount> limbs{};

    bool IsZero() const {
        for (uint32_t limb : limbs) {
            if (limb != 0) {
                return false;
            }
        }
        return true;
    }

    int BitLength() const {
        for (size_t i = kLimbCount; i-- > 0;) {
            const uint32_t limb = limbs[i];
            if (limb == 0) {
                continue;
            }
            int bits = 0;
            uint32_t value = limb;
            while (value != 0) {
                value >>= 1;
                ++bits;
            }
            return static_cast<int>(i * 32 + bits);
        }
        return 0;
    }

    uint64_t Low64() const {
        return static_cast<uint64_t>(limbs[0]) |
               (static_cast<uint64_t>(limbs[1]) << 32);
    }

    bool TestBit(size_t bit) const {
        const size_t limb = bit / 32;
        return limb < kLimbCount && ((limbs[limb] >> (bit % 32)) & 1U) != 0;
    }

    bool AnyBelow(size_t bitCount) const {
        const size_t fullLimbs = bitCount / 32;
        for (size_t i = 0; i < std::min(fullLimbs, kLimbCount); ++i) {
            if (limbs[i] != 0) {
                return true;
            }
        }
        if (fullLimbs < kLimbCount && (bitCount % 32) != 0) {
            const uint32_t mask = (1U << (bitCount % 32)) - 1U;
            return (limbs[fullLimbs] & mask) != 0;
        }
        return false;
    }

    WideInteger ShiftLeft(size_t bits) const {
        WideInteger result;
        if (bits >= kLimbCount * 32) {
            return result;
        }
        const size_t limbShift = bits / 32;
        const size_t bitShift = bits % 32;
        for (size_t i = 0; i + limbShift < kLimbCount; ++i) {
            result.limbs[i + limbShift] |= limbs[i] << bitShift;
            if (bitShift != 0 && i + limbShift + 1 < kLimbCount) {
                result.limbs[i + limbShift + 1] |= limbs[i] >> (32 - bitShift);
            }
        }
        return result;
    }

    WideInteger ShiftRight(size_t bits) const {
        WideInteger result;
        if (bits >= kLimbCount * 32) {
            return result;
        }
        const size_t limbShift = bits / 32;
        const size_t bitShift = bits % 32;
        for (size_t i = 0; i + limbShift < kLimbCount; ++i) {
            const size_t source = i + limbShift;
            result.limbs[i] |= limbs[source] >> bitShift;
            if (bitShift != 0 && source + 1 < kLimbCount) {
                result.limbs[i] |= limbs[source + 1] << (32 - bitShift);
            }
        }
        return result;
    }

    static WideInteger FromU64(uint64_t value) {
        WideInteger result;
        result.limbs[0] = static_cast<uint32_t>(value);
        result.limbs[1] = static_cast<uint32_t>(value >> 32);
        return result;
    }

    static WideInteger FromUInt128(const UInt128Parts& value) {
        WideInteger result;
        result.limbs[0] = static_cast<uint32_t>(value.lo);
        result.limbs[1] = static_cast<uint32_t>(value.lo >> 32);
        result.limbs[2] = static_cast<uint32_t>(value.hi);
        result.limbs[3] = static_cast<uint32_t>(value.hi >> 32);
        return result;
    }

    static WideInteger Add(const WideInteger& left, const WideInteger& right) {
        WideInteger result;
        uint64_t carry = 0;
        for (size_t i = 0; i < kLimbCount; ++i) {
            const uint64_t sum = static_cast<uint64_t>(left.limbs[i]) +
                                 right.limbs[i] + carry;
            result.limbs[i] = static_cast<uint32_t>(sum);
            carry = sum >> 32;
        }
        return result;
    }

    // The caller must provide left >= right.
    static WideInteger Subtract(const WideInteger& left, const WideInteger& right) {
        WideInteger result;
        uint64_t borrow = 0;
        for (size_t i = 0; i < kLimbCount; ++i) {
            const uint64_t subtrahend = static_cast<uint64_t>(right.limbs[i]) + borrow;
            result.limbs[i] = static_cast<uint32_t>(
                static_cast<uint64_t>(left.limbs[i]) - subtrahend);
            borrow = static_cast<uint64_t>(left.limbs[i]) < subtrahend ? 1 : 0;
        }
        return result;
    }

    static int Compare(const WideInteger& left, const WideInteger& right) {
        for (size_t i = kLimbCount; i-- > 0;) {
            if (left.limbs[i] != right.limbs[i]) {
                return left.limbs[i] < right.limbs[i] ? -1 : 1;
            }
        }
        return 0;
    }
};

struct SignedWideInteger {
    bool negative = false;
    WideInteger magnitude;
};

static SignedWideInteger AddSignedWide(const SignedWideInteger& left,
                                       const SignedWideInteger& right) {
    SignedWideInteger result;
    if (left.negative == right.negative) {
        result.negative = left.negative;
        result.magnitude = WideInteger::Add(left.magnitude, right.magnitude);
        return result;
    }

    const int comparison = WideInteger::Compare(left.magnitude, right.magnitude);
    if (comparison == 0) {
        return result;
    }
    if (comparison > 0) {
        result.negative = left.negative;
        result.magnitude = WideInteger::Subtract(left.magnitude, right.magnitude);
    } else {
        result.negative = right.negative;
        result.magnitude = WideInteger::Subtract(right.magnitude, left.magnitude);
    }
    return result;
}

struct IA64FloatingValue {
    uint64_t significand = 0;
    uint32_t exponent = 0;
    bool negative = false;
    bool natVal = false;
};

static IA64FloatingValue ReadIA64FloatingValue(const uint8_t* bytes) {
    const uint64_t signAndExponent = ReadLittleEndian64(bytes + 8);
    IA64FloatingValue value;
    value.significand = ReadLittleEndian64(bytes);
    value.exponent = static_cast<uint32_t>(signAndExponent & 0x1FFFFULL);
    value.negative = (signAndExponent & (1ULL << 17)) != 0;
    value.natVal = IsNatVal(bytes);
    return value;
}

enum class IA64FloatingClass {
    Zero,
    Finite,
    PositiveInfinity,
    NegativeInfinity,
    NaN,
    Unsupported,
    PseudoZero,
};

static IA64FloatingClass ClassifyIA64FloatingValue(const IA64FloatingValue& value) {
    if (value.natVal) {
        // NaTVal is handled before classification by the approximation
        // instructions and is not an IEEE value.
        return IA64FloatingClass::Unsupported;
    }
    if (value.exponent == 0x1FFFFU) {
        if (value.significand == 0x8000000000000000ULL) {
            return value.negative ? IA64FloatingClass::NegativeInfinity
                                   : IA64FloatingClass::PositiveInfinity;
        }
        return value.significand & 0x8000000000000000ULL
            ? IA64FloatingClass::NaN
            : IA64FloatingClass::Unsupported;
    }
    if (value.significand == 0) {
        return value.exponent == 0 ? IA64FloatingClass::Zero
                                   : IA64FloatingClass::PseudoZero;
    }
    return IA64FloatingClass::Finite;
}

static bool IsIA64Finite(IA64FloatingClass value) {
    return value == IA64FloatingClass::Finite;
}

static bool IsIA64ZeroLike(IA64FloatingClass value) {
    return value == IA64FloatingClass::Zero ||
           value == IA64FloatingClass::PseudoZero;
}

static bool IsIA64NaNLike(IA64FloatingClass value) {
    return value == IA64FloatingClass::NaN ||
           value == IA64FloatingClass::Unsupported;
}

static long double IA64ToLongDouble(const IA64FloatingValue& value) {
    if (value.significand == 0) {
        return value.negative ? -0.0L : 0.0L;
    }

    // The register format uses an explicit integer bit and a bias of 65535.
    // Exponent zero has the double-extended denormal scale from Volume 1,
    // rather than the normal biased-exponent formula.
    const int exponent = value.exponent == 0
        ? -16382 - 63
        : static_cast<int>(value.exponent) - 65535 - 63;
    const long double magnitude = std::ldexp(
        static_cast<long double>(value.significand), exponent);
    return value.negative ? -magnitude : magnitude;
}

static void WriteIA64Zero(uint8_t* bytes, bool negative) {
    std::memset(bytes, 0, 16);
    WriteLittleEndian64(bytes + 8, negative ? (1ULL << 17) : 0);
}

static void WriteIA64Infinity(uint8_t* bytes, bool negative) {
    std::memset(bytes, 0, 16);
    WriteLittleEndian64(bytes, 0x8000000000000000ULL);
    WriteLittleEndian64(bytes + 8, 0x1FFFFULL | (negative ? (1ULL << 17) : 0));
}

static void WriteIA64NaN(uint8_t* bytes) {
    std::memset(bytes, 0, 16);
    // Canonical quiet NaN: exponent all ones and significand 1.10...0.
    WriteLittleEndian64(bytes, 0xC000000000000000ULL);
    WriteLittleEndian64(bytes + 8, 0x1FFFFULL);
}

static void WriteIA64Approximation(uint8_t* bytes,
                                   long double value,
                                   bool negative) {
    if (value == 0.0L) {
        WriteIA64Zero(bytes, negative);
        return;
    }
    if (!std::isfinite(value)) {
        WriteIA64Infinity(bytes, negative);
        return;
    }

    const long double magnitude = std::fabs(value);
    int exponent = 0;
    const long double fraction = std::frexp(magnitude, &exponent);
    const long double scaled = std::ldexp(fraction, 64);
    const long double highPart = std::floor(scaled / 4294967296.0L);
    uint64_t high = static_cast<uint64_t>(highPart);
    uint64_t low = static_cast<uint64_t>(std::floor(
        scaled - highPart * 4294967296.0L + 0.5L));
    if (low == 0x100000000ULL) {
        low = 0;
        ++high;
    }

    int64_t biasedExponent = static_cast<int64_t>(65535) + exponent - 1;
    if (high >= 0x100000000ULL) {
        // Rounding 1.111... upward produces 1.000... at the next exponent.
        high = 0x80000000ULL;
        low = 0;
        ++biasedExponent;
    }

    if (biasedExponent >= 0x1FFFF) {
        WriteIA64Infinity(bytes, negative);
        return;
    }
    if (biasedExponent <= 0) {
        // This path is outside the normal Debian/Gentoo operands. Preserve a
        // finite result without manufacturing an unsupported encoding.
        WriteIA64Zero(bytes, negative);
        return;
    }

    std::memset(bytes, 0, 16);
    WriteLittleEndian64(bytes, (high << 32) | low);
    WriteLittleEndian64(bytes + 8,
                        static_cast<uint64_t>(biasedExponent) |
                        (negative ? (1ULL << 17) : 0));
}

static void ExecuteReciprocalApproximation(CPUState& cpu,
                                            InstructionType type,
                                            uint64_t rawBits,
                                            uint8_t destination,
                                            uint8_t source1,
                                            uint8_t source2,
                                            uint8_t predicate2) {
    // sf selects the architectural FPSR status bank. guideXOS currently has
    // no floating-point exception/status implementation, so the numerical
    // result is independent of this field; retaining it in rawBits keeps the
    // decode/disassembly and future FPSR integration correct.
    (void)rawBits;

    uint8_t source1Bytes[16] = {};
    uint8_t source2Bytes[16] = {};
    uint8_t resultBytes[16] = {};
    cpu.GetFR(source1, source1Bytes);
    const IA64FloatingValue first = ReadIA64FloatingValue(source1Bytes);

    if (type == InstructionType::FRSQRTA) {
        if (first.natVal) {
            WriteNatVal(resultBytes);
            cpu.SetFR(destination, resultBytes);
            cpu.SetPR(predicate2, false);
            return;
        }

        const IA64FloatingClass classification = ClassifyIA64FloatingValue(first);
        if (IsIA64ZeroLike(classification)) {
            WriteIA64Infinity(resultBytes, first.negative);
            cpu.SetPR(predicate2, false);
        } else if (classification == IA64FloatingClass::PositiveInfinity) {
            WriteIA64Zero(resultBytes, false);
            cpu.SetPR(predicate2, false);
        } else if (IsIA64Finite(classification) && !first.negative) {
            const long double input = IA64ToLongDouble(first);
            WriteIA64Approximation(resultBytes, 1.0L / std::sqrt(input), false);
            cpu.SetPR(predicate2, true);
        } else {
            WriteIA64NaN(resultBytes);
            cpu.SetPR(predicate2, false);
        }
        cpu.SetFR(destination, resultBytes);
        return;
    }

    cpu.GetFR(source2, source2Bytes);
    const IA64FloatingValue second = ReadIA64FloatingValue(source2Bytes);
    if (first.natVal || second.natVal) {
        WriteNatVal(resultBytes);
        cpu.SetFR(destination, resultBytes);
        cpu.SetPR(predicate2, false);
        return;
    }

    const IA64FloatingClass numeratorClass = ClassifyIA64FloatingValue(first);
    const IA64FloatingClass denominatorClass = ClassifyIA64FloatingValue(second);
    const bool numeratorZero = IsIA64ZeroLike(numeratorClass);
    const bool denominatorZero = IsIA64ZeroLike(denominatorClass);
    const bool numeratorFinite = IsIA64Finite(numeratorClass);
    const bool denominatorFinite = IsIA64Finite(denominatorClass);
    const bool numeratorInfinity = numeratorClass == IA64FloatingClass::PositiveInfinity ||
                                   numeratorClass == IA64FloatingClass::NegativeInfinity;
    const bool denominatorInfinity = denominatorClass == IA64FloatingClass::PositiveInfinity ||
                                     denominatorClass == IA64FloatingClass::NegativeInfinity;

    if (IsIA64NaNLike(numeratorClass) || IsIA64NaNLike(denominatorClass) ||
        (numeratorInfinity && denominatorInfinity) ||
        (numeratorZero && denominatorZero)) {
        WriteIA64NaN(resultBytes);
        cpu.SetPR(predicate2, false);
    } else if (numeratorInfinity || denominatorZero) {
        WriteIA64Infinity(resultBytes, first.negative != second.negative);
        cpu.SetPR(predicate2, false);
    } else if (denominatorInfinity || numeratorZero) {
        WriteIA64Zero(resultBytes, first.negative != second.negative);
        cpu.SetPR(predicate2, false);
    } else if (numeratorFinite && denominatorFinite) {
        WriteIA64Approximation(resultBytes,
                               1.0L / std::fabs(IA64ToLongDouble(second)),
                               second.negative);
        cpu.SetPR(predicate2, true);
    } else {
        WriteIA64NaN(resultBytes);
        cpu.SetPR(predicate2, false);
    }
    cpu.SetFR(destination, resultBytes);
}

static void ExecuteUnsignedFixedTruncate(CPUState& cpu,
                                          uint8_t destination,
                                          uint8_t source) {
    uint8_t sourceBytes[16] = {};
    uint8_t resultBytes[16] = {};
    cpu.GetFR(source, sourceBytes);
    const IA64FloatingValue value = ReadIA64FloatingValue(sourceBytes);

    if (value.natVal) {
        WriteNatVal(resultBytes);
        cpu.SetFR(destination, resultBytes);
        return;
    }

    // The ELILO helper converts a positive, finite quotient into IA-64's
    // integer format before feeding it to XMA.L.  For a normal register-format
    // value, changing the exponent from E to the integer-format exponent
    // 0x1003E is an exact power-of-two shift of the significand.
    if (value.negative ||
        !IsIA64Finite(ClassifyIA64FloatingValue(value))) {
        cpu.SetFR(destination, sourceBytes);
        return;
    }

    constexpr int64_t kIntegerExponent = 0x1003E;
    const int64_t shift = static_cast<int64_t>(value.exponent) -
                          kIntegerExponent;
    uint64_t integerValue = 0;
    if (shift >= 64) {
        integerValue = 0;
    } else if (shift >= 0) {
        integerValue = value.significand << static_cast<unsigned>(shift);
    } else if (shift > -64) {
        integerValue = value.significand >> static_cast<unsigned>(-shift);
    }

    WriteLittleEndian64(resultBytes, integerValue);
    WriteLittleEndian64(resultBytes + 8, kIntegerExponent);
    cpu.SetFR(destination, resultBytes);
}

static unsigned FusedArithmeticPrecisionBits(const CPUState& cpu, uint64_t rawBits) {
    const uint8_t major = static_cast<uint8_t>((rawBits >> 37) & 0x0F);
    const bool fixedSingle = ((rawBits >> 36) & 1ULL) != 0;
    if (fixedSingle) {
        return 24;
    }
    if (major == 0x9 || major == 0xB || major == 0xD) {
        return 53;
    }

    const uint8_t sf = static_cast<uint8_t>((rawBits >> 34) & 0x03);
    const uint64_t controls = (cpu.GetAR(40) >> (sf * 7)) & 0x7FULL;
    switch ((controls >> 2) & 0x03) {
        case 0: return 24;
        case 1: return 53;
        case 2: return 64;
        default: return 64;
    }
}

static bool ShouldRoundUp(const WideInteger& magnitude,
                          size_t shift,
                          uint64_t retained,
                          bool negative,
                          uint8_t roundingControl) {
    if (shift == 0) {
        return false;
    }
    const bool discarded = magnitude.AnyBelow(shift);
    switch (roundingControl & 0x03) {
        case 1: // round toward zero
            return false;
        case 2: // round toward +infinity
            return !negative && discarded;
        case 3: // round toward -infinity
            return negative && discarded;
        case 0: // round to nearest, ties to even
        default:
            if (!magnitude.TestBit(shift - 1)) {
                return false;
            }
            return magnitude.AnyBelow(shift - 1) || ((retained & 1ULL) != 0);
    }
}

static void WriteRoundedIA64Value(uint8_t* bytes,
                                  const SignedWideInteger& exact,
                                  int64_t exponentBase,
                                  unsigned precisionBits,
                                  uint8_t roundingControl) {
    if (exact.magnitude.IsZero()) {
        std::memset(bytes, 0, 16);
        return;
    }

    const int originalBitLength = exact.magnitude.BitLength();
    const size_t shift = originalBitLength > static_cast<int>(precisionBits)
        ? static_cast<size_t>(originalBitLength - static_cast<int>(precisionBits))
        : 0;
    uint64_t retained = exact.magnitude.ShiftRight(shift).Low64();
    if (ShouldRoundUp(exact.magnitude, shift, retained, exact.negative, roundingControl)) {
        ++retained;
    }

    int64_t resultBitLength = originalBitLength;
    if (precisionBits < 64 && retained == (1ULL << precisionBits)) {
        retained >>= 1;
        ++resultBitLength;
    }
    if (originalBitLength < static_cast<int>(precisionBits)) {
        retained <<= static_cast<unsigned>(precisionBits - originalBitLength);
    }
    const uint64_t significand = precisionBits == 64
        ? retained
        : (retained << (64 - precisionBits));
    int64_t exponent = static_cast<int64_t>(0x1003E) + exponentBase +
                       resultBitLength - 64;
    exponent = std::max<int64_t>(0, std::min<int64_t>(0x1FFFF, exponent));

    std::memset(bytes, 0, 16);
    WriteLittleEndian64(bytes, significand);
    WriteLittleEndian64(bytes + 8,
                        static_cast<uint64_t>(exponent) |
                        (exact.negative ? (1ULL << 17) : 0));
}

static void ExecuteFusedArithmetic(CPUState& cpu,
                                   InstructionType type,
                                   uint64_t rawBits,
                                   uint8_t destination,
                                   uint8_t multiplicand,
                                   uint8_t multiplier,
                                   uint8_t addend) {
    uint8_t multiplicandBytes[16] = {};
    uint8_t multiplierBytes[16] = {};
    uint8_t addendBytes[16] = {};
    cpu.GetFR(multiplicand, multiplicandBytes);
    cpu.GetFR(multiplier, multiplierBytes);
    if (addend != 0) {
        cpu.GetFR(addend, addendBytes);
    }

    const IA64FloatingValue left = ReadIA64FloatingValue(multiplicandBytes);
    const IA64FloatingValue right = ReadIA64FloatingValue(multiplierBytes);
    const IA64FloatingValue addendValue = ReadIA64FloatingValue(addendBytes);
    uint8_t resultBytes[16] = {};
    if (left.natVal || right.natVal || (addend != 0 && addendValue.natVal)) {
        WriteNatVal(resultBytes);
        cpu.SetFR(destination, resultBytes);
        return;
    }

    constexpr int64_t kIntegerExponent = 0x1003E;
    const int64_t productExponent =
        static_cast<int64_t>(left.exponent) - kIntegerExponent +
        static_cast<int64_t>(right.exponent) - kIntegerExponent;
    const int64_t addendExponent = addend == 0
        ? productExponent
        : static_cast<int64_t>(addendValue.exponent) - kIntegerExponent;
    const bool productNegative = (left.negative != right.negative) ==
                                 (type != InstructionType::FNMA);
    const bool addendNegative = addend != 0 &&
                                (addendValue.negative != (type == InstructionType::FMS));

    SignedWideInteger exactProduct;
    exactProduct.negative = productNegative;
    exactProduct.magnitude = WideInteger::FromUInt128(
        MultiplyUnsigned64(left.significand, right.significand));
    SignedWideInteger exact = exactProduct;
    if (addend != 0) {
        if (exactProduct.magnitude.IsZero()) {
            exact.negative = addendNegative;
            exact.magnitude = WideInteger::FromU64(addendValue.significand);
            WriteRoundedIA64Value(resultBytes,
                                  exact,
                                  addendExponent,
                                  FusedArithmeticPrecisionBits(cpu, rawBits),
                                  static_cast<uint8_t>((cpu.GetAR(40) >>
                                      (static_cast<uint8_t>((rawBits >> 34) & 0x03) * 7)) & 0x03));
            cpu.SetFR(destination, resultBytes);
            return;
        }
        if (addendValue.significand == 0) {
            WriteRoundedIA64Value(resultBytes,
                                  exactProduct,
                                  productExponent,
                                  FusedArithmeticPrecisionBits(cpu, rawBits),
                                  static_cast<uint8_t>((cpu.GetAR(40) >>
                                      (static_cast<uint8_t>((rawBits >> 34) & 0x03) * 7)) & 0x03));
            cpu.SetFR(destination, resultBytes);
            return;
        }
        // An exponent separation larger than the working width cannot affect
        // the rounded 64-bit result and cannot produce cancellation.
        if (std::llabs(productExponent - addendExponent) <= 256) {
            const int64_t commonExponent = std::min(productExponent, addendExponent);
            exactProduct.magnitude = exactProduct.magnitude.ShiftLeft(
                static_cast<size_t>(productExponent - commonExponent));
            SignedWideInteger exactAddend;
            exactAddend.negative = addendNegative;
            exactAddend.magnitude = WideInteger::FromU64(addendValue.significand).ShiftLeft(
                static_cast<size_t>(addendExponent - commonExponent));
            exact = AddSignedWide(exactProduct, exactAddend);
            WriteRoundedIA64Value(resultBytes,
                                  exact,
                                  commonExponent,
                                  FusedArithmeticPrecisionBits(cpu, rawBits),
                                  static_cast<uint8_t>((cpu.GetAR(40) >>
                                      (static_cast<uint8_t>((rawBits >> 34) & 0x03) * 7)) & 0x03));
            cpu.SetFR(destination, resultBytes);
            return;
        }
        if (productExponent < addendExponent) {
            exact = exactProduct;
        } else {
            exact.negative = addendNegative;
            exact.magnitude = WideInteger::FromU64(addendValue.significand);
        }
    }

    WriteRoundedIA64Value(resultBytes,
                          exact,
                          addend == 0 || productExponent <= addendExponent
                              ? productExponent : addendExponent,
                          FusedArithmeticPrecisionBits(cpu, rawBits),
                          static_cast<uint8_t>((cpu.GetAR(40) >>
                              (static_cast<uint8_t>((rawBits >> 34) & 0x03) * 7)) & 0x03));
    cpu.SetFR(destination, resultBytes);
}

static void ExecuteAddp4(CPUState& cpu,
                         uint8_t destination,
                         uint64_t firstOperand,
                         bool firstOperandNat,
                         uint64_t baseOperand,
                         bool baseOperandNat) {
    const uint64_t low32 = (firstOperand + baseOperand) & 0xFFFFFFFFULL;
    const uint64_t regionBits = ((baseOperand >> 30) & 0x3ULL) << 61;
    cpu.SetGR(destination, low32 | regionBits);
    cpu.SetGRNaT(destination, firstOperandNat || baseOperandNat);
}

static bool IsCompareInstruction(InstructionType type) {
    switch (type) {
        case InstructionType::CMP_EQ:
        case InstructionType::CMP_NE:
        case InstructionType::CMP_LT:
        case InstructionType::CMP_LE:
        case InstructionType::CMP_GT:
        case InstructionType::CMP_GE:
        case InstructionType::CMP_LTU:
        case InstructionType::CMP_LEU:
        case InstructionType::CMP_GTU:
        case InstructionType::CMP_GEU:
        case InstructionType::CMP4_EQ:
        case InstructionType::CMP4_NE:
        case InstructionType::CMP4_LT:
        case InstructionType::CMP4_LE:
        case InstructionType::CMP4_GT:
        case InstructionType::CMP4_GE:
        case InstructionType::CMP4_LTU:
        case InstructionType::CMP4_LEU:
        case InstructionType::CMP4_GTU:
        case InstructionType::CMP4_GEU:
            return true;
        default:
            return false;
    }
}

static const char* CompareCompleterSuffix(CompareCompleter completer) {
    switch (completer) {
        case CompareCompleter::UNC: return ".unc";
        case CompareCompleter::AND: return ".and";
        case CompareCompleter::OR: return ".or";
        case CompareCompleter::OR_ANDCM: return ".or.andcm";
        case CompareCompleter::NORMAL:
        default:
            return "";
    }
}

void InstructionEx::Execute(CPUState& cpu, IMemory& memory, bool ignorePredicate) const {
    // Check qualifying predicate
    if (!ignorePredicate && !CheckPredicate(cpu, predicate_)) {
        if (compareCompleter_ == CompareCompleter::UNC && IsCompareInstruction(type_)) {
            cpu.SetPR(dst_, false);
            cpu.SetPR(src3_, false);
        }
        if (type_ == InstructionType::FRCPA || type_ == InstructionType::FRSQRTA) {
            cpu.SetPR(predicate2_, false);
        }
        // Predicate is false, instruction is nullified
        return;
    }

    auto writeComparePredicates = [&](bool result) {
        switch (compareCompleter_) {
            case CompareCompleter::NORMAL:
            case CompareCompleter::UNC:
                cpu.SetPR(dst_, result);
                cpu.SetPR(src3_, !result);
                break;
            case CompareCompleter::AND:
                if (!result) {
                    cpu.SetPR(dst_, false);
                    cpu.SetPR(src3_, false);
                }
                break;
            case CompareCompleter::OR:
                if (result) {
                    cpu.SetPR(dst_, true);
                    cpu.SetPR(src3_, true);
                }
                break;
            case CompareCompleter::OR_ANDCM:
                if (result) {
                    cpu.SetPR(dst_, true);
                    cpu.SetPR(src3_, false);
                }
                break;
        }
    };
    
    switch (type_) {
        case InstructionType::NOP:
            // Nothing to do
            break;
            
        // ===== MOVE OPERATIONS =====
        
        case InstructionType::MOV_GR:
            // mov rDst = rSrc1
            cpu.SetGR(dst_, cpu.GetGR(src1_));
            break;

        case InstructionType::MOV_FROM_BR:
            // mov rDst = bSrc1
            cpu.SetGR(dst_, cpu.GetBR(src1_));
            break;

        case InstructionType::MOV_TO_BR:
            // mov bDst = rSrc1
            cpu.SetBR(dst_, cpu.GetGR(src1_));
            break;

        case InstructionType::MOV_FROM_AR:
            // mov rDst = arSrc1.  Keep ar.pfs tied to the CFM shadow used by alloc.
            cpu.SetGR(dst_, src1_ == 64 ? cpu.GetCFM() : cpu.GetAR(src1_));
            break;

        case InstructionType::MOV_TO_AR:
            // mov arDst = rSrc1 or mov.i arDst = imm8.
            // ar.pfs restores the previous frame marker.
            {
                const uint64_t value = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                cpu.SetAR(dst_, value);
                if (dst_ == 64) {
                    cpu.SetCFM(value);
                }
            }
            break;

        case InstructionType::GETF_SIG:
            {
                uint8_t fr[16] = {};
                uint64_t significand = 0;
                cpu.GetFR(src1_, fr);
                for (int i = 0; i < 8; ++i) {
                    significand |= static_cast<uint64_t>(fr[i]) << (i * 8);
                }
                cpu.SetGR(dst_, significand);
            }
            break;

        case InstructionType::SETF_SIG:
            {
                uint8_t fr[16] = {};
                if (cpu.GetGRNaT(src1_)) {
                    WriteNatVal(fr);
                } else {
                    WriteLittleEndian64(fr, cpu.GetGR(src1_));
                    WriteLittleEndian64(fr + 8, 0x1003EULL);
                }
                cpu.SetFR(dst_, fr);
            }
            break;

        case InstructionType::FCVT_FXU:
            {
                const uint8_t conversionOpcode =
                    static_cast<uint8_t>((rawBits_ >> 27) & 0x3F);
                if (type_ == InstructionType::FCVT_FXU &&
                    conversionOpcode == 0x1B) {
                    ExecuteUnsignedFixedTruncate(cpu, dst_, src1_);
                    break;
                }

                uint8_t fr[16] = {};
                cpu.GetFR(src1_, fr);
                cpu.SetFR(dst_, fr);
            }
            break;

        case InstructionType::FCVT_FX:
            {
                uint8_t fr[16] = {};
                cpu.GetFR(src1_, fr);
                cpu.SetFR(dst_, fr);
            }
            break;

        case InstructionType::FMA:
        case InstructionType::FMS:
        case InstructionType::FNMA:
            ExecuteFusedArithmetic(cpu, type_, rawBits_, dst_, src1_, src2_, src3_);
            break;

        case InstructionType::FRCPA:
        case InstructionType::FRSQRTA:
            ExecuteReciprocalApproximation(cpu, type_, rawBits_, dst_, src1_, src2_, predicate2_);
            break;

        case InstructionType::XMA:
        case InstructionType::XMA_H:
        case InstructionType::XMA_HU:
            {
                uint8_t fr2[16] = {};
                uint8_t fr3[16] = {};
                uint8_t fr4[16] = {};
                uint8_t resultFR[16] = {};
                cpu.GetFR(src3_, fr2); // xma operand f2 is the addend.
                cpu.GetFR(src1_, fr3);
                cpu.GetFR(src2_, fr4);

                if (IsNatVal(fr2) || IsNatVal(fr3) || IsNatVal(fr4)) {
                    WriteNatVal(resultFR);
                    cpu.SetFR(dst_, resultFR);
                    break;
                }

                const uint64_t addend = ReadLittleEndian64(fr2);
                const uint64_t left = ReadLittleEndian64(fr3);
                const uint64_t right = ReadLittleEndian64(fr4);
                const bool high = type_ != InstructionType::XMA;
                const bool unsignedHigh = type_ == InstructionType::XMA_HU;
                UInt128Parts product = unsignedHigh
                    ? MultiplyUnsigned64(left, right)
                    : MultiplySigned64(left, right);
                product = AddUInt128(product, addend);
                WriteLittleEndian64(resultFR, high ? product.hi : product.lo);
                WriteLittleEndian64(resultFR + 8, 0x1003EULL);
                cpu.SetFR(dst_, resultFR);
            }
            break;

        case InstructionType::MOV_FROM_PR:
            {
                uint64_t value = 1;
                for (uint8_t i = 1; i < 64; ++i) {
                    if (cpu.GetPR(i)) {
                        value |= (1ULL << i);
                    }
                }
                cpu.SetGR(dst_, value);
            }
            break;

        case InstructionType::MOV_FROM_IP:
            cpu.SetGR(dst_, cpu.GetIP());
            break;

        case InstructionType::MOV_TO_PR:
            {
                const uint64_t value = cpu.GetGR(src1_);
                const uint64_t mask = hasImmediate_ ? immediate_ : ~0ULL;
                for (uint8_t i = 1; i < 64; ++i) {
                    if ((mask >> i) & 0x1ULL) {
                        cpu.SetPR(i, ((value >> i) & 0x1ULL) != 0);
                    }
                }
            }
            break;

        case InstructionType::MOV_TO_PR_ROT:
            {
                const uint64_t value = immediate_;
                for (uint8_t i = 16; i < 64; ++i) {
                    cpu.SetPR(i, ((value >> i) & 0x1ULL) != 0);
                }
            }
            break;

        case InstructionType::MOV_IMM:
            // mov rDst = immediate
            if (hasImmediate_) {
                cpu.SetGR(dst_, immediate_);
            }
            break;
            
        case InstructionType::MOVL:
            // movl rDst = immediate64
            if (hasImmediate_) {
                cpu.SetGR(dst_, immediate_);
            }
            break;
            
        // ===== ARITHMETIC OPERATIONS =====
            
        case InstructionType::ADD:
            // add rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) + cpu.GetGR(src2_));
            break;
            
        case InstructionType::ADD_IMM:
            // add rDst = rSrc1, imm14
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) + immediate_);
            }
            break;

        case InstructionType::ADDL:
            // addl rDst = imm22, rSrc1
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) + immediate_);
            }
            break;
            
        case InstructionType::SUB:
            // sub rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) - cpu.GetGR(src2_));
            break;
            
        case InstructionType::SUB_IMM:
            // IA-64 A3: sub rDst = imm8, rSrc2
            if (hasImmediate_) {
                cpu.SetGR(dst_, immediate_ - cpu.GetGR(src2_));
            }
            break;
            
        case InstructionType::ADDP4:
            // Register form: addp4 rDst = rSrc1, rSrc2.
            // Immediate form: addp4 rDst = imm14, rSrc1.
            if (hasImmediate_) {
                ExecuteAddp4(cpu, dst_, immediate_, false,
                             cpu.GetGR(src1_), cpu.GetGRNaT(src1_));
            } else {
                ExecuteAddp4(cpu, dst_, cpu.GetGR(src1_), cpu.GetGRNaT(src1_),
                             cpu.GetGR(src2_), cpu.GetGRNaT(src2_));
            }
            break;
            
        // ===== BITWISE OPERATIONS =====
            
        case InstructionType::AND:
            // and rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) & cpu.GetGR(src2_));
            break;
            
        case InstructionType::AND_IMM:
            // and rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) & immediate_);
            }
            break;
            
        case InstructionType::OR:
            // or rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) | cpu.GetGR(src2_));
            break;
            
        case InstructionType::OR_IMM:
            // or rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) | immediate_);
            }
            break;
            
        case InstructionType::XOR:
            // xor rDst = rSrc1, rSrc2
            cpu.SetGR(dst_, cpu.GetGR(src1_) ^ cpu.GetGR(src2_));
            break;
            
        case InstructionType::XOR_IMM:
            // xor rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) ^ immediate_);
            }
            break;
            
        case InstructionType::ANDCM:
            // andcm rDst = rSrc1, rSrc2 (AND complement)
            cpu.SetGR(dst_, cpu.GetGR(src1_) & ~cpu.GetGR(src2_));
            break;
            
        case InstructionType::ANDCM_IMM:
            // andcm rDst = rSrc1, imm
            if (hasImmediate_) {
                cpu.SetGR(dst_, cpu.GetGR(src1_) & ~immediate_);
            }
            break;
            
        // ===== SHIFT OPERATIONS =====
            
        case InstructionType::SHL:
            // shl rDst = rSrc1, rSrc2
            {
                const uint64_t count = hasImmediate_ ? immediate_ : cpu.GetGR(src2_);
                const uint64_t result = count > 63 ? 0 : (cpu.GetGR(src1_) << count);
                cpu.SetGR(dst_, result);
                cpu.SetGRNaT(dst_, cpu.GetGRNaT(src1_) ||
                                      (!hasImmediate_ && cpu.GetGRNaT(src2_)));
            }
            break;
            
        case InstructionType::SHR:
            // shr rDst = rSrc1, rSrc2 (logical right shift)
            cpu.SetGR(dst_, cpu.GetGR(src1_) >> (cpu.GetGR(src2_) & 0x3F));
            break;
            
        case InstructionType::SHRA:
            // shra rDst = rSrc1, rSrc2 (arithmetic right shift)
            {
                int64_t val = static_cast<int64_t>(cpu.GetGR(src1_));
                int shift = static_cast<int>(cpu.GetGR(src2_) & 0x3F);
                cpu.SetGR(dst_, static_cast<uint64_t>(val >> shift));
            }
            break;

        case InstructionType::SHRP:
            // shrp rDst = rSrc1, rSrc2, count: low 64 bits of (rSrc1:rSrc2) >> count.
            if (hasImmediate_) {
                const uint8_t count = static_cast<uint8_t>(immediate_ & 0x3F);
                const uint64_t low = cpu.GetGR(src2_);
                const uint64_t high = cpu.GetGR(src1_);
                const uint64_t value = count == 0
                    ? low
                    : ((low >> count) | (high << (64 - count)));
                cpu.SetGR(dst_, value);
                cpu.SetGRNaT(dst_, cpu.GetGRNaT(src1_) || cpu.GetGRNaT(src2_));
            }
            break;
            
        case InstructionType::SHLADD:
            // shladd rDst = rSrc1, count, rSrc2
            if (hasImmediate_) {
                uint64_t shifted = cpu.GetGR(src1_) << (immediate_ & 0x3F);
                cpu.SetGR(dst_, shifted + cpu.GetGR(src2_));
            }
            break;
            
        // ===== EXTRACT/DEPOSIT OPERATIONS =====
            
        case InstructionType::EXTR:
            // extr rDst = rSrc1, pos, len
            if (hasImmediate_) {
                uint8_t pos = static_cast<uint8_t>(immediate_ & 0x3F);
                uint8_t len = static_cast<uint8_t>(((immediate_ >> 6) & 0x3F) + 1);
                uint64_t mask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1);
                uint64_t extracted = (cpu.GetGR(src1_) >> pos) & mask;
                cpu.SetGR(dst_, extracted);
            }
            break;
            
        case InstructionType::DEP:
            // dep rDst = rSrc1, rSrc2, pos, len
            if (hasImmediate_) {
                uint8_t pos = static_cast<uint8_t>(immediate_ & 0x3F);
                uint8_t len = static_cast<uint8_t>(((immediate_ >> 6) & 0x3F) + 1);
                uint64_t fieldMask = (len >= 64) ? ~0ULL : ((1ULL << len) - 1);
                uint64_t mask = fieldMask << pos;
                bool immediateSource = ((immediate_ >> 12) & 0x1) != 0;
                uint64_t dest_val = (src2_ != 0) ? cpu.GetGR(src2_) : cpu.GetGR(dst_);
                uint64_t src_val = immediateSource
                    ? ((((immediate_ >> 13) & 0x1) != 0) ? ~0ULL : 0ULL)
                    : cpu.GetGR(src1_);
                uint64_t new_val = (dest_val & ~mask) | ((src_val << pos) & mask);
                cpu.SetGR(dst_, new_val);
            }
            break;
            
        case InstructionType::ZXT1:
            cpu.SetGR(dst_, cpu.GetGR(src1_) & 0xFF);
            break;
            
        case InstructionType::ZXT2:
            cpu.SetGR(dst_, cpu.GetGR(src1_) & 0xFFFF);
            break;
            
        case InstructionType::ZXT4:
            cpu.SetGR(dst_, cpu.GetGR(src1_) & 0xFFFFFFFF);
            break;
            
        case InstructionType::SXT1:
            cpu.SetGR(dst_, static_cast<uint64_t>(SignExtend(cpu.GetGR(src1_) & 0xFF, 8)));
            break;
            
        case InstructionType::SXT2:
            cpu.SetGR(dst_, static_cast<uint64_t>(SignExtend(cpu.GetGR(src1_) & 0xFFFF, 16)));
            break;
            
        case InstructionType::SXT4:
            cpu.SetGR(dst_, static_cast<uint64_t>(SignExtend(cpu.GetGR(src1_) & 0xFFFFFFFF, 32)));
            break;

        // ===== TEST OPERATIONS =====

        case InstructionType::TBIT_Z:
        case InstructionType::TBIT_NZ:
            if (hasImmediate_) {
                const uint8_t bit = static_cast<uint8_t>((cpu.GetGR(src1_) >> (immediate_ & 0x3F)) & 0x1);
                const bool condition = (type_ == InstructionType::TBIT_Z) ? (bit == 0) : (bit != 0);
                cpu.SetPR(dst_, condition);
                cpu.SetPR(src3_, !condition);
            }
            break;

        case InstructionType::TNAT_Z:
        case InstructionType::TNAT_NZ:
            {
                const bool nat = cpu.GetGRNaT(src1_);
                const bool condition = (type_ == InstructionType::TNAT_Z) ? !nat : nat;
                cpu.SetPR(dst_, condition);
                cpu.SetPR(src3_, !condition);
            }
            break;
            
        // ===== COMPARE OPERATIONS (64-bit) =====
            
        case InstructionType::CMP_EQ:
            {
                uint64_t lhs = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                uint64_t rhs = cpu.GetGR(src2_);
                writeComparePredicates(lhs == rhs);
            }
            break;
            
        case InstructionType::CMP_NE:
            {
                uint64_t lhs = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                uint64_t rhs = cpu.GetGR(src2_);
                writeComparePredicates(lhs != rhs);
            }
            break;
            
        case InstructionType::CMP_LT:
            {
                int64_t lhs = hasImmediate_ ? static_cast<int64_t>(immediate_) : static_cast<int64_t>(cpu.GetGR(src1_));
                int64_t rhs = static_cast<int64_t>(cpu.GetGR(src2_));
                writeComparePredicates(lhs < rhs);
            }
            break;
            
        case InstructionType::CMP_LE:
            {
                int64_t lhs = hasImmediate_ ? static_cast<int64_t>(immediate_) : static_cast<int64_t>(cpu.GetGR(src1_));
                int64_t rhs = static_cast<int64_t>(cpu.GetGR(src2_));
                writeComparePredicates(lhs <= rhs);
            }
            break;
            
        case InstructionType::CMP_GT:
            {
                int64_t lhs = hasImmediate_ ? static_cast<int64_t>(immediate_) : static_cast<int64_t>(cpu.GetGR(src1_));
                int64_t rhs = static_cast<int64_t>(cpu.GetGR(src2_));
                writeComparePredicates(lhs > rhs);
            }
            break;
            
        case InstructionType::CMP_GE:
            {
                int64_t lhs = hasImmediate_ ? static_cast<int64_t>(immediate_) : static_cast<int64_t>(cpu.GetGR(src1_));
                int64_t rhs = static_cast<int64_t>(cpu.GetGR(src2_));
                writeComparePredicates(lhs >= rhs);
            }
            break;
            
        case InstructionType::CMP_LTU:
            {
                uint64_t lhs = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                uint64_t rhs = cpu.GetGR(src2_);
                writeComparePredicates(lhs < rhs);
            }
            break;
            
        case InstructionType::CMP_LEU:
            {
                uint64_t lhs = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                uint64_t rhs = cpu.GetGR(src2_);
                writeComparePredicates(lhs <= rhs);
            }
            break;
            
        case InstructionType::CMP_GTU:
            {
                uint64_t lhs = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                uint64_t rhs = cpu.GetGR(src2_);
                writeComparePredicates(lhs > rhs);
            }
            break;
            
        case InstructionType::CMP_GEU:
            {
                uint64_t lhs = hasImmediate_ ? immediate_ : cpu.GetGR(src1_);
                uint64_t rhs = cpu.GetGR(src2_);
                writeComparePredicates(lhs >= rhs);
            }
            break;
            
        // ===== COMPARE OPERATIONS (32-bit) =====
            
        case InstructionType::CMP4_EQ:
            {
                uint32_t val1 = static_cast<uint32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 == val2);
            }
            break;
            
        case InstructionType::CMP4_NE:
            {
                uint32_t val1 = static_cast<uint32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 != val2);
            }
            break;
            
        case InstructionType::CMP4_LT:
            {
                int32_t val1 = static_cast<int32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 < val2);
            }
            break;
            
        case InstructionType::CMP4_LE:
            {
                int32_t val1 = static_cast<int32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 <= val2);
            }
            break;
            
        case InstructionType::CMP4_GT:
            {
                int32_t val1 = static_cast<int32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 > val2);
            }
            break;
            
        case InstructionType::CMP4_GE:
            {
                int32_t val1 = static_cast<int32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                int32_t val2 = static_cast<int32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 >= val2);
            }
            break;
            
        case InstructionType::CMP4_LTU:
            {
                uint32_t val1 = static_cast<uint32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 < val2);
            }
            break;
            
        case InstructionType::CMP4_LEU:
            {
                uint32_t val1 = static_cast<uint32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 <= val2);
            }
            break;
            
        case InstructionType::CMP4_GTU:
            {
                uint32_t val1 = static_cast<uint32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 > val2);
            }
            break;
            
        case InstructionType::CMP4_GEU:
            {
                uint32_t val1 = static_cast<uint32_t>(hasImmediate_ ? immediate_ : cpu.GetGR(src1_));
                uint32_t val2 = static_cast<uint32_t>(cpu.GetGR(src2_));
                writeComparePredicates(val1 >= val2);
            }
            break;
            
        // ===== MEMORY OPERATIONS =====
            
        case InstructionType::LD1:
        case InstructionType::LD1_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint8_t value = 0;
                memory.Read(addr, &value, 1);
                cpu.SetGR(dst_, static_cast<uint64_t>(value));
                if (hasImmediate_) {
                    cpu.SetGR(src1_, addr + static_cast<int64_t>(immediate_));
                } else if (src2_ != 0) {
                    cpu.SetGR(src1_, addr + cpu.GetGR(src2_));
                }
            }
            break;
            
        case InstructionType::LD2:
        case InstructionType::LD2_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint16_t value = 0;
                memory.Read(addr, reinterpret_cast<uint8_t*>(&value), 2);
                cpu.SetGR(dst_, static_cast<uint64_t>(value));
                if (hasImmediate_) {
                    cpu.SetGR(src1_, addr + static_cast<int64_t>(immediate_));
                } else if (src2_ != 0) {
                    cpu.SetGR(src1_, addr + cpu.GetGR(src2_));
                }
            }
            break;
            
        case InstructionType::LD4:
        case InstructionType::LD4_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint32_t value = 0;
                memory.Read(addr, reinterpret_cast<uint8_t*>(&value), 4);
                cpu.SetGR(dst_, static_cast<uint64_t>(value));
                if (hasImmediate_) {
                    cpu.SetGR(src1_, addr + static_cast<int64_t>(immediate_));
                } else if (src2_ != 0) {
                    cpu.SetGR(src1_, addr + cpu.GetGR(src2_));
                }
            }
            break;
            
        case InstructionType::LD8:
        case InstructionType::LD8_S:
            {
                uint64_t addr = cpu.GetGR(src1_);
                uint64_t value = 0;
                memory.Read(addr, reinterpret_cast<uint8_t*>(&value), 8);
                cpu.SetGR(dst_, value);
                if (hasImmediate_) {
                    cpu.SetGR(src1_, addr + static_cast<int64_t>(immediate_));
                } else if (src2_ != 0) {
                    cpu.SetGR(src1_, addr + cpu.GetGR(src2_));
                }
            }
            break;
            
        case InstructionType::ST1:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint8_t value = static_cast<uint8_t>(cpu.GetGR(src1_));
                memory.Write(addr, &value, 1);
                if (hasImmediate_) {
                    cpu.SetGR(dst_, addr + static_cast<int64_t>(immediate_));
                }
            }
            break;
            
        case InstructionType::ST2:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint16_t value = static_cast<uint16_t>(cpu.GetGR(src1_));
                memory.Write(addr, reinterpret_cast<const uint8_t*>(&value), 2);
                if (hasImmediate_) {
                    cpu.SetGR(dst_, addr + static_cast<int64_t>(immediate_));
                }
            }
            break;
            
        case InstructionType::ST4:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint32_t value = static_cast<uint32_t>(cpu.GetGR(src1_));
                memory.Write(addr, reinterpret_cast<const uint8_t*>(&value), 4);
                if (hasImmediate_) {
                    cpu.SetGR(dst_, addr + static_cast<int64_t>(immediate_));
                }
            }
            break;
            
        case InstructionType::ST8:
            {
                uint64_t addr = cpu.GetGR(dst_);
                uint64_t value = cpu.GetGR(src1_);
                memory.Write(addr, reinterpret_cast<const uint8_t*>(&value), 8);
                if (hasImmediate_) {
                    cpu.SetGR(dst_, addr + static_cast<int64_t>(immediate_));
                }
            }
            break;

        case InstructionType::CHK_A_NC:
        case InstructionType::CHK_A_CLR:
            // ALAT tracking is not modeled yet. Treat the advanced-load check as
            // satisfied so userland can continue past compiler-generated checks.
            break;
            
        // ===== BRANCH OPERATIONS =====
            
        case InstructionType::BR_COND:
        case InstructionType::BR_CALL:
            // br.call saves return address
            if (type_ == InstructionType::BR_CALL) {
                cpu.SetBR(dst_, cpu.GetIP() + 16);
            }
            break;
            
        case InstructionType::BR_RET:
            break;

        case InstructionType::BR_CLOOP:
            if (cpu.GetAR(65) != 0) {
                cpu.SetAR(65, cpu.GetAR(65) - 1);
            }
            break;
            
        // ===== REGISTER STACK OPERATIONS =====
            
        case InstructionType::ALLOC:
            // alloc rDst = ar.pfs, sof, sol, sor
            if (hasImmediate_) {
                const uint8_t sof = static_cast<uint8_t>(immediate_ & 0x7F);
                const uint8_t sol = static_cast<uint8_t>((immediate_ >> 7) & 0x7F);
                const uint8_t sor = static_cast<uint8_t>((immediate_ >> 14) & 0xF);
                if (sol > sof || sof > 96 || sor > 4) {
                    throw std::out_of_range("ALLOC frame size invalid");
                }

                const uint64_t old_cfm = cpu.GetCFM();
                const uint64_t old_pfs = cpu.GetAR(64);
                cpu.SetGR(dst_, old_pfs == 0 ? old_cfm : old_pfs);
                cpu.SetAR(64, old_cfm);
                cpu.SetCFM(sof | (static_cast<uint64_t>(sol) << 7) |
                           (static_cast<uint64_t>(sor) << 14));
                std::cout << "[IA64-RSE] alloc ip=0x" << std::hex << cpu.GetIP()
                          << " dst=r" << std::dec << static_cast<int>(dst_)
                          << " oldCFM=0x" << std::hex << old_cfm
                          << " oldPFS=0x" << old_pfs
                          << " sof=" << std::dec << static_cast<unsigned>(sof)
                          << " sol=" << static_cast<unsigned>(sol)
                          << " sor=" << static_cast<unsigned>(sor)
                          << " newCFM=0x" << std::hex << cpu.GetCFM()
                          << std::dec << std::endl;
            }
            break;
        
        case InstructionType::BREAK:
            break;
            
        default:
            break;
    }
}

std::string InstructionEx::GetDisassembly() const {
    std::ostringstream oss;
    auto compareMnemonic = [](InstructionType type) -> const char* {
        switch (type) {
            case InstructionType::CMP_EQ: return "cmp.eq";
            case InstructionType::CMP_NE: return "cmp.ne";
            case InstructionType::CMP_LT: return "cmp.lt";
            case InstructionType::CMP_LE: return "cmp.le";
            case InstructionType::CMP_GT: return "cmp.gt";
            case InstructionType::CMP_GE: return "cmp.ge";
            case InstructionType::CMP_LTU: return "cmp.ltu";
            case InstructionType::CMP_LEU: return "cmp.leu";
            case InstructionType::CMP_GTU: return "cmp.gtu";
            case InstructionType::CMP_GEU: return "cmp.geu";
            case InstructionType::CMP4_EQ: return "cmp4.eq";
            case InstructionType::CMP4_NE: return "cmp4.ne";
            case InstructionType::CMP4_LT: return "cmp4.lt";
            case InstructionType::CMP4_LE: return "cmp4.le";
            case InstructionType::CMP4_GT: return "cmp4.gt";
            case InstructionType::CMP4_GE: return "cmp4.ge";
            case InstructionType::CMP4_LTU: return "cmp4.ltu";
            case InstructionType::CMP4_LEU: return "cmp4.leu";
            case InstructionType::CMP4_GTU: return "cmp4.gtu";
            case InstructionType::CMP4_GEU: return "cmp4.geu";
            default: return "cmp";
        }
    };
    auto compareUsesSignedImmediate = [](InstructionType type) -> bool {
        switch (type) {
            case InstructionType::CMP_LT:
            case InstructionType::CMP_LE:
            case InstructionType::CMP_GT:
            case InstructionType::CMP_GE:
            case InstructionType::CMP4_LT:
            case InstructionType::CMP4_LE:
            case InstructionType::CMP4_GT:
            case InstructionType::CMP4_GE:
                return true;
            default:
                return false;
        }
    };
    auto renderCompare = [&](const char* mnemonic, bool signedImmediate) {
        oss << mnemonic << CompareCompleterSuffix(compareCompleter_)
            << " p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
            << " = ";
        if (hasImmediate_) {
            if (signedImmediate) {
                oss << static_cast<int64_t>(immediate_);
            } else {
                oss << immediate_;
            }
        } else {
            oss << "r" << static_cast<int>(src1_);
        }
        oss << ", r" << static_cast<int>(src2_);
    };
    
    switch (type_) {
        case InstructionType::NOP:
            oss << "nop";
            break;
            
        case InstructionType::MOV_GR:
            oss << "mov r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::MOV_FROM_BR:
            oss << "mov r" << static_cast<int>(dst_) << " = b" << static_cast<int>(src1_);
            break;

        case InstructionType::MOV_TO_BR:
            oss << "mov b" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::MOV_FROM_AR:
            oss << "mov r" << static_cast<int>(dst_) << " = ar.";
            if (src1_ == 64) {
                oss << "pfs";
            } else if (src1_ == 65) {
                oss << "lc";
            } else {
                oss << static_cast<int>(src1_);
            }
            break;

        case InstructionType::MOV_TO_AR:
            oss << (hasImmediate_ ? "mov.i ar." : "mov ar.");
            if (dst_ == 64) {
                oss << "pfs";
            } else if (dst_ == 65) {
                oss << "lc";
            } else {
                oss << static_cast<int>(dst_);
            }
            if (hasImmediate_) {
                oss << " = " << static_cast<int64_t>(immediate_);
            } else {
                oss << " = r" << static_cast<int>(src1_);
            }
            break;

        case InstructionType::MOV_FROM_PR:
            oss << "mov r" << static_cast<int>(dst_) << " = pr";
            break;

        case InstructionType::MOV_FROM_IP:
            oss << "mov r" << static_cast<int>(dst_) << " = ip";
            break;

        case InstructionType::MOV_TO_PR:
            oss << "mov pr = r" << static_cast<int>(src1_);
            if (hasImmediate_) {
                oss << ", 0x" << std::hex << immediate_ << std::dec;
            }
            break;

        case InstructionType::MOV_TO_PR_ROT:
            oss << "mov pr.rot = 0x" << std::hex << immediate_ << std::dec;
            break;

        case InstructionType::GETF_SIG:
            oss << "getf.sig r" << static_cast<int>(dst_) << " = f" << static_cast<int>(src1_);
            break;

        case InstructionType::SETF_SIG:
            oss << "setf.sig f" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::FMA:
        case InstructionType::FMS:
        case InstructionType::FNMA:
            {
                const uint8_t major = static_cast<uint8_t>((rawBits_ >> 37) & 0x0F);
                const bool fixedSingle = ((rawBits_ >> 36) & 1ULL) != 0;
                const char* mnemonic = type_ == InstructionType::FMA
                    ? "fma" : (type_ == InstructionType::FMS ? "fms" : "fnma");
                oss << mnemonic;
                if (fixedSingle && (major == 0x8 || major == 0xA || major == 0xC)) {
                    oss << ".s";
                } else if (!fixedSingle && (major == 0x9 || major == 0xB || major == 0xD)) {
                    oss << ".d";
                }
                oss << ".s" << static_cast<int>((rawBits_ >> 34) & 0x03)
                    << " f" << static_cast<int>(dst_) << " = f"
                    << static_cast<int>(src1_) << ", f"
                    << static_cast<int>(src2_) << ", f"
                    << static_cast<int>(src3_);
            }
            break;

        case InstructionType::FRCPA:
            oss << "frcpa.s" << static_cast<int>((rawBits_ >> 34) & 0x03)
                << " f" << static_cast<int>(dst_)
                << ", p" << static_cast<int>(predicate2_)
                << " = f" << static_cast<int>(src1_)
                << ", f" << static_cast<int>(src2_);
            break;

        case InstructionType::FRSQRTA:
            oss << "frsqrta.s" << static_cast<int>((rawBits_ >> 34) & 0x03)
                << " f" << static_cast<int>(dst_)
                << ", p" << static_cast<int>(predicate2_)
                << " = f" << static_cast<int>(src1_);
            break;

        case InstructionType::FCVT_FX:
            oss << "fcvt.fx";
            if (((rawBits_ >> 27) & 0x3F) == 0x1A) {
                oss << ".trunc";
            }
            if (rawBits_ != 0) {
                oss << ".s" << static_cast<int>((rawBits_ >> 34) & 0x03);
            }
            oss << " f" << static_cast<int>(dst_) << " = f" << static_cast<int>(src1_);
            break;

        case InstructionType::FCVT_FXU:
            oss << "fcvt.fxu";
            if (((rawBits_ >> 27) & 0x3F) == 0x1B) {
                oss << ".trunc";
            }
            if (rawBits_ != 0) {
                oss << ".s" << static_cast<int>((rawBits_ >> 34) & 0x03);
            }
            oss << " f" << static_cast<int>(dst_) << " = f" << static_cast<int>(src1_);
            break;

        case InstructionType::FCVT_XF:
            oss << "fcvt.xf f" << static_cast<int>(dst_) << " = f" << static_cast<int>(src1_);
            break;

        case InstructionType::FCVT_XUF:
            oss << "fcvt.xuf f" << static_cast<int>(dst_) << " = f" << static_cast<int>(src1_);
            break;

        case InstructionType::XMA:
            oss << "xma.l f" << static_cast<int>(dst_) << " = f"
                << static_cast<int>(src1_) << ", f" << static_cast<int>(src2_)
                << ", f" << static_cast<int>(src3_);
            break;

        case InstructionType::XMA_H:
            oss << "xma.h f" << static_cast<int>(dst_) << " = f"
                << static_cast<int>(src1_) << ", f" << static_cast<int>(src2_)
                << ", f" << static_cast<int>(src3_);
            break;

        case InstructionType::XMA_HU:
            oss << "xma.hu f" << static_cast<int>(dst_) << " = f"
                << static_cast<int>(src1_) << ", f" << static_cast<int>(src2_)
                << ", f" << static_cast<int>(src3_);
            break;
            
        case InstructionType::MOV_IMM:
            oss << "mov r" << static_cast<int>(dst_) << " = 0x" 
                << std::hex << immediate_ << std::dec;
            break;
            
        case InstructionType::ADD:
            oss << "add r" << static_cast<int>(dst_) << " = r" 
                << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::SUB:
            oss << "sub r" << static_cast<int>(dst_) << " = r" 
                << static_cast<int>(src1_) << ", r" << static_cast<int>(src2_);
            break;
            
        case InstructionType::ADD_IMM:
            oss << "add r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", " << static_cast<int64_t>(immediate_);
            break;

        case InstructionType::ADDL:
            oss << "addl r" << static_cast<int>(dst_) << " = ";
            if (static_cast<int64_t>(immediate_) < 0) {
                oss << static_cast<int64_t>(immediate_);
            } else {
                oss << "0x" << std::hex << immediate_ << std::dec;
            }
            oss << ", r" << static_cast<int>(src1_);
            break;

        case InstructionType::ADDP4:
            oss << "addp4 r" << static_cast<int>(dst_) << " = ";
            if (hasImmediate_) {
                oss << static_cast<int64_t>(immediate_) << ", r"
                    << static_cast<int>(src1_);
            } else {
                oss << "r" << static_cast<int>(src1_) << ", r"
                    << static_cast<int>(src2_);
            }
            break;
            
        case InstructionType::SUB_IMM:
            oss << "sub r" << static_cast<int>(dst_) << " = "
                << static_cast<int64_t>(immediate_) << ", r"
                << static_cast<int>(src2_);
            break;
            
        case InstructionType::MOVL:
            oss << "movl r" << static_cast<int>(dst_) << " = 0x" << std::hex << immediate_ << std::dec;
            break;
            
        case InstructionType::AND:
            oss << "and r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;

        case InstructionType::AND_IMM:
            oss << "and r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", " << static_cast<int64_t>(immediate_);
            break;
            
        case InstructionType::OR:
            oss << "or r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;

        case InstructionType::OR_IMM:
            oss << "or r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", " << static_cast<int64_t>(immediate_);
            break;
            
        case InstructionType::XOR:
            oss << "xor r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;

        case InstructionType::XOR_IMM:
            oss << "xor r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", " << static_cast<int64_t>(immediate_);
            break;

        case InstructionType::ANDCM:
            oss << "andcm r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", r" << static_cast<int>(src2_);
            break;

        case InstructionType::ANDCM_IMM:
            oss << "andcm r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", " << static_cast<int64_t>(immediate_);
            break;
            
        case InstructionType::SHL:
            oss << "shl r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", ";
            if (hasImmediate_) {
                oss << static_cast<int64_t>(immediate_);
            } else {
                oss << "r" << static_cast<int>(src2_);
            }
            break;
            
        case InstructionType::SHR:
            oss << "shr r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_) 
                << ", r" << static_cast<int>(src2_);
            break;

        case InstructionType::SHRP:
            oss << "shrp r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", r" << static_cast<int>(src2_)
                << ", " << static_cast<int64_t>(immediate_);
            break;

        case InstructionType::SHLADD:
            oss << "shladd r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                << ", " << static_cast<int64_t>(immediate_)
                << ", r" << static_cast<int>(src2_);
            break;

        case InstructionType::EXTR:
            if (hasImmediate_) {
                uint8_t pos = static_cast<uint8_t>(immediate_ & 0x3F);
                uint8_t len = static_cast<uint8_t>(((immediate_ >> 6) & 0x3F) + 1);
                oss << "extr r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_)
                    << ", " << static_cast<int>(pos) << ", " << static_cast<int>(len);
            } else {
                oss << "extr r" << static_cast<int>(dst_);
            }
            break;

        case InstructionType::DEP:
            if (hasImmediate_) {
                uint8_t pos = static_cast<uint8_t>(immediate_ & 0x3F);
                uint8_t len = static_cast<uint8_t>(((immediate_ >> 6) & 0x3F) + 1);
                bool immediateSource = ((immediate_ >> 12) & 0x1) != 0;
                oss << "dep r" << static_cast<int>(dst_) << " = ";
                if (immediateSource) {
                    oss << static_cast<int>((immediate_ >> 13) & 0x1);
                } else {
                    oss << "r" << static_cast<int>(src1_);
                }
                if (src2_ != 0) {
                    oss << ", r" << static_cast<int>(src2_);
                }
                oss << ", " << static_cast<int>(pos) << ", " << static_cast<int>(len);
            } else {
                oss << "dep r" << static_cast<int>(dst_);
            }
            break;

        case InstructionType::ZXT1:
            oss << "zxt1 r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::ZXT2:
            oss << "zxt2 r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::ZXT4:
            oss << "zxt4 r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::SXT1:
            oss << "sxt1 r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::SXT2:
            oss << "sxt2 r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::SXT4:
            oss << "sxt4 r" << static_cast<int>(dst_) << " = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::CMP_EQ:
        case InstructionType::CMP_NE:
        case InstructionType::CMP_LT:
        case InstructionType::CMP_LE:
        case InstructionType::CMP_GT:
        case InstructionType::CMP_GE:
        case InstructionType::CMP_LTU:
        case InstructionType::CMP_LEU:
        case InstructionType::CMP_GTU:
        case InstructionType::CMP_GEU:
        case InstructionType::CMP4_EQ:
        case InstructionType::CMP4_NE:
        case InstructionType::CMP4_LT:
        case InstructionType::CMP4_LE:
        case InstructionType::CMP4_GT:
        case InstructionType::CMP4_GE:
        case InstructionType::CMP4_LTU:
        case InstructionType::CMP4_LEU:
        case InstructionType::CMP4_GTU:
        case InstructionType::CMP4_GEU:
            renderCompare(compareMnemonic(type_), compareUsesSignedImmediate(type_));
            break;

        case InstructionType::TBIT_Z:
            oss << "tbit.z p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_) << ", " << static_cast<int>(immediate_ & 0x3F);
            break;

        case InstructionType::TBIT_NZ:
            oss << "tbit.nz p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_) << ", " << static_cast<int>(immediate_ & 0x3F);
            break;

        case InstructionType::TNAT_Z:
            oss << "tnat.z p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_);
            break;

        case InstructionType::TNAT_NZ:
            oss << "tnat.nz p" << static_cast<int>(dst_) << ", p" << static_cast<int>(src3_)
                << " = r" << static_cast<int>(src1_);
            break;
            
        case InstructionType::LD1:
        case InstructionType::LD1_S:
            oss << "ld1 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            } else if (src2_ != 0) {
                oss << ", r" << static_cast<int>(src2_);
            }
            break;
            
        case InstructionType::LD2:
        case InstructionType::LD2_S:
            oss << "ld2 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            } else if (src2_ != 0) {
                oss << ", r" << static_cast<int>(src2_);
            }
            break;
            
        case InstructionType::LD4:
        case InstructionType::LD4_S:
            oss << "ld4 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            } else if (src2_ != 0) {
                oss << ", r" << static_cast<int>(src2_);
            }
            break;
            
        case InstructionType::LD8:
        case InstructionType::LD8_S:
            oss << "ld8 r" << static_cast<int>(dst_) << " = [r" << static_cast<int>(src1_) << "]";
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            } else if (src2_ != 0) {
                oss << ", r" << static_cast<int>(src2_);
            }
            break;

        case InstructionType::CHK_A_NC:
            oss << "chk.a.nc r" << static_cast<int>(dst_);
            if (hasBranchTarget_) {
                oss << ", 0x" << std::hex << branchTarget_ << std::dec;
            }
            break;

        case InstructionType::CHK_A_CLR:
            oss << "chk.a.clr r" << static_cast<int>(dst_);
            if (hasBranchTarget_) {
                oss << ", 0x" << std::hex << branchTarget_ << std::dec;
            }
            break;
            
        case InstructionType::ST1:
            oss << "st1 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            }
            break;
            
        case InstructionType::ST2:
            oss << "st2 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            }
            break;
            
        case InstructionType::ST4:
            oss << "st4 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            }
            break;
            
        case InstructionType::ST8:
            oss << "st8 [r" << static_cast<int>(dst_) << "] = r" << static_cast<int>(src1_);
            if (hasImmediate_) {
                oss << ", " << static_cast<int64_t>(immediate_);
            }
            break;
            
        case InstructionType::BR_COND:
            if (hasBranchTarget_) {
                oss << "br.cond 0x" << std::hex << branchTarget_ << std::dec;
            } else {
                oss << "br.cond b" << static_cast<int>(src1_);
            }
            break;
            
        case InstructionType::BR_CALL:
            oss << "br.call b" << static_cast<int>(dst_);
            if (hasBranchTarget_) {
                oss << " = 0x" << std::hex << branchTarget_ << std::dec;
            } else {
                oss << " = b" << static_cast<int>(src1_);
            }
            break;
            
        case InstructionType::BR_RET:
            oss << "br.ret b" << static_cast<int>(src1_);
            break;

        case InstructionType::BR_CLOOP:
            if (hasBranchTarget_) {
                oss << "br.cloop 0x" << std::hex << branchTarget_ << std::dec;
            } else {
                oss << "br.cloop";
            }
            break;
            
        case InstructionType::ALLOC:
            if (hasImmediate_) {
                uint8_t sof = static_cast<uint8_t>(immediate_ & 0x7F);
                uint8_t sol = static_cast<uint8_t>((immediate_ >> 7) & 0x7F);
                uint8_t sor = static_cast<uint8_t>((immediate_ >> 14) & 0xF);
                oss << "alloc r" << static_cast<int>(dst_) << " = ar.pfs, " 
                    << static_cast<int>(sof) << ", " << static_cast<int>(sol) 
                    << ", " << static_cast<int>(sor);
            } else {
                oss << "alloc r" << static_cast<int>(dst_);
            }
            break;
        
        case InstructionType::BREAK:
            oss << "break 0x" << std::hex << immediate_ << std::dec;
            break;
            
            
        default:
            oss << "unknown (0x" << std::hex << rawBits_ << std::dec << ")";
            break;
    }
    
    return oss.str();
}

// InstructionDecoder implementation
InstructionDecoder::InstructionDecoder() {
}

// New simplified decoder API
InstructionBundle InstructionDecoder::DecodeBundleNew(const uint8_t* bundleData) const {
    InstructionBundle bundle;
    
    // Extract template field (bits 0-4)
    // Endian-safe: read first byte and mask lower 5 bits
    bundle.template_field = bundleData[0] & 0x1F;
    
    // Extract slot 0 (bits 5-45): 41 bits starting at bit 5
    // NOTE: For little-endian host (Windows), bytes are already in correct order
    // For portability to big-endian systems, add byte-swapping here
    uint64_t slot0_raw = ExtractSlot(bundleData, 0);
    bundle.slot0 = DecodeInstructionSimple(slot0_raw);
    
    // Extract slot 1 (bits 46-86): 41 bits starting at bit 46
    uint64_t slot1_raw = ExtractSlot(bundleData, 1);
    bundle.slot1 = DecodeInstructionSimple(slot1_raw);
    
    // Extract slot 2 (bits 87-127): 41 bits starting at bit 87
    uint64_t slot2_raw = ExtractSlot(bundleData, 2);
    bundle.slot2 = DecodeInstructionSimple(slot2_raw);
    
    return bundle;
}

// Simplified instruction decoder (placeholder)
// TODO: Implement real IA-64 instruction format decoding
// Real IA-64 has multiple instruction formats:
// - A-type: Integer ALU (add, sub, and, or, etc.)
// - I-type: Non-ALU integer (shifts, multimedia)
// - M-type: Memory operations (load, store)
// - F-type: Floating-point
// - B-type: Branch
// - L+X: Long immediate (uses 2 slots)
// Each format has different field layouts for opcode, predicate, registers
Instruction InstructionDecoder::DecodeInstructionSimple(uint64_t rawBits) const {
    Instruction insn;
    
    // Placeholder decoding:
    // Bit layout (simplified, not actual IA-64 format):
    // [40:37] = opcode (4 bits)
    // [36:31] = predicate (6 bits)
    // [30:0]  = operands (split into two fields)
    
    // NOTE: Real IA-64 instruction formats are far more complex:
    // - Predicate is typically in bits [0:5]
    // - Major opcode position varies by instruction format
    // - Register fields are 7 bits each (128 registers)
    // - Immediate fields have format-specific encoding
    
    // Extract fields (placeholder logic)
    insn.opcode = static_cast<uint8_t>((rawBits >> 37) & 0x0F);  // Top 4 bits
    insn.predicate = static_cast<uint8_t>((rawBits >> 31) & 0x3F);  // Next 6 bits
    insn.operand1 = (rawBits >> 16) & 0x7FFF;  // Bits 16-30 (15 bits)
    insn.operand2 = rawBits & 0xFFFF;  // Bits 0-15 (16 bits)
    
    // TODO: Implement proper instruction format detection:
    // 1. Identify instruction format (A, I, M, F, B, L, X)
    // 2. Extract opcode based on format
    // 3. Extract predicate (usually bits 0-5)
    // 4. Extract source/dest registers (7 bits each)
    // 5. Extract immediates (format-specific)
    // 6. Handle special cases (nop, break, hints)
    
    return insn;
}

// Legacy decoder API
Bundle InstructionDecoder::DecodeBundle(const uint8_t* bundleData) const {
    return DecodeBundleAt(bundleData, 0);
}

Bundle InstructionDecoder::DecodeBundleAt(const uint8_t* bundleData, uint64_t bundleIP) const {
    Bundle bundle;
    
    // Extract template (first 5 bits)
    bundle.templateType = ExtractTemplate(bundleData);
    const uint8_t templateId = static_cast<uint8_t>(bundle.templateType);
    const auto* templateInfo = opcodes::getTemplateInfo(templateId);
    if (templateInfo) {
        bundle.stopAfterSlot[0] = templateInfo->stop_after_0;
        bundle.stopAfterSlot[1] = templateInfo->stop_after_1;
        bundle.stopAfterSlot[2] = templateInfo->stop_after_2;
        bundle.hasStop = templateInfo->stop_after_0 || templateInfo->stop_after_1 || templateInfo->stop_after_2;
    } else {
        bundle.hasStop = false;
    }

    if (IsMLXTemplate(static_cast<uint8_t>(bundle.templateType))) {
        const uint64_t slot0Bits = ExtractSlot(bundleData, 0);
        const uint64_t slot1Bits = ExtractSlot(bundleData, 1);
        const uint64_t slot2Bits = ExtractSlot(bundleData, 2);

        bundle.instructions.push_back(DecodeInstruction(slot0Bits, UnitType::M_UNIT));

        formats::LFormat lfmt;
        formats::XFormat xfmt;
        InstructionEx movl;
        if (decoder::LXDecoder::decodeL(slot1Bits, lfmt) &&
            decoder::LXDecoder::decodeX(slot2Bits, xfmt) &&
            decoder::LXDecoder::combineMOVL(lfmt, xfmt, movl)) {
            movl.SetRawBits(slot2Bits);
            bundle.instructions.push_back(movl);
        } else {
            bundle.instructions.push_back(DecodeInstruction(slot1Bits, UnitType::L_UNIT));
            bundle.instructions.push_back(DecodeInstruction(slot2Bits, UnitType::X_UNIT));
        }

        return bundle;
    }
    
    // Get unit types for this template
    auto units = GetUnitsForTemplate(bundle.templateType);
    
    // Extract and decode each instruction slot
    for (size_t i = 0; i < units.size(); ++i) {
        uint64_t slotBits = ExtractSlot(bundleData, i);
        InstructionEx insn = DecodeSlot(slotBits, units[i], bundleIP);
        bundle.instructions.push_back(insn);
    }
    
    return bundle;
}

InstructionEx InstructionDecoder::DecodeInstruction(uint64_t rawBits, UnitType unit) const {
    // Use the comprehensive DecodeSlot function with IP=0 (not needed for non-branch instructions)
    InstructionEx result = DecodeSlot(rawBits, unit, 0);
    
    // Ensure raw bits are preserved
    result.SetRawBits(rawBits);
    
    return result;
}

TemplateType InstructionDecoder::ExtractTemplate(const uint8_t* bundleData) const {
    // Template is the first 5 bits of the bundle
    uint8_t templateBits = bundleData[0] & 0x1F;
    return static_cast<TemplateType>(templateBits);
}

uint64_t InstructionDecoder::ExtractSlot(const uint8_t* bundleData, size_t slotIndex) const {
    // Each slot is 41 bits
    // Slot 0: bits 5-45
    // Slot 1: bits 46-86
    // Slot 2: bits 87-127
    
    // Convert byte array to 128-bit value (stored in two uint64_t)
    // NOTE: Assumes little-endian host (Windows)
    // For big-endian hosts, bytes would need to be swapped
    uint64_t low = 0, high = 0;
    for (int i = 0; i < 8; ++i) {
        low |= static_cast<uint64_t>(bundleData[i]) << (i * 8);
        high |= static_cast<uint64_t>(bundleData[i + 8]) << (i * 8);
    }
    
    // Extract the appropriate 41-bit slot
    // Mask: 0x1FFFFFFFFFF (41 bits set)
    uint64_t slotBits = 0;
    
    switch (slotIndex) {
        case 0:  // Bits 5-45 (41 bits)
            slotBits = (low >> 5) & 0x1FFFFFFFFFFULL;
            break;
        case 1:  // Bits 46-86 (41 bits, spans both low and high)
            slotBits = (low >> 46) | ((high & 0x7FFFFFF) << 18);
            slotBits &= 0x1FFFFFFFFFFULL;
            break;
        case 2:  // Bits 87-127 (41 bits)
            slotBits = (high >> 23) & 0x1FFFFFFFFFFULL;
            break;
    }
    
    return slotBits;
}

std::vector<UnitType> InstructionDecoder::GetUnitsForTemplate(TemplateType tmpl) const {
    // Return execution units for each template type
    // Most templates have 3 slots, MLX/MFI have 2-3
    
    switch (tmpl) {
        case TemplateType::MII:
        case TemplateType::MII_STOP:
            return { UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MI_I:
        case TemplateType::MI_I_STOP:
            return { UnitType::M_UNIT, UnitType::I_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MLX:
        case TemplateType::MLX_STOP:
            return { UnitType::M_UNIT, UnitType::L_UNIT, UnitType::X_UNIT };
            
        case TemplateType::MMI:
        case TemplateType::MMI_STOP:
        case TemplateType::M_MI:
        case TemplateType::M_MI_STOP:
            return { UnitType::M_UNIT, UnitType::M_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MFI:
        case TemplateType::MFI_STOP:
            return { UnitType::M_UNIT, UnitType::F_UNIT, UnitType::I_UNIT };
            
        case TemplateType::MMF:
        case TemplateType::MMF_STOP:
            return { UnitType::M_UNIT, UnitType::M_UNIT, UnitType::F_UNIT };
            
        case TemplateType::MIB:
        case TemplateType::MIB_STOP:
            return { UnitType::M_UNIT, UnitType::I_UNIT, UnitType::B_UNIT };
            
        case TemplateType::MBB:
        case TemplateType::MBB_STOP:
            return { UnitType::M_UNIT, UnitType::B_UNIT, UnitType::B_UNIT };
            
        case TemplateType::BBB:
        case TemplateType::BBB_STOP:
            return { UnitType::B_UNIT, UnitType::B_UNIT, UnitType::B_UNIT };
            
        case TemplateType::MMB:
        case TemplateType::MMB_STOP:
            return { UnitType::M_UNIT, UnitType::M_UNIT, UnitType::B_UNIT };
            
        case TemplateType::MFB:
        case TemplateType::MFB_STOP:
            return { UnitType::M_UNIT, UnitType::F_UNIT, UnitType::B_UNIT };
            
        default:
            // Unknown template, return 3 invalid units
            return { UnitType::INVALID, UnitType::INVALID, UnitType::INVALID };
    }
}

InstructionEx InstructionDecoder::DecodeNop(uint64_t rawBits) const {
    InstructionEx insn(InstructionType::NOP, UnitType::I_UNIT);
    return insn;
}

InstructionEx InstructionDecoder::DecodeMov(uint64_t rawBits, UnitType unit) const {
    // Simplified MOV decoding
    InstructionEx insn(InstructionType::MOV_GR, unit);
    
    // Extract operands (this is highly simplified)
    uint8_t dst = (rawBits >> 6) & 0x7F;
    uint8_t src1 = (rawBits >> 13) & 0x7F;
    
    insn.SetOperands(dst, src1);
    return insn;
}

// ============================================================================
// NEW: Binary Instruction Decoder Integration
// ============================================================================

#include "ia64_formats.h"
#include "ia64_opcodes.h"
#include "ia64_decoders.h"

uint8_t InstructionDecoder::ExtractMajorOpcode(uint64_t slotBits) const {
    // Major opcode is in bits [37:40] (4 bits)
    return static_cast<uint8_t>((slotBits >> 37) & 0x0F);
}

bool InstructionDecoder::IsMLXTemplate(uint8_t template_field) const {
    return (template_field == 0x04 || template_field == 0x05);
}

InstructionEx InstructionDecoder::DecodeSlot(uint64_t slotBits, UnitType unitType, uint64_t ip) const {
    InstructionEx result;
    
    // Extract major opcode
    uint8_t major = ExtractMajorOpcode(slotBits);
    const uint8_t x3 = static_cast<uint8_t>((slotBits >> 33) & 0x7);
    const uint8_t x6 = static_cast<uint8_t>((slotBits >> 27) & 0x3F);

    auto decodeAlloc = [&]() -> bool {
        // IA-64 alloc uses M34: major opcode 1 with x3=6.
        if (!(major == 0x1 && x3 == 0x6)) {
            return false;
        }

        const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
        const uint8_t sof = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
        const uint8_t sol = static_cast<uint8_t>((slotBits >> 20) & 0x7F);
        const uint8_t sor = static_cast<uint8_t>((slotBits >> 27) & 0x0F);
        result = InstructionEx(InstructionType::ALLOC, unitType);
        result.SetOperands(r1, 0, 0);
        result.SetImmediate(sof | (static_cast<uint64_t>(sol) << 7) |
                            (static_cast<uint64_t>(sor) << 14));
        result.SetRawBits(slotBits);
        return true;
    };
    
    // Route to appropriate decoder based on unit type and major opcode
    switch (unitType) {
        case UnitType::M_UNIT:
            if (decodeAlloc()) {
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && (x6 == 0x00 || x6 == 0x01)) {
                result = InstructionEx(InstructionType::NOP, UnitType::M_UNIT);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && (x6 == 0x2A || x6 == 0x32)) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint8_t r2 = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
                const uint8_t ar3 = static_cast<uint8_t>((slotBits >> 20) & 0x7F);
                result = InstructionEx(x6 == 0x2A ? InstructionType::MOV_TO_AR
                                                   : InstructionType::MOV_FROM_AR,
                                       UnitType::M_UNIT);
                if (x6 == 0x2A) {
                    result.SetOperands(ar3, r2, 0);
                } else {
                    result.SetOperands(r1, ar3, 0);
                }
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && (x3 == 0x4 || x3 == 0x5)) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint32_t imm20b = static_cast<uint32_t>((slotBits >> 13) & 0xFFFFF);
                const uint32_t sign = static_cast<uint32_t>((slotBits >> 36) & 0x1);
                const uint32_t imm21 = (sign << 20) | imm20b;
                const int64_t offset = SignExtend(imm21, 21) * 16;
                result = InstructionEx(x3 == 0x4 ? InstructionType::CHK_A_NC
                                                 : InstructionType::CHK_A_CLR,
                                       UnitType::M_UNIT);
                result.SetOperands(r1, 0, 0);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetBranchTarget(static_cast<uint64_t>(static_cast<int64_t>(ip) + offset));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x1 && x3 == 0x0 && (x6 == 0x2A || x6 == 0x22)) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint8_t r2 = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
                const uint8_t ar3 = static_cast<uint8_t>((slotBits >> 20) & 0x7F);
                result = InstructionEx(x6 == 0x2A ? InstructionType::MOV_TO_AR
                                                   : InstructionType::MOV_FROM_AR,
                                       UnitType::M_UNIT);
                if (x6 == 0x2A) {
                    result.SetOperands(ar3, r2, 0);
                } else {
                    result.SetOperands(r1, ar3, 0);
                }
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            // M-unit can execute M-type (memory) or A-type (ALU) instructions
            // Major opcodes for M-type: 0x4-0x7 (primary), also 0x0-0x3 for some forms
            if (major >= 0x0 && major <= 0x7) {
                // M-type: Load/Store operations
                formats::MFormat mfmt;
                if (decoder::MTypeDecoder::decode(slotBits, mfmt)) {
                    if (decoder::MTypeDecoder::toInstruction(mfmt, result)) {
                        result.SetRawBits(slotBits);
                        return result;
                    }
                }
            }
            // Try A-type for M-unit ALU operations
            // Major opcodes: 0x8-0xF can appear in M-unit
            if (major >= 0x8 && major <= 0xF) {
                formats::AFormat afmt;
                if (decoder::ATypeDecoder::decode(slotBits, afmt)) {
                    if (decoder::ATypeDecoder::toInstruction(afmt, result)) {
                        result.SetRawBits(slotBits);
                        return result;
                    }
                }
            }
            break;
            
        case UnitType::I_UNIT:
            if (decodeAlloc()) {
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && (x6 == 0x00 || x6 == 0x01)) {
                result = InstructionEx(InstructionType::NOP, UnitType::I_UNIT);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x2 && x3 == 0x0 && x6 == 0x00 &&
                ((slotBits >> 6) & 0x1FFFFF) == 0) {
                result = InstructionEx(InstructionType::NOP, UnitType::I_UNIT);
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x3 && x6 == 0x1F) {
                const uint8_t mask7a = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint8_t r2 = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
                const uint16_t mask8c = static_cast<uint16_t>((slotBits >> 24) & 0xFF);
                const uint16_t sign = static_cast<uint16_t>((slotBits >> 36) & 0x1);
                const uint16_t imm16 = static_cast<uint16_t>(mask7a | (mask8c << 7) | (sign << 15));
                const uint64_t mask17 = static_cast<uint64_t>(static_cast<int64_t>(SignExtend(static_cast<uint64_t>(imm16) << 1, 17)));

                result = InstructionEx(InstructionType::MOV_TO_PR, UnitType::I_UNIT);
                result.SetOperands(0, r2, 0);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetImmediate(mask17);
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x2 && x6 == 0x00) {
                result = InstructionEx(InstructionType::MOV_TO_PR_ROT, UnitType::I_UNIT);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetImmediate(DecodeMovPrRotImmediate(slotBits));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && x6 == 0x31) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint8_t b2 = static_cast<uint8_t>((slotBits >> 13) & 0x7);
                result = InstructionEx(InstructionType::MOV_FROM_BR, UnitType::I_UNIT);
                result.SetOperands(r1, b2, 0);
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && x6 == 0x30) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                result = InstructionEx(InstructionType::MOV_FROM_IP, UnitType::I_UNIT);
                result.SetOperands(r1, 0, 0);
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && x6 == 0x33) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                result = InstructionEx(InstructionType::MOV_FROM_PR, UnitType::I_UNIT);
                result.SetOperands(r1, 0, 0);
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x7 && x6 == 0x00) {
                const uint8_t b1 = static_cast<uint8_t>((slotBits >> 6) & 0x7);
                const uint8_t r2 = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
                result = InstructionEx(InstructionType::MOV_TO_BR, UnitType::I_UNIT);
                result.SetOperands(b1, r2, 0);
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && x6 == 0x0A) {
                // mov.i ar3 = imm8: IMM8 is split between slot bits 13-19
                // and bit 36, and is a signed two's-complement immediate.
                const uint8_t ar3 = static_cast<uint8_t>((slotBits >> 20) & 0x7F);
                const uint8_t immLow7 = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
                const uint8_t immSign = static_cast<uint8_t>((slotBits >> 36) & 0x1);
                const uint8_t imm8 = static_cast<uint8_t>(immLow7 | (immSign << 7));

                result = InstructionEx(InstructionType::MOV_TO_AR, UnitType::I_UNIT);
                result.SetOperands(ar3, 0, 0);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetImmediate(static_cast<uint64_t>(static_cast<int64_t>(
                    SignExtend(imm8, 8))));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x0 && (x6 == 0x2A || x6 == 0x32)) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint8_t r2 = static_cast<uint8_t>((slotBits >> 13) & 0x7F);
                const uint8_t ar3 = static_cast<uint8_t>((slotBits >> 20) & 0x7F);
                result = InstructionEx(x6 == 0x32 ? InstructionType::MOV_FROM_AR
                                                   : InstructionType::MOV_TO_AR,
                                       UnitType::I_UNIT);
                if (x6 == 0x32) {
                    result.SetOperands(r1, ar3, 0);
                } else {
                    result.SetOperands(ar3, r2, 0);
                }
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            if (major == 0x0 && x3 == 0x2 && x6 == 0x38) {
                const uint8_t r1 = static_cast<uint8_t>((slotBits >> 6) & 0x7F);
                const uint8_t ar3 = static_cast<uint8_t>((slotBits >> 20) & 0x7F);
                result = InstructionEx(InstructionType::MOV_FROM_AR, UnitType::I_UNIT);
                result.SetOperands(r1, ar3, 0);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            // I-unit can execute A-type (ALU) or I-type (non-ALU integer) instructions
            // Try A-type first for ALU operations (major 0x8-0xF)
            if (major >= 0x8 && major <= 0xF) {
                // A-type: Integer ALU operations
                formats::AFormat afmt;
                if (decoder::ATypeDecoder::decode(slotBits, afmt)) {
                    if (decoder::ATypeDecoder::toInstruction(afmt, result)) {
                        result.SetRawBits(slotBits);
                        return result;
                    }
                }
            }
            
            // Try I-type for non-ALU integer operations (major 0x0, 0x5, 0x7, and others)
            if (major >= 0x0 && major <= 0x7) {
                // I-type: Shifts, deposits, extends, ALLOC
                formats::IFormat ifmt;
                if (decoder::ITypeDecoder::decode(slotBits, ifmt)) {
                    if (decoder::ITypeDecoder::toInstruction(ifmt, result)) {
                        result.SetRawBits(slotBits);
                        return result;
                    }
                }
            }
            break;
            
        case UnitType::B_UNIT:
            if (major == 0x2 && x3 == 0x0 && x6 == 0x00 &&
                ((slotBits >> 6) & 0x1FFFFF) == 0) {
                result = InstructionEx(InstructionType::NOP, UnitType::B_UNIT);
                result.SetRawBits(slotBits);
                return result;
            }

            // B-unit executes branch instructions
            // Major opcodes: 0x0, 0x4, 0x5 for various branch types
            if (major >= 0x0 && major <= 0x5) {
                formats::BFormat bfmt;
                if (decoder::BTypeDecoder::decode(slotBits, bfmt, ip)) {
                    if (decoder::BTypeDecoder::toInstruction(bfmt, result)) {
                        result.SetRawBits(slotBits);
                        return result;
                    }
                }
            }
            break;
            
        case UnitType::F_UNIT:
            if (major == 0x0 && x3 == 0x0 && (x6 == 0x00 || x6 == 0x01)) {
                result = InstructionEx(InstructionType::NOP, UnitType::F_UNIT);
                result.SetPredicate(static_cast<uint8_t>(slotBits & 0x3F));
                result.SetRawBits(slotBits);
                return result;
            }

            // F-unit for floating-point operations
            result = decoder::FTypeDecoder::decode(slotBits);
            result.SetRawBits(slotBits);
            return result;
            
        case UnitType::L_UNIT:
            // L-unit is handled specially in DecodeBundle for MOVL
            // Standalone L-unit is rare but could be NOP
            result = InstructionEx(InstructionType::NOP, UnitType::L_UNIT);
            result.SetRawBits(slotBits);
            return result;
            
        case UnitType::X_UNIT:
            // X-unit for extended instructions and MOVL X-portion
            result = decoder::XTypeDecoder::decode(slotBits);
            result.SetRawBits(slotBits);
            return result;
            
        default:
            break;
    }
    
    // If no decoder matched, return UNKNOWN with raw bits preserved
    result = InstructionEx(InstructionType::UNKNOWN, unitType);
    result.SetRawBits(slotBits);
    return result;
}

} // namespace ia64

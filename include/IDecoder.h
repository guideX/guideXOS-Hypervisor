#pragma once

#include <cstdint>
#include <vector>

namespace ia64 {

// Forward declarations
struct InstructionBundle;
struct Bundle;
struct InstructionEx;
enum class UnitType;

/**
 * IDecoder - Abstract interface for IA-64 instruction decoding
 * 
 * This interface provides abstraction for instruction decoding, allowing:
 * - Swapping between simplified and real IA-64 decoding implementations
 * - Future integration with hardware-assisted decoding
 * - Plugin-style architecture for different decoder strategies
 * 
 * Design Philosophy:
 * - Decoders are stateless and thread-safe
 * - Bundle decoding returns structured data, not side effects
 * - Interface supports both legacy and modern bundle formats
 * 
 * Future Extensions:
 * - Hardware-assisted decoding via VT-x or similar
 * - JIT compilation hooks (decode-time analysis)
 * - Dynamic binary translation integration
 * - Instruction trace/profiling callbacks
 */
class IDecoder {
public:
    virtual ~IDecoder() = default;

    /**
     * Decode a 128-bit IA-64 instruction bundle
     * 
     * @param bundleData Pointer to 16-byte (128-bit) bundle in little-endian format
     *                   Layout: [template:5][slot0:41][slot1:41][slot2:41]
     * @return Structured InstructionBundle with template and 3 decoded slots
     * 
     * NOTE: Implementation must handle:
     * - Template field extraction (bits 0-4)
     * - Slot extraction with proper bit alignment
     * - MLX template handling (L+X slots form 64-bit immediate)
     * - Dispersed immediate forms
     */
    virtual InstructionBundle DecodeBundleNew(const uint8_t* bundleData) const = 0;

    /**
     * Legacy bundle decoding API
     * 
     * @param bundleData Pointer to 16-byte bundle
     * @return Legacy Bundle structure with InstructionEx vector
     * 
     * NOTE: This method exists for backward compatibility with existing code.
     * New implementations should prefer DecodeBundleNew().
     */
    virtual Bundle DecodeBundle(const uint8_t* bundleData) const = 0;

    /**
     * Decode a single 41-bit instruction
     * 
     * @param rawBits Raw 41-bit instruction (stored in uint64_t, upper bits ignored)
     * @param unit Execution unit type hint (M, I, F, B, L, X)
     * @return Decoded InstructionEx object
     * 
     * NOTE: The unit parameter provides context for instruction format selection,
     * as some opcodes have different meanings depending on the execution unit.
     */
    virtual InstructionEx DecodeInstruction(uint64_t rawBits, UnitType unit) const = 0;

protected:
    // Protected constructor - interface cannot be instantiated directly
    IDecoder() = default;

    // Prevent copying and moving of interface
    IDecoder(const IDecoder&) = delete;
    IDecoder& operator=(const IDecoder&) = delete;
    IDecoder(IDecoder&&) = delete;
    IDecoder& operator=(IDecoder&&) = delete;
};

} // namespace ia64

#include "core/Depth.h"

#include <bit>

namespace observex {

float halfToFloat(std::uint16_t half) {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
    std::uint32_t exponent = (half >> 10) & 0x1Fu;
    std::uint32_t mantissa = half & 0x3FFu;
    if (exponent == 0) {
        if (mantissa == 0) {
            return std::bit_cast<float>(sign); // Zero.
        }
        // Subnormal: shift it up until it has its leading 1, as a float has.
        exponent = 127 - 15 + 1;
        while ((mantissa & 0x400u) == 0) {
            mantissa <<= 1;
            --exponent;
        }
        mantissa &= 0x3FFu;
        return std::bit_cast<float>(sign | (exponent << 23) | (mantissa << 13));
    }
    if (exponent == 31) {
        return std::bit_cast<float>(sign | 0x7F800000u | (mantissa << 13)); // Infinity or not a number.
    }
    return std::bit_cast<float>(sign | ((exponent + 127 - 15) << 23) | (mantissa << 13));
}

} // namespace observex

#pragma once

#include <cstdint>
#include <vector>

// Appends the varbyte encoding of x to out
inline void vb_encode(uint32_t x, std::vector<uint8_t>& out) {
    while (x >= 128) {
        out.push_back(static_cast<uint8_t>(0x80 | (x & 0x7F)));
        x >>= 7;
    }
    out.push_back(static_cast<uint8_t>(x));
}

// Decodes the varbyte value at p and advances p 
inline uint32_t vb_decode(const uint8_t*& p) {
    uint32_t x = 0;
    int shift = 0;
    while (*p & 0x80) {
        x |= static_cast<uint32_t>(*p++ & 0x7F) << shift;
        shift += 7;
    }
    return x | (static_cast<uint32_t>(*p++) << shift);
}

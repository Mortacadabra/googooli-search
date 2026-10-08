#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// pretty formatting 1234567 -> "1,234,567"
inline std::string commas(unsigned long long n) {
    std::string s = std::to_string(n), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (s.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(s[i]);
    }
    return out;
}

// Seconds on a monotonic clock
inline double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Appends the decimal digits of v to out (fast tostring)
inline void append_uint(std::string& out, uint64_t v) {
    char tmp[24];
    int n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n) out.push_back(tmp[--n]);
}

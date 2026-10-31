#pragma once

#include <algorithm>
#include <string_view>

namespace observex {

// ASCII-only lowercase. Leaves other bytes (including UTF-8) untouched and never depends on the locale.
constexpr char toLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return toLowerAscii(x) == toLowerAscii(y); });
}

// True when `needle` appears in `haystack`, ignoring ASCII case. An empty needle is found everywhere.
inline bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    const auto found = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                   [](char x, char y) { return toLowerAscii(x) == toLowerAscii(y); });
    return found != haystack.end();
}

} // namespace observex

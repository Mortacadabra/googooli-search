// indexer/tokenizer.h -- C++ port of tokenizer.py
//
// Python:  TOKEN_PATTERN = re.compile(r"[A-Za-z0-9]+")
//          tokenize(text) = TOKEN_PATTERN.findall(text.lower())
#pragma once

#include <string>
#include <vector>

inline std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');  // .lower()
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            cur.push_back(static_cast<char>(c));
        } else if (!cur.empty()) {
            tokens.push_back(cur);  // findall(): one token per maximal run
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

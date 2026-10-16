#pragma once

// code for snippet generation, centering on the first rarest query term and finds the first appearance in each top doc

#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

inline constexpr size_t SNIPPET_CHARS = 200;  // length of the snippet window
inline constexpr size_t SNIPPET_BEFORE = 50;  // chars kept before the first match

struct Snippets {
    int pagetable_fd = -1;
    int collection_fd = -1;  // -1 if source collection.tsv is missing, which means snippets will be empty
    uint32_t num_docs = 0;

    void open_files(const std::string& root) {
        pagetable_fd = open((root + "/output/pagetable.bin").c_str(), O_RDONLY);
        collection_fd = open((root + "/data/collection.tsv").c_str(), O_RDONLY);
        if (pagetable_fd >= 0 && pread(pagetable_fd, &num_docs, sizeof num_docs, 0) != sizeof num_docs) {
            num_docs = 0;
        }
    }

    // passage text of doc, without pid prefix
    std::string passage(uint32_t doc) const {
        if (pagetable_fd < 0 || collection_fd < 0 || doc >= num_docs) {
            return "";
        }
        // pagetable.bin: u32 N | u32 pid[N] | u16 docLen[N] | u64 docOffset[N]
        uint64_t offset = 0;
        off_t at = static_cast<off_t>(4 + 6ull * num_docs + 8ull * doc);
        if (pread(pagetable_fd, &offset, sizeof offset, at) != sizeof offset) {
            return "";
        }
        char buf[4096];
        ssize_t got = pread(collection_fd, buf, sizeof buf, static_cast<off_t>(offset));
        if (got <= 0) {
            return "";
        }
        std::string line(buf, static_cast<size_t>(got));
        line = line.substr(0, line.find('\n'));
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            line.erase(0, tab + 1);
        }
        return line;
    }
};

inline bool is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// display snippet window of text starting a bit before the rarest query term in it (highest idf)
inline std::string make_snippet(const std::string& text, const std::vector<std::pair<std::string, double>>& terms) {
    size_t match = 0;
    double best_idf = -1;
    for (size_t i = 0; i < text.size();) {
        if (!is_word_char(text[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        std::string word;
        while (j < text.size() && is_word_char(text[j])) {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(text[j]))));
            ++j;
        }
        for (const auto& [term, idf] : terms) {
            if (word == term && idf > best_idf) {
                match = i;
                best_idf = idf;
            }
        }
        i = j;
    }

    size_t start = 0;
    if (match > SNIPPET_BEFORE) {
        start = match - SNIPPET_BEFORE;
        if (is_word_char(text[start - 1]) && is_word_char(text[start])) {
            size_t space = text.find(' ', start);
            if (space != std::string::npos && space < match) {
                start = space + 1;
            }
        }
    }
    size_t end = text.size();
    if (end - start > SNIPPET_CHARS) {
        end = start + SNIPPET_CHARS;
        size_t space = text.rfind(' ', end);
        if (space != std::string::npos && space > start) {
            end = space;
        }
    }

    std::string out = text.substr(start, end - start);
    if (start > 0) {
        out = "..." + out;
    }
    if (end < text.size()) {
        out += "...";
    }
    return out;
}

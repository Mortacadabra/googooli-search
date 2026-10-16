#pragma once

// handles loading metadata, lexicon and page table columns of index to memory. 

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "../index_format.h"

[[noreturn]] inline void die(const std::string& msg) {
    std::fprintf(stderr, "query: %s\n", msg.c_str());
    std::exit(1);
}

// keeps metadata related to the index, initialized at index loading
struct Index {
    int fd = -1;                        // file handle for index.bin
    std::vector<uint32_t> last_doc;     // last doc ID per block
    std::vector<uint16_t> doc_bytes;     
    std::vector<uint64_t> block_offset; // byte position per block, +1 last entry for the end of index
    std::vector<char> lex_data;         // raw bytes of lexicon -- just here for LexEntry's term string_view
    std::vector<LexEntry> lexicon;      // one entry per term, sorted by term
    std::vector<uint32_t> pid;          // passage id for each doc id
    std::vector<uint16_t> doc_len;       
    double avg_doc_len = 0;           

    void load(const std::string& dir) {
        Meta meta = read_meta(dir + "/meta.bin");
        last_doc = std::move(meta.last_doc);
        doc_bytes = std::move(meta.doc_bytes);
        size_t n = last_doc.size();
        block_offset.assign(n + 1, 0);
        for (size_t b = 0; b < n; ++b) {
            block_offset[b + 1] = block_offset[b] + doc_bytes[b] + meta.freq_bytes[b];
        }

        lexicon = read_lexicon(dir + "/lexicon.bin", lex_data);

        PageTable pt = read_pagetable(dir + "/pagetable.bin", /*with_offsets=*/false);
        pid = std::move(pt.pid);
        doc_len = std::move(pt.doc_len);

        uint64_t total_len = 0;
        for (uint16_t len : doc_len) {
            total_len += len;
        }
        avg_doc_len = static_cast<double>(total_len) / static_cast<double>(doc_len.size());

        fd = open((dir + "/index.bin").c_str(), O_RDONLY);
        if (fd < 0) {
            die("cannot open index.bin");
        }
    }

    const LexEntry* lookup(std::string_view term) const {
        auto it = std::lower_bound(lexicon.begin(), lexicon.end(), term,
                                   [](const LexEntry& e, std::string_view t) { return e.term < t; });
        if (it != lexicon.end() && it->term == term) {
            return &*it;
        }
        return nullptr;
    }
};

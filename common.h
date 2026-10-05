#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace idx {

constexpr uint32_t BLOCK_SIZE   = 64;          // postings per global block
constexpr size_t   MAX_TERM_LEN = 64;          
constexpr uint32_t MAXDID       = UINT32_MAX; 
constexpr double   K1 = 0.9, B = 0.4;          // BM25 parameters

[[noreturn]] inline void die(const std::string& msg) {
    std::fprintf(stderr, "fatal: %s\n", msg.c_str());
    std::exit(1);
}

inline void tokenize(std::string_view text, std::vector<std::string>& out) {
    out.clear();
    std::string cur; 
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (cur.size() < MAX_TERM_LEN) cur.push_back(static_cast<char>(c));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
}

inline void vbEncode(uint32_t x, std::vector<uint8_t>& out) {
    while (x >= 128) { out.push_back(static_cast<uint8_t>(0x80 | (x & 0x7F))); x >>= 7; }
    out.push_back(static_cast<uint8_t>(x));
}

inline uint32_t vbDecode(const uint8_t*& p) {
    uint32_t x = 0;
    int shift = 0;
    while (*p & 0x80) { x |= uint32_t(*p++ & 0x7F) << shift; shift += 7; }
    return x | (uint32_t(*p++) << shift);
}

struct ByteWriter {
    std::vector<uint8_t> buf;
    template <class T> void put(T v) { putBytes(&v, sizeof v); }

    template <class T> void putVec(const std::vector<T>& v) { putBytes(v.data(), v.size() * sizeof(T)); }

    void putBytes(const void* p, size_t n) {
        buf.insert(buf.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
    }

    void save(const std::string& path) const {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f || std::fwrite(buf.data(), 1, buf.size(), f) != buf.size()) die("cannot write " + path);
        std::fclose(f);
    }
};

struct ByteReader {
    std::vector<uint8_t> buf;
    size_t pos = 0;
    std::string path;

    explicit ByteReader(const std::string& p) : path(p) {
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) die("cannot open " + p);
        std::fseek(f, 0, SEEK_END);
        buf.resize(static_cast<size_t>(std::ftell(f)));
        std::rewind(f);
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) die("short read on " + p);
        std::fclose(f);
    }

    void getBytes(void* out, size_t n) {
        if (buf.size() - pos < n) die(path + ": truncated");
        std::memcpy(out, buf.data() + pos, n);
        pos += n;
    }

    template <class T> T get() { T v; getBytes(&v, sizeof v); return v; }
    template <class T> void getVec(std::vector<T>& v, size_t n) { v.resize(n); getBytes(v.data(), n * sizeof(T)); }
    void expectEnd() const { if (pos != buf.size()) die(path + ": unexpected trailing bytes"); }
};

// ---- On-disk records ----

struct LexiconEntry {
    std::string term;
    uint32_t ft;          // number (frequency) of postings 
    uint32_t startBlock;  // block holding the list's first posting
    uint8_t  startSlot;   // position of the first posting within the block
};

struct BlockMeta {
    std::vector<uint32_t> lastDocID;  // docID of the last posting in each block
    std::vector<uint16_t> docBytes;   // byte size of each block's docID section
    std::vector<uint16_t> freqBytes;  // byte size of each block's freq section
};

struct PageTable {
    std::vector<uint32_t> pid;        // docID, which is equivalent to MS MARCO passage id
    std::vector<uint16_t> docLen;     
    std::vector<uint64_t> docOffset;  // byte offset of the line within input TSV data
};

inline void writeLexicon(const std::string& path, const std::vector<LexiconEntry>& lex) {
    ByteWriter w;
    w.put<uint32_t>(static_cast<uint32_t>(lex.size()));
    for (const LexiconEntry& e : lex) {
        w.put<uint8_t>(static_cast<uint8_t>(e.term.size()));
        w.putBytes(e.term.data(), e.term.size());
        w.put(e.ft); w.put(e.startBlock); w.put(e.startSlot);
    }
    w.save(path);
}

inline std::vector<LexiconEntry> readLexicon(const std::string& path) {
    ByteReader r(path);
    std::vector<LexiconEntry> lex(r.get<uint32_t>());
    for (LexiconEntry& e : lex) {
        e.term.resize(r.get<uint8_t>());
        r.getBytes(&e.term[0], e.term.size());  // &s[0]: writable in any C++ standard
        e.ft = r.get<uint32_t>(); e.startBlock = r.get<uint32_t>(); e.startSlot = r.get<uint8_t>();
    }
    r.expectEnd();
    return lex;
}

template <class C, class... Cs>
void writeColumns(const std::string& path, const C& first, const Cs&... rest) {
    ByteWriter w;
    w.put<uint32_t>(static_cast<uint32_t>(first.size()));
    w.putVec(first); (w.putVec(rest), ...);
    w.save(path);
}

template <class... Cs>
void readColumns(const std::string& path, Cs&... cols) {
    ByteReader r(path);
    const uint32_t n = r.get<uint32_t>();
    (r.getVec(cols, n), ...);
    r.expectEnd();
}

inline void writeMeta(const std::string& p, const BlockMeta& m) { writeColumns(p, m.lastDocID, m.docBytes, m.freqBytes); }
inline void writePageTable(const std::string& p, const PageTable& t) { writeColumns(p, t.pid, t.docLen, t.docOffset); }
inline BlockMeta readMeta(const std::string& p) { BlockMeta m; readColumns(p, m.lastDocID, m.docBytes, m.freqBytes); return m; }
inline PageTable readPageTable(const std::string& p) { PageTable t; readColumns(p, t.pid, t.docLen, t.docOffset); return t; }

}  // namespace idx

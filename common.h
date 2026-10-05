// common.h -- definitions shared by the index builder (build.cpp) and the
// query processor (query.cpp).
//
// This header is the single source of truth for the on-disk index format:
//   * tokenize()          how passages and queries are split into terms
//   * varbyte coding      how docIDs and frequencies are compressed
//   * BlockWriter         how the posting stream is cut into blocks (index.bin
//                         + meta.bin); shared with the future merger
//   * BM25                the ranking function
//   * IndexReader         loads pagetable.bin, lexicon.bin and meta.bin
//   * ListCursor          the list-access API (openList / nextGEQ / getFreq)
//
// Index layout (lecture 2, "Index Layout: A Concrete Example"):
// all inverted lists are concatenated in lexicon order into one stream of
// postings, and the stream is cut into global blocks of BLOCK_SIZE postings.
// A list may start and end in the middle of a block, and a block may hold the
// tail of one list and the head of the next. Global slot s is posting number s
// of the stream: it lives in block s / BLOCK_SIZE at position s % BLOCK_SIZE.
//
// All binary integers are little-endian fixed-width types, written with memcpy
// from host memory (so the host must be little-endian).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/types.h> 
#include <vector>

namespace idx {


constexpr uint32_t BLOCK_SIZE   = 64;          // number postings per block
constexpr size_t   MAX_TERM_LEN = 64;          
constexpr uint32_t MAXDID       = UINT32_MAX;  
constexpr double   K1 = 0.9;                   // BM25 parameters
constexpr double   B  = 0.4;

// abort program with a message
[[noreturn]] inline void die(const std::string& msg) {
    std::fprintf(stderr, "fatal: %s\n", msg.c_str());
    std::exit(1);
}

// Basic parser without temming and stop word removal
inline void tokenize(std::string_view text, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    bool inTerm = false;
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        bool isTermChar = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (isTermChar) {
            if (cur.size() < MAX_TERM_LEN) cur.push_back(static_cast<char>(c));
            inTerm = true;
        } else if (inTerm) {
            out.push_back(cur);
            cur.clear();
            inTerm = false;
        }
    }
    if (inTerm) out.push_back(cur);
}

inline void vbEncode(uint32_t x, std::vector<uint8_t>& out) {
    while (x >= 128) {
        out.push_back(static_cast<uint8_t>(0x80 | (x & 0x7F)));
        x >>= 7;
    }
    out.push_back(static_cast<uint8_t>(x));
}

inline uint32_t vbDecode(const uint8_t*& p) {
    uint32_t x = 0;
    int shift = 0;
    while (*p & 0x80) {
        x |= static_cast<uint32_t>(*p++ & 0x7F) << shift;
        shift += 7;
    }
    x |= static_cast<uint32_t>(*p++) << shift;
    return x;
}

// ---------------------------------------------------------------------------
// Little-endian fixed-width helpers (host is checked to be little-endian)
// ---------------------------------------------------------------------------

template <typename T>
inline void putRaw(std::vector<uint8_t>& out, T v) {
    uint8_t buf[sizeof(T)];
    std::memcpy(buf, &v, sizeof(T));
    out.insert(out.end(), buf, buf + sizeof(T));
}

template <typename T>
inline T getRaw(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// ---------------------------------------------------------------------------
// BM25 (query side only: the index stores raw frequencies, no scores)
// ---------------------------------------------------------------------------
// idf as in the lecture slides. Note: it is negative for terms that occur in
// more than half of all documents (ft > N/2), so such terms lower a score.

inline double bm25Idf(uint32_t N, uint32_t ft) {
    return std::log((static_cast<double>(N) - ft + 0.5) / (ft + 0.5));
}

// Contribution of one term with frequency f in a document of length docLen.
inline double bm25Term(double idf, uint32_t f, uint32_t docLen, double avgdl) {
    double K = K1 * (1.0 - B + B * docLen / avgdl);
    return idf * (f * (K1 + 1.0)) / (f + K);
}

// ---------------------------------------------------------------------------
// BlockWriter: cuts the posting stream into global blocks
// ---------------------------------------------------------------------------
// index.bin is a sequence of blocks with no headers:
//   [docIDs of the block, varbyte] [freqs of the block, varbyte]
// meta.bin holds the per-block metadata, columnar:
//   u32 numBlocks | u32 lastDocID[numBlocks] | u16 docBytes[] | u16 freqBytes[]
//
// docID encoding: a posting's raw value is its absolute docID if it is the
// first posting of its block or the first posting of its list; otherwise it is
// the gap to the previous posting (which is then in the same list and block).
// Every block therefore decodes on its own, and no gap ever crosses a list
// boundary.
//
// Usage: for each list in lexicon order, beginList() then add() each posting;
// finally finish() and writeMeta().

class BlockWriter {
public:
    explicit BlockWriter(FILE* out) : out_(out) {}

    // Starts a new list and returns where its first posting will go.
    void beginList(uint32_t& startBlock, uint8_t& startSlot) {
        startBlock = static_cast<uint32_t>(lastDocID_.size());  // blocks flushed so far
        startSlot  = static_cast<uint8_t>(buffered_);
        listStart_ = true;
    }

    // Appends one posting of the current list (docIDs strictly increasing).
    void add(uint32_t docID, uint32_t freq) {
        const bool absolute = listStart_ || buffered_ == 0;
        vbEncode(absolute ? docID : docID - prevDoc_, docBuf_);
        vbEncode(freq, freqBuf_);
        prevDoc_ = docID;
        listStart_ = false;
        ++totalPostings_;
        if (++buffered_ == BLOCK_SIZE) flush();
    }

    // Flushes the partial final block (if any). Call once after the last list.
    void finish() { if (buffered_ > 0) flush(); }

    void writeMeta(const std::string& path) const {
        std::vector<uint8_t> buf;
        const uint32_t n = numBlocks();
        buf.reserve(4 + size_t(n) * 8);
        putRaw<uint32_t>(buf, n);
        for (uint32_t v : lastDocID_) putRaw<uint32_t>(buf, v);
        for (uint16_t v : docBytes_)  putRaw<uint16_t>(buf, v);
        for (uint16_t v : freqBytes_) putRaw<uint16_t>(buf, v);
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f || std::fwrite(buf.data(), 1, buf.size(), f) != buf.size())
            die("cannot write " + path);
        std::fclose(f);
    }

    uint32_t numBlocks() const { return static_cast<uint32_t>(lastDocID_.size()); }
    uint64_t totalPostings() const { return totalPostings_; }

private:
    void flush() {
        // 64 values x 5 bytes max = 320 bytes per section: always fits a u16.
        if (docBuf_.size() > UINT16_MAX || freqBuf_.size() > UINT16_MAX)
            die("block section does not fit in u16");
        if (std::fwrite(docBuf_.data(), 1, docBuf_.size(), out_) != docBuf_.size() ||
            std::fwrite(freqBuf_.data(), 1, freqBuf_.size(), out_) != freqBuf_.size())
            die("write failed on index.bin");
        lastDocID_.push_back(prevDoc_);
        docBytes_.push_back(static_cast<uint16_t>(docBuf_.size()));
        freqBytes_.push_back(static_cast<uint16_t>(freqBuf_.size()));
        docBuf_.clear();
        freqBuf_.clear();
        buffered_ = 0;
    }

    FILE* out_;
    std::vector<uint8_t> docBuf_, freqBuf_;  // encoded sections of the open block
    uint32_t buffered_ = 0;                  // postings in the open block
    uint32_t prevDoc_ = 0;                   // docID of the previous posting
    bool listStart_ = false;                 // next posting starts a list
    uint64_t totalPostings_ = 0;
    std::vector<uint32_t> lastDocID_;        // meta.bin columns
    std::vector<uint16_t> docBytes_, freqBytes_;
};

// ---------------------------------------------------------------------------
// IndexReader: page table, lexicon and block metadata in memory
// ---------------------------------------------------------------------------

struct ListCursor;

// Reads a whole (small) file into memory.
inline std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) die("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (size > 0 && std::fread(buf.data(), 1, buf.size(), f) != buf.size())
        die("short read on " + path);
    std::fclose(f);
    return buf;
}

struct IndexReader {
    // Page table (pagetable.bin), indexed by docID.
    uint32_t N = 0;
    double   avgdl = 0.0;             // computed on load from docLen[]
    std::vector<uint32_t> pid;        // MS MARCO passage id (the document "name")
    std::vector<uint16_t> docLen;     // tokens in the passage
    std::vector<uint64_t> docOffset;  // byte offset of the line in collection.tsv

    // Lexicon (lexicon.bin) as column arrays, sorted by term.
    // Term i is termChars[termStart[i] .. termStart[i+1]).
    uint32_t numTerms = 0;
    std::string           termChars;
    std::vector<uint32_t> termStart;
    std::vector<uint32_t> ft;
    std::vector<uint32_t> startBlock;
    std::vector<uint8_t>  startSlot;

    // Block metadata (meta.bin) plus prefix sums: block i occupies bytes
    // [blockOffset[i], blockOffset[i+1]) of index.bin, docIDs first.
    uint32_t numBlocks = 0;
    std::vector<uint32_t> lastDocID;
    std::vector<uint16_t> docBytes, freqBytes;
    std::vector<uint64_t> blockOffset;  // numBlocks + 1 entries

    uint64_t totalPostings = 0;  // sum of ft over all terms
    std::string indexPath;
    FILE* indexFile = nullptr;

    IndexReader() = default;
    IndexReader(const IndexReader&) = delete;
    IndexReader& operator=(const IndexReader&) = delete;
    ~IndexReader() { if (indexFile) std::fclose(indexFile); }

    void open(const std::string& dir) {
        loadPageTable(dir + "/pagetable.bin");
        loadLexicon(dir + "/lexicon.bin");
        loadMeta(dir + "/meta.bin");
        indexPath = dir + "/index.bin";
        indexFile = std::fopen(indexPath.c_str(), "rb");
        if (!indexFile) die("cannot open " + indexPath);
        checkConsistency();
    }

    std::string_view term(uint32_t i) const {
        return std::string_view(termChars).substr(termStart[i], termStart[i + 1] - termStart[i]);
    }

    // Global slot of the first posting of term i.
    uint64_t listStart(uint32_t i) const {
        return uint64_t(startBlock[i]) * BLOCK_SIZE + startSlot[i];
    }

    // Binary search over the sorted lexicon. Returns -1 if absent.
    int64_t lookup(std::string_view t) const {
        uint32_t lo = 0, hi = numTerms;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            int c = term(mid).compare(t);
            if (c == 0) return mid;
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        return -1;
    }

    // Number of postings in block b (BLOCK_SIZE except possibly the last).
    uint32_t blockLen(uint32_t b) const {
        return (b + 1 < numBlocks) ? BLOCK_SIZE
                                   : static_cast<uint32_t>(totalPostings - uint64_t(b) * BLOCK_SIZE);
    }

    // Reads len bytes of index.bin starting at byte offset `off` into `out`
    // (one seek + one read). Every access to index.bin goes through here, so
    // an inverted-list cache can later be added by changing only this function.
    void readRange(uint64_t off, size_t len, std::vector<uint8_t>& out) const {
        out.resize(len);
        if (len == 0) return;
        if (fseeko(indexFile, static_cast<off_t>(off), SEEK_SET) != 0 ||
            std::fread(out.data(), 1, len, indexFile) != len)
            die("cannot read " + std::to_string(len) + " bytes at offset " +
                std::to_string(off) + " of index.bin");
    }

    // List-access API. A term that is not in the lexicon yields an empty
    // cursor (ft == 0) whose nextGEQ always returns MAXDID. Defined below,
    // after ListCursor.
    ListCursor openList(std::string_view t) const;
    ListCursor openList(uint32_t termIndex) const;
    static void closeList(ListCursor& c);

private:
    void loadPageTable(const std::string& path) {
        std::vector<uint8_t> buf = readFile(path);
        // Layout: u32 N | u32 pid[N] | u16 docLen[N] | u64 docOffset[N]
        if (buf.size() < 4) die(path + ": truncated header");
        N = getRaw<uint32_t>(buf.data());
        if (buf.size() != 4 + size_t(N) * (4 + 2 + 8)) die(path + ": unexpected size");
        const uint8_t* p = buf.data() + 4;
        pid.resize(N);
        docLen.resize(N);
        docOffset.resize(N);
        std::memcpy(pid.data(), p, size_t(N) * 4);       p += size_t(N) * 4;
        std::memcpy(docLen.data(), p, size_t(N) * 2);    p += size_t(N) * 2;
        std::memcpy(docOffset.data(), p, size_t(N) * 8);
        uint64_t totalTokens = 0;
        for (uint16_t l : docLen) totalTokens += l;
        avgdl = N ? static_cast<double>(totalTokens) / N : 0.0;
    }

    void loadLexicon(const std::string& path) {
        std::vector<uint8_t> buf = readFile(path);
        const uint8_t* p = buf.data();
        const uint8_t* end = p + buf.size();
        auto need = [&](size_t n) { if (size_t(end - p) < n) die(path + ": truncated"); };
        need(4);
        numTerms = getRaw<uint32_t>(p); p += 4;
        termStart.reserve(numTerms + 1);
        ft.reserve(numTerms); startBlock.reserve(numTerms);
        startSlot.reserve(numTerms);
        for (uint32_t i = 0; i < numTerms; ++i) {
            // Record: u8 len, term, u32 ft, u32 startBlock, u8 startSlot.
            need(1);
            uint8_t len = *p++;
            need(len + 4 + 4 + 1);
            termStart.push_back(static_cast<uint32_t>(termChars.size()));
            termChars.append(reinterpret_cast<const char*>(p), len); p += len;
            ft.push_back(getRaw<uint32_t>(p));         p += 4;
            startBlock.push_back(getRaw<uint32_t>(p)); p += 4;
            startSlot.push_back(*p);                   p += 1;
            totalPostings += ft.back();
        }
        termStart.push_back(static_cast<uint32_t>(termChars.size()));
        if (p != end) die(path + ": trailing bytes");
    }

    void loadMeta(const std::string& path) {
        std::vector<uint8_t> buf = readFile(path);
        if (buf.size() < 4) die(path + ": truncated header");
        numBlocks = getRaw<uint32_t>(buf.data());
        if (buf.size() != 4 + size_t(numBlocks) * (4 + 2 + 2)) die(path + ": unexpected size");
        const uint8_t* p = buf.data() + 4;
        lastDocID.resize(numBlocks);
        docBytes.resize(numBlocks);
        freqBytes.resize(numBlocks);
        std::memcpy(lastDocID.data(), p, size_t(numBlocks) * 4); p += size_t(numBlocks) * 4;
        std::memcpy(docBytes.data(), p, size_t(numBlocks) * 2);  p += size_t(numBlocks) * 2;
        std::memcpy(freqBytes.data(), p, size_t(numBlocks) * 2);
        blockOffset.resize(size_t(numBlocks) + 1);
        blockOffset[0] = 0;
        for (uint32_t b = 0; b < numBlocks; ++b)
            blockOffset[b + 1] = blockOffset[b] + docBytes[b] + freqBytes[b];
    }

    // Cheap structural checks that the four files belong together.
    void checkConsistency() const {
        if (uint64_t(numBlocks) != (totalPostings + BLOCK_SIZE - 1) / BLOCK_SIZE)
            die("meta.bin: numBlocks != ceil(totalPostings / BLOCK_SIZE)");
        // Lists are contiguous in lexicon order: each starts where the previous ended.
        uint64_t slot = 0;
        for (uint32_t i = 0; i < numTerms; ++i) {
            if (startSlot[i] >= BLOCK_SIZE || listStart(i) != slot || ft[i] == 0)
                die("lexicon.bin: list of '" + std::string(term(i)) + "' is not where expected");
            slot += ft[i];
        }
    }
};

// ---------------------------------------------------------------------------
// ListCursor: the list-access API (openList / nextGEQ / getFreq / closeList)
// ---------------------------------------------------------------------------
// A list occupies global slots [start, start + ft), i.e. from
// (startBlock, startSlot) to (endBlock, endSlot). On open, the compressed
// bytes of blocks startBlock..endBlock are read with one readRange(); nothing
// is decompressed up front. nextGEQ skips whole blocks using lastDocID[] and
// decodes only the docID section of the block it lands in; getFreq decodes
// that block's freq section lazily on first use.
//
// Within a block, only positions lo..hi belong to this list:
//   lo = startSlot in the start block, else 0
//   hi = endSlot in the end block, else the last position of the block
// Positions outside lo..hi belong to neighbouring lists and are ignored.

struct ListCursor {
    // Term info (ft == 0: term not in the index, the list is empty).
    int64_t  termIndex = -1;
    uint32_t ft = 0;
    double   idf = 0.0;
    uint32_t startBlock = 0, startSlot = 0;
    uint32_t endBlock = 0, endSlot = 0;

    const IndexReader* ix = nullptr;  // block metadata (lastDocID, sizes, offsets)
    std::vector<uint8_t> data;        // raw bytes of blocks startBlock..endBlock
    uint64_t dataBase = 0;            // = ix->blockOffset[startBlock]

    // Traversal state.
    bool     done = true;        // exhausted (or empty list)
    uint32_t curBlock = 0;       // block containing the current posting
    int64_t  decodedBlock = -1;  // block whose docIDs are in docs[] (-1 = none)
    bool     freqsDecoded = false;
    uint32_t lo = 0, hi = 0;     // this list's positions in the decoded block
    uint32_t pos = 0;            // position of the current posting
    uint32_t curDoc = MAXDID;    // docID of the current posting
    uint32_t docs[BLOCK_SIZE];   // absolute docIDs, valid at positions lo..hi
    uint32_t freqs[BLOCK_SIZE];  // valid at positions lo..hi once decoded

    // Moves forward to the first posting with docID >= k and returns its
    // docID, or MAXDID if there is none. Never moves backwards: if k is not
    // larger than the current docID, the current posting is returned.
    uint32_t nextGEQ(uint32_t k) {
        if (done) return MAXDID;
        // Skip whole blocks, but only those strictly before endBlock: in them
        // the last position (63) belongs to this list, so lastDocID is ours.
        // lastDocID[endBlock] may belong to a later list and is never used.
        while (curBlock < endBlock && ix->lastDocID[curBlock] < k) ++curBlock;
        if (decodedBlock != curBlock) decodeDocs(curBlock);
        while (pos <= hi && docs[pos] < k) ++pos;
        if (pos > hi) {
            // Only possible in the end block: no posting >= k is left.
            if (curBlock != endBlock) die("lastDocID inconsistent with block contents");
            done = true;
            return curDoc = MAXDID;
        }
        return curDoc = docs[pos];
    }

    // Frequency of the current posting. Only valid after nextGEQ returned a
    // docID other than MAXDID.
    uint32_t getFreq() {
        if (!freqsDecoded) decodeFreqs();
        return freqs[pos];
    }

private:
    // Start of block b's docID section within data.
    const uint8_t* blockPtr(uint32_t b) const {
        return data.data() + (ix->blockOffset[b] - dataBase);
    }

    void decodeDocs(uint32_t b) {
        const uint32_t n = ix->blockLen(b);
        lo = (b == startBlock) ? startSlot : 0;
        hi = (b == endBlock) ? endSlot : n - 1;
        const uint8_t* p = blockPtr(b);
        const uint8_t* end = p + ix->docBytes[b];
        // Varbyte has no random access, so positions before lo are decoded and
        // discarded. Position lo is absolute (block-first or list-first);
        // later positions are gaps.
        for (uint32_t j = 0; j < lo; ++j) vbDecode(p);
        uint32_t run = vbDecode(p);
        docs[lo] = run;
        for (uint32_t j = lo + 1; j <= hi; ++j) docs[j] = (run += vbDecode(p));
        if (p > end || (hi == n - 1 && p != end))
            die("corrupt docID section in block " + std::to_string(b));
        decodedBlock = b;
        freqsDecoded = false;
        pos = lo;
    }

    void decodeFreqs() {
        const uint8_t* p = blockPtr(curBlock) + ix->docBytes[curBlock];
        const uint8_t* end = p + ix->freqBytes[curBlock];
        for (uint32_t j = 0; j <= hi; ++j) freqs[j] = vbDecode(p);
        if (p > end) die("corrupt freq section in block " + std::to_string(curBlock));
        freqsDecoded = true;
    }

};

inline ListCursor IndexReader::openList(std::string_view t) const {
    int64_t i = lookup(t);
    return i < 0 ? ListCursor() : openList(static_cast<uint32_t>(i));
}

inline ListCursor IndexReader::openList(uint32_t i) const {
    ListCursor c;
    c.termIndex = i;
    c.ft = ft[i];
    c.idf = bm25Idf(N, c.ft);
    const uint64_t first = listStart(i), last = first + ft[i] - 1;  // global slots
    c.startBlock = static_cast<uint32_t>(first / BLOCK_SIZE);
    c.startSlot  = static_cast<uint32_t>(first % BLOCK_SIZE);
    c.endBlock   = static_cast<uint32_t>(last / BLOCK_SIZE);
    c.endSlot    = static_cast<uint32_t>(last % BLOCK_SIZE);
    c.ix = this;
    // One seek + one read for the compressed bytes of all blocks of the list.
    c.dataBase = blockOffset[c.startBlock];
    readRange(c.dataBase, blockOffset[c.endBlock + 1] - c.dataBase, c.data);
    c.curBlock = c.startBlock;
    c.done = false;
    return c;
}

inline void IndexReader::closeList(ListCursor& c) { c = ListCursor(); }

// ---------------------------------------------------------------------------
// ASCII dump format, shared by `build --ref` and `query --dump` so the two can
// be diffed:  term ft=<ft> | d1:f1 d2:f2 ...
// ---------------------------------------------------------------------------

inline void printListHeader(FILE* out, std::string_view term, uint32_t ft) {
    std::fprintf(out, "%.*s ft=%u |", int(term.size()), term.data(), ft);
}

}  // namespace idx

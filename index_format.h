#pragma once

// This files defines the binary format of the index files, shared by the indexer and the query processor,
// All files are little-endian.
//
//   index.bin      blocks of BLOCK_SIZE postings: [docIDs varbyte][freqs varbyte] (see merge_runs)
//   meta.bin       u32 numBlocks | u32 lastDocID[] | u16 docBytes[] | u16 freqBytes[]
//   lexicon.bin    u32 numTerms | per term: u8 len, term, u32 ft, u32 startBlock, u8 startSlot
//   pagetable.bin  u32 N | u32 pid[N] | u16 docLen[N] | u64 docOffset[N]

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the index files are little-endian");

[[noreturn]] inline void format_error(const std::string& msg) {
    std::fprintf(stderr, "index files: %s\n", msg.c_str());
    std::exit(1);
}

inline FILE* open_or_die(const std::string& path, const char* mode) {
    FILE* f = std::fopen(path.c_str(), mode);
    if (!f) {
        format_error("cannot open " + path + ": " + std::strerror(errno));
    }
    return f;
}

inline void write_bytes(const void* data, size_t size, FILE* f, const std::string& path) {
    if (size > 0 && std::fwrite(data, 1, size, f) != size) {
        format_error("write failed on " + path + ": " + std::strerror(errno) + " (check free disk space)");
    }
}

inline void read_bytes(void* data, size_t size, FILE* f, const std::string& path) {
    if (size > 0 && std::fread(data, 1, size, f) != size) {
        format_error("truncated " + path);
    }
}

template <class T>
inline void write_column(const std::vector<T>& v, FILE* f, const std::string& path) {
    write_bytes(v.data(), v.size() * sizeof(T), f, path);
}

template <class T>
inline std::vector<T> read_column(size_t n, FILE* f, const std::string& path) {
    std::vector<T> v(n);
    read_bytes(v.data(), n * sizeof(T), f, path);
    return v;
}

// meta.bin: one entry per block of index.bin
struct Meta {
    std::vector<uint32_t> last_doc;    // last docID in the block
    std::vector<uint16_t> doc_bytes;   // size of the block's docID section
    std::vector<uint16_t> freq_bytes;  // size of the block's freq section
};

inline void write_meta(const std::string& path, const Meta& m) {
    FILE* f = open_or_die(path, "wb");
    uint32_t n = static_cast<uint32_t>(m.last_doc.size());
    write_bytes(&n, sizeof n, f, path);
    write_column(m.last_doc, f, path);
    write_column(m.doc_bytes, f, path);
    write_column(m.freq_bytes, f, path);
    std::fclose(f);
}

inline Meta read_meta(const std::string& path) {
    FILE* f = open_or_die(path, "rb");
    uint32_t n;
    read_bytes(&n, sizeof n, f, path);
    Meta m;
    m.last_doc = read_column<uint32_t>(n, f, path);
    m.doc_bytes = read_column<uint16_t>(n, f, path);
    m.freq_bytes = read_column<uint16_t>(n, f, path);
    std::fclose(f);
    return m;
}

// lexicon.bin
struct LexEntry {
    std::string_view term;
    uint32_t ft = 0, start_block = 0;
    uint8_t start_slot = 0;
};

inline constexpr size_t MAX_TERM_BYTES = 255;  // the term length is stored in one byte

// Appends the lexicon record of e to out.
inline void write_lex_entry(std::string& out, const LexEntry& e) {
    out.push_back(static_cast<char>(e.term.size()));
    out += e.term;
    out.append(reinterpret_cast<const char*>(&e.ft), sizeof e.ft);
    out.append(reinterpret_cast<const char*>(&e.start_block), sizeof e.start_block);
    out.push_back(static_cast<char>(e.start_slot));
}

// Reads the lexicon record at p and advances p; the term points into the buffer at p.
inline LexEntry read_lex_entry(const char*& p) {
    LexEntry e;
    uint8_t len = static_cast<uint8_t>(*p++);
    e.term = std::string_view(p, len);
    p += len;
    std::memcpy(&e.ft, p, sizeof e.ft);
    p += sizeof e.ft;
    std::memcpy(&e.start_block, p, sizeof e.start_block);
    p += sizeof e.start_block;
    e.start_slot = static_cast<uint8_t>(*p++);
    return e;
}

// Reads lexicon.bin into data and returns its entries, whose terms point into data.
inline std::vector<LexEntry> read_lexicon(const std::string& path, std::vector<char>& data) {
    FILE* f = open_or_die(path, "rb");
    std::fseek(f, 0, SEEK_END);
    data.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    read_bytes(data.data(), data.size(), f, path);
    std::fclose(f);

    const char* p = data.data();
    uint32_t n;
    std::memcpy(&n, p, sizeof n);
    p += sizeof n;
    std::vector<LexEntry> lexicon(n);
    for (LexEntry& e : lexicon) {
        e = read_lex_entry(p);
    }
    return lexicon;
}

// pagetable.bin: one entry per docID
struct PageTable {
    std::vector<uint32_t> pid;         // MS MARCO passage id
    std::vector<uint16_t> doc_len;     // length in tokens
    std::vector<uint64_t> doc_offset;  // byte offset of the passage's line in collection.tsv
};

// Reads pagetable.bin; the docOffset column is skipped unless with_offsets.
inline PageTable read_pagetable(const std::string& path, bool with_offsets) {
    FILE* f = open_or_die(path, "rb");
    uint32_t n;
    read_bytes(&n, sizeof n, f, path);
    PageTable pt;
    pt.pid = read_column<uint32_t>(n, f, path);
    pt.doc_len = read_column<uint16_t>(n, f, path);
    if (with_offsets) {
        pt.doc_offset = read_column<uint64_t>(n, f, path);
    }
    std::fclose(f);
    return pt;
}

// Streams the page table to disk while documents are parsed: each column goes to its own temp
// file, and finish() concatenates them into pagetable.bin.
class PageTableWriter {
public:
    explicit PageTableWriter(const std::string& dir) {
        for (int c = 0; c < 3; ++c) {
            tmp_path_[c] = dir + "/pagetable." + kName[c] + ".tmp";
            tmp_[c] = open_or_die(tmp_path_[c], "wb");
        }
    }

    // removes the temp files if finish() was never reached
    ~PageTableWriter() {
        if (finished_) {
            return;
        }
        for (int c = 0; c < 3; ++c) {
            std::fclose(tmp_[c]);
            unlink(tmp_path_[c].c_str());
        }
    }
    PageTableWriter(const PageTableWriter&) = delete;
    PageTableWriter& operator=(const PageTableWriter&) = delete;

    void add(uint32_t pid, uint16_t doc_len, uint64_t doc_offset) {
        put(0, &pid, sizeof pid);
        put(1, &doc_len, sizeof doc_len);
        put(2, &doc_offset, sizeof doc_offset);
        ++n_;
    }

    void finish(const std::string& path) {
        if (n_ > UINT32_MAX) {
            format_error("too many documents for a u32 N");
        }
        for (int c = 0; c < 3; ++c) {
            flush(c);
            std::fclose(tmp_[c]);
        }
        FILE* out = open_or_die(path, "wb");
        uint32_t n = static_cast<uint32_t>(n_);
        write_bytes(&n, sizeof n, out, path);
        std::vector<char> chunk(1 << 20);
        for (int c = 0; c < 3; ++c) {  // pid[], docLen[], docOffset[] in order
            FILE* in = open_or_die(tmp_path_[c], "rb");
            size_t got;
            while ((got = std::fread(chunk.data(), 1, chunk.size(), in)) > 0) {
                write_bytes(chunk.data(), got, out, path);
            }
            std::fclose(in);
            unlink(tmp_path_[c].c_str());
        }
        std::fclose(out);
        finished_ = true;
    }

private:
    void put(int c, const void* v, size_t size) {
        buf_[c].append(static_cast<const char*>(v), size);
        if (buf_[c].size() >= (1 << 20)) {
            flush(c);
        }
    }
    void flush(int c) {
        write_bytes(buf_[c].data(), buf_[c].size(), tmp_[c], tmp_path_[c]);
        buf_[c].clear();
    }

    static constexpr const char* kName[3] = {"pid", "len", "off"};
    std::string tmp_path_[3], buf_[3];
    FILE* tmp_[3];
    uint64_t n_ = 0;
    bool finished_ = false;
};

// Build:  make -C query
// Run:    ./query/query [project_root]      (default ".")
// Input:  one query per line, quits on empty line
//
// we load meta.bin, lexicon.bin and pagetable.bin into memory, while the large index.bin 
// stays on disk -- we only decode the blocks if needed

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "../config.h"
#include "../index_format.h"
#include "../varbyte.h"
#include "../indexer/tokenizer.h"
#include "../indexer/util.h"

static const uint32_t END = UINT32_MAX;  // docID past the end of a list
static const double K1 = 0.9, B = 0.4; // BM25 parameters

[[noreturn]] static void die(const std::string& msg) {
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

// Inverted list API
struct ListCursor {
    const Index* ix;
    std::vector<uint8_t> buf;  // blocks [start_block, end_block]
    uint64_t base;             // file offset of buf[0]
    uint32_t ft, start_block, start_slot, end_block, end_slot;
    double idf;

    uint32_t block, decoded = END;  // current block / block held in docs[]
    bool freqs_ready = false;
    int lo, hi, pos;                // this list's slots in the current block are [lo, hi]
    uint32_t docs[BLOCK_SIZE], freqs[BLOCK_SIZE];
    uint32_t cur = END;           
};

static const uint8_t* block_ptr(const ListCursor& lp) {
    return lp.buf.data() + (lp.ix->block_offset[lp.block] - lp.base);
}

// docIDs are stored absolute at the start of a block or of a list, as gaps otherwise
static void decode_docs(ListCursor& lp) {
    if (lp.block == lp.start_block) {
        lp.lo = lp.start_slot;
    } else {
        lp.lo = 0;
    }

    if (lp.block == lp.end_block) {
        lp.hi = lp.end_slot;
    } else {
        lp.hi = BLOCK_SIZE - 1;
    }
    
    const uint8_t* p = block_ptr(lp);
    for (int i = 0; i <= lp.hi; ++i) {
        uint32_t v = vb_decode(p);
        if (i >= lp.lo) {
            if (i == lp.lo) {
                lp.docs[i] = v;
            } else {
                lp.docs[i] = lp.docs[i - 1] + v;
            }
        }
    }
    lp.decoded = lp.block;
    lp.freqs_ready = false;
    lp.pos = lp.lo;
}

static std::unique_ptr<ListCursor> openList(const Index& ix, std::string_view term) {
    const LexEntry* e = ix.lookup(term);
    if (!e) {
        return nullptr;
    }

    auto lp = std::make_unique<ListCursor>();
    lp->ix = &ix;
    lp->ft = e->ft;
    lp->start_block = e->start_block;
    lp->start_slot = e->start_slot;
    uint64_t last = uint64_t(e->start_block) * BLOCK_SIZE + e->start_slot + e->ft - 1;
    lp->end_block = static_cast<uint32_t>(last / BLOCK_SIZE);
    lp->end_slot = static_cast<uint32_t>(last % BLOCK_SIZE);

    lp->base = ix.block_offset[lp->start_block];
    lp->buf.resize(ix.block_offset[lp->end_block + 1] - lp->base);
    if (pread(ix.fd, lp->buf.data(), lp->buf.size(), static_cast<off_t>(lp->base)) != static_cast<ssize_t>(lp->buf.size())) {
        die("incorrect index.bin read");
    }

    double n = ix.pid.size();
    lp->idf = std::log((n - e->ft + 0.5) / (e->ft + 0.5) + 1.0);

    lp->block = lp->start_block;
    decode_docs(*lp);
    lp->cur = lp->docs[lp->pos];
    return lp;
}

// Moves to the first posting with docID >= k and returns its docID, or END.
static uint32_t nextGEQ(ListCursor& lp, uint32_t k) {
    if (lp.cur >= k) {
        return lp.cur;
    }
    const uint32_t* ld = lp.ix->last_doc.data();
    // a list can end mid-block, so last_doc could belong to the next term -- we only check blocks before it
    if (lp.block < lp.end_block && ld[lp.block] < k) {  // skip blocks only when k is past this one, with galloping
        uint32_t lo = lp.block, step = 1, hi = lo + 1;
        while (hi < lp.end_block && ld[hi] < k) {
            lo = hi;
            step <<= 1;
            hi = lo + step;
        }
        hi = std::min(hi, lp.end_block);
        lp.block = static_cast<uint32_t>(std::lower_bound(ld + lo + 1, ld + hi, k) - ld);
    }
    if (lp.block != lp.decoded) {
        decode_docs(lp);
    }
    while (lp.pos <= lp.hi && lp.docs[lp.pos] < k) {
        ++lp.pos;
    }
    if (lp.pos <= lp.hi) {
        lp.cur = lp.docs[lp.pos];
    } else {
        lp.cur = END;
    }
    return lp.cur;
}

// BM25 score of the current posting
static double getScore(ListCursor& lp) {
    if (!lp.freqs_ready) {
        const uint8_t* p = block_ptr(lp) + lp.ix->doc_bytes[lp.block];
        for (int i = 0; i <= lp.hi; ++i) {
            lp.freqs[i] = vb_decode(p);
        }
        lp.freqs_ready = true;
    }
    double tf = lp.freqs[lp.pos];
    double K = K1 * ((1 - B) + B * lp.ix->doc_len[lp.cur] / lp.ix->avg_doc_len);
    return lp.idf * (K1 + 1) * tf / (K + tf);
}


// for debugging: prints ft and the first few postings of a term
static void debug_list(const Index& ix, const std::string& term) {
    auto lp = openList(ix, term);
    if (!lp) {
        std::printf("%s: not found\n", term.c_str());
        return;
    }
    std::printf("%s: ft %u\n", term.c_str(), lp->ft);
    uint32_t d = lp->cur;
    for (int i = 0; i < 5 && d != END; ++i) {
        std::printf("  docID %u score %.4f\n", d, getScore(*lp));
        d = nextGEQ(*lp, d + 1);
    }
}

// toy query impl for debugging: checks the list of every distinct query term
static void run_query(const Index& ix, const std::string& text) {
    std::vector<std::string> terms = tokenize(text);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

    for (const std::string& t : terms) {
        debug_list(ix, t);
    }
}

int main(int argc, char** argv) {
    Index ix;
    std::string root = ".";
    if (argc > 1) {
        root = argv[1];
    }
    ix.load(root + "/output");
    std::printf("Loaded %zu terms, %zu documents.\n", ix.lexicon.size(), ix.pid.size());

    std::string line;
    while (true) {
        std::printf("> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, line) || line.empty()) {
            break;
        }
        run_query(ix, line);
    }
    return 0;
}

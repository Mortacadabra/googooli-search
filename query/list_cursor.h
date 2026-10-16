#pragma once

// inverted list API: openList / nextGEQ / getScore over the compressed inverted lists

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "../config.h"
#include "../varbyte.h"
#include "index.h"

inline constexpr uint32_t END = UINT32_MAX;  // docID past the end of a list
inline constexpr double K1 = 0.9, B = 0.4;   // BM25 parameters

// Inverted list API
struct ListCursor {
    const Index* ix;
    std::vector<uint8_t> buf;  // blocks [start_block, end_block]
    uint64_t base;             // file offset of buf[0]
    uint32_t ft, start_block, start_slot, end_block, end_slot;
    double idf;
    double max_score;               // upper bound on this term's score in any doc

    uint32_t block, decoded = END;  // current block / block held in docs[]
    bool freqs_ready = false;
    int lo, hi, pos;                // this list's slots in the current block are [lo, hi]
    uint32_t docs[BLOCK_SIZE], freqs[BLOCK_SIZE];
    uint32_t cur = END;           
};

inline const uint8_t* block_ptr(const ListCursor& lp) {
    return lp.buf.data() + (lp.ix->block_offset[lp.block] - lp.base);
}

// docIDs are stored absolute at the start of a block or of a list, as gaps otherwise
inline void decode_docs(ListCursor& lp) {
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

inline std::unique_ptr<ListCursor> openList(const Index& ix, std::string_view term) {
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
    lp->max_score = lp->idf * (K1 + 1);

    lp->block = lp->start_block;
    decode_docs(*lp);
    lp->cur = lp->docs[lp->pos];
    return lp;
}

// Moves to the first posting with docID >= k and returns its docID, or END.
inline uint32_t nextGEQ(ListCursor& lp, uint32_t k) {
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
inline double getScore(ListCursor& lp) {
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

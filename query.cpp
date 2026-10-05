#include "common.h"

#include <algorithm>
#include <chrono>
#include <iostream>

using namespace idx;
#pragma GCC diagnostic ignored "-Wunused-parameter" 

struct Index {
    std::vector<LexiconEntry> lex;      // sorted by term
    BlockMeta meta;
    std::vector<uint64_t> blockOffset;  // block i = bytes [blockOffset[i], blockOffset[i+1])
    PageTable pages;                    // TODO: add BM25
    FILE* indexFile = nullptr;
};

Index loadIndex(const std::string& dir) {
    Index ix;
    ix.lex = readLexicon(dir + "/lexicon.bin");
    ix.meta = readMeta(dir + "/meta.bin");
    ix.pages = readPageTable(dir + "/pagetable.bin");
    ix.blockOffset.assign(1, 0);

    for (size_t b = 0; b < ix.meta.lastDocID.size(); ++b) {
        ix.blockOffset.push_back(ix.blockOffset.back() + ix.meta.docBytes[b] + ix.meta.freqBytes[b]);
    }

    ix.indexFile = std::fopen((dir + "/index.bin").c_str(), "rb");
    if (!ix.indexFile) {
        die("cannot open " + dir + "/index.bin");
    }
    return ix;
}

struct Result { double score; uint32_t docID; };

// State of one open inverted list
struct ListCursor {
    // TODO: term info (ft, start/end block and slot), the list's compressed bytes,
    // current block, decoded docIDs/freqs of that block, current position.
};

// Finds `term` in the lexicon and reads the compressed bytes of its
// blocks from index.bin
ListCursor* openList(const Index& ix, const std::string& term) { return nullptr; }  // TODO

void closeList(ListCursor* lp) { delete lp; }  // TODO: release anything else you add

// Moves forward to the first posting with docID >= k and returns it (MAXDID if none)
uint32_t nextGEQ(ListCursor* lp, uint32_t k) { return MAXDID; }  // TODO

// Frequency of the current posting
uint32_t getFreq(ListCursor* lp) { return 0; }  // TODO

double bm25(const Index& ix, uint32_t ft, uint32_t f, uint32_t docID) { return 0; }  // TODO

std::vector<Result> conjunctiveDAAT(const Index& ix, std::vector<ListCursor*>& lists, size_t k) {
    return {};  // TODO
}

std::vector<Result> disjunctiveDAAT(const Index& ix, std::vector<ListCursor*>& lists, size_t k) {
    return {};  // TODO
}

int main(int argc, char** argv) {
    if (argc != 2) {
        die("usage: query <index_dir>");
    }
    Index ix = loadIndex(argv[1]);
    std::fprintf(stderr, "loaded: %zu docs, %zu terms, %zu blocks\n", ix.pages.pid.size(),
                 ix.lex.size(), ix.meta.lastDocID.size());
    std::string line, mode;
    std::vector<std::string> terms;

    while (std::printf("query> "), std::fflush(stdout), std::getline(std::cin, line)) {
        tokenize(line, terms);
        std::sort(terms.begin(), terms.end());
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        if (terms.empty()){ 
            continue;
        }
        std::printf("mode (and/or) [and]> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, mode)) {
            break;
        }

        const bool isOr = (mode == "or");
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<ListCursor*> lists;
        bool missing = false;

        for (const std::string& t : terms) {
            ListCursor* lp = openList(ix, t);
            if (lp) {
                lists.push_back(lp);
            } else {
                missing = true;
            }
        }
        std::vector<Result> results;
        if (!lists.empty() && (isOr || !missing)) {
            results = isOr ? disjunctiveDAAT(ix, lists, 10) : conjunctiveDAAT(ix, lists, 10);
        }
        for (ListCursor* lp : lists) closeList(lp);

        for (size_t i = 0; i < results.size(); ++i)  // rank docID score
            std::printf("%zu %u %u %.6f\n", i + 1, results[i].docID,
                        ix.pages.pid[results[i].docID], results[i].score);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("(%zu results, %s, %.3f ms)\n", results.size(), isOr ? "OR" : "AND", ms);
    }
    return 0;
}

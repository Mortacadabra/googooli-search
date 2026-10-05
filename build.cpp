// build.cpp -- naive in-memory index builder (trial version).
//
// Reads the first N passages of collection.tsv, builds all inverted lists in
// an std::map in memory, and writes the final on-disk format:
//   index.bin      all lists in lexicon order, cut into global blocks of
//                  BLOCK_SIZE postings: [docIDs varbyte][freqs varbyte]
//   meta.bin       per-block lastDocID, docBytes, freqBytes
//   lexicon.bin    term -> (ft, startBlock, startSlot), sorted by term
//   pagetable.bin  N, pid[N], docLen[N], docOffset[N]
// The index stores raw frequencies only; all BM25 scoring is done at query time.
// This builder only exists to pin down the file formats. The real pipeline
// (parser -> sorted runs -> d-way merge) must produce byte-identical files.
//
// Usage: ./build <collection.tsv> <out_dir> [--limit N] [--ref ref.txt]
//   --limit N    index the first N passages (default 10000, 0 = all)
//   --ref FILE   also write the in-memory index as ASCII (same format as
//                `query --dump`), to diff against what was written to disk

#include "common.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <sys/stat.h>  // mkdir
#include <unordered_map>

using namespace idx;

namespace {

using Postings = std::vector<std::pair<uint32_t, uint32_t>>;  // (docID, freq)

void usage() {
    std::fprintf(stderr, "usage: build <collection.tsv> <out_dir> [--limit N] [--ref ref.txt]\n");
    std::exit(2);
}

// Writes a byte buffer to a file, aborting on error.
void writeFile(const std::string& path, const std::vector<uint8_t>& buf) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) die("cannot create " + path);
    if (!buf.empty() && std::fwrite(buf.data(), 1, buf.size(), f) != buf.size())
        die("write failed on " + path);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) usage();
    const std::string inPath = argv[1], outDir = argv[2];
    uint64_t limit = 10000;
    std::string refPath;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--limit" && i + 1 < argc) limit = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--ref" && i + 1 < argc) refPath = argv[++i];
        else usage();
    }
    mkdir(outDir.c_str(), 0755);  // ok if it already exists

    const auto t0 = std::chrono::steady_clock::now();

    // ---- Pass over the collection: tokenize and collect postings ----------
    std::ifstream in(inPath, std::ios::binary);
    if (!in) die("cannot open " + inPath);

    std::map<std::string, Postings> index;   // sorted by term (byte order)
    std::vector<uint32_t> pids;
    std::vector<uint16_t> docLen;
    std::vector<uint64_t> docOffset;
    uint64_t totalTokens = 0;                // statistics only, not stored
    uint64_t lineStart = 0;                  // byte offset of current line
    std::string line;
    std::vector<std::string> tokens;
    std::unordered_map<std::string, uint32_t> tf;  // per-document term counts

    while ((limit == 0 || docLen.size() < limit) && std::getline(in, line)) {
        const uint32_t docID = static_cast<uint32_t>(docLen.size());
        const uint64_t thisLine = lineStart;
        lineStart += line.size() + 1;  // +1 for the '\n' consumed by getline
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Line format: pid \t passage. docID is the 0-based line number; the
        // pid (the passage's name) goes into the page table.
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) die("line " + std::to_string(docID) + ": no pid");
        char* endp = nullptr;
        unsigned long long pid = std::strtoull(line.c_str(), &endp, 10);
        if (endp != line.c_str() + tab || pid > UINT32_MAX)
            die("line " + std::to_string(docID) + ": bad pid '" + line.substr(0, tab) + "'");
        pids.push_back(static_cast<uint32_t>(pid));

        tokenize(std::string_view(line).substr(tab + 1), tokens);
        if (tokens.size() > UINT16_MAX)
            die("doc " + std::to_string(docID) + " has too many tokens for u16 docLen");
        docLen.push_back(static_cast<uint16_t>(tokens.size()));
        docOffset.push_back(thisLine);
        totalTokens += tokens.size();

        tf.clear();
        for (const std::string& t : tokens) ++tf[t];
        // Documents are processed in docID order, so every list stays sorted.
        for (const auto& [term, f] : tf) index[term].emplace_back(docID, f);
    }

    const uint32_t N = static_cast<uint32_t>(docLen.size());
    const double avgdl = N ? static_cast<double>(totalTokens) / N : 0.0;

    // ---- pagetable.bin ----------------------------------------------------
    {
        std::vector<uint8_t> buf;
        buf.reserve(4 + size_t(N) * 14);
        putRaw<uint32_t>(buf, N);
        for (uint32_t p : pids) putRaw<uint32_t>(buf, p);
        for (uint16_t l : docLen) putRaw<uint16_t>(buf, l);
        for (uint64_t o : docOffset) putRaw<uint64_t>(buf, o);
        writeFile(outDir + "/pagetable.bin", buf);
    }

    // ---- index.bin + meta.bin + lexicon.bin (+ optional ASCII reference) --
    FILE* indexOut = std::fopen((outDir + "/index.bin").c_str(), "wb");
    if (!indexOut) die("cannot create " + outDir + "/index.bin");
    FILE* refOut = nullptr;
    if (!refPath.empty() && !(refOut = std::fopen(refPath.c_str(), "w")))
        die("cannot create " + refPath);

    std::vector<uint8_t> lex;
    putRaw<uint32_t>(lex, static_cast<uint32_t>(index.size()));
    BlockWriter writer(indexOut);

    for (const auto& [term, postings] : index) {
        const uint32_t ft = static_cast<uint32_t>(postings.size());
        // Append the list to the global posting stream.
        uint32_t startBlock;
        uint8_t startSlot;
        writer.beginList(startBlock, startSlot);
        for (const auto& [d, f] : postings) writer.add(d, f);

        // Lexicon record: u8 len, term, u32 ft, u32 startBlock, u8 startSlot.
        lex.push_back(static_cast<uint8_t>(term.size()));
        lex.insert(lex.end(), term.begin(), term.end());
        putRaw<uint32_t>(lex, ft);
        putRaw<uint32_t>(lex, startBlock);
        lex.push_back(startSlot);

        if (refOut) {
            printListHeader(refOut, term, ft);
            for (const auto& [d, f] : postings) std::fprintf(refOut, " %u:%u", d, f);
            std::fputc('\n', refOut);
        }
    }
    writer.finish();
    std::fclose(indexOut);
    if (refOut) std::fclose(refOut);
    writer.writeMeta(outDir + "/meta.bin");
    writeFile(outDir + "/lexicon.bin", lex);

    // ---- Statistics -------------------------------------------------------
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("docs indexed:    %u\n", N);
    std::printf("distinct terms:  %zu\n", index.size());
    std::printf("total postings:  %llu\n", static_cast<unsigned long long>(writer.totalPostings()));
    std::printf("blocks:          %u\n", writer.numBlocks());
    std::printf("total tokens:    %llu (avgdl %.3f)\n",
                static_cast<unsigned long long>(totalTokens), avgdl);
    std::printf("elapsed:         %.2f s\n", secs);
    return 0;
}

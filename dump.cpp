#include "common.h"

using namespace idx;

int main(int argc, char** argv) {
    if (argc < 2) die("usage: dump <index_dir> [term | --pagetable K]");
    const std::string dir = argv[1];
    const std::string arg = argc > 2 ? argv[2] : "";

    if (arg == "--pagetable") {
        const PageTable pt = readPageTable(dir + "/pagetable.bin");
        const size_t k = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 10;
        std::printf("N=%zu\ndocID pid len offset\n", pt.pid.size());
        for (size_t d = 0; d < pt.pid.size() && d < k; ++d)
            std::printf("%zu %u %u %llu\n", d, pt.pid[d], pt.docLen[d], (unsigned long long)pt.docOffset[d]);
        return 0;
    }

    const std::vector<LexiconEntry> lex = readLexicon(dir + "/lexicon.bin");
    const BlockMeta meta = readMeta(dir + "/meta.bin");
    FILE* f = std::fopen((dir + "/index.bin").c_str(), "rb");
    if (!f) die("cannot open " + dir + "/index.bin");

    // Walk the posting stream block by block; lists follow each other in lexicon order.
    size_t t = 0;            // current term
    uint32_t left = 0;       // postings of term t still to come (0 = next posting starts a list)
    uint32_t prev = 0;       // previous docID
    std::vector<uint8_t> buf;
    for (size_t b = 0; b < meta.lastDocID.size(); ++b) {
        buf.resize(size_t(meta.docBytes[b]) + meta.freqBytes[b]);
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) die("index.bin too short");
        const uint8_t* p = buf.data();
        const uint8_t* docEnd = p + meta.docBytes[b];
        const uint8_t* q = docEnd;  // freq section
        for (size_t j = 0; p < docEnd; ++j) {
            const bool listStart = (left == 0);
            if (listStart) {
                if (j > 0 || b > 0) ++t;  // the previous list ended
                if (t >= lex.size() || lex[t].startBlock != b || lex[t].startSlot != j)
                    die("lexicon does not match the posting stream at block " + std::to_string(b));
                left = lex[t].ft;
                if (arg.empty() || arg == lex[t].term) std::printf("%s ft=%u |", lex[t].term.c_str(), left);
            }
            const uint32_t raw = vbDecode(p);
            prev = (j == 0 || listStart) ? raw : prev + raw;  // absolute or gap
            const uint32_t freq = vbDecode(q);
            const bool show = arg.empty() || arg == lex[t].term;
            if (show) std::printf(" %u:%u", prev, freq);
            if (--left == 0 && show) std::putchar('\n');
        }
        if (p != docEnd || q != docEnd + meta.freqBytes[b] || prev != meta.lastDocID[b])
            die("block " + std::to_string(b) + " does not match meta.bin");
    }
    if (left != 0 || (!lex.empty() && t + 1 != lex.size()) || std::fgetc(f) != EOF)
        die("index.bin and lexicon.bin disagree");
    std::fclose(f);
    return 0;
}

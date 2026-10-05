#include "common.h"

#include <chrono>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unordered_map>

using namespace idx;

// Appends postings to index.bin, cutting the stream every BLOCK_SIZE postings.
class BlockWriter {
public:
    BlockWriter(FILE* out, BlockMeta& meta) : out_(out), meta_(meta) {}

    // Starts a new list; returns where its first posting is at
    void beginList(uint32_t& startBlock, uint8_t& startSlot) {
        startBlock = static_cast<uint32_t>(meta_.lastDocID.size());
        startSlot = static_cast<uint8_t>(n_);
        listStart_ = true;
    }

    void add(uint32_t doc, uint32_t freq) {
        vbEncode(listStart_ || n_ == 0 ? doc : doc - prev_, docs_);  // if not first doc, we write difference
        vbEncode(freq, freqs_);
        prev_ = doc;
        listStart_ = false;
        if (++n_ == BLOCK_SIZE) flush();
    }

    void finish() { if (n_ > 0) flush(); }

private:
    void flush() {
        if (std::fwrite(docs_.data(), 1, docs_.size(), out_) != docs_.size() ||
            std::fwrite(freqs_.data(), 1, freqs_.size(), out_) != freqs_.size())
            die("write failed on index.bin");
        meta_.lastDocID.push_back(prev_);
        meta_.docBytes.push_back(static_cast<uint16_t>(docs_.size())); 
        meta_.freqBytes.push_back(static_cast<uint16_t>(freqs_.size()));
        docs_.clear(); freqs_.clear(); n_ = 0;
    }
    FILE* out_;
    BlockMeta& meta_;
    std::vector<uint8_t> docs_, freqs_;  // encoded sections of the open block
    uint32_t n_ = 0, prev_ = 0;
    bool listStart_ = false;
};

int main(int argc, char** argv) {
    if (argc < 3) {
        die("usage: build <collection.tsv> <out_dir> [--limit N] [--ref ref.txt]");
    }
    const std::string out = argv[2];
    uint64_t limit = 10000;
    std::string refPath;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (std::string(argv[i]) == "--limit") limit = std::strtoull(argv[i + 1], nullptr, 10);
        else if (std::string(argv[i]) == "--ref") refPath = argv[i + 1];
        else die(std::string("unknown option ") + argv[i]);
    }
    mkdir(out.c_str(), 0755);
    const auto t0 = std::chrono::steady_clock::now();

    // parse and collect (docID, freq) postings.
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        die(std::string("cannot open ") + argv[1]);
    }

    std::map<std::string, std::vector<std::pair<uint32_t, uint32_t>>> index; 
    PageTable pt;
    uint64_t offset = 0;
    std::string line;
    std::vector<std::string> tokens;
    std::unordered_map<std::string, uint32_t> tf;

    while ((limit == 0 || pt.pid.size() < limit) && std::getline(in, line)) {
        const uint32_t docID = static_cast<uint32_t>(pt.pid.size());
        pt.docOffset.push_back(offset);
        offset += line.size() + 1;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        const size_t tab = line.find('\t');  // each line is pid \t passage
        char* end = nullptr;
        const unsigned long long pid = std::strtoull(line.c_str(), &end, 10);
        if (tab == std::string::npos || end != line.c_str() + tab || pid > UINT32_MAX) {
            die("bad pid on line " + std::to_string(docID));
        }

        tokenize(std::string_view(line).substr(tab + 1), tokens);
        if (tokens.size() > UINT16_MAX) {
            die("doc " + std::to_string(docID) + " too long");
        }

        pt.pid.push_back(static_cast<uint32_t>(pid));
        pt.docLen.push_back(static_cast<uint16_t>(tokens.size()));
        tf.clear();

        for (const std::string& t : tokens) ++tf[t];
        for (const auto& [term, f] : tf) index[term].emplace_back(docID, f);
    }

    // write index.bin , lexicon, meta, page table, optional ref in ASCII for debugging
    FILE* idxFile = std::fopen((out + "/index.bin").c_str(), "wb");
    if (!idxFile) die("cannot create " + out + "/index.bin");

    FILE* ref = refPath.empty() ? nullptr : std::fopen(refPath.c_str(), "w");
    BlockMeta meta;
    BlockWriter writer(idxFile, meta);
    std::vector<LexiconEntry> lex;
    uint64_t postings = 0;

    for (const auto& [term, list] : index) {
        LexiconEntry e{term, static_cast<uint32_t>(list.size()), 0, 0};
        writer.beginList(e.startBlock, e.startSlot);
        for (const auto& [d, f] : list) writer.add(d, f);
        lex.push_back(e);
        postings += list.size();
        if (ref) {
            std::fprintf(ref, "%s ft=%u |", term.c_str(), e.ft);
            for (const auto& [d, f] : list) std::fprintf(ref, " %u:%u", d, f);
            std::fputc('\n', ref);
        }
    }
    writer.finish();
    std::fclose(idxFile);

    if (ref) std::fclose(ref);
    writeLexicon(out + "/lexicon.bin", lex);
    writeMeta(out + "/meta.bin", meta);
    writePageTable(out + "/pagetable.bin", pt);

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("docs %zu, terms %zu, postings %llu, blocks %zu, %.2f s (file sizes: ls -l %s)\n",
                pt.pid.size(), lex.size(), static_cast<unsigned long long>(postings),
                meta.lastDocID.size(), secs, out.c_str());
    return 0;
}

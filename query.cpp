// query.cpp -- query processor and index inspection tools (trial version).
//
// Modes:
//   ./query <index_dir>                     REPL: conjunctive DAAT + BM25, top 10
//   ./query <index_dir> --dump [term]       print lists as ASCII (same format as build --ref)
//   ./query <index_dir> --dump --pagetable K   print N, avgdl and the first K page-table rows
//   ./query --brute <tsv> <N> "<query>"     reference scorer: scans the first N
//                                           passages directly, no index
//   ./query [<index_dir>] --selftest        varbyte + tokenizer checks; with an
//                                           index dir also the index checks

#include "common.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <queue>
#include <random>
#include <unistd.h>

using namespace idx;

namespace {

constexpr size_t TOP_K = 10;

struct Result {
    double   score;
    uint32_t docID;
};

// Result order: higher score first, ties broken by smaller docID.
bool better(const Result& a, const Result& b) {
    return a.score != b.score ? a.score > b.score : a.docID < b.docID;
}

// Keeps the TOP_K best results. The heap top is the worst result kept, so a
// new candidate only has to beat the top to get in.
class TopK {
public:
    void push(const Result& r) {
        if (heap_.size() < TOP_K) heap_.push(r);
        else if (better(r, heap_.top())) { heap_.pop(); heap_.push(r); }
    }
    std::vector<Result> sorted() {
        std::vector<Result> out;
        while (!heap_.empty()) { out.push_back(heap_.top()); heap_.pop(); }
        std::sort(out.begin(), out.end(), better);
        return out;
    }
private:
    struct WorseOnTop { bool operator()(const Result& a, const Result& b) const { return better(a, b); } };
    std::priority_queue<Result, std::vector<Result>, WorseOnTop> heap_;
};

// Prints one line per result: rank docID pid score.
void printResults(const std::vector<Result>& results, const std::vector<uint32_t>& pid) {
    for (size_t i = 0; i < results.size(); ++i)
        std::printf("%zu %u %u %.6f\n", i + 1, results[i].docID, pid[results[i].docID],
                    results[i].score);
}

// Tokenizes a query and removes duplicate terms.
std::vector<std::string> queryTerms(const std::string& q) {
    std::vector<std::string> terms;
    tokenize(q, terms);
    std::sort(terms.begin(), terms.end());
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    return terms;
}

// ---------------------------------------------------------------------------
// decodeAll: independent reference decoder for index.bin
// ---------------------------------------------------------------------------
// Reads index.bin sequentially and decodes every block straight from the
// format spec, without ListCursor's skipping logic. Each section is decoded
// by reading varbyte values until its byte size (from meta.bin) is used up.
// Result: absolute docID and freq for every global slot, so the list of term
// i is slots [listStart(i), listStart(i) + ft[i]).
struct DecodedStream {
    std::vector<uint32_t> docs, freqs;
};

DecodedStream decodeAll(const IndexReader& ix) {
    const std::vector<uint8_t> buf = readFile(ix.indexPath);
    DecodedStream s;
    s.docs.reserve(ix.totalPostings);
    s.freqs.reserve(ix.totalPostings);

    // Mark the slots where a list begins: their docID is stored absolute.
    std::vector<bool> listFirst(ix.totalPostings, false);
    for (uint32_t i = 0; i < ix.numTerms; ++i) listFirst[ix.listStart(i)] = true;

    uint64_t off = 0;
    for (uint32_t b = 0; b < ix.numBlocks; ++b) {
        const uint8_t* p = buf.data() + off;
        const uint8_t* docEnd = p + ix.docBytes[b];
        const uint8_t* freqEnd = docEnd + ix.freqBytes[b];
        if (uint64_t(freqEnd - buf.data()) > buf.size()) die("decodeAll: block past end of file");
        const size_t first = s.docs.size();  // global slot of position 0
        uint32_t prev = 0;
        while (p < docEnd) {
            const size_t slot = s.docs.size();
            const uint32_t raw = vbDecode(p);
            prev = (slot == first || listFirst[slot]) ? raw : prev + raw;
            s.docs.push_back(prev);
        }
        const size_t n = s.docs.size() - first;
        while (p < freqEnd) s.freqs.push_back(vbDecode(p));
        if (p != freqEnd || s.freqs.size() != s.docs.size())
            die("decodeAll: block " + std::to_string(b) + " sections do not match");
        if (n != ix.blockLen(b))
            die("decodeAll: block " + std::to_string(b) + " has " + std::to_string(n) + " postings");
        if (s.docs.back() != ix.lastDocID[b])
            die("decodeAll: lastDocID mismatch in block " + std::to_string(b));
        off = freqEnd - buf.data();
    }
    if (off != buf.size()) die("decodeAll: trailing bytes in index.bin");
    return s;
}

// ---------------------------------------------------------------------------
// Conjunctive DAAT (lecture algorithm): only documents that contain every
// query term are scored. lp must be sorted by ft ascending.
// ---------------------------------------------------------------------------
std::vector<Result> conjunctiveDAAT(const IndexReader& ix, std::vector<ListCursor>& lp) {
    TopK top;
    const size_t num = lp.size();
    uint32_t did = 0;
    while (true) {
        // Candidate from the shortest list.
        did = lp[0].nextGEQ(did);
        if (did == MAXDID) break;
        // Check whether the other lists contain the same docID.
        uint32_t d = did;
        for (size_t i = 1; i < num && (d = lp[i].nextGEQ(did)) == did; ++i) {}
        if (d == MAXDID) break;              // some list is exhausted: no more matches
        if (d > did) { did = d; continue; }  // not in the intersection
        // docID is in all lists: sum the BM25 contributions.
        double s = 0.0;
        for (size_t i = 0; i < num; ++i)
            s += bm25Term(lp[i].idf, lp[i].getFreq(), ix.docLen[did], ix.avgdl);
        top.push({s, did});
        ++did;  // did < MAXDID here, so this cannot overflow
    }
    return top.sorted();
}

int runRepl(const IndexReader& ix) {
    const bool interactive = isatty(STDIN_FILENO);
    std::fprintf(stderr, "loaded index: %u docs, %u terms, %u blocks\n", ix.N, ix.numTerms,
                 ix.numBlocks);
    std::string line;
    while (true) {
        if (interactive) { std::printf("> "); std::fflush(stdout); }
        if (!std::getline(std::cin, line)) break;
        const auto t0 = std::chrono::steady_clock::now();

        std::vector<std::string> terms = queryTerms(line);
        if (terms.empty()) continue;
        std::vector<ListCursor> lp;
        std::string missing;
        for (const std::string& t : terms) {
            lp.push_back(ix.openList(t));
            if (lp.back().ft == 0) missing = t;
        }
        std::vector<Result> results;
        if (!missing.empty()) {
            std::printf("term '%s' not in index: no results\n", missing.c_str());
        } else {
            // Shortest list first; ties by term order to keep summation order
            // (and hence exact scores) deterministic and equal to --brute.
            std::sort(lp.begin(), lp.end(), [](const ListCursor& a, const ListCursor& b) {
                return a.ft != b.ft ? a.ft < b.ft : a.termIndex < b.termIndex;
            });
            results = conjunctiveDAAT(ix, lp);
            printResults(results, ix.pid);
        }
        for (ListCursor& c : lp) IndexReader::closeList(c);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("(%zu results, %.3f ms)\n", results.size(), ms);
        std::fflush(stdout);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// --dump
// ---------------------------------------------------------------------------
// Lists are decoded through ListCursor, so the round-trip diff against
// `build --ref` exercises the same code the query processor uses.
void dumpList(const IndexReader& ix, uint32_t i) {
    ListCursor c = ix.openList(i);
    printListHeader(stdout, ix.term(i), c.ft);
    for (uint32_t d = c.nextGEQ(0); d != MAXDID; d = c.nextGEQ(d + 1))
        std::printf(" %u:%u", d, c.getFreq());
    std::putchar('\n');
}

int runDump(const IndexReader& ix, const char* term) {
    if (term) {
        int64_t i = ix.lookup(term);
        if (i < 0) { std::fprintf(stderr, "term '%s' not in lexicon\n", term); return 1; }
        dumpList(ix, static_cast<uint32_t>(i));
    } else {
        for (uint32_t i = 0; i < ix.numTerms; ++i) dumpList(ix, i);
    }
    return 0;
}

int runDumpPageTable(const IndexReader& ix, uint64_t k) {
    std::printf("N=%u avgdl=%.6f\n", ix.N, ix.avgdl);
    std::printf("docID pid len offset\n");
    for (uint32_t d = 0; d < ix.N && d < k; ++d)
        std::printf("%u %u %u %llu\n", d, ix.pid[d], ix.docLen[d],
                    static_cast<unsigned long long>(ix.docOffset[d]));
    return 0;
}

// ---------------------------------------------------------------------------
// --brute: score the first N passages directly from the collection
// ---------------------------------------------------------------------------
int runBrute(const std::string& tsv, uint64_t limit, const std::string& query) {
    std::vector<std::string> terms = queryTerms(query);
    const size_t m = terms.size();
    std::ifstream in(tsv, std::ios::binary);
    if (!in) die("cannot open " + tsv);

    std::vector<uint32_t> ftq(m, 0);
    std::vector<uint32_t> docLen, pids;
    struct Cand { uint32_t docID; std::vector<uint32_t> f; };
    std::vector<Cand> cands;                // docs containing every query term
    uint64_t totalTokens = 0;
    std::string line;
    std::vector<std::string> tokens;
    while (docLen.size() < limit && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        tokenize(tab == std::string::npos ? std::string_view() : std::string_view(line).substr(tab + 1), tokens);
        const uint32_t docID = static_cast<uint32_t>(docLen.size());
        pids.push_back(static_cast<uint32_t>(std::strtoul(line.c_str(), nullptr, 10)));
        docLen.push_back(static_cast<uint32_t>(tokens.size()));
        totalTokens += tokens.size();
        std::vector<uint32_t> f(m, 0);
        for (const std::string& t : tokens)
            for (size_t j = 0; j < m; ++j) if (t == terms[j]) ++f[j];
        bool all = m > 0;
        for (size_t j = 0; j < m; ++j) {
            if (f[j]) ++ftq[j]; else all = false;
        }
        if (all) cands.push_back({docID, f});
    }
    const uint32_t N = static_cast<uint32_t>(docLen.size());
    const double avgdl = N ? static_cast<double>(totalTokens) / N : 0.0;

    // Sum in the same term order as the index-based processor (ft asc, then
    // term) so that floating-point sums are bit-identical.
    std::vector<size_t> order(m);
    for (size_t j = 0; j < m; ++j) order[j] = j;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ftq[a] != ftq[b] ? ftq[a] < ftq[b] : terms[a] < terms[b];
    });
    TopK top;
    for (const Cand& c : cands) {
        double s = 0.0;
        for (size_t j : order) s += bm25Term(bm25Idf(N, ftq[j]), c.f[j], docLen[c.docID], avgdl);
        top.push({s, c.docID});
    }
    printResults(top.sorted(), pids);
    return 0;
}

// ---------------------------------------------------------------------------
// --selftest
// ---------------------------------------------------------------------------
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) { ++failures; std::printf("  FAIL: %s\n", what.c_str()); }
}

void testVarbyte() {
    std::printf("varbyte round-trip...\n");
    std::vector<uint32_t> vals = {0, 1, 127, 128, 16383, 16384, 1u << 21, UINT32_MAX};
    const size_t expectedLen[] = {1, 1, 1, 2, 2, 3, 4, 5};
    for (size_t i = 0; i < vals.size(); ++i) {
        std::vector<uint8_t> buf;
        vbEncode(vals[i], buf);
        check(buf.size() == expectedLen[i], "length of " + std::to_string(vals[i]));
    }
    std::mt19937 rng(12345);
    for (int i = 0; i < 100000; ++i) {
        uint32_t v = rng() >> (rng() % 32);  // spread across all byte lengths
        vals.push_back(v);
    }
    std::vector<uint8_t> buf;
    for (uint32_t v : vals) vbEncode(v, buf);
    const uint8_t* p = buf.data();
    bool ok = true;
    for (uint32_t v : vals) ok &= (vbDecode(p) == v);
    check(ok && p == buf.data() + buf.size(), "decode(encode(x)) == x for all values");
}

void testTokenize() {
    std::printf("tokenize...\n");
    auto tok = [](const std::string& s) {
        std::vector<std::string> out;
        tokenize(s, out);
        std::string joined;
        for (const auto& t : out) joined += (joined.empty() ? "" : " ") + t;
        return joined;
    };
    check(tok("Hello, World! 2024") == "hello world 2024", "Hello, World! 2024");
    check(tok("don't") == "don t", "don't");
    check(tok("caf\xC3\xA9") == "caf", "cafe with UTF-8 e-acute");
    check(tok("\xFF\xFE" "abc\x80\xC3" "def\xE2") == "abc def", "invalid UTF-8");
    check(tok("") == "" && tok(" \t\n,.") == "", "empty / separators only");
    std::vector<std::string> out;
    tokenize(std::string(100, 'a') + " b", out);
    check(out.size() == 2 && out[0] == std::string(64, 'a') && out[1] == "b", "truncation to 64");
}

// Structural checks on the whole index via the independent decoder.
// (IndexReader::open already checked numBlocks == ceil(totalPostings / 64),
// blockOffset[numBlocks] == size of index.bin, and list contiguity.)
void testStream(const IndexReader& ix, const DecodedStream& s) {
    std::printf("index structure (%u blocks, %llu postings)...\n", ix.numBlocks,
                static_cast<unsigned long long>(ix.totalPostings));
    check(s.docs.size() == ix.totalPostings, "decoded postings == sum of ft");
    bool ok = true;
    for (uint32_t i = 0; i < ix.numTerms; ++i) {
        const uint64_t lo = ix.listStart(i), hi = lo + ix.ft[i];
        for (uint64_t k = lo + 1; k < hi; ++k) ok &= s.docs[k] > s.docs[k - 1];
        for (uint64_t k = lo; k < hi; ++k) ok &= s.docs[k] < ix.N && s.freqs[k] >= 1;
    }
    check(ok, "every list strictly increasing, docIDs < N, freqs >= 1");
}

// Every list, fully traversed through ListCursor, equals decodeAll's slice.
void testEveryList(const IndexReader& ix, const DecodedStream& s) {
    std::printf("full traversal of all %u lists...\n", ix.numTerms);
    bool ok = true;
    for (uint32_t i = 0; i < ix.numTerms && ok; ++i) {
        ListCursor c = ix.openList(i);
        uint64_t k = ix.listStart(i), end = k + ix.ft[i];
        for (uint32_t d = c.nextGEQ(0); d != MAXDID; d = c.nextGEQ(d + 1), ++k)
            ok &= (k < end && d == s.docs[k] && c.getFreq() == s.freqs[k]);
        ok &= (k == end);
        if (!ok) check(false, "traversal of '" + std::string(ix.term(i)) + "'");
    }
}

// Checks nextGEQ/getFreq of one list against decodeAll, on a fresh cursor per
// target and on one reused cursor with increasing targets.
void checkList(const IndexReader& ix, const DecodedStream& s, uint32_t t, const char* label,
               size_t nRandom, std::mt19937& rng) {
    const uint64_t first = ix.listStart(t), last = first + ix.ft[t] - 1;
    std::printf("  %-34s '%.*s' ft=%u  block %llu:%llu -> %llu:%llu\n", label,
                int(ix.term(t).size()), ix.term(t).data(), ix.ft[t],
                (unsigned long long)(first / BLOCK_SIZE), (unsigned long long)(first % BLOCK_SIZE),
                (unsigned long long)(last / BLOCK_SIZE), (unsigned long long)(last % BLOCK_SIZE));
    const std::vector<uint32_t> docs(s.docs.begin() + first, s.docs.begin() + last + 1);
    const std::vector<uint32_t> freqs(s.freqs.begin() + first, s.freqs.begin() + last + 1);

    // Targets: around every posting for short lists, random for long ones,
    // plus the ends (0, past the last docID, MAXDID).
    std::vector<uint32_t> targets = {0, docs.front(), docs.back(), docs.back() + 1,
                                     MAXDID - 1, MAXDID};
    if (nRandom == 0) {
        for (uint32_t d : docs) { targets.push_back(d); targets.push_back(d + 1);
                                  if (d > 0) targets.push_back(d - 1); }
    } else {
        std::uniform_int_distribution<uint32_t> dist(0, docs.back() + 10);
        for (size_t i = 0; i < nRandom; ++i) targets.push_back(dist(rng));
    }
    auto expect = [&](uint32_t k) {
        return size_t(std::lower_bound(docs.begin(), docs.end(), k) - docs.begin());
    };

    bool ok = true;
    for (uint32_t k : targets) {  // fresh cursor per target
        ListCursor c = ix.openList(t);
        const size_t e = expect(k);
        const uint32_t got = c.nextGEQ(k);
        ok &= (e == docs.size()) ? got == MAXDID : (got == docs[e] && c.getFreq() == freqs[e]);
    }
    check(ok, std::string(label) + ": fresh cursor");

    std::sort(targets.begin(), targets.end());
    ok = true;
    ListCursor c = ix.openList(t);  // reused cursor, increasing targets
    for (size_t i = 0; i < targets.size(); ++i) {
        const size_t e = expect(targets[i]);
        const uint32_t got = c.nextGEQ(targets[i]);
        if (e == docs.size()) { ok &= (got == MAXDID); continue; }
        ok &= (got == docs[e]);
        if (i % 3 == 0) ok &= (c.getFreq() == freqs[e]);  // exercise lazy freq decoding
    }
    ok &= (c.nextGEQ(0) == MAXDID);  // stays exhausted
    check(ok, std::string(label) + ": reused cursor");
}

// nextGEQ boundary cases of PLAN.md test 4, with terms picked from the lexicon.
void testBoundaries(const IndexReader& ix, const DecodedStream& s) {
    std::printf("nextGEQ boundary cases:\n");
    std::mt19937 rng(42);
    auto startSlot = [&](uint32_t i) { return ix.listStart(i) % BLOCK_SIZE; };
    auto endSlot = [&](uint32_t i) { return (ix.listStart(i) + ix.ft[i] - 1) % BLOCK_SIZE; };
    auto blocksSpanned = [&](uint32_t i) {
        return (ix.listStart(i) + ix.ft[i] - 1) / BLOCK_SIZE - ix.listStart(i) / BLOCK_SIZE + 1;
    };
    // Runs the check on the first term satisfying pred, or reports a skip.
    auto pick = [&](const char* label, auto pred, size_t nRandom = 0) {
        for (uint32_t i = 0; i < ix.numTerms; ++i)
            if (pred(i)) { checkList(ix, s, i, label, nRandom, rng); return; }
        std::printf("  %-34s SKIPPED: no such term in this index\n", label);
    };

    pick("ft=1, strictly inside a block", [&](uint32_t i) {
        return ix.ft[i] == 1 && startSlot(i) > 0 && startSlot(i) < BLOCK_SIZE - 1; });
    for (uint32_t f : {63u, 64u, 65u, 129u}) {
        std::string any = "ft=" + std::to_string(f);
        std::string aligned = "ft=" + std::to_string(f) + ", starting at slot 0";
        pick(any.c_str(), [&](uint32_t i) { return ix.ft[i] == f; });
        pick(aligned.c_str(), [&](uint32_t i) { return ix.ft[i] == f && startSlot(i) == 0; });
    }
    pick("starts and ends mid-block, 3+ blocks", [&](uint32_t i) {
        return startSlot(i) > 0 && endSlot(i) < BLOCK_SIZE - 1 && blocksSpanned(i) >= 3; });
    pick("starts at slot 0, 2+ blocks", [&](uint32_t i) {
        return startSlot(i) == 0 && blocksSpanned(i) >= 2; });
    pick("ends at slot 63, 2+ blocks", [&](uint32_t i) {
        return endSlot(i) == BLOCK_SIZE - 1 && blocksSpanned(i) >= 2; });
    int64_t the = ix.lookup("the");
    if (the >= 0) checkList(ix, s, static_cast<uint32_t>(the), "long list, 1000 random targets", 1000, rng);
    else std::printf("  long list: SKIPPED ('the' not in index)\n");
}

int runSelftest(const char* indexDir) {
    testVarbyte();
    testTokenize();
    if (indexDir) {
        IndexReader ix;
        ix.open(indexDir);
        const DecodedStream s = decodeAll(ix);
        testStream(ix, s);
        testEveryList(ix, s);
        testBoundaries(ix, s);
    } else {
        std::printf("(no index dir given: skipping index tests)\n");
    }
    std::printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}

void usage() {
    std::fprintf(stderr,
                 "usage: query <index_dir>\n"
                 "       query <index_dir> --dump [term]\n"
                 "       query <index_dir> --dump --pagetable K\n"
                 "       query --brute <collection.tsv> <N> \"<query>\"\n"
                 "       query [<index_dir>] --selftest\n");
    std::exit(2);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) usage();
    const std::string a1 = argv[1];
    if (a1 == "--selftest") return runSelftest(nullptr);
    if (a1 == "--brute") {
        if (argc != 5) usage();
        return runBrute(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4]);
    }
    if (argc >= 3 && std::string(argv[2]) == "--selftest") return runSelftest(argv[1]);

    IndexReader ix;
    ix.open(a1);
    if (argc == 2) return runRepl(ix);
    if (std::string(argv[2]) != "--dump") usage();
    if (argc == 5 && std::string(argv[3]) == "--pagetable")
        return runDumpPageTable(ix, std::strtoull(argv[4], nullptr, 10));
    if (argc <= 4) return runDump(ix, argc == 4 ? argv[3] : nullptr);
    usage();
}

// Build:  make -C indexer
// Run:    ./build_runs [project_root]      (default ".")
//         reads <project_root>/data/collection.tsv, writes <project_root>/runs and /output
//         output/pagetable.bin (final format, little-endian), written directly by this program:
//             u32 N | u32 pid[N] | u16 docLen[N] | u64 docOffset[N]
//         docOffset = byte offset of the document's line in the uncompressed collection.tsv

#include "dataset_reader.h"
#include "tokenizer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// PROJECT_ROOT, DATASET_PATH, RUNS_DIR, OUTPUT_DIR are set in main() from argv.
static std::string PROJECT_ROOT = ".";
static std::string DATASET_PATH;
static std::string RUNS_DIR;
static std::string OUTPUT_DIR;

// Benchmark settings
static const int64_t MAX_DOCS = -1;  // -1 = None (all documents)

// Flush once roughly this many postings are in memory
static const size_t MAX_POSTINGS_PER_RUN = 1000000;

// Print progress every N documents
static const int64_t PROGRESS_INTERVAL = 100000;

// One posting: (term, doc_id, tf)
struct Posting {
    std::string term;
    uint32_t doc_id;
    uint32_t tf;
};

// f"{n:,}"
static std::string commas(unsigned long long n) {
    std::string s = std::to_string(n), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && (s.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(s[i]);
    }
    return out;
}

static double now_seconds() {  // time.perf_counter()
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void append_uint(std::string& out, uint64_t v) {  // fast str(int)
    char tmp[24];
    int n = 0;
    do { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (n) out.push_back(tmp[--n]);
}

static int write_run(std::vector<Posting>& postings, int run_number) {

    // postings.sort(key=lambda x: (x[0], x[1]))
    std::sort(postings.begin(), postings.end(), [](const Posting& a, const Posting& b) {
        int c = a.term.compare(b.term);
        return c != 0 ? c < 0 : a.doc_id < b.doc_id;
    });

    char name[32];
    std::snprintf(name, sizeof name, "run_%03d.dat", run_number);
    std::string run_path = RUNS_DIR + "/" + name;

    FILE* f = std::fopen(run_path.c_str(), "wb");
    if (!f) { std::perror(run_path.c_str()); std::exit(1); }

    std::string buf;  // f.write(f"{term}\t{doc_id}\t{tf}\n"), batched into 1 MB writes
    buf.reserve((1 << 20) + 256);
    for (const Posting& p : postings) {
        buf += p.term;
        buf.push_back('\t');
        append_uint(buf, p.doc_id);
        buf.push_back('\t');
        append_uint(buf, p.tf);
        buf.push_back('\n');
        if (buf.size() >= (1 << 20)) { std::fwrite(buf.data(), 1, buf.size(), f); buf.clear(); }
    }
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);

    int num_postings = static_cast<int>(postings.size());

    std::printf("Wrote %s postings to %s\n", commas(static_cast<unsigned long long>(num_postings)).c_str(), name);

    return num_postings;
}

static void remove_old_runs() {
    if (DIR* d = opendir(RUNS_DIR.c_str())) {
        while (dirent* e = readdir(d)) {  // for run_file in RUNS_DIR.glob("run_*.dat")
            std::string n = e->d_name;
            if (n.rfind("run_", 0) == 0 && n.size() >= 4 && n.compare(n.size() - 4, 4, ".dat") == 0)
                unlink((RUNS_DIR + "/" + n).c_str());
        }
        closedir(d);
    }
}

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "pagetable.bin is written little-endian");

// Streams the page table to disk in the final pagetable.bin layout, in constant memory.
// The three columns are appended to temporary files while parsing (the position of each
// column in the final file depends on N, which is only known at the end). finish() writes
// the header and copies the columns after it, then deletes the temporary files.
class PageTableWriter {
public:
    explicit PageTableWriter(const std::string& dir) {
        for (int c = 0; c < 3; ++c) {
            tmp_path_[c] = dir + "/pagetable." + kName[c] + ".tmp";
            tmp_[c] = std::fopen(tmp_path_[c].c_str(), "wb");
            if (!tmp_[c]) { std::perror(tmp_path_[c].c_str()); std::exit(1); }
        }
    }

    // If the program aborts before finish(), do not leave the temporary files behind.
    ~PageTableWriter() {
        if (finished_) return;
        for (int c = 0; c < 3; ++c) { std::fclose(tmp_[c]); unlink(tmp_path_[c].c_str()); }
    }

    void add(uint32_t pid, uint16_t doc_len, uint64_t doc_offset) {
        put(0, &pid, sizeof pid);
        put(1, &doc_len, sizeof doc_len);
        put(2, &doc_offset, sizeof doc_offset);
        ++n_;
    }

    void finish(const std::string& path) {
        if (n_ > UINT32_MAX) { std::fprintf(stderr, "too many documents for a u32 N\n"); std::exit(1); }
        for (int c = 0; c < 3; ++c) { flush(c); std::fclose(tmp_[c]); }
        FILE* out = std::fopen(path.c_str(), "wb");
        if (!out) { std::perror(path.c_str()); std::exit(1); }
        uint32_t n = static_cast<uint32_t>(n_);
        std::fwrite(&n, sizeof n, 1, out);
        std::vector<char> chunk(1 << 20);
        for (int c = 0; c < 3; ++c) {  // pid[], docLen[], docOffset[] in this order
            FILE* in = std::fopen(tmp_path_[c].c_str(), "rb");
            size_t got;
            while ((got = std::fread(chunk.data(), 1, chunk.size(), in)) > 0) std::fwrite(chunk.data(), 1, got, out);
            std::fclose(in);
            unlink(tmp_path_[c].c_str());
        }
        std::fclose(out);
        finished_ = true;
    }

private:
    void put(int c, const void* v, size_t size) {
        buf_[c].append(static_cast<const char*>(v), size);
        if (buf_[c].size() >= (1 << 20)) flush(c);
    }
    void flush(int c) {
        std::fwrite(buf_[c].data(), 1, buf_[c].size(), tmp_[c]);
        buf_[c].clear();
    }

    static constexpr const char* kName[3] = {"pid", "len", "off"};
    std::string tmp_path_[3], buf_[3];
    FILE* tmp_[3];
    uint64_t n_ = 0;
    bool finished_ = false;
};

int main(int argc, char** argv) {

    if (argc > 1) PROJECT_ROOT = argv[1];
    DATASET_PATH = PROJECT_ROOT + "/data/collection.tsv";
    RUNS_DIR = PROJECT_ROOT + "/runs";
    OUTPUT_DIR = PROJECT_ROOT + "/output";

    mkdir(RUNS_DIR.c_str(), 0755);
    mkdir(OUTPUT_DIR.c_str(), 0755);

    // Prevent old run files from mixing with new ones
    remove_old_runs();

    std::vector<Posting> postings;

    int run_number = 0;
    int64_t num_docs = 0;
    uint64_t total_tokens = 0;
    uint64_t total_postings = 0;

    std::string pagetable_path = OUTPUT_DIR + "/pagetable.bin";
    std::string stats_path = OUTPUT_DIR + "/stats.txt";

    double start_time = now_seconds();

    PageTableWriter page_table(OUTPUT_DIR);

    MsMarcoReader reader(DATASET_PATH);
    std::string pid, text;
    std::unordered_map<std::string, uint32_t> term_freqs;

    for (int64_t doc_id = 0; reader.next(pid, text); ++doc_id) {

        if (MAX_DOCS >= 0 && doc_id >= MAX_DOCS) break;

        uint64_t doc_offset = reader.line_offset();  // byte offset of this line in collection.tsv

        // TOKENIZATION
        std::vector<std::string> tokens = tokenize(text);
        size_t document_length = tokens.size();

        num_docs += 1;
        total_tokens += document_length;


        // PAGE TABLE: pid must be an unsigned 32-bit number, length must fit in 16 bits
        char* pid_end = nullptr;
        unsigned long long pid_value = std::strtoull(pid.c_str(), &pid_end, 10);
        if (pid.empty() || *pid_end != '\0' || pid_value > UINT32_MAX) {
            std::fprintf(stderr, "doc %lld: pid '%s' is not a u32\n", static_cast<long long>(doc_id), pid.c_str());
            return 1;
        }
        if (document_length > UINT16_MAX) {
            std::fprintf(stderr, "doc %lld: %zu tokens do not fit in a u16 length\n", static_cast<long long>(doc_id), document_length);
            return 1;
        }
        page_table.add(static_cast<uint32_t>(pid_value), static_cast<uint16_t>(document_length), doc_offset);


        // TERM FREQUENCIES
        term_freqs.clear();
        for (const std::string& t : tokens) ++term_freqs[t];  // Counter(tokens)


        // POSTINGS
        for (const auto& [term, tf] : term_freqs)
            postings.push_back({term, static_cast<uint32_t>(doc_id), tf});


        // FLUSH RUN
        if (postings.size() >= MAX_POSTINGS_PER_RUN) {

            total_postings += static_cast<uint64_t>(write_run(postings, run_number));

            postings.clear();
            run_number += 1;
        }


        // PROGRESS
        if (num_docs % PROGRESS_INTERVAL == 0) {

            double elapsed = now_seconds() - start_time;

            double docs_per_second = elapsed > 0 ? static_cast<double>(num_docs) / elapsed : 0;

            std::printf("Processed %s docs | %s docs/sec | elapsed %.2fs | buffer %s postings\n",
                        commas(static_cast<unsigned long long>(num_docs)).c_str(),
                        commas(static_cast<unsigned long long>(docs_per_second + 0.5)).c_str(),
                        elapsed, commas(postings.size()).c_str());
            std::fflush(stdout);
        }
    }

    // FINAL PARTIAL RUN
    if (!postings.empty()) {

        total_postings += static_cast<uint64_t>(write_run(postings, run_number));

        postings.clear();
        run_number += 1;
    }

    page_table.finish(pagetable_path);


    // FINAL STATISTICS
    double elapsed = now_seconds() - start_time;

    double avg_doc_length = num_docs > 0 ? static_cast<double>(total_tokens) / static_cast<double>(num_docs) : 0;

    double docs_per_second = elapsed > 0 ? static_cast<double>(num_docs) / elapsed : 0;

    if (FILE* stats_file = std::fopen(stats_path.c_str(), "wb")) {

        std::fprintf(stats_file, "num_docs=%lld\n", static_cast<long long>(num_docs));

        std::fprintf(stats_file, "total_tokens=%llu\n", static_cast<unsigned long long>(total_tokens));

        std::fprintf(stats_file, "avg_doc_length=%.4f\n", avg_doc_length);

        std::fprintf(stats_file, "total_postings=%llu\n", static_cast<unsigned long long>(total_postings));

        std::fprintf(stats_file, "num_runs=%d\n", run_number);

        std::fprintf(stats_file, "elapsed_seconds=%.4f\n", elapsed);

        std::fprintf(stats_file, "docs_per_second=%.4f\n", docs_per_second);

        std::fclose(stats_file);
    }

    std::printf("\n");
    std::printf("Finished.\n");
    std::printf("Documents:       %s\n", commas(static_cast<unsigned long long>(num_docs)).c_str());
    std::printf("Tokens:          %s\n", commas(total_tokens).c_str());
    std::printf("Postings:        %s\n", commas(total_postings).c_str());
    std::printf("Runs:            %d\n", run_number);
    std::printf("Elapsed:         %.2f seconds\n", elapsed);
    std::printf("Speed:           %s docs/sec\n", commas(static_cast<unsigned long long>(docs_per_second + 0.5)).c_str());
    std::printf("Average length:  %.2f\n", avg_doc_length);

    std::printf("\n");
    std::printf("Page table:      %s\n", pagetable_path.c_str());
    std::printf("Statistics:      %s\n", stats_path.c_str());
    return 0;
}

// Build:  make -C indexer
// Run:    ./merge_runs [project_root]      (default ".")
//         reads  <project_root>/runs/run_NNN.dat
//         writes <project_root>/output/index.bin, meta.bin, lexicon.bin
// Output format
//   index.bin    blocks of BLOCK_SIZE (128) postings: [docIDs varbyte][freqs varbyte]. All lists are
//                concatenated in lexicon order, and an inverted list may start/end mid-block. A docID 
//                is stored absolute if it is the first posting of its block or of its list, else as the 
//                gap to the previous posting.
//   meta.bin, lexicon.bin   layouts defined in index_format.h

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "../config.h"
#include "../index_format.h"
#include "../varbyte.h"
#include "util.h"

static const size_t FAN_IN = 200;          // runs merged at once (d)
static const size_t MEMORY_MB = 512;        // total budget for input buffers

[[noreturn]] static void die(const std::string& msg) {
    std::fprintf(stderr, "merge_runs: %s\n", msg.c_str());
    std::exit(1);
}

static void write_or_die(const void* data, size_t size, FILE* f, const std::string& what) {
    if (size > 0 && std::fwrite(data, 1, size, f) != size)
        die("write failed on " + what + ": " + std::strerror(errno) + " (check free disk space)");
}

// reader for one input run, with its own buffer and exposes the current posting 
class RunReader {
public:
    RunReader(const std::string& path, size_t buffer_bytes) : path_(path), buf_(buffer_bytes) {
        file_ = std::fopen(path.c_str(), "rb");
        if (!file_) die("cannot open " + path);
        advance();  // load the first posting
    }

    ~RunReader() { 
        if (file_) { std::fclose(file_); }
    }
    RunReader(const RunReader&) = delete;
    RunReader& operator=(const RunReader&) = delete;

    bool valid() const { return valid_; }
    const std::string& term() const { return term_; }
    uint32_t doc() const { return doc_; }
    uint32_t tf() const { return tf_; }

    // Moves to the next posting; return false at the end of the run. 
    bool advance() {
        std::string_view line;
        if (!next_line(line)) { 
            valid_ = false; 
            return false; 
        }

        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string_view::npos ? t1 : line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos) {
            die(path_ + ": malformed line '" + std::string(line.substr(0, 60)) + "'");
        }

        std::string_view term = line.substr(0, t1);
        uint32_t doc = 0, tf = 0;
        auto r1 = std::from_chars(line.data() + t1 + 1, line.data() + t2, doc);
        auto r2 = std::from_chars(line.data() + t2 + 1, line.data() + line.size(), tf);
        if (r1.ec != std::errc() || r1.ptr != line.data() + t2 || r2.ec != std::errc() ||
            r2.ptr != line.data() + line.size() || tf == 0) {
            die(path_ + ": malformed line '" + std::string(line.substr(0, 60)) + "'");
        }

        if (!have_ || term != term_) {
            if (have_ && term < term_) die(path_ + ": terms not sorted at '" + std::string(term) + "'");
            term_.assign(term);
        } else if (doc <= doc_) {
            die(path_ + ": docIDs out of order for term '" + term_ + "'");
        }
        doc_ = doc;
        tf_ = tf;
        have_ = valid_ = true;
        return true;
    }

private:
    // finds the next line without its '\n'
    bool next_line(std::string_view& line) {
        while (true) {
            const char* begin = buf_.data() + pos_;
            const char* nl = static_cast<const char*>(std::memchr(begin, '\n', len_ - pos_));
            if (nl) {
                line = std::string_view(begin, static_cast<size_t>(nl - begin));
                pos_ += line.size() + 1;
                return true;
            }
            if (eof_) {
                if (pos_ == len_) return false;
                die(path_ + ": last line has no newline");
            }
            // No complete line buffered: move the partial line to the front and read more.
            size_t rest = len_ - pos_;
            std::memmove(buf_.data(), buf_.data() + pos_, rest);
            pos_ = 0;
            len_ = rest;
            if (len_ == buf_.size()) buf_.resize(buf_.size() * 2);  // a very long line
            size_t got = std::fread(buf_.data() + len_, 1, buf_.size() - len_, file_);
            len_ += got;
            if (got == 0) eof_ = true;
        }
    }

    std::string path_;
    FILE* file_ = nullptr;
    std::vector<char> buf_;
    size_t pos_ = 0, len_ = 0;
    bool eof_ = false, have_ = false, valid_ = false;
    std::string term_;
    uint32_t doc_ = 0, tf_ = 0;
};

// Intermediate passes: writes an intermediate run in text format
// A sink gets begin_term(term), add(doc, tf) for each posting in docID order, end_term()
class TextRunSink {
public:
    explicit TextRunSink(const std::string& path) : path_(path) {
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_) die("cannot create " + path);
        buf_.reserve(kFlush + 4096);
    }
    void begin_term(const std::string& term) { term_ = term; }
    void add(uint32_t doc, uint32_t tf) {
        buf_ += term_;
        buf_.push_back('\t');
        append_uint(buf_, doc);
        buf_.push_back('\t');
        append_uint(buf_, tf);
        buf_.push_back('\n');
        if (buf_.size() >= kFlush) flush();
    }
    void end_term() {}
    void close() { flush(); std::fclose(file_); file_ = nullptr; }

private:
    static const size_t kFlush = 4 << 20;
    void flush() { write_or_die(buf_.data(), buf_.size(), file_, path_); buf_.clear(); }
    std::string path_, term_, buf_;
    FILE* file_ = nullptr;
};

// Final pass: block writer + lexicon writer.
class IndexSink {
public:
    explicit IndexSink(const std::string& out_dir)
        : dir_(out_dir), idx_buf_(8 << 20) {
        index_ = std::fopen((dir_ + "/index.bin").c_str(), "wb");
        lexicon_ = std::fopen((dir_ + "/lexicon.bin").c_str(), "wb");
        if (!index_ || !lexicon_) die("cannot create output files in " + dir_);
        std::setvbuf(index_, idx_buf_.data(), _IOFBF, idx_buf_.size());  // big output buffer
        uint32_t placeholder = 0;  // numTerms, patched in close()
        write_or_die(&placeholder, sizeof placeholder, lexicon_, "lexicon.bin");
    }

    void begin_term(const std::string& term) {
        if (term.size() > MAX_TERM_BYTES)
            die("term of " + std::to_string(term.size()) + " bytes does not fit the one-byte length field");
        term_ = term;
        start_block_ = static_cast<uint32_t>(meta_.last_doc.size());  // blocks flushed so far
        start_slot_ = static_cast<uint8_t>(n_);                  // postings buffered in the open block
        first_of_list_ = true;
        ft_ = 0;
    }

    void add(uint32_t doc, uint32_t tf) {
        // absolute docID at the start of a block or of a list, gap otherwise
        vb_encode(first_of_list_ || n_ == 0 ? doc : doc - prev_, docs_);
        vb_encode(tf, freqs_);
        prev_ = doc;
        first_of_list_ = false;
        ++ft_;
        ++total_postings_;
        if (++n_ == BLOCK_SIZE) flush_block();
    }

    void end_term() {
        write_lex_entry(lex_, {term_, ft_, start_block_, start_slot_});
        ++num_terms_;
        if (lex_.size() >= (1 << 20)) flush_lexicon();
    }

    // Flushes the partial last block and writes lexicon.bin and meta.bin.
    void close() {
        if (n_ > 0) flush_block();
        std::fclose(index_);
        flush_lexicon();
        if (std::fseek(lexicon_, 0, SEEK_SET) != 0) die("seek failed on lexicon.bin");
        write_or_die(&num_terms_, sizeof num_terms_, lexicon_, "lexicon.bin");
        std::fclose(lexicon_);

        write_meta(dir_ + "/meta.bin", meta_);
    }

    uint64_t total_postings() const { return total_postings_; }
    uint32_t num_terms() const { return num_terms_; }
    size_t num_blocks() const { return meta_.last_doc.size(); }
    uint64_t last_term_postings() const { return ft_; }

private:
    void flush_block() {
        write_or_die(docs_.data(), docs_.size(), index_, "index.bin");
        write_or_die(freqs_.data(), freqs_.size(), index_, "index.bin");
        meta_.last_doc.push_back(prev_);
        meta_.doc_bytes.push_back(static_cast<uint16_t>(docs_.size()));
        meta_.freq_bytes.push_back(static_cast<uint16_t>(freqs_.size()));
        docs_.clear();
        freqs_.clear();
        n_ = 0;
        if (total_postings_ - reported_ >= 50000000) {
            reported_ = total_postings_;
            std::printf("  ... %s postings written (%s terms)\n", commas(total_postings_).c_str(),
                        commas(num_terms_).c_str());
            std::fflush(stdout);
        }
    }
    void flush_lexicon() { write_or_die(lex_.data(), lex_.size(), lexicon_, "lexicon.bin"); lex_.clear(); }

    std::string dir_;
    FILE *index_ = nullptr, *lexicon_ = nullptr;
    std::vector<char> idx_buf_;
    std::string term_, lex_;
    std::vector<uint8_t> docs_, freqs_;  // encoded sections of the open block
    uint32_t n_ = 0, prev_ = 0, ft_ = 0, start_block_ = 0;
    uint8_t start_slot_ = 0;
    bool first_of_list_ = false;
    uint32_t num_terms_ = 0;
    uint64_t total_postings_ = 0, reported_ = 0;
    Meta meta_;  // meta.bin columns
};

// D-way merge of the sub indices
template <class Sink>
static void merge(const std::vector<std::string>& paths, size_t buffer_bytes, Sink& sink) {
    std::vector<std::unique_ptr<RunReader>> readers;
    for (const std::string& p : paths) readers.push_back(std::make_unique<RunReader>(p, buffer_bytes));

    // Min-heap of run numbers ordered by (current term, run number).
    auto later = [&](int a, int b) {
        int c = readers[a]->term().compare(readers[b]->term());
        return c != 0 ? c > 0 : a > b;
    };
    std::vector<int> heap;
    for (size_t i = 0; i < readers.size(); ++i) {
        if (readers[i]->valid()) {
            heap.push_back(static_cast<int>(i));
        }
    }
    std::make_heap(heap.begin(), heap.end(), later);

    std::string cur;
    while (!heap.empty()) {
        cur = readers[heap.front()]->term();
        sink.begin_term(cur);
        uint32_t last_doc = 0;
        bool any = false;

        while (!heap.empty() && readers[heap.front()]->term() == cur) {
            std::pop_heap(heap.begin(), heap.end(), later);
            int i = heap.back();
            heap.pop_back();
            RunReader& r = *readers[i];

            do {  // all postings of this term in this run
                if (any && r.doc() <= last_doc) die("docIDs of term '" + cur + "' are not increasing across runs");
                sink.add(r.doc(), r.tf());
                last_doc = r.doc();
                any = true;
            } while (r.advance() && r.term() == cur);

            if (r.valid()) { 
                heap.push_back(i); 
                std::push_heap(heap.begin(), heap.end(), later); 
            }
        }
        sink.end_term();
    }
}

// read the run files
static std::vector<std::string> list_runs(const std::string& dir) {
    std::vector<std::pair<unsigned long long, std::string>> found;
    DIR* d = opendir(dir.c_str());
    if (!d) die("cannot open " + dir);
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 8 && n.rfind("run_", 0) == 0 && n.compare(n.size() - 4, 4, ".dat") == 0) {
            std::string digits = n.substr(4, n.size() - 8);
            if (digits.find_first_not_of("0123456789") == std::string::npos)
                found.emplace_back(std::strtoull(digits.c_str(), nullptr, 10), dir + "/" + n);
        }
    }
    closedir(d);
    std::sort(found.begin(), found.end());
    std::vector<std::string> paths;
    for (auto& f : found) paths.push_back(f.second);
    return paths;
}

static bool is_temp(const std::string& path) { return path.size() > 4 && path.compare(path.size() - 4, 4, ".tmp") == 0; }

static long long file_size(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string runs_dir = root + "/runs", out_dir = root + "/output";
    mkdir(out_dir.c_str(), 0755);
    const double start_time = now_seconds();

    // FAN_IN input files plus the output files must fit in the open-file limit (macOS default: 256).
    struct rlimit rl;
    const size_t open_limit = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
                                                   ? static_cast<size_t>(rl.rlim_cur) : (1u << 20);
    assert(FAN_IN + 32 <= open_limit && "FAN_IN exceeds the system open-file limit");
    const size_t fan_in = FAN_IN;
    auto buffer_for = [&](size_t d) { return std::clamp<size_t>(MEMORY_MB * (1 << 20) / std::max<size_t>(d, 1), 64 << 10, 8 << 20); };

    std::vector<std::string> runs = list_runs(runs_dir);
    if (runs.empty()) die("no run_*.dat files in " + runs_dir);
    std::printf("Merging %zu runs from %s (fan-in %zu, input buffers %.0f KB each)\n", runs.size(),
                runs_dir.c_str(), fan_in, buffer_for(std::min(runs.size(), fan_in)) / 1024.0);

    // Intermediate passes while there are more runs than each d-way merge
    int pass = 0;
    while (runs.size() > fan_in) {
        size_t groups = (runs.size() + fan_in - 1) / fan_in;
        size_t base = runs.size() / groups, extra = runs.size() % groups, pos = 0;
        std::vector<std::string> next;
        for (size_t g = 0; g < groups; ++g) {
            size_t size = base + (g < extra ? 1 : 0);
            std::vector<std::string> group(runs.begin() + pos, runs.begin() + pos + size);
            pos += size;
            if (size == 1) { next.push_back(group[0]); continue; }  // nothing to merge
            std::string out = runs_dir + "/merge_p" + std::to_string(pass) + "_" + std::to_string(g) + ".tmp";
            TextRunSink sink(out);
            merge(group, buffer_for(size), sink);
            sink.close();
            for (const std::string& p : group) if (is_temp(p)) unlink(p.c_str());  // never the original runs
            next.push_back(out);
        }
        std::printf("Pass %d: %zu runs -> %zu runs\n", pass, runs.size(), next.size());
        std::fflush(stdout);
        runs = next;
        ++pass;
    }

    // Final merge into the index files.
    IndexSink sink(out_dir);
    merge(runs, buffer_for(runs.size()), sink);
    sink.close();
    for (const std::string& p : runs) if (is_temp(p)) unlink(p.c_str());

    const double elapsed = now_seconds() - start_time;
    std::printf("\nFinished.\n");
    std::printf("Runs merged:     %zu (%d intermediate passes)\n", list_runs(runs_dir).size(), pass);
    std::printf("Terms:           %s\n", commas(sink.num_terms()).c_str());
    std::printf("Postings:        %s\n", commas(sink.total_postings()).c_str());
    std::printf("Blocks:          %s\n", commas(sink.num_blocks()).c_str());
    for (const char* f : {"index.bin", "meta.bin", "lexicon.bin", "pagetable.bin"})
        std::printf("  %-14s %s bytes\n", f, commas(static_cast<unsigned long long>(std::max(0LL, file_size(out_dir + "/" + f)))).c_str());
    std::printf("Elapsed:         %.2f seconds\n", elapsed);
    return 0;
}

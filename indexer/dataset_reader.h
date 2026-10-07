// indexer/dataset_reader.h -- C++ port of dataset_reader.py
//
// Python:  read_msmarco(archive_path) opens collection.tar.gz, takes the member
//          "collection.tsv", and yields (pid, passage) for every non-empty line
//          ("pid<TAB>passage"), with the trailing "\n" removed.
//
// Same here: MsMarcoReader::next(pid, passage) returns the next pair. The
// archive is streamed (never held in memory): zlib inflates the gzip layer and
// a minimal tar parser walks the 512-byte headers to find collection.tsv.
// A plain collection.tsv (path not ending in .tar.gz) is read directly too.
// Invalid UTF-8 needs no handling: bytes are passed through unchanged.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <zlib.h>

class MsMarcoReader {
public:
    explicit MsMarcoReader(const std::string& archive_path) {
        is_tar_ = archive_path.size() >= 7 && archive_path.compare(archive_path.size() - 7, 7, ".tar.gz") == 0;
        if (is_tar_) {
            gz_ = gzopen(archive_path.c_str(), "rb");
            if (!gz_) fail("Could not open " + archive_path);
            gzbuffer(gz_, 1 << 20);
            seek_member("collection.tsv");
        } else {
            plain_ = std::fopen(archive_path.c_str(), "rb");
            if (!plain_) fail("Could not open " + archive_path);
        }
        buf_.resize(1 << 20);
    }
    ~MsMarcoReader() {
        if (gz_) gzclose(gz_);
        if (plain_) std::fclose(plain_);
    }
    MsMarcoReader(const MsMarcoReader&) = delete;
    MsMarcoReader& operator=(const MsMarcoReader&) = delete;

    // Byte offset, in the input data, of the line returned by the last next() call 
    unsigned long long line_offset() const { return line_offset_; }

    // Yields the next (pid, passage); returns false at the end of the file.
    bool next(std::string& pid, std::string& passage) {
        while (next_line(line_)) {
            if (line_.empty()) continue;  // Python: if not line: continue
            size_t tab = line_.find('\t');
            if (tab == std::string::npos) fail("line without a tab: " + line_.substr(0, 60));
            pid.assign(line_, 0, tab);       // pid, passage = line.split("\t", 1)
            passage.assign(line_, tab + 1, std::string::npos);
            return true;
        }
        return false;
    }

private:
    [[noreturn]] static void fail(const std::string& msg) {
        std::fprintf(stderr, "RuntimeError: %s\n", msg.c_str());
        std::exit(1);
    }

    // Reads exactly n bytes from the gzip stream (false if the stream ends first).
    bool gz_read_full(void* dst, size_t n) {
        char* p = static_cast<char*>(dst);
        while (n > 0) {
            int got = gzread(gz_, p, static_cast<unsigned>(n > (1u << 30) ? (1u << 30) : n));
            if (got <= 0) return false;
            p += got;
            n -= static_cast<size_t>(got);
        }
        return true;
    }

    // Walks tar headers until the member `name`; leaves the stream at its data.
    void seek_member(const std::string& name) {
        char header[512];
        while (gz_read_full(header, 512)) {
            if (header[0] == '\0') break;  // end-of-archive marker
            std::string member(header, strnlen(header, 100));
            unsigned long long size = std::strtoull(std::string(header + 124, 12).c_str(), nullptr, 8);
            if (member == name) { remaining_ = size; return; }
            unsigned long long skip = (size + 511) / 512 * 512;  // data is padded to 512
            if (gzseek(gz_, static_cast<z_off_t>(skip), SEEK_CUR) < 0) break;
        }
        fail("Could not open " + name);
    }

    // Refills buf_ with the next chunk of collection.tsv; false at its end.
    bool fill() {
        size_t want = buf_.size();
        if (is_tar_) {
            if (remaining_ == 0) return false;
            if (want > remaining_) want = static_cast<size_t>(remaining_);
            int got = gzread(gz_, buf_.data(), static_cast<unsigned>(want));
            if (got <= 0) return false;
            remaining_ -= static_cast<unsigned long long>(got);
            len_ = static_cast<size_t>(got);
        } else {
            len_ = std::fread(buf_.data(), 1, want, plain_);
            if (len_ == 0) return false;
        }
        pos_ = 0;
        return true;
    }

    // Next line without its "\n"; false at end of file.
    bool next_line(std::string& line) {
        line.clear();
        line_offset_ = consumed_;
        bool any = false;
        while (true) {
            if (pos_ == len_ && !fill()) return any;
            any = true;
            const char* start = buf_.data() + pos_;
            const char* nl = static_cast<const char*>(std::memchr(start, '\n', len_ - pos_));
            if (nl) {
                line.append(start, static_cast<size_t>(nl - start));
                pos_ += static_cast<size_t>(nl - start) + 1;
                consumed_ += static_cast<unsigned long long>(nl - start) + 1;
                return true;
            }
            line.append(start, len_ - pos_);
            consumed_ += len_ - pos_;
            pos_ = len_;
        }
    }

    bool is_tar_ = false;
    gzFile gz_ = nullptr;
    FILE* plain_ = nullptr;
    unsigned long long remaining_ = 0;  // bytes of collection.tsv still unread (tar mode)
    unsigned long long consumed_ = 0;     // bytes of collection.tsv consumed so far
    unsigned long long line_offset_ = 0;  // start offset of the line just returned
    std::string buf_, line_;
    size_t pos_ = 0, len_ = 0;
};

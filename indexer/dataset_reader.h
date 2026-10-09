#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

class MsMarcoReader {
public:
    explicit MsMarcoReader(const std::string& path) {
        file_ = std::fopen(path.c_str(), "rb");
        if (!file_) fail("Could not open " + path);
        buf_.resize(1 << 20);
    }
    ~MsMarcoReader() { if (file_) std::fclose(file_); }
    MsMarcoReader(const MsMarcoReader&) = delete;
    MsMarcoReader& operator=(const MsMarcoReader&) = delete;

    // Byte offset in collection.tsv of the line returned by the last next() call
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

    // Refills buf_ with the next chunk of the file; false at its end.
    bool fill() {
        len_ = std::fread(buf_.data(), 1, buf_.size(), file_);
        pos_ = 0;
        return len_ > 0;
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

    FILE* file_ = nullptr;
    unsigned long long consumed_ = 0;     // bytes of collection.tsv consumed so far
    unsigned long long line_offset_ = 0;  // start offset of the line just returned
    std::string buf_, line_;
    size_t pos_ = 0, len_ = 0;
};

#pragma once
//
// Reader — streams a gzip'd ITCH 5.0 file and hands out one framed message at a
// time. Never gunzips the whole file to disk or materializes it in memory.
//
// ITCH files downloaded from NASDAQ are a sequence of records, each of the form:
//
//     [ 2-byte big-endian length N ][ N bytes of message payload ]
//
// Byte 0 of the payload is the message type. See messages.hpp.
//
#include <zlib.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace itchbook {

class Reader {
public:
    explicit Reader(const std::string& path) {
        gz_ = gzopen(path.c_str(), "rb");
        if (gz_ == nullptr) {
            throw std::runtime_error("Reader: cannot open " + path);
        }
        gzbuffer(gz_, 1u << 20);  // 1 MiB internal inflate buffer
    }

    ~Reader() {
        if (gz_ != nullptr) {
            gzclose(gz_);
        }
    }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    Reader(Reader&& other) noexcept : gz_(other.gz_), bytes_(other.bytes_) {
        other.gz_ = nullptr;
    }

    // Reads the next framed message into `out`. Returns false at clean EOF.
    // Throws on a truncated frame — a truncated frame means desync, and you
    // want to know immediately rather than 200 MB later.
    bool next(std::vector<uint8_t>& out) {
        uint8_t len_buf[2];
        int n = gzread(gz_, len_buf, 2);
        if (n == 0) {
            return false;  // clean EOF
        }
        if (n != 2) {
            throw std::runtime_error("Reader: truncated length prefix");
        }
        uint16_t len = static_cast<uint16_t>((static_cast<uint16_t>(len_buf[0]) << 8) | len_buf[1]);
        out.resize(len);
        int m = gzread(gz_, out.data(), len);
        if (m != static_cast<int>(len)) {
            throw std::runtime_error("Reader: truncated message body");
        }
        bytes_ += 2 + len;
        ++messages_;
        return true;
    }

    uint64_t bytes() const { return bytes_; }
    uint64_t messages() const { return messages_; }

private:
    gzFile gz_ = nullptr;
    uint64_t bytes_ = 0;
    uint64_t messages_ = 0;
};

}  // namespace itchbook

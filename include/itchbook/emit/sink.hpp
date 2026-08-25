#pragma once
//
// sink.hpp — where emitted ITCH goes.
//
// The exchange publishes; something consumes. Keeping that seam abstract is
// what lets the phase-12.2 gate wire the publisher straight into a consumer in
// one process and compare books after every message, while the same publisher
// writes an ordinary framed .itch file that book_replay, itch_census and
// mold_wrap can all read without knowing anything was different about it. The
// loop grades itself with the tools that already exist.
//
// A virtual call per message is deliberate and is not on any measured path yet.
// Phase 4's no-allocation, no-I/O rules apply to the book thread; the publisher
// is not that thread, and 12.7 is where its cost has to be answered rather than
// asserted. It is a null check when no sink is attached, which is what phase
// 12.1's gate runs with.
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace itchbook::emit {

class Sink {
public:
    virtual ~Sink() = default;
    // One complete ITCH payload, type byte first. Framing is the sink's
    // business: a file wants the 2-byte length prefix, an in-process consumer
    // does not.
    virtual void on_message(const uint8_t* payload, size_t len) = 0;
};

// Writes the ordinary NASDAQ file framing -- 2-byte big-endian length, then the
// payload -- so the result is byte-for-byte the shape of the files in data/raw
// and every existing reader accepts it.
class FileSink : public Sink {
public:
    explicit FileSink(std::FILE* f) : f_(f) {}

    void on_message(const uint8_t* p, size_t len) override {
        uint8_t hdr[2] = {static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
        std::fwrite(hdr, 1, 2, f_);
        std::fwrite(p, 1, len, f_);
        ++messages_;
        bytes_ += 2 + len;
    }

    uint64_t messages() const { return messages_; }
    uint64_t bytes() const { return bytes_; }

private:
    std::FILE* f_;
    uint64_t messages_ = 0;
    uint64_t bytes_ = 0;
};

// Keeps the payloads of the current input message so the gate can apply them to
// a book and compare them against the input, then clear and go round again.
// Bounded by the most messages one input message can produce, which is the
// number of makers a single aggressor can walk.
class BufferSink : public Sink {
public:
    void on_message(const uint8_t* p, size_t len) override {
        const size_t at = blob_.size();
        blob_.insert(blob_.end(), p, p + len);
        spans_.push_back({at, len});
    }

    void clear() {
        blob_.clear();
        spans_.clear();
    }

    size_t count() const { return spans_.size(); }
    const uint8_t* at(size_t i) const { return blob_.data() + spans_[i].first; }
    size_t len(size_t i) const { return spans_[i].second; }

private:
    std::vector<uint8_t> blob_;
    std::vector<std::pair<size_t, size_t>> spans_;
};

// Fans one publisher out to several sinks. The gate writes a file AND feeds a
// live consumer from the same emission, so the file it ships is provably the
// stream the consumer graded rather than a second rendering of it.
class TeeSink : public Sink {
public:
    void add(Sink* s) { sinks_.push_back(s); }

    void on_message(const uint8_t* p, size_t len) override {
        for (Sink* s : sinks_) s->on_message(p, len);
    }

private:
    std::vector<Sink*> sinks_;
};

}  // namespace itchbook::emit

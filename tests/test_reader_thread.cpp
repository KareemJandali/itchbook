// The threaded reader must be indistinguishable from the single-threaded one.
//
// Not "produce the same book" -- that is the phase 10.6 gate and it is a weaker
// claim than it looks, because two paths can agree on a book while disagreeing
// about which messages they saw. These compare the MESSAGE STREAM: same types,
// same bytes, same order, same count, same failures on the same malformed
// input.
//
// The chunk-boundary cases carry most of the weight. The first version of
// parse_threaded computed its fill limit as `c.len + 2 + 65535 > ChunkBytes`,
// which with a 64 KB chunk is true before a single byte is read -- so the
// producer published nothing, never reached EOF, and spun forever. It was found
// by running a tool and waiting, which is the slowest possible way to find it.
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/pipe/reader_thread.hpp"
#include "tests/check.hpp"

using namespace itchbook;

namespace {

struct Seen {
    std::vector<std::string> messages;
    void on_message(char, const uint8_t* p, uint16_t len) {
        messages.emplace_back(reinterpret_cast<const char*>(p), len);
    }
};

std::string temp_path(const char* tag) {
    return std::string("/tmp/itchbook-reader-thread-") + tag + ".gz";
}

// Writes [len][payload] frames. `payload` starts with the type byte.
void write_feed(const std::string& path, const std::vector<std::string>& msgs,
                int truncate_last_by = 0) {
    gzFile g = gzopen(path.c_str(), "wb");
    CHECK(g != nullptr);
    for (size_t i = 0; i < msgs.size(); ++i) {
        const std::string& m = msgs[i];
        uint8_t lb[2] = {static_cast<uint8_t>(m.size() >> 8),
                         static_cast<uint8_t>(m.size() & 0xff)};
        gzwrite(g, lb, 2);
        size_t n = m.size();
        if (i + 1 == msgs.size() && truncate_last_by > 0) {
            n -= static_cast<size_t>(truncate_last_by);
        }
        if (n > 0) gzwrite(g, m.data(), static_cast<unsigned>(n));
    }
    gzclose(g);
}

// An 'A' is 36 bytes by spec and the parser enforces that, so the generic
// filler uses a type with no spec length: anything unknown is framed and
// length-checked but not required to be any particular size.
std::string filler(size_t len, uint8_t tag) {
    std::string s(len, static_cast<char>(tag));
    s[0] = 'z';        // not in spec_length(), so any length is legal
    return s;
}

std::vector<std::string> sequential(const std::string& path) {
    Seen s;
    Reader r(path);
    parse(r, s);
    return s.messages;
}

void test_an_empty_file_yields_nothing() {
    const std::string p = temp_path("empty");
    write_feed(p, {});
    Seen s;
    CHECK_EQ(pipe::parse_threaded(p, s), uint64_t{0});
    CHECK(s.messages.empty());
}

void test_one_message_survives_the_handoff() {
    const std::string p = temp_path("one");
    write_feed(p, {filler(40, 1)});
    Seen s;
    pipe::ReaderStats st;
    CHECK_EQ(pipe::parse_threaded(p, s, &st), uint64_t{1});
    CHECK_EQ(s.messages.size(), size_t{1});
    CHECK_EQ(s.messages[0].size(), size_t{40});
    CHECK_EQ(st.messages, uint64_t{1});
    CHECK_EQ(st.bytes, uint64_t{42});
    CHECK_EQ(st.chunks, uint64_t{1});
}

void test_the_stream_matches_the_single_threaded_reader() {
    const std::string p = temp_path("many");
    std::vector<std::string> msgs;
    for (size_t i = 0; i < 20000; ++i) {
        msgs.push_back(filler(20 + (i % 60), static_cast<uint8_t>(i)));
    }
    write_feed(p, msgs);
    Seen s;
    CHECK_EQ(pipe::parse_threaded(p, s), uint64_t{20000});
    CHECK(s.messages == sequential(p));
    CHECK(s.messages == std::vector<std::string>(msgs.begin(), msgs.end()));
}

// The case the design exists to make impossible: a message that does not fit in
// the chunk being filled is carried whole to the next one. A tiny chunk makes
// that happen on almost every message rather than once in a thousand.
void test_messages_never_straddle_a_chunk() {
    const std::string p = temp_path("straddle");
    std::vector<std::string> msgs;
    for (size_t i = 0; i < 3000; ++i) {
        msgs.push_back(filler(1 + (i * 37) % 400, static_cast<uint8_t>(i)));
    }
    write_feed(p, msgs);
    for (int variant = 0; variant < 3; ++variant) {
        Seen s;
        pipe::ReaderStats st;
        uint64_t n = 0;
        // Chunk sizes chosen to land awkwardly against the message sizes: 512
        // is barely larger than the largest message, so most chunks carry.
        if (variant == 0) n = pipe::parse_threaded<Seen, 512, 4>(p, s, &st);
        if (variant == 1) n = pipe::parse_threaded<Seen, 1024, 2>(p, s, &st);
        if (variant == 2) n = pipe::parse_threaded<Seen, 4096, 8>(p, s, &st);
        CHECK_EQ(n, uint64_t{3000});
        CHECK(s.messages == msgs);
        CHECK_EQ(st.messages, uint64_t{3000});
        CHECK(st.chunks > 1);
    }
}

// ...and the one case that cannot be carried anywhere.
void test_a_message_larger_than_the_chunk_says_so() {
    const std::string p = temp_path("toobig");
    write_feed(p, {filler(50, 1), filler(900, 2)});
    Seen s;
    bool threw = false;
    try {
        pipe::parse_threaded<Seen, 256, 4>(p, s);
    } catch (const std::exception& e) {
        threw = true;
        // The message has to name both sizes, or the reader is a hang with a
        // better error code.
        CHECK(std::string(e.what()).find("902") != std::string::npos);
        CHECK(std::string(e.what()).find("256") != std::string::npos);
    }
    CHECK(threw);
}

// An exception on the producer thread would call std::terminate if it were
// simply allowed to escape. It has to arrive on the caller's thread, the way
// the single-threaded reader's would.
void test_a_truncated_body_throws_on_the_callers_thread() {
    const std::string p = temp_path("trunc");
    write_feed(p, {filler(40, 1), filler(40, 2)}, /*truncate_last_by=*/20);
    Seen s;
    bool threw = false;
    try {
        pipe::parse_threaded(p, s);
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("truncated") != std::string::npos);
    }
    CHECK(threw);
}

// A length prefix that disagrees with the spec is a desync, and the threaded
// path must refuse it exactly where parse() does.
void test_a_spec_length_mismatch_throws() {
    const std::string p = temp_path("mismatch");
    std::string bad(30, 'x');
    bad[0] = 'A';                     // spec says 36
    write_feed(p, {bad});
    Seen s;
    bool threw = false;
    try {
        pipe::parse_threaded(p, s);
    } catch (const std::exception& e) {
        threw = true;
        CHECK(std::string(e.what()).find("length mismatch") != std::string::npos);
    }
    CHECK(threw);
    // ...and the single-threaded reader agrees, which is the actual claim.
    bool seq_threw = false;
    try {
        sequential(p);
    } catch (const std::exception&) {
        seq_threw = true;
    }
    CHECK(seq_threw);
}

void test_a_missing_file_throws_rather_than_hanging() {
    Seen s;
    bool threw = false;
    try {
        pipe::parse_threaded("/tmp/itchbook-no-such-feed-42.gz", s);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

// The stall counters are the instrument that says WHICH SIDE is slow, so they
// have to move. A ring of two slots against a consumer doing nothing means the
// producer must have waited at some point.
void test_the_stall_counters_record_who_waited() {
    const std::string p = temp_path("stalls");
    std::vector<std::string> msgs;
    for (size_t i = 0; i < 5000; ++i) msgs.push_back(filler(60, static_cast<uint8_t>(i)));
    write_feed(p, msgs);
    Seen s;
    pipe::ReaderStats st;
    CHECK_EQ((pipe::parse_threaded<Seen, 512, 2>(p, s, &st)), uint64_t{5000});
    CHECK(st.chunks > 100);
    CHECK(st.max_occupancy >= 1);
    CHECK(st.max_occupancy <= 2);
    CHECK(st.producer_stalls + st.consumer_stalls > 0);
}

}  // namespace

int main() {
    test_an_empty_file_yields_nothing();
    test_one_message_survives_the_handoff();
    test_the_stream_matches_the_single_threaded_reader();
    test_messages_never_straddle_a_chunk();
    test_a_message_larger_than_the_chunk_says_so();
    test_a_truncated_body_throws_on_the_callers_thread();
    test_a_spec_length_mismatch_throws();
    test_a_missing_file_throws_rather_than_hanging();
    test_the_stall_counters_record_who_waited();
    return REPORT();
}

// mold_replay — read a MoldUDP64 packet file, report what survived the wire.
//
// The counterpart to mold_wrap. Runs the sequencer over a packet stream and
// says what it found: gaps, duplicates, reordering, heartbeats, truncation. On
// an undamaged file every counter but `packets` and `messages` must be zero,
// and `--unwrap` writing a file byte-identical to the original is the proof
// that the framing layer is transparent when nothing is wrong.
//
// That round trip matters more than it looks. A receiver that quietly drops
// one message in ten thousand would pass every unit test in test_mold.cpp,
// because those build their own packets; only replaying a real feed through
// the whole path and comparing bytes catches it.
//
// Usage:
//   mold_replay <packets.gz> [--unwrap out.gz] [--reorder-depth N]
//               [--reorder-patience N] [--quiet]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <zlib.h>

#include "itchbook/itch/reader.hpp"
#include "itchbook/mold/sequencer.hpp"

using namespace itchbook;

namespace {

struct Recovered {
    gzFile out = nullptr;
    uint64_t messages = 0;
    uint64_t gaps = 0;
    uint64_t lost = 0;
    bool verbose = true;

    void on_message(char, const uint8_t* p, uint16_t len) {
        ++messages;
        if (out == nullptr) return;
        uint8_t lb[2] = {static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xff)};
        gzwrite(out, lb, 2);
        gzwrite(out, p, len);
    }

    // The whole point of the layer. A gap is reported with its exact range and
    // NOT silently skipped: what to do about it is a policy decision, and one
    // this tool is deliberately not making.
    void on_gap(uint64_t first, uint64_t count) {
        ++gaps;
        lost += count;
        if (verbose) {
            std::fprintf(stderr, "GAP  sequence %" PRIu64 "..%" PRIu64
                                 "  (%" PRIu64 " messages lost)\n",
                         first, first + count - 1, count);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* unwrap_path = nullptr;
    mold::SequencerConfig cfg;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--unwrap" && i + 1 < argc) unwrap_path = argv[++i];
        else if (a == "--reorder-depth" && i + 1 < argc)
            cfg.reorder_depth = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--reorder-patience" && i + 1 < argc)
            cfg.reorder_patience = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--quiet") quiet = true;
        else if (in_path == nullptr) in_path = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (in_path == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <packets.gz> [--unwrap out.gz] [--reorder-depth N]\n"
                     "                       [--reorder-patience N] [--quiet]\n",
                     argv[0]);
        return 2;
    }

    Recovered sink;
    sink.verbose = !quiet;
    if (unwrap_path != nullptr) {
        sink.out = gzopen(unwrap_path, "wb");
        if (sink.out == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", unwrap_path);
            return 1;
        }
    }

    mold::Sequencer<Recovered> seq{cfg};
    try {
        Reader reader(in_path);
        std::vector<uint8_t> packet;
        while (reader.next(packet)) {
            seq.on_packet(packet.data(), packet.size(), sink);
        }
        // Anything still held out of order was never reachable. A gap open at
        // the end of the stream is still a gap.
        seq.flush(sink);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (sink.out != nullptr) gzclose(sink.out);
        return 1;
    }
    if (sink.out != nullptr) gzclose(sink.out);

    const mold::SequencerStats& s = seq.stats();
    std::printf("session               %s\n", seq.session());
    std::printf("packets               %" PRIu64 "\n", s.packets);
    std::printf("messages delivered    %" PRIu64 "\n", s.messages);
    std::printf("heartbeats            %" PRIu64 "\n", s.heartbeats);
    std::printf("---- damage ----------------------------\n");
    std::printf("gaps declared         %" PRIu64 "\n", s.gaps);
    std::printf("messages lost         %" PRIu64 "\n", s.messages_lost);
    std::printf("duplicate packets     %" PRIu64 "\n", s.duplicate_packets);
    std::printf("duplicate messages    %" PRIu64 "\n", s.duplicate_messages);
    std::printf("reordered packets     %" PRIu64 "\n", s.reordered_packets);
    std::printf("truncated packets     %" PRIu64 "\n", s.truncated_packets);
    std::printf("session changes       %" PRIu64 "\n", s.session_changes);
    std::printf("end of session        %s\n", seq.ended() ? "yes" : "NO (stream cut short)");

    // A non-zero exit when messages were lost, so a harness can assert on it
    // without parsing this table.
    return s.messages_lost > 0 ? 3 : 0;
}

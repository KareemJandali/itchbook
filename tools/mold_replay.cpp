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

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/mold/sequencer.hpp"
#include "itchbook/recover/gap_policy.hpp"

using namespace itchbook;

namespace {

struct Recovered {
    gzFile out = nullptr;
    uint64_t messages = 0;
    uint64_t gaps = 0;
    uint64_t lost = 0;
    bool verbose = true;

    // Optional: build a book behind the recovery policy, so the convergence
    // claim can be observed rather than asserted.
    book::Book* bk = nullptr;
    recover::GapTracker* gap = nullptr;

    void on_message(char type, const uint8_t* p, uint16_t len) {
        ++messages;
        if (out != nullptr) {
            uint8_t lb[2] = {static_cast<uint8_t>(len >> 8),
                             static_cast<uint8_t>(len & 0xff)};
            gzwrite(out, lb, 2);
            gzwrite(out, p, len);
        }
        if (bk == nullptr) return;
        // A halted book applies nothing further. Continuing to mutate a book
        // nobody is allowed to read wastes work and invites someone to read it.
        if (gap != nullptr && gap->halted()) return;

        // Whether the reference resolved is the recovery signal, and the book
        // already counts it. Taking the delta keeps that knowledge in one place
        // rather than re-deriving which message types carry a reference.
        const uint64_t before = bk->unknown_ref();
        book::apply(*bk, type, p);
        if (gap == nullptr) return;
        const bool carries_ref = (type == 'E' || type == 'C' || type == 'X' ||
                                  type == 'D' || type == 'U');
        if (carries_ref) gap->on_reference(bk->unknown_ref() == before);
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
        if (gap == nullptr) return;
        // The policy decides; the book does the discarding. A book rebuilt from
        // here holds no wrong orders, only missing ones.
        if (gap->on_gap(first, count) && bk != nullptr) bk->clear_orders();
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* unwrap_path = nullptr;
    mold::SequencerConfig cfg;
    recover::GapConfig gcfg;
    bool quiet = false;
    bool build_book = false;
    const char* book_out = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--unwrap" && i + 1 < argc) unwrap_path = argv[++i];
        else if (a == "--reorder-depth" && i + 1 < argc)
            cfg.reorder_depth = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--reorder-patience" && i + 1 < argc)
            cfg.reorder_patience = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--quiet") quiet = true;
        else if (a == "--book") build_book = true;
        else if (a == "--book-out" && i + 1 < argc) { book_out = argv[++i]; build_book = true; }
        else if (a == "--halt-on-gap") gcfg.policy = recover::Policy::Halt;
        else if (a == "--recover-after" && i + 1 < argc)
            gcfg.clean_messages_to_recover = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--max-gaps" && i + 1 < argc)
            gcfg.max_gaps_before_halt = std::strtoull(argv[++i], nullptr, 10);
        else if (in_path == nullptr) in_path = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (in_path == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <packets.gz> [--unwrap out.gz] [--reorder-depth N]\n"
                     "                       [--reorder-patience N] [--quiet]\n"
                     "                       [--book] [--halt-on-gap]\n"
                     "                       [--recover-after N] [--max-gaps N]\n"
                     "                       [--book-out levels.csv]\n",
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

    book::Book bk;
    recover::GapTracker gap{gcfg};
    if (build_book) {
        sink.bk = &bk;
        sink.gap = &gap;
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

    if (build_book) {
        const recover::GapStats& g = gap.stats();
        std::printf("---- book ------------------------------\n");
        std::printf("state                 %s\n", recover::to_string(gap.state()));
        std::printf("resting orders        %zu\n", bk.resting_orders());
        std::printf("resting shares        %" PRIu64 "\n", bk.resting_shares());
        std::printf("rebuilds              %" PRIu64 "\n", g.rebuilds);
        std::printf("recoveries            %" PRIu64 "\n", g.recoveries);
        std::printf("unknown refs post-gap %" PRIu64 "\n", g.unknown_refs_after_gap);
        std::printf("messages untrusted    %" PRIu64 "\n", g.messages_untrusted);
        std::printf("clean run at end      %" PRIu64 "\n", gap.clean_run());
        std::printf("\nA book in `recovering` holds no wrong orders, only missing "
                    "ones.\nIt is a subset of the truth, and it is never to be "
                    "quoted from.\n");

        // The whole final book, every level, both sides. Two runs that end
        // with identical files ended with identical books — which is the only
        // way to check "converged back to the truth" without trusting a pair
        // of summary totals that could coincide by accident.
        if (book_out != nullptr) {
            std::FILE* f = std::fopen(book_out, "w");
            if (f == nullptr) {
                std::fprintf(stderr, "error: cannot write %s\n", book_out);
                return 1;
            }
            std::fprintf(f, "side,price,shares,orders\n");
            for (char side : {'B', 'S'}) {
                std::vector<book::LevelView> levels;
                bk.top(side, 1u << 20, &levels);
                for (const book::LevelView& lv : levels) {
                    std::fprintf(f, "%c,%" PRId32 ",%" PRIu64 ",%" PRIu32 "\n", side,
                                 lv.price, lv.shares, lv.count);
                }
            }
            std::fclose(f);
        }
    }

    // A non-zero exit when messages were lost, so a harness can assert on it
    // without parsing this table.
    return s.messages_lost > 0 ? 3 : 0;
}

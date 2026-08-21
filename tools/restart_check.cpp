// restart_check — prove that dying and coming back changes nothing.
//
// The build plan asks for "state recovery: reconstruct position and open orders
// after a mid-day process restart". A restart is only worth having if it is
// transparent, and transparent is a claim with an exact test:
//
//     run the feed straight through
//     run it again, but snapshot at message N, throw the book away, restore,
//         and continue from N
//     the two must end identical
//
// Identical means every order at every price in queue order, plus the tape.
// Comparing top-of-book, or level totals, or the order count would all pass
// while queue priority was scrambled — and scrambled priority is precisely
// what a careless snapshot produces and what silently breaks every queue model
// in phase 6.
//
// This tool checks the MARKET half. The plan's line also covers position and
// our own open orders, which a book replay does not have — those are in
// recover/strategy_snapshot.hpp and are checked by test_restart.cpp, which
// drives a restored lane through a further fill and requires it to agree with
// the lane that never died.
//
// The unit test in test_restart.cpp cuts a synthetic stream at every offset.
// This does one cut on a real feed, which is the part that catches anything
// the synthetic stream did not happen to contain.
//
// Usage:
//   restart_check <feed.gz> [--at N] [--snapshot-file PATH] [--quiet]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/recover/halt.hpp"
#include "itchbook/recover/snapshot.hpp"

using namespace itchbook;

namespace {

// The whole book plus the tape, as text. A failure prints what differs.
std::string fingerprint(const book::Book& b) {
    std::string s;
    char buf[128];
    for (char side : {'B', 'S'}) {
        b.for_each_order(side, [&](const book::Order& o) {
            std::snprintf(buf, sizeof(buf), "%c%" PRIu64 "@%" PRId32 "x%u ", side, o.ref,
                          o.price, o.shares);
            s += buf;
        });
    }
    std::snprintf(buf, sizeof(buf),
                  "|vol=%" PRIu64 " tr=%" PRIu64 " not=%" PRIu64 " hid=%" PRIu64
                  " x=%" PRIu64 " unk=%" PRIu64 " o=%" PRId32 " h=%" PRId32
                  " l=%" PRId32 " c=%" PRId32,
                  b.volume(), b.trades(), b.notional(), b.hidden_volume(),
                  b.cross_volume(), b.unknown_ref(), b.open(), b.high(), b.low(),
                  b.close());
    s += buf;
    return s;
}

// Collects the feed once so both runs see byte-identical input.
struct Collect {
    std::vector<uint8_t> bytes;
    std::vector<uint32_t> offset;
    std::vector<uint16_t> length;
    void on_message(char, const uint8_t* p, uint16_t len) {
        offset.push_back(static_cast<uint32_t>(bytes.size()));
        length.push_back(len);
        bytes.insert(bytes.end(), p, p + len);
    }
    size_t size() const { return offset.size(); }
    const uint8_t* at(size_t i) const { return bytes.data() + offset[i]; }
};

void apply_range(book::Book* b, recover::HaltTracker* halt, const Collect& feed,
                 size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
        const uint8_t* p = feed.at(i);
        const char type = static_cast<char>(*p);
        const uint64_t ts = itch::timestamp(p);
        if (type == 'H') halt->on_trading_action(itch::trading_action::state(p), ts);
        if (type == 'Q') halt->on_cross(itch::cross_trade::cross_type(p), ts);
        book::apply(*b, type, p);
        halt->observe(*b, ts);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed_path = nullptr;
    size_t at = 0;                 // 0 = halfway
    const char* snap_path = nullptr;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--at" && i + 1 < argc) at = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--snapshot-file" && i + 1 < argc) snap_path = argv[++i];
        else if (a == "--quiet") quiet = true;
        else if (feed_path == nullptr) feed_path = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (feed_path == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <feed.gz> [--at N] [--snapshot-file PATH] [--quiet]\n",
                     argv[0]);
        return 2;
    }

    Collect feed;
    try {
        Reader reader(feed_path);
        parse(reader, feed);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (feed.size() < 2) {
        std::fprintf(stderr, "error: feed too short\n");
        return 1;
    }
    if (at == 0 || at >= feed.size()) at = feed.size() / 2;

    // The process that never died.
    book::Book continuous;
    recover::HaltTracker halt_a;
    apply_range(&continuous, &halt_a, feed, 0, feed.size());

    // The one that did. Run to the cut, snapshot, throw everything away,
    // restore, carry on.
    book::Book before;
    recover::HaltTracker halt_b;
    apply_range(&before, &halt_b, feed, 0, at);
    const recover::Snapshot snap = recover::capture(before, at, 0);

    // Through the file system, not just through memory: a capture/restore pair
    // can be correct while the serializer silently drops a field.
    recover::Snapshot loaded = snap;
    if (snap_path != nullptr) {
        if (!recover::write_file(snap_path, snap)) {
            std::fprintf(stderr, "error: cannot write %s\n", snap_path);
            return 1;
        }
        if (!recover::read_file(snap_path, &loaded)) {
            std::fprintf(stderr, "error: cannot read back %s\n", snap_path);
            return 1;
        }
    }

    book::Book after;
    recover::HaltTracker halt_c;
    recover::restore(&after, loaded);
    apply_range(&after, &halt_c, feed, loaded.next_sequence, feed.size());

    const std::string want = fingerprint(continuous);
    const std::string got = fingerprint(after);

    if (!quiet) {
        std::printf("messages              %zu\n", feed.size());
        std::printf("restart at            %zu\n", at);
        std::printf("orders in snapshot    %zu\n", snap.orders.size());
        std::printf("snapshot bytes        %zu\n", recover::serialize(snap).size());
        std::printf("through a file        %s\n", snap_path ? snap_path : "no");
        std::printf("resting orders        %zu (continuous) / %zu (restarted)\n",
                    continuous.resting_orders(), after.resting_orders());
        std::printf("halts seen            %" PRIu64 " (continuous) / %" PRIu64
                    " (restarted)\n",
                    halt_a.stats().halts, halt_c.stats().halts);
        std::printf("crossed while halted  %" PRIu64 "\n",
                    halt_a.stats().crossed_while_halted);
        std::printf("crossed while trading %" PRIu64 "  (must be 0)\n",
                    halt_a.stats().crossed_while_trading);
        std::printf("halt crosses          %" PRIu64 "\n", halt_a.stats().halt_crosses);
        std::printf("still crossed at open %" PRIu64 "  (must be 0: the auction "
                    "clears it)\n", halt_a.stats().crossed_at_resume);
        std::printf("halted for            %.1f s of feed time\n",
                    static_cast<double>(halt_a.stats().halted_ns) / 1e9);
    }

    // The conditional invariant, enforced. A crossed book while halted is
    // expected and counted; a crossed book while trading is a defect in the
    // book, the parser or the feed, and the whole reason for tracking session
    // state rather than asserting bid < ask unconditionally.
    if (halt_a.stats().crossed_while_trading > 0 ||
        halt_a.stats().crossed_at_resume > 0) {
        std::fprintf(stderr,
                     "\nFAIL: book crossed while trading (%" PRIu64
                     " observations, %" PRIu64 " of them at a resume)\n",
                     halt_a.stats().crossed_while_trading,
                     halt_a.stats().crossed_at_resume);
        return 1;
    }

    if (want != got) {
        std::fprintf(stderr, "\nFAIL: a restarted process disagrees with one that "
                             "never died\n");
        // Print the first difference rather than two enormous strings.
        size_t i = 0;
        while (i < want.size() && i < got.size() && want[i] == got[i]) ++i;
        const size_t from = i > 60 ? i - 60 : 0;
        std::fprintf(stderr, "  first difference at offset %zu\n", i);
        std::fprintf(stderr, "  continuous: ...%s\n", want.substr(from, 160).c_str());
        std::fprintf(stderr, "  restarted:  ...%s\n", got.substr(from, 160).c_str());
        return 1;
    }
    std::printf("\nOK: identical after restart — every order, every queue "
                "position, and the tape\n");
    return 0;
}

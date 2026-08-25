//
// split_replay_gate.cpp — phase 12.1's gate: with zero strategy orders, the
// split replayer must leave the book exactly where the phase-9 path leaves it.
//
//   ./split_replay_gate <feed.gz> [--limit N] [--max-report N]
//                       [--per-symbol-ref a.csv] [--per-symbol-split b.csv]
//
// Both paths run in ONE process over ONE feed, and the book the current message
// touched is compared after every single message. That is what makes this a
// per-message gate rather than an end-of-day one: a divergence that appears at
// message 40,000,000 and heals by the close is invisible to a comparison of
// final states, and healing divergences are the ones worth finding, because
// they are the ones a strategy would have traded through.
//
// Comparing all 8,700 books after each of 268 million messages is not
// affordable and is not necessary: a message can only change the book it is
// routed to, so comparing that one book is O(1) per message and misses nothing
// a full sweep would catch. The full sweep runs once at the end anyway, and so
// does a byte-comparison of the two per-symbol CSVs, written by the single
// writer in report.hpp so that the gate compares books rather than two
// transcriptions of books.
//
// Exit codes follow the house convention: 0 pass, 1 a real failure, 2 usage,
// 3 a refusal -- the run could not guarantee the reference partition, so its
// numbers must not be believed either way.
//
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/book/book_set.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/book/report.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/replay/split.hpp"

namespace {

// Everything a book can say about itself that a divergence could show up in.
// Deliberately wider than the per-symbol CSV: the CSV is the end-state check,
// and this one has to catch a difference the moment it appears.
struct Snap {
    size_t resting_orders = 0;
    uint64_t resting_shares = 0;
    uint64_t volume = 0;
    uint64_t notional = 0;
    uint64_t trades = 0;
    uint64_t hidden_volume = 0;
    uint64_t cross_volume = 0;
    uint64_t unknown_ref = 0;
    uint64_t locate_mismatch = 0;
    uint64_t adds = 0;
    uint64_t off_band_adds = 0;
    uint64_t recentres = 0;
    int32_t open = 0, high = 0, low = 0, close = 0;
    int32_t best_bid = -1, best_ask = -1;
    size_t overflow_levels = 0;
    char trading_state = 0, system_event = 0;
};

Snap snap_of(const itchbook::book::Book& b) {
    Snap s;
    s.resting_orders = b.resting_orders();
    s.resting_shares = b.resting_shares();
    s.volume = b.volume();
    s.notional = b.notional();
    s.trades = b.trades();
    s.hidden_volume = b.hidden_volume();
    s.cross_volume = b.cross_volume();
    s.unknown_ref = b.unknown_ref();
    s.locate_mismatch = b.locate_mismatch();
    s.adds = b.adds();
    s.off_band_adds = b.off_band_adds();
    s.recentres = b.recentres();
    s.open = b.open();
    s.high = b.high();
    s.low = b.low();
    s.close = b.close();
    int32_t v = 0;
    s.best_bid = b.best_bid(&v) ? v : -1;
    s.best_ask = b.best_ask(&v) ? v : -1;
    s.overflow_levels = b.overflow_levels();
    s.trading_state = b.trading_state();
    s.system_event = b.system_event();
    return s;
}

// Returns the name of the first field that differs, or nullptr when identical.
// Named rather than boolean: "the books differ" sends you reading the whole
// replayer, "notional differs" sends you to the one line that records it.
const char* first_difference(const Snap& a, const Snap& b) {
#define FIELD(f) if (a.f != b.f) return #f;
    FIELD(resting_orders) FIELD(resting_shares) FIELD(volume) FIELD(notional)
    FIELD(trades) FIELD(hidden_volume) FIELD(cross_volume) FIELD(unknown_ref)
    FIELD(locate_mismatch) FIELD(adds) FIELD(off_band_adds) FIELD(recentres)
    FIELD(open) FIELD(high) FIELD(low) FIELD(close)
    FIELD(best_bid) FIELD(best_ask) FIELD(overflow_levels)
    FIELD(trading_state) FIELD(system_event)
#undef FIELD
    return nullptr;
}

void print_snap(const char* tag, const Snap& s) {
    std::printf("    %-6s orders=%zu shares=%" PRIu64 " vol=%" PRIu64
                " notional=%" PRIu64 " trades=%" PRIu64 " hidden=%" PRIu64
                " cross=%" PRIu64 " unknown=%" PRIu64 "\n",
                tag, s.resting_orders, s.resting_shares, s.volume, s.notional,
                s.trades, s.hidden_volume, s.cross_volume, s.unknown_ref);
    std::printf("    %-6s adds=%" PRIu64 " offband=%" PRIu64 " ohlc=%d/%d/%d/%d"
                " bid=%d ask=%d state=%c\n",
                "", s.adds, s.off_band_adds, s.open, s.high, s.low, s.close,
                s.best_bid, s.best_ask, s.trading_state ? s.trading_state : '-');
}

bool files_identical(const char* a, const char* b) {
    std::FILE* fa = std::fopen(a, "rb");
    std::FILE* fb = std::fopen(b, "rb");
    if (fa == nullptr || fb == nullptr) {
        if (fa) std::fclose(fa);
        if (fb) std::fclose(fb);
        return false;
    }
    bool same = true;
    char ba[65536], bb[65536];
    for (;;) {
        size_t na = std::fread(ba, 1, sizeof(ba), fa);
        size_t nb = std::fread(bb, 1, sizeof(bb), fb);
        if (na != nb || std::memcmp(ba, bb, na) != 0) { same = false; break; }
        if (na == 0) break;
    }
    std::fclose(fa);
    std::fclose(fb);
    return same;
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed = nullptr;
    uint64_t limit = 0;
    uint64_t max_report = 5;
    const char* csv_ref = nullptr;
    const char* csv_split = nullptr;
    const char* json_out = nullptr;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--limit") {
            limit = std::strtoull(next("--limit"), nullptr, 10);
        } else if (a == "--max-report") {
            max_report = std::strtoull(next("--max-report"), nullptr, 10);
        } else if (a == "--per-symbol-ref") {
            csv_ref = next("--per-symbol-ref");
        } else if (a == "--per-symbol-split") {
            csv_split = next("--per-symbol-split");
        } else if (a == "--json") {
            json_out = next("--json");
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return 2;
        } else {
            feed = argv[i];
        }
    }
    if (feed == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <feed.gz> [--limit N] [--max-report N]\n"
                     "       [--per-symbol-ref a.csv] [--per-symbol-split b.csv] [--json out.json]\n",
                     argv[0]);
        return 2;
    }

    // Sized exactly as book_replay sizes a full day. BookSet's own defaults
    // leave band_levels at 0, which means dense levels up to kMaxDenseLevels --
    // 1<<22 per side, per symbol -- and on a real day that is not a slow run,
    // it is an out-of-memory kill.
    itchbook::book::BookSet ref_set(size_t{1} << 23, 100, 20, 512);
    itchbook::book::BookSet split_set(size_t{1} << 23, 100, 20, 512);
    itchbook::replay::SplitReplayer split(split_set);

    itchbook::Reader rd(feed);
    std::vector<uint8_t> msg;

    uint64_t read = 0;
    uint64_t divergences = 0;
    uint64_t first_divergence_at = 0;

    while (rd.next(msg)) {
        const uint8_t* p = msg.data();
        const char type = static_cast<char>(p[0]);
        ++read;

        itchbook::book::apply(ref_set, type, p);
        split.apply(type, p);

        // Only a routed message can have changed a book, and it can only have
        // changed the one it was routed to.
        if (type != 'S' && itchbook::book::modelled(type)) {
            const uint16_t loc = itchbook::itch::stock_locate(p);
            const itchbook::book::Book* a = ref_set.peek(loc);
            const itchbook::book::Book* b = split_set.peek(loc);
            if ((a == nullptr) != (b == nullptr)) {
                if (divergences++ < max_report) {
                    std::printf("DIVERGENCE msg #%" PRIu64 " locate %u type '%c': "
                                "one path built a book and the other did not\n",
                                read, loc, type);
                }
                if (first_divergence_at == 0) first_divergence_at = read;
            } else if (a != nullptr) {
                const Snap sa = snap_of(*a);
                const Snap sb = snap_of(*b);
                if (const char* field = first_difference(sa, sb)) {
                    if (divergences++ < max_report) {
                        std::printf("DIVERGENCE msg #%" PRIu64 " locate %u type '%c'"
                                    "  field: %s\n", read, loc, type, field);
                        print_snap("phase9", sa);
                        print_snap("split", sb);
                    } else {
                        ++divergences;
                    }
                    if (first_divergence_at == 0) first_divergence_at = read;
                }
            }
        }

        if (limit != 0 && read >= limit) break;
    }

    const auto& c = split.counters();
    std::printf("\n=== split replayer, phase 12.1 ===\n");
    std::printf("%-30s %16" PRIu64 "\n", "messages", c.messages);
    std::printf("%-30s %16" PRIu64 "\n", "applied as state", c.state_applied);
    std::printf("%-30s %16" PRIu64 "\n", "aggressors ('E')", c.aggressors);
    std::printf("%-30s %16" PRIu64 "\n", "aggressor shares", c.aggressor_shares);
    std::printf("%-30s %16" PRIu64 "\n", "strategy shares taken", c.strategy_shares_taken);
    std::printf("%-30s %16" PRIu64 "\n", "historical orders ahead", c.historical_ahead);
    std::printf("%-30s %16" PRIu64 "\n", "aggressor unknown refs", c.aggressor_unknown_ref);
    std::printf("%-30s %16" PRIu64 "\n", "partition violations", c.partition_violations);

    // A full sweep at the end. The per-message check should make this
    // redundant; it runs because "should" is not a verification, and because a
    // book that no message ever routed to is not covered by the check above.
    uint64_t end_state_diffs = 0;
    for (uint32_t loc = 0; loc < (uint32_t{1} << 16); ++loc) {
        const itchbook::book::Book* a = ref_set.peek(static_cast<uint16_t>(loc));
        const itchbook::book::Book* b = split_set.peek(static_cast<uint16_t>(loc));
        if ((a == nullptr) != (b == nullptr)) { ++end_state_diffs; continue; }
        if (a == nullptr) continue;
        if (first_difference(snap_of(*a), snap_of(*b)) != nullptr) ++end_state_diffs;
    }

    std::printf("\n=== gate ===\n");
    std::printf("%-30s %16" PRIu64 "\n", "messages compared", read);
    std::printf("%-30s %16" PRIu64 "\n", "per-message divergences", divergences);
    std::printf("%-30s %16" PRIu64 "\n", "end-state book differences", end_state_diffs);

    bool csv_ok = true;
    if (csv_ref != nullptr && csv_split != nullptr) {
        if (!itchbook::book::write_per_symbol(ref_set, csv_ref) ||
            !itchbook::book::write_per_symbol(split_set, csv_split)) {
            return 1;
        }
        csv_ok = files_identical(csv_ref, csv_split);
        std::printf("%-30s %16s\n", "per-symbol CSV",
                    csv_ok ? "byte-identical" : "DIFFERS");
    } else {
        std::printf("%-30s %16s\n", "per-symbol CSV", "not requested");
    }

    if (json_out != nullptr) {
        std::FILE* j = std::fopen(json_out, "w");
        if (j == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", json_out);
            return 1;
        }
        // Every input to the verdict, so a reader can re-derive it instead of
        // trusting the word PASS.
        std::fprintf(j,
            "{\n"
            "  \"feed\": \"%s\",\n"
            "  \"messages\": %" PRIu64 ",\n"
            "  \"state_applied\": %" PRIu64 ",\n"
            "  \"aggressors\": %" PRIu64 ",\n"
            "  \"aggressor_shares\": %" PRIu64 ",\n"
            "  \"strategy_shares_taken\": %" PRIu64 ",\n"
            "  \"historical_orders_ahead\": %" PRIu64 ",\n"
            "  \"aggressor_unknown_refs\": %" PRIu64 ",\n"
            "  \"partition_violations\": %" PRIu64 ",\n"
            "  \"per_message_divergences\": %" PRIu64 ",\n"
            "  \"end_state_book_differences\": %" PRIu64 ",\n"
            "  \"per_symbol_csv_identical\": %s\n"
            "}\n",
            feed, read, c.state_applied, c.aggressors, c.aggressor_shares,
            c.strategy_shares_taken, c.historical_ahead, c.aggressor_unknown_ref,
            c.partition_violations, divergences, end_state_diffs,
            (csv_ref != nullptr && csv_split != nullptr)
                ? (csv_ok ? "true" : "false") : "null");
        std::fclose(j);
        std::printf("%-30s %16s\n", "artifact", json_out);
    }

    // The partition is a precondition on believing anything above, so it is
    // checked before the verdict and refuses rather than fails.
    if (!split.partition_held()) {
        std::printf("\nREFUSED: %" PRIu64 " historical reference(s) had bit 63 set.\n"
                    "The strategy reference space is not disjoint from the feed's, so a\n"
                    "collision would corrupt a price level silently. No verdict.\n",
                    c.partition_violations);
        return 3;
    }
    // A gate that passes without having run is the failure this repository has
    // already shipped once, in a receiver that returned before reading its own
    // counters. Zero messages is not a clean sheet.
    if (read == 0 || c.aggressors == 0) {
        std::printf("\nREFUSED: %" PRIu64 " messages, %" PRIu64 " aggressors.\n"
                    "Nothing was exercised, so nothing was proven.\n",
                    read, c.aggressors);
        return 3;
    }
    if (divergences != 0 || end_state_diffs != 0 || !csv_ok) {
        std::printf("\nFAIL: first divergence at message %" PRIu64 ".\n", first_divergence_at);
        return 1;
    }
    std::printf("\nPASS: %" PRIu64 " messages, %" PRIu64 " executions replayed as crossing\n"
                "events, book identical to the phase-9 path after every one.\n",
                read, c.aggressors);
    return 0;
}

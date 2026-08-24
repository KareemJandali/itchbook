#pragma once
//
// report.hpp — one row per security, written by exactly one piece of code.
//
// This lived inside book_replay.cpp until phase 10.6 needed a second writer.
// The determinism gate compares a book built synchronously from a file against
// one built by two threads over a socket, and requires the two to be BYTE
// IDENTICAL. A second implementation of this formatting, however careful, would
// have made that comparison meaningless in both directions: it would fail on a
// trailing zero that differs, or -- much worse -- pass while both copies made
// the same mistake. There is one writer, and the gate compares books rather
// than transcriptions of books.
//
// The columns are the same ones a single-symbol run reports, so a row here can
// also be diffed against `--symbol X` on the same feed. That is what stands
// between "the routing works" and "the routing appears to work".
//
// Two overflow columns, deliberately. `overflow_levels` is the map's size when
// the run stopped, and on a complete session it is zero for every symbol --
// the book empties and pop() erases each level behind it. `peak_overflow_levels`
// is the high-water mark, and it is the one that can be set against peak RSS,
// which is also a high-water mark. Keeping both is what makes the zero legible
// as a fact about WHEN the first is sampled rather than about overflow.
//
#include <cinttypes>
#include <cstdio>
#include <string>

#include "itchbook/book/book_set.hpp"

namespace itchbook::book {

inline bool write_per_symbol(const BookSet& set, const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "error: cannot write %s\n", path);
        return false;
    }
    std::fputs("locate,symbol,directoried,resting_orders,resting_shares,volume,notional,"
               "trades,hidden_volume,cross_volume,open,high,low,close,best_bid,best_ask,"
               "unknown_refs,locate_mismatch,overflow_levels,peak_overflow_levels,"
               "trading_state,system_event,"
               "operational_halts,broken_trades,tradable,adds,off_band_adds,recentres\n", f);
    set.for_each_book([&](uint16_t locate, const Book& b, const SymbolInfo& info) {
        int32_t bid = 0;
        int32_t ask = 0;
        const bool have_bid = b.best_bid(&bid);
        const bool have_ask = b.best_ask(&ask);
        // Absent is empty, not a sentinel. The single-symbol summary learned
        // this the hard way on a real day (see book_replay's write_json): a -1
        // read as a price manufactures a disagreement out of two spellings of
        // "there isn't one", and this file exists to be compared against that
        // one.
        auto opt_px = [](int32_t v) { return v < 0 ? std::string() : std::to_string(v); };
        std::fprintf(f,
                     "%u,%s,%s,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                     ",%" PRIu64 ",%s,%s,%s,%s,%s,%s,%" PRIu64 ",%" PRIu64 ",%zu,%zu,%c,%c"
                     ",%" PRIu64 ",%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                     locate, info.symbol, info.directoried ? "yes" : "no",
                     b.resting_orders(), b.resting_shares(), b.volume(), b.notional(),
                     b.trades(), b.hidden_volume(), b.cross_volume(),
                     opt_px(b.open()).c_str(), opt_px(b.high()).c_str(),
                     opt_px(b.low()).c_str(), opt_px(b.close()).c_str(),
                     (have_bid ? std::to_string(bid) : std::string()).c_str(),
                     (have_ask ? std::to_string(ask) : std::string()).c_str(),
                     b.unknown_ref(), b.locate_mismatch(), b.overflow_levels(),
                     b.peak_overflow_levels(),
                     b.trading_state() == 0 ? '-' : b.trading_state(),
                     b.system_event() == 0 ? '-' : b.system_event(),
                     info.operational_halts, info.broken_trades,
                     set.tradable(locate) ? "yes" : "no",
                     b.adds(), b.off_band_adds(), b.recentres());
    });
    const bool bad = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !bad;
}

}  // namespace itchbook::book

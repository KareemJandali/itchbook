// test_book_set — every symbol in one process.
//
// The container is small; what it has to get right is not. Three of these
// tests exist because the failure they guard against is silent: a book that
// never learns the session opened, 8,699 summaries with no symbol on them, and
// a locate nothing in the directory ever named.
#include <cstdint>
#include <string>

#include "itchbook/book/book_set.hpp"
#include "tests/check.hpp"

using itchbook::book::BookSet;
using itchbook::book::SymbolInfo;

namespace {

void test_books_are_built_on_demand_not_up_front() {
    BookSet set;
    CHECK_EQ(set.books(), 0u);
    CHECK(set.peek(42) == nullptr);

    set.at(42).add(1, 'B', 1000000, 100);
    CHECK_EQ(set.books(), 1u);
    CHECK(set.peek(42) != nullptr);
    CHECK(set.peek(43) == nullptr);

    // Asking again does not build a second one.
    set.at(42).add(2, 'B', 999900, 200);
    CHECK_EQ(set.books(), 1u);
    CHECK_EQ(set.peek(42)->resting_orders(), 2u);

    // A locate that never appears costs a null pointer and a SymbolInfo. If
    // this ever constructs 65,536 books the process will say so long before
    // the assertion does.
    CHECK(set.peek(65535) == nullptr);
}

void test_two_symbols_share_storage_and_nothing_else() {
    BookSet set;
    set.at(1).add(10, 'B', 1000000, 100);
    set.at(1).add(11, 'S', 1000200, 300);
    set.at(2).add(20, 'B', 500000, 400);

    CHECK_EQ(set.peek(1)->resting_orders(), 2u);
    CHECK_EQ(set.peek(2)->resting_orders(), 1u);
    // One pool for the pair. This is the whole point of the arrangement: the
    // set's resting count is the market's, each book's is its own.
    CHECK_EQ(set.resting_orders(), 3u);
    CHECK_EQ(set.storage().refs.size(), 3u);

    set.at(1).execute(10, 100);
    CHECK_EQ(set.peek(1)->volume(), 100u);
    CHECK_EQ(set.peek(2)->volume(), 0u);      // symbol 2 did not trade
    CHECK_EQ(set.resting_orders(), 2u);

    int32_t px = 0;
    CHECK(set.peek(2)->best_bid(&px));
    CHECK_EQ(px, 500000);
}

void test_the_directory_is_kept_and_the_padding_is_not() {
    BookSet set;
    CHECK_EQ(set.directory_entries(), 0u);
    CHECK(!set.info(7).directoried);

    set.set_directory(7, "MSFT    ", 8, 'Q', ' ', 100);
    CHECK_EQ(set.directory_entries(), 1u);
    CHECK(set.info(7).directoried);
    // Trailing spaces are the wire's, not the symbol's. A summary keyed on
    // "MSFT    " is a summary nobody can grep.
    CHECK_STR(set.info(7).symbol, "MSFT");
    CHECK_EQ(set.info(7).market_category, 'Q');
    CHECK_EQ(set.info(7).financial_status, ' ');
    CHECK_EQ(set.info(7).round_lot, 100u);

    // A symbol that fills the field keeps all eight characters.
    set.set_directory(8, "ABCDEFGH", 8, 'N', 'D', 1);
    CHECK_STR(set.info(8).symbol, "ABCDEFGH");
    CHECK_EQ(set.directory_entries(), 2u);

    // A second 'R' for a locate updates it without double-counting.
    set.set_directory(7, "MSFT    ", 8, 'Q', 'D', 100);
    CHECK_EQ(set.directory_entries(), 2u);
    CHECK_EQ(set.info(7).financial_status, 'D');
}

void test_a_message_for_an_undirectoried_locate_is_counted_not_ignored() {
    // On a well-formed day every 'R' precedes every order. If that is ever not
    // true, the alternative to counting it is guessing, and this project does
    // not do that quietly.
    BookSet set;
    set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);

    set.at(1).add(1, 'B', 1000000, 100);
    CHECK_EQ(set.undirectoried_messages(), 0u);

    set.at(2).add(2, 'B', 1000000, 100);
    set.at(2).add(3, 'B', 1000000, 100);
    CHECK_EQ(set.undirectoried_messages(), 2u);

    // Once the directory catches up, the count stops growing.
    set.set_directory(2, "BBB     ", 8, 'Q', ' ', 100);
    set.at(2).add(4, 'B', 1000000, 100);
    CHECK_EQ(set.undirectoried_messages(), 2u);
}

void test_the_session_belongs_to_the_market_not_to_a_symbol() {
    // 'S' carries stock locate 0. Routing it by locate would file the whole
    // market's session state under whichever symbol owns that code and leave
    // every other summary blank — 8,699 wrong outputs and no crash.
    BookSet set;
    set.at(1).add(1, 'B', 1000000, 100);

    set.set_system_event('Q');                 // market hours
    CHECK_EQ(set.system_event(), 'Q');
    CHECK_EQ(set.peek(1)->system_event(), 'Q');  // a book that already existed

    // ...and one that did not. A symbol whose first order arrives at 10:00
    // must not think the market never opened.
    set.at(2).add(2, 'B', 500000, 100);
    CHECK_EQ(set.peek(2)->system_event(), 'Q');

    set.set_system_event('C');                 // close
    CHECK_EQ(set.peek(1)->system_event(), 'C');
    CHECK_EQ(set.peek(2)->system_event(), 'C');
    CHECK_EQ(set.system_event(), 'C');
}

// ---- the dense band ---------------------------------------------------------
//
// The band is a LOCALITY knob and nothing else. Where a level is stored -- a
// slot in the dense array or a node in the cold std::map -- must not change any
// number the book reports, because the two paths are merged in price order on
// the way out. That property is what lets the width be chosen on a memory
// budget instead of argued about, and it is asserted here rather than assumed.

void fill_a_drifting_book(BookSet& set, uint16_t locate, int32_t start, int steps) {
    uint64_t ref = static_cast<uint64_t>(locate) * 1000000 + 1;
    int32_t centre = start;
    for (int i = 0; i < steps; ++i) {
        set.at(locate).add(ref++, 'B', centre - 100, 100);
        set.at(locate).add(ref++, 'S', centre + 100, 100);
        centre += 100;                       // one tick per step: the band ages
    }
}

void test_a_pool_per_symbol_builds_the_same_book_and_costs_more() {
    // Phase 9.9 measures whether sharing the pool is worth anything. For that
    // measurement to mean anything, the two arrangements have to produce the
    // same book -- otherwise the timing compares two different days -- and the
    // per-book variant has to be honestly priced, which means counting every
    // pool's capacity rather than one of them.
    auto fill = [](BookSet& set) {
        for (uint16_t locate = 1; locate <= 20; ++locate) {
            set.set_directory(locate, "SYM     ", 8, 'Q', ' ', 100);
            itchbook::book::Book& b = set.at(locate);
            uint64_t ref = static_cast<uint64_t>(locate) * 100000;
            for (int i = 0; i < 50; ++i) {
                b.add(ref++, 'B', 1000000 - i * 100, 100);
                b.add(ref++, 'S', 1000100 + i * 100, 100);
            }
        }
    };

    // The chunk size is the whole story, so it is the default one: 4,096
    // orders, which phase 4 chose because a smaller first slab kept the page
    // faults off the hot path. Twenty symbols of a hundred orders each fit in
    // ONE such chunk shared, and need TWENTY of them apiece.
    BookSet shared(1u << 12, 100, 20, 64, itchbook::book::PoolMode::Shared, 4096, 4096);
    BookSet apiece(1u << 12, 100, 20, 64, itchbook::book::PoolMode::PerBook, 4096, 4096);
    fill(shared);
    fill(apiece);

    CHECK_EQ(shared.pools(), 1u);
    CHECK_EQ(apiece.pools(), 20u);
    CHECK_EQ(shared.resting_orders(), apiece.resting_orders());

    for (uint16_t locate = 1; locate <= 20; ++locate) {
        const itchbook::book::Book& a = *shared.peek(locate);
        const itchbook::book::Book& b = *apiece.peek(locate);
        CHECK_EQ(a.resting_orders(), b.resting_orders());
        CHECK_EQ(a.resting_shares(), b.resting_shares());
        int32_t pa = 0;
        int32_t pb = 0;
        CHECK_EQ(a.best_bid(&pa), b.best_bid(&pb));
        CHECK_EQ(pa, pb);
    }

    // Twenty free lists means twenty symbols each rounding up to a whole chunk.
    // At the default 4,096 that is 81,920 orders of capacity to hold 2,000, and
    // at 8,906 symbols it is why the per-book variant is only runnable at all
    // with a shrunken chunk -- which is itself part of what phase 9.9 measures,
    // and why the comparison cannot be reported as if the two arrangements were
    // otherwise identical.
    CHECK_EQ(shared.pool_capacity(), 4096u);
    CHECK_EQ(apiece.pool_capacity(), 20u * 4096u);

    // ...and the waste is not intrinsic to the arrangement, it is intrinsic to
    // the chunk. Shrink it below a symbol's order count and per-book is the
    // tighter of the two, because a shared pool's doubling overshoots.
    BookSet small_shared(1u << 12, 100, 20, 64, itchbook::book::PoolMode::Shared, 64, 4096);
    BookSet small_apiece(1u << 12, 100, 20, 64, itchbook::book::PoolMode::PerBook, 64, 4096);
    fill(small_shared);
    fill(small_apiece);
    CHECK(small_apiece.pool_capacity() < small_shared.pool_capacity());
}

void test_band_width_changes_nothing_the_book_reports() {
    // Same messages, three wildly different budgets. A width of 4 slots cannot
    // hold a book that walks 300 ticks; a width of 4096 holds all of it. The
    // reported state has to be identical anyway.
    const int32_t start = 1000000;
    const int steps = 300;
    struct Result { size_t orders; uint64_t shares; int32_t bid; int32_t ask; };
    Result seen[3];
    const size_t widths[3] = {4, 128, 4096};

    for (int w = 0; w < 3; ++w) {
        BookSet set(1u << 12, 100, 20, widths[w]);
        set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
        fill_a_drifting_book(set, 1, start, steps);
        const itchbook::book::Book& b = *set.peek(1);
        int32_t bid = 0;
        int32_t ask = 0;
        CHECK(b.best_bid(&bid));
        CHECK(b.best_ask(&ask));
        seen[w] = Result{b.resting_orders(), b.resting_shares(), bid, ask};
    }
    for (int w = 1; w < 3; ++w) {
        CHECK_EQ(seen[w].orders, seen[0].orders);
        CHECK_EQ(seen[w].shares, seen[0].shares);
        CHECK_EQ(seen[w].bid, seen[0].bid);
        CHECK_EQ(seen[w].ask, seen[0].ask);
    }

    // ...and the narrow band really did push work into the overflow map, or the
    // comparison above proved nothing.
    BookSet narrow(1u << 12, 100, 20, 4);
    narrow.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    fill_a_drifting_book(narrow, 1, start, steps);
    CHECK(narrow.peek(1)->off_band_adds() > 0);
    CHECK(narrow.peek(1)->overflow_levels() > 0);

    BookSet wide(1u << 12, 100, 20, 4096);
    wide.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    fill_a_drifting_book(wide, 1, start, steps);
    CHECK(wide.peek(1)->off_band_adds() < narrow.peek(1)->off_band_adds());
}

void test_the_band_waits_for_a_two_sided_quote() {
    // The first order of an ITCH day arrives around 04:00 and may be a stub
    // quote -- 77.6% of symbols posted one at or above $100,000 on the day this
    // project validates against. A band centred there covers nothing. So the
    // first order does not open the band; the first order on each side does.
    BookSet set(1u << 12, 100, 20, 64);
    set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    itchbook::book::Book& b = set.at(1);

    b.add(1, 'S', 1999999900, 100);      // $199,999.99 -- the stub
    CHECK_EQ(b.off_band_adds(), 1u);     // nowhere to put it yet

    b.add(2, 'B', 1000000, 100);         // $100.00 -- now there are two sides
    b.add(3, 'B', 999900, 100);
    b.add(4, 'S', 1000100, 100);

    // The band opened between $100.00 and $199,999.99, which is a terrible
    // centre -- and the point is that the book is still correct, just slow:
    // everything it cannot index lands in the overflow map and is still found.
    int32_t px = 0;
    CHECK(b.best_bid(&px));
    CHECK_EQ(px, 1000000);
    CHECK(b.best_ask(&px));
    CHECK_EQ(px, 1000100);
    CHECK_EQ(b.resting_orders(), 4u);
}

void test_a_recentre_keeps_price_time_priority() {
    // Re-centring rebuilds both dense arrays and re-indexes every resting
    // order. If it reordered a queue, every fill in phase 6 downstream of it
    // would be wrong, and nothing in the summary would show it -- the shares
    // are all still there, just in the wrong sequence.
    BookSet set(1u << 12, 100, 20, 8);
    set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    itchbook::book::Book& b = set.at(1);
    b.add(1, 'B', 1000000, 100);
    b.add(2, 'S', 1000100, 100);

    // Three orders at one price, in a known arrival order.
    b.add(10, 'B', 999900, 100);
    b.add(11, 'B', 999900, 200);
    b.add(12, 'B', 999900, 300);

    // Drag the touch far enough that the 8-slot band is hopeless, past the
    // window where the policy is allowed to look.
    uint64_t ref = 100;
    for (int i = 0; i < 1200; ++i) {
        b.add(ref++, 'B', 1000000 + i * 100, 100);
        b.add(ref++, 'S', 1000200 + i * 100, 100);
    }
    CHECK_EQ(b.recentres(), 1u);

    // The queue at 999900 must still be 10, then 11, then 12. An execution
    // takes from the front, so executing 100 shares must remove ref 10.
    CHECK(b.find(10) != nullptr);
    b.execute(10, 100);
    CHECK(b.find(10) == nullptr);
    CHECK(b.find(11) != nullptr);
    CHECK(b.find(12) != nullptr);
    CHECK_EQ(b.shares_at('B', 999900), 500u);
}

void test_an_odd_spread_does_not_send_the_book_to_overflow() {
    // The band is centred on (bid + ask) / 2, and across a one-tick spread that
    // midpoint sits BETWEEN two ticks. index_of() addresses a slot as
    // (price - base_) / tick_ and sends anything with a remainder to overflow,
    // so an unsnapped base made every real price off-grid and the whole book
    // fell through to the std::map -- correct, and quietly not fast.
    //
    // It only bit on odd spreads, which is why a feed whose first quote
    // happened to be two ticks wide looked perfectly healthy.
    for (int spread : {1, 2, 3, 7}) {
        BookSet set(1u << 12, 100, 20, 1024);
        set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
        itchbook::book::Book& b = set.at(1);
        b.add(1, 'B', 1000000, 100);
        b.add(2, 'S', 1000000 + spread * 100, 100);
        for (uint64_t i = 0; i < 200; ++i) {
            b.add(10 + i, 'B', 1000000 - static_cast<int32_t>(i) * 100, 100);
        }
        // One: the stub-avoiding wait means the very first order predates the
        // band. Everything after it is indexable whatever the spread.
        CHECK_EQ(b.off_band_adds(), 1u);
    }
}

void test_a_band_that_is_working_is_never_moved() {
    BookSet set(1u << 12, 100, 20, 1024);
    set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    itchbook::book::Book& b = set.at(1);
    uint64_t ref = 1;
    for (int i = 0; i < 2000; ++i) {
        b.add(ref++, 'B', 1000000 - (i % 50) * 100, 100);
        b.add(ref++, 'S', 1000100 + (i % 50) * 100, 100);
    }
    CHECK_EQ(b.recentres(), 0u);
    CHECK_EQ(b.off_band_adds(), 1u);   // only the very first, before both sides
}

void test_tradability_comes_from_three_places_not_one() {
    // 'H' has been per symbol since phase 7. 'h' and 'W' were nowhere: the book
    // derived tradability from 'H' alone, so a symbol could be operationally
    // halted, or the whole market could be through a circuit breaker, and every
    // book would still call itself tradable. One symbol on one quiet day
    // reaches neither, which is why it went unnoticed for eight phases.
    BookSet set;
    set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    set.set_directory(2, "BBB     ", 8, 'Q', ' ', 100);
    set.at(1).add(1, 'B', 1000000, 100);
    set.at(2).add(2, 'B', 1000000, 100);

    // Nothing has said the symbol may trade yet, so it may not. Unknown is not
    // the same as permitted.
    CHECK(!set.tradable(1));

    set.at(1).set_trading_state('T');
    set.at(2).set_trading_state('T');
    CHECK(set.tradable(1));
    CHECK(set.tradable(2));

    // 'h' — venue-level, one symbol only.
    set.set_operational_halt(1, 'H');
    CHECK(!set.tradable(1));
    CHECK(set.tradable(2));            // its neighbour is unaffected
    CHECK_EQ(set.operational_halts(), 1u);
    CHECK_EQ(set.symbols_operationally_halted(), 1u);

    // A repeated halt is not a second halt.
    set.set_operational_halt(1, 'H');
    CHECK_EQ(set.operational_halts(), 1u);

    set.set_operational_halt(1, 'T');
    CHECK(set.tradable(1));
    CHECK_EQ(set.symbols_operationally_halted(), 0u);
    CHECK_EQ(set.operational_halts(), 1u);   // the count survives the resume

    // 'W' — market-wide, so it takes everything down at once.
    set.set_mwcb_breached('1');
    CHECK(!set.tradable(1));
    CHECK(!set.tradable(2));
    CHECK_EQ(set.mwcb_level_breached(), '1');

    // A locate that never got a book is not tradable, and asking must not
    // build one for it.
    CHECK(!set.tradable(9));
    CHECK(set.peek(9) == nullptr);
}

void test_a_broken_trade_is_counted_and_not_applied() {
    // 'B' busts a print, which means a day's volume can be revised after the
    // fact. Undoing it needs the match number of every trade the day printed,
    // and the file this project validates against contains no 'B' at all. So
    // the number is reported instead: a daily bar that disagrees with a
    // vendor's can then be explained rather than argued with.
    BookSet set;
    set.set_directory(1, "AAA     ", 8, 'Q', ' ', 100);
    set.at(1).add(1, 'B', 1000000, 100);
    set.at(1).execute(1, 100);
    const uint64_t volume = set.peek(1)->volume();
    CHECK_EQ(volume, 100u);

    set.note_broken_trade(1);
    CHECK_EQ(set.broken_trades(), 1u);
    CHECK_EQ(set.peek(1)->volume(), volume);   // unchanged, deliberately

    // And it does not conjure a book for a symbol that has none.
    set.note_broken_trade(5);
    CHECK_EQ(set.broken_trades(), 2u);
    CHECK(set.peek(5) == nullptr);
}

void test_the_counters_that_matter_are_asked_of_the_feed() {
    BookSet set;
    set.at(1).add(10, 'B', 1000000, 100);
    set.at(2).add(20, 'B', 500000, 100);
    CHECK_EQ(set.unknown_ref(), 0u);
    CHECK_EQ(set.locate_mismatch(), 0u);

    set.at(1).remove(999);                     // no order anywhere has it
    set.at(2).remove(998);
    CHECK_EQ(set.unknown_ref(), 2u);

    // Symbol 2 handed symbol 1's reference. Impossible on a real feed, which
    // is why the aggregate has to be reported rather than assumed.
    set.at(2).remove(10);
    CHECK_EQ(set.locate_mismatch(), 1u);
    CHECK_EQ(set.unknown_ref(), 2u);           // still two: a different fact
    CHECK_EQ(set.peek(1)->resting_orders(), 1u);
}

void test_walking_the_set_visits_the_books_that_exist_in_locate_order() {
    BookSet set;
    set.set_directory(5, "EEE     ", 8, 'Q', ' ', 100);
    set.set_directory(2, "BBB     ", 8, 'Q', ' ', 100);
    set.at(5).add(1, 'B', 1000000, 100);
    set.at(2).add(2, 'B', 500000, 200);

    std::string seen;
    size_t visits = 0;
    set.for_each_book([&](uint16_t loc, const itchbook::book::Book& b, const SymbolInfo& info) {
        ++visits;
        seen += std::string(info.symbol) + ":" + std::to_string(loc) + " ";
        CHECK_EQ(b.resting_orders(), 1u);
    });
    CHECK_EQ(visits, 2u);
    CHECK_STR(seen, "BBB:2 EEE:5 ");
}

}  // namespace

int main() {
    test_books_are_built_on_demand_not_up_front();
    test_two_symbols_share_storage_and_nothing_else();
    test_the_directory_is_kept_and_the_padding_is_not();
    test_a_message_for_an_undirectoried_locate_is_counted_not_ignored();
    test_the_session_belongs_to_the_market_not_to_a_symbol();
    test_a_pool_per_symbol_builds_the_same_book_and_costs_more();
    test_band_width_changes_nothing_the_book_reports();
    test_the_band_waits_for_a_two_sided_quote();
    test_a_recentre_keeps_price_time_priority();
    test_an_odd_spread_does_not_send_the_book_to_overflow();
    test_a_band_that_is_working_is_never_moved();
    test_tradability_comes_from_three_places_not_one();
    test_a_broken_trade_is_counted_and_not_applied();
    test_the_counters_that_matter_are_asked_of_the_feed();
    test_walking_the_set_visits_the_books_that_exist_in_locate_order();
    return REPORT();
}

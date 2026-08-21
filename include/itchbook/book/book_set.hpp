#pragma once
//
// book_set.hpp — every symbol in the feed, one process.
//
// A single-symbol run is a Book. A whole-day run is 8,700 of them sharing one
// reference map and one pool (see Storage in book.hpp), because a book that
// owned its own map would want 16 MB and nine thousand of those do not fit in a
// machine.
//
// Three things live here that have nowhere sensible to live in a Book:
//
//   * **The directory.** 'R' names every security the session will carry, at
//     the top of the day. The book does not need it to reconstruct anything —
//     it is metadata — but a summary of 8,700 rows with no symbols in it is not
//     a result anybody can read, and `round_lot` and the market category are
//     what a later phase will need to say anything per-symbol.
//
//   * **The session.** 'S' is one market's clock: pre-open, open, close. It
//     carries stock locate 0, so routing it by locate would file it under
//     whatever symbol happens to own that code and leave the other 8,699
//     summaries with no session state at all. That is not a crash — it is 8,699
//     quietly wrong outputs, which is the failure mode this repository is built
//     to refuse.
//
//   * **The aggregate counters.** unknown_ref and locate_mismatch are per book,
//     and the question phase 9 asks is about the feed.
//
// What is deliberately NOT here: any knowledge of the wire. This header never
// sees a message. dispatch.hpp stays the only file that knows both an ITCH
// layout and a book operation, and set_directory() takes fields rather than a
// pointer so it stays that way.
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"

namespace itchbook::book {

// What 'R' says about a security. The book skipped most of these fields for
// seven phases because one symbol's replay never needed them.
struct SymbolInfo {
    char symbol[9] = {0};            // NUL-terminated, trailing spaces trimmed
    char market_category = '\0';     // 'Q' NASDAQ Global Select, 'N' NYSE, ...
    char financial_status = '\0';    // 'D' deficient, 'H' halted, ' ' normal
    uint32_t round_lot = 0;
    bool directoried = false;        // an 'R' was actually seen for this locate

    // ---- tradability, from the messages that do not touch a book ------------
    //
    // 'H' (stock trading action) lives on the Book, because it has since phase
    // 7 and it is per symbol there already. These two do not, and until a whole
    // file was read they were not anywhere.
    bool operational_halt = false;   // 'h' — venue-level, separate from 'H'
    uint64_t operational_halts = 0;  // entries into it, so a count survives a resume
    uint64_t broken_trades = 0;      // 'B' — a printed trade was busted
};

// One market, one clock. Not per symbol, however tempting the wire makes it
// look: 'S' carries locate 0 and means the same thing for everything.
struct SessionState {
    char system_event = '\0';        // 'O','S','Q','M','E','C'

    // 'W'. A market-wide circuit breaker halts everything, so this is not a
    // per-symbol fact and cannot be stored as one.
    char mwcb_level_breached = '\0';  // '1', '2', '3'; '\0' = none this session
    uint64_t mwcb_events = 0;
};

class BookSet {
public:
    // Every locate the wire can express. 65,536 null pointers is 512 KB, which
    // is cheaper than depending on the directory arriving before anything else
    // — true of a well-formed day, and not a thing to build a container's
    // sizing on. Books are constructed on first use, so the locates that never
    // trade cost a pointer and a SymbolInfo and nothing more.
    static constexpr size_t kLocates = size_t{1} << 16;

    // `refs_capacity` defaults to four million slots rather than a Book's one
    // million, because this map holds every symbol's orders at once. It is a
    // placeholder for a measurement: phase 9.0's `itch_census --peak-orders`
    // reports the real high-water mark for a day, and 9.9 pre-sizes from it so
    // that a rehash — hundreds of milliseconds, straight into the worst-sample
    // column — never happens mid-replay.
    // `band_levels` is the per-side dense band, in slots. Total band memory is
    // active_symbols x 2 x band_levels x sizeof(Level), which is 32 bytes -- so
    // 512 slots across the 8,892 symbols that quoted on 2019-12-30 is 291 MB,
    // and that number is knowable before the run rather than after it. Zero
    // keeps the phase-3 percentage policy, which is what every single-symbol
    // caller still gets.
    explicit BookSet(size_t refs_capacity = 1u << 22, int32_t tick = 100,
                     int32_t band_pct = 20, size_t band_levels = 0)
        : store_(refs_capacity), tick_(tick), band_pct_(band_pct),
          band_levels_(band_levels), books_(kLocates), dir_(kLocates) {}

    // The book for a locate, created if this is the first message for it. A
    // newly created book inherits the session state, so a symbol that first
    // trades after the open is not left thinking the market never opened.
    Book& at(uint16_t locate) {
        std::unique_ptr<Book>& slot = books_[locate];
        if (slot == nullptr) {
            slot = std::make_unique<Book>(store_, locate, tick_, band_pct_);
            slot->set_band_levels(band_levels_);
            slot->set_system_event(session_.system_event);
            ++constructed_;
        }
        if (!dir_[locate].directoried) ++undirectoried_messages_;
        return *slot;
    }

    // The book for a locate if one exists, without creating it. For readers —
    // a summary walk must not conjure 65,000 empty books.
    const Book* peek(uint16_t locate) const { return books_[locate].get(); }

    const SymbolInfo& info(uint16_t locate) const { return dir_[locate]; }

    // From 'R'. Fields, not a pointer: this header does not know the wire.
    void set_directory(uint16_t locate, const char* symbol, size_t symbol_len,
                       char market_category, char financial_status, uint32_t round_lot) {
        SymbolInfo& s = dir_[locate];
        const size_t n = symbol_len < 8 ? symbol_len : 8;
        std::memcpy(s.symbol, symbol, n);
        s.symbol[n] = '\0';
        for (size_t i = n; i > 0 && s.symbol[i - 1] == ' '; --i) s.symbol[i - 1] = '\0';
        s.market_category = market_category;
        s.financial_status = financial_status;
        s.round_lot = round_lot;
        if (!s.directoried) {
            s.directoried = true;
            ++directoried_;
        }
    }

    // From 'S'. Recorded once for the session and pushed to every book that
    // already exists, so that a per-symbol summary can report it without every
    // book having to have been present when it arrived.
    void set_system_event(char code) {
        session_.system_event = code;
        for (const std::unique_ptr<Book>& b : books_) {
            if (b != nullptr) b->set_system_event(code);
        }
    }

    const SessionState& session() const { return session_; }
    char system_event() const { return session_.system_event; }

    // ---- tradability --------------------------------------------------------
    //
    // From 'h'. Per symbol, and deliberately not routed through at(): a book
    // exists to hold orders, and conjuring one for a symbol whose only message
    // was a halt would put a phantom row in every summary.
    void set_operational_halt(uint16_t locate, char action) {
        SymbolInfo& s = dir_[locate];
        const bool halted = (action == 'H');
        if (halted && !s.operational_halt) ++s.operational_halts;
        s.operational_halt = halted;
    }

    // From 'W'.
    void set_mwcb_breached(char level) {
        session_.mwcb_level_breached = level;
        ++session_.mwcb_events;
    }

    // From 'B'. Counted, not applied: busting a print would mean revising
    // volume, VWAP and possibly the close, and doing that correctly needs the
    // match number of every trade the day printed — 268 million lookups to
    // undo something that happened zero times in the file this project
    // validates against. The number is reported instead, so that a daily bar
    // which disagrees with a vendor's can be explained rather than argued with.
    void note_broken_trade(uint16_t locate) { ++dir_[locate].broken_trades; }

    // Whether this symbol may trade right now, from all three sources.
    //
    // Two things a reader is owed about this predicate.
    //
    // **'h' and 'W' have never fired on real data.** 'H' is graded against real
    // bytes across a whole day; the other two do not occur in the file this
    // project validates against, so those conditions are derived from the spec
    // and have never been exercised by anything but a generated feed. See
    // messages.hpp, which marks all three offsets UNCONFIRMED for the same
    // reason.
    //
    // **A circuit-breaker breach is treated as permanent, and it is not.** A
    // Level 1 or 2 breach halts the market for a fixed period and then trading
    // resumes; only Level 3 ends the day. Nothing in the feed says "the MWCB
    // halt is over" — the resume arrives as ordinary session and trading-action
    // messages — so reconstructing it means modelling the halt clock, which
    // this does not do. The consequence is that after a breach this returns
    // false for the rest of the session, including a period when the market has
    // in fact reopened. That is wrong in the safe direction for a predicate
    // whose consumers are risk checks, and it is a limitation rather than a
    // conservatism to be proud of: it is written down here so it can be fixed
    // deliberately rather than discovered.
    bool tradable(uint16_t locate) const {
        const Book* b = books_[locate].get();
        if (b == nullptr || b->trading_state() != 'T') return false;
        if (dir_[locate].operational_halt) return false;
        if (session_.mwcb_level_breached != '\0') return false;
        return true;
    }

    // Feed-level tallies for the three, so a run reports them whether or not
    // any occurred. A zero here is a result: it says the constants those
    // messages are parsed with are still unconfirmed, and why.
    uint64_t operational_halts() const {
        uint64_t n = 0;
        for (const SymbolInfo& s : dir_) n += s.operational_halts;
        return n;
    }

    size_t symbols_operationally_halted() const {
        size_t n = 0;
        for (const SymbolInfo& s : dir_) {
            if (s.operational_halt) ++n;
        }
        return n;
    }

    uint64_t broken_trades() const {
        uint64_t n = 0;
        for (const SymbolInfo& s : dir_) n += s.broken_trades;
        return n;
    }

    char mwcb_level_breached() const { return session_.mwcb_level_breached; }

    Storage& storage() { return store_; }
    const Storage& storage() const { return store_; }

    size_t books() const { return constructed_; }
    size_t directory_entries() const { return directoried_; }

    // Messages for a locate the directory never named. On a well-formed day
    // this is zero: 'R' precedes everything. Non-zero means either the file is
    // not what it claims or the framing put a message under the wrong locate,
    // and both of those are worth more than a silent guess.
    uint64_t undirectoried_messages() const { return undirectoried_messages_; }

    // Every book that exists, in locate order.
    template <typename Fn>
    void for_each_book(Fn&& fn) const {
        for (size_t loc = 0; loc < books_.size(); ++loc) {
            if (books_[loc] != nullptr) fn(static_cast<uint16_t>(loc), *books_[loc], dir_[loc]);
        }
    }

    // The feed-level counters. Per book they answer a question about a symbol;
    // summed they answer the one phase 9 actually asks.
    uint64_t unknown_ref() const {
        uint64_t n = 0;
        for_each_book([&](uint16_t, const Book& b, const SymbolInfo&) { n += b.unknown_ref(); });
        return n;
    }

    uint64_t locate_mismatch() const {
        uint64_t n = 0;
        for_each_book([&](uint16_t, const Book& b, const SymbolInfo&) {
            n += b.locate_mismatch();
        });
        return n;
    }

    size_t resting_orders() const { return store_.pool.live(); }

private:
    Storage store_;
    SessionState session_;
    int32_t tick_;
    int32_t band_pct_;
    size_t band_levels_;
    std::vector<std::unique_ptr<Book>> books_;
    std::vector<SymbolInfo> dir_;
    size_t constructed_ = 0;
    size_t directoried_ = 0;
    uint64_t undirectoried_messages_ = 0;
};

}  // namespace itchbook::book

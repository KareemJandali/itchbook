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
};

// One market, one clock. Not per symbol, however tempting the wire makes it
// look: 'S' carries locate 0 and means the same thing for everything.
struct SessionState {
    char system_event = '\0';        // 'O','S','Q','M','E','C'
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
    explicit BookSet(size_t refs_capacity = 1u << 22, int32_t tick = 100,
                     int32_t band_pct = 20)
        : store_(refs_capacity), tick_(tick), band_pct_(band_pct),
          books_(kLocates), dir_(kLocates) {}

    // The book for a locate, created if this is the first message for it. A
    // newly created book inherits the session state, so a symbol that first
    // trades after the open is not left thinking the market never opened.
    Book& at(uint16_t locate) {
        std::unique_ptr<Book>& slot = books_[locate];
        if (slot == nullptr) {
            slot = std::make_unique<Book>(store_, locate, tick_, band_pct_);
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
    std::vector<std::unique_ptr<Book>> books_;
    std::vector<SymbolInfo> dir_;
    size_t constructed_ = 0;
    size_t directoried_ = 0;
    uint64_t undirectoried_messages_ = 0;
};

}  // namespace itchbook::book

#pragma once
//
// split.hpp — the phase-12.1 split replayer: adds are state, executions are events.
//
// Phase 9 replays a historical day by APPLYING every message to a book. That is
// right for reconstruction and wrong the moment a strategy order rests in the
// same queue, because it makes that order unfillable: the aggressor that would
// have hit it is not in the feed as an order, only as its consequence on
// somebody else's. So the message classes are not treated alike.
//
//   'E'  is a crossing event. It is replayed as a synthesised aggressor that
//        walks the queue, and a strategy order sitting ahead of the named
//        historical order takes those shares first.
//   everything else is applied as state, through the phase-9 dispatch path.
//
// Two of those classifications were MEASURED rather than assumed, on
// 12302019.NASDAQ_ITCH50.gz (268,744,780 messages), by asking where in its own
// queue each executed order was sitting at the instant it traded:
//
//                          at front of level   behind others   worse price
//     E  (5,722,824)            99.81%            0.185%         0.0047%
//     C  (   99,917)            17.6%            82.4%           --
//
// 'C' is Order Executed With Price — an execution that prints AWAY from the
// resting price — and 82.4% of them name an order a mean 4,501 shares deep in
// its own queue. Those trades did not respect displayed price-time priority;
// they are price improvement and non-displayed interaction. Replaying a 'C' as
// a queue-walking aggressor would therefore hand a resting strategy order fills
// it would never have received, which is the optimistic-fill error phases 6 and
// 11 exist to prevent. 'C' is applied as state. This is a deliberate deviation
// from docs/phase12-design.md §3, which grouped 'E' and 'C' together; the
// measurement above is the reason, and 445x is not a rounding difference.
//
// 'P' (non-cross trade) is applied as state for a narrower reason: it consumed
// NON-DISPLAYED liquidity, and Book::trade() correspondingly moves counters
// without touching a single resting order. Whether a displayed strategy quote
// should have been hit ahead of the hidden order it actually traded against is
// a modelling question about display priority, not a mechanical one, and it is
// left to 12.7 rather than guessed at here.
//
// The reference partition is enforced here and not merely documented. Strategy
// orders take the high half of the 64-bit reference space; a historical
// reference arriving with bit 63 set would collide with one of ours and corrupt
// a single price level silently. Every reference on every message is checked —
// including the NEW reference of a 'U', which is the one an "original_ref only"
// check would miss. It is counted rather than asserted, because assert() is
// compiled out under NDEBUG and this must hold in the Release build the gate
// actually runs; refusing to emit numbers is the caller's job and
// partition_violations is what it refuses on.
//
#include <cstdint>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/book_set.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/messages.hpp"

namespace itchbook::replay {

// Strategy references occupy the high half of the space. Historical NASDAQ
// references have never been observed there — 0 violations across 268,744,780
// messages on 2019-12-30 — but "never observed" is a claim about a sample, so
// the replayer checks rather than trusts.
inline constexpr uint64_t kStrategyRefBit = uint64_t{1} << 63;

inline constexpr bool is_strategy_ref(uint64_t ref) {
    return (ref & kStrategyRefBit) != 0;
}

enum class Treatment : uint8_t {
    State,      // applied through the phase-9 path, unchanged
    Aggressor,  // synthesised as a crossing event and walked through the queue
};

// THE classification table. There is exactly one, for the same reason
// dispatch.hpp keeps exactly one switch over message types: a second copy is a
// second thing to keep in step.
inline constexpr Treatment treatment_of(char type) {
    return type == 'E' ? Treatment::Aggressor : Treatment::State;
}

class SplitReplayer {
public:
    struct Counters {
        uint64_t messages = 0;            // every message handed to apply()
        uint64_t state_applied = 0;       // went down the phase-9 path
        uint64_t aggressors = 0;          // 'E' replayed as a crossing event
        uint64_t aggressor_shares = 0;    // shares those aggressors carried
        uint64_t strategy_shares_taken = 0;   // taken from strategy orders ahead
        uint64_t historical_ahead = 0;    // a HISTORICAL order sat ahead of the
                                          // named one and was correctly skipped
        uint64_t aggressor_unknown_ref = 0;   // named a reference no book holds
        uint64_t partition_violations = 0;    // bit 63 set on a historical ref
    };

    explicit SplitReplayer(book::BookSet& set) : set_(set) {}

    const Counters& counters() const { return c_; }
    book::BookSet& set() { return set_; }
    const book::BookSet& set() const { return set_; }

    // True when every message so far has respected the reference partition. A
    // run that cannot guarantee it must not produce numbers.
    bool partition_held() const { return c_.partition_violations == 0; }

    // Apply one message. Mirrors dispatch::apply(BookSet&, ...)'s return: true
    // when the message counted as a book or volume mutation.
    bool apply(char type, const uint8_t* p) {
        ++c_.messages;
        check_partition(type, p);

        if (treatment_of(type) == Treatment::Aggressor) {
            return aggress(p);
        }
        ++c_.state_applied;
        return book::apply(set_, type, p);
    }

private:
    // ---- the reference partition ---------------------------------------------
    //
    // Every message that names an order, in either direction. 'U' names two and
    // both are checked: the replacement reference is the one that gets INSERTED
    // into the map, so a check that only looked at original_ref would miss the
    // half that can actually collide.
    void check_partition(char type, const uint8_t* p) {
        namespace m = itchbook::itch;
        switch (type) {
            case 'A': note_ref(m::add_order::ref(p)); break;
            case 'F': note_ref(m::add_order_mpid::ref(p)); break;
            case 'E': note_ref(m::order_executed::ref(p)); break;
            case 'C': note_ref(m::order_executed_price::ref(p)); break;
            case 'X': note_ref(m::order_cancel::ref(p)); break;
            case 'D': note_ref(m::order_delete::ref(p)); break;
            case 'U':
                note_ref(m::order_replace::original_ref(p));
                note_ref(m::order_replace::new_ref(p));
                break;
            default: break;
        }
    }

    void note_ref(uint64_t ref) {
        if (is_strategy_ref(ref)) ++c_.partition_violations;
    }

    // ---- 'E' as a crossing event ---------------------------------------------
    //
    // The aggressor is TARGETED at the order the feed named, and takes strategy
    // shares ahead of it on the way. It does NOT take historical shares ahead of
    // it, and that is the whole subtlety: the feed is ground truth about which
    // historical order traded, so a historical order resting in front of the
    // named one demonstrably did not trade here, whatever our reconstruction's
    // queue says. Consuming it would invent a fill history contradicts. It
    // happens 10,565 times in a day and is counted, not swallowed.
    //
    // A plain marketable aggressor priced at the resting order's price was the
    // obvious construction and is wrong for the same reason, more often: it
    // would walk from the front of the best level and eat whatever it found.
    bool aggress(const uint8_t* p) {
        namespace m = itchbook::itch;
        const uint64_t ref = m::order_executed::ref(p);
        const uint32_t shares = m::order_executed::executed_shares(p);
        const uint16_t locate = m::stock_locate(p);

        ++c_.aggressors;
        c_.aggressor_shares += shares;

        // set_.at() rather than peek(): dispatch::apply routes a modelled
        // message through BookSet::at(), which constructs the book on first
        // use. Reaching for peek() here would make an 'E' for an unseen symbol
        // build no book where phase 9 builds one, and the two runs would part
        // company over a symbol that never traded again.
        book::Book& b = set_.at(locate);

        const book::Order* o = b.find(ref);
        if (o == nullptr) {
            // Unknown reference. Route it through execute() rather than
            // returning early, because execute()'s own resolve() is what counts
            // unknown_ref and locate_mismatch, and those counters are compared
            // against phase 9. Skipping the call would make the split path
            // report a cleaner sheet than the feed deserves.
            ++c_.aggressor_unknown_ref;
            b.execute(ref, shares);
            return true;
        }

        const char side = static_cast<char>(o->side);
        const int32_t price = o->price;

        // Collect first, take second. take() destroys an order that reaches
        // zero, which unlinks it from the very list being walked.
        ahead_.clear();
        for (const book::Order* q = b.first_order(side, price);
             q != nullptr && q->ref != ref; q = q->next) {
            if (is_strategy_ref(q->ref)) {
                ahead_.push_back(q->ref);
            } else {
                ++c_.historical_ahead;
            }
        }

        uint32_t remaining = shares;
        for (const uint64_t sref : ahead_) {
            if (remaining == 0) break;
            const book::Order* so = b.find(sref);
            if (so == nullptr) continue;
            const uint32_t take = so->shares < remaining ? so->shares : remaining;
            b.take(sref, take);
            remaining -= take;
            c_.strategy_shares_taken += take;
        }

        // The tape saw ONE print of `shares` at the named order's resting
        // price, however many resting orders the book split it across. Phase 9
        // records exactly that, with the message's share count and not the
        // clamped one — executing 250 against a 100-share order counts 250 —
        // so the split path records it the same way, once, before the book
        // mutation that may destroy the order it reads the price from.
        b.note_feed_trade(price, shares);
        if (remaining > 0) b.take(ref, remaining);
        return true;
    }

    book::BookSet& set_;
    Counters c_;
    std::vector<uint64_t> ahead_;   // reused; the walk is empty 99.8% of the time
};

}  // namespace itchbook::replay

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
#include <cstring>
#include <unordered_map>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/book_set.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/engine/matcher.hpp"
#include "itchbook/emit/itch_encode.hpp"
#include "itchbook/emit/sink.hpp"
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

// Phase 12.8 chain B. The replayer reports a fill the instant it exists, and
// the caller decides what to do with it -- the class does not own the storage.
struct FillTrace {
    // ref: the strategy order that was filled. ordinal: the n-th fill of THAT
    // reference, counted here and again in the strategy, because the match
    // number is shared by every fill of one aggressor and cannot key a sample.
    // mold_seq: the sequence the ITCH 'E' about to be emitted will carry, which
    // is the only thing connecting this instant to the datagram that leaves
    // later.
    virtual ~FillTrace() = default;
    virtual void on_strategy_fill(uint64_t ref, uint32_t ordinal,
                                  uint64_t mold_seq, uint32_t shares) = 0;
};

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
        uint64_t emitted = 0;                 // ITCH messages published
        uint64_t unmodelled_not_emitted = 0;  // input types we do not parse, so
                                              // cannot republish -- see emit_state
    };

    explicit SplitReplayer(book::BookSet& set) : set_(set) {}

    // Attach the publisher. Null (the default) is phase 12.1's behaviour and
    // costs one predictable branch per message.
    void set_sink(emit::Sink* s) { sink_ = s; }

    // Attach the engine that owns the strategy orders on this book, so a
    // strategy order the aggressor takes shares from is reduced THROUGH the
    // Matcher rather than behind its back. Optional: with no Matcher the
    // replayer behaves exactly as it did for 12.2's P1 gate, which is what
    // keeps that gate a valid regression check on this change.
    void set_matcher(engine::Matcher* m) { matcher_ = m; }

    // Phase 12.8. `t` is told about each fill of a strategy order as it
    // happens; `seq` supplies the sequence the next emitted message will carry.
    // Both null by default, so the 12.1 and 12.7 paths are unchanged.
    void set_fill_trace(FillTrace* t, uint64_t (*seq)(void*), void* seq_ctx) {
        trace_ = t;
        seq_fn_ = seq;
        seq_ctx_ = seq_ctx;
    }

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
        const bool mutated = book::apply(set_, type, p);
        emit_state(type, p);
        return mutated;
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

    // ---- publishing --------------------------------------------------------
    //
    // The exchange republishes the mutation it just performed, RE-ENCODED from
    // the fields it parsed rather than copied from the input bytes. That is the
    // whole content of P1 at zero strategy orders: the emitted stream is a
    // decode-then-re-encode round trip of the input, so a field written at the
    // wrong offset, in the wrong endianness, or not written at all shows up
    // either as a byte difference against the input or as a divergence in the
    // book a consumer rebuilds from it.
    //
    // Match and tracking numbers are carried across rather than reissued. A
    // reissued match number would break 'B' (Broken Trade), which names the
    // match number of a trade that has already been published; our own
    // numbering would make it point at a trade that never happened. It also
    // buys the stronger gate: with the header copied and every body field
    // re-encoded, the emitted message must come out BYTE-IDENTICAL to the
    // input, which catches the fields a book consumer never reads.
    //
    // Types this project does not parse are not republished. It cannot
    // republish what it never decoded, and inventing bytes would be worse than
    // the gap; they are counted so the gap is visible rather than silent. They
    // have no book effect, so the reconstructed book is unaffected -- 1.58% of
    // a real day, per the census.
    void emit_state(char type, const uint8_t* p) {
        if (sink_ == nullptr) return;
        namespace m = itchbook::itch;
        namespace e = itchbook::emit;

        uint8_t o[e::kMaxMessage];
        const uint16_t loc = m::stock_locate(p);
        const uint16_t trk = m::be16(p + 3);
        const uint64_t ts = ts_of(p);
        size_t n = 0;

        switch (type) {
            case 'S':
                n = e::system_event(o, loc, trk, ts, m::system_event::code(p));
                break;
            case 'R':
                n = e::stock_directory(o, loc, trk, ts, m::stock_directory::stock(p),
                                       m::stock_directory::market_category(p),
                                       m::stock_directory::financial_status(p),
                                       m::stock_directory::round_lot_size(p));
                // Bytes 25..38 of a Stock Directory are fields this project has
                // never decoded -- issue classification, authenticity, the
                // short-sale threshold, the IPO flag, the LULD tier, the ETP
                // flags. There is exactly one right thing to do with content we
                // do not understand and are republishing: carry it across
                // verbatim. Zeroing it destroyed real content and showed up as
                // 8,906 byte-differences on a real day, one per symbol.
                //
                // Stated plainly because it is the one place the emitter is a
                // relay rather than an encoder: those 14 bytes are COPIED, so
                // P1's byte-identity says nothing about them. Everything else
                // in every message is re-encoded from a decoded field.
                std::memcpy(o + 25, p + 25, 14);
                break;
            case 'A':
                n = e::add_order(o, loc, trk, ts, m::add_order::ref(p),
                                 m::add_order::side(p), m::add_order::shares(p),
                                 m::add_order::stock(p), m::add_order::price(p));
                break;
            case 'F':
                n = e::add_order_mpid(o, loc, trk, ts, m::add_order_mpid::ref(p),
                                      m::add_order_mpid::side(p), m::add_order_mpid::shares(p),
                                      m::add_order_mpid::stock(p), m::add_order_mpid::price(p),
                                      m::add_order_mpid::attribution(p));
                break;
            case 'C':
                n = e::order_executed_price(o, loc, trk, ts, m::order_executed_price::ref(p),
                                            m::order_executed_price::executed_shares(p),
                                            m::order_executed_price::match_number(p),
                                            m::order_executed_price::printable(p),
                                            m::order_executed_price::price(p));
                break;
            case 'X':
                n = e::order_cancel(o, loc, trk, ts, m::order_cancel::ref(p),
                                    m::order_cancel::canceled_shares(p));
                break;
            case 'D':
                n = e::order_delete(o, loc, trk, ts, m::order_delete::ref(p));
                break;
            case 'U':
                n = e::order_replace(o, loc, trk, ts, m::order_replace::original_ref(p),
                                     m::order_replace::new_ref(p), m::order_replace::shares(p),
                                     m::order_replace::price(p));
                break;
            case 'P':
                n = e::trade(o, loc, trk, ts, m::trade::ref(p), m::trade::side(p),
                             m::trade::shares(p), m::trade::stock(p), m::trade::price(p),
                             m::trade::match_number(p));
                break;
            case 'Q':
                n = e::cross_trade(o, loc, trk, ts, m::cross_trade::shares(p),
                                   m::cross_trade::stock(p), m::cross_trade::price(p),
                                   m::cross_trade::match_number(p),
                                   m::cross_trade::cross_type(p));
                break;
            case 'H':
                n = e::trading_action(o, loc, trk, ts, m::trading_action::stock(p),
                                      m::trading_action::state(p), m::trading_action::reason(p));
                break;
            case 'h':
                n = e::operational_halt(o, loc, trk, ts, m::operational_halt::stock(p),
                                        m::operational_halt::market_code(p),
                                        m::operational_halt::action(p));
                break;
            case 'W':
                n = e::mwcb_status(o, loc, trk, ts, m::mwcb_status::breached_level(p));
                break;
            case 'B':
                n = e::broken_trade(o, loc, trk, ts, m::broken_trade::match_number(p));
                break;
            default:
                ++c_.unmodelled_not_emitted;
                return;
        }
        sink_->on_message(o, n);
        ++c_.emitted;
    }

    static uint64_t ts_of(const uint8_t* p) {
        uint64_t v = 0;
        for (int i = 0; i < 6; ++i) v = (v << 8) | p[5 + i];
        return v;
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
            // Republished even though it named nothing: a consumer rebuilding
            // from our feed must see the same unknown reference we did, and
            // count it in the same place. Swallowing it would hand the consumer
            // a cleaner book than the one the exchange actually has.
            emit_exec(p, ref, shares);
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
            uint32_t take = 0;
            if (matcher_ != nullptr) {
                // The return value is the single source of truth for how much
                // moved: clamping here against Meta::resting rather than
                // against book::Order::shares means a Meta/book desync
                // surfaces as a wrong quantity instead of propagating.
                take = matcher_->apply_external_fill(
                    sref, remaining, price, m::order_executed::match_number(p));
                if (take == 0) continue;   // terminal, or gone since the walk
            } else {
                const book::Order* so = b.find(sref);
                if (so == nullptr) continue;
                take = so->shares < remaining ? so->shares : remaining;
                b.take(sref, take);
            }
            b.note_feed_trade(price, take);
            remaining -= take;
            c_.strategy_shares_taken += take;
            // t5a: the fill exists. Reported BEFORE emit_exec, so the sequence
            // read here is the one the 'E' about to be emitted will carry.
            if (trace_ != nullptr) {
                const uint32_t ord = fill_ordinal_[sref]++;
                const uint64_t ms = (seq_fn_ != nullptr) ? seq_fn_(seq_ctx_) : 0;
                trace_->on_strategy_fill(sref, ord, ms, take);
            }
            // One 'E' per fill, all sharing the aggressor's match number --
            // which is what a real venue does when one incoming order walks
            // several makers. At zero strategy orders this loop does not run
            // and the single emission below is the whole of it.
            emit_exec(p, sref, take);
        }

        // ONE PRINT PER FILL, not one per message, and the difference only
        // shows up once a strategy order is in the queue.
        //
        // The obvious rule is one print per input execution, since the tape saw
        // one. It is wrong, and P1 cannot see that it is wrong: the emitted
        // stream publishes one 'E' per fill, a consumer replaying two of them
        // calls Book::execute twice, and record_trade does ++trades_ each time
        // (book.hpp). The exchange's own book would have counted one where its
        // published feed says two, and the two books would part company by one
        // trade per split fill. Since the gate runs at zero strategy orders,
        // where there is exactly one fill per execution, both spellings agree
        // and the defect would have sat there until 12.7.
        //
        // Volume is unaffected either way -- the fills sum to the message's
        // share count -- and so is the price, because the aggressor only ever
        // walks the named order's own level. What changes is the trade COUNT,
        // and a print is a trade.
        //
        // The message's share count is used and not the clamped one: executing
        // 250 against a 100-share order counts 250, matching Book::execute.
        if (remaining > 0) {
            b.note_feed_trade(price, remaining);
            b.take(ref, remaining);
            emit_exec(p, ref, remaining);
        }
        return true;
    }

    // An execution the matcher performed, described as ITCH. The match number
    // and timestamp come from the historical execution being replayed, so every
    // fill of one aggressor shares them.
    void emit_exec(const uint8_t* p, uint64_t ref, uint32_t shares) {
        if (sink_ == nullptr) return;
        namespace m = itchbook::itch;
        uint8_t o[itchbook::emit::kMaxMessage];
        const size_t n = itchbook::emit::order_executed(
            o, m::stock_locate(p), m::be16(p + 3), ts_of(p), ref, shares,
            m::order_executed::match_number(p));
        sink_->on_message(o, n);
        ++c_.emitted;
    }

    book::BookSet& set_;
    emit::Sink* sink_ = nullptr;
    engine::Matcher* matcher_ = nullptr;
    FillTrace* trace_ = nullptr;
    uint64_t (*seq_fn_)(void*) = nullptr;
    void* seq_ctx_ = nullptr;
    // Fills seen per strategy reference, so the ordinal is counted rather than
    // guessed. Cleared nowhere: a reference is used once.
    std::unordered_map<uint64_t, uint32_t> fill_ordinal_;
    Counters c_;
    std::vector<uint64_t> ahead_;   // reused; the walk is empty 99.8% of the time
};

}  // namespace itchbook::replay

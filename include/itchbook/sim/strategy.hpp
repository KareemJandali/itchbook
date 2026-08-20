#pragma once
//
// strategy.hpp — what a strategy may see, and what it may not.
//
// A strategy is handed the market and nothing else. It cannot see its fills, its
// position, or its queue position. Those omissions are the design:
//
//   * **No fills, no position.** Fills differ between the four models by
//     construction, so a strategy that reacts to them makes different decisions
//     in each lane, and the headline claim — "same strategy, four fill models" —
//     stops being true. With no feedback path, the intent stream is byte-
//     identical across lanes as a structural fact rather than a hope, and the
//     P&L difference is attributable to the fill model and to nothing else.
//
//   * **No queue position.** It is an estimate whose error bars are the entire
//     subject of this phase. A strategy that consumes it would be conditioning
//     on the thing being measured.
//
// Risk limits are therefore the harness's job, not the strategy's. A position
// limit is by definition a function of fills, so enforcing it inside strategy
// code would smuggle the feedback path back in through the side door.
//
// The interface is a template parameter rather than a base class, matching
// parser.hpp: dispatch inlines and there is no indirect call per event.
//
#include <cstdint>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/sim/event.hpp"
#include "itchbook/sim/mid.hpp"

namespace itchbook::sim {

// Everything a strategy is allowed to know.
struct MarketView {
    const book::Book* book = nullptr;
    Mid mid;
    uint64_t ts = 0;
    bool tradable = false;
    uint64_t event_index = 0;

    bool best_bid(int32_t* out) const { return book->best_bid(out); }
    bool best_ask(int32_t* out) const { return book->best_ask(out); }
    uint64_t shares_at(Side side, Price4 price) const {
        return book->shares_at(to_char(side), price);
    }
};

enum class IntentKind : uint8_t { Quote, Cancel, Take };

struct Intent {
    IntentKind kind = IntentKind::Quote;
    uint64_t id = 0;
    Side side = Side::Buy;
    Price4 price = 0;
    uint32_t quantity = 0;
    uint32_t display = 0;
};

// What a strategy writes its decisions into. Deliberately write-only: there is
// no accessor here that could leak fill or position state back.
class Ctx {
public:
    void quote(uint64_t id, Side side, Price4 price, uint32_t quantity,
               uint32_t display = 0) {
        intents_.push_back(Intent{IntentKind::Quote, id, side, price, quantity, display});
    }
    void cancel(uint64_t id) {
        intents_.push_back(Intent{IntentKind::Cancel, id, Side::Buy, 0, 0, 0});
    }
    void take(uint64_t id, Side side, Price4 price, uint32_t quantity) {
        intents_.push_back(Intent{IntentKind::Take, id, side, price, quantity, 0});
    }

    const std::vector<Intent>& intents() const { return intents_; }
    void clear() { intents_.clear(); }

private:
    std::vector<Intent> intents_;
};

}  // namespace itchbook::sim

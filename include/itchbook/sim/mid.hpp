#pragma once
//
// mid.hpp — the prevailing mid, and whether it can be trusted.
//
// Carried DOUBLED (`two_mid = bid + ask`) and never divided until print time.
// The reason is not half-cent rounding — Price(4) has four implied decimals, so
// half a cent is 50 units and exactly representable — it is that every
// intermediate in the accounting path stays an exact integer with no `double`
// anywhere, and stays exact for sub-penny prices too: sub-$1 names, midpoint
// prints, and price-slid ranks, all of which book.hpp already routes to its
// overflow map.
//
// int64 is required regardless: the validated day's $199,999.99 stub quote is
// 1,999,999,900 in Price(4), and two of those overflow int32.
//
#include <cstdint>

#include "itchbook/book/book.hpp"
#include "itchbook/sim/money.hpp"

namespace itchbook::sim {

enum class MidStatus : uint8_t {
    Ok,
    Locked,             // bid == ask; a transient single-venue lock still has a
                        // well-defined midpoint, so this is usable
    OneSided,           // one side empty — no mid exists
    StrictlyCrossed,    // bid > ask
    Halted,
    OutsideContinuous,
    Empty,
};

inline bool usable(MidStatus s) { return s == MidStatus::Ok || s == MidStatus::Locked; }

struct Mid {
    int64_t two_mid = 0;      // bid + ask, NOT divided
    MidStatus status = MidStatus::Empty;

    bool ok() const { return usable(status); }
    // Micro-dollars. Only call at print time.
    Money micros() const { return two_mid * (kMicrosPerPrice4 / 2); }
};

// `in_continuous` gates on session phase, `tradable` on trading state. Both are
// the caller's business because the book does not know the difference between a
// halt and the close.
inline Mid observe(const book::Book& book, bool in_continuous, bool tradable) {
    Mid m;
    if (!in_continuous) {
        m.status = MidStatus::OutsideContinuous;
        return m;
    }
    if (!tradable) {
        m.status = MidStatus::Halted;
        return m;
    }
    int32_t bid = 0;
    int32_t ask = 0;
    const bool have_bid = book.best_bid(&bid);
    const bool have_ask = book.best_ask(&ask);
    if (!have_bid && !have_ask) {
        m.status = MidStatus::Empty;
        return m;
    }
    if (!have_bid || !have_ask) {
        m.status = MidStatus::OneSided;
        return m;
    }
    if (bid > ask) {
        // Book::crossed() counts a locked book as crossed, which is right for
        // the matcher's invariant and wrong here, so the sim uses its own
        // strict predicate rather than changing that API.
        m.status = MidStatus::StrictlyCrossed;
        return m;
    }
    m.two_mid = static_cast<int64_t>(bid) + static_cast<int64_t>(ask);
    m.status = (bid == ask) ? MidStatus::Locked : MidStatus::Ok;
    return m;
}

}  // namespace itchbook::sim

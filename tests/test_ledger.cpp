// test_ledger — P&L accounting and adverse-selection measurement.
//
// The reconciliation identity here is a real check, not a tautology: `equity`
// is computed from cash and position, while `edge` and `drift` are accumulated
// from mids at fill time and at the mark. They share no code, so agreement
// means the decomposition is right.
//
// It is stated with fees as a separate term on purpose. Folding fees into a
// two-term identity would dump the whole rebate stream into drift, which is the
// number this phase exists to measure, and maker-taker is large enough to change
// its sign.
#include <cstdint>

#include "itchbook/sim/ledger.hpp"
#include "itchbook/sim/markout.hpp"
#include "tests/check.hpp"

using namespace itchbook::sim;

namespace {

constexpr Price4 P100 = 1000000;    // $100.0000
constexpr Price4 CENT = 100;

Mid mid_at(int32_t bid, int32_t ask) {
    Mid m;
    m.two_mid = static_cast<int64_t>(bid) + ask;
    m.status = (bid == ask) ? MidStatus::Locked : MidStatus::Ok;
    return m;
}

SimFill fill(Side side, Price4 price, uint32_t shares, uint64_t ts,
             Liquidity liq = Liquidity::Added) {
    SimFill f;
    f.side = side;
    f.price = price;
    f.shares = shares;
    f.ts = ts;
    f.liquidity = liq;
    return f;
}

// A schedule with no fees, so the pure price arithmetic can be checked without
// the fee stream on top of it.
FeeSchedule free_schedule() {
    FeeSchedule f;
    f.exchange.maker = 0;
    f.exchange.taker = 0;
    f.exchange.cross = 0;
    f.regulatory.sec_per_million_dollars_centis = 0;
    f.regulatory.taf_per_share = 0;
    return f;
}

void test_a_round_trip_at_the_same_price_is_flat() {
    Ledger l(free_schedule());
    l.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100));
    l.on_fill(fill(Side::Sell, P100, 100, 2), mid_at(P100, P100));
    CHECK_EQ(l.position(), 0);
    CHECK_EQ(l.equity(P100), 0);
}

void test_buying_low_and_selling_high_makes_money() {
    Ledger l(free_schedule());
    l.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100));
    l.on_fill(fill(Side::Sell, P100 + CENT, 100, 2), mid_at(P100, P100));
    CHECK_EQ(l.position(), 0);
    // One cent on 100 shares is $1.00 = 1,000,000 micro-dollars.
    CHECK_EQ(l.equity(P100), 1000000);
}

void test_open_position_is_marked() {
    Ledger l(free_schedule());
    l.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100));
    CHECK_EQ(l.position(), 100);
    CHECK_EQ(l.equity(P100), 0);                  // marked where we bought
    CHECK_EQ(l.equity(P100 + CENT), 1000000);     // a cent up is $1.00
    CHECK_EQ(l.equity(P100 - CENT), -1000000);
}

void test_the_reconciliation_identity_holds() {
    // equity == edge + drift - fees, with fees a separate term.
    Ledger l(free_schedule());
    // Buy passively at the bid: the mid is above us, so we captured half a spread.
    l.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100 + CENT));
    // Sell passively at the ask a little later, with the market higher.
    l.on_fill(fill(Side::Sell, P100 + 2 * CENT, 100, 2),
              mid_at(P100 + CENT, P100 + 2 * CENT));

    const int64_t two_mark = 2 * static_cast<int64_t>(P100 + CENT);
    CHECK_EQ(l.reconciliation_error(two_mark), 0);
    CHECK_EQ(l.position(), 0);
    // Both fills captured half a cent against the prevailing mid:
    // $0.005 x 100 shares x 2 fills = $1.00 = 1,000,000 micro-dollars.
    CHECK_EQ(l.edge(), 1000000);
}

void test_the_identity_survives_fees() {
    Ledger l;   // the real NASDAQ schedule, rebates and regulatory fees included
    l.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100 + CENT));
    l.on_fill(fill(Side::Sell, P100 + CENT, 100, 2), mid_at(P100, P100 + CENT));
    const int64_t two_mark = 2 * static_cast<int64_t>(P100) + CENT;
    CHECK_EQ(l.reconciliation_error(two_mark), 0);
    CHECK(l.fees() != 0);   // and fees genuinely moved
}

void test_a_fill_without_a_mid_is_counted_not_dropped() {
    // Dropping it would make the identity hold by construction and prove nothing.
    Ledger l(free_schedule());
    Mid none;
    none.status = MidStatus::OneSided;
    l.on_fill(fill(Side::Buy, P100, 100, 1), none);
    CHECK_EQ(l.fills_without_mid(), 1u);
    CHECK_EQ(l.position(), 100);        // it still moved position and cash
    CHECK_EQ(l.edge(), 0);              // but contributed no edge
}

void test_edge_sign_convention() {
    Ledger l(free_schedule());
    // Buying BELOW the mid is favourable.
    l.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100 + 2 * CENT));
    CHECK(l.edge() > 0);

    Ledger l2(free_schedule());
    // Buying ABOVE the mid is not — which a passive fill can genuinely be here,
    // because our order is not in the book and can be reached by price priority.
    l2.on_fill(fill(Side::Buy, P100 + 2 * CENT, 100, 1), mid_at(P100, P100 + CENT));
    CHECK(l2.edge() < 0);
}

void test_fees_are_per_share_not_per_total() {
    Ledger a(free_schedule());
    Ledger b(free_schedule());
    a.on_fill(fill(Side::Buy, P100, 100, 1), mid_at(P100, P100 + CENT));
    for (int i = 0; i < 10; ++i) {
        b.on_fill(fill(Side::Buy, P100, 100, static_cast<uint64_t>(i)),
                  mid_at(P100, P100 + CENT));
    }
    // Ten times the fills, ten times the edge — but the same edge PER SHARE.
    // Totals reward whichever model fills most, which would make the fill-model
    // comparison meaningless.
    CHECK_EQ(b.edge(), 10 * a.edge());
    CHECK_EQ(b.per_share(b.edge()), a.per_share(a.edge()));
}

void test_markout_detects_being_picked_off() {
    // We buy, and the mid then falls. That is adverse selection and must show as
    // a negative drift.
    MarkoutEngine m;
    m.on_fill(fill(Side::Buy, P100, 100, 0), mid_at(P100, P100 + CENT));
    m.observe(kHorizons[0] + 1, mid_at(P100 - 2 * CENT, P100 - CENT));
    m.observe(kHorizons[1] + 1, mid_at(P100 - 2 * CENT, P100 - CENT));
    m.observe(kHorizons[2] + 1, mid_at(P100 - 2 * CENT, P100 - CENT));

    const HorizonReport r = m.report(0);
    CHECK_EQ(r.resolved_fills, 1u);
    CHECK_EQ(r.shares, 100u);
    CHECK(r.drift_per_share < 0);                    // picked off
    CHECK(r.price_impact_per_share() > 0);           // the Rule 605 sign
}

void test_markout_rewards_favourable_drift() {
    MarkoutEngine m;
    m.on_fill(fill(Side::Buy, P100, 100, 0), mid_at(P100, P100 + CENT));
    m.observe(kHorizons[2] + 1, mid_at(P100 + 2 * CENT, P100 + 3 * CENT));
    CHECK(m.report(0).drift_per_share > 0);
}

void test_unresolved_horizons_are_counted_not_dropped() {
    // A fill near the close never sees its 10s horizon. Dropping those
    // conditions the sample on surviving to the end of the day, which is
    // precisely the population least likely to have been picked off.
    MarkoutEngine m;
    m.on_fill(fill(Side::Buy, P100, 100, 0), mid_at(P100, P100 + CENT));
    m.observe(kHorizons[0] + 1, mid_at(P100, P100 + CENT));   // only the first
    CHECK_EQ(m.report(0).resolved_fills, 1u);
    CHECK_EQ(m.report(1).resolved_fills, 0u);
    CHECK_EQ(m.report(1).unresolved_fills, 1u);
    CHECK_EQ(m.report(2).unresolved_fills, 1u);
}

void test_markout_is_share_weighted() {
    // A 1000-share fill must not count the same as a 1-share fill.
    MarkoutEngine m;
    m.on_fill(fill(Side::Buy, P100, 1000, 0), mid_at(P100, P100));
    m.on_fill(fill(Side::Buy, P100, 1, 0), mid_at(P100, P100));
    m.observe(kHorizons[0] + 1, mid_at(P100 - CENT, P100 - CENT));
    const HorizonReport r = m.report(0);
    CHECK_EQ(r.shares, 1001u);
    // Both fills drifted the same way, so per-share drift is exactly one cent
    // against us: -10,000 micro-dollars.
    CHECK_EQ(r.drift_per_share, -10000);
}

void test_a_halt_does_not_stamp_a_horizon() {
    MarkoutEngine m;
    m.on_fill(fill(Side::Buy, P100, 100, 0), mid_at(P100, P100));
    Mid halted;
    halted.status = MidStatus::Halted;
    m.observe(kHorizons[0] + 1, halted);        // must not record a mid
    CHECK_EQ(m.report(0).resolved_fills, 0u);
    m.observe(kHorizons[0] + 2, mid_at(P100, P100));
    CHECK_EQ(m.report(0).resolved_fills, 1u);   // stamped once it is real again
}

void test_mid_status_gating() {
    itchbook::book::Book b;
    CHECK(observe(b, false, true).status == MidStatus::OutsideContinuous);
    CHECK(observe(b, true, false).status == MidStatus::Halted);
    CHECK(observe(b, true, true).status == MidStatus::Empty);
    b.add(1, 'B', P100, 100);
    CHECK(observe(b, true, true).status == MidStatus::OneSided);
    b.add(2, 'S', P100 + CENT, 100);
    const Mid m = observe(b, true, true);
    CHECK(m.status == MidStatus::Ok);
    CHECK_EQ(m.two_mid, 2 * static_cast<int64_t>(P100) + CENT);
    CHECK(m.ok());
}

}  // namespace

int main() {
    test_a_round_trip_at_the_same_price_is_flat();
    test_buying_low_and_selling_high_makes_money();
    test_open_position_is_marked();
    test_the_reconciliation_identity_holds();
    test_the_identity_survives_fees();
    test_a_fill_without_a_mid_is_counted_not_dropped();
    test_edge_sign_convention();
    test_fees_are_per_share_not_per_total();
    test_markout_detects_being_picked_off();
    test_markout_rewards_favourable_drift();
    test_unresolved_horizons_are_counted_not_dropped();
    test_markout_is_share_weighted();
    test_a_halt_does_not_stamp_a_horizon();
    test_mid_status_gating();
    return REPORT();
}

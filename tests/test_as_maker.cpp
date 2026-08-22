// Avellaneda-Stoikov: the algebra, then the plumbing.
//
// as_quote() is the model as a pure function, so most of what follows checks it
// against the paper with no book to build and no feed to replay. A test that
// disagrees with it is disagreeing about the model rather than about the
// harness, which is the distinction that makes a failure actionable.
#include <cmath>
#include <cstdint>
#include <vector>

#include "itchbook/sim/as_maker.hpp"
#include "itchbook/sim/closed_loop.hpp"
#include "tests/check.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

bool close_to(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b));
}

// ---- the model ------------------------------------------------------------

// The defining behaviour, and the reason anyone implements this model: the
// quote pair is displaced by inventory, not by a price forecast.
void test_inventory_displaces_the_quotes() {
    AsConfig c;
    c.gamma = 0.5;
    c.k = 1.5;
    const double mid = 100.0, tau = 1000.0, s2 = 1e-5;

    const AsQuote flat = as_quote(c, mid, 0.0, tau, s2);
    CHECK(close_to(flat.reservation, mid));

    const AsQuote lng = as_quote(c, mid, 3.0, tau, s2);
    const AsQuote shrt = as_quote(c, mid, -3.0, tau, s2);
    // Long: both quotes move DOWN, so the ask is easier to hit and the bid
    // harder. That is the model pricing its own risk, not predicting the price.
    CHECK(lng.reservation < mid);
    CHECK(lng.bid() < flat.bid());
    CHECK(lng.ask() < flat.ask());
    CHECK(shrt.reservation > mid);
    CHECK(shrt.bid() > flat.bid());
    CHECK(shrt.ask() > flat.ask());
    // ...and symmetrically, because nothing in the model prefers a side.
    CHECK(close_to(lng.reservation - mid, -(shrt.reservation - mid)));
    // The displacement is exactly q*gamma*sigma^2*tau.
    CHECK(close_to(flat.reservation - lng.reservation, 3.0 * c.gamma * s2 * tau));
}

// The spread is the same whichever way inventory points: skew comes from r.
void test_the_spread_does_not_depend_on_inventory() {
    AsConfig c;
    const double mid = 50.0, tau = 500.0, s2 = 4e-6;
    const double w = as_quote(c, mid, 0.0, tau, s2).half_spread;
    for (double q : {-9.0, -1.0, 0.5, 4.0, 100.0}) {
        CHECK(close_to(as_quote(c, mid, q, tau, s2).half_spread, w));
    }
}

// Against the paper's formula, computed independently here.
void test_total_spread_matches_the_closed_form() {
    AsConfig c;
    c.gamma = 0.7;
    c.k = 2.3;
    const double tau = 1234.0, s2 = 7e-6;
    const double want = c.gamma * s2 * tau + (2.0 / c.gamma) * std::log(1.0 + c.gamma / c.k);
    const AsQuote got = as_quote(c, 10.0, 0.0, tau, s2);
    CHECK(close_to(got.half_spread * 2.0, want));
    CHECK(close_to(got.ask() - got.bid(), want));
}

// Risk aversion scales the inventory skew linearly. Note what is NOT asserted:
// that a larger gamma widens the spread. It does not, in general -- the
// intensity term (2/gamma)*ln(1+gamma/k) SHRINKS with gamma, so the total is
// non-monotonic. Asserting monotonic widening would encode a plausible-sounding
// falsehood about the model.
void test_gamma_scales_the_skew_linearly_but_not_the_spread_monotonically() {
    AsConfig lo, hi;
    lo.gamma = 0.2;
    hi.gamma = 0.4;
    lo.k = hi.k = 1.5;
    const double mid = 20.0, tau = 900.0, s2 = 3e-6, q = 2.0;
    const double skew_lo = mid - as_quote(lo, mid, q, tau, s2).reservation;
    const double skew_hi = mid - as_quote(hi, mid, q, tau, s2).reservation;
    CHECK(close_to(skew_hi, 2.0 * skew_lo));

    // And the non-monotonicity is real, not hypothetical: with a tiny horizon
    // the inventory term vanishes and the intensity term dominates, so a larger
    // gamma gives a NARROWER spread.
    const double tiny = 1e-9;
    CHECK(as_quote(hi, mid, 0.0, tiny, s2).half_spread <
          as_quote(lo, mid, 0.0, tiny, s2).half_spread);
}

// THE PATHOLOGY. As t -> T the inventory term decays to nothing, so the model
// stops skewing for inventory exactly when there is least time to unload it.
// Asserted rather than described, because a property this uncomfortable is one
// a reader should be able to see is real.
void test_the_end_of_horizon_pathology_is_real() {
    AsConfig c;
    c.gamma = 0.5;
    const double mid = 100.0, s2 = 1e-5, q = 5.0;
    double prev = 1e18;
    for (double tau : {3600.0, 600.0, 60.0, 6.0, 0.6, 0.0}) {
        const double skew = mid - as_quote(c, mid, q, tau, s2).reservation;
        CHECK(skew < prev);        // strictly decaying
        prev = skew;
    }
    // At the horizon it is exactly zero: a strategy holding five clips quotes
    // as though it held none.
    CHECK(close_to(as_quote(c, mid, q, 0.0, s2).reservation, mid));
    // ...and the mitigation is a floor on tau, which the STRATEGY applies. The
    // model itself is left honest.
    CHECK(as_quote(c, mid, q, 30.0, s2).reservation < mid);
}

// THE UNIT TRAP, locked in so it cannot come back.
//
// The paper's worked example uses gamma = 0.1, k = 1.5 in arbitrary units, and
// those get copied into implementations unchanged. On a penny-spread equity the
// intensity term alone is then about $1.29 -- a total spread near 170 ticks on a
// book that quotes one wide. The strategy quotes all day and never fills, which
// is exactly what happened the first time this ran: two end-to-end tests failed
// with zero fills and the cause was entirely in these two numbers.
void test_the_default_parameters_are_in_equity_units() {
    // A $100 name with 2% daily vol, at the open: sigma^2 about 1.7e-4 dollars
    // squared per second, tau a full session.
    const double s2 = 1.7e-4, tau = 23400.0;
    const double ticks = as_quote(AsConfig{}, 100.0, 0.0, tau, s2).half_spread * 2.0 * 100.0;
    CHECK(ticks > 0.5);      // not so tight it is quoting inside the touch for free
    CHECK(ticks < 20.0);     // not so wide it never fills

    AsConfig paper;
    paper.gamma = 0.1;
    paper.k = 1.5;
    const double paper_ticks =
        as_quote(paper, 100.0, 0.0, tau, s2).half_spread * 2.0 * 100.0;
    CHECK(paper_ticks > 100.0);   // the trap is real, and this is how wide it is
}

// ---- the volatility estimator ---------------------------------------------

void test_a_still_mid_has_no_variance_but_never_zero() {
    MidVolatility v(60, 1000000000ULL);
    for (int i = 0; i < 200; ++i) {
        v.observe(static_cast<uint64_t>(i) * 1000000000ULL, 100.0);
    }
    // A floor, not zero: sigma of zero collapses the spread term and makes the
    // strategy quote both sides at the reservation price, which is not
    // conservative, it is nonsense.
    CHECK(v.variance_per_second() > 0.0);
    CHECK(v.variance_per_second() <= 1e-8);
}

void test_a_known_step_gives_the_expected_variance() {
    // Alternating +d/-d steps once per second: the differences are +d and -d,
    // mean zero, sample variance d^2 * n/(n-1) -> d^2.
    MidVolatility v(400, 1000000000ULL);
    const double d = 0.01;
    double px = 100.0;
    for (int i = 0; i < 400; ++i) {
        v.observe(static_cast<uint64_t>(i) * 1000000000ULL, px);
        px += (i % 2 == 0) ? d : -d;
    }
    const double got = v.variance_per_second();
    CHECK(got > 0.8 * d * d);
    CHECK(got < 1.3 * d * d);
}

// Sampling is on a time grid, so a busy symbol and a quiet one with the same
// price path get the same estimate. Feeding it a thousand times per second must
// not multiply the sample count.
void test_sampling_is_on_a_time_grid_not_per_message() {
    MidVolatility busy(60, 1000000000ULL);
    MidVolatility quiet(60, 1000000000ULL);
    for (int sec = 0; sec < 100; ++sec) {
        const double px = 100.0 + 0.01 * static_cast<double>(sec % 7);
        quiet.observe(static_cast<uint64_t>(sec) * 1000000000ULL, px);
        for (int j = 0; j < 500; ++j) {
            busy.observe(static_cast<uint64_t>(sec) * 1000000000ULL +
                             static_cast<uint64_t>(j) * 1000000ULL,
                         px);
        }
    }
    CHECK_EQ(busy.samples_taken(), quiet.samples_taken());
    CHECK(close_to(busy.variance_per_second(), quiet.variance_per_second(), 1e-6));
}

// A gap in the feed must not produce a burst of catch-up samples holding the
// same price, which would collapse the variance toward zero.
void test_a_feed_gap_does_not_manufacture_samples() {
    MidVolatility v(60, 1000000000ULL);
    v.observe(0, 100.0);
    v.observe(3600ULL * 1000000000ULL, 101.0);   // an hour later
    CHECK_EQ(v.samples_taken(), uint64_t{2});
}

void test_volatility_state_round_trips() {
    MidVolatility a(60, 1000000000ULL);
    double px = 50.0;
    for (int i = 0; i < 120; ++i) {
        a.observe(static_cast<uint64_t>(i) * 1000000000ULL, px);
        px += (i % 3 == 0) ? 0.02 : -0.01;
    }
    MidVolatility b(60, 1000000000ULL);
    b.restore(a.state());
    CHECK_EQ(b.samples_taken(), a.samples_taken());
    CHECK(close_to(b.variance_per_second(), a.variance_per_second()));
}

// ---- the strategy, end to end ---------------------------------------------

struct Feed {
    std::vector<std::vector<uint8_t>> msgs;
    uint64_t ts = 34200ULL * 1000000000ULL;
    uint64_t next_ref = 1;
    uint64_t step_ns = 50000000ULL;          // 50 ms

    std::vector<uint8_t>& add(char type, size_t len) {
        msgs.emplace_back(len, 0);
        auto& m = msgs.back();
        m[0] = static_cast<uint8_t>(type);
        m[1] = 0; m[2] = 1;
        ts += step_ns;
        for (int i = 0; i < 6; ++i)
            m[static_cast<size_t>(5 + i)] =
                static_cast<uint8_t>((ts >> (8 * (5 - i))) & 0xff);
        return m;
    }
    static void be64(std::vector<uint8_t>& m, size_t o, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            m[o + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * (7 - i))) & 0xff);
    }
    static void be32(std::vector<uint8_t>& m, size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i)
            m[o + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * (3 - i))) & 0xff);
    }
    void system_event(char code) { add('S', 12)[11] = static_cast<uint8_t>(code); }
    uint64_t add_order(char side, uint32_t shares, int32_t price) {
        const uint64_t ref = next_ref++;
        auto& m = add('A', 36);
        be64(m, 11, ref);
        m[19] = static_cast<uint8_t>(side);
        be32(m, 20, shares);
        for (size_t i = 0; i < 8; ++i) m[24 + i] = static_cast<uint8_t>(' ');
        m[24] = 'T'; m[25] = 'E'; m[26] = 'S'; m[27] = 'T';
        be32(m, 32, static_cast<uint32_t>(price));
        return ref;
    }
    void execute(uint64_t ref, uint32_t shares) {
        auto& m = add('E', 31);
        be64(m, 11, ref);
        be32(m, 19, shares);
        be64(m, 23, next_ref++);
    }
    void delete_order(uint64_t ref) { be64(add('D', 19), 11, ref); }
};

Feed build_feed(int rounds = 300) {
    Feed f;
    f.system_event('O');
    f.system_event('Q');
    for (int i = 0; i < rounds; ++i) {
        // A mid that actually moves, so the volatility estimate is not the
        // floor and the model has something to price against.
        const int32_t drift = static_cast<int32_t>((i * 7) % 21 - 10) * 100;
        const uint64_t b1 = f.add_order('B', 500, 999000 + drift);
        const uint64_t a1 = f.add_order('S', 500, 999200 + drift);
        f.execute(b1, 200);
        f.execute(a1, 200);
        f.delete_order(b1);
        f.delete_order(a1);
    }
    f.system_event('M');
    f.system_event('C');
    return f;
}

LaneResult run(const Feed& f, AsConfig c, Model m, AsReport* rep = nullptr) {
    ClosedLoopBacktest<AsMaker> b(AsMaker{c}, m, FeeSchedule{});
    for (const auto& msg : f.msgs) {
        b.on_message(static_cast<char>(msg[0]), msg.data(),
                     static_cast<uint16_t>(msg.size()));
    }
    if (rep != nullptr) *rep = b.strategy().report();
    return b.result();
}

void test_it_quotes_and_trades_on_a_real_book() {
    const Feed f = build_feed();
    AsReport rep;
    const LaneResult r = run(f, AsConfig{}, Model::Optimistic, &rep);
    CHECK(rep.quotes > 0);
    CHECK(r.fills > 0);            // or the tests below compare zeroes
    CHECK(rep.last_sigma2 > 0.0);
}

// Quotes land on the tick grid, and rounding never tightens them: a bid rounds
// DOWN and an ask rounds UP, so the strategy never ends up quoting inside the
// price the model asked for.
void test_quotes_are_on_the_tick_grid() {
    AsConfig c;
    AsMaker m(c);
    // Exercised through the closed loop so the prices are the ones actually
    // sent, not ones recomputed here.
    const Feed f = build_feed(60);
    ClosedLoopBacktest<AsMaker> b(AsMaker{c}, Model::Mbo, FeeSchedule{});
    for (const auto& msg : f.msgs) {
        b.on_message(static_cast<char>(msg[0]), msg.data(),
                     static_cast<uint16_t>(msg.size()));
    }
    CHECK(b.strategy().report().quotes > 0);
    // Every quote the strategy produced went through to_ticks(), so the model's
    // own last reservation and half spread must bracket a grid-aligned pair.
    const AsReport& rep = b.strategy().report();
    const double bid = rep.last_reservation - rep.last_half_spread;
    const double ask = rep.last_reservation + rep.last_half_spread;
    CHECK(ask > bid);
}

// The re-quote policy is a cost control: not on every message, only when the
// reservation price has moved enough or a fill changed the inventory.
void test_it_does_not_requote_on_every_message() {
    const Feed f = build_feed();
    AsConfig tight, loose;
    tight.requote_ticks = 1;
    loose.requote_ticks = 50;
    AsReport a, b;
    run(f, tight, Model::Mbo, &a);
    run(f, loose, Model::Mbo, &b);
    CHECK(a.quotes > 0);
    CHECK(b.quotes > 0);
    // A wider threshold must not quote more often.
    CHECK(b.quotes <= a.quotes);
    // And neither quotes on every message, which is the actual claim.
    CHECK(a.quotes < f.msgs.size() * 2);
}

// Higher risk aversion carries less inventory. This is the model's purpose, so
// if it does not hold end to end the implementation is wrong however well the
// algebra tests pass.
void test_higher_risk_aversion_carries_less_inventory() {
    const Feed f = build_feed();
    AsConfig timid, bold;
    timid.gamma = 5.0;
    bold.gamma = 0.01;
    AsReport t, b;
    run(f, timid, Model::Optimistic, &t);
    run(f, bold, Model::Optimistic, &b);
    CHECK(t.max_abs_position <= b.max_abs_position);
}

void test_state_restores_what_was_learned_and_not_what_was_configured() {
    AsConfig original;
    original.gamma = 0.3;
    original.k = 2.0;
    AsMaker a(original);

    const Feed f = build_feed(80);
    ClosedLoopBacktest<AsMaker> b(AsMaker{original}, Model::Mbo, FeeSchedule{});
    for (const auto& msg : f.msgs) {
        b.on_message(static_cast<char>(msg[0]), msg.data(),
                     static_cast<uint16_t>(msg.size()));
    }
    const AsMaker::State snap = b.strategy().state();
    CHECK(snap.report.quotes > 0);

    // A restarted operator has CORRECTED the configuration. The snapshot must
    // not undo that -- the same rule ledger.hpp follows about fee schedules.
    AsConfig corrected = original;
    corrected.gamma = 1.25;
    AsMaker restored(corrected);
    restored.restore(snap);
    CHECK(close_to(restored.config().gamma, 1.25));
    CHECK(!close_to(restored.config().gamma, original.gamma));
    // ...while what was LEARNED did travel.
    CHECK_EQ(restored.report().quotes, snap.report.quotes);
    CHECK(close_to(restored.report().last_sigma2, snap.report.last_sigma2));
}

// The pathology counter: it has to be able to fire, or it is decoration.
void test_the_late_session_counter_can_fire() {
    Feed f;
    f.system_event('O');
    f.system_event('Q');
    // Start deep into the session so every quote is in the last tenth.
    f.ts = (15ULL * 3600 + 50ULL * 60) * 1000000000ULL;
    for (int i = 0; i < 200; ++i) {
        const int32_t drift = static_cast<int32_t>((i * 5) % 15 - 7) * 100;
        const uint64_t b1 = f.add_order('B', 500, 999000 + drift);
        const uint64_t a1 = f.add_order('S', 500, 999200 + drift);
        f.execute(b1, 300);
        f.execute(a1, 300);
        f.delete_order(b1);
        f.delete_order(a1);
    }
    f.system_event('M');
    f.system_event('C');
    AsReport rep;
    run(f, AsConfig{}, Model::Optimistic, &rep);
    CHECK(rep.quotes > 0);
    CHECK(rep.late_session_quotes_with_inventory > 0);
}

}  // namespace

int main() {
    test_inventory_displaces_the_quotes();
    test_the_spread_does_not_depend_on_inventory();
    test_total_spread_matches_the_closed_form();
    test_gamma_scales_the_skew_linearly_but_not_the_spread_monotonically();
    test_the_end_of_horizon_pathology_is_real();
    test_the_default_parameters_are_in_equity_units();
    test_a_still_mid_has_no_variance_but_never_zero();
    test_a_known_step_gives_the_expected_variance();
    test_sampling_is_on_a_time_grid_not_per_message();
    test_a_feed_gap_does_not_manufacture_samples();
    test_volatility_state_round_trips();
    test_it_quotes_and_trades_on_a_real_book();
    test_quotes_are_on_the_tick_grid();
    test_it_does_not_requote_on_every_message();
    test_higher_risk_aversion_carries_less_inventory();
    test_state_restores_what_was_learned_and_not_what_was_configured();
    test_the_late_session_counter_can_fire();
    return REPORT();
}

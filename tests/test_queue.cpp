// test_queue — hand-computed queue scenarios.
//
// Every case here is a rule from docs/phase6-design.md, and most of them are a
// trap from its section 11. The fuzzer proves invariants hold; this proves the
// model does the specific things the microstructure requires.
#include <cstdint>
#include <vector>

#include "itchbook/sim/queue_model.hpp"
#include "tests/check.hpp"

using namespace itchbook::sim;
namespace bk = itchbook::book;

namespace {

constexpr Price4 P100 = 1000000;   // $100.0000
constexpr Price4 TICK = 100;

QueueModel make(Model m, bool clamp = true) {
    QueueConfig c;
    c.model = m;
    c.clamp = clamp;
    QueueModel q(c);
    q.set_trading_state('T');
    return q;
}

// A trade at a resting order's own price.
Resolved trade(Side side, Price4 price, uint32_t shares, uint64_t ref = 1, uint64_t ts = 10) {
    Resolved r;
    r.type = 'E';
    r.cls = Class::Trade;
    r.known = true;
    r.ref = ref;
    r.side = side;
    r.resting_price = price;
    r.shares = shares;
    r.printable = true;
    r.print_price = price;
    r.ts = ts;
    return r;
}

Resolved cancel(Side side, Price4 price, uint32_t shares, uint64_t ref = 1, uint64_t ts = 10) {
    Resolved r;
    r.type = 'D';
    r.cls = Class::Cancel;
    r.known = true;
    r.ref = ref;
    r.side = side;
    r.resting_price = price;
    r.shares = shares;
    r.ts = ts;
    return r;
}

void test_arrival_records_shares_ahead() {
    bk::Book b;
    b.add(1, 'B', P100, 300);
    b.add(2, 'B', P100, 200);
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    CHECK_EQ(q.find(100)->ahead, 500u);   // behind both
    CHECK_EQ(q.find(100)->ahead0, 500u);
}

void test_executions_advance_every_model() {
    for (Model m : {Model::Optimistic, Model::Pessimistic, Model::Mbo}) {
        bk::Book b;
        b.add(1, 'B', P100, 500);
        QueueModel q = make(m);
        q.place(b, 100, Side::Buy, P100, 100, 0, 1);
        std::vector<SimFill> fills;
        b.take(1, 200);
        q.commit(trade(Side::Buy, P100, 200), b, &fills);
        CHECK_EQ(q.find(100)->ahead, 300u);
        CHECK_EQ(fills.size(), 0u);        // advanced, not filled
    }
}

void test_cancels_are_where_the_models_differ() {
    // The one boolean the whole phase turns on.
    bk::Book b0;
    b0.add(1, 'B', P100, 500);

    bk::Book bo;
    bo.add(1, 'B', P100, 500);
    QueueModel opt = make(Model::Optimistic, /*clamp=*/false);
    opt.place(bo, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> f1;
    bo.cancel(1, 200);
    opt.commit(cancel(Side::Buy, P100, 200), bo, &f1);
    CHECK_EQ(opt.find(100)->ahead, 300u);   // assumed to be in front of us

    bk::Book bp;
    bp.add(1, 'B', P100, 500);
    QueueModel pess = make(Model::Pessimistic, /*clamp=*/false);
    pess.place(bp, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> f2;
    bp.cancel(1, 200);
    pess.commit(cancel(Side::Buy, P100, 200), bp, &f2);
    CHECK_EQ(pess.find(100)->ahead, 500u);  // assumed to be behind us

    CHECK_EQ(f1.size(), 0u);   // and neither one fills on a cancel
    CHECK_EQ(f2.size(), 0u);
}

void test_a_cancel_never_fills_us() {
    // Trap 6: one uniform "consume the queue then fill from the remainder" path
    // fills you off a large delete at your price. The two rules are separate.
    bk::Book b;
    b.add(1, 'B', P100, 100);
    QueueModel q = make(Model::Optimistic, /*clamp=*/false);
    q.place(b, 100, Side::Buy, P100, 500, 0, 1);
    std::vector<SimFill> fills;
    b.remove(1);
    q.commit(cancel(Side::Buy, P100, 100), b, &fills);
    CHECK_EQ(fills.size(), 0u);
    CHECK_EQ(q.find(100)->ahead, 0u);
    CHECK_EQ(q.find(100)->display, 500u);   // still fully resting
}

void test_trade_spills_into_a_fill_once_the_queue_is_gone() {
    bk::Book b;
    b.add(1, 'B', P100, 100);
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 100, Side::Buy, P100, 200, 0, 1);
    CHECK_EQ(q.find(100)->ahead, 100u);

    std::vector<SimFill> fills;
    b.take(1, 100);                                   // the 100 ahead trades out
    q.commit(trade(Side::Buy, P100, 250), b, &fills); // 250 traded: 100 ahead, 150 to us
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(fills[0].shares, 150u);
    CHECK_EQ(fills[0].price, P100);
    CHECK_EQ(q.find(100)->display, 50u);              // 200 - 150 left
    CHECK_EQ(q.find(100)->ahead, 0u);                 // and now at the front
}

void test_fill_is_capped_at_our_size() {
    // A trade far larger than our order cannot fill more than we have.
    bk::Book b;
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> fills;
    q.commit(trade(Side::Buy, P100, 1000000), b, &fills);
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(fills[0].shares, 100u);
}

void test_unsigned_underflow_does_not_invent_a_fill() {
    // Trap 7: `removed - ahead` evaluated the wrong way round wraps to ~4e9 and
    // reports a fill of the entire order.
    bk::Book b;
    b.add(1, 'B', P100, 10000);
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> fills;
    b.take(1, 1);
    q.commit(trade(Side::Buy, P100, 1), b, &fills);
    CHECK_EQ(fills.size(), 0u);
    CHECK_EQ(q.find(100)->ahead, 9999u);
}

void test_opposite_side_and_other_prices_are_ignored() {
    bk::Book b;
    b.add(1, 'B', P100, 500);
    QueueModel q = make(Model::Optimistic, /*clamp=*/false);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> fills;
    q.commit(cancel(Side::Sell, P100, 100), b, &fills);        // other side
    q.commit(cancel(Side::Buy, P100 - TICK, 100), b, &fills);  // worse price
    q.commit(cancel(Side::Buy, P100 + TICK, 100), b, &fills);  // better price
    CHECK_EQ(q.find(100)->ahead, 500u);                        // untouched
    CHECK_EQ(fills.size(), 0u);
}

void test_the_clamp_bounds_ahead_by_reality() {
    // Pessimistic never advances on cancels, so without the clamp it can claim
    // 500 shares sit in front of us at a level that is provably empty.
    bk::Book b;
    b.add(1, 'B', P100, 500);
    QueueModel unclamped = make(Model::Pessimistic, /*clamp=*/false);
    QueueModel clamped = make(Model::Pessimistic, /*clamp=*/true);
    unclamped.place(b, 100, Side::Buy, P100, 100, 0, 1);
    clamped.place(b, 100, Side::Buy, P100, 100, 0, 1);

    std::vector<SimFill> f;
    b.remove(1);                                     // the level empties
    Resolved r = cancel(Side::Buy, P100, 500);
    unclamped.commit(r, b, &f);
    clamped.commit(r, b, &f);

    CHECK_EQ(unclamped.find(100)->ahead, 500u);      // not a bound, a bug
    CHECK_EQ(clamped.find(100)->ahead, 0u);          // at the front, correctly
    CHECK(clamped.clamp_events() > 0);
    CHECK_EQ(clamped.clamp_shares(), 500u);
}

void test_clamp_keeps_our_own_earlier_order_ahead_of_us() {
    // Trap 4: our orders are not in book::Book, so clamping to the raw level
    // total deletes our own earlier order from `ahead` and double-fills a
    // two-order quoting strategy.
    bk::Book b;
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 1, Side::Buy, P100, 100, 0, 1);   // ours, first
    q.place(b, 2, Side::Buy, P100, 100, 0, 2);   // ours, behind it
    CHECK_EQ(q.find(2)->ahead, 100u);

    std::vector<SimFill> fills;
    q.commit(cancel(Side::Buy, P100 + TICK, 1), b, &fills);   // an unrelated event
    CHECK_EQ(q.find(2)->ahead, 100u);   // still behind our own first order
}

void test_partial_fill_leaves_us_at_the_front() {
    // Trap 12: re-deriving `ahead` after a partial fill re-inserts us behind
    // liquidity the counterfactual already traded through, making a partial fill
    // strictly worse than no fill at all.
    bk::Book b;
    b.add(1, 'B', P100, 400);        // real liquidity that stays in the book
    QueueModel q = make(Model::Pessimistic, /*clamp=*/false);
    q.place(b, 100, Side::Buy, P100, 300, 0, 1);
    std::vector<SimFill> fills;
    q.commit(trade(Side::Buy, P100, 500), b, &fills);   // eats 400 ahead, 100 to us
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(fills[0].shares, 100u);
    CHECK_EQ(q.find(100)->ahead, 0u);      // front, despite 400 still in the book
    CHECK_EQ(q.find(100)->display, 200u);
}

void test_mbo_resolves_by_reference() {
    // The whole point of an order-by-order feed: whether that cancel was ahead
    // of us is recorded, not guessed.
    bk::Book b;
    b.add(11, 'B', P100, 300);       // resting before we arrive -> ahead
    QueueModel q = make(Model::Mbo, /*clamp=*/false);
    q.place(b, 100, Side::Buy, P100, 100, 0, 5);
    b.add(22, 'B', P100, 200);       // arrives after us -> behind

    std::vector<SimFill> fills;
    q.commit(cancel(Side::Buy, P100, 200, /*ref=*/22), b, &fills);
    CHECK_EQ(q.find(100)->ahead, 300u);   // behind-us cancel: no advance

    q.commit(cancel(Side::Buy, P100, 300, /*ref=*/11), b, &fills);
    CHECK_EQ(q.find(100)->ahead, 0u);     // ahead-of-us cancel: advances
}

void test_mbo_is_bracketed_by_the_two_bounds() {
    // The containment invariant, which is what makes mbo an internal oracle:
    // optimistic <= mbo <= pessimistic, in shares still believed ahead.
    bk::Book bo, bm, bp;
    for (bk::Book* b : {&bo, &bm, &bp}) {
        b->add(11, 'B', P100, 300);
    }
    QueueModel opt = make(Model::Optimistic, false);
    QueueModel mbo = make(Model::Mbo, false);
    QueueModel pess = make(Model::Pessimistic, false);
    opt.place(bo, 1, Side::Buy, P100, 100, 0, 5);
    mbo.place(bm, 1, Side::Buy, P100, 100, 0, 5);
    pess.place(bp, 1, Side::Buy, P100, 100, 0, 5);

    for (bk::Book* b : {&bo, &bm, &bp}) {
        b->add(22, 'B', P100, 200);   // behind us
    }
    std::vector<SimFill> f;
    Resolved r = cancel(Side::Buy, P100, 200, /*ref=*/22);
    opt.commit(r, bo, &f);
    mbo.commit(r, bm, &f);
    pess.commit(r, bp, &f);

    CHECK(opt.find(1)->ahead <= mbo.find(1)->ahead);
    CHECK(mbo.find(1)->ahead <= pess.find(1)->ahead);
    CHECK_EQ(mbo.find(1)->ahead, 300u);   // the truth, here
    CHECK_EQ(opt.find(1)->ahead, 100u);   // too optimistic
    CHECK_EQ(pess.find(1)->ahead, 300u);
}

void test_naive_ignores_the_queue_entirely() {
    bk::Book b;
    b.add(1, 'B', P100, 100000);      // an enormous queue in front
    QueueModel q = make(Model::Naive);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> fills;
    q.commit(trade(Side::Buy, P100, 100), b, &fills);
    CHECK_EQ(fills.size(), 1u);       // filled anyway: that is the fantasy
    CHECK_EQ(fills[0].shares, 100u);
}

void test_naive_is_capped_by_the_print_size() {
    // "Fill whenever the market trades at your price", taken literally, lets a
    // 500-share quote fill in full against a 100-share print and invents 400
    // shares that never traded. Then naive is not an upper bound on anything.
    bk::Book b;
    QueueModel q = make(Model::Naive);
    q.place(b, 100, Side::Buy, P100, 500, 0, 1);
    std::vector<SimFill> fills;
    q.commit(trade(Side::Buy, P100, 100), b, &fills);
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(fills[0].shares, 100u);
    CHECK_EQ(q.find(100)->display, 400u);
}

void test_naive_never_fills_on_a_cross() {
    // Trap: a closing cross is millions of shares at one price. A naive model
    // that fills on every print at-or-through its limit consumes its whole order
    // on that one message, and the headline chart becomes a measurement of the
    // auction. 40.6% of the validated day's volume was in two cross messages.
    bk::Book b;
    QueueModel q = make(Model::Naive);
    q.place(b, 100, Side::Buy, P100, 500, 0, 1);
    Resolved cross = trade(Side::Buy, P100, 2500408);
    cross.type = 'Q';
    std::vector<SimFill> fills;
    q.commit(cross, b, &fills);
    CHECK_EQ(fills.size(), 0u);
}

void test_naive_hidden_fills_are_counted_separately() {
    bk::Book b;
    QueueModel q = make(Model::Naive);
    q.place(b, 100, Side::Buy, P100, 500, 0, 1);
    Resolved hidden = trade(Side::Buy, P100, 100);
    hidden.type = 'P';
    std::vector<SimFill> fills;
    q.commit(hidden, b, &fills);
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(q.naive_hidden_fills(), 1u);     // reported, because it flatters naive
    CHECK_EQ(q.naive_hidden_shares(), 100u);
}

void test_a_trade_through_our_price_fills_us() {
    // R3. A sweep that eats our level and walks past it would have taken us
    // first under price priority. Without this the fill is capped at our level's
    // real depth, and the omission is one-directional.
    bk::Book b;
    QueueModel q = make(Model::Pessimistic, /*clamp=*/false);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    std::vector<SimFill> fills;
    Resolved through = trade(Side::Buy, P100 - TICK, 300);   // printed BELOW our bid
    q.commit(through, b, &fills);
    CHECK_EQ(fills.size(), 1u);
    CHECK(fills[0].trigger == Trigger::Through);
    CHECK_EQ(fills[0].price, P100);    // at OUR price, not the print price
}

void test_the_other_side_coming_to_us_is_a_fill() {
    // R1. Our order is not in book::Book, so the feed can legally rest an ask at
    // our bid and Book::crossed() stays false. In reality that takes us out.
    bk::Book b;
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    b.add(9, 'S', P100, 400);          // an ask arrives AT our bid: locked
    std::vector<SimFill> fills;
    q.apply_price_priority(b, 20, &fills);
    CHECK_EQ(fills.size(), 1u);
    CHECK(fills[0].trigger == Trigger::Lock);
    CHECK_EQ(fills[0].shares, 100u);
}

void test_iceberg_shows_a_slice_and_rejoins_the_back() {
    bk::Book b;
    QueueModel q = make(Model::Pessimistic, /*clamp=*/false);
    q.place(b, 100, Side::Buy, P100, 500, /*display=*/100, 1);
    CHECK_EQ(q.find(100)->display, 100u);
    CHECK_EQ(q.find(100)->hidden, 400u);

    b.add(7, 'B', P100, 250);        // real liquidity we will rejoin behind
    std::vector<SimFill> fills;
    q.commit(trade(Side::Buy, P100, 100), b, &fills);
    CHECK_EQ(fills.size(), 1u);
    CHECK_EQ(fills[0].shares, 100u);            // capped at the SLICE, not 500
    CHECK_EQ(q.find(100)->display, 100u);       // next slice showing
    CHECK_EQ(q.find(100)->hidden, 300u);
    CHECK_EQ(q.find(100)->ahead, 250u);         // and back behind the queue
}

void test_halts_suspend_every_model_identically() {
    for (char state : {'H', 'P', 'Q', '\0'}) {
        bk::Book b;
        QueueConfig c;
        c.model = Model::Naive;
        QueueModel q(c);
        q.set_trading_state(state);
        q.place(b, 100, Side::Buy, P100, 100, 0, 1);
        std::vector<SimFill> fills;
        q.commit(trade(Side::Buy, P100, 100), b, &fills);
        CHECK_EQ(fills.size(), 0u);
        CHECK(!q.tradable());
    }
}

void test_cancelling_our_order_removes_it() {
    bk::Book b;
    QueueModel q = make(Model::Pessimistic);
    q.place(b, 100, Side::Buy, P100, 100, 0, 1);
    q.cancel(100);
    std::vector<SimFill> fills;
    q.commit(trade(Side::Buy, P100, 100), b, &fills);
    CHECK_EQ(fills.size(), 0u);
}

}  // namespace

int main() {
    test_arrival_records_shares_ahead();
    test_executions_advance_every_model();
    test_cancels_are_where_the_models_differ();
    test_a_cancel_never_fills_us();
    test_trade_spills_into_a_fill_once_the_queue_is_gone();
    test_fill_is_capped_at_our_size();
    test_unsigned_underflow_does_not_invent_a_fill();
    test_opposite_side_and_other_prices_are_ignored();
    test_the_clamp_bounds_ahead_by_reality();
    test_clamp_keeps_our_own_earlier_order_ahead_of_us();
    test_partial_fill_leaves_us_at_the_front();
    test_mbo_resolves_by_reference();
    test_mbo_is_bracketed_by_the_two_bounds();
    test_naive_ignores_the_queue_entirely();
    test_naive_is_capped_by_the_print_size();
    test_naive_never_fills_on_a_cross();
    test_naive_hidden_fills_are_counted_separately();
    test_a_trade_through_our_price_fills_us();
    test_the_other_side_coming_to_us_is_a_fill();
    test_iceberg_shows_a_slice_and_rejoins_the_back();
    test_halts_suspend_every_model_identically();
    test_cancelling_our_order_removes_it();
    return REPORT();
}

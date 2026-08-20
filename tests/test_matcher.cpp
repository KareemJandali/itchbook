// test_matcher — the matching engine's semantics, one rule at a time.
//
// The property fuzzer proves invariants hold over a million random sequences.
// These prove the engine does the specific things an exchange is supposed to
// do, which a fuzzer checking invariants would never notice: an invariant-safe
// engine that fills at the wrong price is still wrong.
#include <cstdint>
#include <vector>

#include "itchbook/engine/matcher.hpp"
#include "tests/check.hpp"

using namespace itchbook::engine;

namespace {

constexpr int32_t P(int cents) { return cents * 100; }   // Price(4) from cents

Request limit(uint64_t id, Side side, int32_t price, uint32_t qty) {
    Request r;
    r.id = id;
    r.side = side;
    r.type = Type::Limit;
    r.price = price;
    r.quantity = qty;
    return r;
}

void test_resting_order_sets_the_price() {
    // The order that was there first sets the terms. A buyer willing to pay
    // $10.50 who lifts a $10.00 offer pays $10.00 — the improvement is theirs.
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    Request take = limit(2, Side::Buy, P(1050), 100);
    Result r = m.submit(take);

    CHECK_EQ(r.filled, 100u);
    CHECK_EQ(m.fills().size(), 1u);
    CHECK_EQ(m.fills()[0].price, P(1000));
    CHECK(r.state == State::Filled);
}

void test_price_time_priority() {
    // Same price: first in, first filled.
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 50));
    m.submit(limit(2, Side::Sell, P(1000), 50));
    m.submit(limit(3, Side::Sell, P(1000), 50));
    m.submit(limit(4, Side::Buy, P(1000), 100));

    CHECK_EQ(m.fills().size(), 2u);
    CHECK_EQ(m.fills()[0].maker, 1u);
    CHECK_EQ(m.fills()[1].maker, 2u);
    CHECK(m.find(3)->state == State::Accepted);   // untouched
}

void test_better_price_trades_first() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1002), 100));
    m.submit(limit(2, Side::Sell, P(1000), 100));   // better offer, later arrival
    m.submit(limit(3, Side::Buy, P(1005), 150));

    CHECK_EQ(m.fills().size(), 2u);
    CHECK_EQ(m.fills()[0].maker, 2u);          // price beats time
    CHECK_EQ(m.fills()[0].price, P(1000));
    CHECK_EQ(m.fills()[1].price, P(1002));
}

void test_book_never_crosses() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    m.submit(limit(2, Side::Buy, P(1010), 50));   // marketable, fully filled
    CHECK(!m.crossed());
    m.submit(limit(3, Side::Buy, P(1010), 500));  // takes the rest, then rests
    CHECK(!m.crossed());
    int32_t bid = 0;
    CHECK(m.best_bid(&bid));
    CHECK_EQ(bid, P(1010));
}

void test_partial_fill_leaves_a_remainder_resting() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 30));
    Result r = m.submit(limit(2, Side::Buy, P(1000), 100));
    CHECK_EQ(r.filled, 30u);
    CHECK_EQ(r.resting, 70u);
    CHECK(r.state == State::PartiallyFilled);
    CHECK_EQ(m.book().shares_at('B', P(1000)), 70u);
}

void test_market_order_never_rests() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 40));
    Request mk;
    mk.id = 2;
    mk.side = Side::Buy;
    mk.type = Type::Market;
    mk.quantity = 100;
    Result r = m.submit(mk);
    CHECK_EQ(r.filled, 40u);
    CHECK_EQ(r.cancelled, 60u);      // the rest evaporates
    CHECK_EQ(r.resting, 0u);
    CHECK(r.state == State::Cancelled);
}

void test_market_order_with_no_liquidity_is_rejected() {
    Matcher m;
    Request mk;
    mk.id = 1;
    mk.side = Side::Buy;
    mk.type = Type::Market;
    mk.quantity = 100;
    Result r = m.submit(mk);
    CHECK(r.state == State::Rejected);
    CHECK(r.reject == Reject::NoLiquidity);
    CHECK_EQ(m.shares_submitted(), 0u);   // never entered the accounting
}

void test_ioc_takes_what_it_can_and_drops_the_rest() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 40));
    Request ioc = limit(2, Side::Buy, P(1000), 100);
    ioc.type = Type::IOC;
    Result r = m.submit(ioc);
    CHECK_EQ(r.filled, 40u);
    CHECK_EQ(r.cancelled, 60u);
    CHECK(r.state == State::Cancelled);
    CHECK_EQ(m.book().shares_at('B', P(1000)), 0u);
}

void test_fok_is_all_or_nothing() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 40));

    Request fok = limit(2, Side::Buy, P(1000), 100);
    fok.type = Type::FOK;
    Result r = m.submit(fok);
    CHECK(r.state == State::Rejected);
    CHECK(r.reject == Reject::FokUnfillable);
    CHECK_EQ(m.fills().size(), 0u);                       // nothing traded
    CHECK_EQ(m.book().shares_at('S', P(1000)), 40u);      // book untouched

    Request ok = limit(3, Side::Buy, P(1000), 40);
    ok.type = Type::FOK;
    Result r2 = m.submit(ok);
    CHECK(r2.state == State::Filled);
    CHECK_EQ(r2.filled, 40u);
}

void test_fok_may_sweep_several_levels() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 40));
    m.submit(limit(2, Side::Sell, P(1001), 40));
    Request fok = limit(3, Side::Buy, P(1001), 80);
    fok.type = Type::FOK;
    Result r = m.submit(fok);
    CHECK(r.state == State::Filled);
    CHECK_EQ(r.filled, 80u);
}

void test_iceberg_shows_only_its_slice() {
    Matcher m;
    Request ice = limit(1, Side::Sell, P(1000), 500);
    ice.display = 100;
    m.submit(ice);
    // The book holds one slice; the rest is hidden.
    CHECK_EQ(m.book().shares_at('S', P(1000)), 100u);
    CHECK_EQ(m.find(1)->hidden, 400u);
}

void test_iceberg_refresh_goes_to_the_back_of_the_queue() {
    // Hiding size costs queue position. If it did not, everyone would hide
    // everything and price-time priority would mean nothing.
    Matcher m;
    Request ice = limit(1, Side::Sell, P(1000), 300);
    ice.display = 100;
    m.submit(ice);
    m.submit(limit(2, Side::Sell, P(1000), 50));   // queued behind the slice

    // Take the whole visible slice; the iceberg refreshes behind order 2.
    m.submit(limit(3, Side::Buy, P(1000), 100));
    CHECK_EQ(m.fills().size(), 1u);
    CHECK_EQ(m.fills()[0].maker, 1u);

    // The next taker must hit order 2 first, not the refreshed slice.
    m.submit(limit(4, Side::Buy, P(1000), 50));
    CHECK_EQ(m.fills().size(), 2u);
    CHECK_EQ(m.fills()[1].maker, 2u);
    CHECK_EQ(m.find(1)->hidden, 100u);
}

void test_stp_cancel_newest() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));   // owner 0
    Request take = limit(2, Side::Buy, P(1000), 100);
    take.stp = Stp::CancelNewest;
    Result r = m.submit(take);
    CHECK_EQ(r.filled, 0u);
    CHECK(r.state == State::Cancelled);
    CHECK_EQ(m.book().shares_at('S', P(1000)), 100u);   // the resting side lives
}

void test_stp_cancel_oldest() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));   // ours: owner 0
    Request other = limit(2, Side::Sell, P(1001), 100);
    other.owner = 5;                                // somebody else's
    m.submit(other);
    Request take = limit(3, Side::Buy, P(1001), 100);
    take.stp = Stp::CancelOldest;
    Result r = m.submit(take);
    // Order 1 is pulled, then the taker trades with order 2 as normal.
    CHECK(m.find(1)->state == State::Cancelled);
    CHECK_EQ(r.filled, 100u);
    CHECK_EQ(m.fills()[0].maker, 2u);
}

void test_stp_cancel_both() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    Request take = limit(2, Side::Buy, P(1000), 100);
    take.stp = Stp::CancelBoth;
    Result r = m.submit(take);
    CHECK(m.find(1)->state == State::Cancelled);
    CHECK(r.state == State::Cancelled);
    CHECK_EQ(r.filled, 0u);
    CHECK_EQ(m.book().shares_at('S', P(1000)), 0u);
}

void test_stp_only_applies_to_the_same_owner() {
    Matcher m;
    Request rest_order = limit(1, Side::Sell, P(1000), 100);
    rest_order.owner = 7;
    m.submit(rest_order);

    Request take = limit(2, Side::Buy, P(1000), 100);
    take.owner = 9;                 // different owner
    take.stp = Stp::CancelBoth;
    Result r = m.submit(take);
    CHECK_EQ(r.filled, 100u);       // trades normally
    CHECK(r.state == State::Filled);
}

void test_stop_sleeps_until_the_market_trades_through_it() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    m.submit(limit(2, Side::Sell, P(1010), 100));

    Request stop;
    stop.id = 3;
    stop.side = Side::Buy;
    stop.type = Type::StopMarket;
    stop.stop_price = P(1005);
    stop.quantity = 50;
    Result r = m.submit(stop);
    CHECK(r.state == State::Accepted);
    CHECK_EQ(m.fills().size(), 0u);          // dormant

    // A trade at 1000 is below the trigger; the stop stays asleep.
    m.submit(limit(4, Side::Buy, P(1000), 100));
    CHECK(m.find(3)->state == State::Accepted);

    // A trade at 1010 takes the price through it, and it fires.
    m.submit(limit(5, Side::Sell, P(1010), 0 + 100));
    m.submit(limit(6, Side::Buy, P(1010), 100));
    CHECK(is_terminal(m.find(3)->state) || m.find(3)->filled > 0);
}

void test_cancel_removes_resting_shares() {
    Matcher m;
    m.submit(limit(1, Side::Buy, P(1000), 100));
    CHECK(m.cancel(1));
    CHECK(m.find(1)->state == State::Cancelled);
    CHECK_EQ(m.book().shares_at('B', P(1000)), 0u);
    CHECK(!m.cancel(1));       // already terminal
    CHECK(!m.cancel(999));     // never existed
}

void test_rejections() {
    Matcher m;
    Request zero = limit(1, Side::Buy, P(1000), 0);
    CHECK(m.submit(zero).reject == Reject::ZeroQuantity);

    Request bad_price = limit(2, Side::Buy, 0, 100);
    CHECK(m.submit(bad_price).reject == Reject::InvalidPrice);

    Request big_display = limit(3, Side::Buy, P(1000), 100);
    big_display.display = 500;
    CHECK(m.submit(big_display).reject == Reject::DisplayTooLarge);

    m.submit(limit(4, Side::Buy, P(1000), 100));
    CHECK(m.submit(limit(4, Side::Buy, P(1000), 100)).reject == Reject::DuplicateId);
}

void test_state_machine_rules() {
    CHECK(legal_transition(State::New, State::Accepted));
    CHECK(legal_transition(State::Accepted, State::PartiallyFilled));
    CHECK(legal_transition(State::PartiallyFilled, State::Filled));
    CHECK(legal_transition(State::PartiallyFilled, State::PartiallyFilled));

    // Nothing leaves a terminal state.
    CHECK(!legal_transition(State::Filled, State::PartiallyFilled));
    CHECK(!legal_transition(State::Cancelled, State::Accepted));
    CHECK(!legal_transition(State::Rejected, State::Accepted));
    // ...and nothing goes backwards.
    CHECK(!legal_transition(State::Accepted, State::New));
    CHECK(!legal_transition(State::PartiallyFilled, State::Accepted));
}

void test_share_conservation_holds() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    m.submit(limit(2, Side::Sell, P(1001), 100));
    Request ice = limit(3, Side::Buy, P(999), 500);
    ice.display = 100;
    m.submit(ice);
    m.submit(limit(4, Side::Buy, P(1001), 150));
    m.cancel(3);
    CHECK(m.conserves_shares());
    CHECK_EQ(m.shares_filled(), 2 * m.volume_traded());
}

}  // namespace

int main() {
    test_resting_order_sets_the_price();
    test_price_time_priority();
    test_better_price_trades_first();
    test_book_never_crosses();
    test_partial_fill_leaves_a_remainder_resting();
    test_market_order_never_rests();
    test_market_order_with_no_liquidity_is_rejected();
    test_ioc_takes_what_it_can_and_drops_the_rest();
    test_fok_is_all_or_nothing();
    test_fok_may_sweep_several_levels();
    test_iceberg_shows_only_its_slice();
    test_iceberg_refresh_goes_to_the_back_of_the_queue();
    test_stp_cancel_newest();
    test_stp_cancel_oldest();
    test_stp_cancel_both();
    test_stp_only_applies_to_the_same_owner();
    test_stop_sleeps_until_the_market_trades_through_it();
    test_cancel_removes_resting_shares();
    test_rejections();
    test_state_machine_rules();
    test_share_conservation_holds();
    return REPORT();
}

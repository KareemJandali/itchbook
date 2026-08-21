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

Request stop(uint64_t id, Side side, Type type, int32_t trigger, int32_t limit_price,
             uint32_t qty) {
    Request r;
    r.id = id;
    r.side = side;
    r.type = type;
    r.stop_price = trigger;
    r.price = limit_price;      // ignored by StopMarket
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

// ---- stop-limit ------------------------------------------------------------
//
// StopLimit had no test anywhere and the property fuzzer never emitted a stop
// of either kind, so four of the six order types the engine implements carried
// all the evidence. Adding stops to the fuzzer found two bugs in an afternoon;
// these pin the behaviour those bugs were hiding.

void test_stop_limit_becomes_a_limit_at_its_own_price() {
    // The distinction that makes StopLimit worth having: once triggered it will
    // NOT pay any price. It becomes a limit order and rests if the market has
    // moved past it, where a StopMarket would have chased.
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    m.submit(limit(2, Side::Buy, P(1000), 100));    // trade at 1000 -> last_trade
    CHECK_EQ(m.fills().size(), 1u);

    // Buy stop triggered at 1000, but unwilling to pay more than 1001.
    // The only offer left is at 1020, so it must rest, not lift it.
    m.submit(limit(3, Side::Sell, P(1020), 100));
    Result r = m.submit(stop(4, Side::Buy, Type::StopLimit, P(1000), P(1001), 50));

    CHECK_EQ(r.filled, 0u);
    CHECK(m.find(4)->state == State::Accepted);
    CHECK_EQ(m.book().shares_at('B', P(1001)), 50u);   // resting at ITS price
    CHECK_EQ(m.book().shares_at('S', P(1020)), 100u);  // the offer is untouched
}

void test_stop_limit_takes_liquidity_when_its_price_allows() {
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    m.submit(limit(2, Side::Buy, P(1000), 100));    // trade at 1000
    m.submit(limit(3, Side::Sell, P(1010), 100));

    // Same trigger, but willing to pay 1010 this time.
    Result r = m.submit(stop(4, Side::Buy, Type::StopLimit, P(1000), P(1010), 50));
    CHECK_EQ(r.filled, 50u);
    CHECK_EQ(m.fills().back().price, P(1010));      // the maker's price
    CHECK(m.find(4)->state == State::Filled);
}

void test_stop_market_firing_into_an_empty_book_is_cancelled_not_rejected() {
    // The first bug the fuzzer found. A parked stop is already Accepted, and
    // Accepted -> Rejected is not a legal transition, so this aborted the
    // process. Rejected means "never touched the book"; an order that was
    // accepted and then could not trade is CANCELLED.
    Matcher m;
    m.submit(limit(1, Side::Sell, P(1000), 100));
    m.submit(limit(2, Side::Buy, P(1000), 100));    // trade at 1000

    // Park a buy stop above the market. It must actually PARK — a stop whose
    // trigger is already met runs straight from New and never reaches the
    // state this test is about.
    Result parked = m.submit(stop(3, Side::Buy, Type::StopMarket, P(1010), 0, 50));
    CHECK(parked.state == State::Accepted);

    // Now trade at 1010 in a way that leaves nothing on the offer, so the stop
    // fires into an empty book.
    m.submit(limit(4, Side::Sell, P(1010), 100));
    m.submit(limit(5, Side::Buy, P(1010), 100));
    CHECK(!m.book().best_order('S'));

    CHECK(m.find(3)->state == State::Cancelled);
    CHECK(m.conserves_shares());

    // ...while the same refusal from New is still a rejection.
    Matcher m2;
    Request mk = limit(1, Side::Buy, P(1000), 50);
    mk.type = Type::Market;
    Result r2 = m2.submit(mk);
    CHECK(r2.state == State::Rejected);
    CHECK(r2.reject == Reject::NoLiquidity);
}

void test_triggered_stop_takes_its_sequence_at_trigger_time() {
    // The second bug. A stop is parked, not queued — nobody can trade with it
    // and it holds no shares. It joins the market when it TRIGGERS, so that is
    // when its arrival sequence is taken. Keeping the sequence from submit
    // time let a stop parked early rest ahead of orders that had been queued
    // at that price the whole time.
    Matcher m;
    // Park the stop FIRST, so a submit-time sequence would put it in front.
    m.submit(stop(1, Side::Buy, Type::StopLimit, P(1010), P(1005), 50));
    CHECK(m.find(1)->state == State::Accepted);
    CHECK_EQ(m.book().shares_at('B', P(1005)), 0u);   // parked, not resting

    // A plain limit queues at 1005 while the stop is still asleep.
    m.submit(limit(2, Side::Buy, P(1005), 50));

    // Now trade at 1010 to trigger the stop.
    m.submit(limit(3, Side::Sell, P(1010), 10));
    m.submit(limit(4, Side::Buy, P(1010), 10));
    CHECK(m.find(1)->state == State::Accepted);
    CHECK_EQ(m.book().shares_at('B', P(1005)), 100u);  // both now resting

    // The book's intrusive list is FIFO by insertion, so it gets this right on
    // its own. What was wrong is the SEQUENCE — the engine's separate record of
    // arrival order, which the queue models in phase 6 read off every fill.
    // The stop was submitted first and so held the lower number while resting
    // behind. Two mechanisms that are supposed to agree, disagreeing.
    CHECK(m.find(1)->sequence > m.find(2)->sequence);

    // Order 2 was queued first in real time, so it must fill first...
    m.submit(limit(5, Side::Sell, P(1005), 50));
    CHECK_EQ(m.fills().back().maker, 2u);
    CHECK_EQ(m.find(2)->filled, 50u);
    CHECK_EQ(m.find(1)->filled, 0u);

    // ...and the fill must report a sequence consistent with that, which is
    // the form the fuzzer's price-time invariant checks.
    CHECK(m.fills().back().maker_sequence < m.find(1)->sequence);
}

void test_stop_needs_a_trigger_price() {
    Matcher m;
    Result r = m.submit(stop(1, Side::Buy, Type::StopLimit, 0, P(1000), 50));
    CHECK(r.state == State::Rejected);
    CHECK(r.reject == Reject::InvalidPrice);
}

void test_sell_stop_triggers_downward() {
    // Buy stops fire when the price rises to them; sell stops when it falls.
    // A single-direction comparison passes one of these and fails the other.
    Matcher m;
    m.submit(limit(1, Side::Buy, P(1000), 100));
    m.submit(limit(2, Side::Sell, P(1000), 100));   // trade at 1000

    Result r = m.submit(stop(3, Side::Sell, Type::StopMarket, P(990), 0, 10));
    CHECK(r.state == State::Accepted);              // 1000 has not fallen to 990

    m.submit(limit(4, Side::Buy, P(985), 100));
    m.submit(limit(5, Side::Sell, P(985), 100));    // trade at 985 -> through it
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
    test_stop_limit_becomes_a_limit_at_its_own_price();
    test_stop_limit_takes_liquidity_when_its_price_allows();
    test_stop_market_firing_into_an_empty_book_is_cancelled_not_rejected();
    test_triggered_stop_takes_its_sequence_at_trigger_time();
    test_stop_needs_a_trigger_price();
    test_sell_stop_triggers_downward();
    test_cancel_removes_resting_shares();
    test_rejections();
    test_state_machine_rules();
    test_share_conservation_holds();
    return REPORT();
}

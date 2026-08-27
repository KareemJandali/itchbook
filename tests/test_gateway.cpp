//
// test_gateway.cpp — phase 12.5's gateway: validation, the risk layer, and
// flatten.
//
// Written against an adversarial design review that ran before gateway.hpp
// existed and found six blockers. Every test below names the finding it
// pins. The most important thing this file does NOT do is assert flatness by
// counting cancels: Matcher::cancel_meta() zeroes resting/hidden/in_book
// whether or not the underlying Book::remove() found anything, so a buggy
// flatten can emit N cancels and leave N orders resting and a
// count-the-cancels test would pass. Flatness is asserted against the BOOK,
// and independently by submitting a marketable order and requiring zero
// fills -- the one check that depends on neither layer's own bookkeeping.
//
#include <cstdint>
#include <cstring>
#include <vector>

#include "itchbook/book/book_set.hpp"
#include "itchbook/engine/gateway.hpp"
#include "itchbook/ouch/encode.hpp"
#include "itchbook/emit/sink.hpp"
#include "itchbook/replay/split.hpp"
#include "itchbook/soupbin/session.hpp"
#include "tests/check.hpp"

namespace {

namespace eng = itchbook::engine;
namespace ouch = itchbook::ouch;
namespace sb = itchbook::soupbin;
namespace bk = itchbook::book;
namespace rp = itchbook::replay;

constexpr uint64_t SEC = 1'000'000'000ULL;
constexpr int32_t PX = 1000000;      // $100.0000
constexpr uint16_t LOCATE = 7;
int32_t probe_ask_ = 0;

class ItchCapture : public itchbook::emit::Sink {
public:
    std::vector<std::vector<uint8_t>> msgs;
    void on_message(const uint8_t* p, size_t n) override { msgs.emplace_back(p, p + n); }
};

class VecSink : public sb::Sink {
public:
    std::vector<std::vector<uint8_t>> messages;
    void on_message(const uint8_t* p, size_t n) override { messages.emplace_back(p, p + n); }
    void clear() { messages.clear(); }
};

// A whole venue: one shared book, a borrowing matcher, a session, a gateway.
struct Venue {
    bk::BookSet set{1u << 14, 100, 20, 64};
    bk::Book& book = set.at(LOCATE);
    eng::Matcher matcher{book, 100};
    eng::RefSource refs;
    itchbook::risk::KillSwitch kill;
    VecSink wire;
    sb::ServerSession session{{}, 0, &wire, nullptr};
    eng::Gateway gw;

    explicit Venue(eng::Gateway::Config cfg = default_cfg(),
                   itchbook::risk::KillSwitchConfig kcfg = {})
        : kill(kcfg), gw(cfg, session, matcher, refs, kill) {
        // Drive the session to LoggedIn so the gateway's emissions go out.
        uint8_t lr[sb::kLoginRequestWireBytes];
        sb::encode::login_request(lr, "U", "P", "", "1");
        replayer.set_matcher(&matcher);
        session.on_bytes(lr, sizeof(lr), 0);
        gw.decide_login(true, "SESS1", "1", 0);
        wire.clear();
    }

    static eng::Gateway::Config default_cfg() {
        eng::Gateway::Config c;
        std::memcpy(c.stock, "TEST    ", 8);
        return c;
    }

    // Historical liquidity, applied straight to the shared book the way
    // phase 12.1's replayer does -- NOT through the matcher.
    void historical(uint64_t ref, char side, int32_t px, uint32_t sh) {
        book.add(ref, side, px, sh);
    }

    // A historical execution naming `ref`, replayed through the real
    // SplitReplayer so the aggressor path is the one under test.
    void aggress(uint64_t ref, uint32_t shares, uint64_t match) {
        std::vector<uint8_t> e(31, 0);
        e[0] = 'E';
        e[1] = uint8_t(LOCATE >> 8);
        e[2] = uint8_t(LOCATE);
        for (int i = 0; i < 8; ++i) e[11 + i] = uint8_t(ref >> (56 - 8 * i));
        e[19] = uint8_t(shares >> 24); e[20] = uint8_t(shares >> 16);
        e[21] = uint8_t(shares >> 8);  e[22] = uint8_t(shares);
        for (int i = 0; i < 8; ++i) e[23 + i] = uint8_t(match >> (56 - 8 * i));
        replayer.apply('E', e.data());
    }

    rp::SplitReplayer replayer{set};
};

std::vector<uint8_t> enter(const char* token, char side, uint32_t shares,
                            const char* stock, int32_t price) {
    std::vector<uint8_t> m(ouch::kEnterOrderLen);
    ouch::encode::enter_order(m.data(), token, side, shares, stock, price, 0,
                              "FIRM", 'Y', 'A', 'N', 0, 'N', ' ');
    return m;
}

std::vector<uint8_t> cancel_msg(const char* token, uint32_t shares) {
    std::vector<uint8_t> m(ouch::kCancelOrderLen);
    ouch::encode::cancel_order(m.data(), token, shares);
    return m;
}

std::vector<uint8_t> replace_msg(const char* existing, const char* fresh,
                                  uint32_t shares, int32_t price) {
    std::vector<uint8_t> m(ouch::kReplaceOrderLen);
    ouch::encode::replace_order(m.data(), existing, fresh, shares, price, 0,
                                'Y', 'N', 0);
    return m;
}

bool engine_terminal(eng::State s) { return eng::is_terminal(s); }

char last_type(const VecSink& w) {
    if (w.messages.empty()) return 0;
    // The wire carries SoupBinTCP frames; the OUCH message starts at byte 3.
    const auto& m = w.messages.back();
    return m.size() > 3 ? static_cast<char>(m[3]) : 0;
}

// Was a message of this type sent at all during the op? The right question
// when the claim is "the order was accepted" -- an Accepted is now legitimately
// followed by Executed messages when the order crosses, so asking for the LAST
// message conflates acceptance with not-having-traded.
bool saw_type(const VecSink& w, char t) {
    for (const auto& m : w.messages) {
        if (m.size() > 3 && static_cast<char>(m[3]) == t) return true;
    }
    return false;
}

char last_reject_reason(const VecSink& w) {
    if (w.messages.empty()) return 0;
    const auto& m = w.messages.back();
    return m.size() >= 3 + ouch::kRejectedLen ? ouch::rejected::reason(m.data() + 3) : 0;
}

// ---- 1. the one-book topology, end to end ------------------------------------
//
// The premise of docs/phase12-design.md section 4, and the reason the
// borrowing Matcher constructor was added in this sub-phase. A strategy order
// entering through the gateway must join the SAME queue as historical orders,
// behind them.
void test_strategy_order_joins_the_historical_queue() {
    Venue v;
    v.historical(1001, 'B', PX, 500);
    v.historical(1002, 'B', PX, 300);

    const auto m = enter("T1", 'B', 200, "TEST    ", PX);
    v.gw.on_ouch(m.data(), m.size(), SEC);

    CHECK_EQ(v.book.resting_orders(), size_t{3});
    CHECK_EQ(v.book.shares_at('B', PX), uint64_t{1000});

    // Third in the queue, behind both historical orders.
    const bk::Order* head = v.book.first_order('B', PX);
    CHECK(head != nullptr);
    if (head == nullptr) return;
    CHECK_EQ(head->ref, uint64_t{1001});
    CHECK(head->next != nullptr && head->next->ref == 1002);
    CHECK(head->next != nullptr && head->next->next != nullptr);
    if (head->next && head->next->next) {
        CHECK(itchbook::replay::is_strategy_ref(head->next->next->ref));
    }
}

// ---- 2. Request::id, the published reference, and the token map are ONE -----
//
// partition lens, blocker: if they differ, split.hpp's aggressor walk
// classifies the strategy order as historical and never fills it.
void test_reference_is_one_integer_in_the_strategy_half() {
    Venue v;
    const auto m = enter("T1", 'B', 100, "TEST    ", PX);
    v.gw.on_ouch(m.data(), m.size(), SEC);

    CHECK_EQ(v.wire.messages.size(), size_t{1});
    CHECK_EQ(last_type(v.wire), 'A');
    const uint8_t* acc = v.wire.messages.back().data() + 3;
    const uint64_t published = ouch::accepted::reference_number(acc);

    // Bit 63 set -- the partition 12.1 asserts on.
    CHECK(itchbook::replay::is_strategy_ref(published));
    // And the BOOK holds that same integer as the order's ref.
    const bk::Order* o = v.book.find(published);
    CHECK(o != nullptr);
    if (o != nullptr) CHECK_EQ(o->ref, published);
    // And the matcher keys it by the same value.
    CHECK(v.matcher.find(published) != nullptr);
}

// ---- 3. one venue-scoped RefSource, no collisions across gateways -----------
//
// partition lens, blocker: Matcher::orders_ is never erased, so a duplicate
// id is permanent -- every later order from the second gateway would be
// rejected DuplicateId forever.
void test_two_gateways_share_one_ref_source() {
    bk::BookSet set{1u << 14, 100, 20, 64};
    bk::Book& book = set.at(LOCATE);
    eng::Matcher matcher{book, 100};
    eng::RefSource refs;            // ONE source
    itchbook::risk::KillSwitch kill;

    VecSink w1, w2;
    sb::ServerSession s1{{}, 0, &w1, nullptr};
    sb::ServerSession s2{{}, 0, &w2, nullptr};
    eng::Gateway g1(Venue::default_cfg(), s1, matcher, refs, kill);
    eng::Gateway g2(Venue::default_cfg(), s2, matcher, refs, kill);

    uint8_t lr[sb::kLoginRequestWireBytes];
    sb::encode::login_request(lr, "U", "P", "", "1");
    s1.on_bytes(lr, sizeof(lr), 0);
    g1.decide_login(true, "S", "1", 0);
    s2.on_bytes(lr, sizeof(lr), 0);
    g2.decide_login(true, "S", "1", 0);
    w1.clear();
    w2.clear();

    const auto m = enter("T1", 'B', 100, "TEST    ", PX);
    g1.on_ouch(m.data(), m.size(), SEC);
    g2.on_ouch(m.data(), m.size(), SEC);   // same TOKEN, different gateway

    // Both accepted -- tokens are per-session, references are per-venue.
    CHECK_EQ(last_type(w1), 'A');
    CHECK_EQ(last_type(w2), 'A');
    const uint64_t r1 = ouch::accepted::reference_number(w1.messages.back().data() + 3);
    const uint64_t r2 = ouch::accepted::reference_number(w2.messages.back().data() + 3);
    CHECK(r1 != r2);                       // no collision
    CHECK_EQ(book.resting_orders(), size_t{2});

    // Inequality alone is too weak -- two independent per-gateway counters
    // would also produce different values here, because both gateways happen
    // to allocate in the same order. The claim that actually matters is that
    // ONE source issued both, so the second gateway's reference is the first
    // one's successor and the shared counter has advanced twice.
    CHECK_EQ(refs.issued(), uint64_t{2});
    CHECK_EQ(r2, r1 + 1);

    // The sharpest form: a gateway created LATER must not reissue a reference
    // an earlier gateway already used -- which is the permanent, unrecoverable
    // collision, since Matcher::orders_ is never erased.
    VecSink w3;
    sb::ServerSession s3{{}, 0, &w3, nullptr};
    eng::Gateway g3(Venue::default_cfg(), s3, matcher, refs, kill);
    s3.on_bytes(lr, sizeof(lr), 0);
    g3.decide_login(true, "S", "1", 0);
    w3.clear();
    g3.on_ouch(m.data(), m.size(), SEC);
    CHECK_EQ(last_type(w3), 'A');          // not DuplicateId
    const uint64_t r3 = ouch::accepted::reference_number(w3.messages.back().data() + 3);
    CHECK(r3 != r1 && r3 != r2);
    CHECK_EQ(book.resting_orders(), size_t{3});
}

// ---- 4. well-formedness before any field read -------------------------------
//
// validation lens, blocker: ServerSession delivers Unsequenced Data payloads
// of ANY length including zero, so the first field access would be an
// out-of-bounds read.
void test_malformed_ouch_is_refused_before_any_field_read() {
    Venue v;
    CHECK(!v.gw.on_ouch(nullptr, 0, SEC));            // zero-length
    const uint8_t stub[1] = {'O'};
    CHECK(!v.gw.on_ouch(stub, 1, SEC));               // right type, wrong length
    const uint8_t unknown[4] = {'?', 0, 0, 0};
    CHECK(!v.gw.on_ouch(unknown, 4, SEC));            // unknown type
    std::vector<uint8_t> short_enter(ouch::kEnterOrderLen - 1, 'O');
    short_enter[0] = 'O';
    CHECK(!v.gw.on_ouch(short_enter.data(), short_enter.size(), SEC));
    // None of these produced any wire traffic: a malformed message has no
    // token to echo, so J is not an available answer.
    CHECK_EQ(v.wire.messages.size(), size_t{0});
}

// ---- 5. symbol and side are validated, never defaulted ----------------------
void test_wrong_symbol_and_bad_side_rejected() {
    Venue v;
    const auto wrong = enter("T1", 'B', 100, "MSFT    ", PX);
    v.gw.on_ouch(wrong.data(), wrong.size(), SEC);
    CHECK_EQ(last_type(v.wire), 'J');
    CHECK_EQ(last_reject_reason(v.wire), ouch::reject_reason::kInvalidStock);
    CHECK_EQ(v.book.resting_orders(), size_t{0});

    const auto bad_side = enter("T2", 'Z', 100, "TEST    ", PX);
    v.gw.on_ouch(bad_side.data(), bad_side.size(), SEC);
    CHECK_EQ(last_type(v.wire), 'J');
    CHECK_EQ(v.book.resting_orders(), size_t{0});   // never silently a Sell
}

// ---- 6. the market-order threshold, not just the sentinel -------------------
//
// validation lens, major: the spec's rule is price >= kMarketOrderThreshold.
// $200,000.00 exactly is a market order, not an invalid price.
void test_market_order_threshold_not_only_the_sentinel() {
    Venue v;
    v.historical(2001, 'S', PX, 100);   // something to trade against

    const auto at_threshold = enter("T1", 'B', 100, "TEST    ",
                                     ouch::kMarketOrderThreshold);
    v.wire.clear();
    v.gw.on_ouch(at_threshold.data(), at_threshold.size(), SEC);
    CHECK(saw_type(v.wire, 'A'));    // accepted as a MARKET order, not J
    CHECK(!saw_type(v.wire, 'J'));
    // And it genuinely traded against the historical offer, which is only
    // possible because 12.6 taught match() to fill against a maker it does
    // not own.
    CHECK(saw_type(v.wire, 'E'));

    Venue v2;
    const auto above_max = enter("T1", 'B', 100, "TEST    ", ouch::kMaxPrice + 1);
    v2.gw.on_ouch(above_max.data(), above_max.size(), SEC);
    CHECK_EQ(last_type(v2.wire), 'J');   // above max but below threshold: invalid
    CHECK_EQ(last_reject_reason(v2.wire), ouch::reject_reason::kInvalidPrice);
}

// ---- 7. the collar measures the SHARED book ---------------------------------
//
// validation lens, major: with a private matcher book the collar would see
// only the strategy's own orders and never fire.
void test_collar_measured_against_historical_liquidity() {
    eng::Gateway::Config cfg = Venue::default_cfg();
    cfg.collar_ticks = 10;              // 10 ticks = $0.10
    Venue v(cfg);
    v.historical(2001, 'S', PX, 100);   // best ask $100.0000, historical only

    // A NON-marketable bid: below the ask, so it rests without consuming the
    // liquidity the collar is measured against. (Pricing it above the ask
    // would cross -- which the engine now does correctly, since 12.6 taught
    // match() to trade with makers it does not own -- and the ask would be
    // gone before the collar was ever tested. The first version of this test
    // did exactly that and passed only because the engine could not trade.)
    const auto ok = enter("T1", 'B', 100, "TEST    ", PX - 500);
    v.gw.on_ouch(ok.data(), ok.size(), SEC);
    CHECK_EQ(last_type(v.wire), 'A');
    CHECK(v.book.best_ask(&probe_ask_) && probe_ask_ == PX);   // still there

    // $101.00 is 100 ticks through the ask -- far outside the 10-tick collar.
    const auto bad = enter("T2", 'B', 100, "TEST    ", PX + 10000);
    v.gw.on_ouch(bad.data(), bad.size(), SEC);
    CHECK_EQ(last_type(v.wire), 'J');
    CHECK_EQ(last_reject_reason(v.wire), ouch::reject_reason::kRiskFatFinger);

    // And the collar does not fire on a price just inside it, so the test is
    // not passing merely because everything is rejected.
    const auto inside = enter("T3", 'B', 100, "TEST    ", PX + 900);
    v.wire.clear();
    v.gw.on_ouch(inside.data(), inside.size(), SEC);
    CHECK(saw_type(v.wire, 'A'));
    CHECK(!saw_type(v.wire, 'J'));
}

// ---- 8. the kill switch gates on INTENT, not message type -------------------
//
// validation lens, blocker: a tripped switch must stop Replace too, or a
// Replace re-arms flow the flatten just removed. Cancel must stay open.
void test_kill_switch_gates_enter_and_replace_but_never_cancel() {
    itchbook::risk::KillSwitchConfig kcfg;
    kcfg.max_position = 1000;
    Venue v(Venue::default_cfg(), kcfg);
    const auto e1 = enter("T1", 'B', 100, "TEST    ", PX);
    v.gw.on_ouch(e1.data(), e1.size(), SEC);
    CHECK_EQ(last_type(v.wire), 'A');
    CHECK_EQ(v.book.resting_orders(), size_t{1});

    // Trip it the way it is actually meant to trip: a real position breach,
    // reported post-trade. There is no manual trip -- trip() is private, and
    // kill_switch.hpp's whole argument is that this is a POST-trade control.
    v.kill.on_fill(2 * SEC, 10000, 0, PX);
    CHECK(!v.kill.live());

    const auto e2 = enter("T2", 'B', 100, "TEST    ", PX);
    v.gw.on_ouch(e2.data(), e2.size(), 3 * SEC);
    CHECK_EQ(last_type(v.wire), 'J');   // Enter blocked

    const auto rp = replace_msg("T1", "T3", 200, PX);
    v.gw.on_ouch(rp.data(), rp.size(), 4 * SEC);
    CHECK_EQ(last_type(v.wire), 'J');   // Replace blocked too
    CHECK_EQ(v.book.resting_orders(), size_t{1});   // original untouched

    // Cancel is never gated: it is the only client action that reduces risk.
    const auto cx = cancel_msg("T1", 0);
    v.gw.on_ouch(cx.data(), cx.size(), 5 * SEC);
    CHECK_EQ(last_type(v.wire), 'C');
    CHECK_EQ(v.book.resting_orders(), size_t{0});
}

// ---- 9. Replace is atomic: a rejected replacement leaves the original alive -
//
// partition lens, blocker: cancel-then-validate leaves NEITHER token live,
// permanently, because cancel_meta() is irreversible.
void test_rejected_replacement_leaves_the_original_resting() {
    eng::Gateway::Config cfg = Venue::default_cfg();
    cfg.collar_ticks = 10;
    Venue v(cfg);
    v.historical(2001, 'S', PX, 100);

    const auto e1 = enter("T1", 'B', 100, "TEST    ", PX - 100);
    v.gw.on_ouch(e1.data(), e1.size(), SEC);
    CHECK_EQ(last_type(v.wire), 'A');
    const uint64_t ref = ouch::accepted::reference_number(v.wire.messages.back().data() + 3);
    CHECK_EQ(v.book.resting_orders(), size_t{2});   // historical + ours

    // A replacement far through the collar must be refused -- and must leave
    // the ORIGINAL exactly where it was.
    const auto rp = replace_msg("T1", "T2", 100, PX + 10000);
    v.gw.on_ouch(rp.data(), rp.size(), 2 * SEC);
    CHECK_EQ(last_type(v.wire), 'J');
    CHECK_EQ(v.book.resting_orders(), size_t{2});
    CHECK(v.book.find(ref) != nullptr);            // still there
    CHECK_EQ(v.gw.live_orders(), uint64_t{1});

    // And the original token is still usable afterwards.
    const auto cx = cancel_msg("T1", 0);
    v.gw.on_ouch(cx.data(), cx.size(), 3 * SEC);
    CHECK_EQ(last_type(v.wire), 'C');
    CHECK(v.book.find(ref) == nullptr);
}

// ---- 10. flatten on session death, through EVERY terminal path ---------------
//
// integration lens, blocker: tick() can only reach Dead and LoginTimedOut.
// Ended and ProtocolViolation are set inside on_bytes(). A design keyed on
// tick()'s return value would never flatten the two most ordinary deaths.
void check_flatten_for_death(const char* what, int how) {
    Venue v;
    v.historical(2001, 'S', PX + 10000, 100);   // far away; nothing crosses
    for (int i = 0; i < 3; ++i) {
        char tok[15];
        std::snprintf(tok, sizeof(tok), "T%d", i);
        const auto m = enter(tok, 'B', 100, "TEST    ", PX);
        v.gw.on_ouch(m.data(), m.size(), SEC);
    }
    CHECK_EQ(v.book.resting_orders(), size_t{4});   // 3 ours + 1 historical
    CHECK_EQ(v.gw.live_orders(), uint64_t{3});

    if (how == 0) {                       // Dead: silence past the timeout
        v.session.tick(60 * SEC);
        CHECK(v.session.state() == sb::State::Dead);
    } else if (how == 1) {                // Ended: the client logs out politely
        uint8_t lo[sb::kLogoutRequestWireBytes];
        sb::encode::logout_request(lo);
        v.session.on_bytes(lo, sizeof(lo), 2 * SEC);
        CHECK(v.session.state() == sb::State::Ended);
    } else {                              // ProtocolViolation: a bad frame
        const uint8_t bad[3] = {0, 1, '?'};
        v.session.on_bytes(bad, sizeof(bad), 2 * SEC);
        CHECK(v.session.state() == sb::State::ProtocolViolation);
    }

    const bool did = v.gw.poll(61 * SEC);
    CHECK(did);
    CHECK(v.gw.flattened());

    // Assert the BOOK, not the bookkeeping: only the historical order is left.
    CHECK_EQ(v.book.resting_orders(), size_t{1});
    CHECK_EQ(v.gw.live_orders(), uint64_t{0});
    const bk::Order* left = v.book.first_order('S', PX + 10000);
    CHECK(left != nullptr && left->ref == 2001);
    (void)what;

    // Exactly once: a second poll does nothing.
    CHECK(!v.gw.poll(62 * SEC));
}

void test_flatten_on_every_terminal_path() {
    check_flatten_for_death("Dead", 0);
    check_flatten_for_death("Ended", 1);
    check_flatten_for_death("ProtocolViolation", 2);
}

// ---- 11. flatness proven independently of both layers' bookkeeping ----------
//
// flatten lens, major: cancel_meta() zeroes resting/hidden/in_book whether or
// not Book::remove() found anything, so every Meta-side assertion after a
// flatten is self-confirming. This one is not: submit a marketable order and
// require zero fills.
void test_flatten_leaves_nothing_to_trade_against() {
    Venue v;
    for (int i = 0; i < 4; ++i) {
        char tok[15];
        std::snprintf(tok, sizeof(tok), "B%d", i);
        const auto m = enter(tok, 'B', 100, "TEST    ", PX);
        v.gw.on_ouch(m.data(), m.size(), SEC);
    }
    CHECK_EQ(v.book.shares_at('B', PX), uint64_t{400});

    v.session.tick(60 * SEC);
    v.gw.poll(61 * SEC);

    CHECK_EQ(v.book.resting_orders(), size_t{0});
    CHECK_EQ(v.book.resting_shares(), uint64_t{0});
    CHECK(v.matcher.conserves_shares());

    // The independent check: a sell that would have crossed all four bids
    // must now fill nothing.
    eng::Request probe;
    probe.id = 999;
    probe.side = eng::Side::Sell;
    probe.type = eng::Type::Limit;
    probe.price = PX;
    probe.quantity = 1000;
    const eng::Result r = v.matcher.submit(probe);
    CHECK_EQ(r.filled, uint32_t{0});
}

// ---- 12. a parked stop order does not survive the flatten -------------------
//
// partition lens, major: a stop awaiting its trigger has resting == 0 and
// in_book == false. A flatten filtering on resting shares leaves the book
// flat, passes a count-the-cancels test, and leaves the account armed.
void test_parked_stop_is_cancelled_by_flatten() {
    Venue v;
    // Submit a stop directly -- OUCH 4.2's core subset has no stop type, so
    // this models any order the matcher holds as live-but-not-resting.
    eng::Request stop;
    stop.id = itchbook::replay::kStrategyRefBit | 5000;
    stop.side = eng::Side::Buy;
    stop.type = eng::Type::StopLimit;
    stop.price = PX;
    stop.stop_price = PX + 500;
    stop.quantity = 100;
    const eng::Result sres = v.matcher.submit(stop);
    CHECK(sres.accepted());
    const eng::Matcher::Meta* meta = v.matcher.find(stop.id);
    CHECK(meta != nullptr);
    if (meta != nullptr) {
        CHECK_EQ(meta->resting, uint32_t{0});     // parked: nothing resting
        CHECK(!engine_terminal(meta->state));      // but still live
    }

    // Register it with the gateway the way an accepted order would be, so
    // flatten() sees it in its live set. (OUCH 4.2's core subset has no stop
    // type, so this is the one place a test reaches past the wire format to
    // model an order the matcher holds as live-but-not-resting.)
    v.gw.adopt_for_test(stop.id);
    CHECK_EQ(v.gw.live_orders(), uint64_t{1});

    // Now kill the session and let the GATEWAY flatten -- this is the code
    // path under test, not Matcher::cancel().
    v.session.tick(60 * SEC);
    CHECK(v.gw.poll(61 * SEC));

    // The parked stop must be terminal. A flatten filtering on resting shares
    // would have skipped it: resting == 0 the whole time.
    const eng::Matcher::Meta* after = v.matcher.find(stop.id);
    CHECK(after != nullptr);
    if (after != nullptr) {
        CHECK(engine_terminal(after->state));
        CHECK_EQ(after->resting, uint32_t{0});
    }
    CHECK_EQ(v.gw.live_orders(), uint64_t{0});

    // And it can no longer be triggered into life: drive a trade through the
    // stop price and require nothing new rests.
    const size_t before = v.book.resting_orders();
    eng::Request drive;
    drive.id = itchbook::replay::kStrategyRefBit | 6000;
    drive.side = eng::Side::Sell;
    drive.type = eng::Type::Limit;
    drive.price = PX + 1000;
    drive.quantity = 100;
    v.matcher.submit(drive);
    CHECK_EQ(v.book.resting_orders(), before + 1);   // only the driver itself
}

// ---- 13. flatten's cancels never touch the kill switch's rate limit ---------
//
// flatten lens, blocker: remediation traffic is not strategy traffic. A
// flatten of many orders, all stamped with one instant, would otherwise trip
// the message-rate limit with the risk layer's own remediation.
void test_flatten_does_not_feed_the_rate_limit() {
    itchbook::risk::KillSwitchConfig kcfg;
    kcfg.max_messages_per_second = 4;   // deliberately tiny
    Venue v(Venue::default_cfg(), kcfg);

    for (int i = 0; i < 8; ++i) {
        char tok[15];
        std::snprintf(tok, sizeof(tok), "T%d", i);
        const auto m = enter(tok, 'B', 100, "TEST    ", PX);
        v.gw.on_ouch(m.data(), m.size(), SEC);
    }
    CHECK_EQ(v.gw.live_orders(), uint64_t{8});
    CHECK(v.kill.live());   // the gateway never fed on_message_sent()

    v.session.tick(60 * SEC);
    v.gw.poll(61 * SEC);

    CHECK_EQ(v.book.resting_orders(), size_t{0});
    // Eight cancels in one instant, and the switch is still live: the flatten
    // did not trip the control that was supposed to be stopping the strategy.
    CHECK(v.kill.live());
}

// ---- 14. the two clocks are genuinely separate -------------------------------
//
// docs/phase12-design.md section 6 requires two independent time bases. One
// member served both the ITCH tape (replay time, and a 48-bit wire field) and
// the SoupBinTCP session's heartbeat bookkeeping (wall clock), so whichever
// was passed, one of them was silently wrong.
void test_two_clocks_are_separate() {
    Venue v;
    // Replay time: a plausible mid-session nanoseconds-past-midnight.
    const uint64_t replay = 34200ULL * 1000000000ULL;   // 09:30:00
    v.gw.set_replay_now(replay);
    CHECK_EQ(v.gw.replay_now(), replay);

    ItchCapture itch;
    v.gw.set_itch_sink(&itch);
    const auto m = enter("T1", 'B', 100, "TEST    ", PX);
    // A wall clock far larger than any 48-bit value -- which is what a real
    // wall clock looks like, and what would have overflowed the tape.
    v.gw.on_ouch(m.data(), m.size(), 1787000000000000000ULL);

    CHECK(!itch.msgs.empty());
    if (!itch.msgs.empty()) {
        // The published timestamp is the REPLAY clock, and fits the field.
        const uint8_t* p = itch.msgs.back().data();
        uint64_t ts = 0;
        for (int i = 0; i < 6; ++i) ts = (ts << 8) | p[5 + i];
        CHECK_EQ(ts, replay);
        CHECK(ts < (uint64_t{1} << 48));
    }
}

// ---- 15. a passive fill moves the position ------------------------------------
//
// The replayer's aggressor hitting a resting strategy order produces a Fill
// whose taker is 0. Resolving the TAKER for both sides made find(0) return
// null, the delta zero, and the position never move -- for exactly the fill
// class 12.7 exists to demonstrate.
void test_passive_fill_moves_position() {
    Venue v;
    v.gw.set_replay_now(34200ULL * 1000000000ULL);

    // Our order rests first, so it is ahead in the queue.
    const auto m = enter("T1", 'B', 300, "TEST    ", PX);
    v.gw.on_ouch(m.data(), m.size(), SEC);
    CHECK_EQ(v.gw.position(), int64_t{0});          // nothing traded yet
    const uint64_t ref = ouch::accepted::reference_number(v.wire.messages.back().data() + 3);

    // A historical order joins behind ours at the same price.
    v.historical(5000, 'B', PX, 100);

    // The feed executes the historical order; per 12.1 the aggressor takes the
    // strategy shares resting ahead of it first.
    v.aggress(5000, 200, 77);

    v.gw.pump_fills(2 * SEC);
    CHECK_EQ(v.gw.position(), int64_t{200});        // we bought 200
    const eng::Matcher::Meta* meta = v.matcher.find(ref);
    CHECK(meta != nullptr);
    if (meta != nullptr) CHECK_EQ(meta->resting, uint32_t{100});
    CHECK(v.matcher.agrees_with_book());
}

// ---- 16. the two streams join on a passive fill --------------------------------
//
// The OUCH Executed and the ITCH 'E' for the same passive fill must carry the
// same match number, or 12.6's differential cannot pair them. Before the fix
// the passive fill inherited whatever match number the last order entry left
// behind.
void test_passive_fill_join_key() {
    Venue v;
    v.gw.set_replay_now(34200ULL * 1000000000ULL);
    const auto m = enter("T1", 'B', 300, "TEST    ", PX);
    v.gw.on_ouch(m.data(), m.size(), SEC);
    v.historical(5000, 'B', PX, 100);
    v.wire.clear();

    constexpr uint64_t kFeedMatch = 987654321ULL;
    v.aggress(5000, 200, kFeedMatch);
    v.gw.pump_fills(2 * SEC);

    // The OUCH Executed the client received carries the FEED's match number.
    bool found = false;
    for (const auto& msg : v.wire.messages) {
        if (msg.size() > 3 && static_cast<char>(msg[3]) == 'E') {
            CHECK_EQ(ouch::executed::match_number(msg.data() + 3), kFeedMatch);
            found = true;
        }
    }
    CHECK(found);
}

}  // namespace

int main() {
    test_strategy_order_joins_the_historical_queue();
    test_reference_is_one_integer_in_the_strategy_half();
    test_two_gateways_share_one_ref_source();
    test_malformed_ouch_is_refused_before_any_field_read();
    test_wrong_symbol_and_bad_side_rejected();
    test_market_order_threshold_not_only_the_sentinel();
    test_collar_measured_against_historical_liquidity();
    test_kill_switch_gates_enter_and_replace_but_never_cancel();
    test_rejected_replacement_leaves_the_original_resting();
    test_flatten_on_every_terminal_path();
    test_flatten_leaves_nothing_to_trade_against();
    test_parked_stop_is_cancelled_by_flatten();
    test_flatten_does_not_feed_the_rate_limit();
    test_two_clocks_are_separate();
    test_passive_fill_moves_position();
    test_passive_fill_join_key();
    return REPORT();
}

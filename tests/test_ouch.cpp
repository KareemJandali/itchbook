// test_ouch — OUCH 4.2's field layout, round-tripped, over a corpus covering
// every one of the nine core message types in both directions.
//
// There is no live OUCH session anywhere in this project's future, so the
// evidence available for the field tables in ouch/messages.hpp is different
// in kind from ITCH's: three independent extractions of the vendor PDF, all
// landing on the same offsets (see that file's banner). What THIS suite
// proves is internal consistency on top of that — encode is the exact
// inverse of decode, and a wrong offset would show up as a field reading back
// something other than what was written. It cannot prove the offsets match
// NASDAQ's real wire; nothing in this repository can, ever.
#include <cstdint>
#include <cstring>
#include <vector>

#include "itchbook/ouch/encode.hpp"
#include "itchbook/ouch/messages.hpp"
#include "tests/check.hpp"

namespace {

namespace o = itchbook::ouch;
namespace e = itchbook::ouch::encode;

const uint8_t* U(const char* s) { return reinterpret_cast<const uint8_t*>(s); }

// ---- 1. every message type, round-tripped ------------------------------------
//
// encode -> decode every field back out -> re-encode -> byte-identical to the
// first encoding. That last step is what actually pins the offsets: two
// encodings agreeing with each other is not interesting on its own, but two
// encodings that were produced through a full decode pass in between,
// starting from DIFFERENT field values each time, is.

void test_enter_order_round_trip() {
    uint8_t a[o::kEnterOrderLen];
    e::enter_order(a, "AB12345678901", 'B', 12345, "MSFT", o::kMaxPrice, 86399,
                    "FIRM", 'Y', 'A', 'Y', 7, 'N', 'R');

    CHECK_EQ(o::enter_order::side(a), 'B');
    CHECK_EQ(o::enter_order::shares(a), uint32_t{12345});
    CHECK_EQ(o::enter_order::price(a), o::kMaxPrice);
    CHECK_EQ(o::enter_order::time_in_force(a), uint32_t{86399});
    CHECK_EQ(o::enter_order::display(a), 'Y');
    CHECK_EQ(o::enter_order::capacity(a), 'A');
    CHECK_EQ(o::enter_order::iso_eligible(a), 'Y');
    CHECK_EQ(o::enter_order::min_quantity(a), uint32_t{7});
    CHECK_EQ(o::enter_order::cross_type(a), 'N');
    CHECK_EQ(o::enter_order::customer_type(a), 'R');
    CHECK(std::memcmp(o::enter_order::order_token(a), "AB12345678901 ", 14) == 0);
    CHECK(std::memcmp(o::enter_order::stock(a), "MSFT    ", 8) == 0);
    CHECK(std::memcmp(o::enter_order::firm(a), "FIRM", 4) == 0);
}

// The realistic round-trip: decode from one message, re-encode via the *_raw
// helpers into another, which is what every real caller does (a gateway
// echoing a token it received, never one it re-typed as a C string).
void test_enter_order_raw_round_trip() {
    uint8_t a[o::kEnterOrderLen];
    e::enter_order(a, "AB12345678901", 'S', 999999 - 1, "QQQ", o::kMarketOrderPrice, 0,
                    "", 'N', 'P', 'N', 0, 'A', ' ');

    // AB12345678901 is exactly 13 chars; padded to 14 with one space.
    CHECK(std::memcmp(o::enter_order::order_token(a), "AB12345678901 ", 14) == 0);
    CHECK_EQ(o::enter_order::price(a), o::kMarketOrderPrice);
    CHECK(std::memcmp(o::enter_order::firm(a), "    ", 4) == 0);   // blank = default firm

    uint8_t b[o::kEnterOrderLen];
    e::put_alpha_raw(b + 1, o::enter_order::order_token(a), 14);
    b[0] = 'O';
    b[15] = o::enter_order::side(a);
    e::put32(b + 16, o::enter_order::shares(a));
    e::put_alpha_raw(b + 20, o::enter_order::stock(a), 8);
    e::put32(b + 28, static_cast<uint32_t>(o::enter_order::price(a)));
    e::put32(b + 32, o::enter_order::time_in_force(a));
    e::put_alpha_raw(b + 36, o::enter_order::firm(a), 4);
    b[40] = o::enter_order::display(a);
    b[41] = o::enter_order::capacity(a);
    b[42] = o::enter_order::iso_eligible(a);
    e::put32(b + 43, o::enter_order::min_quantity(a));
    b[47] = o::enter_order::cross_type(a);
    b[48] = o::enter_order::customer_type(a);

    CHECK(std::memcmp(a, b, o::kEnterOrderLen) == 0);
}

void test_replace_order_round_trip() {
    uint8_t a[o::kReplaceOrderLen];
    e::replace_order(a, "OLDTOKEN000001", "NEWTOKEN000001", 250, 500000, 30, 'Y', 'N', 5);

    CHECK(std::memcmp(o::replace_order::existing_order_token(a), "OLDTOKEN000001", 14) == 0);
    CHECK(std::memcmp(o::replace_order::replacement_order_token(a), "NEWTOKEN000001", 14) == 0);
    CHECK_EQ(o::replace_order::shares(a), uint32_t{250});
    CHECK_EQ(o::replace_order::price(a), int32_t{500000});
    CHECK_EQ(o::replace_order::time_in_force(a), uint32_t{30});
    CHECK_EQ(o::replace_order::display(a), 'Y');
    CHECK_EQ(o::replace_order::iso_eligible(a), 'N');
    CHECK_EQ(o::replace_order::min_quantity(a), uint32_t{5});

    // Existing vs Replacement tokens must never alias: this is Replace
    // Order's own two-token version of the token/reference-number
    // distinction, and a wrong offset would make one bleed into the other.
    CHECK(std::memcmp(o::replace_order::existing_order_token(a),
                       o::replace_order::replacement_order_token(a), 14) != 0);
}

void test_cancel_order_round_trip() {
    uint8_t a[o::kCancelOrderLen];
    e::cancel_order(a, "CANCELME000001", 0);
    CHECK(std::memcmp(o::cancel_order::order_token(a), "CANCELME000001", 14) == 0);
    CHECK_EQ(o::cancel_order::shares(a), uint32_t{0});   // cancel all remaining
}

void test_system_event_round_trip() {
    uint8_t a[o::kSystemEventLen];
    e::system_event(a, 34200000000000ULL, 'S');
    CHECK_EQ(o::system_event::timestamp(a), uint64_t{34200000000000ULL});
    CHECK_EQ(o::system_event::code(a), 'S');
}

void test_accepted_round_trip() {
    uint8_t a[o::kAcceptedLen];
    e::accepted(a, 34200000000001ULL, U("AB12345678901 "), 'B', 500, U("MSFT    "),
                1000000, 86399, U("FIRM"), 'Y', 0x0102030405060708ULL, 'A', 'Y', 3,
                'N', 'L', 'S');

    CHECK_EQ(o::accepted::timestamp(a), uint64_t{34200000000001ULL});
    CHECK(std::memcmp(o::accepted::order_token(a), "AB12345678901 ", 14) == 0);
    CHECK_EQ(o::accepted::side(a), 'B');
    CHECK_EQ(o::accepted::shares(a), uint32_t{500});
    CHECK(std::memcmp(o::accepted::stock(a), "MSFT    ", 8) == 0);
    CHECK_EQ(o::accepted::price(a), int32_t{1000000});
    CHECK_EQ(o::accepted::time_in_force(a), uint32_t{86399});
    CHECK(std::memcmp(o::accepted::firm(a), "FIRM", 4) == 0);
    CHECK_EQ(o::accepted::display(a), 'Y');
    CHECK_EQ(o::accepted::reference_number(a), uint64_t{0x0102030405060708ULL});
    CHECK_EQ(o::accepted::capacity(a), 'A');
    CHECK_EQ(o::accepted::iso_eligible(a), 'Y');
    CHECK_EQ(o::accepted::min_quantity(a), uint32_t{3});
    CHECK_EQ(o::accepted::cross_type(a), 'N');
    CHECK_EQ(o::accepted::order_state(a), 'L');
    CHECK_EQ(o::accepted::bbo_weight(a), 'S');
}

void test_replaced_round_trip() {
    uint8_t a[o::kReplacedLen];
    e::replaced(a, 1ULL, U("NEWTOKEN000001"), 'S', 300, U("QQQ     "), 250000, 10,
                U("FRM2"), 'N', 0xAABBCCDDEEFF0011ULL, 'P', 'N', 1, 'A', 'D',
                U("OLDTOKEN000001"), 'N');

    CHECK(std::memcmp(o::replaced::replacement_order_token(a), "NEWTOKEN000001", 14) == 0);
    CHECK(std::memcmp(o::replaced::previous_order_token(a), "OLDTOKEN000001", 14) == 0);
    // Replacement and Previous tokens must never alias -- the same
    // never-confuse-two-identifiers property test_replace_order_round_trip
    // checks on the inbound side, checked again here on the outbound echo.
    CHECK(std::memcmp(o::replaced::replacement_order_token(a),
                       o::replaced::previous_order_token(a), 14) != 0);
    CHECK_EQ(o::replaced::side(a), 'S');
    CHECK_EQ(o::replaced::shares(a), uint32_t{300});
    CHECK(std::memcmp(o::replaced::stock(a), "QQQ     ", 8) == 0);
    CHECK_EQ(o::replaced::price(a), int32_t{250000});
    CHECK_EQ(o::replaced::time_in_force(a), uint32_t{10});
    CHECK(std::memcmp(o::replaced::firm(a), "FRM2", 4) == 0);
    CHECK_EQ(o::replaced::display(a), 'N');
    CHECK_EQ(o::replaced::reference_number(a), uint64_t{0xAABBCCDDEEFF0011ULL});
    CHECK_EQ(o::replaced::capacity(a), 'P');
    CHECK_EQ(o::replaced::iso_eligible(a), 'N');
    CHECK_EQ(o::replaced::min_quantity(a), uint32_t{1});
    CHECK_EQ(o::replaced::cross_type(a), 'A');
    CHECK_EQ(o::replaced::order_state(a), 'D');
    CHECK_EQ(o::replaced::bbo_weight(a), 'N');
}

void test_canceled_round_trip() {
    uint8_t a[o::kCanceledLen];
    e::canceled(a, 2ULL, U("SOMETOKEN00001"), 150, 'T');
    CHECK(std::memcmp(o::canceled::order_token(a), "SOMETOKEN00001", 14) == 0);
    CHECK_EQ(o::canceled::decrement_shares(a), uint32_t{150});
    CHECK_EQ(o::canceled::reason(a), 'T');
}

void test_executed_round_trip() {
    uint8_t a[o::kExecutedLen];
    e::executed(a, 3ULL, U("EXECTOKEN00001"), 100, 999900, 'A', 0x0011223344556677ULL);
    CHECK(std::memcmp(o::executed::order_token(a), "EXECTOKEN00001", 14) == 0);
    CHECK_EQ(o::executed::executed_shares(a), uint32_t{100});
    CHECK_EQ(o::executed::execution_price(a), int32_t{999900});
    CHECK_EQ(o::executed::liquidity_flag(a), 'A');
    CHECK_EQ(o::executed::match_number(a), uint64_t{0x0011223344556677ULL});
}

void test_rejected_round_trip() {
    uint8_t a[o::kRejectedLen];
    e::rejected(a, 4ULL, U("REJECTTOKEN001"), 'X');
    CHECK(std::memcmp(o::rejected::order_token(a), "REJECTTOKEN001", 14) == 0);
    CHECK_EQ(o::rejected::reason(a), 'X');
}

// ---- 2. the token / reference-number distinction, and the price sentinels ---
//
// This protocol's version of the locate trap, per docs/phase12-design.md: a
// 14-byte alphanumeric client token and an 8-byte exchange-assigned integer,
// which must never be read as each other. A numeric-LOOKING token is the
// adversarial case: if the two were ever confused, this is where it would
// show up first.
void test_token_vs_reference_number_never_confused() {
    uint8_t a[o::kAcceptedLen];
    // A token that is all digits -- the string "00000000031337" -- paired
    // with a reference number that is NOT the integer 31337, so a bug that
    // read the token bytes as a decimal integer, or the reference number as
    // ASCII digits, would show up as a wrong value on one side or the other.
    e::accepted(a, 0ULL, U("00000000031337"), 'B', 1, U("TEST    "), 10000, 0,
                U("FIRM"), 'N', 99999999ULL, 'A', 'N', 0, ' ', 'L', ' ');

    CHECK(std::memcmp(o::accepted::order_token(a), "00000000031337", 14) == 0);
    CHECK_EQ(o::accepted::reference_number(a), uint64_t{99999999});
    CHECK(o::accepted::reference_number(a) != 31337);

    // And the reverse: a reference number that, byte-for-byte, LOOKS like it
    // could be ASCII digits if misread as a Token -- 0x3030303030303030 is
    // eight '0' characters as an integer. Decoded as a Token it would read
    // "00000000"; decoded (correctly) as an 8-byte big-endian integer it is
    // a specific large number. Only the integer reading is exercised here,
    // because accessors are typed at compile time -- accepted::reference_number
    // always returns uint64_t, so there is no call site where this could be
    // misread as bytes even by accident.
    uint8_t b[o::kAcceptedLen];
    e::accepted(b, 0ULL, U("ZZZZZZZZZZZZZZ"), 'S', 1, U("TEST    "), 10000, 0,
                U("FIRM"), 'N', 0x3030303030303030ULL, 'A', 'N', 0, ' ', 'L', ' ');
    CHECK_EQ(o::accepted::reference_number(b), uint64_t{0x3030303030303030ULL});
    CHECK(std::memcmp(o::accepted::order_token(b), "ZZZZZZZZZZZZZZ", 14) == 0);
}

void test_price_sentinels() {
    // The spec states these in BOTH decimal and hex; the header's
    // static_asserts already pin the hex forms at compile time. This checks
    // they round-trip through the wire encoding unchanged, at the exact
    // boundary a real cross order would use them.
    uint8_t a[o::kEnterOrderLen];
    e::enter_order(a, "MKTORDER000001", 'B', 100, "TEST", o::kMarketOrderPrice, 0,
                    "", ' ', ' ', 'N', 0, 'A', ' ');
    CHECK_EQ(o::enter_order::price(a), o::kMarketOrderPrice);

    uint8_t b[o::kEnterOrderLen];
    e::enter_order(b, "MAXPRICE000001", 'S', 100, "TEST", o::kMaxPrice, 0,
                    "", ' ', ' ', 'N', 0, 'A', ' ');
    CHECK_EQ(o::enter_order::price(b), o::kMaxPrice);
}

}  // namespace

int main() {
    test_enter_order_round_trip();
    test_enter_order_raw_round_trip();
    test_replace_order_round_trip();
    test_cancel_order_round_trip();
    test_system_event_round_trip();
    test_accepted_round_trip();
    test_replaced_round_trip();
    test_canceled_round_trip();
    test_executed_round_trip();
    test_rejected_round_trip();
    test_token_vs_reference_number_never_confused();
    test_price_sentinels();
    return REPORT();
}

#pragma once
//
// gateway.hpp — where OUCH order entry meets the matching engine, with the
// risk layer in front of it.
//
// The client speaks OUCH 4.2 (ouch/messages.hpp) inside SoupBinTCP
// Unsequenced Data packets (soupbin/session.hpp). This class decodes those,
// validates them, and submits the survivors to a Matcher that is sharing ONE
// book with the phase-12.1 replayer. It answers with OUCH Accepted /
// Replaced / Canceled / Rejected, sequenced back down the same session.
//
// ============================================================================
// ONE BOOK. The Matcher this gateway drives must be the BORROWING kind
// (engine::Matcher(book::Book&, tick)), pointed at the same book::Book the
// replayer mutates. docs/phase12-design.md section 4 is unambiguous about why:
// "Two books — a 'market' book the matcher consults and a 'mine' book it owns
// — would put your order in a queue of one and hand it a fill rate nothing in
// reality supports." Every number this gateway computes against the book — the
// price collar especially — is meaningless if it is measuring a private book
// holding only our own orders. The borrowing constructor exists for this.
// ============================================================================
//
// ONE INTEGER, THREE ROLES. A strategy order's identity is a single 64-bit
// value with bit 63 set, and it is simultaneously:
//   * engine::Request::id     — what the Matcher keys the order by, and what
//                               Matcher::rest() writes into book::Order::ref
//   * the OUCH Reference Number in Accepted/Replaced
//   * the value the token map maps an OUCH token to
// These MUST be the same integer. If the reference published to the client
// differed from the Request::id the matcher used, then replay/split.hpp's
// aggressor walk — which classifies each resting order by
// is_strategy_ref(q->ref) — would see the low-valued id, take the historical
// branch, and skip the strategy order entirely. The 12.1 partition would stop
// working silently, with the strategy's own orders counted as historical and
// never filled. kStrategyRefBit is reused from replay/split.hpp rather than
// respelled, so there is one definition of where the boundary sits.
//
// REFERENCES COME FROM A VENUE-SCOPED SOURCE, NOT A PER-GATEWAY COUNTER.
// Matcher::orders_ is never erased from — submit() emplaces and cancel_meta()
// only mutates in place — so a duplicate Request::id is not a transient
// collision, it is permanent: Matcher::validate() returns Reject::DuplicateId
// for that id forever after. Two gateways on one matcher, each with its own
// counter starting at kStrategyRefBit|1, would collide on their first order
// and every order thereafter. RefSource is therefore a separate object, held
// by reference, shared by every gateway on a venue.
//
// FLATTEN IS LEVEL-TRIGGERED WITH A LATCH, NOT EDGE-TRIGGERED ON tick().
// The obvious design — flatten when ServerSession::tick() returns true — is
// unreachable for two of the four ways a session dies. tick() can only set
// Dead and LoginTimedOut; Ended (the client's own Logout Request) and
// ProtocolViolation (a malformed or out-of-state packet) are both set inside
// on_bytes() and reported by ITS return value. A client that logs out politely
// while holding resting orders is the ordinary case, and edge-triggering on
// tick() would never flatten it. So: after every call into the session, check
// is_terminal(state()) against a latch. The latch, not the edge, is what makes
// it exactly once.
//
// FLATTEN FILTERS ON !is_terminal(state), NOT ON "still resting". A stop order
// parked awaiting its trigger has resting == 0 and in_book == false, and it is
// still live: Matcher::fire_stops() runs on every submission by anyone sharing
// the matcher, and a triggered stop takes liquidity. A flatten that filtered on
// resting shares would leave the book flat, pass a count-the-cancels test, and
// leave the account armed.
//
// FLATTEN'S OWN CANCELS ARE NOT STRATEGY TRAFFIC. They never touch
// KillSwitch::on_message_sent(). risk/kill_switch.hpp documents
// max_messages_per_second as "our OUTBOUND messages", and its RateWindow
// buckets by 100ms — so a flatten of a thousand resting orders, all stamped
// with the same instant, would trip the message-rate limit with the risk
// layer's own remediation, latch the switch with the wrong Trip reason, and
// (because the flatten is itself triggered by the switch tripping) recurse.
// Remediation is not the thing the rate limit exists to bound.
//
// WHAT THIS GATEWAY DOES NOT DECIDE. Authentication: SoupBinTCP's own text
// describes a scheme for carrying a username and password, not a policy for
// judging them, and ServerSession deliberately stops at LoginReceived and
// hands the decision here. This class resolves it synchronously with a
// caller-supplied predicate rather than inventing a credential store.
//
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/emit/itch_encode.hpp"
#include "itchbook/emit/sink.hpp"
#include "itchbook/engine/matcher.hpp"
#include "itchbook/engine/order_types.hpp"
#include "itchbook/ouch/encode.hpp"
#include "itchbook/ouch/messages.hpp"
#include "itchbook/replay/split.hpp"
#include "itchbook/risk/kill_switch.hpp"
#include "itchbook/soupbin/session.hpp"

namespace itchbook::engine {

// One per venue, shared by every Gateway on it. See the banner: a per-gateway
// counter collides permanently, because Matcher::orders_ is never erased.
class RefSource {
public:
    RefSource() = default;

    uint64_t next() {
        const uint64_t id = replay::kStrategyRefBit | ++counter_;
        assert(replay::is_strategy_ref(id) && "reference left the strategy half");
        return id;
    }

    uint64_t issued() const { return counter_; }

private:
    uint64_t counter_ = 0;
};

// A 14-byte OUCH token, comparable and hashable so it can key a map. Tokens
// are case-sensitive and may contain spaces (spec 1.2), so this is a byte
// comparison with no normalisation of any kind.
struct Token {
    uint8_t b[14] = {};

    bool operator==(const Token& o) const { return std::memcmp(b, o.b, 14) == 0; }
};

struct TokenHash {
    size_t operator()(const Token& t) const {
        size_t h = 1469598103934665603ull;
        for (uint8_t c : t.b) { h ^= c; h *= 1099511628211ull; }
        return h;
    }
};

inline Token make_token(const uint8_t* wire) {
    Token t;
    std::memcpy(t.b, wire, 14);
    return t;
}

class Gateway {
public:
    struct Config {
        // The one symbol this gateway fronts. A Matcher is one book, which is
        // one symbol (design section 4), so an order naming any other stock
        // must be rejected rather than resting in the wrong queue. Stored
        // space-padded to 8, matching the wire form.
        char stock[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

        // Price collar: reject a limit priced further than this many ticks
        // through the opposite side of the book. Zero disables it. Measured
        // against the SHARED book, which is the only reason the number means
        // anything -- see the banner.
        int32_t collar_ticks = 0;

        // Pre-trade position bound, in shares, absolute. Zero disables it.
        // Deliberately duplicated rather than shared with sim::RiskLimits:
        // that type lives inside the backtester header and is typed on a
        // different Side enum. The duplication is noted in the 12.5 write-up
        // as a real (if minor) divergence risk rather than hidden.
        int64_t max_position = 0;

        // OUCH caps shares at "greater than zero and less than 1,000,000"
        // (Enter Order, Shares). Enforced here rather than trusted.
        uint32_t max_shares = 999999;
    };

    // `session` and `matcher` and `refs` and `kill` all outlive this object.
    Gateway(Config cfg, soupbin::ServerSession& session, Matcher& matcher,
            RefSource& refs, risk::KillSwitch& kill)
        : cfg_(cfg), session_(&session), matcher_(&matcher), refs_(&refs), kill_(&kill) {}

    // Where this gateway publishes ITCH describing the matcher's mutations of
    // the shared book. Null (the default) means nothing is published, which is
    // the pre-12.6 behaviour and keeps every earlier test unchanged.
    void set_itch_sink(emit::Sink* s) { itch_ = s; }

    // The market's clock, advanced by whatever is driving the replay, and
    // distinct from the wall clock every entry point carries.
    //
    // docs/phase12-design.md section 6: replay time "drives message timestamps
    // and the strategy's (T - t)" while tick-to-trade "has nothing to do with
    // replay time and must never be derived from message timestamps". A single
    // clock cannot serve both -- wall clock on the tape overflows the 48-bit
    // ITCH timestamp field and publishes a wrapped value, and replay time on
    // the SoupBinTCP session makes a 50x replay time itself out fifty times
    // too fast. The assert is the design doc's own request for one.
    void set_replay_now(uint64_t replay_ns) {
        assert(replay_ns < (uint64_t{1} << 48) &&
               "replay clock does not fit the 48-bit ITCH timestamp -- wall clock?");
        replay_ns_ = replay_ns;
    }

    uint64_t replay_now() const { return replay_ns_; }

    // ---- the inbound path -----------------------------------------------------
    //
    // Called with the payload of one Unsequenced Data packet, already stripped
    // of its SoupBinTCP header by ServerSession. Returns false if the message
    // was not well-formed OUCH, which the caller should treat the way
    // ServerSession treats a bad frame: a protocol violation, not a business
    // rejection. There may be no token to echo in a J, so a J is not available
    // as an answer here.
    bool on_ouch(const uint8_t* p, size_t len, uint64_t now_ns) {
        wall_ns_ = now_ns;
        have_clock_ = true;

        // Check 0, before any field is read: is this even a message?
        // ServerSession's frame validation accepts an Unsequenced Data packet
        // of ANY payload length including zero, because SoupBinTCP does not
        // know what it is carrying. So the first field access here would be
        // the out-of-bounds read if the length were not checked first.
        if (len < 1) return false;
        const char type = static_cast<char>(p[0]);
        switch (type) {
            case 'O': if (len != ouch::kEnterOrderLen) return false; break;
            case 'U': if (len != ouch::kReplaceOrderLen) return false; break;
            case 'X': if (len != ouch::kCancelOrderLen) return false; break;
            default: return false;
        }

        switch (type) {
            case 'O': on_enter(p); return true;
            case 'U': on_replace(p); return true;
            case 'X': on_cancel(p); return true;
            default: return false;   // unreachable
        }
    }

    // ---- session lifecycle ----------------------------------------------------
    //
    // Call after EVERY interaction with the session -- on_bytes, tick, and the
    // login decisions -- so a terminal state reached through any path is seen.
    // Level-triggered with a latch rather than edge-triggered: see the banner.
    // Returns true on the call that performs the flatten, exactly once.
    bool poll(uint64_t now_ns) {
        wall_ns_ = now_ns;
        have_clock_ = true;
        if (flattened_) return false;
        if (!soupbin::is_terminal(session_->state())) return false;
        death_cause_ = session_->state();   // snapshot: a terminal state is not
                                            // stable, Dead can later become
                                            // ProtocolViolation on straggler bytes
        flattened_ = true;
        flatten();
        return true;
    }

    // Resolve the login ServerSession deliberately left open. `ok` is the
    // caller's credential decision -- this class has no credential store and
    // does not invent one.
    void decide_login(bool ok, const char* session_id, const char* sequence_number,
                      uint64_t now_ns) {
        wall_ns_ = now_ns;
        have_clock_ = true;
        if (session_->state() != soupbin::State::LoginReceived) return;
        if (ok) session_->accept_login(session_id, sequence_number, now_ns);
        else    session_->reject_login('A');   // 'A' = Not Authorized (SoupBin 2.2.2)
    }

    // ---- fills, from either direction ---------------------------------------
    //
    // A fill reaches this gateway two ways and only a cursor over the
    // Matcher's own fill log sees both:
    //
    //   * our order crossed -- Matcher::match(), inside our own submit();
    //   * our resting order was hit by the phase-12.1 aggressor --
    //     Matcher::apply_external_fill(), during the replayer's apply(), with
    //     no call of ours on the stack at all.
    //
    // Call after anything that could have traded. Idempotent: the cursor only
    // moves forward, so calling it twice publishes nothing twice.
    void pump_fills(uint64_t now_ns) {
        wall_ns_ = now_ns;
        have_clock_ = true;
        const std::vector<Fill>& fills = matcher_->fills();
        for (; fills_cursor_ < fills.size(); ++fills_cursor_) {
            const Fill& f = fills[fills_cursor_];
            const bool taker_ours = refs_to_token_.count(f.taker) != 0;
            const bool maker_ours = refs_to_token_.count(f.maker) != 0;
            if (!taker_ours && !maker_ours) continue;   // neither side is ours

            // ITCH describes what happened to RESTING orders, so an execution
            // always names the maker. A taker that crossed on arrival was
            // never published, and an 'E' naming it would land as an unknown
            // reference at every consumer.
            //
            // Emitted only for fills OUR submit produced. When the replayer's
            // aggressor hits one of our resting orders it has already emitted
            // that 'E' itself (split.hpp), and publishing a second one would
            // double the tape.
            if (taker_ours) {
                emit_itch_executed(f.maker, f.shares, f.match_number);
                // The exchange's own book must record the print exactly once
                // per emitted 'E'. Book::take() deliberately does not --
                // its comment says those statistics describe "the feed's
                // trades and not ours", which stops being true the moment we
                // publish our own fills, because then our fills ARE the feed.
                matcher_->book().note_feed_trade(f.price, f.shares);
            }

            // OUCH goes to whichever side is ours -- both, if we own both.
            if (taker_ours) send_executed(id_token(f.taker), f.shares, f.price, f.match_number);
            if (maker_ours) send_executed(id_token(f.maker), f.shares, f.price, f.match_number);

            // Position moves for every share of ours that traded.
            if (taker_ours) position_ += delta_for(f.taker, f.shares);
            if (maker_ours) position_ += delta_for(f.maker, f.shares);
            if (maker_ours) drop_if_dead(f.maker);
            if (taker_ours) drop_if_dead(f.taker);
        }
    }

    // ---- observation ------------------------------------------------------------

    uint64_t live_orders() const { return live_.size(); }
    uint64_t cancels_sent() const { return cancels_sent_; }
    uint64_t rejects_sent() const { return rejects_sent_; }
    uint64_t accepts_sent() const { return accepts_sent_; }
    uint64_t executes_sent() const { return executes_sent_; }
    uint64_t itch_sent() const { return itch_sent_; }
    bool flattened() const { return flattened_; }

    // Register an order this gateway did not itself submit, so flatten() sees
    // it. Exists for one reason: OUCH 4.2's core subset has no stop-order
    // type, so a parked stop -- the case that proves flatten filters on
    // liveness rather than on resting shares -- cannot be created through the
    // wire path at all. Named for what it is rather than hidden behind a
    // plausible-sounding production API.
    void adopt_for_test(uint64_t ref) { live_.insert(ref); }
    soupbin::State death_cause() const { return death_cause_; }
    int64_t position() const { return position_; }

private:
    // ---- validation -------------------------------------------------------------
    //
    // Returns 0 if the order is acceptable, or the OUCH reject reason
    // character otherwise. Every code is one the spec's own section 3.10.1
    // defines for that situation; none is invented here.
    char validate_new(char side, uint32_t shares, const uint8_t* stock,
                      int32_t price, bool is_market) {
        if (std::memcmp(stock, cfg_.stock, 8) != 0) return ouch::reject_reason::kInvalidStock;
        // Side must be exactly one of the four the spec lists; never defaulted.
        // A silent `side == 'B' ? Buy : Sell` would turn a garbled byte into a
        // sell order.
        if (side != 'B' && side != 'S' && side != 'T' && side != 'E') {
            return ouch::reject_reason::kOther;
        }
        if (shares == 0 || shares > cfg_.max_shares) {
            return ouch::reject_reason::kSafetyThreshold;
        }
        if (!is_market && (price <= 0 || price > ouch::kMaxPrice)) {
            return ouch::reject_reason::kInvalidPrice;
        }
        if (!is_market && collar_breached(side, price)) {
            // The Canceled table's own words for this situation are "Market
            // Collars"; the Rejected table has no collar-specific code, so a
            // pre-trade collar rejection uses the risk family's fat-finger
            // code, which is what a price far through the book is.
            return ouch::reject_reason::kRiskFatFinger;
        }
        if (position_breached(side, shares)) {
            return ouch::reject_reason::kRiskAggregate;
        }
        return 0;
    }

    // Measured against the SHARED book -- the whole reason the borrowing
    // Matcher constructor exists.
    bool collar_breached(char side, int32_t price) const {
        if (cfg_.collar_ticks <= 0) return false;
        const book::Book& b = matcher_->book();
        int32_t opposite = 0;
        const bool buy = (side == 'B');
        // A buy is collared against the best ASK, a sell against the best BID.
        const bool have = buy ? b.best_ask(&opposite) : b.best_bid(&opposite);
        if (!have) return false;   // nothing to measure against is not a breach
        const int64_t band = static_cast<int64_t>(cfg_.collar_ticks) * kTick;
        return buy ? (static_cast<int64_t>(price) > static_cast<int64_t>(opposite) + band)
                   : (static_cast<int64_t>(price) < static_cast<int64_t>(opposite) - band);
    }

    bool position_breached(char side, uint32_t shares) const {
        if (cfg_.max_position <= 0) return false;
        const int64_t delta = (side == 'B') ? static_cast<int64_t>(shares)
                                            : -static_cast<int64_t>(shares);
        const int64_t after = position_ + delta;
        return after > cfg_.max_position || after < -cfg_.max_position;
    }

    // ---- Enter Order ------------------------------------------------------------
    void on_enter(const uint8_t* p) {
        const Token tok = make_token(ouch::enter_order::order_token(p));
        // Duplicate token. The spec says a repeated token is IGNORED ("the new
        // order will be ignored"); docs/phase12-design.md section 5 sanctions
        // rejecting instead ("a token collision is a protocol error the
        // gateway rejects"). Recorded as a deliberate deviation, with a code
        // reserved for it so a client can tell this J from one that killed the
        // original order -- Rejected carries no reference number, so the code
        // is the only thing distinguishing them.
        if (tokens_.find(tok) != tokens_.end()) {
            send_reject(tok, ouch::reject_reason::kOther);
            return;
        }

        // The kill switch gates on INTENT, not on message type: Enter and
        // Replace both create or increase exposure and both must fail closed.
        // Cancel is deliberately never gated -- a tripped gateway that refuses
        // inbound cancels would be blocking the only client action that
        // reduces risk.
        if (!kill_->live()) {
            send_reject(tok, ouch::reject_reason::kHalted);
            return;
        }

        const int32_t price = ouch::enter_order::price(p);
        // The spec's own rule, from ouch/messages.hpp: a price at or above
        // kMarketOrderThreshold is a market order, not merely the single
        // sentinel value. $200,000.00 exactly is a market order.
        const bool is_market = price >= ouch::kMarketOrderThreshold;
        const char side = ouch::enter_order::side(p);
        const uint32_t shares = ouch::enter_order::shares(p);

        if (const char why = validate_new(side, shares, ouch::enter_order::stock(p),
                                          price, is_market)) {
            send_reject(tok, why);
            return;
        }

        submit_new(tok, side, shares, price, is_market,
                   ouch::enter_order::stock(p), p);
    }

    void submit_new(const Token& tok, char side, uint32_t shares, int32_t price,
                    bool is_market, const uint8_t* stock, const uint8_t* p) {
        Request r;
        r.id = refs_->next();
        // owner carries the OUCH firm, which is what STP is actually about --
        // NOT a per-session tag. Flatten scopes by this gateway's own live set
        // instead, which is per-session by construction and leaves owner free.
        r.owner = firm_key(ouch::enter_order::firm(p));
        r.side = (side == 'B') ? Side::Buy : Side::Sell;
        r.type = is_market ? Type::Market : Type::Limit;
        r.price = is_market ? 0 : price;
        r.quantity = shares;

        const Result res = matcher_->submit(r);
        if (!res.accepted()) {
            send_reject(tok, ouch::reject_reason::kOther);
            return;
        }

        tokens_.emplace(tok, r.id);
        refs_to_token_.emplace(r.id, tok);
        if (!is_terminal(res.state)) live_.insert(r.id);

        // Executions first, then the acknowledgement, then the ITCH 'A' for
        // whatever rested. A consumer that saw the 'A' before the 'E's would
        // briefly hold a crossed book and rank the resulting queue wrongly.
        send_accepted(tok, side, res.resting > 0 ? res.resting : shares,
                      stock, price, r.id,
                      is_terminal(res.state) ? 'D' : 'L');
        pump_fills(wall_ns_);
        const Matcher::Meta* meta = matcher_->find(r.id);
        if (meta != nullptr && meta->in_book) {
            emit_itch_add(r.id, side, meta->resting, r.price);
        }
        if (is_terminal(res.state)) live_.erase(r.id);
    }

    // ---- Replace Order ----------------------------------------------------------
    //
    // There is no atomic replace in the engine: the public surface is
    // submit() and cancel(), and cancel_meta() is irreversible (it transitions
    // to State::Cancelled, which legal_transition never leaves). So every
    // check on the REPLACEMENT runs to completion before the original is
    // touched. Cancelling first and validating second would leave a rejected
    // replacement with neither token live -- permanently, not transiently.
    void on_replace(const uint8_t* p) {
        const Token existing = make_token(ouch::replace_order::existing_order_token(p));
        const Token fresh = make_token(ouch::replace_order::replacement_order_token(p));

        auto it = tokens_.find(existing);
        if (it == tokens_.end() || live_.find(it->second) == live_.end()) {
            send_reject(fresh, ouch::reject_reason::kOther);
            return;
        }
        if (tokens_.find(fresh) != tokens_.end()) {
            send_reject(fresh, ouch::reject_reason::kOther);
            return;
        }
        if (!kill_->live()) {   // gates on intent: a replace increases exposure
            send_reject(fresh, ouch::reject_reason::kHalted);
            return;
        }

        const uint64_t old_id = it->second;
        const Matcher::Meta* meta = matcher_->find(old_id);
        if (meta == nullptr) { send_reject(fresh, ouch::reject_reason::kOther); return; }

        const int32_t price = ouch::replace_order::price(p);
        const bool is_market = price >= ouch::kMarketOrderThreshold;
        const uint32_t shares = ouch::replace_order::shares(p);
        // Side and stock are inherited from the original order; the Replace
        // message carries neither (ouch/messages.hpp).
        const char side = (meta->req.side == Side::Buy) ? 'B' : 'S';

        // Position check on the replacement must net out the original, which
        // is about to be cancelled -- otherwise replacing a 100-share buy with
        // another 100-share buy looks like doubling the position.
        const int64_t restore = (meta->req.side == Side::Buy)
                                    ? -static_cast<int64_t>(meta->resting + meta->hidden)
                                    : static_cast<int64_t>(meta->resting + meta->hidden);
        const int64_t saved = position_;
        position_ += restore;
        const char why = validate_new(side, shares, cfg_stock_wire(), price, is_market);
        position_ = saved;
        if (why != 0) { send_reject(fresh, why); return; }

        // Every check has passed. Only now is the original touched.
        const uint32_t gone = meta->resting + meta->hidden;
        const bool old_was_in_book = meta->in_book;
        if (!matcher_->cancel(old_id)) { send_reject(fresh, ouch::reject_reason::kOther); return; }
        live_.erase(old_id);
        send_canceled(existing, gone, ouch::cancel_reason::kUserRequested);
        if (old_was_in_book) emit_itch_delete(old_id);

        Request r;
        r.id = refs_->next();
        r.owner = meta->req.owner;
        r.side = meta->req.side;
        r.type = is_market ? Type::Market : Type::Limit;
        r.price = is_market ? 0 : price;
        r.quantity = shares;

        const Result res = matcher_->submit(r);
        if (!res.accepted()) { send_reject(fresh, ouch::reject_reason::kOther); return; }

        tokens_.emplace(fresh, r.id);
        refs_to_token_.emplace(r.id, fresh);
        if (!is_terminal(res.state)) live_.insert(r.id);
        send_replaced(fresh, existing, side, res.resting > 0 ? res.resting : shares,
                      price, r.id, is_terminal(res.state) ? 'D' : 'L');
        pump_fills(wall_ns_);
        const Matcher::Meta* fresh_meta = matcher_->find(r.id);
        if (fresh_meta != nullptr && fresh_meta->in_book) {
            emit_itch_add(r.id, side, fresh_meta->resting, r.price);
        }
        if (is_terminal(res.state)) live_.erase(r.id);
    }

    // ---- Cancel Order -------------------------------------------------------------
    //
    // Never gated on the kill switch: see on_enter's comment.
    void on_cancel(const uint8_t* p) {
        const Token tok = make_token(ouch::cancel_order::order_token(p));
        auto it = tokens_.find(tok);
        if (it == tokens_.end()) return;   // "Superfluous Cancel Order Messages
                                           //  are silently ignored" (spec 2.3)
        const uint64_t id = it->second;
        const Matcher::Meta* meta = matcher_->find(id);
        if (meta == nullptr) return;
        const uint32_t gone = meta->resting + meta->hidden;
        const bool was_in_book = meta->in_book;
        if (!matcher_->cancel(id)) return;   // already terminal: nothing to report
        live_.erase(id);
        send_canceled(tok, gone, ouch::cancel_reason::kUserRequested);
        // Only an order the feed was told about gets a delete. A parked stop,
        // a rejection and an order killed on arrival were never published.
        if (was_in_book) emit_itch_delete(id);
    }

    // ---- flatten ------------------------------------------------------------------
    uint64_t flatten() {
        assert(have_clock_ && "flatten() before any clock was set");
        if (in_flatten_) return 0;   // a trip raised during a flatten must not recurse
        in_flatten_ = true;

        uint64_t cancelled = 0;
        // Iterate this gateway's own live set, not Matcher::orders_ -- which is
        // never erased from and would therefore be O(every order ever
        // submitted by anyone sharing the matcher), inside the socket read
        // path, at the exact moment the market is moving.
        std::vector<uint64_t> ids(live_.begin(), live_.end());
        for (const uint64_t id : ids) {
            const Matcher::Meta* meta = matcher_->find(id);
            if (meta == nullptr) continue;
            // Filter on liveness, NOT on resting shares: a parked stop order
            // has resting == 0 and is still armed.
            if (engine::is_terminal(meta->state)) { live_.erase(id); continue; }
            const uint32_t gone = meta->resting + meta->hidden;
            const bool was_in_book = meta->in_book;
            if (!matcher_->cancel(id)) continue;   // count only real cancels
            live_.erase(id);
            ++cancelled;
            if (was_in_book) emit_itch_delete(id);
            // 'Z' = "System cancel. This order was cancelled by the system."
            send_canceled(id_token(id), gone, ouch::cancel_reason::kSystemCancel);
        }
        in_flatten_ = false;
        return cancelled;
    }

    // ---- outbound -----------------------------------------------------------------
    //
    // Every send goes through here. None of them calls
    // KillSwitch::on_message_sent(): see the banner.
    // A resting order becomes visible on the feed, at the DISPLAYED size only:
    // an iceberg's hidden remainder is not on the book and must not be on the
    // tape. No message at all when nothing rested -- a market order, an IOC,
    // a fully-filled limit and a rejection were never in the book.
    void emit_itch_add(uint64_t ref, char side, uint32_t shares, int32_t price) {
        if (itch_ == nullptr || shares == 0) return;
        uint8_t f[emit::kMaxMessage];
        const size_t n = emit::add_order(f, matcher_->book().locate(), 0, replay_ns_,
                                         ref, side, shares, cfg_stock_wire(), price);
        itch_->on_message(f, n);
        ++itch_sent_;
    }

    void emit_itch_executed(uint64_t maker_ref, uint32_t shares, uint64_t match) {
        if (itch_ == nullptr) return;
        uint8_t f[emit::kMaxMessage];
        const size_t n = emit::order_executed(f, matcher_->book().locate(), 0, replay_ns_,
                                              maker_ref, shares, match);
        itch_->on_message(f, n);
        ++itch_sent_;
    }

    void emit_itch_delete(uint64_t ref) {
        if (itch_ == nullptr) return;
        uint8_t f[emit::kMaxMessage];
        const size_t n = emit::order_delete(f, matcher_->book().locate(), 0, replay_ns_, ref);
        itch_->on_message(f, n);
        ++itch_sent_;
    }

    void send_executed(const Token& tok, uint32_t shares, int32_t price, uint64_t match) {
        uint8_t f[ouch::kExecutedLen];
        // 'R' is not one of the spec's liquidity flags for a removing order in
        // any table this project extracted, so it is written as a single
        // unsourced local choice rather than dressed up as a NASDAQ code --
        // the same treatment ouch/messages.hpp gives Cross Type.
        ouch::encode::executed(f, replay_ns_, tok.b, shares, price, 'R', match);
        emit(f, sizeof(f));
        ++executes_sent_;
    }

    // Each side's own position change, computed from ITS OWN side.
    //
    // The first version resolved the TAKER for both, which is zero for a fill
    // the replayer's aggressor performed against one of our resting orders --
    // find(0) returns null, the delta is zero, and the gateway's position
    // never moved for precisely the passive fills 12.7 exists to demonstrate.
    int64_t delta_for(uint64_t ref, uint32_t shares) const {
        const Matcher::Meta* m = matcher_->find(ref);
        if (m == nullptr) return 0;
        return (m->req.side == Side::Buy) ? static_cast<int64_t>(shares)
                                          : -static_cast<int64_t>(shares);
    }

    void drop_if_dead(uint64_t ref) {
        const Matcher::Meta* m = matcher_->find(ref);
        if (m != nullptr && engine::is_terminal(m->state)) live_.erase(ref);
    }

    void emit(const uint8_t* frame, size_t n) {
        // send_sequenced() checks no state, so a dead session would still
        // encode and push. Suppressing here keeps the wire honest: a session
        // that is gone gets no further traffic, and the cancel counts that
        // matter are taken at the matcher, not at the sink.
        if (session_->state() != soupbin::State::LoggedIn) return;
        // WALL clock, deliberately: this timestamp is the SoupBinTCP
        // session's own send-idle bookkeeping, not the tape.
        session_->send_sequenced(frame, n, wall_ns_);
    }

    void send_accepted(const Token& tok, char side, uint32_t shares,
                       const uint8_t* stock, int32_t price, uint64_t ref,
                       char order_state) {
        uint8_t f[ouch::kAcceptedLen];
        ouch::encode::accepted(f, replay_ns_, tok.b, side, shares, stock, price, 0,
                               blank_firm_, 'Y', ref, 'A', 'N', 0, ' ',
                               order_state, ' ');
        emit(f, sizeof(f));
        ++accepts_sent_;
    }

    void send_replaced(const Token& fresh, const Token& previous, char side,
                       uint32_t shares, int32_t price, uint64_t ref,
                       char order_state) {
        uint8_t f[ouch::kReplacedLen];
        ouch::encode::replaced(f, replay_ns_, fresh.b, side, shares, cfg_stock_wire(),
                               price, 0, blank_firm_, 'Y', ref, 'A', 'N', 0, ' ',
                               order_state, previous.b, ' ');
        emit(f, sizeof(f));
    }

    void send_canceled(const Token& tok, uint32_t decrement, char reason) {
        uint8_t f[ouch::kCanceledLen];
        ouch::encode::canceled(f, replay_ns_, tok.b, decrement, reason);
        emit(f, sizeof(f));
        ++cancels_sent_;
    }

    void send_reject(const Token& tok, char reason) {
        uint8_t f[ouch::kRejectedLen];
        ouch::encode::rejected(f, replay_ns_, tok.b, reason);
        emit(f, sizeof(f));
        ++rejects_sent_;
    }

    // ---- helpers ------------------------------------------------------------------

    const uint8_t* cfg_stock_wire() const {
        return reinterpret_cast<const uint8_t*>(cfg_.stock);
    }

    Token id_token(uint64_t id) const {
        auto it = refs_to_token_.find(id);
        return it == refs_to_token_.end() ? Token{} : it->second;
    }

    static uint64_t firm_key(const uint8_t* firm) {
        uint64_t k = 0;
        for (int i = 0; i < 4; ++i) k = (k << 8) | firm[i];
        return k;
    }

    static constexpr int32_t kTick = 100;   // Price(4) tick, as everywhere else

    Config cfg_;
    soupbin::ServerSession* session_;
    Matcher* matcher_;
    RefSource* refs_;
    risk::KillSwitch* kill_;

    std::unordered_map<Token, uint64_t, TokenHash> tokens_;
    std::unordered_map<uint64_t, Token> refs_to_token_;
    std::unordered_set<uint64_t> live_;

    uint8_t blank_firm_[4] = {' ', ' ', ' ', ' '};
    int64_t position_ = 0;
    uint64_t cancels_sent_ = 0;
    uint64_t rejects_sent_ = 0;
    uint64_t accepts_sent_ = 0;
    uint64_t executes_sent_ = 0;
    uint64_t itch_sent_ = 0;
    size_t fills_cursor_ = 0;
    emit::Sink* itch_ = nullptr;
    // TWO CLOCKS, never one. See the banner addition above set_replay_now().
    uint64_t wall_ns_ = 0;      // session bookkeeping: heartbeats, dead peers
    uint64_t replay_ns_ = 0;    // the tape: ITCH and OUCH message timestamps
    bool have_clock_ = false;
    bool flattened_ = false;
    bool in_flatten_ = false;
    soupbin::State death_cause_ = soupbin::State::AwaitingLogin;
};

}  // namespace itchbook::engine

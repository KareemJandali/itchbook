#pragma once
//
// strategy_snapshot.hpp — the other half of a mid-day restart.
//
// snapshot.hpp restores the MARKET: every order at every price, in queue
// order, plus the tape. That is the hard half and it was the only half. The
// build plan's phase 7 line is "reconstruct position and open orders after a
// mid-day process restart", and a restored market with no position and no
// resting orders of our own is a process that comes back flat and empty
// believing it always was.
//
// What that failure looks like matters, because it is not a crash. The book is
// right, the replay continues, every invariant holds, and the run reports a
// P&L for a strategy that spent the afternoon with no inventory. Nothing in
// the output says a restart happened. It is the exact shape of silent
// wrongness this phase exists to make impossible, sitting inside the mechanism
// built to prevent it.
//
// Two things are restored, and they are the two the plan names:
//
//   POSITION — Ledger: cash, position, fees, the edge accumulator and the fill
//   log. The log travels because drift() integrates over it and per_share()
//   divides by it; restoring the position without it would return the right
//   inventory and quietly change every derived number in the report.
//
//   OPEN ORDERS — QueueModel: every live entry WITH its `ahead` count, and for
//   the mbo model the ahead-set of references behind it. An order restored at
//   ahead = 0 is an order at the front of a queue it never reached: it fills
//   immediately, and the backtest reports money the strategy did not make.
//   This is the same correctness condition snapshot.hpp has about fill order,
//   for the same reason.
//
// What is NOT restored, said out loud rather than left to be discovered:
//
//   * MARKOUT SAMPLES IN FLIGHT. A markout is an observation of where the mid
//     went 100ms/1s/10s after a fill. A sample whose horizon spans the restart
//     has no observation — the mid at that instant was never seen by anyone.
//     Dropping it is not lost state; it is a measurement that did not happen,
//     and inventing one would be worse. The report counts unresolved fills
//     already, so it shows up there rather than vanishing.
//   * KILL-SWITCH counters and trip state. A restart is a decision point for
//     risk, not a thing to paper over: a switch that latched before the crash
//     must be re-armed by whoever restarted the process, deliberately.
//   * CONFIGURATION — the fee schedule, the queue model, the limits. Those
//     come from the command line on both sides. A snapshot that could override
//     them would let a stale file silently change what the operator asked for.
//
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "itchbook/recover/snapshot.hpp"
#include "itchbook/sim/ledger.hpp"
#include "itchbook/sim/queue_model.hpp"

namespace itchbook::recover {

inline constexpr uint32_t kStrategyMagic = 0x53545254;   // "STRT"
inline constexpr uint32_t kStrategyVersion = 1;

// One lane's recoverable state. queue_backtest runs four of these at once and
// they fill differently, so each carries its own.
struct StrategySnapshot {
    sim::Ledger::State ledger;
    sim::QueueModel::State queue;
};

inline std::vector<uint8_t> serialize(const StrategySnapshot& s) {
    std::vector<uint8_t> out;
    put(&out, kStrategyMagic);
    put(&out, kStrategyVersion);

    put(&out, s.ledger.cash);
    put(&out, s.ledger.position);
    put(&out, s.ledger.fees_total);
    put(&out, s.ledger.two_edge);
    put(&out, s.ledger.fills_without_mid);
    put(&out, static_cast<uint64_t>(s.ledger.fills.size()));
    for (const sim::LedgerFill& f : s.ledger.fills) {
        put(&out, f.ts);
        put(&out, static_cast<uint8_t>(f.side));
        put(&out, f.price);
        put(&out, f.shares);
        put(&out, f.two_mid);
        put(&out, static_cast<uint8_t>(f.mid_ok));
        put(&out, f.fee);
        put(&out, static_cast<uint8_t>(f.liquidity));
    }

    put(&out, static_cast<uint64_t>(s.queue.entries.size()));
    for (const sim::Entry& e : s.queue.entries) {
        put(&out, e.id);
        put(&out, static_cast<uint8_t>(e.side));
        put(&out, e.price);
        put(&out, e.display);
        put(&out, e.slice_size);
        put(&out, e.hidden);
        put(&out, e.ahead);
        put(&out, e.ahead0);
        put(&out, e.arrived_ns);
        put(&out, e.refreshes);
        put(&out, static_cast<uint8_t>(e.live));
    }

    put(&out, static_cast<uint64_t>(s.queue.ahead_sets.size()));
    for (const auto& [id, flat] : s.queue.ahead_sets) {
        put(&out, id);
        put(&out, static_cast<uint64_t>(flat.size()));
        for (const auto& [ref, shares] : flat) {
            put(&out, ref);
            put(&out, shares);
        }
    }

    put(&out, s.queue.state);
    put(&out, static_cast<uint8_t>(s.queue.tradable));
    put(&out, s.queue.clamp_events);
    put(&out, s.queue.clamp_shares);
    put(&out, s.queue.priority_anomalies);
    put(&out, s.queue.anomaly_shares);
    put(&out, s.queue.hidden_inside_shares);
    put(&out, s.queue.naive_hidden_fills);
    put(&out, s.queue.naive_hidden_shares);
    return out;
}

inline bool deserialize_strategy(const uint8_t* buf, size_t len, StrategySnapshot* s) {
    size_t off = 0;
    uint32_t magic = 0, version = 0;
    if (!get(buf, len, &off, &magic) || magic != kStrategyMagic) return false;
    // Refuse rather than guess, exactly as snapshot.hpp does. A v2 file read by
    // v1 code restores a position that is subtly wrong and says nothing.
    if (!get(buf, len, &off, &version) || version != kStrategyVersion) return false;

    auto u8 = [&](bool* v) { uint8_t b = 0; if (!get(buf, len, &off, &b)) return false;
                             *v = b != 0; return true; };

    sim::Ledger::State& L = s->ledger;
    if (!get(buf, len, &off, &L.cash)) return false;
    if (!get(buf, len, &off, &L.position)) return false;
    if (!get(buf, len, &off, &L.fees_total)) return false;
    if (!get(buf, len, &off, &L.two_edge)) return false;
    if (!get(buf, len, &off, &L.fills_without_mid)) return false;

    uint64_t n = 0;
    if (!get(buf, len, &off, &n)) return false;
    // A length prefix from a corrupt file must not become a multi-gigabyte
    // reserve. Every element costs bytes, so the remaining buffer bounds the
    // count; reserving on an unchecked n is how a truncated file turns into an
    // out-of-memory abort instead of a clean refusal.
    if (n > (len - off)) return false;
    L.fills.clear();
    L.fills.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        sim::LedgerFill f;
        uint8_t side = 0, liq = 0;
        if (!get(buf, len, &off, &f.ts)) return false;
        if (!get(buf, len, &off, &side)) return false;
        if (!get(buf, len, &off, &f.price)) return false;
        if (!get(buf, len, &off, &f.shares)) return false;
        if (!get(buf, len, &off, &f.two_mid)) return false;
        if (!u8(&f.mid_ok)) return false;
        if (!get(buf, len, &off, &f.fee)) return false;
        if (!get(buf, len, &off, &liq)) return false;
        f.side = static_cast<sim::Side>(side);
        f.liquidity = static_cast<sim::Liquidity>(liq);
        L.fills.push_back(f);
    }

    sim::QueueModel::State& Q = s->queue;
    if (!get(buf, len, &off, &n)) return false;
    if (n > (len - off)) return false;
    Q.entries.clear();
    Q.entries.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        sim::Entry e;
        uint8_t side = 0;
        if (!get(buf, len, &off, &e.id)) return false;
        if (!get(buf, len, &off, &side)) return false;
        if (!get(buf, len, &off, &e.price)) return false;
        if (!get(buf, len, &off, &e.display)) return false;
        if (!get(buf, len, &off, &e.slice_size)) return false;
        if (!get(buf, len, &off, &e.hidden)) return false;
        if (!get(buf, len, &off, &e.ahead)) return false;
        if (!get(buf, len, &off, &e.ahead0)) return false;
        if (!get(buf, len, &off, &e.arrived_ns)) return false;
        if (!get(buf, len, &off, &e.refreshes)) return false;
        if (!u8(&e.live)) return false;
        e.side = static_cast<sim::Side>(side);
        Q.entries.push_back(e);
    }

    if (!get(buf, len, &off, &n)) return false;
    if (n > (len - off)) return false;
    Q.ahead_sets.clear();
    Q.ahead_sets.reserve(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t id = 0, m = 0;
        if (!get(buf, len, &off, &id)) return false;
        if (!get(buf, len, &off, &m)) return false;
        if (m > (len - off)) return false;
        std::vector<std::pair<uint64_t, uint32_t>> flat;
        flat.reserve(static_cast<size_t>(m));
        for (uint64_t j = 0; j < m; ++j) {
            uint64_t ref = 0;
            uint32_t shares = 0;
            if (!get(buf, len, &off, &ref)) return false;
            if (!get(buf, len, &off, &shares)) return false;
            flat.emplace_back(ref, shares);
        }
        Q.ahead_sets.emplace_back(id, std::move(flat));
    }

    if (!get(buf, len, &off, &Q.state)) return false;
    if (!u8(&Q.tradable)) return false;
    if (!get(buf, len, &off, &Q.clamp_events)) return false;
    if (!get(buf, len, &off, &Q.clamp_shares)) return false;
    if (!get(buf, len, &off, &Q.priority_anomalies)) return false;
    if (!get(buf, len, &off, &Q.anomaly_shares)) return false;
    if (!get(buf, len, &off, &Q.hidden_inside_shares)) return false;
    if (!get(buf, len, &off, &Q.naive_hidden_fills)) return false;
    if (!get(buf, len, &off, &Q.naive_hidden_shares)) return false;
    return true;
}

}  // namespace itchbook::recover

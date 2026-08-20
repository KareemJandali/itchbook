#pragma once
//
// latency_model.hpp — your order does not arrive when you send it.
//
// The build plan: "Your order takes N microseconds to reach the exchange. The
// book moves in between. Make N configurable and show P&L sensitivity to it."
//
// Three things follow, and the third is the one people forget:
//
//   * **Queue position is a property of ARRIVAL, not of decision.** By the time
//     the order lands, the level it was aiming at has grown, shrunk, or gone.
//     Computing `ahead` at decision time is straightforward look-ahead: it uses
//     a book state the order never actually met.
//
//   * **Cancels are late too.** A cancel decided at T does not take effect until
//     T + latency, so fills in between still happen. This is the "cancelled too
//     late" case and it is a real, one-directional source of losses — precisely
//     the fills a strategy most wanted to avoid.
//
//   * **Zero is not a safe default.** It is the strongest single form of silent
//     optimism available here, so the default is non-zero and a zero-latency run
//     has to be asked for explicitly.
//
// Same-nanosecond ties resolve against us: an action whose arrival coincides
// with a feed message is applied AFTER that message. Where the tie-break is
// arbitrary, it should not be arbitrary in our favour.
//
#include <cstdint>

namespace itchbook::sim {

struct LatencyModel {
    // One way, decision to exchange. 250us is a plausible colocated round trip
    // for a retail-adjacent setup and is deliberately not zero.
    uint64_t order_ns = 250000;
    // Cancels usually travel the same path, but they are separate so the
    // asymmetry can be explored: a venue that prioritises cancels changes the
    // adverse-selection picture materially.
    uint64_t cancel_ns = 250000;

    static LatencyModel zero() { return LatencyModel{0, 0}; }
    static LatencyModel uniform(uint64_t ns) { return LatencyModel{ns, ns}; }

    uint64_t arrival_for_quote(uint64_t decided_ns) const { return decided_ns + order_ns; }
    uint64_t arrival_for_cancel(uint64_t decided_ns) const { return decided_ns + cancel_ns; }
};

}  // namespace itchbook::sim

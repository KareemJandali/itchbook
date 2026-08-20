// queue_backtest — the phase 6 driver.
//
// Same feed, same strategy, four fill models, one pass. The headline output is
// the P&L-per-share column: the gap between `naive` and `pessimistic` is the
// point of the whole project, and `mbo` sitting inside the band is what says the
// band is honest.
//
// Usage:
//   queue_backtest <feed.gz> [--strategy touch-maker|patient-maker|crosser|null|far-quoter]
//                  [--size N] [--json out.json] [--fees base|top|inverted]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/sim/backtest.hpp"
#include "itchbook/sim/strategies.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

double cents(Money micros) { return static_cast<double>(micros) / 10000.0; }
double dollars(Money micros) { return static_cast<double>(micros) / 1000000.0; }

void print_results(const std::vector<LaneResult>& rs, uint64_t events,
                   const char* strategy) {
    std::printf("strategy: %s   events: %" PRIu64 "\n\n", strategy, events);
    std::printf("%-12s %8s %9s %12s %11s %11s %11s %9s\n", "model", "fills", "shares",
                "P&L ($)", "c/share", "edge c/sh", "fees c/sh", "resid");
    std::printf("---------------------------------------------------------"
                "--------------------------------\n");
    for (const LaneResult& r : rs) {
        std::printf("%-12s %8" PRIu64 " %9" PRIu64 " %12.2f %11.4f %11.4f %11.4f %9" PRId64 "\n",
                    to_string(r.model), r.fills, r.shares, dollars(r.equity),
                    cents(r.equity_per_share),
                    r.shares ? cents(divide_round(r.edge, static_cast<Money>(r.shares))) : 0.0,
                    r.shares ? cents(divide_round(r.fees, static_cast<Money>(r.shares))) : 0.0,
                    r.residual_position);
    }

    std::printf("\nadverse selection (drift, cents/share; negative = picked off)\n");
    std::printf("%-12s %12s %12s %12s %10s\n", "model", "100ms", "1s", "10s", "unresolved");
    std::printf("-----------------------------------------------------------------\n");
    for (const LaneResult& r : rs) {
        std::printf("%-12s %12.4f %12.4f %12.4f %10" PRIu64 "\n", to_string(r.model),
                    cents(r.markouts[0].drift_per_share),
                    cents(r.markouts[1].drift_per_share),
                    cents(r.markouts[2].drift_per_share),
                    r.markouts[2].unresolved_fills);
    }

    std::printf("\nmechanism (how the fills were reached, not how much they made)\n");
    std::printf("%-12s %10s %10s %10s %10s\n", "model", "lock", "through", "clamped",
                "anomalies");
    std::printf("------------------------------------------------------------\n");
    for (const LaneResult& r : rs) {
        std::printf("%-12s %10" PRIu64 " %10" PRIu64 " %10" PRIu64 " %10" PRIu64 "\n",
                    to_string(r.model), r.lock_fills, r.through_fills, r.clamp_events,
                    r.priority_anomalies);
    }

    // Inventory, because a maker's P&L is only a maker's P&L if the inventory
    // stayed small. A large peak means most of the number is a directional bet.
    std::printf("\ninventory (a maker's P&L is only a maker's P&L if this stayed small)\n");
    std::printf("%-12s %14s %14s %14s\n", "model", "peak position", "residual",
                "quotes refused");
    std::printf("--------------------------------------------------------\n");
    for (const LaneResult& r : rs) {
        std::printf("%-12s %14" PRId64 " %14" PRId64 " %14" PRIu64 "\n", to_string(r.model),
                    r.peak_position, r.residual_position, r.suppressed_quotes);
    }

    // A tripped lane stopped trading partway through the day, so its P&L is not
    // a full session and must never be compared against one that ran to the
    // close without the reader being told.
    bool any_trip = false;
    for (const LaneResult& r : rs) any_trip = any_trip || r.trip != risk::Trip::None;
    if (any_trip) {
        std::printf("\nKILL SWITCH\n");
        std::printf("%-12s %16s %12s %14s %14s\n", "model", "reason", "at event",
                    "observed", "limit");
        std::printf("---------------------------------------------------------"
                    "----------------\n");
        for (const LaneResult& r : rs) {
            if (r.trip == risk::Trip::None) {
                std::printf("%-12s %16s\n", to_string(r.model), "-");
                continue;
            }
            std::printf("%-12s %16s %12" PRIu64 " %14" PRId64 " %14" PRId64 "\n",
                        to_string(r.model), risk::to_string(r.trip),
                        r.tripped_at_event, r.trip_observed, r.trip_limit);
        }
        std::printf("\nA tripped lane stopped early. Its P&L is a partial "
                    "session, not a worse one.\n");
    }
}

void write_json(const char* path, const std::vector<LaneResult>& rs, uint64_t events,
                const char* strategy) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return;
    std::fprintf(f, "{\n  \"strategy\": \"%s\",\n  \"events\": %" PRIu64 ",\n  \"models\": {",
                 strategy, events);
    bool first = true;
    for (const LaneResult& r : rs) {
        std::fprintf(f,
                     "%s\n    \"%s\": {\"fills\": %" PRIu64 ", \"shares\": %" PRIu64
                     ", \"equity_micros\": %" PRId64 ", \"per_share_micros\": %" PRId64
                     ", \"edge_micros\": %" PRId64 ", \"fees_micros\": %" PRId64
                     ", \"residual\": %" PRId64 ", \"lock_fills\": %" PRIu64
                     ", \"through_fills\": %" PRIu64 ", \"clamp_events\": %" PRIu64
                     ", \"priority_anomalies\": %" PRIu64
                     ", \"drift_100ms\": %" PRId64 ", \"drift_1s\": %" PRId64
                     ", \"drift_10s\": %" PRId64
                     ", \"unresolved_10s\": %" PRIu64
                     ", \"peak_position\": %" PRId64
                     ", \"suppressed_quotes\": %" PRIu64
                     ", \"trip\": \"%s\"}",
                     first ? "" : ",", to_string(r.model), r.fills, r.shares, r.equity,
                     r.equity_per_share, r.edge, r.fees, r.residual_position, r.lock_fills,
                     r.through_fills, r.clamp_events, r.priority_anomalies,
                     r.markouts[0].drift_per_share, r.markouts[1].drift_per_share,
                     r.markouts[2].drift_per_share, r.markouts[2].unresolved_fills,
                     r.peak_position, r.suppressed_quotes, risk::to_string(r.trip));
        first = false;
    }
    std::fprintf(f, "\n  }\n}\n");
    std::fclose(f);
}

template <typename S>
int run(const char* feed, S strategy, FeeSchedule fees, LatencyModel latency,
        RiskLimits limits, risk::KillSwitchConfig kill, const char* json_path) {
    Backtest<S> bt(strategy, fees, latency, limits, kill);
    try {
        Reader reader(feed);
        parse(reader, bt);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    const std::vector<LaneResult> rs = bt.results();
    std::printf("latency: %" PRIu64 " ns order / %" PRIu64 " ns cancel\n",
                latency.order_ns, latency.cancel_ns);
    print_results(rs, bt.events(), S::name());
    if (json_path != nullptr) write_json(json_path, rs, bt.events(), S::name());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed = nullptr;
    std::string strategy = "touch-maker";
    std::string fee_tier = "base";
    const char* json_path = nullptr;
    uint32_t size = 100;
    LatencyModel latency;   // deliberately non-zero by default
    RiskLimits risk;        // no position limit unless asked for
    risk::KillSwitchConfig kill;   // every limit off unless asked for

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--strategy" && i + 1 < argc) strategy = argv[++i];
        else if (a == "--fees" && i + 1 < argc) fee_tier = argv[++i];
        else if (a == "--json" && i + 1 < argc) json_path = argv[++i];
        else if (a == "--size" && i + 1 < argc) size = static_cast<uint32_t>(std::atol(argv[++i]));
        else if (a == "--latency-ns" && i + 1 < argc)
            latency = LatencyModel::uniform(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--cancel-latency-ns" && i + 1 < argc)
            latency.cancel_ns = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--max-position" && i + 1 < argc)
            risk.max_position = std::strtoll(argv[++i], nullptr, 10);
        // The kill-switch limits. Distinct from --max-position on purpose:
        // that one declines an order, these end the session.
        else if (a == "--kill-position" && i + 1 < argc)
            kill.max_position = std::strtoll(argv[++i], nullptr, 10);
        else if (a == "--kill-notional" && i + 1 < argc)
            kill.max_notional = std::strtoll(argv[++i], nullptr, 10) * 1000000;
        else if (a == "--kill-msg-rate" && i + 1 < argc)
            kill.max_messages_per_second = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--kill-order-to-fill" && i + 1 < argc)
            kill.max_orders_per_fill = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--kill-drawdown" && i + 1 < argc)
            kill.max_drawdown = std::strtoll(argv[++i], nullptr, 10) * 1000000;
        else if (feed == nullptr) feed = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (feed == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <feed.gz> [--strategy touch-maker|patient-maker|crosser|null|far-quoter]\n"
                     "                    [--size N] [--fees base|top|inverted] [--json out]\n"
                     "                    [--latency-ns N] [--cancel-latency-ns N]\n"
                     "                    [--max-position N]\n"
                     "                    [--kill-position N] [--kill-notional DOLLARS]\n"
                     "                    [--kill-msg-rate N] [--kill-order-to-fill N]\n"
                     "                    [--kill-drawdown DOLLARS]\n",
                     argv[0]);
        return 2;
    }

    FeeSchedule fees;
    if (fee_tier == "top") fees = nasdaq_top_tier_2019();
    else if (fee_tier == "inverted") fees = inverted_venue_2019();

    if (strategy == "touch-maker") {
        TouchMaker s;
        s.size = size;
        return run(feed, s, fees, latency, risk, kill, json_path);
    }
    if (strategy == "patient-maker") {
        PatientMaker s;
        s.size = size;
        return run(feed, s, fees, latency, risk, kill, json_path);
    }
    if (strategy == "crosser") {
        Crosser s;
        s.size = size;
        return run(feed, s, fees, latency, risk, kill, json_path);
    }
    if (strategy == "far-quoter") {
        FarQuoter s;
        s.size = size;
        return run(feed, s, fees, latency, risk, kill, json_path);
    }
    if (strategy == "null") return run(feed, NullStrategy{}, fees, latency, risk, kill, json_path);
    std::fprintf(stderr, "error: unknown strategy '%s'\n", strategy.c_str());
    return 2;
}

// queue_sim — replay a feed with one or more of our orders resting in it, and
// report what each fill model says we would have traded.
//
// This is the phase's differential-test driver: python/reference/queue_sim.py
// does the same thing slowly and obviously, and the two must produce byte-
// identical fill logs. That discipline caught real bugs in phases 2 and 3.
//
// The order of operations per message is load-bearing and is mirrored exactly
// in the Python oracle:
//
//   1. session/halt state is updated first, so all lanes see identical eligibility
//   2. the pre-mutation snapshot is taken (E/C/X/D/U carry only a reference)
//   3. the book is mutated
//   4. the message is resolved and committed to every lane
//   5. price priority is evaluated against the post-mutation book
//
// Usage:
//   queue_sim <feed.gz> --place B:1000000:100:500[:display[:cancel_after]]
//             [--place ...] [--fills out.csv] [--json out.json]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/sim/queue_model.hpp"
#include "itchbook/sim/resolve.hpp"

using namespace itchbook;
using namespace itchbook::sim;

namespace {

struct Placement {
    Side side = Side::Buy;
    Price4 price = 0;
    uint32_t quantity = 0;
    uint64_t after = 0;      // place once this many messages have been applied
    uint32_t display = 0;
    // Pull the order once this many messages have been applied. Zero means
    // never, which is the default and the right one for a sweep; the
    // leave-one-out oracle needs it, because a replacement order that outlives
    // the order it replaced keeps filling after that order was gone and every
    // model reads as over-filling for a reason that is not the model.
    uint64_t cancel_after = 0;
    uint64_t id = 0;
    bool placed = false;
    bool cancelled = false;
};

const char* trigger_name(Trigger t) {
    switch (t) {
        case Trigger::Execution: return "execution";
        case Trigger::Lock:      return "lock";
        case Trigger::Through:   return "through";
        case Trigger::Naive:     return "naive";
        case Trigger::Taking:    return "taking";
    }
    return "?";
}

struct Lane {
    Model model;
    QueueModel q;
    std::vector<SimFill> fills;
    explicit Lane(Model m, QueueConfig c) : model(m), q(c) {}
};

struct Handler {
    book::Book book{100, 20, 1u << 20};
    std::vector<Lane>* lanes = nullptr;
    std::vector<Placement>* places = nullptr;
    uint64_t index = 0;

    void on_message(char type, const uint8_t* p, uint16_t) {
        // 1. session and halt state, before anything else, so every lane sees
        //    byte-identical eligibility.
        if (type == 'H') {
            const char state = itch::trading_action::state(p);
            for (Lane& l : *lanes) l.q.set_trading_state(state);
        } else if (type == 'S') {
            const char code = itch::system_event::code(p);
            // 'Q' opens market hours, 'M' closes them. Everything else leaves
            // the state as it was.
            if (code == 'Q') {
                for (Lane& l : *lanes) l.q.set_trading_state('T');
            } else if (code == 'M' || code == 'C') {
                for (Lane& l : *lanes) l.q.set_trading_state('\0');
            }
        }

        // 2 + 3. snapshot, then mutate.
        book::PreState pre;
        book::apply_ex(book, type, p, &pre);

        // 4. resolve and commit.
        const Resolved r = resolve(type, p, pre);
        for (Lane& l : *lanes) l.q.commit(r, book, &l.fills);

        // 5. price priority against the post-mutation book.
        const uint64_t ts = itch::timestamp(p);
        for (Lane& l : *lanes) l.q.apply_price_priority(book, ts, &l.fills);

        ++index;

        // Placements happen after the message that reaches their index, using
        // the book as it stands at that moment — arrival-time queue position,
        // never decision-time.
        for (Placement& pl : *places) {
            if (pl.placed || index < pl.after) continue;
            for (Lane& l : *lanes) {
                l.q.place(book, pl.id, pl.side, pl.price, pl.quantity, pl.display, ts);
            }
            pl.placed = true;
        }
        for (Placement& pl : *places) {
            if (!pl.placed || pl.cancelled || pl.cancel_after == 0) continue;
            if (index < pl.cancel_after) continue;
            for (Lane& l : *lanes) l.q.cancel(pl.id);
            pl.cancelled = true;
        }
    }
};

bool parse_placement(const char* spec, Placement* out) {
    // SIDE:PRICE:QTY:AFTER[:DISPLAY[:CANCEL_AFTER]]
    char side = 0;
    long long price = 0;
    long long qty = 0;
    long long after = 0;
    long long display = 0;
    long long cancel_after = 0;
    const int n = std::sscanf(spec, "%c:%lld:%lld:%lld:%lld:%lld", &side, &price, &qty,
                              &after, &display, &cancel_after);
    if (n < 4) return false;
    out->side = (side == 'B' || side == 'b') ? Side::Buy : Side::Sell;
    out->price = static_cast<Price4>(price);
    out->quantity = static_cast<uint32_t>(qty);
    out->after = static_cast<uint64_t>(after);
    out->display = (n >= 5) ? static_cast<uint32_t>(display) : 0;
    out->cancel_after = (n >= 6) ? static_cast<uint64_t>(cancel_after) : 0;
    return out->quantity > 0 && out->price > 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* feed = nullptr;
    const char* fills_path = nullptr;
    const char* json_path = nullptr;
    std::vector<Placement> places;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--place" && i + 1 < argc) {
            Placement pl;
            if (!parse_placement(argv[++i], &pl)) {
                std::fprintf(stderr, "error: bad --place spec '%s'\n", argv[i]);
                return 2;
            }
            pl.id = 1000000 + places.size();
            places.push_back(pl);
        } else if (a == "--fills" && i + 1 < argc) {
            fills_path = argv[++i];
        } else if (a == "--json" && i + 1 < argc) {
            json_path = argv[++i];
        } else if (feed == nullptr) {
            feed = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
            return 2;
        }
    }
    if (feed == nullptr || places.empty()) {
        std::fprintf(stderr,
                     "usage: %s <feed.gz> "
                     "--place SIDE:PRICE:QTY:AFTER[:DISPLAY[:CANCEL_AFTER]] ...\n"
                     "                    [--fills out.csv] [--json out.json]\n",
                     argv[0]);
        return 2;
    }

    std::vector<Lane> lanes;
    for (Model m : {Model::Naive, Model::Optimistic, Model::Mbo, Model::Pessimistic}) {
        QueueConfig c;
        c.model = m;
        lanes.emplace_back(m, c);
    }

    Handler h;
    h.lanes = &lanes;
    h.places = &places;

    try {
        Reader reader(feed);
        parse(reader, h);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    if (fills_path != nullptr) {
        std::FILE* f = std::fopen(fills_path, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", fills_path);
            return 1;
        }
        std::fprintf(f, "model,order_id,ts,side,price,shares,trigger\n");
        for (const Lane& l : lanes) {
            for (const SimFill& s : l.fills) {
                std::fprintf(f, "%s,%" PRIu64 ",%" PRIu64 ",%c,%" PRId32 ",%u,%s\n",
                             to_string(l.model), s.order_id, s.ts, to_char(s.side),
                             s.price, s.shares, trigger_name(s.trigger));
            }
        }
        std::fclose(f);
    }

    std::printf("%-12s %8s %10s %10s %8s\n", "model", "fills", "shares", "clamped", "anomaly");
    std::printf("------------------------------------------------------\n");
    for (const Lane& l : lanes) {
        uint64_t shares = 0;
        for (const SimFill& s : l.fills) shares += s.shares;
        std::printf("%-12s %8zu %10" PRIu64 " %10" PRIu64 " %8" PRIu64 "\n",
                    to_string(l.model), l.fills.size(), shares, l.q.clamp_events(),
                    l.q.priority_anomalies());
    }

    if (json_path != nullptr) {
        std::FILE* f = std::fopen(json_path, "w");
        if (f == nullptr) return 1;
        std::fprintf(f, "{\n  \"messages\": %" PRIu64 ",\n  \"models\": {", h.index);
        bool first = true;
        for (const Lane& l : lanes) {
            uint64_t shares = 0;
            for (const SimFill& s : l.fills) shares += s.shares;
            std::fprintf(f,
                         "%s\n    \"%s\": {\"fills\": %zu, \"shares\": %" PRIu64
                         ", \"clamp_events\": %" PRIu64 ", \"priority_anomalies\": %" PRIu64 "}",
                         first ? "" : ",", to_string(l.model), l.fills.size(), shares,
                         l.q.clamp_events(), l.q.priority_anomalies());
            first = false;
        }
        std::fprintf(f, "\n  }\n}\n");
        std::fclose(f);
    }
    return 0;
}

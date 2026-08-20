// book_replay — reconstruct one symbol's book from an ITCH feed.
//
// The C++ counterpart of python/reference/replay.py, and it must produce a
// byte-identical snapshot CSV and an identical summary for the same input. That
// equality is the phase 3 done-condition: not "close", identical. Anywhere the
// two differ, one of them misunderstands the protocol.
//
// Usage:
//   book_replay <feed.gz> [--symbol SYM] [--snapshots out.csv]
//               [--interval-ms N] [--levels N] [--limit N] [--tick N] [--quiet]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"

namespace {

// LOBSTER's dummy fills for levels a thin book does not reach. Matching them
// means a slice of this CSV diffs straight against a LOBSTER orderbook file.
constexpr int64_t kNoAskPrice = 9999999999LL;
constexpr int64_t kNoBidPrice = -9999999999LL;

struct Options {
    const char* feed = nullptr;
    std::string symbol;
    const char* snapshots = nullptr;
    uint64_t interval_ns = 60ULL * 1000 * 1000 * 1000;   // 1 minute
    size_t levels = 10;
    uint64_t limit = 0;         // 0 = no limit
    uint64_t end_ns = 0;        // 0 = run to the end of the file
    int32_t tick = 100;         // a penny, in Price(4) units
    bool quiet = false;
};

// 1,234,567 — matches Python's f"{n:,}" so the two summaries diff cleanly.
std::string comma(uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) {
        s.insert(static_cast<size_t>(i), ",");
    }
    return s;
}

// Price(4) integer -> dollars, or "-" when there is nothing to show.
std::string px(int32_t price) {
    if (price < 0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(price) / 10000.0);
    return buf;
}

std::string px_double(double price, bool present) {
    if (!present) return "-";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", price / 10000.0);
    return buf;
}

void write_header(std::FILE* out, size_t levels) {
    std::fputs("ts", out);
    for (size_t i = 1; i <= levels; ++i) {
        std::fprintf(out, ",ask_px_%zu,ask_sz_%zu,bid_px_%zu,bid_sz_%zu", i, i, i, i);
    }
    std::fputc('\n', out);
}

void write_snapshot(std::FILE* out, itchbook::book::Book& book, uint64_t ts, size_t levels,
                    std::vector<itchbook::book::LevelView>* asks,
                    std::vector<itchbook::book::LevelView>* bids) {
    book.top('S', levels, asks);
    book.top('B', levels, bids);
    std::fprintf(out, "%" PRIu64, ts);
    for (size_t i = 0; i < levels; ++i) {
        if (i < asks->size()) {
            std::fprintf(out, ",%" PRId32 ",%" PRIu64, (*asks)[i].price, (*asks)[i].shares);
        } else {
            std::fprintf(out, ",%" PRId64 ",0", kNoAskPrice);
        }
        if (i < bids->size()) {
            std::fprintf(out, ",%" PRId32 ",%" PRIu64, (*bids)[i].price, (*bids)[i].shares);
        } else {
            std::fprintf(out, ",%" PRId64 ",0", kNoBidPrice);
        }
    }
    std::fputc('\n', out);
}

// Streams the feed into the book, emitting snapshots on the way past each grid
// point. A handler type, not a std::function, so parse() inlines the whole
// dispatch — no indirect call on the hot path.
struct Replayer {
    Options opt;
    itchbook::book::Book book;
    std::FILE* out = nullptr;
    std::string padded_symbol;      // 8 chars, space-padded, as on the wire

    uint16_t locate = 0;
    bool locate_known = false;
    uint64_t read = 0;
    uint64_t applied = 0;
    uint64_t written = 0;
    uint64_t next_grid = 0;
    bool grid_started = false;
    bool stop = false;

    std::vector<itchbook::book::LevelView> asks_buf;
    std::vector<itchbook::book::LevelView> bids_buf;

    explicit Replayer(const Options& o) : opt(o), book(o.tick) {
        padded_symbol = o.symbol;
        padded_symbol.resize(8, ' ');
    }

    void on_message(char type, const uint8_t* p, uint16_t) {
        if (stop) return;
        ++read;
        if (opt.limit != 0 && read > opt.limit) {
            --read;
            stop = true;
            return;
        }
        if (!itchbook::book::modelled(type)) return;

        uint64_t ts = itchbook::itch::timestamp(p);
        // Messages are chronological within a feed, so once past the session
        // end there is nothing later to see. Mirrors replay.py --end-ns.
        if (opt.end_ns != 0 && ts >= opt.end_ns && ts > 0) {
            --read;
            stop = true;
            return;
        }

        if (!opt.symbol.empty()) {
            if (type == 'R' &&
                std::memcmp(itchbook::itch::stock_directory::stock(p),
                            padded_symbol.data(), 8) == 0) {
                locate = itchbook::itch::stock_locate(p);
                locate_known = true;
            }
            // System events carry no meaningful locate but bracket the session.
            if (type != 'S' &&
                (!locate_known || itchbook::itch::stock_locate(p) != locate)) {
                return;
            }
        }

        if (out != nullptr && ts > 0) {
            if (!grid_started) {
                next_grid = (ts / opt.interval_ns + 1) * opt.interval_ns;
                grid_started = true;
            }
            while (ts >= next_grid) {
                write_snapshot(out, book, next_grid, opt.levels, &asks_buf, &bids_buf);
                ++written;
                next_grid += opt.interval_ns;
            }
        }

        if (itchbook::book::apply(book, type, p)) ++applied;
    }
};

void print_summary(const Replayer& r) {
    const auto& b = r.book;
    int32_t bid = -1;
    int32_t ask = -1;
    if (!b.best_bid(&bid)) bid = -1;
    if (!b.best_ask(&ask)) ask = -1;

    auto row = [](const char* k, const std::string& v) {
        std::printf("%-26s %18s\n", k, v.c_str());
    };
    auto rule = []() { std::printf("---------------------------------------------\n"); };

    std::printf("%-26s %18s\n", "field", "value");
    rule();
    row("symbol", r.opt.symbol.empty() ? "(whole file)" : r.opt.symbol);
    row("messages read", comma(r.read));
    row("messages applied", comma(r.applied));
    row("unknown order refs", comma(b.unknown_ref()));
    rule();
    row("volume (shares)", comma(b.volume()));
    row("  of which hidden ('P')", comma(b.hidden_volume()));
    row("  of which cross ('Q')", comma(b.cross_volume()));
    row("trades (printable)", comma(b.trades()));
    row("open", px(b.open()));
    row("high", px(b.high()));
    row("low", px(b.low()));
    row("close", px(b.close()));
    row("vwap", px_double(b.vwap(), b.volume() > 0));
    rule();
    row("resting orders", comma(b.resting_orders()));
    row("resting shares", comma(b.resting_shares()));
    row("best bid", px(bid));
    row("best ask", px(ask));
    row("book crossed", b.crossed() ? "YES - BUG" : "no");
    row("last system event", b.system_event() == '\0' ? "-" : std::string(1, b.system_event()));
    row("trading state", b.trading_state() == '\0' ? "-" : std::string(1, b.trading_state()));
    if (r.out != nullptr) {
        rule();
        row("snapshots written", comma(r.written) + " -> " + r.opt.snapshots);
    }
}

bool parse_args(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--symbol") {
            opt->symbol = next("--symbol");
        } else if (a == "--snapshots") {
            opt->snapshots = next("--snapshots");
        } else if (a == "--interval-ms") {
            double ms = std::atof(next("--interval-ms"));
            if (ms <= 0) { std::fprintf(stderr, "error: --interval-ms must be positive\n"); return false; }
            opt->interval_ns = static_cast<uint64_t>(ms * 1e6);
        } else if (a == "--levels") {
            long v = std::atol(next("--levels"));
            if (v <= 0) { std::fprintf(stderr, "error: --levels must be positive\n"); return false; }
            opt->levels = static_cast<size_t>(v);
        } else if (a == "--limit") {
            opt->limit = std::strtoull(next("--limit"), nullptr, 10);
        } else if (a == "--tick") {
            long v = std::atol(next("--tick"));
            if (v <= 0) { std::fprintf(stderr, "error: --tick must be positive\n"); return false; }
            opt->tick = static_cast<int32_t>(v);
        } else if (a == "--end-ns") {
            opt->end_ns = std::strtoull(next("--end-ns"), nullptr, 10);
        } else if (a == "--quiet") {
            opt->quiet = true;
        } else if (a == "-h" || a == "--help") {
            return false;
        } else if (opt->feed == nullptr) {
            opt->feed = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]);
            return false;
        }
    }
    return opt->feed != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) {
        std::fprintf(stderr,
                     "usage: %s <feed.gz> [--symbol SYM] [--snapshots out.csv]\n"
                     "           [--interval-ms N] [--levels N] [--limit N]\n"
                     "           [--tick N] [--end-ns N] [--quiet]\n",
                     argv[0]);
        return 2;
    }

    Replayer r(opt);
    try {
        if (opt.snapshots != nullptr) {
            r.out = std::fopen(opt.snapshots, "w");
            if (r.out == nullptr) {
                std::fprintf(stderr, "error: cannot open %s for writing\n", opt.snapshots);
                return 1;
            }
            write_header(r.out, opt.levels);
        }

        itchbook::Reader reader(opt.feed);
        itchbook::parse(reader, r);

        if (r.out != nullptr) {
            std::fclose(r.out);
        }
        if (!opt.symbol.empty() && !r.locate_known) {
            std::fprintf(stderr, "error: symbol '%s' not found in Stock Directory\n",
                         opt.symbol.c_str());
            return 1;
        }
        if (!opt.quiet) print_summary(r);
    } catch (const std::exception& e) {
        if (r.out != nullptr) std::fclose(r.out);
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

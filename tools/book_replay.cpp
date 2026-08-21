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
//
// --all-symbols replays every security in the file into a BookSet instead, and
// writes one summary row per symbol. It is a separate handler on purpose: the
// single-symbol path above carries a byte-identical regression gate, and the
// safest way to keep that promise is to leave its code alone.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"
#include "itchbook/book/book_set.hpp"
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
    const char* json = nullptr;
    uint64_t interval_ns = 60ULL * 1000 * 1000 * 1000;   // 1 minute
    size_t levels = 10;
    uint64_t limit = 0;         // 0 = no limit
    uint64_t end_ns = 0;        // 0 = run to the end of the file
    int32_t tick = 100;         // a penny, in Price(4) units
    bool quiet = false;
    bool all_symbols = false;
    const char* per_symbol = nullptr;   // one row per security
    size_t refs_capacity = size_t{1} << 22;
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

// ---- every symbol at once ----------------------------------------------------
//
// The single-symbol Replayer above filters the feed down to one locate and
// snapshots it through the session. This does neither: it routes every modelled
// message to its own book and reports where each one ended up. Snapshotting
// 8,700 books on a one-second grid would write more output than the input file.
struct AllSymbols {
    Options opt;
    itchbook::book::BookSet set;
    uint64_t read = 0;
    uint64_t applied = 0;
    bool stop = false;

    explicit AllSymbols(const Options& o)
        : opt(o), set(o.refs_capacity, o.tick) {}

    void on_message(char type, const uint8_t* p, uint16_t) {
        if (stop) return;
        ++read;
        if (opt.limit != 0 && read > opt.limit) {
            --read;
            stop = true;
            return;
        }
        const uint64_t ts = itchbook::itch::timestamp(p);
        if (opt.end_ns != 0 && ts >= opt.end_ns && ts > 0) {
            --read;
            stop = true;
            return;
        }
        if (itchbook::book::apply(set, type, p)) ++applied;
    }
};

// One row per security, with the fields a single-symbol run also reports, so
// that a row can be diffed against `--symbol X` on the same feed. That
// comparison is the only thing standing between "the routing works" and "the
// routing appears to work": every book here runs the same code as before, so
// if a symbol's numbers change, it is the routing that changed them.
bool write_per_symbol(const AllSymbols& a, const char* path) {
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "error: cannot write %s\n", path);
        return false;
    }
    std::fputs("locate,symbol,directoried,resting_orders,resting_shares,volume,notional,"
               "trades,hidden_volume,cross_volume,open,high,low,close,best_bid,best_ask,"
               "unknown_refs,locate_mismatch,overflow_levels,trading_state,system_event,"
               "operational_halts,broken_trades,tradable\n", f);
    a.set.for_each_book([&](uint16_t locate, const itchbook::book::Book& b,
                            const itchbook::book::SymbolInfo& info) {
        int32_t bid = 0;
        int32_t ask = 0;
        const bool have_bid = b.best_bid(&bid);
        const bool have_ask = b.best_ask(&ask);
        // Absent is empty, not a sentinel. The single-symbol summary learned
        // this the hard way on a real day (see write_json): a -1 read as a
        // price manufactures a disagreement out of two spellings of "there
        // isn't one", and this file exists to be compared against that one.
        auto opt_px = [](int32_t v) { return v < 0 ? std::string() : std::to_string(v); };
        std::fprintf(f,
                     "%u,%s,%s,%zu,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                     ",%" PRIu64 ",%s,%s,%s,%s,%s,%s,%" PRIu64 ",%" PRIu64 ",%zu,%c,%c"
                     ",%" PRIu64 ",%" PRIu64 ",%s\n",
                     locate, info.symbol, info.directoried ? "yes" : "no",
                     b.resting_orders(), b.resting_shares(), b.volume(), b.notional(),
                     b.trades(), b.hidden_volume(), b.cross_volume(),
                     opt_px(b.open()).c_str(), opt_px(b.high()).c_str(),
                     opt_px(b.low()).c_str(), opt_px(b.close()).c_str(),
                     (have_bid ? std::to_string(bid) : std::string()).c_str(),
                     (have_ask ? std::to_string(ask) : std::string()).c_str(),
                     b.unknown_ref(), b.locate_mismatch(), b.overflow_levels(),
                     b.trading_state() == 0 ? '-' : b.trading_state(),
                     b.system_event() == 0 ? '-' : b.system_event(),
                     info.operational_halts, info.broken_trades,
                     a.set.tradable(locate) ? "yes" : "no");
    });
    const bool bad = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !bad;
}

void print_all_summary(const AllSymbols& a) {
    uint64_t volume = 0;
    uint64_t resting_shares = 0;
    size_t quoted = 0;
    size_t overflow_symbols = 0;
    a.set.for_each_book([&](uint16_t, const itchbook::book::Book& b,
                            const itchbook::book::SymbolInfo&) {
        volume += b.volume();
        resting_shares += b.resting_shares();
        if (b.resting_orders() > 0) ++quoted;
        if (b.overflow_levels() > 0) ++overflow_symbols;
    });
    std::printf("%-28s %16s\n", "messages read", comma(a.read).c_str());
    std::printf("%-28s %16s\n", "messages applied", comma(a.applied).c_str());
    std::printf("%-28s %16s\n", "books built", comma(a.set.books()).c_str());
    std::printf("%-28s %16s\n", "directory entries", comma(a.set.directory_entries()).c_str());
    std::printf("%-28s %16s\n", "symbols still quoting", comma(quoted).c_str());
    std::printf("%-28s %16s\n", "executed volume (all symbols)", comma(volume).c_str());
    std::printf("%-28s %16s\n", "resting shares", comma(resting_shares).c_str());
    std::printf("%-28s %16s\n", "resting orders", comma(a.set.resting_orders()).c_str());
    std::printf("%-28s %16s\n", "ref map slots", comma(a.set.storage().refs.capacity()).c_str());
    std::printf("%-28s %16s\n", "pool capacity (orders)", comma(a.set.storage().pool.capacity()).c_str());
    std::printf("%-28s %16s\n", "symbols using overflow", comma(overflow_symbols).c_str());
    // The three that must be zero on a feed that is what it claims to be.
    std::printf("%-28s %16s\n", "unknown references", comma(a.set.unknown_ref()).c_str());
    std::printf("%-28s %16s\n", "locate mismatches", comma(a.set.locate_mismatch()).c_str());
    std::printf("%-28s %16s\n", "undirectoried messages",
                comma(a.set.undirectoried_messages()).c_str());
    // The three the book does not model and cannot ignore. Reported whether or
    // not any occurred: a zero here says the constants they are parsed with are
    // still unconfirmed against real bytes, which is a fact about the run.
    std::printf("%-28s %16s\n", "operational halts ('h')",
                comma(a.set.operational_halts()).c_str());
    std::printf("%-28s %16s\n", "  symbols halted at close",
                comma(a.set.symbols_operationally_halted()).c_str());
    std::printf("%-28s %16s\n", "broken trades ('B')",
                comma(a.set.broken_trades()).c_str());
    std::printf("%-28s %16c\n", "MWCB level breached ('W')",
                a.set.mwcb_level_breached() == 0 ? '-' : a.set.mwcb_level_breached());
    std::printf("%-28s %16c\n", "last system event",
                a.set.system_event() == 0 ? '-' : a.set.system_event());
}

// The same fields the Python oracle writes, with the same names and the same
// types, so the two can be compared key by key instead of eyeballed. The
// snapshot CSV proves the two books agree at the instants it samples; this
// proves they agree on everything cumulative in between, which is a different
// failure and the one a sampled comparison cannot see.
void write_json(const Replayer& r, const char* path) {
    const auto& b = r.book;
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "error: cannot write %s\n", path);
        return;
    }
    // An absent touch is null, not a sentinel. The Python oracle writes None
    // and JSON has a way to say "there isn't one", so a -1 here would be the
    // comparison harness inventing a disagreement out of two spellings of the
    // same fact. It did: on MSFT the book is empty at the close — every order
    // cancelled — and the two implementations were reported as differing on
    // best_bid and best_ask while agreeing there was no bid and no ask.
    //
    // Only a real day shows this. A generated feed ends with orders still
    // resting, so both sides print a price and match.
    int32_t bid = 0;
    int32_t ask = 0;
    const bool have_bid = b.best_bid(&bid);
    const bool have_ask = b.best_ask(&ask);

    std::fprintf(f, "{\n");
    if (have_ask) {
        std::fprintf(f, "  \"best_ask\": %" PRId32 ",\n", ask);
    } else {
        std::fprintf(f, "  \"best_ask\": null,\n");
    }
    if (have_bid) {
        std::fprintf(f, "  \"best_bid\": %" PRId32 ",\n", bid);
    } else {
        std::fprintf(f, "  \"best_bid\": null,\n");
    }
    // OHLC and VWAP are undefined until something trades, and the book marks
    // that with -1. The oracle writes null. Every one of these is the same
    // mistake as best_bid: a sentinel is not a value, and a comparison that
    // treats it as one manufactures a disagreement between two implementations
    // that agree there is nothing to report.
    auto price_field = [&f](const char* key, int32_t v) {
        if (v < 0) {
            std::fprintf(f, "  \"%s\": null,\n", key);
        } else {
            std::fprintf(f, "  \"%s\": %" PRId32 ",\n", key, v);
        }
    };
    price_field("close", b.close());
    std::fprintf(f, "  \"cross_prices\": {");
    bool first = true;
    for (const auto& kv : b.cross_prices()) {
        std::fprintf(f, "%s\"%c\": %" PRId32, first ? "" : ", ", kv.first, kv.second);
        first = false;
    }
    std::fprintf(f, "},\n");
    std::fprintf(f, "  \"cross_volume\": %" PRIu64 ",\n", b.cross_volume());
    std::fprintf(f, "  \"crossed\": %s,\n", b.strictly_crossed() ? "true" : "false");
    std::fprintf(f, "  \"hidden_volume\": %" PRIu64 ",\n", b.hidden_volume());
    price_field("high", b.high());
    price_field("low", b.low());
    std::fprintf(f, "  \"messages_applied\": %" PRIu64 ",\n", r.applied);
    std::fprintf(f, "  \"messages_read\": %" PRIu64 ",\n", r.read);
    std::fprintf(f, "  \"notional\": %" PRIu64 ",\n", b.notional());
    price_field("open", b.open());
    std::fprintf(f, "  \"resting_orders\": %zu,\n", b.resting_orders());
    std::fprintf(f, "  \"resting_shares\": %" PRIu64 ",\n", b.resting_shares());
    std::fprintf(f, "  \"snapshots_written\": %" PRIu64 ",\n", r.written);
    // Same principle as the touch, and the same trap: these are '\0' until the
    // feed says otherwise, and printing that raw emits a NUL byte inside a JSON
    // string — not a wrong value, an unparseable file. The oracle writes null.
    auto char_field = [&f](const char* key, char c, bool comma) {
        if (c == '\0') {
            std::fprintf(f, "  \"%s\": null%s\n", key, comma ? "," : "");
        } else {
            std::fprintf(f, "  \"%s\": \"%c\"%s\n", key, c, comma ? "," : "");
        }
    };
    char_field("system_event", b.system_event(), true);
    std::fprintf(f, "  \"trades\": %" PRIu64 ",\n", b.trades());
    char_field("trading_state", b.trading_state(), true);
    std::fprintf(f, "  \"unknown_refs\": %" PRIu64 ",\n", b.unknown_ref());
    std::fprintf(f, "  \"volume\": %" PRIu64 ",\n", b.volume());
    if (b.volume() == 0) {
        std::fprintf(f, "  \"vwap\": null\n");   // no trades, no average
    } else {
        std::fprintf(f, "  \"vwap\": %.10f\n", b.vwap());
    }
    std::fprintf(f, "}\n");
    std::fclose(f);
}

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
        if (a == "--all-symbols") {
            opt->all_symbols = true;
        } else if (a == "--per-symbol") {
            opt->per_symbol = next("--per-symbol");
            opt->all_symbols = true;
        } else if (a == "--refs-capacity") {
            opt->refs_capacity = static_cast<size_t>(std::strtoull(next("--refs-capacity"), nullptr, 10));
        } else if (a == "--symbol") {
            opt->symbol = next("--symbol");
        } else if (a == "--snapshots") {
            opt->snapshots = next("--snapshots");
        } else if (a == "--json") {
            opt->json = next("--json");
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
                     "           [--tick N] [--end-ns N] [--json out.json] [--quiet]\n"
                     "       %s <feed.gz> --all-symbols [--per-symbol out.csv]\n"
                     "           [--refs-capacity N] [--limit N] [--tick N] [--quiet]\n",
                     argv[0], argv[0]);
        return 2;
    }

    if (opt.all_symbols) {
        if (!opt.symbol.empty()) {
            std::fprintf(stderr, "error: --symbol and --all-symbols ask different questions\n");
            return 2;
        }
        if (opt.snapshots != nullptr) {
            std::fprintf(stderr, "error: --snapshots is single-symbol; 8,700 books on a "
                                 "one-second grid outweighs the feed\n");
            return 2;
        }
        try {
            AllSymbols a(opt);
            itchbook::Reader reader(opt.feed);
            itchbook::parse(reader, a);
            if (!opt.quiet) print_all_summary(a);
            if (opt.per_symbol != nullptr && !write_per_symbol(a, opt.per_symbol)) return 1;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
        return 0;
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
        if (opt.json != nullptr) write_json(r, opt.json);
    } catch (const std::exception& e) {
        if (r.out != nullptr) std::fclose(r.out);
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

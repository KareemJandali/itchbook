// itch_census — count messages by type across a whole day.
//
// A healthy NASDAQ day is mostly A / D / X / E, a few thousand R at the top,
// and a small fixed number of S. Anything wildly off means framing is broken.
//
// The default pass builds nothing: it decompresses, frames, length-checks and
// counts. That is what makes it the cheapest check in the repository and the
// floor for any end-to-end timing — time it, and no all-symbols replay can
// ever beat that number.
//
// Two opt-in passes size the phase 9 work, and both are opt-in for the same
// reason: they cost memory and time that the framing check does not, and the
// framing check is the one that gets run casually.
//
//   --peak-orders        high-water mark of orders resting simultaneously,
//                        across every symbol. This is what the shared RefMap
//                        has to be sized from, and there is no honest way to
//                        get it except to count.
//   --per-symbol FILE    per-locate JSON: message counts, quoted and printed
//                        price ranges, crosses, and the two message types that
//                        can make a vendor's daily bar legitimately disagree
//                        with ours.
//   --timing FILE        just the wall clock and the sizes, as JSON. Works with
//                        any combination of the above, including none of them,
//                        which is the only way to record the floor: the cost of
//                        decompressing, framing and length-checking the file
//                        while building nothing at all.
//
// Usage:  itch_census <file.gz> [--peak-orders] [--per-symbol out.json]
//                     [--timing out.json]
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/itch/live_orders.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"

namespace {

// ---- per-locate ---------------------------------------------------------------

struct PerSymbol {
    char symbol[9] = {0};
    bool directoried = false;
    uint64_t messages = 0;
    uint64_t adds = 0, executions = 0, cancels = 0, deletes = 0, replaces = 0;
    uint64_t trades = 0, crosses = 0;
    uint64_t opening_crosses = 0, closing_crosses = 0;
    uint64_t halts = 0;          // 'H' — stock trading action
    uint64_t op_halts = 0;       // 'h' — operational halt, NOT modelled
    uint64_t broken = 0;         // 'B' — broken trade, NOT modelled
    uint64_t executed_shares = 0;
    // Quoted prices: A / F / U.
    //
    // These turned out to be almost useless for sizing anything, and finding
    // out why was worth the pass. On the real day, 77.6% of symbols that quoted
    // at all posted an order at or above $100,000 and 80.2% posted one at or
    // below $0.01 — clustered on $199,999.99, $199,999.00 and $100,000.00.
    // Those are STUB QUOTES: a market maker with a two-sided quoting
    // obligation parks an order where it will never fill. So the min and max of
    // what a symbol quoted spans essentially the whole price axis for three
    // symbols in four, and says nothing about where its book actually lives.
    //
    // They are kept because that fact is the finding, and because the count of
    // symbols hitting the extremes is how you show it.
    int32_t first_price = -1, last_price = -1, min_price = -1, max_price = -1;

    // Printed prices: C / P / Q. Stub quotes do not trade, so this is the
    // range the symbol's book actually occupied — which is what the dense band
    // has to cover, and the only per-symbol scale in this file that a band
    // policy can be built on.
    int32_t first_trade = -1, last_trade = -1, min_trade = -1, max_trade = -1;
    uint64_t prints = 0;
};

void note_trade(PerSymbol& s, int32_t px) {
    if (px <= 0) return;
    ++s.prints;
    if (s.first_trade < 0) s.first_trade = px;
    s.last_trade = px;
    if (s.min_trade < 0 || px < s.min_trade) s.min_trade = px;
    if (s.max_trade < 0 || px > s.max_trade) s.max_trade = px;
}

void note_price(PerSymbol& s, int32_t px) {
    if (px <= 0) return;
    if (s.first_price < 0) s.first_price = px;
    s.last_price = px;
    if (s.min_price < 0 || px < s.min_price) s.min_price = px;
    if (s.max_price < 0 || px > s.max_price) s.max_price = px;
}

std::string trim(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// ---- the handler ---------------------------------------------------------------

struct Census {
    std::array<uint64_t, 256> counts{};
    bool track_live = false;
    bool track_symbols = false;
    itchbook::itch::LiveOrders live;
    std::vector<PerSymbol> sym;
    uint64_t adds = 0, deletes = 0, replaces = 0;
    uint64_t full_executions = 0, partial_executions = 0;
    uint64_t full_cancels = 0, partial_cancels = 0;

    void on_message(char type, const uint8_t* p, uint16_t) {
        namespace m = itchbook::itch;
        ++counts[static_cast<uint8_t>(type)];
        if (!track_live && !track_symbols) return;

        PerSymbol* s = nullptr;
        if (track_symbols) {
            s = &sym[m::stock_locate(p)];
            ++s->messages;
        }

        switch (type) {
            case 'R':
                if (s != nullptr) {
                    s->directoried = true;
                    std::memcpy(s->symbol, m::stock_directory::stock(p), 8);
                    s->symbol[8] = '\0';
                }
                break;
            case 'A':
            case 'F': {
                const uint64_t ref = type == 'A' ? m::add_order::ref(p) : m::add_order_mpid::ref(p);
                const uint32_t sh = type == 'A' ? m::add_order::shares(p)
                                                : m::add_order_mpid::shares(p);
                const int32_t px = type == 'A' ? m::add_order::price(p)
                                               : m::add_order_mpid::price(p);
                if (track_live) { live.insert(ref, sh); ++adds; }
                if (s != nullptr) { ++s->adds; note_price(*s, px); }
                break;
            }
            case 'E': {
                const uint32_t sh = m::order_executed::executed_shares(p);
                if (track_live) {
                    if (live.reduce(m::order_executed::ref(p), sh)) ++full_executions;
                    else ++partial_executions;
                }
                if (s != nullptr) { ++s->executions; s->executed_shares += sh; }
                break;
            }
            case 'C': {
                const uint32_t sh = m::order_executed_price::executed_shares(p);
                if (track_live) {
                    if (live.reduce(m::order_executed_price::ref(p), sh)) ++full_executions;
                    else ++partial_executions;
                }
                if (s != nullptr) {
                    ++s->executions;
                    s->executed_shares += sh;
                    // 'C' states its own print price, which is not the resting
                    // order's price. 'E' does not carry one at all — it trades
                    // at a price only the book knows — so a census cannot see
                    // it, and that limitation belongs in the record rather than
                    // in a footnote nobody reads.
                    if (m::order_executed_price::printable(p)) {
                        note_trade(*s, m::order_executed_price::price(p));
                    }
                }
                break;
            }
            case 'X': {
                if (track_live) {
                    if (live.reduce(m::order_cancel::ref(p), m::order_cancel::canceled_shares(p)))
                        ++full_cancels;
                    else
                        ++partial_cancels;
                }
                if (s != nullptr) ++s->cancels;
                break;
            }
            case 'D':
                if (track_live) { live.erase(m::order_delete::ref(p)); ++deletes; }
                if (s != nullptr) ++s->deletes;
                break;
            case 'U':
                if (track_live) {
                    live.erase(m::order_replace::original_ref(p));
                    live.insert(m::order_replace::new_ref(p), m::order_replace::shares(p));
                    ++replaces;
                }
                if (s != nullptr) { ++s->replaces; note_price(*s, m::order_replace::price(p)); }
                break;
            case 'P':
                if (s != nullptr) { ++s->trades; note_trade(*s, m::trade::price(p)); }
                break;
            case 'Q':
                if (s != nullptr) {
                    ++s->crosses;
                    note_trade(*s, m::cross_trade::price(p));
                    const char kind = m::cross_trade::cross_type(p);
                    if (kind == 'O') ++s->opening_crosses;
                    if (kind == 'C') ++s->closing_crosses;
                }
                break;
            case 'H':
                if (s != nullptr) ++s->halts;
                break;
            case 'h':
                if (s != nullptr) ++s->op_halts;
                break;
            case 'B':
                if (s != nullptr) ++s->broken;
                break;
            default:
                break;
        }
    }
};

const char* type_name(char t) {
    switch (t) {
        case 'S': return "System Event";
        case 'R': return "Stock Directory";
        case 'H': return "Stock Trading Action";
        case 'Y': return "Reg SHO Restriction";
        case 'L': return "Market Participant Position";
        case 'A': return "Add Order";
        case 'F': return "Add Order w/ MPID";
        case 'E': return "Order Executed";
        case 'C': return "Order Executed w/ Price";
        case 'X': return "Order Cancel";
        case 'D': return "Order Delete";
        case 'U': return "Order Replace";
        case 'P': return "Trade (non-cross)";
        case 'Q': return "Cross Trade";
        case 'B': return "Broken Trade";
        case 'I': return "NOII";
        case 'h': return "Operational Halt";
        case 'V': return "MWCB Decline Level";
        case 'W': return "MWCB Status";
        case 'K': return "IPO Quoting Period Update";
        case 'J': return "LULD Auction Collar";
        case 'N': return "Retail Price Improvement";
        case 'O': return "Direct Listing w/ Capital Raise";
        default:  return "";
    }
}

void print_price(std::FILE* f, const char* key, int32_t px) {
    if (px < 0) std::fprintf(f, "\"%s\":null", key);
    else std::fprintf(f, "\"%s\":%.4f", key, static_cast<double>(px) / 10000.0);
}

// The size of the file on disk, which is not the size of the feed. Recorded
// next to it because the ratio between them is what any decompression argument
// rests on, and a number nobody wrote down is a number that gets guessed.
uint64_t file_size(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return 0;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n < 0 ? 0 : static_cast<uint64_t>(n);
}

// What every pass can report, whatever else it was asked to do. Separated out
// because the floor -- decompress, frame, length-check, build nothing -- is
// measured by the pass with NO other flags, and that pass has nothing else to
// write. A number with no artifact is a number that gets retyped.
bool write_timing(const std::string& path, const std::string& file, uint64_t total,
                  uint64_t bytes, double elapsed_s, const char* pass) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n  \"file\": \"%s\",\n  \"pass\": \"%s\",\n"
                    "  \"messages\": %llu,\n  \"bytes\": %llu,\n"
                    "  \"compressed_bytes\": %llu,\n  \"elapsed_seconds\": %.2f\n}\n",
                 file.c_str(), pass,
                 static_cast<unsigned long long>(total),
                 static_cast<unsigned long long>(bytes),
                 static_cast<unsigned long long>(file_size(file)), elapsed_s);
    const bool bad = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !bad;
}

bool write_per_symbol(const std::string& path, const std::string& file, uint64_t total,
                      uint64_t bytes, double elapsed_s, const Census& c) {
    auto c_counts = [&c](int i) { return c.counts[static_cast<size_t>(i)]; };
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return false;
    std::fprintf(f, "{\n  \"file\": \"%s\",\n", file.c_str());
    std::fprintf(f, "  \"messages\": %llu,\n", static_cast<unsigned long long>(total));
    std::fprintf(f, "  \"bytes\": %llu,\n", static_cast<unsigned long long>(bytes));
    std::fprintf(f, "  \"compressed_bytes\": %llu,\n",
                 static_cast<unsigned long long>(file_size(file)));
    // Written by the tool rather than transcribed from `time`, for the same
    // reason every other number here is: a figure that passes through a human
    // and a keyboard is a figure that can drift from its run.
    std::fprintf(f, "  \"elapsed_seconds\": %.2f,\n", elapsed_s);
    std::fprintf(f, "  \"pass\": \"%s\",\n",
                 c.track_live ? "framing + live-order tracking" : "framing only");
    if (c.track_live) {
        std::fprintf(f, "  \"live_orders\": {\"peak\": %llu, \"final\": %llu, \"adds\": %llu, "
                        "\"deletes\": %llu, \"replaces\": %llu, \"full_executions\": %llu, "
                        "\"partial_executions\": %llu, \"full_cancels\": %llu, "
                        "\"partial_cancels\": %llu, \"unknown_refs\": %llu},\n",
                     static_cast<unsigned long long>(c.live.peak()),
                     static_cast<unsigned long long>(c.live.size()),
                     static_cast<unsigned long long>(c.adds),
                     static_cast<unsigned long long>(c.deletes),
                     static_cast<unsigned long long>(c.replaces),
                     static_cast<unsigned long long>(c.full_executions),
                     static_cast<unsigned long long>(c.partial_executions),
                     static_cast<unsigned long long>(c.full_cancels),
                     static_cast<unsigned long long>(c.partial_cancels),
                     static_cast<unsigned long long>(c.live.unknown()));
    }
    // The type histogram, so that a later run can check its own arithmetic
    // against this one. "messages read minus messages applied" has to equal the
    // unmodelled count plus the modelled-but-non-mutating types, and there is
    // no way to check that without knowing how many of each there were.
    std::fprintf(f, "  \"types\": {");
    bool first_type = true;
    for (int c = 0; c < 256; ++c) {
        if (c_counts(c) == 0) continue;
        if (!first_type) std::fprintf(f, ", ");
        first_type = false;
        std::fprintf(f, "\"%c\": %llu", c, static_cast<unsigned long long>(c_counts(c)));
    }
    std::fprintf(f, "},\n");
    std::fprintf(f, "  \"symbols\": [\n");
    bool first = true;
    for (size_t loc = 0; loc < c.sym.size(); ++loc) {
        const PerSymbol& s = c.sym[loc];
        if (s.messages == 0) continue;
        if (!first) std::fprintf(f, ",\n");
        first = false;
        std::fprintf(f, "    {\"locate\":%zu,\"symbol\":\"%s\",\"directoried\":%s,"
                        "\"messages\":%llu,\"adds\":%llu,\"executions\":%llu,\"cancels\":%llu,"
                        "\"deletes\":%llu,\"replaces\":%llu,\"trades\":%llu,\"crosses\":%llu,"
                        "\"opening_crosses\":%llu,\"closing_crosses\":%llu,\"halts\":%llu,"
                        "\"operational_halts\":%llu,\"broken_trades\":%llu,"
                        "\"executed_shares\":%llu,",
                     loc, trim(reinterpret_cast<const uint8_t*>(s.symbol), 8).c_str(),
                     s.directoried ? "true" : "false",
                     static_cast<unsigned long long>(s.messages),
                     static_cast<unsigned long long>(s.adds),
                     static_cast<unsigned long long>(s.executions),
                     static_cast<unsigned long long>(s.cancels),
                     static_cast<unsigned long long>(s.deletes),
                     static_cast<unsigned long long>(s.replaces),
                     static_cast<unsigned long long>(s.trades),
                     static_cast<unsigned long long>(s.crosses),
                     static_cast<unsigned long long>(s.opening_crosses),
                     static_cast<unsigned long long>(s.closing_crosses),
                     static_cast<unsigned long long>(s.halts),
                     static_cast<unsigned long long>(s.op_halts),
                     static_cast<unsigned long long>(s.broken),
                     static_cast<unsigned long long>(s.executed_shares));
        print_price(f, "first_price", s.first_price);  std::fprintf(f, ",");
        print_price(f, "last_price", s.last_price);    std::fprintf(f, ",");
        print_price(f, "min_price", s.min_price);      std::fprintf(f, ",");
        print_price(f, "max_price", s.max_price);      std::fprintf(f, ",");
        std::fprintf(f, "\"prints\":%llu,", static_cast<unsigned long long>(s.prints));
        print_price(f, "first_trade", s.first_trade);  std::fprintf(f, ",");
        print_price(f, "last_trade", s.last_trade);    std::fprintf(f, ",");
        print_price(f, "min_trade", s.min_trade);      std::fprintf(f, ",");
        print_price(f, "max_trade", s.max_trade);
        std::fprintf(f, "}");
    }
    std::fprintf(f, "\n  ]\n}\n");
    const bool bad = std::ferror(f) != 0;
    return std::fclose(f) == 0 && !bad;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string per_symbol_out;
    std::string timing_out;
    bool peak_orders = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--peak-orders") {
            peak_orders = true;
        } else if (a == "--per-symbol" && i + 1 < argc) {
            per_symbol_out = argv[++i];
        } else if (a == "--timing" && i + 1 < argc) {
            timing_out = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::printf("usage: %s <file.gz> [--peak-orders] [--per-symbol out.json]"
                        " [--timing out.json]\n", argv[0]);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return 2;
        } else if (path.empty()) {
            path = a;
        } else {
            std::fprintf(stderr, "error: unexpected argument %s\n", a.c_str());
            return 2;
        }
    }
    if (path.empty()) {
        std::fprintf(stderr, "usage: %s <file.gz> [--peak-orders] [--per-symbol out.json]"
                             " [--timing out.json]\n", argv[0]);
        return 2;
    }

    try {
        const auto started = std::chrono::steady_clock::now();
        itchbook::Reader reader(path);
        Census census;
        census.track_live = peak_orders;
        census.track_symbols = !per_symbol_out.empty();
        // 65,536 locates is the whole uint16 space the wire can address. At
        // ~120 bytes each that is 8 MB, which is cheaper than deciding how many
        // symbols a day is allowed to have.
        if (census.track_symbols) census.sym.assign(65536, PerSymbol{});
        uint64_t total = itchbook::parse(reader, census);
        const double elapsed_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();

        std::printf("%-5s %-28s %14s  %s\n", "type", "name", "count", "modelled");
        std::printf("---------------------------------------------------------------\n");
        uint64_t ignored = 0;
        for (int c = 0; c < 256; ++c) {
            if (census.counts[c] == 0) continue;
            const char t = static_cast<char>(c);
            const bool known = itchbook::itch::modelled(t);
            if (!known) ignored += census.counts[c];
            std::printf("%-5c %-28s %14llu  %s\n", c, type_name(t),
                        static_cast<unsigned long long>(census.counts[c]),
                        known ? "yes" : "no");
        }
        std::printf("---------------------------------------------------------------\n");
        // A feed is not just what we read. Saying how much of it we ignored is
        // the difference between "this book handles the day" and "this book
        // handles the parts of the day it happens to know about".
        std::printf("%-34s %14llu\n", "messages NOT modelled",
                    static_cast<unsigned long long>(ignored));
        std::printf("%-34s %14llu\n", "TOTAL messages", static_cast<unsigned long long>(total));
        std::printf("%-34s %14llu\n", "TOTAL bytes", static_cast<unsigned long long>(reader.bytes()));
        // This pass decompresses, frames and length-checks the file and builds
        // nothing. Its wall clock is therefore the floor for any replay of the
        // same file: no end-to-end number can be below it, and the gap between
        // the two is what the book itself costs.
        std::printf("%-34s %14.2f\n", "seconds", elapsed_s);
        if (elapsed_s > 0.0) {
            std::printf("%-34s %14.1f\n", "MB/s (uncompressed)",
                        static_cast<double>(reader.bytes()) / elapsed_s / 1e6);
            std::printf("%-34s %14.2f\n", "M msg/s",
                        static_cast<double>(total) / elapsed_s / 1e6);
        }

        if (peak_orders) {
            std::printf("\nlive orders (every symbol, one shared reference space)\n");
            std::printf("---------------------------------------------------------------\n");
            std::printf("%-34s %14llu\n", "PEAK live orders",
                        static_cast<unsigned long long>(census.live.peak()));
            std::printf("%-34s %14llu\n", "live at end of file",
                        static_cast<unsigned long long>(census.live.size()));
            std::printf("%-34s %14llu\n", "adds (A+F)",
                        static_cast<unsigned long long>(census.adds));
            std::printf("%-34s %14llu\n", "replaces (U)",
                        static_cast<unsigned long long>(census.replaces));
            std::printf("%-34s %14llu\n", "deletes (D)",
                        static_cast<unsigned long long>(census.deletes));
            std::printf("%-34s %14llu\n", "executions that emptied an order",
                        static_cast<unsigned long long>(census.full_executions));
            std::printf("%-34s %14llu\n", "executions that did not",
                        static_cast<unsigned long long>(census.partial_executions));
            std::printf("%-34s %14llu\n", "cancels that emptied an order",
                        static_cast<unsigned long long>(census.full_cancels));
            std::printf("%-34s %14llu\n", "cancels that did not",
                        static_cast<unsigned long long>(census.partial_cancels));
            std::printf("%-34s %14llu\n", "references naming no live order",
                        static_cast<unsigned long long>(census.live.unknown()));
            std::printf("%-34s %14llu\n", "references inserted twice",
                        static_cast<unsigned long long>(census.live.duplicates()));
            // The identity, computed by a program that has never heard of a
            // price level. The book has to satisfy the same one, and if the two
            // ever disagree, neither of them gets to say which is right.
            std::printf("%-34s %14s\n", "inserted == live + removed + emptied",
                        census.live.accounts() ? "yes" : "NO — BUG");
        }

        const char* pass_name = peak_orders
            ? (census.track_symbols ? "framing + live orders + per symbol"
                                    : "framing + live orders")
            : (census.track_symbols ? "framing + per symbol" : "framing only");
        if (!timing_out.empty() &&
            !write_timing(timing_out, path, total, reader.bytes(), elapsed_s, pass_name)) {
            std::fprintf(stderr, "error: cannot write %s\n", timing_out.c_str());
            return 1;
        }

        if (census.track_symbols) {
            if (!write_per_symbol(per_symbol_out, path, total, reader.bytes(), elapsed_s,
                                  census)) {
                std::fprintf(stderr, "error: cannot write %s\n", per_symbol_out.c_str());
                return 1;
            }
            size_t seen = 0, directoried = 0, quoted = 0, with_close = 0;
            size_t traded = 0, stub_high = 0, stub_low = 0;
            // A stub quote is an order parked where it will never fill, to
            // satisfy a two-sided quoting obligation. There is no flag for one
            // on the wire; what identifies it is a price nothing could trade
            // at. These two counts are what make the point that a symbol's
            // quoted range cannot size a band.
            constexpr int32_t kAbsurdlyHigh = 1000000000;   // $100,000
            constexpr int32_t kAbsurdlyLow = 100;           // $0.01
            for (const PerSymbol& s : census.sym) {
                if (s.messages == 0) continue;
                ++seen;
                if (s.directoried) ++directoried;
                if (s.adds > 0) ++quoted;
                if (s.prints > 0) ++traded;
                if (s.closing_crosses > 0) ++with_close;
                if (s.adds > 0 && s.max_price >= kAbsurdlyHigh) ++stub_high;
                if (s.adds > 0 && s.min_price > 0 && s.min_price <= kAbsurdlyLow) ++stub_low;
            }
            std::printf("\nper-symbol -> %s\n", per_symbol_out.c_str());
            std::printf("---------------------------------------------------------------\n");
            std::printf("%-34s %14zu\n", "locates seen", seen);
            std::printf("%-34s %14zu\n", "with a stock directory entry", directoried);
            std::printf("%-34s %14zu\n", "that ever quoted an order", quoted);
            std::printf("%-34s %14zu\n", "that ever printed a trade", traded);
            std::printf("%-34s %14zu\n", "with a closing cross", with_close);
            std::printf("%-34s %14zu\n", "quoting at or above $100,000", stub_high);
            std::printf("%-34s %14zu\n", "quoting at or below $0.01", stub_low);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

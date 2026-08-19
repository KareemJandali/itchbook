// itch_census — count messages by type across a whole day.
//
// A healthy NASDAQ day is mostly A / D / X / E, a few thousand R at the top,
// and a small fixed number of S. Anything wildly off means framing is broken.
//
// Usage:  itch_census <file.gz>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"

namespace {

struct Census {
    std::array<uint64_t, 256> counts{};
    void on_message(char type, const uint8_t*, uint16_t) {
        ++counts[static_cast<uint8_t>(type)];
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
        default:  return "";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <file.gz>\n", argv[0]);
        return 2;
    }
    try {
        itchbook::Reader reader(argv[1]);
        Census census;
        uint64_t total = itchbook::parse(reader, census);

        std::printf("%-5s %-28s %14s\n", "type", "name", "count");
        std::printf("-------------------------------------------------------\n");
        for (int c = 0; c < 256; ++c) {
            if (census.counts[c] == 0) continue;
            std::printf("%-5c %-28s %14llu\n", c, type_name(static_cast<char>(c)),
                        static_cast<unsigned long long>(census.counts[c]));
        }
        std::printf("-------------------------------------------------------\n");
        std::printf("%-34s %14llu\n", "TOTAL messages", static_cast<unsigned long long>(total));
        std::printf("%-34s %14llu\n", "TOTAL bytes", static_cast<unsigned long long>(reader.bytes()));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

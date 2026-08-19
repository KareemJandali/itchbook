// itch_dump — print type, timestamp, and hex payload for the first N messages.
//
// Usage:  itch_dump <file.gz> [N]        (default N = 20)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"

namespace {

struct Dumper {
    uint64_t limit;
    uint64_t seen = 0;

    void on_message(char type, const uint8_t* p, uint16_t len) {
        if (seen >= limit) return;
        ++seen;

        // Timestamp lives at offset 5 in every message long enough to have one.
        std::printf("[%6llu] type=%c len=%-3u", static_cast<unsigned long long>(seen), type, len);
        if (len >= 11) {
            std::printf(" ts=%015llu",
                        static_cast<unsigned long long>(itchbook::itch::timestamp(p)));
        }
        std::printf("  ");
        for (uint16_t i = 0; i < len; ++i) {
            std::printf("%02x", p[i]);
        }
        std::printf("\n");
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <file.gz> [N]\n", argv[0]);
        return 2;
    }
    uint64_t n = (argc == 3) ? std::strtoull(argv[2], nullptr, 10) : 20;
    try {
        itchbook::Reader reader(argv[1]);
        Dumper dumper{n};
        // Stop early once we've printed N: throw a sentinel from the handler is
        // ugly, so just let it scan — N is small and the Reader is fast. For a
        // huge file you'd add an early-out to parse(); fine for Phase 1.
        itchbook::parse(reader, dumper);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

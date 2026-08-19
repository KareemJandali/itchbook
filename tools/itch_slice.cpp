// itch_slice — extract every message for one symbol into a small per-symbol file.
//
// Build this early: it turns a multi-GB full-day scan into a tiny file you can
// replay in milliseconds. Slow feedback loops are what kill projects like this.
//
// Two passes:
//   1. Read the 'R' (Stock Directory) block to map the target ticker to its
//      2-byte stock-locate code.
//   2. Re-scan the file, keeping only messages whose stock-locate matches, and
//      write them back out in the same [len][payload] framing (gzip'd).
//
// Usage:  itch_slice <in.gz> <SYMBOL> <out.gz>
#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"

namespace {

// Pass 1: find the stock-locate code for the requested symbol.
struct LocateFinder {
    std::string want;   // padded to 8 chars, space-filled
    uint16_t found = 0;
    bool ok = false;

    void on_message(char type, const uint8_t* p, uint16_t) {
        if (ok || type != 'R') return;
        // Stock Directory: type(1) locate(2) tracking(2) ts(6) stock(8 @ off 11)
        if (std::memcmp(p + 11, want.data(), 8) == 0) {
            found = itchbook::itch::stock_locate(p);
            ok = true;
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <in.gz> <SYMBOL> <out.gz>\n", argv[0]);
        return 2;
    }
    const char* in_path = argv[1];
    std::string symbol = argv[2];
    const char* out_path = argv[3];

    // ITCH stock fields are 8 chars, left-justified, space-padded.
    std::string padded = symbol;
    padded.resize(8, ' ');

    try {
        // ---- Pass 1 ----
        LocateFinder finder{padded};
        {
            itchbook::Reader reader(in_path);
            itchbook::parse(reader, finder);
        }
        if (!finder.ok) {
            std::fprintf(stderr, "error: symbol '%s' not found in Stock Directory\n", symbol.c_str());
            return 1;
        }
        std::printf("symbol %s -> stock_locate %u\n", symbol.c_str(), finder.found);

        // ---- Pass 2 ----
        gzFile out = gzopen(out_path, "wb");
        if (out == nullptr) {
            std::fprintf(stderr, "error: cannot open output %s\n", out_path);
            return 1;
        }

        struct Slicer {
            uint16_t target;
            gzFile out;
            uint64_t kept = 0;
            void on_message(char type, const uint8_t* p, uint16_t len) {
                // System events ('S') have no meaningful locate but bracket the
                // session — keep them so the slice still has open/close markers.
                bool keep = (type == 'S') || (itchbook::itch::stock_locate(p) == target);
                if (!keep) return;
                uint8_t lb[2] = {static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xff)};
                gzwrite(out, lb, 2);
                gzwrite(out, p, len);
                ++kept;
            }
        } slicer{finder.found, out};

        itchbook::Reader reader(in_path);
        itchbook::parse(reader, slicer);
        gzclose(out);

        std::printf("wrote %llu messages to %s\n",
                    static_cast<unsigned long long>(slicer.kept), out_path);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}

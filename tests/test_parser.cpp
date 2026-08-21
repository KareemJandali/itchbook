// test_parser — unit tests for the big-endian field readers and framing.
//
// Shared harness lives in tests/check.hpp.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/parser.hpp"
#include "itchbook/itch/reader.hpp"
#include "tests/check.hpp"

namespace {

// Frame messages the way the wire does — 2-byte big-endian length prefix — and
// write them to a plain file. gzopen reads uncompressed input transparently,
// so Reader takes this without a gzip step.
std::string write_feed(const std::vector<std::vector<uint8_t>>& msgs) {
    std::string path = "/tmp/itchbook_test_parser_feed.itch";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return "";
    for (const auto& m : msgs) {
        const auto n = static_cast<uint16_t>(m.size());
        const uint8_t hdr[2] = {static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n & 0xff)};
        std::fwrite(hdr, 1, 2, f);
        std::fwrite(m.data(), 1, m.size(), f);
    }
    std::fclose(f);
    return path;
}

struct CountByType {
    std::vector<char> seen;
    void on_message(char type, const uint8_t*, uint16_t) { seen.push_back(type); }
};

}  // namespace

int main() {
    using namespace itchbook::itch;

    // big-endian readers
    const uint8_t b16[] = {0x12, 0x34};
    CHECK(be16(b16) == 0x1234);

    const uint8_t b32[] = {0x12, 0x34, 0x56, 0x78};
    CHECK(be32(b32) == 0x12345678u);

    const uint8_t b48[] = {0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    CHECK(be48(b48) == 256u);

    const uint8_t b64[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00};
    CHECK(be64(b64) == 256u);

    // spec lengths — the modelled types
    CHECK(spec_length('A') == 36);
    CHECK(spec_length('D') == 19);
    CHECK(spec_length('S') == 12);

    // ...and the types this book frames but does not interpret. These are
    // written out as literals on purpose: the header holds the same numbers,
    // and a constant that only appears once is a constant nothing checks.
    CHECK(spec_length('Y') == 20);   // Reg SHO Restriction
    CHECK(spec_length('L') == 26);   // Market Participant Position
    CHECK(spec_length('V') == 35);   // MWCB Decline Level
    CHECK(spec_length('W') == 12);   // MWCB Status
    CHECK(spec_length('K') == 28);   // IPO Quoting Period Update
    CHECK(spec_length('J') == 35);   // LULD Auction Collar
    CHECK(spec_length('h') == 21);   // Operational Halt
    CHECK(spec_length('B') == 19);   // Broken Trade
    CHECK(spec_length('I') == 50);   // NOII
    CHECK(spec_length('N') == 20);   // Retail Price Improvement
    CHECK(spec_length('O') == 48);   // Direct Listing with Capital Raise

    // A type genuinely not in the spec must NOT get a guessed length: -1 means
    // "pass the frame through unchecked", which is the only safe answer.
    CHECK(spec_length('Z') == -1);
    CHECK(spec_length('\0') == -1);

    // modelled() is what separates "we read it" from "we merely framed it".
    CHECK(modelled('A'));
    CHECK(modelled('Q'));
    CHECK(modelled('H'));            // 'H' the trading action IS modelled...
    CHECK(!modelled('h'));           // ...'h' the operational halt is not.
    CHECK(!modelled('V'));
    CHECK(!modelled('W'));
    CHECK(!modelled('B'));
    CHECK(!modelled('O'));
    CHECK(!modelled('Z'));

    // Build a synthetic Add Order ('A', 36 bytes) and read its fields back.
    std::vector<uint8_t> a(36, 0);
    a[0] = 'A';
    // timestamp @5 = 1_000_000_000 ns
    uint64_t ts = 1'000'000'000ull;
    for (int i = 0; i < 6; ++i) a[5 + 5 - i] = static_cast<uint8_t>((ts >> (8 * i)) & 0xff);
    // ref @11 = 42
    a[11 + 7] = 42;
    a[19] = 'B';        // side
    a[23] = 100;        // shares (last byte of the 4-byte field)
    a[24] = 'T'; a[25] = 'E'; a[26] = 'S'; a[27] = 'T';
    for (int i = 28; i < 32; ++i) a[i] = ' ';
    a[35] = 0x64;       // price low byte = 100 -> $0.0100

    CHECK(timestamp(a.data()) == ts);
    CHECK(add_order::ref(a.data()) == 42u);
    CHECK(add_order::side(a.data()) == 'B');
    CHECK(add_order::shares(a.data()) == 100u);
    CHECK(add_order::price(a.data()) == 100);

    // ---- unmodelled types are framed, length-checked, and delivered --------
    //
    // The handler ignores them, but the parser must still see them: skipping a
    // metadata message means skipping the desync check on it, and a desync that
    // starts inside an 'h' is a desync all the same.
    {
        auto msg = [](char t, int len) {
            std::vector<uint8_t> m(static_cast<size_t>(len), 0);
            m[0] = static_cast<uint8_t>(t);
            return m;
        };
        const std::string path = write_feed({
            msg('A', 36), msg('h', 21), msg('V', 35), msg('W', 12),
            msg('B', 19), msg('D', 19),
        });
        CHECK(!path.empty());
        itchbook::Reader reader(path);
        CountByType h;
        const uint64_t n = itchbook::parse(reader, h);
        CHECK(n == 6);
        CHECK(h.seen.size() == 6);
        CHECK(h.seen[1] == 'h');
        CHECK(h.seen[2] == 'V');
        CHECK(h.seen[4] == 'B');
        std::remove(path.c_str());
    }

    // ...and a wrong length on one of them is a desync, not a shrug. Before
    // these types had spec lengths this frame passed straight through.
    {
        std::vector<uint8_t> bad(20, 0);          // 'h' is 21 bytes, not 20
        bad[0] = 'h';
        const std::string path = write_feed({bad});
        CHECK(!path.empty());
        itchbook::Reader reader(path);
        CountByType h;
        bool threw = false;
        try {
            itchbook::parse(reader, h);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        std::remove(path.c_str());
    }

    return REPORT();
}

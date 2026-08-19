// test_parser — unit tests for the big-endian field readers and framing.
//
// Tiny hand-rolled harness (no GoogleTest dependency yet — added in Phase 3).
// Exit code 0 = all passed, 1 = a failure.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "itchbook/itch/messages.hpp"

namespace {

int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

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

    // spec lengths
    CHECK(spec_length('A') == 36);
    CHECK(spec_length('D') == 19);
    CHECK(spec_length('S') == 12);
    CHECK(spec_length('Z') == -1);

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

    if (failures == 0) {
        std::printf("all tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d test(s) failed\n", failures);
    return 1;
}

// mold_damage — do to a packet file what a network does to a feed.
//
// The test instrument for recovery. Everything else in this repo has been
// verified against a feed where nothing went wrong, which verifies the happy
// path and nothing else. Recovery code that has never seen a gap is not code
// that recovers; it is code that compiles.
//
// Damage is applied at the PACKET level, which is the level at which it
// happens. Dropping a packet drops every message in it — around forty on a
// 1400-byte MTU — consecutively, from the middle of the book. Dropping
// individual messages would be gentler and would not be a thing that occurs.
//
// Usage:
//   mold_damage <in.gz> <out.gz> [--drop N] [--duplicate N] [--reorder N]
//               [--truncate N] [--disconnect-at SEQ --disconnect-for N]
//               [--seed N]
//
// --drop/--duplicate/--reorder/--truncate are one-in-N rates: --drop 1000
// drops roughly one packet in a thousand. --disconnect-at models the case that
// is not random at all — a link that goes away at a particular moment and
// comes back some packets later, which is what a mid-day outage looks like
// from inside the receiver.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <zlib.h>

#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/mold/packet.hpp"

using namespace itchbook;

namespace {

struct Out {
    gzFile f;
    uint64_t packets = 0;
    void write(const std::vector<uint8_t>& p) {
        uint8_t lb[2] = {static_cast<uint8_t>(p.size() >> 8),
                         static_cast<uint8_t>(p.size() & 0xff)};
        gzwrite(f, lb, 2);
        gzwrite(f, p.data(), static_cast<unsigned>(p.size()));
        ++packets;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* out_path = nullptr;
    uint64_t drop = 0, duplicate = 0, reorder = 0, truncate = 0;
    uint64_t disconnect_at = 0, disconnect_for = 0, disconnect_at_ns = 0;
    uint64_t halt_at_ns = 0, halt_for_ns = 60ULL * 1000000000ULL;
    unsigned seed = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto num = [&](uint64_t* dst) { *dst = std::strtoull(argv[++i], nullptr, 10); };
        if (a == "--drop" && i + 1 < argc) num(&drop);
        else if (a == "--duplicate" && i + 1 < argc) num(&duplicate);
        else if (a == "--reorder" && i + 1 < argc) num(&reorder);
        else if (a == "--truncate" && i + 1 < argc) num(&truncate);
        else if (a == "--disconnect-at" && i + 1 < argc) num(&disconnect_at);
        // The plan asks for "a mid-day disconnect at 14:00", which is a wall
        // clock time and not a sequence number. Expressing it as one keeps the
        // scenario faithful instead of making the harness guess which packet
        // 14:00 landed in.
        else if (a == "--disconnect-at-ns" && i + 1 < argc) num(&disconnect_at_ns);
        // The plan's fifth injection: a trading halt. Not damage in the sense
        // the others are — nothing is lost — but a state transition the whole
        // recovery stack has to keep straight, and one a real day may simply
        // not contain. MSFT did not halt on 30 December 2019.
        else if (a == "--halt-at-ns" && i + 1 < argc) num(&halt_at_ns);
        else if (a == "--halt-for-ns" && i + 1 < argc) num(&halt_for_ns);
        else if (a == "--disconnect-for" && i + 1 < argc) num(&disconnect_for);
        else if (a == "--seed" && i + 1 < argc) seed = static_cast<unsigned>(std::atol(argv[++i]));
        else if (in_path == nullptr) in_path = argv[i];
        else if (out_path == nullptr) out_path = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (in_path == nullptr || out_path == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <in.gz> <out.gz> [--drop N] [--duplicate N]\n"
                     "           [--reorder N] [--truncate N]\n"
                     "           [--disconnect-at SEQ | --disconnect-at-ns NS]\n"
                     "           [--disconnect-for N] [--halt-at-ns NS]\n"
                     "           [--halt-for-ns NS] [--seed N]\n"
                     "\nrates are one-in-N; --disconnect-at is a sequence number\n",
                     argv[0]);
        return 2;
    }

    gzFile out = gzopen(out_path, "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "error: cannot write %s\n", out_path);
        return 1;
    }
    Out w{out};
    std::mt19937_64 rng(seed);
    auto hit = [&](uint64_t rate) { return rate > 0 && (rng() % rate) == 0; };

    uint64_t dropped = 0, duped = 0, reordered = 0, truncated = 0, disconnected = 0;
    uint64_t halted = 0;
    // Injecting a message shifts every sequence number after it. Dropped
    // packets must still leave a real hole, so the stream cannot simply be
    // renumbered from one — instead every surviving packet's sequence is its
    // original plus however many messages have been injected ahead of it. Gaps
    // stay gaps; the injected messages take their own numbers.
    uint64_t seq_offset = 0;
    bool halt_sent = false;
    bool resume_sent = false;
    uint64_t read = 0;
    std::vector<uint8_t> held;          // one packet delayed, to reorder it
    bool holding = false;
    uint64_t disconnect_until = 0;      // packets remaining in the outage

    try {
        Reader reader(in_path);
        std::vector<uint8_t> packet;
        while (reader.next(packet)) {
            ++read;
            mold::Header hdr;
            const bool have_hdr = mold::parse_header(packet.data(), packet.size(), &hdr);

            // The outage. Not random: a link that goes away at a moment and
            // comes back. Everything in between is simply gone, which is a
            // much bigger hole than any one-in-N rate produces and is the case
            // a receiver most needs to survive.
            bool outage_starts = false;
            if (have_hdr && disconnect_until == 0 && disconnected == 0) {
                if (disconnect_at > 0 && hdr.sequence >= disconnect_at) {
                    outage_starts = true;
                } else if (disconnect_at_ns > 0 && hdr.message_count() > 0) {
                    // Peek at the first message in the packet. A datagram is
                    // the unit that goes missing, so the outage begins with
                    // the first one carrying a timestamp at or past the mark.
                    mold::BlockIterator it(packet.data(), packet.size(),
                                           hdr.message_count());
                    const uint8_t* msg = nullptr;
                    uint16_t msg_len = 0;
                    if (it.next(&msg, &msg_len) && msg_len >= 11 &&
                        itch::timestamp(msg) >= disconnect_at_ns) {
                        outage_starts = true;
                    }
                }
            }
            if (outage_starts) disconnect_until = disconnect_for;
            if (disconnect_until > 0) {
                --disconnect_until;
                ++disconnected;
                continue;
            }

            // ---- inject the halt, and the resume that ends it ----
            // Each goes out as its own single-message packet, which is what an
            // exchange does with a trading action anyway: it is not batched
            // behind forty order messages.
            if (halt_at_ns > 0 && have_hdr && hdr.message_count() > 0) {
                mold::BlockIterator it(packet.data(), packet.size(), hdr.message_count());
                const uint8_t* msg = nullptr;
                uint16_t msg_len = 0;
                if (it.next(&msg, &msg_len) && msg_len >= 11) {
                    const uint64_t ts = itch::timestamp(msg);
                    const uint16_t locate = itch::stock_locate(msg);
                    auto emit_action = [&](char state, uint64_t at) {
                        std::vector<uint8_t> pkt(mold::kHeaderLen);
                        mold::write_header(pkt.data(), hdr.session,
                                           hdr.sequence + seq_offset, 1);
                        uint8_t lb[2];
                        mold::write_be16(lb, 25);
                        pkt.push_back(lb[0]);
                        pkt.push_back(lb[1]);
                        // 'H': type(1) locate(2) tracking(2) ts(6) stock(8)
                        // state(1) reserved(1) reason(4) = 25 bytes. Only the
                        // state byte is read downstream; the symbol is left
                        // blank because a single-symbol slice has exactly one.
                        std::vector<uint8_t> m(25, ' ');
                        m[0] = 'H';
                        mold::write_be16(m.data() + 1, locate);
                        mold::write_be16(m.data() + 3, 0);
                        for (int k = 0; k < 6; ++k) {
                            m[5 + k] = static_cast<uint8_t>(at >> (8 * (5 - k)));
                        }
                        m[19] = static_cast<uint8_t>(state);
                        pkt.insert(pkt.end(), m.begin(), m.end());
                        w.write(pkt);
                        ++seq_offset;
                        ++halted;
                    };
                    if (!halt_sent && ts >= halt_at_ns) {
                        emit_action('H', ts);
                        halt_sent = true;
                    } else if (halt_sent && !resume_sent && ts >= halt_at_ns + halt_for_ns) {
                        emit_action('T', ts);
                        resume_sent = true;
                    }
                }
            }

            // Everything after an injection is renumbered by however many
            // messages went in ahead of it.
            if (seq_offset > 0 && have_hdr) {
                mold::write_be64(packet.data() + mold::kSessionLen,
                                 hdr.sequence + seq_offset);
            }

            // End-of-session is a marker, not data. Damaging it would test the
            // harness rather than the receiver.
            const bool structural = have_hdr && hdr.end_of_session();

            if (!structural && hit(drop)) { ++dropped; continue; }

            if (!structural && holding == false && hit(reorder)) {
                held = packet;                  // hold it back one slot
                holding = true;
                ++reordered;
                continue;
            }

            if (!structural && hit(truncate) && packet.size() > mold::kHeaderLen + 4) {
                // Clip the tail. The header still promises its full count, so
                // the receiver has to notice the bytes are not there.
                packet.resize(mold::kHeaderLen + (packet.size() - mold::kHeaderLen) / 2);
                ++truncated;
            }

            w.write(packet);
            if (!structural && hit(duplicate)) { w.write(packet); ++duped; }

            if (holding) {
                w.write(held);                  // ...and let it arrive late
                holding = false;
            }
        }
        if (holding) w.write(held);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        gzclose(out);
        return 1;
    }
    gzclose(out);

    std::printf("read %" PRIu64 " packets, wrote %" PRIu64 "\n", read, w.packets);
    std::printf("dropped      %" PRIu64 "\n", dropped);
    std::printf("duplicated   %" PRIu64 "\n", duped);
    std::printf("reordered    %" PRIu64 "\n", reordered);
    std::printf("truncated    %" PRIu64 "\n", truncated);
    std::printf("disconnected %" PRIu64 "  (a single outage)\n", disconnected);
    std::printf("halted       %" PRIu64 "  (trading action messages injected)\n", halted);
    return 0;
}

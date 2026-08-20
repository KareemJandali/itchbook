// mold_wrap — package a plain ITCH file into MoldUDP64 packets.
//
// NASDAQ ships the daily sample files already de-MoldUDP'd: a bare stream of
// length-prefixed messages, every one present, in order, exactly once. That is
// a convenience for anyone reconstructing a book and a problem for anyone
// testing a receiver, because the file cannot express the failures the receiver
// exists to handle. There is nothing to drop.
//
// So put the framing back. This is not a fiction — it is the format the feed
// actually arrives in, and re-imposing it is what lets the adversarial harness
// damage PACKETS rather than messages. That distinction is the point: real loss
// takes out a whole datagram and everything in it, so losing one packet loses
// forty messages at once, from the middle of the book, together.
//
// Usage:
//   mold_wrap <in.gz> <out.gz> [--mtu N] [--session NAME] [--heartbeat-ns N]
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/mold/packet.hpp"

using namespace itchbook;

namespace {

// Packets are written into the same [len][payload] gzip framing the rest of
// the repo uses, so a packet file is readable by the same Reader. The 2-byte
// prefix stands in for the UDP datagram boundary, which is the one thing a
// stream format cannot otherwise represent.
struct Writer {
    gzFile out;
    uint64_t packets = 0;

    void write(const uint8_t* p, size_t len) {
        uint8_t lb[2] = {static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xff)};
        gzwrite(out, lb, 2);
        gzwrite(out, p, static_cast<unsigned>(len));
        ++packets;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const char* in_path = nullptr;
    const char* out_path = nullptr;
    size_t mtu = 1400;               // a plausible datagram, not a jumbo frame
    std::string session = "ITCHBOOK01";
    uint64_t heartbeat_ns = 0;       // 0 = none

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mtu" && i + 1 < argc) mtu = static_cast<size_t>(std::atol(argv[++i]));
        else if (a == "--session" && i + 1 < argc) session = argv[++i];
        else if (a == "--heartbeat-ns" && i + 1 < argc)
            heartbeat_ns = std::strtoull(argv[++i], nullptr, 10);
        else if (in_path == nullptr) in_path = argv[i];
        else if (out_path == nullptr) out_path = argv[i];
        else { std::fprintf(stderr, "error: unexpected '%s'\n", argv[i]); return 2; }
    }
    if (in_path == nullptr || out_path == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <in.gz> <out.gz> [--mtu N] [--session NAME]\n"
                     "                           [--heartbeat-ns N]\n",
                     argv[0]);
        return 2;
    }
    if (mtu <= mold::kHeaderLen + 2) {
        std::fprintf(stderr, "error: --mtu must leave room for a header and a block\n");
        return 2;
    }

    gzFile out = gzopen(out_path, "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "error: cannot open %s\n", out_path);
        return 1;
    }
    Writer w{out};

    std::vector<uint8_t> packet(mtu);
    size_t used = mold::kHeaderLen;
    uint16_t count = 0;
    uint64_t sequence = 1;           // MoldUDP64 sequences start at 1
    uint64_t messages = 0;
    uint64_t last_hb_ts = 0;

    auto flush = [&]() {
        if (count == 0) return;
        mold::write_header(packet.data(), session.c_str(), sequence, count);
        w.write(packet.data(), used);
        sequence += count;
        used = mold::kHeaderLen;
        count = 0;
    };

    try {
        Reader reader(in_path);
        std::vector<uint8_t> buf;
        while (reader.next(buf)) {
            const size_t need = 2 + buf.size();
            // One message must always fit alone, or it could never be sent.
            if (need > mtu - mold::kHeaderLen) {
                std::fprintf(stderr, "error: message of %zu bytes exceeds --mtu %zu\n",
                             buf.size(), mtu);
                gzclose(out);
                return 1;
            }
            if (used + need > mtu || count == mold::kEndOfSession - 1) flush();

            mold::write_be16(packet.data() + used, static_cast<uint16_t>(buf.size()));
            std::memcpy(packet.data() + used + 2, buf.data(), buf.size());
            used += need;
            ++count;
            ++messages;

            // Heartbeats between packets, on feed time. Their sequence is the
            // next one that will be sent, which is what makes a quiet period
            // distinguishable from a dead link.
            if (heartbeat_ns > 0 && buf.size() >= 11) {
                const uint64_t ts = itch::timestamp(buf.data());
                if (last_hb_ts == 0) last_hb_ts = ts;
                if (ts - last_hb_ts >= heartbeat_ns) {
                    flush();
                    uint8_t hb[mold::kHeaderLen];
                    mold::write_header(hb, session.c_str(), sequence, 0);
                    w.write(hb, sizeof(hb));
                    last_hb_ts = ts;
                }
            }
        }
        flush();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        gzclose(out);
        return 1;
    }

    // End of session. Count 0xFFFF, and it carries no messages — a receiver
    // that reads the count as a plain integer walks off the end of this one.
    uint8_t eos[mold::kHeaderLen];
    mold::write_header(eos, session.c_str(), sequence, mold::kEndOfSession);
    w.write(eos, sizeof(eos));
    gzclose(out);

    std::printf("wrote %" PRIu64 " packets (%" PRIu64 " messages, mtu %zu) to %s\n",
                w.packets, messages, mtu, out_path);
    std::printf("sequence 1..%" PRIu64 "\n", sequence - 1);
    return 0;
}

// wire_to_book — the number this project did not have.
//
// Every latency figure in this repository before phase 10 was HANDLER COST:
// cycles between cycles_begin() and cycles_end() around a function call, in one
// thread, over a feed already in memory. 82 cycles per message is a true
// statement about a function and it is not a latency. A trading system means,
// by latency, the time from bytes arriving to book updated -- across a thread
// boundary, with a queue in the middle, under a load that may exceed what the
// consumer can absorb.
//
//     mold_replay_udp  --UDP-->  receiver thread --ring--> book thread
//     (paced sender)             (recvmmsg, stamp)         (parse, apply, stamp)
//
// The sample is (book applied) - (packet arrived). Read
// docs/phase10-methodology.md before reading any number this prints: it was
// written before this file existed, and it is the reason several of the
// decisions below look more paranoid than the code needs.
//
// WHAT THE RECEIVER DOES, AND DELIBERATELY DOES NOT DO.
//
// It stamps once per recvmmsg batch, not per packet -- one TSC read amortised
// over up to 32 packets. Per-packet kernel timestamps via SO_TIMESTAMPNS are
// the better instrument and are a stretch goal; whichever was used is recorded
// in the output so a table can say which.
//
// It unwraps MoldUDP64 and pushes RAW MESSAGE BYTES into the ring. It does not
// parse them. That is a deliberate split: the producer's per-message work stays
// near a memcpy so it can absorb a burst, and the parse cost lands inside the
// measured region on the consumer side, where it belongs. A receiver that
// parsed would make the book look faster by doing the book's work off the
// clock.
//
// And when the ring is full it drops the PACKET -- whole, before the sequencer
// sees it -- and counts it. It does not block, does not spin, does not
// overwrite. A dropped MoldUDP64 packet is exactly a sequence gap, which is
// exactly what recover/gap_policy.hpp and the phase-7 grader already exist for.
// Backpressure becomes a graded feed gap rather than silent loss, which is the
// same promise phase 7 made about the wire.
//
// THE DROPS THAT ARE NOT YOURS. A run that counts only ring-full events can
// lose thousands of packets upstream and report a clean sheet -- phase 7's
// failure mode reintroduced one layer higher. So SO_RCVBUF is set AND read back
// with getsockopt, because Linux silently caps it and the value you asked for
// is not evidence; and kernel drops are read from /proc/net/udp and reported
// separately from ring drops at every rate.
//
// WHICH ONE OVERFLOWS FIRST IS A PROPERTY OF THE SIZING, not of loopback. This
// comment used to assert that the socket buffer always goes first, and the
// artifact contradicts it: at --rcvbuf-mb 16 (32 MB granted) against a
// 65,536-slot ring, a sweep recorded 88,629 ring-full drops and ZERO kernel
// drops, because the book is the bottleneck and the receiver drains the socket
// as fast as recvmmsg will go. Shrink the buffer and the order reverses -- at
// --rcvbuf-mb 0 (2,304 bytes granted) the same feed produces kernel drops in
// the thousands, which is what the negative self-test in
// scripts/wire-to-book-check.sh uses. Both counters are reported at every rate
// precisely because which one moves is not knowable in advance.
//
// Usage:
//   wire_to_book [--port N] [--rcvbuf-mb N] [--ring-log2 N] [--timeout-ms N]
//                [--band-levels N] [--refs-capacity N] [--tick N]
//                [--expect-messages N] [--json out.json] [--hist-csv out.csv]
//                [--per-symbol out.csv] [--applied-out out.gz]
//                [--cpu-recv N] [--cpu-book N] [--quiet]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <zlib.h>

#include <string>
#include <thread>
#include <vector>

#include "itchbook/bench/histogram.hpp"
#include "itchbook/bench/rdtsc.hpp"
#include "itchbook/book/book_set.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/book/report.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/mold/packet.hpp"
#include "itchbook/mold/sequencer.hpp"
#include "itchbook/pipe/spsc_ring.hpp"
#include "itchbook/recover/gap_policy.hpp"
#include "itchbook/risk/kill_switch.hpp"

using namespace itchbook;

namespace {

// ---- the slot ---------------------------------------------------------------
//
// 64 bytes, and the bound is checked by the compiler rather than asserted in a
// comment. spec_length() puts the largest modelled message at 44 bytes ('P')
// and the largest framed one at 50 ('I'), so 8 for the arrival stamp and 2 for
// the length leaves headroom -- and a future message type that does not fit
// breaks the build instead of the run.
constexpr size_t kMaxMessage = 54;

// A GAP TRAVELS THROUGH THE RING LIKE A MESSAGE, and it has to.
//
// The sequencer runs on the receiver thread and the book runs on the consumer,
// so a gap is DETECTED on one side and ACTED ON on the other. Handing it over
// out of band -- an atomic counter, a flag the consumer polls -- would arrive
// at the wrong point in the stream: rebuild-forward means discarding the book
// at exactly the message the gap precedes, and a flag that overtakes the
// messages still in flight discards the wrong ones. So a gap occupies a slot,
// in order, and the consumer sees it exactly where it happened.
//
// len == 0 is the marker; a real message is never zero-length, because the
// sequencer only delivers a message when msg_len > 0.
constexpr uint16_t kGapMarker = 0;

struct Slot {
    uint64_t arrival = 0;
    uint16_t len = 0;
    uint8_t bytes[kMaxMessage] = {};

    bool is_gap() const { return len == kGapMarker; }
    uint64_t gap_count() const {
        uint64_t n = 0;
        std::memcpy(&n, bytes, sizeof(n));
        return n;
    }
    void set_gap(uint64_t count) {
        len = kGapMarker;
        std::memcpy(bytes, &count, sizeof(count));
    }
};
static_assert(sizeof(Slot) == 64, "the slot is a cache line; keep it one");

// A NOTE ON THE CONSUMER'S SPIN, AND A HYPOTHESIS THAT DIED.
//
// The book thread below spins on readable() without pausing or yielding. That
// is deliberate: this consumer is meant to own a core, and a sleeping consumer
// pays a wake-up on every message, which is most of the latency at low rates.
//
// It was still suspected of something. Phase 10.7's sweep found the paced
// sender missing its schedule at every rate, and the first measurement of the
// sender with and without this program running showed p99.9 lateness of 26 us
// against 14,277,397 ns -- 539x -- which looked exactly like a spinning
// consumer starving the sender's nanosleep on a two-core box. A bounded spin
// with a yield was written to fix it.
//
// It fixed nothing, because there was nothing there. Best of five runs each:
// 811,999 ns for the sender alone against 562,005 ns with this program
// consuming -- indistinguishable, and nominally BETTER with the consumer
// running. Both configurations ranged over two orders of magnitude between
// repeats. The original pair was one sample of each side of a distribution
// that spans 40 ms, and the 539x was noise wearing a mechanism's clothes.
//
// The scheduler jitter in this container is simply larger than anything being
// measured, which is what the sweep's NO RATE QUALIFIED verdict already said.
// The spin stays unbounded, the yield was reverted, and this comment is here
// because a refuted hypothesis that leaves no trace gets re-derived by the next
// person to read the sweep output -- most likely me.

constexpr size_t kBatch = 32;          // recvmmsg batch
constexpr size_t kMaxDatagram = 2048;

// recvmmsg and its struct are Linux-only. The development machine for this
// project is a Mac, and a file that only compiles on the measurement host is a
// file whose bugs are found on the measurement host. So: the same shape, one
// datagram at a time, and the output says which was used.
#if !defined(__linux__)
struct mmsghdr {
    msghdr msg_hdr;
    unsigned msg_len;
};
#endif

struct Options {
    uint16_t port = 26400;
    size_t rcvbuf_mb = 8;
    int ring_log2 = 16;                // 65,536 slots
    uint64_t timeout_ms = 5000;
    size_t band_levels = 512;
    size_t refs_capacity = size_t{1} << 23;
    // The histogram reserves this many samples. Growing it would allocate
    // INSIDE the measured region, which is the one thing phase 4's rules
    // forbid, so the run reports whether the reserve was exceeded rather than
    // quietly reallocating and pretending the numbers are clean.
    size_t expect_messages = size_t{1} << 22;
    const char* json = nullptr;
    const char* hist_csv = nullptr;
    const char* per_symbol = nullptr;
    // Records the raw bytes of every message the book thread applied, in the
    // order it applied them. This is the phase 10.6 torture instrument, and it
    // is NOT a measurement mode: it memcpys every message inside the measured
    // loop, so a run with it on reports timings that describe the recording.
    const char* applied_out = nullptr;
    int32_t tick = 100;
    // Phase 7's machinery, on phase 10's pipeline. Rebuild-forward on a gap,
    // and a convergence window before the book is called trusted again.
    uint64_t recover_after = 20000;
    // Phase 10's new kill-switch input. Zero disables, like every other limit.
    uint64_t kill_ring_occupancy = 0;
    uint64_t kill_backlog_ms = 100;
    // The consumer-slow scenario's knob: burn this many nanoseconds per
    // message in the book thread, so the ring fills and the receiver is forced
    // to drop packets. Backpressure on demand.
    uint64_t slow_consumer_ns = 0;
    int cpu_recv = -1;
    int cpu_book = -1;
    // Self-test only: see ToRing::discard_markers.
    bool break_gap_markers = false;
    bool quiet = false;
};

bool pin_to(int cpu) {
    if (cpu < 0) return false;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    return false;
#endif
}

// Datagrams the KERNEL discarded before this program ever saw them, from
// /proc/net/udp's last column. Returns UINT64_MAX where the file does not
// exist, which is not zero and must not be reported as zero: "no drops" and
// "this platform cannot tell you" are different claims.
uint64_t kernel_drops(uint16_t port) {
#if defined(__linux__)
    std::FILE* f = std::fopen("/proc/net/udp", "r");
    if (f == nullptr) return UINT64_MAX;
    char line[512];
    uint64_t total = 0;
    bool found = false;
    if (std::fgets(line, sizeof(line), f) == nullptr) {   // header
        std::fclose(f);
        return UINT64_MAX;
    }
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned local_port = 0;
        char local[64] = {};
        // sl  local_address rem_address st tx rx tr tm retr uid to inode ref ptr drops
        if (std::sscanf(line, "%*d: %63[0-9A-Fa-f]:%X", local, &local_port) != 2) continue;
        if (local_port != port) continue;
        // TRIM FIRST. /proc/net/udp pads the drops column with trailing
        // spaces:
        //
        //   3953: 00000000:67F3 ... 0000000000000000 14202<spaces>\n
        //
        // so strrchr(line, ' ') lands on the padding AFTER the number, and
        // strtoull then starts on whitespace, finds the newline, and returns
        // zero. This was the second of two independent reasons kernel_drops
        // read 0 for every run this tool ever took -- fixing the read-after-
        // close ordering alone still produced a constant, which is how this
        // one was found: the negative self-test in wire-to-book-check.sh
        // failed on its first run against a socket whose row said 14,202.
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        const char* last = std::strrchr(line, ' ');
        if (last == nullptr) continue;
        total += std::strtoull(last, nullptr, 10);
        found = true;
    }
    std::fclose(f);
    // NOT zero. A port with no row in /proc/net/udp is a port this function
    // cannot report on, and "no drops" is a different claim from "unreadable".
    // Returning 0 here made kernel_known true for every run ever taken, which
    // silently retired the exit-4 path and the kernel half of the LOSSY gate.
    return found ? total : UINT64_MAX;
#else
    (void)port;
    return UINT64_MAX;
#endif
}

// ---- shared state between the two threads ----------------------------------

template <size_t Cap>
struct SharedState {
    pipe::SpscRing<Slot, Cap> ring;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> ring_full_drops{0};     // packets refused for want of room
    // SLOTS published, which since 10.9 is messages PLUS gap markers. Every
    // reader of this wants one or the other and never their sum, so the
    // reporting below separates them rather than leaving each caller to
    // remember -- the scripts that did not remember failed the accounting
    // identity by exactly the gap count, which is how this was found.
    std::atomic<uint64_t> pushed{0};
    std::atomic<uint64_t> recv_errors{0};
    std::atomic<uint64_t> max_occupancy{0};
};

// The receiver's handler: the sequencer hands it messages in order, and it
// copies them into ring slots. Framing only -- no parse.
//
// A HOLE ALWAYS HAS A MARKER IN FRONT OF IT.
//
// The budget below is the room that was checked for ONE packet's message
// count, but a packet that closes a reorder gap makes the sequencer release
// everything it was holding, so a single on_packet() can emit far more than
// the packet it was handed. When that runs the budget out, the rest of the
// delivery cannot be staged -- and the first version of this counted those
// refusals and carried on, which left the consumer applying the messages after
// them without ever being told anything was missing. That is exactly the
// silent wrongness this pipeline exists to make impossible, and the torture
// leg of the determinism gate found it: at 3M msg/s into a 1024-slot ring the
// gap MARKERS themselves could not be staged.
//
// So a hole is a DEBT, not a statistic. Everything that vanishes -- a gap the
// sequencer declared, a message too large for a slot, a message the budget
// could not take -- adds to `owed`, and nothing further may be staged until a
// marker carrying `owed` has been. Order survives because once the budget is
// out NOTHING more is staged from that delivery, so everything after the first
// refusal belongs to the same hole; and the debt is always payable, because
// the next packet is only fed after at least one slot was proved free.
//
// The consequence is that markers COALESCE: one marker can carry several
// declared gaps plus whatever the budget refused after them. Any check that
// counts markers against the sequencer's gap count is therefore wrong, and the
// identities below count MESSAGES missing instead.
template <typename Shared>
struct ToRing {
    Shared* sh = nullptr;
    uint64_t arrival = 0;
    size_t staged = 0;
    // How many slots the caller proved were free before handing this packet
    // over. It is not decoration: a packet that arrives out of order is HELD,
    // and the packet that later closes the gap makes the sequencer deliver the
    // held ones too -- so one on_packet() call can emit far more messages than
    // the packet it was given contains. Without a budget those extra messages
    // are written past the room that was checked for, which is a silent
    // overrun of slots the consumer has not finished with.
    size_t budget = 0;
    uint64_t overflow = 0;
    uint64_t oversize = 0;
    uint64_t gaps = 0;
    uint64_t lost = 0;
    uint64_t owed = 0;        // missing messages the book has not been told about
    uint64_t deferrals = 0;   // times a marker had to wait for a slot
    uint64_t markers = 0;     // markers staged; <= gaps, because they coalesce
    uint64_t discarded = 0;   // --break-gap-markers only; see below
    // A GATE WHOSE FAILURE CASE IS NEVER EXERCISED IS A GATE NOBODY HAS CHECKED
    // OPENS. Setting this restores the pre-10.10 behaviour exactly -- a marker
    // with no room is thrown away and the messages behind it are published
    // anyway -- so the determinism gate can be shown to catch the very bug that
    // reached CI green. It is reachable only from --break-gap-markers, it is
    // named to be impossible to set by accident, and the run it produces fails.
    bool discard_markers = false;

    // Pay the debt if there is room for the marker. A false return is the
    // caller's instruction not to stage anything either: a message published
    // ahead of the marker that covers the hole before it is the whole failure.
    bool settle() {
        if (owed == 0) return true;
        if (staged >= budget) {
            ++deferrals;
            if (discard_markers) { discarded += owed; owed = 0; return true; }
            return false;
        }
        Slot& s = sh->ring.write_slot(staged);
        s.arrival = arrival;
        s.set_gap(owed);
        ++staged;
        ++markers;
        owed = 0;
        return true;
    }

    void on_message(char, const uint8_t* payload, uint16_t len) {
        // Both refusals below are holes, and both are now announced. Oversize
        // is a "cannot happen" -- kMaxMessage is static_asserted against the
        // parser's largest message -- but a cannot-happen that silently
        // shortens the stream is the one worth wiring into the same path as
        // the one that does happen.
        if (len > kMaxMessage) { ++oversize; ++owed; return; }
        if (!settle() || staged >= budget) { ++overflow; ++owed; return; }
        Slot& s = sh->ring.write_slot(staged);
        s.arrival = arrival;
        s.len = len;
        std::memcpy(s.bytes, payload, len);
        ++staged;
    }
    // Staged like a message, so the consumer discards the book at the point in
    // the stream where the loss actually happened -- or held as debt until a
    // slot frees, which is the same point, because nothing may pass a debt.
    void on_gap(uint64_t, uint64_t count) {
        ++gaps;
        lost += count;
        owed += count;
        settle();
    }
};

template <size_t Cap>
int run(const Options& opt);

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") opt.port = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (a == "--rcvbuf-mb") opt.rcvbuf_mb = static_cast<size_t>(std::atoi(next("--rcvbuf-mb")));
        else if (a == "--ring-log2") opt.ring_log2 = std::atoi(next("--ring-log2"));
        else if (a == "--timeout-ms") opt.timeout_ms = std::strtoull(next("--timeout-ms"), nullptr, 10);
        else if (a == "--band-levels") opt.band_levels = static_cast<size_t>(std::atoi(next("--band-levels")));
        else if (a == "--refs-capacity") opt.refs_capacity = std::strtoull(next("--refs-capacity"), nullptr, 10);
        else if (a == "--expect-messages")
            opt.expect_messages = std::strtoull(next("--expect-messages"), nullptr, 10);
        else if (a == "--per-symbol") opt.per_symbol = next("--per-symbol");
        else if (a == "--recover-after")
            opt.recover_after = std::strtoull(next("--recover-after"), nullptr, 10);
        else if (a == "--kill-ring-occupancy")
            opt.kill_ring_occupancy = std::strtoull(next("--kill-ring-occupancy"), nullptr, 10);
        else if (a == "--kill-backlog-ms")
            opt.kill_backlog_ms = std::strtoull(next("--kill-backlog-ms"), nullptr, 10);
        else if (a == "--slow-consumer-ns")
            opt.slow_consumer_ns = std::strtoull(next("--slow-consumer-ns"), nullptr, 10);
        else if (a == "--applied-out") opt.applied_out = next("--applied-out");
        else if (a == "--tick") opt.tick = std::atoi(next("--tick"));
        else if (a == "--json") opt.json = next("--json");
        else if (a == "--hist-csv") opt.hist_csv = next("--hist-csv");
        else if (a == "--cpu-recv") opt.cpu_recv = std::atoi(next("--cpu-recv"));
        else if (a == "--cpu-book") opt.cpu_book = std::atoi(next("--cpu-book"));
        else if (a == "--break-gap-markers") opt.break_gap_markers = true;
        else if (a == "--quiet") opt.quiet = true;
        else {
            std::fprintf(stderr,
                "usage: %s [--port N] [--rcvbuf-mb N] [--timeout-ms N] [--band-levels N]\n"
                "       [--ring-log2 10|12|14|16|18] [--expect-messages N]\n"
                "       [--per-symbol out.csv] [--applied-out out.gz] [--tick N]\n"
                "       [--recover-after N] [--slow-consumer-ns N]\n"
                "       [--kill-ring-occupancy N] [--kill-backlog-ms N]\n"
                "       [--refs-capacity N] [--json out.json] [--hist-csv out.csv]\n"
                "       [--cpu-recv N] [--cpu-book N] [--break-gap-markers] [--quiet]\n",
                argv[0]);
            return 2;
        }
    }

    // The ring's capacity is a template parameter -- that is what makes the
    // wrap a mask instead of a modulo -- so --ring-log2 picks among
    // instantiations rather than passing a number. Five sizes is enough to walk
    // the knee in 10.7, and the flag exists at all because the ring-full path
    // is the design's central claim: backpressure becomes a sequence gap, not
    // silent loss. A claim that cannot be provoked on demand is untested, and
    // on a 65,536-slot ring nothing short of a sustained overload provokes it.
    switch (opt.ring_log2) {
        case 10: return run<size_t{1} << 10>(opt);
        case 12: return run<size_t{1} << 12>(opt);
        case 14: return run<size_t{1} << 14>(opt);
        case 16: return run<size_t{1} << 16>(opt);
        case 18: return run<size_t{1} << 18>(opt);
        default:
            std::fprintf(stderr, "error: --ring-log2 must be 10, 12, 14, 16 or 18\n");
            return 2;
    }
}

namespace {

template <size_t Cap>
int run(const Options& opt) {
    using Ring = pipe::SpscRing<Slot, Cap>;
    using Shared = SharedState<Cap>;

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "error: socket: %s\n", std::strerror(errno));
        return 1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // Ask, then READ BACK. Linux caps this at net.core.rmem_max and does not
    // tell you; it also doubles what it grants for bookkeeping. The number that
    // goes in the results is the one getsockopt returns, not the one asked for.
    int want = static_cast<int>(opt.rcvbuf_mb * 1024 * 1024);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &want, sizeof(want));
    int got = 0;
    socklen_t got_len = sizeof(got);
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &got, &got_len);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(opt.port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "error: bind %u: %s\n", opt.port, std::strerror(errno));
        ::close(fd);
        return 1;
    }
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(opt.timeout_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((opt.timeout_ms % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    const uint64_t drops_before = kernel_drops(opt.port);
    // On the heap, not the stack. At --ring-log2 18 this object is 16 MB and
    // the default stack is 8, so the version that declared it here worked only
    // because 65,536 slots happened to fit.
    auto shared_owner = std::make_unique<Shared>();
    Shared& shared = *shared_owner;
    mold::SequencerStats seq_stats;
    bool recv_pinned = false;
    bool book_pinned = false;
    uint64_t oversize = 0;
    uint64_t stage_overflow = 0;
    uint64_t unannounced_loss = 0;   // sink.owed at end of stream; must be 0
    uint64_t gap_deferrals = 0;
    uint64_t gap_markers = 0;
    uint64_t malformed = 0;
    // Sequence bookkeeping, kept by the RECEIVER rather than the sequencer,
    // because the sequencer only ever sees the packets that were not dropped.
    // A run whose first packets are refused for want of ring space starts the
    // sequencer at a later sequence number, and those earlier messages are then
    // lost without anything counting them -- the drop path silently erasing its
    // own evidence. first_seq/last_end come from the headers, before any drop
    // decision, so the two ends of the stream are known regardless.
    uint64_t first_seq = 0, last_end = 0, seq_start = 0, seq_end = 0;
    bool have_first = false, have_start = false;

    // ---- receiver thread ----------------------------------------------------
    std::thread receiver([&] {
        recv_pinned = pin_to(opt.cpu_recv);
        ToRing<Shared> sink;
        sink.sh = &shared;
        sink.discard_markers = opt.break_gap_markers;
        mold::Sequencer<ToRing<Shared>> seq;

        std::vector<uint8_t> storage(kBatch * kMaxDatagram);
        std::vector<mmsghdr> msgs(kBatch);
        std::vector<iovec> iov(kBatch);
        for (size_t i = 0; i < kBatch; ++i) {
            iov[i].iov_base = storage.data() + i * kMaxDatagram;
            iov[i].iov_len = kMaxDatagram;
            std::memset(&msgs[i], 0, sizeof(msgs[i]));
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }

        // Free slots, and the comparison has to be against what this packet
        // NEEDS.
        //
        // writable() is a LOWER bound by contract: it reads a cached copy of
        // the consumer's cursor and refreshes only when that copy says zero, so
        // a small answer means "at least this many", not "only this many". The
        // staleness grows with every publish and is a function of how many
        // messages have gone by, not of the clock -- which is why the first
        // version of this dropped an IDENTICAL 2,464 packets at 30,000 and at
        // 90,000 msg/s, with peak ring occupancy at 1,683 of 4,096 slots. It
        // was refusing packets for want of room in a ring that was 41% full,
        // reproducibly, because the cheap answer was stale and nothing asked
        // for the real one.
        //
        // So: take the cheap answer when it is already enough, and pay for the
        // true figure before declaring a drop. Never the other way round. That
        // earlier version compared the cheap answer against kBatch instead of
        // against `need`, which is the same mistake wearing a plausible
        // constant.
        //
        // And the true figure comes from writable_exact(), NOT from
        // `capacity() - size()`. Those two return the same number and are not
        // interchangeable: writable_exact() also refreshes the producer's
        // cached view of the consumer, which is what keeps the next writable()
        // call honest. Publishing against a figure writable() did not issue
        // leaves `head - cached_tail_` past Capacity, and the next writable()
        // underflows to eighteen quintillion free slots. That version applied
        // messages out of order by exactly one lap of the ring and passed every
        // count-based check, because nothing was lost -- only reordered.
        auto free_slots = [&](size_t need) {
            const size_t cheap = shared.ring.writable();
            return cheap >= need ? cheap : shared.ring.writable_exact();
        };

        // Stage whatever the sequencer emits for one packet, then publish once.
        auto feed = [&](const uint8_t* p, size_t len, uint64_t arrival, size_t room) {
            sink.arrival = arrival;
            sink.staged = 0;
            sink.budget = room;
            seq.on_packet(p, len, sink);
            if (sink.staged > 0) {
                shared.ring.publish(sink.staged);
                shared.pushed.fetch_add(sink.staged, std::memory_order_relaxed);
            }
        };

        bool running = true;
        while (running) {
#if defined(__linux__)
            const int n = ::recvmmsg(fd, msgs.data(), static_cast<unsigned>(kBatch),
                                     MSG_WAITFORONE, nullptr);
#else
            // recvmmsg is Linux-only. One datagram at a time elsewhere, which
            // costs a TSC read per packet instead of per batch and is stated in
            // the output rather than hidden -- the point is that the code runs
            // for development, not that the numbers are comparable.
            const ssize_t one_len = ::recvfrom(fd, iov[0].iov_base, iov[0].iov_len, 0,
                                               nullptr, nullptr);
            const int n = one_len < 0 ? -1 : 1;
            if (n == 1) msgs[0].msg_len = static_cast<unsigned>(one_len);
#endif
            if (n <= 0) break;   // timeout or error: the sender has stopped

            // ONE stamp for the batch. Everything in it is treated as having
            // arrived at the same instant, which is true to within the cost of
            // draining the batch and is stated as the instrument's resolution.
            const uint64_t arrival = bench::cycles_end();
            shared.packets.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

            for (int i = 0; i < n; ++i) {
                const uint8_t* p = static_cast<const uint8_t*>(iov[static_cast<size_t>(i)].iov_base);
                const size_t len = msgs[static_cast<size_t>(i)].msg_len;

                // parse_header FIRST, and believe its answer. Reading
                // message_count() out of a header the parser refused to fill is
                // a read of uninitialised stack that happens to work until the
                // day it decides the packet holds four billion messages.
                mold::Header hdr;
                if (!mold::parse_header(p, len, &hdr)) {
                    ++malformed;
                    continue;
                }
                if (hdr.end_of_session()) {
                    seq.on_packet(p, len, sink);   // let the sequencer record it
                    running = false;
                    break;
                }
                if (!hdr.heartbeat()) {
                    if (!have_first) { first_seq = hdr.sequence; have_first = true; }
                    const uint64_t end = hdr.sequence + hdr.message_count();
                    if (end > last_end) last_end = end;
                }

                // Room for the WHOLE packet before any of it is staged. A
                // half-written packet is worse than a dropped one: the
                // sequencer would have delivered part of a block and the gap
                // would be invisible.
                const size_t need = hdr.message_count() == 0 ? 1 : hdr.message_count();
                const size_t room = free_slots(need);
                if (room < need) {
                    shared.ring_full_drops.fetch_add(1, std::memory_order_relaxed);
                    continue;      // the sequencer never sees it: a real gap
                }
                if (!have_start) { seq_start = hdr.sequence; have_start = true; }
                feed(p, len, arrival, room);
            }
            const size_t occ = shared.ring.size();
            if (occ > shared.max_occupancy.load(std::memory_order_relaxed)) {
                shared.max_occupancy.store(occ, std::memory_order_relaxed);
            }
        }

        // Anything still held out of order was never reachable, and a gap open
        // at end of session is still a gap. flush() can emit messages, so it
        // gets a budget like everything else.
        //
        // ONE exit, and it is the reason this loop is shaped the way it is.
        // The first version of this file returned from the middle of the batch
        // on the end-of-session packet -- which is the NORMAL way a run ends --
        // and so never reached the lines below. Every clean run would have
        // reported zero gaps and zero messages lost because the counters were
        // never copied out, not because nothing was lost. A clean sheet by
        // construction is the exact failure phase 7 was built to prevent.
        {
            const size_t room = Ring::capacity() - shared.ring.size();
            sink.staged = 0;
            sink.budget = room;
            seq.flush(sink);
            if (sink.staged > 0) {
                shared.ring.publish(sink.staged);
                shared.pushed.fetch_add(sink.staged, std::memory_order_relaxed);
            }
        }

        // AND THE DEBT IS PAID BEFORE THE STREAM ENDS.
        //
        // Every other settle happens because something wanted to be staged
        // behind the marker. At end of session there is nothing behind it, so a
        // hole opened by the last delivery would go unannounced -- the same
        // silent wrongness, one slot from the end. The consumer is still
        // draining (done is not set until below), so waiting for a slot cannot
        // deadlock; the deadline is only so that a consumer which has died
        // becomes a reported failure instead of a hang.
        if (sink.owed != 0) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (sink.owed != 0 && std::chrono::steady_clock::now() < deadline) {
                sink.staged = 0;
                sink.budget = shared.ring.writable_exact();
                if (!sink.settle()) continue;
                shared.ring.publish(sink.staged);
                shared.pushed.fetch_add(sink.staged, std::memory_order_relaxed);
            }
        }

        seq_end = seq.expected();
        seq_stats = seq.stats();
        oversize = sink.oversize;
        stage_overflow = sink.overflow;
        unannounced_loss = sink.owed + sink.discarded;
        gap_deferrals = sink.deferrals;
        gap_markers = sink.markers;
        shared.done.store(true, std::memory_order_release);
    });

    // ---- book thread --------------------------------------------------------
    // Same construction arguments as book_replay's --all-symbols, because
    // 10.6 diffs the two books byte for byte and the band policy is visible in
    // the output (off_band_adds, recentres). A tick or a band width that
    // differed would make them disagree for a reason that has nothing to do
    // with the pipeline.
    // Calibrated before the threads start: the throttle converts nanoseconds
    // to cycles, and calibrating inside the measured loop would be both slow
    // and wrong.
    const double cyc_per_ns_for_throttle = bench::calibrate_cycles_per_ns();

    book::BookSet books(opt.refs_capacity, opt.tick, 20, opt.band_levels);
    // The samples, as (cycles, type) pairs in two reserved vectors.
    //
    // The plan asks for a histogram per message type. 256 Histograms, each
    // guessing its own reserve, would reallocate mid-run the moment one type
    // outgrew its guess -- and 'A' is 40% of a real feed while 'C' is a
    // rounding error, so no single reserve is right for both. Recording the
    // type as one byte beside the sample costs two push_backs into reserved
    // vectors, and the split is then done afterwards, off the clock, from data
    // that is exact rather than pre-bucketed.
    std::vector<uint32_t> sample_cycles;
    std::vector<uint8_t> sample_type;
    sample_cycles.reserve(opt.expect_messages);
    sample_type.reserve(opt.expect_messages);
    // [len][payload], the framing the rest of the repo reads, accumulated in
    // memory and written after the run. Reserved up front for the same reason
    // everything else here is: growing it mid-run would allocate on the
    // consumer path.
    std::vector<uint8_t> applied_bytes;
    if (opt.applied_out != nullptr) applied_bytes.reserve(opt.expect_messages * (kMaxMessage + 2));
    uint64_t applied = 0;
    uint64_t consumed = 0;

    recover::GapConfig gcfg;
    gcfg.recovery_window = opt.recover_after;
    recover::GapTracker gap(gcfg);
    risk::KillSwitchConfig kcfg;
    kcfg.max_ring_occupancy = opt.kill_ring_occupancy;
    kcfg.backlog_sustained_ns = opt.kill_backlog_ms * 1000000ULL;
    risk::KillSwitch kill(kcfg);
    uint64_t gaps_seen = 0;
    uint64_t messages_lost = 0;

    std::thread bookthread([&] {
        book_pinned = pin_to(opt.cpu_book);
        for (;;) {
            const size_t ready = shared.ring.readable();
            if (ready == 0) {
                if (shared.done.load(std::memory_order_acquire)) {
                    if (shared.ring.readable() == 0) break;
                }
                continue;
            }
            // The kill switch sees the queue from the side that drains it,
            // once per batch rather than once per message: the limit is about a
            // backlog that persists over milliseconds, and sampling it a
            // million times a second measures the sampler.
            //
            // NANOSECONDS, from a clock that is in nanoseconds. The first
            // version passed bench::cycles_end() here, which is a TSC reading
            // -- so "sustained for 100 ms" was silently compared against a
            // cycle count and meant whatever the core's frequency happened to
            // make it. A risk control is the last place to infer a unit. This
            // is a monotonic clock read once per batch, which costs nothing at
            // this granularity and cannot be misread.
            kill.on_ring_occupancy(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count()),
                shared.ring.size());

            for (size_t k = 0; k < ready; ++k) {
                const Slot& s = shared.ring.read_slot(k);

                // A gap arrives here in stream order, which is the whole reason
                // it travelled through the ring. Rebuild-forward: discard every
                // resting order and keep going. The books that come out hold no
                // wrong orders, only missing ones.
                if (s.is_gap()) {
                    ++gaps_seen;
                    messages_lost += s.gap_count();
                    if (gap.on_gap(0, s.gap_count())) books.clear_all_orders();
                    continue;
                }

                const char type = static_cast<char>(s.bytes[0]);

                // Whether a reference resolved is the recovery signal. Reading
                // it needs the ONE book the message routes to -- BookSet's
                // unknown_ref() sums every book, which is O(symbols) and would
                // cost more per message than the book update itself.
                const bool carries_ref = (type == 'E' || type == 'C' || type == 'X' ||
                                          type == 'D' || type == 'U');
                uint64_t before = 0;
                book::Book* one = nullptr;
                if (carries_ref) {
                    one = &books.at(itch::stock_locate(s.bytes));
                    before = one->unknown_ref();
                }
                if (book::apply(books, type, s.bytes)) ++applied;
                if (carries_ref) gap.on_reference(one->unknown_ref() == before);

                // The throttle. A spin rather than a sleep: sleeping hands the
                // core to the receiver, which is the opposite of what a slow
                // consumer does to a pipeline, and would make the scenario test
                // the scheduler instead of the backpressure path.
                if (opt.slow_consumer_ns > 0) {
                    const uint64_t until = bench::cycles_end() +
                        static_cast<uint64_t>(static_cast<double>(opt.slow_consumer_ns) *
                                              cyc_per_ns_for_throttle);
                    while (bench::cycles_end() < until) { }
                }
                if (opt.applied_out != nullptr) {
                    applied_bytes.push_back(static_cast<uint8_t>(s.len >> 8));
                    applied_bytes.push_back(static_cast<uint8_t>(s.len & 0xff));
                    applied_bytes.insert(applied_bytes.end(), s.bytes, s.bytes + s.len);
                }
                // The sample. Taken after apply, inside the loop, into a
                // preallocated histogram: no allocation, no I/O, no printing in
                // the measured region -- the phase-4 rules, which were about a
                // benchmark and are about this.
                const uint64_t now = bench::cycles_end();
                const uint64_t sample = now > s.arrival ? now - s.arrival : 0;
                sample_cycles.push_back(sample > UINT32_MAX ? UINT32_MAX
                                                            : static_cast<uint32_t>(sample));
                sample_type.push_back(static_cast<uint8_t>(type));
            }
            shared.ring.consume(ready);
            consumed += ready;
        }
    });

    // ---- READY, and why bind() is not the same thing -----------------------
    //
    // A load generator that starts when the PORT appears starts while this
    // thread is still between bind() and here: calibrate_cycles_per_ns sleeps
    // 50 ms, BookSet allocates its ref table, and two vectors reserve. The
    // receiver thread above is already stamping arrivals through all of it,
    // and nothing is draining the ring, so every message that lands in that
    // window carries the whole remaining stall as its wire-to-book sample.
    // It showed up as multi-millisecond samples at EVERY rate on the ladder
    // including 1x, where the ring is nearly empty -- a stall whose DURATION
    // did not change across the whole ladder, which is not queueing. The gap
    // itself is the measurement: 91-102 ms across six runs, between the port
    // appearing in /proc/net/udp and this line.
    //
    // Two cautions for anyone re-deriving that from an artifact. The histogram
    // buckets are raw TSC CYCLES while every *_ns field beside them is
    // nanoseconds, so bucket edges read ~3.6x too large if taken literally --
    // which is the mistake made when this comment was first written, and why
    // bench/rate-sweep.py now records "buckets_unit". And the per-rate figures
    // move every run, so no specific one is quoted here; read them from the
    // artifact in the tree rather than from this comment.
    //
    // So the consumer says when it exists. stdout, unconditionally: a marker
    // that --quiet could suppress is a marker that goes missing exactly when
    // the harness is driving.
    std::printf("READY consumer\n");
    std::fflush(stdout);

    receiver.join();
    bookthread.join();

    // BEFORE close(fd), and this ordering is the entire fix. A UDP socket
    // leaves /proc/net/udp the instant it closes, so a read taken afterwards
    // finds no row, and kernel_drops() used to answer that with 0.
    const uint64_t drops_after = kernel_drops(opt.port);
    ::close(fd);
    // Three ways this can be unknowable, and all three must land in the same
    // place: no /proc/net/udp, no row for the port, or a counter that went
    // BACKWARDS -- kernel_drops() sums every row matching the port and the
    // socket sets SO_REUSEADDR, so another binding can make before > after.
    // The first version clamped that third case to 0 while leaving
    // kernel_known true, which republished "cannot attribute" as "no drops":
    // the exact conflation this function was just fixed to stop making.
    const bool kernel_known = drops_before != UINT64_MAX &&
                              drops_after != UINT64_MAX &&
                              drops_after >= drops_before;
    const uint64_t kernel_lost = kernel_known ? drops_after - drops_before : 0;

    // Built after the run, not during it: filling the histogram was the last
    // thing left in the measured loop that could allocate.
    bench::Histogram overall(sample_cycles.size());
    for (uint32_t c : sample_cycles) overall.add(c);
    overall.finalize();
    const double cyc_per_ns = bench::calibrate_cycles_per_ns();
    auto ns = [cyc_per_ns](double cycles) { return cycles / cyc_per_ns; };

    if (!opt.quiet) {
        std::printf("%-32s %s\n", "clock", bench::clock_name());
        std::printf("%-32s %s / %s\n", "pinned (receiver / book)",
                    recv_pinned ? "yes" : "no", book_pinned ? "yes" : "no");
#if defined(__linux__)
        std::printf("%-32s %s\n", "arrival stamp", "one per recvmmsg batch");
#else
        std::printf("%-32s %s\n", "arrival stamp", "one per packet (no recvmmsg here)");
#endif
        std::printf("%-32s %d MB asked, %d MB granted\n", "SO_RCVBUF",
                    static_cast<int>(opt.rcvbuf_mb), got / (1024 * 1024));
        std::printf("%-32s %zu slots\n", "ring", Ring::capacity());
        std::printf("\n%-32s %14" PRIu64 "\n", "packets received",
                    shared.packets.load());
        std::printf("%-32s %14" PRIu64 "\n", "messages into the ring",
                    shared.pushed.load() - gaps_seen);
        std::printf("%-32s %14" PRIu64 "\n", "  gap markers into the ring", gaps_seen);
        std::printf("%-32s %14" PRIu64 "\n", "messages applied to books", applied);
        std::printf("%-32s %14zu\n", "peak ring occupancy",
                    static_cast<size_t>(shared.max_occupancy.load()));
        // %zu, not PRIu64: books() returns size_t. On Linux x86-64 those are
        // the same type and -Wformat says nothing; on macOS uint64_t is
        // `unsigned long long` and size_t is `unsigned long`, and this line was
        // a -Werror=format build failure there for the whole of phase 10.
        std::printf("%-32s %14zu\n", "books built", books.books());

        // The two kinds of loss, never added together. One is the ring
        // refusing work it cannot hold; the other is the kernel discarding
        // datagrams before this program existed. A run that reports only the
        // first can lose thousands upstream and look clean.
        std::printf("\n%-32s %14" PRIu64 "\n", "packets dropped: ring full",
                    shared.ring_full_drops.load());
        if (kernel_known) {
            std::printf("%-32s %14" PRIu64 "\n", "packets dropped: kernel buffer", kernel_lost);
        } else {
            std::printf("%-32s %14s\n", "packets dropped: kernel buffer",
                        "UNKNOWN — no /proc/net/udp");
        }
        std::printf("%-32s %14" PRIu64 "\n", "sequence gaps declared", seq_stats.gaps);
        std::printf("%-32s %14" PRIu64 "\n", "messages lost to gaps", seq_stats.messages_lost);
        std::printf("%-32s %14" PRIu64 "\n", "  gaps that reached the book", gaps_seen);
        std::printf("%-32s %14" PRIu64 "\n", "  messages the book was told were lost",
                    messages_lost);
        std::printf("%-32s %14" PRIu64 "\n", "  markers that carried them", gap_markers);
        std::printf("%-32s %14" PRIu64 "\n", "  markers made to wait for room",
                    gap_deferrals);
        // Zero on every run that is not broken. It is printed anyway, because
        // the number that is always zero is the one nobody notices going
        // non-zero unless it is on the page next to the ones that are not.
        std::printf("%-32s %14" PRIu64 "\n", "  losses never announced",
                    unannounced_loss);

        // Phase 7's verdict inputs, on phase 10's pipeline. "Trusted" is a
        // claim about the book, and it is the half of the CORRECT / SAFE /
        // CAUTIOUS / WRONG grading that a book comparison cannot supply.
        std::printf("\n%-32s %14s\n", "feed state", recover::to_string(gap.state()));
        std::printf("%-32s %14s\n", "book trusted", gap.trusted() ? "yes" : "no");
        std::printf("%-32s %14" PRIu64 "\n", "rebuilds", gap.stats().rebuilds);
        std::printf("%-32s %14" PRIu64 "\n", "recoveries", gap.stats().recoveries);
        if (opt.kill_ring_occupancy > 0) {
            std::printf("%-32s %14s\n", "kill switch",
                        kill.live() ? "live" : risk::to_string(kill.report().reason));
            if (!kill.live()) {
                std::printf("%-32s %14lld (limit %lld)\n", "  tripped at occupancy",
                            static_cast<long long>(kill.report().observed),
                            static_cast<long long>(kill.report().limit));
            }
            std::printf("%-32s %14" PRIu64 "\n", "  peak occupancy seen",
                        kill.peak_ring_occupancy());
        }
        std::printf("%-32s %14" PRIu64 "\n", "malformed packets", malformed);
        std::printf("%-32s %14" PRIu64 "\n", "oversize messages refused", oversize);
        // Not "must be 0" -- it is 0 at every sustainable rate and non-zero
        // above the knee, and it is the ONE loss the sequencer cannot see. A
        // ring-full drop removes a whole packet, so the next packet's sequence
        // number exposes it and a gap gets declared. This removes messages from
        // the MIDDLE of a delivery: the sequencer has already advanced its
        // cursor past them, so nothing downstream would know to expect them.
        //
        // Which is why they are folded into the next marker rather than merely
        // counted. The book is told the same thing it is told about a declared
        // gap -- this many messages are missing, here -- and the run is still
        // reported as lossy rather than quoted as a latency, because a rate
        // that needs this is not a rate the pipeline can carry.
        std::printf("%-32s %14" PRIu64 "\n", "refused mid-block (folded into a gap)",
                    stage_overflow);

        // Messages the drop path removed from BEFORE the sequencer started and
        // from AFTER it last advanced. Neither can appear as a declared gap --
        // there is no surrounding sequence to interpolate between -- so they
        // get their own line rather than quietly not existing.
        const uint64_t head_loss = have_start ? seq_start - first_seq : last_end - first_seq;
        const uint64_t tail_loss = have_start && last_end > seq_end ? last_end - seq_end : 0;
        std::printf("%-32s %14" PRIu64 "\n", "lost before sequencer started", head_loss);
        std::printf("%-32s %14" PRIu64 "\n", "lost after sequencer's last", tail_loss);
        std::printf("%-32s %14" PRIu64 "\n", "messages on the wire",
                    last_end > first_seq ? last_end - first_seq : 0);

        std::printf("\nwire-to-book, nanoseconds (%zu samples)\n", overall.count());
        std::printf("%-32s %14.0f\n", "p50", ns(overall.percentile(50)));
        std::printf("%-32s %14.0f\n", "p99", ns(overall.percentile(99)));
        std::printf("%-32s %14.0f\n", "p99.9", ns(overall.percentile(99.9)));
        std::printf("%-32s %14.0f\n", "worst", ns(overall.max()));

        // Per type, from the recorded pairs. 'A' dominates the count and 'U'
        // does two books' worth of work in one message; an overall p99 that
        // mixes them describes no operation the machine actually performs.
        std::printf("\n%-8s %12s %12s %12s   %s\n",
                    "type", "count", "p50 ns", "p99 ns", "name");
        for (int t = 0; t < 256; ++t) {
            bench::Histogram h(1024);
            for (size_t i = 0; i < sample_type.size(); ++i) {
                if (sample_type[i] == t) h.add(sample_cycles[i]);
            }
            if (h.empty()) continue;
            h.finalize();
            std::printf("%-8c %12zu %12.0f %12.0f   %s\n", static_cast<char>(t),
                        h.count(), ns(h.percentile(50)), ns(h.percentile(99)),
                        itch::type_name(static_cast<char>(t)));
        }

        // The sample is stamped on one core and closed on another. On x86 that
        // is two reads of two TSCs, and whether their difference means anything
        // is the question tools/tsc_offset.cpp exists to answer. A latency of
        // "60 ns" from cores whose clocks sit 200 ns apart is a subtraction,
        // not a measurement.
        std::printf("\nCross-core sample: arrival is stamped on the receiver's core and the\n"
                    "close on the book's. Run tools/tsc_offset before quoting these numbers;\n"
                    "an offset larger than p50 makes p50 an artefact of the clocks.\n");

        if (overall.count() > opt.expect_messages) {
            std::printf("\nThe sample buffers reserved %zu and took %zu, so they reallocated\n"
                        "inside the measured region and the tail includes that cost.\n"
                        "Re-run with --expect-messages above %zu.\n",
                        opt.expect_messages, overall.count(), overall.count());
        }
        if (opt.applied_out != nullptr) {
            std::printf("\nRECORDING. --applied-out memcpys every message inside the measured\n"
                        "loop, so the figures above describe the recording as much as the\n"
                        "pipeline. This mode exists to prove what was applied, not how fast.\n");
        }
        if (!recv_pinned || !book_pinned) {
            std::printf("\nUNPINNED. docs/phase10-methodology.md section 4 requires pinning,\n"
                        "and phase 4 measured 19.3%% run-to-run variance without it. These\n"
                        "numbers describe a scheduler as much as a pipeline.\n");
        }
        if (!kernel_known) {
            std::printf("\nKernel drops are UNKNOWN on this platform, so \"no drops\" cannot be\n"
                        "claimed. A rate is only sustainable when BOTH counters are zero and\n"
                        "one of them is unreadable here.\n");
        }
    }

    if (opt.hist_csv != nullptr && !overall.write_buckets_csv(opt.hist_csv)) {
        std::fprintf(stderr, "error: cannot write %s\n", opt.hist_csv);
        return 1;
    }
    if (opt.per_symbol != nullptr && !book::write_per_symbol(books, opt.per_symbol)) {
        return 1;
    }
    if (opt.applied_out != nullptr) {
        gzFile g = gzopen(opt.applied_out, "wb");
        if (g == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", opt.applied_out);
            return 1;
        }
        const bool wrote = applied_bytes.empty() ||
                           gzwrite(g, applied_bytes.data(),
                                   static_cast<unsigned>(applied_bytes.size())) ==
                               static_cast<int>(applied_bytes.size());
        if (gzclose(g) != Z_OK || !wrote) {
            std::fprintf(stderr, "error: cannot write %s\n", opt.applied_out);
            return 1;
        }
    }
    if (opt.json != nullptr) {
        std::FILE* f = std::fopen(opt.json, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", opt.json);
            return 1;
        }
        std::fprintf(f,
            "{\n  \"clock\": \"%s\",\n  \"pinned_receiver\": %s,\n  \"pinned_book\": %s,\n"
            "  \"rcvbuf_bytes_granted\": %d,\n  \"ring_slots\": %zu,\n"
            "  \"packets\": %" PRIu64 ",\n  \"messages_into_ring\": %" PRIu64 ",\n"
            "  \"slots_into_ring\": %" PRIu64 ",\n"
            "  \"messages_applied\": %" PRIu64 ",\n  \"peak_ring_occupancy\": %zu,\n"
            "  \"books\": %zu,\n"
            "  \"ring_full_drops\": %" PRIu64 ",\n"
            "  \"kernel_drops\": %s,\n"
            "  \"sequence_gaps\": %" PRIu64 ",\n  \"messages_lost\": %" PRIu64 ",\n"
            "  \"oversize_messages\": %" PRIu64 ",\n"
            "  \"malformed_packets\": %" PRIu64 ",\n"
            "  \"staging_overflow\": %" PRIu64 ",\n"
            "  \"gaps_to_book\": %" PRIu64 ",\n"
            "  \"messages_lost_seen_by_book\": %" PRIu64 ",\n"
            "  \"gap_markers_deferred\": %" PRIu64 ",\n"
            "  \"gap_markers_to_book\": %" PRIu64 ",\n"
            "  \"gaps_lost_to_full_ring\": %" PRIu64 ",\n"
            "  \"state\": \"%s\",\n"
            "  \"trusted\": %s,\n"
            "  \"rebuilds\": %" PRIu64 ",\n"
            "  \"recoveries\": %" PRIu64 ",\n"
            "  \"kill_switch\": \"%s\",\n"
            "  \"kill_peak_occupancy\": %" PRIu64 ",\n"
            "  \"wire_messages\": %" PRIu64 ",\n"
            "  \"lost_before_sequencer\": %" PRIu64 ",\n"
            "  \"lost_after_sequencer\": %" PRIu64 ",\n"
            "  \"histogram_reserved\": %zu,\n"
            "  \"samples\": %zu,\n  \"cycles_per_ns\": %.6f,\n"
            "  \"wire_to_book_ns\": {\"p50\": %.0f, \"p99\": %.0f, \"p999\": %.0f, \"max\": %.0f}\n"
            "}\n",
            bench::clock_name(), recv_pinned ? "true" : "false",
            book_pinned ? "true" : "false", got, Ring::capacity(),
            shared.packets.load(), shared.pushed.load() - gaps_seen,
            shared.pushed.load(), applied,
            static_cast<size_t>(shared.max_occupancy.load()), books.books(),
            shared.ring_full_drops.load(),
            kernel_known ? std::to_string(kernel_lost).c_str() : "null",
            seq_stats.gaps, seq_stats.messages_lost, oversize, malformed,
            stage_overflow,
            gaps_seen, messages_lost, gap_deferrals, gap_markers, unannounced_loss,
            recover::to_string(gap.state()), gap.trusted() ? "true" : "false",
            gap.stats().rebuilds, gap.stats().recoveries,
            opt.kill_ring_occupancy == 0
                ? "disabled"
                : (kill.live() ? "live" : risk::to_string(kill.report().reason)),
            kill.peak_ring_occupancy(),
            last_end > first_seq ? last_end - first_seq : 0,
            have_start ? seq_start - first_seq : last_end - first_seq,
            have_start && last_end > seq_end ? last_end - seq_end : 0,
            opt.expect_messages,
            overall.count(), cyc_per_ns,
            ns(overall.percentile(50)), ns(overall.percentile(99)),
            ns(overall.percentile(99.9)), ns(overall.max()));
        if (std::fclose(f) != 0) return 1;
    }

    // ---- the identities ------------------------------------------------------
    //
    // Three of them, each failable, because "no messages were lost" is the one
    // claim this tool exists to make and the one a reader has no way to check.
    // The small-ring run that first exercised the drop path delivered 1,003
    // messages and declared 251,479 lost against 252,482 on the wire, which is
    // exact -- and it was exact by arithmetic done afterwards by hand. An
    // identity checked by hand once is an anecdote.
    const uint64_t head_loss = have_start ? seq_start - first_seq : last_end - first_seq;
    const uint64_t tail_loss = have_start && last_end > seq_end ? last_end - seq_end : 0;
    const uint64_t wire = last_end > first_seq ? last_end - first_seq : 0;
    int failures = 0;

    // 1. Every message the sequencer delivered was staged, refused for size, or
    //    refused for room. There is no fourth outcome.
    //
    // The ring now carries gap markers as well as messages, so what was pushed
    // is not all messages. Subtracting the gaps is not bookkeeping tidiness: if
    // it were left out the identity would fail on every lossy run, and an
    // identity that fails whenever the interesting thing happens gets switched
    // off within a week.
    const uint64_t pushed_messages = shared.pushed.load() - gaps_seen;
    if (seq_stats.messages != pushed_messages + oversize + stage_overflow) {
        std::fprintf(stderr, "\nFAIL: sequencer delivered %" PRIu64 " but %" PRIu64
                             " staged + %" PRIu64 " oversize + %" PRIu64 " overflow.\n",
                     seq_stats.messages, pushed_messages, oversize, stage_overflow);
        ++failures;
    }
    // 1b. EVERY MISSING MESSAGE WAS ANNOUNCED, AND THE COUNT WAS RIGHT.
    //
    //     Three things remove a message from the stream after the sequencer has
    //     already advanced past it: a gap it declared, a message too large for
    //     a slot, and a message the staging budget could not take. All three are
    //     holes the book cannot infer, and all three now travel as gap markers.
    //     So the messages the book was told were missing must equal the
    //     messages that actually went missing.
    //
    //     Counting MARKERS against the sequencer's gap count -- which is what
    //     this check used to do -- stopped being possible when markers began to
    //     coalesce, and that is the right trade: the old check compared how
    //     OFTEN loss was announced, this one compares how MUCH, and how much is
    //     what decides whether the book rebuilds at the right place.
    //
    //     `unannounced_loss` is what the producer could not announce before the
    //     stream ended. It is zero on any run that is not broken, and it is
    //     inside the identity rather than outside it so that the sum still has
    //     to close on a run that is.
    if (messages_lost + unannounced_loss !=
        seq_stats.messages_lost + oversize + stage_overflow) {
        std::fprintf(stderr, "\nFAIL: %" PRIu64 " messages went missing (%" PRIu64
                             " to declared gaps, %" PRIu64 " oversize, %" PRIu64
                             " refused mid-block),\nbut the book was told about %" PRIu64
                             " and %" PRIu64 " were never announced.\n",
                     seq_stats.messages_lost + oversize + stage_overflow,
                     seq_stats.messages_lost, oversize, stage_overflow,
                     messages_lost, unannounced_loss);
        ++failures;
    }
    // 1c. And every marker the producer staged came out the other side. This is
    //     conservation across the thread boundary for the slots that are not
    //     messages -- the ones the check above cannot see, because it works in
    //     messages and a marker is not one.
    if (gaps_seen != gap_markers) {
        std::fprintf(stderr, "\nFAIL: producer staged %" PRIu64 " gap markers, the book "
                             "saw %" PRIu64 ".\n", gap_markers, gaps_seen);
        ++failures;
    }
    // 2. The sequencer's cursor moved exactly once per message it delivered and
    //    once per message it declared lost.
    if (have_start && seq_end - seq_start != seq_stats.messages + seq_stats.messages_lost) {
        std::fprintf(stderr, "\nFAIL: sequence advanced %" PRIu64 " over %" PRIu64
                             " delivered + %" PRIu64 " lost.\n",
                     seq_end - seq_start, seq_stats.messages, seq_stats.messages_lost);
        ++failures;
    }
    // 3. And nothing fell between the wire and those two buckets.
    if (wire != head_loss + seq_stats.messages + seq_stats.messages_lost + tail_loss) {
        std::fprintf(stderr, "\nFAIL: %" PRIu64 " on the wire, %" PRIu64 " accounted for.\n",
                     wire, head_loss + seq_stats.messages + seq_stats.messages_lost + tail_loss);
        ++failures;
    }
    if (failures != 0) return 1;

    // A hole the ring could not announce is the one failure mode this whole
    // design was built to avoid: the consumer goes on applying the messages
    // after it without ever being told anything is missing. Backpressure is
    // supposed to BECOME a graded gap, not erase one.
    //
    // Since 10.10 a marker waits for a slot instead of being discarded, and
    // nothing may be staged in front of a waiting one, so reaching here means
    // the consumer stopped draining entirely -- not that the load was too high.
    if (unannounced_loss != 0) {
        std::fprintf(stderr, "\nFAIL: %" PRIu64 " missing messages were never announced, "
                             "so the book applied\nthe messages after them without being "
                             "told anything was missing.\n", unannounced_loss);
        return 1;
    }

    // Conservation across the thread boundary: everything the ring accepted
    // came out of it. This is the one failure the whole structure exists to
    // make impossible.
    if (consumed != shared.pushed.load()) {
        std::fprintf(stderr, "\nFAIL: %" PRIu64 " messages entered the ring and %" PRIu64
                             " came out.\n", shared.pushed.load(), consumed);
        return 1;
    }
    // ---- and finally: was this rate sustainable? -----------------------------
    //
    // Exit 3, not 1. The identities above are correctness -- a 1 means the
    // pipeline is broken and no number it printed means anything. This is
    // capacity: the pipeline worked exactly as designed and the offered load
    // was more than it could absorb. A sweep needs to tell those apart, because
    // one ends the sweep and the other IS the sweep: the rate at which this
    // first turns non-zero is the knee phase 10.7 is looking for.
    //
    // Kernel drops count here too. On loopback the socket buffer overflows
    // before the ring does, so a gate that watched only ring drops would call a
    // rate sustainable while the kernel quietly discarded thousands of
    // datagrams upstream -- phase 7's failure mode one layer higher.
    const bool lossy = shared.ring_full_drops.load() != 0 || stage_overflow != 0 ||
                       (kernel_known && kernel_lost != 0);
    if (lossy) {
        std::fprintf(stderr,
                     "\nLOSSY: %" PRIu64 " packets dropped ring-full, %" PRIu64
                     " messages refused mid-block, %" PRIu64 " dropped by the kernel.\n"
                     "The latency figures above describe a saturated pipeline, not a\n"
                     "sustainable rate.\n",
                     shared.ring_full_drops.load(), stage_overflow,
                     kernel_known ? kernel_lost : 0);
        return 3;
    }
    if (!kernel_known) {
        std::fprintf(stderr,
                     "\nUNVERIFIED: ring drops were zero but kernel drops are unreadable on\n"
                     "this platform, so this rate cannot be called sustainable.\n");
        return 4;
    }
    return 0;
}

}  // namespace

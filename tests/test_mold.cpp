// test_mold — the four things a file cannot do to you and a network can.
//
// Loss, reordering, duplication and silence. The first two are the pair that
// matters, because at the instant a sequence number jumps they are
// indistinguishable and they require opposite responses: wait for one, resync
// on the other. Every case below asserts on the MESSAGES that came out the far
// end, not on the counters, because the counters agreeing while the message
// stream is wrong is exactly the failure this is meant to catch.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/mold/sequencer.hpp"
#include "tests/check.hpp"

using namespace itchbook::mold;

namespace {

// Records what actually reached the book.
struct Sink {
    std::string seen;                       // one char per message, in order
    std::vector<std::pair<uint64_t, uint64_t>> gaps;

    void on_message(char type, const uint8_t*, uint16_t) { seen.push_back(type); }
    void on_gap(uint64_t first, uint64_t count) { gaps.emplace_back(first, count); }
};

// A packet carrying one-byte messages, so the payload of message N is just a
// character and the delivered stream is a readable string.
std::vector<uint8_t> packet(uint64_t seq, const std::string& msgs,
                            const char* session = "TEST") {
    std::vector<uint8_t> p(kHeaderLen);
    write_header(p.data(), session, seq, static_cast<uint16_t>(msgs.size()));
    for (char c : msgs) {
        uint8_t lb[2];
        write_be16(lb, 1);
        p.push_back(lb[0]);
        p.push_back(lb[1]);
        p.push_back(static_cast<uint8_t>(c));
    }
    return p;
}

std::vector<uint8_t> heartbeat(uint64_t seq, const char* session = "TEST") {
    std::vector<uint8_t> p(kHeaderLen);
    write_header(p.data(), session, seq, 0);
    return p;
}

std::vector<uint8_t> end_of_session(uint64_t seq, const char* session = "TEST") {
    std::vector<uint8_t> p(kHeaderLen);
    write_header(p.data(), session, seq, kEndOfSession);
    return p;
}

void feed(Sequencer<Sink>& s, Sink& sink, const std::vector<uint8_t>& p) {
    s.on_packet(p.data(), p.size(), sink);
}

void test_a_clean_stream_arrives_intact() {
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "abc"));
    feed(s, sink, packet(4, "de"));
    feed(s, sink, packet(6, "fgh"));
    CHECK_STR(sink.seen, std::string("abcdefgh"));
    CHECK_EQ(sink.gaps.size(), 0u);
    CHECK_EQ(s.stats().messages, 8u);
    CHECK_EQ(s.expected(), 9u);
}

void test_a_reordered_packet_is_not_a_gap() {
    // The case that makes a naive receiver resync all day. Packet 4 arrives
    // after packet 6; nothing was lost and nothing may be reported lost.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "abc"));
    feed(s, sink, packet(6, "fgh"));      // early
    feed(s, sink, packet(4, "de"));       // fills the hole
    CHECK_STR(sink.seen, std::string("abcdefgh"));
    CHECK_EQ(sink.gaps.size(), 0u);
    CHECK_EQ(s.stats().reordered_packets, 1u);
    CHECK_EQ(s.stats().messages_lost, 0u);
}

void test_reordering_across_several_packets_still_resolves() {
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "ab"));
    feed(s, sink, packet(7, "g"));
    feed(s, sink, packet(5, "ef"));
    feed(s, sink, packet(3, "cd"));       // the one everything was waiting on
    CHECK_STR(sink.seen, std::string("abcdefg"));
    CHECK_EQ(sink.gaps.size(), 0u);
}

void test_a_real_gap_is_declared_once_patience_runs_out() {
    SequencerConfig c;
    c.reorder_depth = 3;
    Sink sink;
    Sequencer<Sink> s{c};
    feed(s, sink, packet(1, "ab"));
    // 3 and 4 never arrive. Enough later packets pile up to fill the buffer.
    feed(s, sink, packet(5, "e"));
    feed(s, sink, packet(6, "f"));
    feed(s, sink, packet(7, "g"));
    CHECK_EQ(sink.gaps.size(), 1u);
    CHECK_EQ(sink.gaps[0].first, 3u);     // first missing sequence
    CHECK_EQ(sink.gaps[0].second, 2u);    // two messages gone
    CHECK_STR(sink.seen, std::string("abefg"));
    CHECK_EQ(s.stats().messages_lost, 2u);
}

void test_a_duplicate_packet_is_applied_once() {
    // Applying it twice double-counts every execution in it, which a book
    // cannot detect and a P&L quietly inherits.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "abc"));
    feed(s, sink, packet(1, "abc"));      // exact retransmission
    feed(s, sink, packet(4, "d"));
    CHECK_STR(sink.seen, std::string("abcd"));
    CHECK_EQ(s.stats().duplicate_packets, 1u);
    CHECK_EQ(s.stats().duplicate_messages, 3u);
}

void test_a_partially_overlapping_retransmission_delivers_only_its_tail() {
    // A retransmission that starts before where we are. Dropping it loses the
    // messages past `expected`; applying all of it replays the ones before.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "ab"));       // expected is now 3
    feed(s, sink, packet(2, "bcd"));      // covers 2,3,4 — only 3,4 are new
    CHECK_STR(sink.seen, std::string("abcd"));
    CHECK_EQ(s.stats().duplicate_messages, 1u);
    CHECK_EQ(s.expected(), 5u);
}

void test_a_heartbeat_reveals_loss_during_silence() {
    // On a quiet symbol nothing else will ever tell you. Without this, "the
    // market is quiet" and "my link is down" are the same observation.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "ab"));       // expected 3
    feed(s, sink, heartbeat(9));          // sender says it has sent up to 8
    CHECK_EQ(sink.gaps.size(), 1u);
    CHECK_EQ(sink.gaps[0].first, 3u);
    CHECK_EQ(sink.gaps[0].second, 6u);
    CHECK_EQ(s.stats().heartbeats, 1u);
}

void test_a_heartbeat_in_a_healthy_stream_says_nothing() {
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "ab"));
    feed(s, sink, heartbeat(3));          // exactly where we are: all is well
    feed(s, sink, packet(3, "cd"));
    CHECK_STR(sink.seen, std::string("abcd"));
    CHECK_EQ(sink.gaps.size(), 0u);
}

void test_end_of_session_carries_no_messages() {
    // Count 0xFFFF is a marker, not 65,535 messages. A receiver that reads it
    // as a plain integer walks off the end of the buffer here, and only ever
    // at 16:00.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "ab"));
    feed(s, sink, end_of_session(3));
    CHECK_STR(sink.seen, std::string("ab"));
    CHECK(s.ended());
    CHECK_EQ(s.stats().messages, 2u);
}

void test_a_truncated_packet_is_loss_not_corruption_downstream() {
    // The header promises four messages and the bytes hold two. The two that
    // are there are valid and must be delivered; the two that are not are a
    // gap, declared now rather than left to manufacture a phantom gap later.
    Sink sink;
    Sequencer<Sink> s;
    std::vector<uint8_t> p = packet(1, "abcd");
    p.resize(p.size() - 6);               // lop off the last two blocks
    feed(s, sink, p);
    CHECK_STR(sink.seen, std::string("ab"));
    CHECK_EQ(sink.gaps.size(), 1u);
    CHECK_EQ(sink.gaps[0].second, 2u);
    CHECK_EQ(s.stats().truncated_packets, 1u);
    CHECK_EQ(s.expected(), 5u);
}

void test_a_new_session_resets_the_sequence_space() {
    // Sequence numbers are per session. Carrying the old expectation across
    // would declare a gap of however far the previous session got.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(500, "ab", "MORNING"));
    feed(s, sink, packet(1, "cd", "AFTERNOON"));
    CHECK_STR(sink.seen, std::string("abcd"));
    CHECK_EQ(sink.gaps.size(), 0u);
    CHECK_EQ(s.stats().session_changes, 1u);
}

void test_an_unfilled_gap_is_reported_at_the_end_of_the_stream() {
    // A gap still open at end-of-day is still a gap. Without this the session
    // finishes looking clean, which is the quiet failure the whole phase is
    // about.
    Sink sink;
    Sequencer<Sink> s;
    feed(s, sink, packet(1, "ab"));
    feed(s, sink, packet(9, "z"));        // buffered, never resolved
    CHECK_EQ(sink.gaps.size(), 0u);       // patient, so far
    s.flush(sink);
    CHECK_EQ(sink.gaps.size(), 1u);
    CHECK_EQ(sink.gaps[0].first, 3u);
    CHECK_EQ(sink.gaps[0].second, 6u);
    CHECK_STR(sink.seen, std::string("abz"));
}

}  // namespace

int main() {
    test_a_clean_stream_arrives_intact();
    test_a_reordered_packet_is_not_a_gap();
    test_reordering_across_several_packets_still_resolves();
    test_a_real_gap_is_declared_once_patience_runs_out();
    test_a_duplicate_packet_is_applied_once();
    test_a_partially_overlapping_retransmission_delivers_only_its_tail();
    test_a_heartbeat_reveals_loss_during_silence();
    test_a_heartbeat_in_a_healthy_stream_says_nothing();
    test_end_of_session_carries_no_messages();
    test_a_truncated_packet_is_loss_not_corruption_downstream();
    test_a_new_session_resets_the_sequence_space();
    test_an_unfilled_gap_is_reported_at_the_end_of_the_stream();
    return REPORT();
}

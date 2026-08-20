#pragma once
//
// gap_policy.hpp — what to do about a hole, when you cannot ask for the
// missing bytes.
//
// The build plan says: "On a gap, discard book state, request/replay a
// snapshot, resume. Do not silently continue with a corrupt book." The middle
// clause is the one we cannot honour literally, and pretending otherwise would
// be the worst thing in this file.
//
// NASDAQ's real answer is two services this project does not have: a
// retransmission server that will resend sequence numbers on request, and
// GLIMPSE, a snapshot feed you connect to and receive the current book from.
// Working from a historical file there is no one to ask. A snapshot we took
// ourselves is no help either — it is from before the gap, so it is stale by
// exactly the messages we are missing, which is the definition of not a fix.
//
// So the honest choices are these, and both are implemented:
//
//   * **Halt.** Stop. The book is wrong and nothing will make it right, so
//     refuse to serve it. Always correct, never useful past the first packet
//     loss of the day.
//   * **Rebuild forward.** Discard the book and rebuild from the next message
//     on. This is the interesting one, and its property is worth stating
//     precisely: the resulting book contains NO WRONG ORDERS, only missing
//     ones. Every order it holds was added by a message we actually received.
//     It is a subset of the truth, and a subset is a thing you can reason
//     about — a quote from it is never invented, it is only sometimes absent.
//
// Rebuild-forward has a convergence property, and the feed itself tells you
// when you have converged. Every message that deletes, executes or replaces an
// order the book has never heard of is a message about the pre-gap world we
// threw away. As those old orders age out of the real market, that rate falls
// to zero — and when it has been zero for long enough, everything still
// resting is something we saw added, and the book is whole again.
//
// That is the measurement this file exists for. `unknown_ref` is not a
// diagnostic counter here; it is the recovery signal.
//
// What is NOT claimed: that the book is correct during recovery. It is
// explicitly untrusted, everything downstream is told so, and the state is
// reported rather than inferred. Never silently wrong is the requirement; the
// word doing the work is *silently*.
//
#include <cstdint>

namespace itchbook::recover {

enum class State : uint8_t {
    // Nothing has been lost. The book is a faithful reconstruction.
    Trusted,
    // A gap was declared and the book was rebuilt from the following message.
    // It holds no wrong orders and an unknown number of missing ones.
    Recovering,
    // Stopped. Either the policy is Halt, or recovery gave up.
    Halted,
};

inline const char* to_string(State s) {
    switch (s) {
        case State::Trusted:    return "trusted";
        case State::Recovering: return "recovering";
        case State::Halted:     return "halted";
    }
    return "?";
}

enum class Policy : uint8_t {
    // Discard and rebuild from the next message. Converges.
    RebuildForward,
    // Stop dead. Correct, and useless after the first loss of the day.
    Halt,
};

struct GapConfig {
    Policy policy = Policy::RebuildForward;
    // Consecutive reference-carrying messages that must all resolve before the
    // book is called trusted again. This is the convergence bar and it is a
    // judgement, not a derivation: too low and a quiet minute is mistaken for
    // recovery, too high and a book that is already whole stays sidelined.
    // 50,000 is roughly a busy minute on a liquid name.
    uint64_t clean_messages_to_recover = 50000;
    // Give up after this many gaps. A feed losing packets steadily is not a
    // feed you are recovering from, it is a feed you are not receiving, and a
    // system that resyncs forever looks healthy while being blind.
    uint64_t max_gaps_before_halt = 100;
};

struct GapStats {
    uint64_t gaps = 0;
    uint64_t messages_lost = 0;
    uint64_t rebuilds = 0;
    uint64_t recoveries = 0;         // times the book came back to Trusted
    uint64_t unknown_refs_after_gap = 0;
    uint64_t messages_untrusted = 0; // messages applied while not Trusted
};

// Tracks recovery state. Deliberately owns no book: the caller applies the
// messages and does the clearing, because the thing that knows how to rebuild
// a book is the book, and a policy object that reaches into one would be the
// second place in this system that knows how the book works.
class GapTracker {
public:
    GapTracker() = default;
    explicit GapTracker(GapConfig cfg) : cfg_(cfg) {}

    // A gap was declared. Returns true if the caller must clear the book.
    bool on_gap(uint64_t first_missing, uint64_t count) {
        ++stats_.gaps;
        stats_.messages_lost += count;
        last_gap_at_ = first_missing;
        clean_run_ = 0;

        if (cfg_.policy == Policy::Halt || stats_.gaps > cfg_.max_gaps_before_halt) {
            state_ = State::Halted;
            return false;
        }
        state_ = State::Recovering;
        ++stats_.rebuilds;
        return true;
    }

    // Called after each message that carries an order reference, with whether
    // the book resolved it. `resolved == false` while recovering is not an
    // error: it is a message about the pre-gap world, and it is the signal
    // that recovery is not finished.
    void on_reference(bool resolved) {
        if (state_ != State::Recovering) return;
        ++stats_.messages_untrusted;
        if (!resolved) {
            ++stats_.unknown_refs_after_gap;
            clean_run_ = 0;
            return;
        }
        if (++clean_run_ >= cfg_.clean_messages_to_recover) {
            state_ = State::Trusted;
            ++stats_.recoveries;
            clean_run_ = 0;
        }
    }

    // May the book be used to make decisions?
    bool trusted() const { return state_ == State::Trusted; }
    bool halted() const { return state_ == State::Halted; }
    State state() const { return state_; }
    const GapStats& stats() const { return stats_; }
    uint64_t clean_run() const { return clean_run_; }
    uint64_t last_gap_at() const { return last_gap_at_; }

private:
    GapConfig cfg_;
    GapStats stats_;
    State state_ = State::Trusted;
    uint64_t clean_run_ = 0;
    uint64_t last_gap_at_ = 0;
};

}  // namespace itchbook::recover

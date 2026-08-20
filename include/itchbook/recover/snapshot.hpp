#pragma once
//
// snapshot.hpp — coming back after the process died.
//
// This is where a snapshot finally earns its keep. It is no use at all for a
// sequence gap — a snapshot taken before the gap is stale by exactly the
// messages you are missing — but it is precisely the right tool for the other
// discontinuity in the build plan: "reconstruct position and open orders after
// a mid-day process restart."
//
// The difference is where the missing information is. After a gap, the bytes
// were never received and nothing on disk can recover them. After a crash, the
// bytes were received and processed; what was lost is the process's memory,
// and memory is a thing you can write down.
//
// The property this file has to deliver, and the one the tests assert:
//
//     snapshot at message N, restart, replay from N
//         == a process that ran continuously through N
//
// Not "close to". Identical — same orders, same queue positions, same tape.
// Anything less means a restarted process quietly disagrees with the one it
// replaced, and the disagreement is invisible until it costs money.
//
// The detail that makes or breaks it is ORDER. Orders are written in fill
// order — best price first, oldest first within a price — and restored by
// replaying adds, because add() appends to the back of a level. Dump the ref
// map in hash order instead and you restore a book with the right levels, the
// right shares at every level, and scrambled queue priority: every level-based
// check passes, every queue model is silently wrong, and phase 6's entire
// output becomes noise. It is the kind of bug that survives a code review
// because the book "looks right".
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "itchbook/book/book.hpp"

namespace itchbook::recover {

inline constexpr uint32_t kSnapshotMagic = 0x49544348;   // "ITCH"
inline constexpr uint32_t kSnapshotVersion = 1;

// Everything a restarting process needs that is not in the feed.
struct Snapshot {
    // Where to resume. A restart replays from here, so an off-by-one is a
    // duplicated or a dropped message — which is why it is written explicitly
    // rather than inferred from the last timestamp.
    uint64_t next_sequence = 0;
    uint64_t last_timestamp = 0;
    book::Book::TapeState tape;

    struct OrderRecord {
        uint64_t ref;
        int32_t price;
        uint32_t shares;
        char side;
    };
    // In fill order. See the header comment — this is not an implementation
    // detail, it is the correctness condition.
    std::vector<OrderRecord> orders;
};

// ---- little-endian scalars, because this file never leaves the host ---------
//
// The wire format is big-endian because NASDAQ says so. This one is ours, and
// matching the host's byte order keeps the reader honest about what it is:
// process state, not a protocol.

template <typename T>
inline void put(std::vector<uint8_t>* out, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out->insert(out->end(), p, p + sizeof(T));
}

template <typename T>
inline bool get(const uint8_t* buf, size_t len, size_t* off, T* v) {
    if (*off + sizeof(T) > len) return false;
    std::memcpy(v, buf + *off, sizeof(T));
    *off += sizeof(T);
    return true;
}

inline std::vector<uint8_t> serialize(const Snapshot& s) {
    std::vector<uint8_t> out;
    put(&out, kSnapshotMagic);
    put(&out, kSnapshotVersion);
    put(&out, s.next_sequence);
    put(&out, s.last_timestamp);
    put(&out, s.tape.volume);
    put(&out, s.tape.notional);
    put(&out, s.tape.trades);
    put(&out, s.tape.hidden_volume);
    put(&out, s.tape.cross_volume);
    put(&out, s.tape.unknown_ref);
    put(&out, s.tape.open);
    put(&out, s.tape.high);
    put(&out, s.tape.low);
    put(&out, s.tape.close);
    put(&out, s.tape.trading_state);
    put(&out, s.tape.system_event);
    put(&out, static_cast<uint64_t>(s.orders.size()));
    for (const Snapshot::OrderRecord& o : s.orders) {
        put(&out, o.ref);
        put(&out, o.price);
        put(&out, o.shares);
        put(&out, o.side);
    }
    return out;
}

inline bool deserialize(const uint8_t* buf, size_t len, Snapshot* s) {
    size_t off = 0;
    uint32_t magic = 0, version = 0;
    if (!get(buf, len, &off, &magic) || magic != kSnapshotMagic) return false;
    // A version check that refuses rather than guesses. Reading a v2 snapshot
    // with v1 code produces a book that is subtly wrong and does not say so,
    // which is worse than not starting.
    if (!get(buf, len, &off, &version) || version != kSnapshotVersion) return false;
    if (!get(buf, len, &off, &s->next_sequence)) return false;
    if (!get(buf, len, &off, &s->last_timestamp)) return false;
    if (!get(buf, len, &off, &s->tape.volume)) return false;
    if (!get(buf, len, &off, &s->tape.notional)) return false;
    if (!get(buf, len, &off, &s->tape.trades)) return false;
    if (!get(buf, len, &off, &s->tape.hidden_volume)) return false;
    if (!get(buf, len, &off, &s->tape.cross_volume)) return false;
    if (!get(buf, len, &off, &s->tape.unknown_ref)) return false;
    if (!get(buf, len, &off, &s->tape.open)) return false;
    if (!get(buf, len, &off, &s->tape.high)) return false;
    if (!get(buf, len, &off, &s->tape.low)) return false;
    if (!get(buf, len, &off, &s->tape.close)) return false;
    if (!get(buf, len, &off, &s->tape.trading_state)) return false;
    if (!get(buf, len, &off, &s->tape.system_event)) return false;
    uint64_t count = 0;
    if (!get(buf, len, &off, &count)) return false;
    // A truncated snapshot restores a partial book that looks entirely
    // plausible. Check the length before trusting the count, exactly as the
    // packet reader does with a MoldUDP64 header.
    const size_t per_order = sizeof(uint64_t) + sizeof(int32_t) + sizeof(uint32_t) +
                             sizeof(char);
    if (count > (len - off) / per_order) return false;
    s->orders.clear();
    s->orders.reserve(count);
    for (uint64_t i = 0; i < count; ++i) {
        Snapshot::OrderRecord o{};
        if (!get(buf, len, &off, &o.ref)) return false;
        if (!get(buf, len, &off, &o.price)) return false;
        if (!get(buf, len, &off, &o.shares)) return false;
        if (!get(buf, len, &off, &o.side)) return false;
        s->orders.push_back(o);
    }
    return true;
}

// ---- capture and restore ----------------------------------------------------

inline Snapshot capture(const book::Book& b, uint64_t next_sequence,
                        uint64_t last_timestamp) {
    Snapshot s;
    s.next_sequence = next_sequence;
    s.last_timestamp = last_timestamp;
    s.tape = b.tape();
    // Bids then asks, each in fill order. Restoring replays them as adds, and
    // add() appends, so the FIFO comes back exactly as it was.
    for (char side : {'B', 'S'}) {
        b.for_each_order(side, [&](const book::Order& o) {
            s.orders.push_back(Snapshot::OrderRecord{
                o.ref, o.price, o.shares, static_cast<char>(o.side)});
        });
    }
    return s;
}

inline void restore(book::Book* b, const Snapshot& s) {
    b->clear_orders();
    for (const Snapshot::OrderRecord& o : s.orders) {
        b->add(o.ref, o.side, o.price, o.shares);
    }
    b->restore_tape(s.tape);
}

// ---- files ------------------------------------------------------------------

inline bool write_file(const std::string& path, const Snapshot& s) {
    const std::vector<uint8_t> bytes = serialize(s);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    return n == bytes.size();
}

inline bool read_file(const std::string& path, Snapshot* s) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> bytes;
    uint8_t chunk[8192];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + n);
    }
    std::fclose(f);
    return deserialize(bytes.data(), bytes.size(), s);
}

}  // namespace itchbook::recover

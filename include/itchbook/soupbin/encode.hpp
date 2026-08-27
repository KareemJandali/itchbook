#pragma once
//
// encode.hpp — writing SoupBinTCP 3.00, the inverse of messages.hpp.
//
// See that file's banner for the evidence class every offset carries, and why
// this protocol gets its own be16/put16 rather than reaching into itch's,
// emit's or ouch's.
//
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchbook::soupbin::encode {

inline void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

// Right-padded with spaces (section 1.5 / the Login Request notes: "Both
// Username and Password are case-insensitive and should be padded on the
// right with spaces").
inline void put_right_padded(uint8_t* p, const char* s, size_t n) {
    size_t i = 0;
    for (; i < n && s != nullptr && s[i] != '\0'; ++i) p[i] = static_cast<uint8_t>(s[i]);
    for (; i < n; ++i) p[i] = ' ';
}

// Left-padded with spaces — the explicit rule for Login Accepted's Session
// and Sequence Number fields, and applied by analogy (see messages.hpp) to
// Login Request's Requested Session and Requested Sequence Number, which the
// spec does not restate a direction for.
inline void put_left_padded(uint8_t* p, const char* s, size_t n) {
    const size_t len = s == nullptr ? 0 : std::strlen(s);
    const size_t take = len < n ? len : n;
    const size_t pad = n - take;
    for (size_t i = 0; i < pad; ++i) p[i] = ' ';
    if (take > 0) std::memcpy(p + pad, s, take);
}

// Every packet is framed the same way: 2-byte length, 1-byte type, payload.
// `type_and_payload_len` is Type(1) + payload — the value the Packet Length
// FIELD holds, not the total wire size. Returns the total bytes written
// (2 + type_and_payload_len).
inline size_t frame(uint8_t* o, char type, uint16_t type_and_payload_len) {
    put16(o, type_and_payload_len);
    o[2] = static_cast<uint8_t>(type);
    return static_cast<size_t>(type_and_payload_len) + 2;
}

// ---- server-sent ----------------------------------------------------------------

inline size_t debug(uint8_t* o, const char* text, size_t text_len) {
    const size_t n = frame(o, '+', static_cast<uint16_t>(1 + text_len));
    if (text_len > 0) std::memcpy(o + 3, text, text_len);
    return n;
}

inline size_t login_accepted(uint8_t* o, const char* session, const char* sequence_number) {
    const size_t n = frame(o, 'A', 1 + 10 + 20);
    put_left_padded(o + 3, session, 10);
    put_left_padded(o + 13, sequence_number, 20);
    return n;
}

inline size_t login_rejected(uint8_t* o, char reason) {
    const size_t n = frame(o, 'J', 1 + 1);
    o[3] = static_cast<uint8_t>(reason);
    return n;
}

inline size_t sequenced_data(uint8_t* o, const uint8_t* message, size_t message_len) {
    const size_t n = frame(o, 'S', static_cast<uint16_t>(1 + message_len));
    if (message_len > 0) std::memcpy(o + 3, message, message_len);
    return n;
}

inline size_t server_heartbeat(uint8_t* o) { return frame(o, 'H', 1); }

inline size_t end_of_session(uint8_t* o) { return frame(o, 'Z', 1); }

// ---- client-sent ----------------------------------------------------------------

inline size_t login_request(uint8_t* o, const char* username, const char* password,
                            const char* requested_session,
                            const char* requested_sequence_number) {
    const size_t n = frame(o, 'L', 1 + 6 + 10 + 10 + 20);
    put_right_padded(o + 3, username, 6);
    put_right_padded(o + 9, password, 10);
    put_left_padded(o + 19, requested_session, 10);
    put_left_padded(o + 29, requested_sequence_number, 20);
    return n;
}

inline size_t unsequenced_data(uint8_t* o, const uint8_t* message, size_t message_len) {
    const size_t n = frame(o, 'U', static_cast<uint16_t>(1 + message_len));
    if (message_len > 0) std::memcpy(o + 3, message, message_len);
    return n;
}

inline size_t client_heartbeat(uint8_t* o) { return frame(o, 'R', 1); }

inline size_t logout_request(uint8_t* o) { return frame(o, 'O', 1); }

}  // namespace itchbook::soupbin::encode

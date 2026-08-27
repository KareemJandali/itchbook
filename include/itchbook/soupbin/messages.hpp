#pragma once
//
// messages.hpp — SoupBinTCP 3.00 field access.
//
// SoupBinTCP is the session layer OUCH (and, in principle, any higher-level
// protocol) rides on: login, heartbeats, and an envelope around whatever the
// higher-level protocol actually carries. Where OUCH is one flat 49-or-66-byte
// struct per message, SoupBinTCP's own packets are almost all EMPTY —
// heartbeats and end-of-session carry no payload at all — because its whole
// job is timing and framing, not content.
//
// SOURCE: "SoupBinTCP Version 3.00", NASDAQ,
// https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/soupbintcp.pdf
//
// EVIDENCE CLASS — same ceiling as ouch/messages.hpp, for the same reason: no
// live SoupBinTCP session exists or will exist for this project, so every
// offset below is spec-only, for every packet type, with no exception. What
// differs from a single read is that a second, independent extraction of the
// vendor PDF reproduced the same field tables with no gaps or overlaps before
// any of this became code — see docs/build-plan-9-12.md's 12.4 write-up for
// what that pass found and did not find.
//
// FRAMING (every packet, both directions):
//   Packet Length   offset 0  len 2  Integer, big-endian
//   Packet Type     offset 2  len 1  Alpha
//   payload         offset 3  len variable, packet-type-dependent
// Packet Length's value is the byte count of EVERYTHING AFTER ITSELF — Type
// plus payload, NOT including the 2-byte length field. So for a fixed-size
// packet of N total wire bytes, the length field's value is N-2, and the
// physical bytes on the wire (what a socket read actually sees) are 2 + that
// value. Getting this backwards corrupts every framing calculation that
// follows it, which is exactly why frame_length() below is the one function
// every reader of this file should use rather than re-deriving the +2/-2
// arithmetic at each call site.
//
#include <cstddef>
#include <cstdint>

namespace itchbook::soupbin {

inline uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// The value stored in the Packet Length field: Type(1) + payload.
inline uint16_t declared_length(const uint8_t* p) { return be16(p); }

// Total physical bytes this packet occupies on the wire, INCLUDING the
// 2-byte length field itself — what a caller needs to know how far to
// advance a read buffer.
inline size_t frame_length(const uint8_t* p) {
    return static_cast<size_t>(declared_length(p)) + 2;
}

inline char packet_type(const uint8_t* p) { return static_cast<char>(p[2]); }

// ---- server-sent packets ------------------------------------------------------

// Debug Packet ('+'). Either side may send one at any time; both client and
// server application software are meant to ignore it. Free-form text, no
// fixed length.
namespace debug {
inline const uint8_t* text(const uint8_t* p) { return p + 3; }
// declared_length(p) - 1 (Type) gives the text's byte length.
inline size_t text_length(const uint8_t* p) {
    return static_cast<size_t>(declared_length(p)) - 1;
}
}  // namespace debug

// Login Accepted Packet ('A'). Always the first non-debug packet the server
// sends after a successful login. 33 bytes on the wire (31 declared + 2).
namespace login_accepted {
inline const uint8_t* session(const uint8_t* p) { return p + 3; }    //  3..12, left-padded
inline const uint8_t* sequence_number(const uint8_t* p) { return p + 13; }  // 13..32, left-padded ASCII
}  // namespace login_accepted
inline constexpr size_t kLoginAcceptedWireBytes = 33;

// Login Rejected Packet ('J'). The server's only non-debug reply on a failed
// login; the server closes the socket after sending it. 4 bytes on the wire.
namespace login_rejected {
// 'A' = Not Authorized (bad username/password). 'S' = Session not available
// (the Requested Session in the Login Request was invalid or unavailable).
inline char reason(const uint8_t* p) { return static_cast<char>(p[3]); }
}  // namespace login_rejected
inline constexpr size_t kLoginRejectedWireBytes = 4;

// Sequenced Data Packet ('S'). One higher-level message per packet.
// Sequence numbers are never carried on the wire — both sides count locally,
// starting from the number the Login Accepted Packet stated, incrementing by
// one per Sequenced Data Packet. A gap in that local count IS the detectable
// loss signal; there is no other one.
namespace sequenced_data {
inline const uint8_t* message(const uint8_t* p) { return p + 3; }
inline size_t message_length(const uint8_t* p) {
    return static_cast<size_t>(declared_length(p)) - 1;
}
}  // namespace sequenced_data

// Server Heartbeat Packet ('H'). No payload. 3 bytes on the wire.
inline constexpr size_t kServerHeartbeatWireBytes = 3;

// End of Session Packet ('Z'). No payload; the session (not merely the
// connection) has ended, and the socket will close shortly after this
// arrives. 3 bytes on the wire.
inline constexpr size_t kEndOfSessionWireBytes = 3;

// ---- client-sent packets -------------------------------------------------------

// Login Request Packet ('L'). The client must send this immediately upon
// opening a new TCP connection. 49 bytes on the wire (47 declared + 2).
namespace login_request {
inline const uint8_t* username(const uint8_t* p) { return p + 3; }    //  3..8,  right-padded
inline const uint8_t* password(const uint8_t* p) { return p + 9; }    //  9..18, right-padded
// Blank (space-filled) to log into the currently active session.
inline const uint8_t* requested_session(const uint8_t* p) { return p + 19; }  // 19..28
// ASCII numeric, or "0" to start receiving the most recently generated
// message rather than resuming from a specific point. The document states
// the padding direction for Login ACCEPTED's Session/Sequence Number
// fields explicitly ("left padded with spaces"); it does not restate a
// padding direction for these two client-side fields, so the same
// left-padding convention is applied here by analogy to the numeric field
// it mirrors, not by an explicit sentence — recorded as such rather than
// presented as equally certain.
inline const uint8_t* requested_sequence_number(const uint8_t* p) { return p + 29; }  // 29..48
}  // namespace login_request
inline constexpr size_t kLoginRequestWireBytes = 49;

// Unsequenced Data Packet ('U'). Client-to-server, not sequenced, may be
// lost across a socket failure — the higher-level protocol's problem, not
// SoupBinTCP's.
namespace unsequenced_data {
inline const uint8_t* message(const uint8_t* p) { return p + 3; }
inline size_t message_length(const uint8_t* p) {
    return static_cast<size_t>(declared_length(p)) - 1;
}
}  // namespace unsequenced_data

// Client Heartbeat Packet ('R'). No payload. 3 bytes on the wire.
inline constexpr size_t kClientHeartbeatWireBytes = 3;

// Logout Request Packet ('O'). No payload; the server terminates the
// connection immediately on receipt. 3 bytes on the wire. (The spec's own
// field table lists this one Packet Length's Value column as "Binary" where
// every other packet's identical field says "Integer" — section 1.5's Data
// Types statement, "Integer fields are binary big-endian values," makes the
// two words synonymous in this document, so this is read as the same
// wording drifting inside one table rather than a distinct field type.)
inline constexpr size_t kLogoutRequestWireBytes = 3;

}  // namespace itchbook::soupbin

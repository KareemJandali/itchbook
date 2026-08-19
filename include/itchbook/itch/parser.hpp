#pragma once
//
// parser.hpp — framing loop + dispatch.
//
// The parser knows nothing about order books. It reads framed messages from a
// Reader and dispatches each to the caller's handler. The handler is a template
// parameter (not std::function / not a virtual), so the whole dispatch inlines
// and there is no indirect call on the hot path.
//
// A handler is any type with an `on_message(char type, const uint8_t* payload,
// uint16_t len)` method. See tools/ for examples.
//
#include <cstdint>
#include <vector>

#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/reader.hpp"

namespace itchbook {

template <typename Handler>
uint64_t parse(Reader& reader, Handler& handler) {
    std::vector<uint8_t> buf;
    uint64_t count = 0;
    while (reader.next(buf)) {
        char type = static_cast<char>(buf[0]);
        auto len = static_cast<uint16_t>(buf.size());

        // If we know the spec length for this type, the prefix must agree.
        // Disagreement means we have desynced — fail loudly, now.
        int expected = itch::spec_length(type);
        if (expected > 0 && len != expected) {
            throw std::runtime_error(std::string("parser: length mismatch for type '") +
                                     type + "' (got " + std::to_string(len) + ", expected " +
                                     std::to_string(expected) + ")");
        }

        handler.on_message(type, buf.data(), len);
        ++count;
    }
    return count;
}

}  // namespace itchbook

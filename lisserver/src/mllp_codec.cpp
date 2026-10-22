// src/mllp_codec.cpp
#include <lis/mllp_codec.h>

namespace lis {

namespace { constexpr uint8_t kStartByte = 0x0B, kEnd1 = 0x1C, kEnd2 = 0x0D; }

std::optional<std::string> MllpCodec::feed(uint8_t byte) {
    switch (state_) {
    case State::kStart:
        if (byte != kStartByte) throw ProtocolError("MLLP violation: expected start byte 0x0B");
        buf_.clear(); state_ = State::kBody; return std::nullopt;
    case State::kBody:
        if (byte == kStartByte) throw ProtocolError("MLLP violation: unexpected start byte 0x0B in body");
        if (byte == kEnd1) { state_ = State::kExpectEnd; return std::nullopt; }
        buf_.push_back(static_cast<char>(byte)); return std::nullopt;
    case State::kExpectEnd:
        if (byte != kEnd2) throw ProtocolError("MLLP violation: expected end byte 0x0D");
        state_ = State::kEnd; [[fallthrough]];
    case State::kEnd:
        state_ = State::kStart;
        return std::optional<std::string>(std::move(buf_));
    }
    return std::nullopt;
}

std::string MllpCodec::frame(const std::string& payload) const {
    std::string out;
    out.reserve(payload.size() + 3);
    out.push_back(static_cast<char>(kStartByte));
    out += payload;
    out.push_back(static_cast<char>(kEnd1));
    out.push_back(static_cast<char>(kEnd2));
    return out;
}

}  // namespace lis

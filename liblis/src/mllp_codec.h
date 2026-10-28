// src/mllp_codec.h —— 库内实现头，不对外暴露
#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace lis {

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& m) : std::runtime_error(m) {}
};

// MLLP 帧状态机：等 0x0B -> 收正文 -> 等 0x1C -> 等 0x0D。
// 帧字节违规抛 ProtocolError（调用方负责关闭该连接）。
class MllpCodec {
public:
    std::optional<std::string> feed(uint8_t byte);
    std::string frame(const std::string& payload) const;

private:
    enum class State { kStart, kBody, kExpectEnd, kEnd } state_ = State::kStart;
    std::string buf_;
};

}  // namespace lis

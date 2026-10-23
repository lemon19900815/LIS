// include/lis/transport.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace lis {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool connect(const std::string& host, uint16_t port, int timeout_ms) = 0;
    virtual int  read(uint8_t* buf, size_t n, int timeout_ms) = 0;  // >0 字节; 0 超时; <0 关闭/错误
    virtual bool write(const uint8_t* buf, size_t n) = 0;           // 整帧单次写出
    virtual void close() = 0;
};

}  // namespace lis

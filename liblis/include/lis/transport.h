// include/lis/transport.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lis {

// 传输抽象。线程契约：read 与 write 可跨线程并发（write 内部串行化）；
// close 可从另一线程调用，须打断该 transport 上阻塞中的 read；析构前须保证无在途调用。
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool connect(const std::string& host, uint16_t port, int timeout_ms) = 0;
    virtual int  read(uint8_t* buf, size_t n, int timeout_ms) = 0;  // >0 字节; 0 超时; <0 关闭/错误
    virtual bool write(const uint8_t* buf, size_t n) = 0;           // 整帧单次写出
    virtual void close() = 0;
};

// 默认 TCP 实现：声明在抽象头，定义在库内（tcp_transport.cpp）
std::unique_ptr<ITransport> make_tcp_client();

}  // namespace lis

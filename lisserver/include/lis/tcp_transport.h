// include/lis/tcp_transport.h
#pragma once
#include <lis/transport.h>
#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace lis {

// standalone Asio 同步实现。
// io_ 仅驱动 connect/accept 的超时等待（async_* + run_for，单线程使用）。
// 被接受的连接借用 listener 的 io_context（Windows 下 socket 句柄不能跨 io_context
// 二次关联完成端口），其 read/write 走同步路径（SO_RCVTIMEO），从不 run io_。
// 线程契约：read 与 write 可并发（write 内部按 transport 串行化）；
// close 可从另一线程调用，会打断该 transport 上阻塞中的 read。
class TcpTransport : public ITransport {
public:
    TcpTransport();                                 // 客户端：自有 io_context
    explicit TcpTransport(asio::io_context& io);    // 服务端 accept 出的连接：借用 acceptor 的 io
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeout_ms) override;
    bool listen(uint16_t port);                          // 服务端建 acceptor（SO_REUSEADDR）
    std::unique_ptr<TcpTransport> accept(int timeout_ms);// 阻塞至超时(nullptr)或新连接
    int  read(uint8_t* buf, size_t n, int timeout_ms) override;
    bool write(const uint8_t* buf, size_t n) override;
    void close() override;

    uint16_t local_port() const;
    std::string remote_ip() const;
    uint16_t remote_port() const;

private:
    asio::io_context io_{};
    asio::ip::tcp::socket socket_;
    std::optional<asio::ip::tcp::acceptor> acceptor_;
    std::mutex write_mu_;
};

}  // namespace lis

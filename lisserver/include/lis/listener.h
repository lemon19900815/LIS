// include/lis/listener.h
#pragma once
#include <lis/transport.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lis {

struct Accepted {
    // 借用产出它的 IListener 的执行上下文：连接须先于该 IListener 销毁（先析构 transport，后析构 listener）
    std::unique_ptr<ITransport> transport;
    std::string remote_ip;
    uint16_t remote_port = 0;
};

// 服务端监听抽象：accept 阻塞至超时或出错（均返回 nullopt，不可区分）或新连接
class IListener {
public:
    virtual ~IListener() = default;
    virtual bool listen(uint16_t port) = 0;
    virtual std::optional<Accepted> accept(int timeout_ms) = 0;
    virtual uint16_t local_port() const = 0;
};

// 默认 TCP 实现：声明在抽象头，定义在库内（tcp_transport.cpp）
std::unique_ptr<IListener> make_tcp_listener();

}  // namespace lis

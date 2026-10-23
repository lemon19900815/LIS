// include/lis/lis_client.h
#pragma once
#include <lis/message.h>
#include <lis/tcp_transport.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace lis {

class LisTimeoutError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class LisIoError : public std::runtime_error { public: using std::runtime_error::runtime_error; };

// HL7 v2.x over MLLP 客户端：send_and_receive 同步阻塞。
// 语义（对齐 HAPI + 设计 §5.8）：先按 MSH-10 登记 pending -> 写帧 -> 按 MSA-2 归位；
// 超时/IO 失败 -> 丢弃连接重建 -> 原报文重试 N 次（沿用原 MSH-10，每次独立计时）。
// 多线程共享安全；对端 NAK 不抛异常，原样返回给调用方解析 MSA-1。
class LisClient {
public:
    struct Config { std::string host; uint16_t port; int timeout_ms = 5000; int retry = 3; int read_timeout_ms = 500; };
    using StatusCb = std::function<void(const std::string& msg_id, bool ok)>;

    explicit LisClient(Config cfg);
    ~LisClient();
    LisClient(const LisClient&) = delete;
    LisClient& operator=(const LisClient&) = delete;

    Message send_and_receive(const Message& out);
    bool connected() const { return connected_.load(); }
    void report_async(const Message& result, std::string msg_id, StatusCb cb);

private:
    struct Pending { std::promise<std::string> prom; };
    void ensure_connected();
    void disconnect();
    void read_loop();

    Config cfg_;
    TcpTransport transport_;
    std::thread reader_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::mutex connect_mu_;
    std::mutex pending_mu_;
    std::map<std::string, std::shared_ptr<Pending>> pending_;
    ThreadPool pool_{2, 64};
};

}  // namespace lis

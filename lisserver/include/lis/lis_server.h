// include/lis/lis_server.h
#pragma once
#include <lis/router.h>
#include <lis/listener.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lis {

// HL7 v2.x over MLLP 服务器：1 accept 线程 + 每连接 1 读线程 + 有界业务线程池。
// Router 全连接共享；帧违规/IO 错误只关闭当前连接。
// 生命周期不变量：connections_（借用 listener_ 持有的 io_context）必须声明在 listener_ 之后
// （先析构）；pool_ 停止必须先于成员析构（~LisServer 经 stop() 保证）。
class LisServer {
public:
    struct Config { uint16_t port; size_t workers = 4; int read_timeout_ms = 500; };
    using ConnListener = std::function<void(const std::string& ip, bool online)>;

    LisServer(Config cfg, Router router);
    ~LisServer();
    LisServer(const LisServer&) = delete;
    LisServer& operator=(const LisServer&) = delete;

    void start();
    void stop();
    uint16_t local_port() const;   // 须与 start()/stop() 同线程调用（本类不保证并发安全）
    // 连接回调：在 acceptor 与各 reader 线程并发调用，回调自身须线程安全；
    // 不得在回调内调用 start()/stop()（reader 线程内 stop 会自 join 死锁）；
    // 须在 start() 前设置（此后对 conn_cb_ 的读写无锁）。
    void set_conn_listener(ConnListener cb);

private:
    struct Connection {
        std::unique_ptr<ITransport> transport;
        std::thread reader;
        std::atomic<bool> open{false};
        std::string ip;          // accept 时快照（transport 关闭后仍可用）
        uint16_t port = 0;
    };
    void accept_loop();
    void read_loop(std::shared_ptr<Connection> conn);

    Config cfg_;
    Router router_;
    // 同一连接多帧并发处理，回复到达顺序不保证（客户端须按 MSA-2 配对）
    ThreadPool pool_;
    std::unique_ptr<IListener> listener_;   // start() 创建、stop() 末尾销毁（释放端口）
    std::thread acceptor_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::mutex conn_mu_;
    ConnListener conn_cb_;
    std::atomic<bool> stopping_{false};
};

}  // namespace lis

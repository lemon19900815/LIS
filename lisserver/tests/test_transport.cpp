// tests/test_transport.cpp
#include <gtest/gtest.h>
#include <lis/listener.h>
#include <lis/tcp_transport.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <chrono>
#include <thread>

TEST(Transport, LoopbackEcho) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));                      // 0 = 随机端口
    const auto port = listener->local_port();
    ASSERT_NE(port, 0u);

    std::thread server_thread([&] {
        auto peer = listener->accept(3000);
        ASSERT_TRUE(peer.has_value());
        uint8_t buf[64];
        int n = peer->transport->read(buf, sizeof buf, 3000);
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->transport->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");

    client.close();
    server_thread.join();
}

TEST(ThreadPool, CallerRunsWhenQueueFull) {
    lis::ThreadPool pool(1, 1);                            // 1 worker，队列上限 1
    std::atomic<int> done{0};
    pool.submit([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++done; });
    pool.submit([&] { ++done; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_EQ(done.load(), 2);
}

TEST(ThreadPool, CallerRunsDeterministic) {
    lis::ThreadPool pool(1, 0);                            // 队列上限 0：任何 submit 都 CallerRuns
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> on_caller{false};
    pool.submit([&] { on_caller = std::this_thread::get_id() == caller; });
    ASSERT_TRUE(on_caller.load());
}

TEST(Transport, ReadTimeoutThenStillUsable) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();

    std::thread server_thread([&] {
        auto peer = listener->accept(3000);
        ASSERT_TRUE(peer.has_value());
        uint8_t buf[64];
        ASSERT_EQ(peer->transport->read(buf, sizeof buf, 200), 0);    // 第一次超时
        ASSERT_EQ(peer->transport->read(buf, sizeof buf, 200), 0);    // 第二次超时（无悬垂 handler 干扰）
        int n = peer->transport->read(buf, sizeof buf, 3000);         // 超时后连接仍可正常收数据
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->transport->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));     // 让 server 走完两次超时
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("ok"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "ok");

    client.close();
    server_thread.join();
}

// 原 Transport.AcceptTimeoutRearms 已由紧随其后的 Listener.AcceptTimeoutRearms 取代（T5 迁移）

TEST(Listener, AcceptTimeoutRearms) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    ASSERT_FALSE(listener->accept(200).has_value());       // 第一次超时
    ASSERT_FALSE(listener->accept(200).has_value());       // 第二次超时（重挂无残留 handler）
    lis::TcpTransport client;                              // 超时后仍能接受真连接
    ASSERT_TRUE(client.connect("127.0.0.1", listener->local_port(), 2000));
    ASSERT_TRUE(listener->accept(2000).has_value());
}

TEST(Listener, AcceptedCarriesRemoteEndpoint) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    ASSERT_NE(port, 0u);

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000));
    auto accepted = listener->accept(2000);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted->remote_ip, "127.0.0.1");
    ASSERT_GT(accepted->remote_port, 0u);
    ASSERT_NE(accepted->remote_port, port);            // 必须是对端临时端口，而非监听端口
    ASSERT_NE(accepted->transport, nullptr);

    ASSERT_TRUE(accepted->transport->write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[8]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 2000), 2);      // Accepted.transport 可正常收发
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");
}

TEST(Transport, ConnectTimeoutThenReusable) {
    lis::TcpTransport t;
    ASSERT_FALSE(t.connect("192.0.2.1", 12345, 500));      // RFC5737 不可路由：超时或立即失败均可
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    ASSERT_TRUE(t.connect("127.0.0.1", listener->local_port(), 2000));  // 同一对象重连成功
    ASSERT_TRUE(listener->accept(2000).has_value());
}

TEST(Transport, CloseUnblocksBlockedRead) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000));
    auto peer = listener->accept(2000);
    ASSERT_TRUE(peer.has_value());

    std::atomic<int> read_result{-100};
    std::thread reader([&] {
        uint8_t buf[64];
        read_result = peer->transport->read(buf, sizeof buf, 10000);   // 阻塞最长 10s
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto start = std::chrono::steady_clock::now();
    peer->transport->close();                              // 另一线程 close 解锁阻塞读
    reader.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    ASSERT_LT(elapsed, 5000);                              // 远未到 10s 即返回
    ASSERT_EQ(read_result.load(), -1);
}

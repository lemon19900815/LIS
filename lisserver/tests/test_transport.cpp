// tests/test_transport.cpp
#include <gtest/gtest.h>
#include <lis/tcp_transport.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <chrono>
#include <thread>

TEST(Transport, LoopbackEcho) {
    lis::TcpTransport listener;
    ASSERT_TRUE(listener.listen(0));                       // 0 = 随机端口
    const auto port = listener.local_port();
    ASSERT_NE(port, 0u);

    std::thread server_thread([&] {
        auto peer = listener.accept(3000);
        ASSERT_NE(peer, nullptr);
        uint8_t buf[64];
        int n = peer->read(buf, sizeof buf, 3000);
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");

    client.close();
    server_thread.join();
    listener.close();
}

TEST(ThreadPool, CallerRunsWhenQueueFull) {
    lis::ThreadPool pool(1, 1);                            // 1 worker，队列上限 1
    std::atomic<int> done{0};
    pool.submit([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++done; });
    pool.submit([&] { ++done; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
    lis::TcpTransport listener;
    ASSERT_TRUE(listener.listen(0));
    const auto port = listener.local_port();

    std::thread server_thread([&] {
        auto peer = listener.accept(3000);
        uint8_t buf[64];
        ASSERT_EQ(peer->read(buf, sizeof buf, 200), 0);    // 第一次超时
        ASSERT_EQ(peer->read(buf, sizeof buf, 200), 0);    // 第二次超时（无悬垂 handler 干扰）
        int n = peer->read(buf, sizeof buf, 3000);         // 超时后连接仍可正常收数据
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));   // 让 server 走完两次超时
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("ok"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "ok");

    client.close();
    server_thread.join();
    listener.close();
}

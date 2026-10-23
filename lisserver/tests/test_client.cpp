// tests/test_client.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/tcp_transport.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static const char* kQry = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|K1|P|2.3.1\rQRD||||BAR9";
static const char* kQryA = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|CA|P|2.3.1\rQRD||||BAR9";
static const char* kQryB = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|CB|P|2.3.1\rQRD||||BAR9";
static const char* kQryDup = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|DUP|P|2.3.1\rQRD||||BAR9";
static const char* kQryNoId = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02||P|2.3.1\rQRD||||BAR9";

static std::unique_ptr<lis::LisServer> make_ack_server(uint16_t port, uint16_t* bound) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) { return lis::build_ack(in, lis::AckCode::AA); }));
    auto s = std::make_unique<lis::LisServer>(lis::LisServer::Config{port, 2, 500}, std::move(router));
    s->start();
    *bound = s->local_port();
    return s;
}

TEST(Client, QueryAckRoundTrip) {
    uint16_t port = 0;
    auto server = make_ack_server(0, &port);
    lis::LisClient client({"127.0.0.1", port, 2000, 1});
    lis::Er7Parser p;
    auto resp = client.send_and_receive(p.parse(kQry));
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-2"), "K1");
    server->stop();
}

TEST(Client, TimeoutThrowsAfterRetries) {
    lis::TcpTransport listener;
    ASSERT_TRUE(listener.listen(0));
    const auto port = listener.local_port();
    std::thread t([&] { auto peer = listener.accept(3000); if (peer) std::this_thread::sleep_for(std::chrono::seconds(2)); });
    lis::LisClient client({"127.0.0.1", port, 400, 1});     // 1 次重试
    lis::Er7Parser p;
    ASSERT_THROW(client.send_and_receive(p.parse(kQry)), lis::LisTimeoutError);
    t.join();
    listener.close();
}

TEST(Client, RetryAfterReconnect) {
    uint16_t port = 0;
    auto server = make_ack_server(0, &port);
    lis::LisClient client({"127.0.0.1", port, 1500, 2});
    lis::Er7Parser p;
    ASSERT_EQ(lis::Terser::get(client.send_and_receive(p.parse(kQry)), "/MSA-1"), "AA");
    server->stop();                                          // 断线
    uint16_t port2 = 0;
    auto server2 = make_ack_server(port, &port2);            // 同端口重启
    ASSERT_EQ(port2, port);
    auto resp = client.send_and_receive(p.parse(kQry));      // 内部重连+重试成功
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-1"), "AA");
    server2->stop();
}

// 多线程共享安全：并发在途的两路请求各自配对自己的应答（无交叉投递）
TEST(Client, ConcurrentRoundTrip) {
    uint16_t port = 0;
    auto server = make_ack_server(0, &port);
    lis::LisClient client({"127.0.0.1", port, 2000, 1});
    std::promise<std::string> ra, rb;                        // 线程内不 ASSERT：结果汇聚回主线程断言
    auto fa = ra.get_future();
    auto fb = rb.get_future();
    std::thread ta([&] {
        try {
            lis::Er7Parser p;
            const auto resp = client.send_and_receive(p.parse(kQryA));
            ra.set_value(lis::Terser::get(resp, "/MSA-1") + "|" + lis::Terser::get(resp, "/MSA-2"));
        } catch (...) { ra.set_exception(std::current_exception()); }
    });
    std::thread tb([&] {
        try {
            lis::Er7Parser p;
            const auto resp = client.send_and_receive(p.parse(kQryB));
            rb.set_value(lis::Terser::get(resp, "/MSA-1") + "|" + lis::Terser::get(resp, "/MSA-2"));
        } catch (...) { rb.set_exception(std::current_exception()); }
    });
    ta.join();
    tb.join();
    ASSERT_EQ(fa.get(), "AA|CA");
    ASSERT_EQ(fb.get(), "AA|CB");
    server->stop();
}

// I-1：并发在途时任一方超时断开，"连接丢失"不得直抛给另一方——须同样走重试直到超时
TEST(Client, ConcurrentTimeoutAllRetryToTimeout) {
    lis::TcpTransport listener;                              // 永不应答对端：接受连接但既不回应也不关闭
    ASSERT_TRUE(listener.listen(0));
    const auto port = listener.local_port();
    std::atomic<bool> running{true};
    std::vector<std::unique_ptr<lis::TcpTransport>> peers;
    std::thread acceptor([&] {
        while (running.load())
            if (auto peer = listener.accept(200)) peers.push_back(std::move(peer));
    });
    lis::LisClient client({"127.0.0.1", port, 400, 2});      // 2 次重试
    auto send = [](lis::LisClient& c, const char* qry, std::promise<std::string>& done) {
        try {
            lis::Er7Parser p;
            c.send_and_receive(p.parse(qry));
            done.set_value("ok");
        } catch (const lis::LisTimeoutError&) { done.set_value("timeout"); }
        catch (const lis::LisIoError&) { done.set_value("io"); }   // 修复前：他方断开直抛给本调用方
        catch (...) { done.set_value("other"); }
    };
    std::promise<std::string> ra, rb;
    auto fa = ra.get_future();
    auto fb = rb.get_future();
    std::thread ta([&] { send(client, kQryA, ra); });        // 独立控制 ID 并发在途（同 ID 复用会先触发 I-2 拒绝）
    std::thread tb([&] { send(client, kQryB, rb); });
    ta.join();
    tb.join();
    running = false;
    acceptor.join();
    for (auto& peer : peers) peer->close();
    listener.close();
    ASSERT_EQ(fa.get(), "timeout");
    ASSERT_EQ(fb.get(), "timeout");
}

// I-2：并发复用在途 MSH-10 须立即响亮拒绝（不发送、不烧满超时）
TEST(Client, DuplicateControlIdRejected) {
    lis::TcpTransport listener;
    ASSERT_TRUE(listener.listen(0));
    const auto port = listener.local_port();
    std::thread holder([&] {                                 // 单次 accept 持有即足：被拒方不建新连接
        if (auto peer = listener.accept(3000))
            std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    lis::LisClient client({"127.0.0.1", port, 2000, 0});     // 0 次重试
    std::thread bg([&] {                                     // 后台先登记在途（结局超时，忽略）
        lis::Er7Parser p;
        try { client.send_and_receive(p.parse(kQryDup)); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 确保对方已登记在途
    lis::Er7Parser p;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(client.send_and_receive(p.parse(kQryDup)), lis::LisIoError);   // EXPECT：失败也要走到下方 join 清理
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 1000);                              // 立即拒绝而非等待整个超时
    bg.join();
    holder.join();
    listener.close();
}

// report_async：成功回调 ok=true；连接不可达回调 ok=false（均等回调完成后再析构 client）
TEST(Client, ReportAsyncCallbacks) {
    {
        uint16_t port = 0;
        auto server = make_ack_server(0, &port);
        lis::LisClient client({"127.0.0.1", port, 2000, 1});
        lis::Er7Parser p;
        std::promise<std::pair<std::string, bool>> done;
        auto fut = done.get_future();
        client.report_async(p.parse(kQry), "RA1",
                            [&](const std::string& id, bool ok) { done.set_value({id, ok}); });
        const auto r = fut.get();
        ASSERT_EQ(r.first, "RA1");
        ASSERT_TRUE(r.second);
        server->stop();
    }
    {
        lis::TcpTransport t;                                 // 拿一个刚关闭的端口：连接必失败
        ASSERT_TRUE(t.listen(0));
        const auto dead_port = t.local_port();
        t.close();
        lis::LisClient client({"127.0.0.1", dead_port, 500, 1});
        lis::Er7Parser p;
        std::promise<std::pair<std::string, bool>> done;
        auto fut = done.get_future();
        client.report_async(p.parse(kQry), "RA2",
                            [&](const std::string& id, bool ok) { done.set_value({id, ok}); });
        const auto r = fut.get();
        ASSERT_EQ(r.first, "RA2");
        ASSERT_FALSE(r.second);
    }
}

// 对端 NAK：原样返回给调用方（不抛异常），由调用方解析 MSA-1
TEST(Client, NakReturnedToCaller) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) {
            return lis::build_ack(in, lis::AckCode::AE, "nope");
        }));
    lis::LisServer server(lis::LisServer::Config{0, 2, 500}, std::move(router));
    server.start();
    lis::LisClient client({"127.0.0.1", server.local_port(), 2000, 1});
    lis::Er7Parser p;
    const auto resp = client.send_and_receive(p.parse(kQry));
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-1"), "AE");
    server.stop();
}

// 入站 MSH-10 为空时客户端自动生成控制 ID（CLI 前缀），应答按生成值配对投递
TEST(Client, AutoMsgIdWhenMsh10Empty) {
    std::string seen;                                        // handler 在服务端业务池线程写入
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [&seen](const lis::Message& in, const lis::MessageMeta& meta) {
            seen = meta.control_id;
            return lis::build_ack(in, lis::AckCode::AA);
        }));
    lis::LisServer server(lis::LisServer::Config{0, 2, 500}, std::move(router));
    server.start();
    lis::LisClient client({"127.0.0.1", server.local_port(), 2000, 1});
    lis::Er7Parser p;
    const auto resp = client.send_and_receive(p.parse(kQryNoId));
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-1"), "AA");
    server.stop();                                           // join 业务池后再读 seen（天然线程安全）
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-2"), seen);
    ASSERT_EQ(seen.substr(0, 3), "CLI");
}

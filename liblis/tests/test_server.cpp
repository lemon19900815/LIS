// tests/test_server.cpp
#include <gtest/gtest.h>
#include <lis/lis_server.h>
#include "mllp_codec.h"
#include <lis/er7_parser.h>
#include <lis/ack_builder.h>
#include "tcp_transport.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <chrono>
#include <thread>
#include <vector>

static const char* kQry =
    "MSH|^~\\&|Instr|Fac|Lis|Fac|20261022120000||QRY^Q02|T1|P|2.3.1\rQRD|202610221159|R|I|BAR001";
static const char* kQry2 =
    "MSH|^~\\&|Instr|Fac|Lis|Fac|20261022120000||QRY^Q02|T2|P|2.3.1\rQRD|202610221159|R|I|BAR001";

// 客户端侧原始收发工具：写一帧，读一帧（带超时）
static std::string transact(uint16_t port, const std::string& payload) {
    lis::TcpTransport c;
    EXPECT_TRUE(c.connect("127.0.0.1", port, 2000));
    lis::MllpCodec codec;
    auto framed = codec.frame(payload);
    EXPECT_TRUE(c.write(reinterpret_cast<const uint8_t*>(framed.data()), framed.size()));
    uint8_t buf[8192];
    std::optional<std::string> msg;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!msg && std::chrono::steady_clock::now() < deadline) {
        int n = c.read(buf, sizeof buf, 500);
        if (n <= 0) continue;
        for (int i = 0; i < n && !msg; ++i) msg = codec.feed(buf[i]);
    }
    c.close();
    EXPECT_TRUE(msg.has_value());
    return msg.value_or("");
}

TEST(Server, QueryGetsAck) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) {
            return lis::build_ack(in, lis::AckCode::AA);
        }));
    lis::LisServer server({0, 2, 500}, std::move(router));   // port 0 = 随机端口
    server.start();
    const auto port = server.local_port();
    ASSERT_NE(port, 0u);

    auto reply = transact(port, kQry);
    lis::Er7Parser p;
    auto m = p.parse(reply);
    ASSERT_EQ(lis::Terser::get(m, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(m, "/MSA-2"), "T1");
    server.stop();
}

TEST(Server, ConnListenerReportsOnlineOffline) {
    lis::Router router;
    lis::LisServer server({0, 2, 500}, std::move(router));
    std::vector<std::pair<std::string, bool>> events;
    server.set_conn_listener([&](const std::string& ip, bool online) {
        events.emplace_back(ip, online);
    });
    server.start();
    const auto port = server.local_port();

    { lis::TcpTransport c; ASSERT_TRUE(c.connect("127.0.0.1", port, 2000)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 等读线程感知断开
    server.stop();

    ASSERT_EQ(events.size(), 2u);
    ASSERT_EQ(events[0].first, "127.0.0.1");
    ASSERT_EQ(events[0].second, true);
    ASSERT_EQ(events[1].second, false);
}

TEST(Server, StopReleasesPort) {
    lis::Router r1;
    lis::LisServer s1({0, 1, 500}, std::move(r1));
    s1.start();
    const auto port = s1.local_port();
    s1.stop();

    lis::Router r2;                                          // 同端口可立即重绑
    lis::LisServer s2({port, 1, 500}, std::move(r2));
    s2.start();
    ASSERT_EQ(s2.local_port(), port);
    s2.stop();
}

TEST(Server, PipelinedFramesBothGetAcks) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) {
            return lis::build_ack(in, lis::AckCode::AA);
        }));
    lis::LisServer server({0, 2, 500}, std::move(router));
    server.start();
    const auto port = server.local_port();

    lis::TcpTransport c;                                     // 单次 write 携带两帧
    ASSERT_TRUE(c.connect("127.0.0.1", port, 2000));
    lis::MllpCodec codec;
    std::string both = codec.frame(kQry) + codec.frame(kQry2);
    ASSERT_TRUE(c.write(reinterpret_cast<const uint8_t*>(both.data()), both.size()));

    std::vector<std::string> ids;                            // 收满两个 ACK
    lis::MllpCodec rx;
    uint8_t buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (ids.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        int n = c.read(buf, sizeof buf, 500);
        if (n <= 0) continue;
        for (int i = 0; i < n && ids.size() < 2; ++i) {
            auto msg = rx.feed(buf[i]);
            if (msg) {
                lis::Er7Parser p;
                ids.push_back(lis::Terser::get(p.parse(*msg), "/MSA-2"));
            }
        }
    }
    c.close();
    server.stop();

    ASSERT_EQ(ids.size(), 2u);                               // 两帧都被处理（不丢帧）
    ASSERT_NE(std::find(ids.begin(), ids.end(), "T1"), ids.end());
    ASSERT_NE(std::find(ids.begin(), ids.end(), "T2"), ids.end());
}

TEST(Server, FrameViolationIsolatesConnection) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) {
            return lis::build_ack(in, lis::AckCode::AA);
        }));
    lis::LisServer server({0, 2, 500}, std::move(router));
    server.start();
    const auto port = server.local_port();

    lis::TcpTransport bad;                                   // 坏帧客户端：裸 0x1C
    ASSERT_TRUE(bad.connect("127.0.0.1", port, 2000));
    uint8_t viol = 0x1C;
    ASSERT_TRUE(bad.write(&viol, 1));
    uint8_t buf[64];
    ASSERT_LT(bad.read(buf, sizeof buf, 1000), 0);           // 本连接被关

    auto reply = transact(port, kQry);                       // 其它连接不受影响
    lis::Er7Parser p;
    ASSERT_EQ(lis::Terser::get(p.parse(reply), "/MSA-1"), "AA");
    server.stop();
}

TEST(Server, StopWithLiveClientIsClean) {
    lis::Router router;
    lis::LisServer server({0, 2, 500}, std::move(router));
    server.start();
    const auto port = server.local_port();

    lis::TcpTransport c;
    ASSERT_TRUE(c.connect("127.0.0.1", port, 2000));
    server.stop();                                           // 带活跃连接停止：不挂死

    uint8_t buf[64];
    ASSERT_LT(c.read(buf, sizeof buf, 2000), 0);             // 客户端读被打断

    lis::Router r2;                                          // 端口可立即重绑
    lis::LisServer s2({port, 1, 500}, std::move(r2));
    s2.start();
    ASSERT_EQ(s2.local_port(), port);
    s2.stop();
}

TEST(Server, FireAndForgetKeepsMetaIp) {
    std::string seen_ip;
    std::atomic<int> handler_done{0};
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [&](const lis::Message&, const lis::MessageMeta& meta) {
            seen_ip = meta.remote_ip;
            ++handler_done;
            return lis::build_ack(lis::Er7Parser{}.parse(kQry), lis::AckCode::AA);
        }));
    lis::LisServer server({0, 2, 500}, std::move(router));
    server.start();
    const auto port = server.local_port();

    lis::TcpTransport c;                                     // 发一帧立即断开
    ASSERT_TRUE(c.connect("127.0.0.1", port, 2000));
    lis::MllpCodec codec;
    auto framed = codec.frame(kQry);
    ASSERT_TRUE(c.write(reinterpret_cast<const uint8_t*>(framed.data()), framed.size()));
    c.close();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (handler_done.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    server.stop();

    ASSERT_EQ(handler_done.load(), 1);
    ASSERT_EQ(seen_ip, "127.0.0.1");                          // transport 已关，meta 仍有 ip
}

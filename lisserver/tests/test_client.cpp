// tests/test_client.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/tcp_transport.h>
#include <lis/terser.h>
#include <chrono>
#include <thread>

static const char* kQry = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|K1|P|2.3.1\rQRD||||BAR9";

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

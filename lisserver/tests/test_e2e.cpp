// tests/test_e2e.cpp
#include <atomic>
#include <gtest/gtest.h>
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/terser.h>

TEST(E2e, QueryReturnsOrfAndOruGetsAck) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::QryHandler>(
        std::make_shared<lis::SimProvider>(std::vector<std::string>{"GLU", "ALT"})));
    std::atomic<bool> oru_seen{false};   // D1：OruHandler 回调在服务端业务池线程执行
    router.add("ORU", "R01", "*", std::make_shared<lis::OruHandler>(
        [&](const lis::Message&) { oru_seen = true; }));
    lis::LisServer server({0, 2, 500}, std::move(router));
    server.start();
    lis::LisClient client({"127.0.0.1", server.local_port(), 2000, 1});
    lis::Er7Parser p;

    auto orf = client.send_and_receive(p.parse(
        "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|E1|P|2.3.1\rQRD|||||BAR001"));
    ASSERT_EQ(lis::Terser::get(orf, "/MSH-9"), "ORF^Q02");
    ASSERT_EQ(lis::Terser::get(orf, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(orf, "/MSA-2"), "E1");
    ASSERT_EQ(lis::Terser::get(orf, "/PID-3"), "P0001");
    ASSERT_EQ(lis::Terser::get(orf, "/OBR-2"), "BAR001");
    int obx = 0;
    for (auto& s : orf.segments) if (s.name == "OBX") ++obx;
    ASSERT_EQ(obx, 2);

    auto ack = client.send_and_receive(p.parse(
        "MSH|^~\\&|I|F|L|F|20261022120100||ORU^R01|E2|P|2.3.1\rPID|1||P0001\rOBR|1||BAR001\rOBX|1|ST|GLU||5.5"));
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-1"), "AA");
    ASSERT_TRUE(oru_seen);
    server.stop();
}

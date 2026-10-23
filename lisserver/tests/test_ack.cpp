// tests/test_ack.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/terser.h>

static const char* kInbound =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1";

TEST(Ack, AaCopiesAndSwapsFields) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AA);
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-9"), "ACK^Q02");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-2"), "CTRL001");      // 带回入站消息控制 ID
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-3"), "RecvApp");      // 收发互换
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-4"), "RecvFac");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-5"), "SendApp");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-6"), "SendFac");
    ASSERT_NE(lis::Terser::get(ack, "/MSH-10"), "CTRL001");     // 新消息 ID
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-11"), "P");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-12"), "2.3.1");       // 版本复制
}

TEST(Ack, AeWithErrAddsErrSegment) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AE, "bad query");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-1"), "AE");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-3"), "bad query");
    ASSERT_EQ(ack.segments.size(), 3u);                          // MSH/MSA/ERR
    ASSERT_EQ(ack.segments[2].name, "ERR");
}

TEST(Ack, EncodeRoundTrips) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AR);
    auto m2 = p.parse(p.encode(ack));
    ASSERT_EQ(lis::Terser::get(m2, "/MSA-1"), "AR");
    ASSERT_EQ(lis::Terser::get(m2, "/MSA-2"), "CTRL001");
}

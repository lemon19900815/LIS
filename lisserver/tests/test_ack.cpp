// tests/test_ack.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/terser.h>

static const char* kInbound =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1";
static const char* kInboundNoTrigger =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY|CTRL001|P|2.3.1";
static const char* kInboundFiveCharMsh2 =
    "MSH|^~\\&#|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1";
static const char* kInboundCustom =
    "MSH|@#$%|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY@Q02|CTRL001|P|2.3.1";

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

TEST(Ack, MissingTriggerLeavesNoTrailingComponent) {
    lis::Er7Parser p;
    auto in = p.parse(kInboundNoTrigger);      // MSH-9 无触发事件
    auto ack = lis::build_ack(in, lis::AckCode::AA);
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-9"), "ACK");   // 不拖尾 ^
    auto m2 = p.parse(p.encode(ack));          // 编码后可完整 re-parse
    ASSERT_EQ(lis::Terser::get(m2, "/MSH-9"), "ACK");
}

TEST(Ack, FiveCharMsh2Verbatim) {
    lis::Er7Parser p;
    auto in = p.parse(kInboundFiveCharMsh2);   // v2.7 的 5 字符 MSH-2（含截断符 #）
    auto ack = lis::build_ack(in, lis::AckCode::AA);
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-2"), "^~\\&#");
    ASSERT_NE(p.encode(ack).find("MSH|^~\\&#|"), std::string::npos);
}

TEST(Ack, EscapesErrDelimiters) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AE, "bad | field ^ tail \r");
    auto m2 = p.parse(p.encode(ack));
    ASSERT_EQ(m2.segments.size(), 3u);         // \r 不再制造假段边界
    ASSERT_EQ(lis::Terser::get(m2, "/MSA-3"), "bad \\F\\ field \\S\\ tail \\X0d\\");
    ASSERT_EQ(lis::Terser::get(m2, "/ERR-1-4"), "bad \\F\\ field \\S\\ tail \\X0d\\");
}

TEST(Ack, CustomEncodingCharsPreservedAndEscaped) {
    lis::Er7Parser p;
    auto in = p.parse(kInboundCustom);         // 组分@ 重复# 转义$ 子组分%
    auto ack = lis::build_ack(in, lis::AckCode::AE, "a|b@c");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-2"), "@#$%");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-3"), "RecvApp");   // 收发互换
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-4"), "RecvFac");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-5"), "SendApp");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-6"), "SendFac");
    auto m2 = p.parse(p.encode(ack));
    ASSERT_EQ(lis::Terser::get(m2, "/MSA-3"), "a$F$b$S$c");  // 转义符随报文取 $，@ 同样双侧包夹
}

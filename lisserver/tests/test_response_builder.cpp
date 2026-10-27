// tests/test_response_builder.cpp
#include <gtest/gtest.h>
#include <lis/er7_parser.h>
#include <lis/response_builder.h>
#include <lis/terser.h>
#include <stdexcept>

static const char* kInbound =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1";
static const char* kInboundCustom =
    "MSH|@#$%|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY@Q02|CTRL001|P|2.3.1";
static const char* kInboundFiveCharMsh2 =
    "MSH|^~\\&#|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1";

namespace {

lis::Message wrap(const lis::Message& inbound, lis::Segment msh) {
    lis::Message out;
    out.ec = inbound.ec;
    out.segments.push_back(std::move(msh));
    return out;
}

}  // namespace

TEST(ResponseBuilder, SwapsCopiesAndStamps) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto msh = lis::build_response_msh(in, "ORF", "Q02");
    ASSERT_EQ(msh.name, "MSH");
    auto out = wrap(in, msh);
    ASSERT_EQ(lis::Terser::get(out, "/MSH-2"), "^~\\&");        // 原样复制
    ASSERT_EQ(lis::Terser::get(out, "/MSH-3"), "RecvApp");      // 收发互换
    ASSERT_EQ(lis::Terser::get(out, "/MSH-4"), "RecvFac");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-5"), "SendApp");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-6"), "SendFac");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-7").size(), 14u);     // yyyyMMddHHmmss
    ASSERT_EQ(lis::Terser::get(out, "/MSH-9"), "ORF^Q02");
    ASSERT_FALSE(lis::Terser::get(out, "/MSH-10").empty());     // 新控制 ID
    ASSERT_NE(lis::Terser::get(out, "/MSH-10"), "CTRL001");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-11"), "P");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-12"), "2.3.1");
}

TEST(ResponseBuilder, EmptyTriggerLeavesNoTrailingComponent) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto out = wrap(in, lis::build_response_msh(in, "ACK"));
    ASSERT_EQ(lis::Terser::get(out, "/MSH-9"), "ACK");
    ASSERT_NE(p.encode(out).find("|ACK|"), std::string::npos);
}

TEST(ResponseBuilder, EmptyMsgTypeThrows) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    ASSERT_THROW(lis::build_response_msh(in, ""), std::invalid_argument);
}

TEST(ResponseBuilder, FiveCharMsh2Verbatim) {
    lis::Er7Parser p;
    auto in = p.parse(kInboundFiveCharMsh2);
    auto out = wrap(in, lis::build_response_msh(in, "ORF", "Q02"));
    ASSERT_EQ(lis::Terser::get(out, "/MSH-2"), "^~\\&#");
    ASSERT_NE(p.encode(out).find("MSH|^~\\&#|"), std::string::npos);
}

TEST(ResponseBuilder, CustomEncodingChars) {
    lis::Er7Parser p;
    auto in = p.parse(kInboundCustom);                 // 组分@ 重复# 转义$ 子组分%
    auto out = wrap(in, lis::build_response_msh(in, "ORF", "Q02"));
    ASSERT_EQ(lis::Terser::get(out, "/MSH-2"), "@#$%");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-9"), "ORF@Q02");
    ASSERT_EQ(lis::Terser::get(out, "/MSH-3"), "RecvApp");
}

TEST(ResponseBuilder, EmptyMsh2RebuiltFromEncodingChars) {
    lis::Message in;                                  // 退化入站：只有 MSH-1 占位
    in.segments.push_back(lis::Segment{"MSH", {lis::Field{}}});
    auto out = wrap(in, lis::build_response_msh(in, "ACK"));
    ASSERT_EQ(lis::Terser::get(out, "/MSH-2"), "^~\\&");
}

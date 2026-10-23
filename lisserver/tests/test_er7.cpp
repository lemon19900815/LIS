// tests/test_er7.cpp
#include <gtest/gtest.h>
#include <lis/er7_parser.h>

using lis::Er7Parser;

static const char* kAdt =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1\r"
    "QRD|202610221159|R|I||BAR001|||||RES";

TEST(Er7, DelimitersComeFromMessageItself) {
    // 用非常规字段分隔符 # 与组分 ! 的报文
    Er7Parser p;
    auto m = p.parse("MSH#^~\\&#A#B#C#D#202601010000##QRY^Q02#ID1#P#2.3");
    ASSERT_EQ(m.ec.field, '#');
    ASSERT_EQ(lis::Terser::get(m, "/MSH-3"), "A");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-9-1"), "QRY");
}

TEST(Er7, MshSpecialFirstFields) {
    Er7Parser p;
    auto m = p.parse(kAdt);
    // MSH-1/MSH-2 是分隔符本身，不占普通字段
    ASSERT_EQ(lis::Terser::get(m, "/MSH-3"), "SendApp");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-9-1"), "QRY");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-9-2"), "Q02");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-10"), "CTRL001");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-12"), "2.3.1");
    ASSERT_EQ(m.version, "2.3.1");
}

TEST(Er7, NormalSegmentsAndComponents) {
    Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A\rPID|1||123^4^5||Doe^John");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3"), "123^4^5");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3-2"), "4");
    ASSERT_EQ(lis::Terser::get(m, "/PID-5-1"), "Doe");
}

TEST(Er7, EncodeRoundTrip) {
    Er7Parser p;
    auto m = p.parse(kAdt);
    auto raw = p.encode(m);
    auto m2 = p.parse(raw);
    ASSERT_EQ(lis::Terser::get(m2, "/MSH-10"), "CTRL001");
    ASSERT_EQ(lis::Terser::get(m2, "/QRD-5"), "BAR001");
}

TEST(Er7, ParseRejectsGarbage) {
    Er7Parser p;
    ASSERT_THROW(p.parse("NOT_A_HL7_MESSAGE"), lis::ParseError);
}

TEST(Er7, AcceptsTrailingCrLf) {
    Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A|B|C|D|||QRY^Q02|ID1|P|2.3\rPID|1||123\r\n");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3"), "123");
}

TEST(Er7, AcceptsCrLfBetweenSegments) {
    Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A|B|C|D|||QRY^Q02|ID1|P|2.3\r\nPID|1||123\r\nOBR|1");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3"), "123");
    ASSERT_EQ(lis::Terser::get(m, "/OBR-1"), "1");
}

TEST(Er7, EmptyFieldsKeepPosition) {
    Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A||C");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-3"), "A");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-4"), "");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-5"), "C");
    ASSERT_EQ(p.encode(m), "MSH|^~\\&|A||C");
}

TEST(Er7, Msh2RoundTrips) {
    Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A|B|C|D|||QRY^Q02|ID1|P|2.3");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-2"), "^~\\&");
    ASSERT_EQ(p.encode(p.parse(p.encode(m))), p.encode(m));
}

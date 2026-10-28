// tests/test_terser.cpp —— 路径护栏（病态路径必须抛 TerserPathError 而非触发补位爆炸）
#include <gtest/gtest.h>
#include <lis/er7_parser.h>
#include <lis/message.h>
#include <lis/terser.h>

using lis::Message;
using lis::Terser;
using lis::TerserPathError;

TEST(Terser, PathGuardRejectsHugeFieldNo) {
    // 曾在评审探针中触发 set 补位循环吃满内存（20G+），必须拒绝
    Message m;
    ASSERT_THROW(Terser::set(m, "/MSH-2000000000", "x"), TerserPathError);
    ASSERT_THROW(Terser::get(m, "/MSH-2000000000"), TerserPathError);
}

TEST(Terser, PathGuardRejectsMalformedTokens) {
    Message m;
    ASSERT_THROW(Terser::set(m, "/MSH-1000", "x"), TerserPathError);    // 超上限
    ASSERT_THROW(Terser::set(m, "/MSH-0", "x"), TerserPathError);       // 0 号字段
    ASSERT_THROW(Terser::set(m, "/MSH-3-abc", "x"), TerserPathError);   // 非数字组分
    ASSERT_THROW(Terser::set(m, "/MSH-9-2-9-1", "x"), TerserPathError); // 超过 4 级
    ASSERT_THROW(Terser::set(m, "/MSH-3(0)-1", "x"), TerserPathError);  // 0 号重复项
    ASSERT_THROW(Terser::set(m, "/MSH-3(1", "x"), TerserPathError);     // 未闭合的重复序号
    ASSERT_THROW(Terser::set(m, "/-3", "x"), TerserPathError);          // 空段名
    ASSERT_THROW(Terser::set(m, "MSH-3", "x"), TerserPathError);        // 缺 '/'
    ASSERT_THROW(Terser::set(m, "/MSH", "x"), TerserPathError);         // 缺字段号
}

TEST(Terser, PathGuardAcceptsUpperBound) {
    Message m;
    Terser::set(m, "/MSH-999", "x");                 // 999 在界内，补位量级极小
    ASSERT_EQ(Terser::get(m, "/MSH-999"), "x");
    ASSERT_EQ(Terser::get(m, "/MSH-998"), "");       // 补位出来的空字段
}

TEST(Terser, SetExistingAndNewField) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A|B|C|D|||QRY^Q02|ID1|P|2.3");
    lis::Terser::set(m, "/MSH-10", "NEWID");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-10"), "NEWID");
    lis::Terser::set(m, "/PID-5-1", "Zhang");
    lis::Terser::set(m, "/PID-5-2", "San");
    ASSERT_EQ(lis::Terser::get(m, "/PID-5"), "Zhang^San");
    // set 按 1 基字段号补位空字段（PID-1..4 为空），HL7 正确编码是 5 个竖线，非 "PID|Zhang^San"
    ASSERT_NE(p.encode(m).find("PID|||||Zhang^San"), std::string::npos);
}

TEST(Terser, GetMissingReturnsEmpty) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3"), "");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-99"), "");
}

TEST(Terser, BadPathThrows) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A");
    ASSERT_THROW(lis::Terser::get(m, "MSH-3"), lis::TerserPathError);
}

TEST(Terser, RepeatIndexPath) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A\rPID|1||A^B~C^D");
    ASSERT_EQ(Terser::get(m, "/PID-3"), "A^B~C^D");        // 整字段文本不变
    ASSERT_EQ(Terser::get(m, "/PID-3-1"), "A");            // 省略重复序号 = 第 1 项
    ASSERT_EQ(Terser::get(m, "/PID-3(1)-1"), "A");
    ASSERT_EQ(Terser::get(m, "/PID-3(2)-1"), "C");
    ASSERT_EQ(Terser::get(m, "/PID-3(2)"), "C^D");
    ASSERT_EQ(Terser::get(m, "/PID-3(3)-1"), "");          // 越界重复项

    Terser::set(m, "/PID-3(2)-1", "Y");
    ASSERT_EQ(Terser::get(m, "/PID-3"), "A^B~Y^D");        // 只改第 2 重复项首组分
    Terser::set(m, "/PID-3(1)", "Z");
    ASSERT_EQ(Terser::get(m, "/PID-3"), "Z~Y^D");
}

TEST(Terser, SubcomponentPath) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A\rPID|1||123||Zhang&San^Si");
    ASSERT_EQ(Terser::get(m, "/PID-5-1"), "Zhang&San");    // 不指定子组分：整组分原样
    ASSERT_EQ(Terser::get(m, "/PID-5-1-1"), "Zhang");
    ASSERT_EQ(Terser::get(m, "/PID-5-1-2"), "San");
    ASSERT_EQ(Terser::get(m, "/PID-5-1-3"), "");           // 越界子组分

    Terser::set(m, "/PID-5-1-2", "X");
    ASSERT_EQ(Terser::get(m, "/PID-5"), "Zhang&X^Si");
    Terser::set(m, "/PID-5-1-3", "T");                     // 子组分补位
    ASSERT_EQ(Terser::get(m, "/PID-5-1"), "Zhang&X&T");
}

TEST(Terser, RepeatIndexAfterSubcomponentAndViceVersa) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A\rPID|1||A&B~C&D");
    ASSERT_EQ(Terser::get(m, "/PID-3(2)-1-2"), "D");
    ASSERT_EQ(Terser::get(m, "/PID-3(1)-1-1"), "A");
}

TEST(Terser, Msh2ComponentAccessRejected) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A|B");
    ASSERT_THROW(Terser::get(m, "/MSH-2-1"), TerserPathError);
    ASSERT_THROW(Terser::set(m, "/MSH-2-1", "x"), TerserPathError);
    ASSERT_EQ(Terser::get(m, "/MSH-2"), "^~\\&");          // 整字段仍可读
}

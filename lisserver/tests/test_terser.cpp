// tests/test_terser.cpp —— 路径护栏（病态路径必须抛 TerserPathError 而非触发补位爆炸）
#include <gtest/gtest.h>
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
    ASSERT_THROW(Terser::set(m, "/MSH-9-2-9", "x"), TerserPathError);   // 多余后缀
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

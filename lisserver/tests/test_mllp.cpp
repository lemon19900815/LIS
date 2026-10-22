// tests/test_mllp.cpp
#include <gtest/gtest.h>
#include <lis/mllp_codec.h>

using lis::MllpCodec;

TEST(Mllp, FrameWrapsPayload) {
    MllpCodec c;
    auto framed = c.frame("MSH|^~\\&|A");
    ASSERT_EQ(framed, std::string("\x0BMSH|^~\\&|A\x1C\x0D", 13));
}

TEST(Mllp, FeedAssemblesMessageAcrossChunks) {
    MllpCodec c;
    std::optional<std::string> msg;
    for (char ch : std::string("\x0B""MSH|ABC\x1C")) msg = c.feed(static_cast<uint8_t>(ch));
    ASSERT_FALSE(msg.has_value());
    msg = c.feed(0x0D);
    ASSERT_TRUE(msg.has_value());
    ASSERT_EQ(*msg, "MSH|ABC");
}

TEST(Mllp, FeedByteViolationThrows) {
    MllpCodec c;
    c.feed(0x0B);
    ASSERT_THROW(c.feed(0x0B), lis::ProtocolError);   // START 状态收到第二个 0x0B
}

TEST(Mllp, FeedExpectEndViolationThrows) {
    MllpCodec c;
    for (char ch : std::string("\x0B""AB\x1C")) c.feed(static_cast<uint8_t>(ch));
    ASSERT_THROW(c.feed('X'), lis::ProtocolError);    // 0x1C 后必须是 0x0D
}

TEST(Mllp, FrameRoundTripWithEmbeddedCr) {
    MllpCodec c;
    std::string payload = "MSH|^~\\&|A\rPID|1||123";
    auto framed = c.frame(payload);
    std::optional<std::string> msg;
    for (size_t i = 0; i + 2 < framed.size(); ++i) msg = c.feed(static_cast<uint8_t>(framed[i]));
    ASSERT_FALSE(msg.has_value());
    msg = c.feed(static_cast<uint8_t>(framed[framed.size() - 2]));
    ASSERT_FALSE(msg.has_value());
    msg = c.feed(static_cast<uint8_t>(framed.back()));
    ASSERT_TRUE(msg.has_value());
    ASSERT_EQ(*msg, payload);
}

TEST(Mllp, TwoFramesBackToBack) {
    MllpCodec c;
    std::optional<std::string> msg;
    for (char ch : std::string("\x0B""ONE\x1C\x0D")) msg = c.feed(static_cast<uint8_t>(ch));
    ASSERT_TRUE(msg.has_value());
    ASSERT_EQ(*msg, "ONE");
    for (char ch : std::string("\x0B""TWO\x1C\x0D")) msg = c.feed(static_cast<uint8_t>(ch));
    ASSERT_TRUE(msg.has_value());
    ASSERT_EQ(*msg, "TWO");
}

TEST(Mllp, GarbageFirstByteThrows) {
    MllpCodec c;
    ASSERT_THROW(c.feed('X'), lis::ProtocolError);
}

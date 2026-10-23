#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/router.h>
#include <lis/terser.h>

static const char* kQry =
    "MSH|^~\\&|A|B|C|D|20261022120000||QRY^Q02|ID9|P|2.3.1";

static std::shared_ptr<lis::IHandler> fn_handler(
    std::function<lis::Message(const lis::Message&, const lis::MessageMeta&)> fn) {
    return std::make_shared<lis::FnHandler>(std::move(fn));
}

TEST(Router, ExactMatch) {
    lis::Router r;
    bool called = false;
    r.add("QRY", "Q02", "*", fn_handler([&](const lis::Message&, const lis::MessageMeta&) {
        called = true;
        return lis::Message{};
    }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_TRUE(called);
}

TEST(Router, FirstRegisteredWins) {
    lis::Router r;
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&) {
        lis::Message m; m.segments.push_back(lis::Segment{"AAA", {}}); return m;
    }));
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&) {
        lis::Message m; m.segments.push_back(lis::Segment{"BBB", {}}); return m;
    }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_EQ(out.segments[0].name, "AAA");
}

TEST(Router, NoMatchReturnsAr) {
    lis::Router r;
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_EQ(lis::Terser::get(out, "/MSA-1"), "AR");
    ASSERT_EQ(lis::Terser::get(out, "/MSA-2"), "ID9");
}

TEST(Router, HandlerExceptionReturnsAe) {
    lis::Router r;
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&)
        -> lis::Message { throw std::runtime_error("boom"); }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_EQ(lis::Terser::get(out, "/MSA-1"), "AE");
}

TEST(Router, EmptyResponseMeansNoReply) {
    lis::Router r;
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&) {
        return lis::Message{};
    }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_TRUE(out.segments.empty());
}

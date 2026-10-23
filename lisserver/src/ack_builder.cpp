// src/ack_builder.cpp
#include <lis/ack_builder.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>
#include <ctime>

namespace lis {
namespace {

std::string now_yyyymmddhhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
    return buf;
}

std::string new_message_id() {
    static std::atomic<unsigned long long> counter{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "ACK" + std::to_string(ms) + std::to_string(counter++);
}

Segment make_msh_like(const Message& inbound, const std::string& trigger) {
    Segment msh{"MSH", {}};
    msh.fields.push_back(Field{});                        // fields[0] 占位（MSH-1）
    msh.fields.push_back(Field{{std::string(1, inbound.ec.component) +
                                inbound.ec.repeat + std::string(1, inbound.ec.escape) +
                                std::string(1, inbound.ec.subcomponent)}});  // fields[1]=MSH-2
    auto put = [&](int field_no, std::initializer_list<std::string> comps) {
        while (msh.fields.size() < static_cast<size_t>(field_no)) msh.fields.push_back(Field{});
        msh.fields[field_no - 1].components.assign(comps.begin(), comps.end());
    };
    put(3, {Terser::get(inbound, "/MSH-5")});   // 我们的发送方 = 对方的接收方
    put(4, {Terser::get(inbound, "/MSH-6")});
    put(5, {Terser::get(inbound, "/MSH-3")});   // 我们的接收方 = 对方的发送方
    put(6, {Terser::get(inbound, "/MSH-4")});
    put(7, {now_yyyymmddhhmmss()});
    put(9, {"ACK", trigger});                   // MSH-9
    put(10, {new_message_id()});
    put(11, {Terser::get(inbound, "/MSH-11")});
    std::string ver = Terser::get(inbound, "/MSH-12");
    if (!ver.empty()) put(12, {ver});
    return msh;
}

}  // namespace

Message build_ack(const Message& inbound, AckCode code, const std::string& err) {
    Message ack;
    ack.ec = inbound.ec;
    ack.version = inbound.version;
    const char* code_str = code == AckCode::AA ? "AA" : code == AckCode::AE ? "AE" : "AR";
    const std::string trigger = Terser::get(inbound, "/MSH-9-2");

    ack.segments.push_back(make_msh_like(inbound, trigger));

    Segment msa{"MSA", {}};
    msa.fields.push_back(Field{{code_str}});                          // MSA-1
    msa.fields.push_back(Field{{Terser::get(inbound, "/MSH-10")}});   // MSA-2
    if (!err.empty()) msa.fields.push_back(Field{{err}});             // MSA-3
    ack.segments.push_back(std::move(msa));

    if (!err.empty()) {
        Segment seg_err{"ERR", {}};
        seg_err.fields.push_back(Field{{"", "", "", err}});            // ERR-1 组分最小形态
        ack.segments.push_back(std::move(seg_err));
    }
    return ack;
}

}  // namespace lis

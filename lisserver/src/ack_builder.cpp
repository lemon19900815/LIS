// src/ack_builder.cpp
#include <lis/ack_builder.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <initializer_list>

namespace lis {
namespace {

std::string now_yyyymmddhhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) return "";
    char buf[16];
    if (std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm) == 0) return "";
    return buf;
}

// HL7 转义：err 文本里的分隔符/转义符映射为 <escape>X<escape> 序列（\r 用十六进制 \X0d\）。
// 单趟逐字符处理——连续 replace 会把上一轮产出的转义符再转义，故不可分趟。
std::string escape_err(const std::string& err, const EncodingChars& ec) {
    std::string out;
    out.reserve(err.size());
    for (char c : err) {
        const char* seq = nullptr;
        if (c == ec.escape) seq = "E";
        else if (c == ec.field) seq = "F";
        else if (c == ec.component) seq = "S";
        else if (c == ec.repeat) seq = "R";
        else if (c == ec.subcomponent) seq = "T";
        else if (c == '\r') seq = "X0d";
        else if (c == '\n') seq = "X0a";
        if (seq) {
            out.push_back(ec.escape);
            out.append(seq);
            out.push_back(ec.escape);
        } else {
            out.push_back(c);
        }
    }
    return out;
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
    std::string msh2 = Terser::get(inbound, "/MSH-2");    // 原样复制，保住 5 字符 MSH-2 等变体
    if (msh2.empty())                                     // 退化入站：重建，MSH-2 不许为空
        msh2 = std::string(1, inbound.ec.component) + inbound.ec.repeat +
               std::string(1, inbound.ec.escape) + std::string(1, inbound.ec.subcomponent);
    msh.fields.push_back(Field{{msh2}});                  // fields[1]=MSH-2
    auto put = [&](int field_no, std::initializer_list<std::string> comps) {
        while (msh.fields.size() < static_cast<size_t>(field_no)) msh.fields.push_back(Field{});
        msh.fields[field_no - 1].reps.assign(1, Repetition{comps});
    };
    put(3, {Terser::get(inbound, "/MSH-5")});   // 我们的发送方 = 对方的接收方
    put(4, {Terser::get(inbound, "/MSH-6")});
    put(5, {Terser::get(inbound, "/MSH-3")});   // 我们的接收方 = 对方的发送方
    put(6, {Terser::get(inbound, "/MSH-4")});
    put(7, {now_yyyymmddhhmmss()});
    if (trigger.empty()) put(9, {"ACK"});       // 无触发事件：单组分，线上不拖尾 ^
    else put(9, {"ACK", trigger});              // MSH-9
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
    const std::string escaped = escape_err(err, inbound.ec);   // 按入站报文自身的分隔符转义

    ack.segments.push_back(make_msh_like(inbound, trigger));

    Segment msa{"MSA", {}};
    msa.fields.push_back(Field{{code_str}});                          // MSA-1
    msa.fields.push_back(Field{{Terser::get(inbound, "/MSH-10")}});   // MSA-2
    if (!err.empty()) msa.fields.push_back(Field{{escaped}});         // MSA-3
    ack.segments.push_back(std::move(msa));

    if (!err.empty()) {
        Segment seg_err{"ERR", {}};
        seg_err.fields.push_back(Field{{"", "", "", escaped}});        // ERR-1 组分最小形态
        ack.segments.push_back(std::move(seg_err));
    }
    return ack;
}

}  // namespace lis

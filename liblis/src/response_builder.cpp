// src/response_builder.cpp
#include <lis/response_builder.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <initializer_list>
#include <stdexcept>

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

std::string new_message_id() {
    static std::atomic<unsigned long long> counter{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    return "MSG" + std::to_string(ms) + std::to_string(counter++);
}

}  // namespace

Segment build_response_msh(const Message& inbound, const std::string& msg_type,
                           const std::string& trigger) {
    if (msg_type.empty())
        throw std::invalid_argument("build_response_msh: msg_type is empty");

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
    if (trigger.empty()) put(9, {msg_type});    // 无触发事件：单组分，线上不拖尾 ^
    else put(9, {msg_type, trigger});           // MSH-9
    put(10, {new_message_id()});
    put(11, {Terser::get(inbound, "/MSH-11")});
    std::string ver = Terser::get(inbound, "/MSH-12");
    if (!ver.empty()) put(12, {ver});
    return msh;
}

}  // namespace lis

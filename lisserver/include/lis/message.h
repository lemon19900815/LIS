// include/lis/message.h
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace lis {

struct EncodingChars {   // 取自报文本身（MSH-1/MSH-2），非硬编码
    char field = '|', component = '^', repeat = '~', escape = '\\', subcomponent = '&';
};

// 一个重复项：组分之间用 ^ 分隔；子组分 & 仍以字面量留在组分串内
struct Repetition {
    std::vector<std::string> components;
    Repetition() = default;
    Repetition(std::initializer_list<std::string> comps) : components(comps) {}
};

// 字段：重复项之间用 ~ 分隔（ER7 层级 字段 -> 重复项 -> 组分 -> 文本内的子组分）
struct Field {
    std::vector<Repetition> reps;
    Field() = default;
    Field(std::initializer_list<Repetition> rs) : reps(rs) {}
};

struct Segment {
    std::string name;              // "MSH"/"PID"/...
    std::vector<Field> fields;     // 统一规则：SEG-n -> fields[n-1]；MSH 的 fields[0] 为占位、fields[1] 为 MSH-2
};

struct Message {
    EncodingChars ec;
    std::string version;           // MSH-12
    std::vector<Segment> segments;
    std::string raw;               // parse 时的原文快照，set 后不更新；序列化一律走 Er7Parser::encode
};

struct MessageMeta {
    std::string remote_ip;
    uint16_t remote_port = 0;
    std::string control_id;        // 入站 MSH-10
};

}  // namespace lis

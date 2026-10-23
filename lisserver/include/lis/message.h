// include/lis/message.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lis {

struct EncodingChars {   // 取自报文本身（MSH-1/MSH-2），非硬编码
    char field = '|', component = '^', repeat = '~', escape = '\\', subcomponent = '&';
};

struct Field { std::vector<std::string> components; };  // 子组分 & 先以字面量存于 component 串内

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

// include/lis/terser.h
#pragma once
#include <lis/message.h>
#include <stdexcept>
#include <string>

namespace lis {

class TerserPathError : public std::runtime_error {
public:
    explicit TerserPathError(const std::string& m) : std::runtime_error(m) {}
};

// 路径形如 "/SEG-n"、"/SEG-n(rep)"、"/SEG-n(rep)-m" 或 "/SEG-n(rep)-m-k"
// （n=字段号, rep=重复序号（HAPI 风格，省略=1）, m=1 基组分号, k=1 基子组分号；
// 各级编号须为 1..999 的纯数字，越界或畸形 token 抛 TerserPathError——上限防 set 补位内存爆炸）。
// 统一规则：SEG-n -> fields[n-1]（MSH 因 fields[0] 占位天然满足 MSH-3->fields[2]）。
// 未指定重复序号时取第 1 个重复项；未指定子组分号时返回整组分串（子组分以 & 字面量保留）。
// 从零构造消息须先 set("/MSH-2", 分隔符串)，否则 encode 后无法 re-parse；MSH-2 不支持组分访问。
class Terser {
public:
    static std::string get(const Message& m, const char* path);
    static void set(Message& m, const char* path, const std::string& v);
};

}  // namespace lis

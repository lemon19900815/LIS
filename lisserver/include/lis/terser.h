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

// 路径形如 "/SEG-n" 或 "/SEG-n-m"（n=字段号, m=1 基组分号）。
// 统一规则：SEG-n -> fields[n-1]（MSH 因 fields[0] 占位天然满足 MSH-3->fields[2]）。
class Terser {
public:
    static std::string get(const Message& m, const char* path);
    static void set(Message& m, const char* path, const std::string& v);
};

}  // namespace lis

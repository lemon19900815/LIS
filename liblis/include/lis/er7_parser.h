// include/lis/er7_parser.h
#pragma once
#include <lis/message.h>
#include <lis/terser.h>
#include <stdexcept>
#include <string>

namespace lis {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};

class Er7Parser {
public:
    Message parse(const std::string& raw);
    std::string encode(const Message& m) const;
};

}  // namespace lis

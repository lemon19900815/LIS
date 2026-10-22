// src/er7_parser.cpp
#include <lis/er7_parser.h>
#include <lis/terser.h>
#include <sstream>

namespace lis {
namespace {

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { out.push_back(cur); cur.clear(); } else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

Field make_field(const std::string& text, char comp) {
    Field f;
    for (auto& c : split(text, comp)) f.components.push_back(c);
    return f;
}

Segment parse_segment(const std::string& line, const EncodingChars& ec, bool is_msh) {
    Segment seg;
    size_t pos = line.find(ec.field);
    if (pos == std::string::npos || pos < 3) throw ParseError("segment too short: " + line);
    seg.name = line.substr(0, 3);
    auto parts = split(line.substr(pos + 1), ec.field);
    if (is_msh) {
        seg.fields.push_back(Field{});                       // fields[0]：MSH-1 占位
        seg.fields.push_back(make_field(parts.front(), ec.component));  // fields[1]：MSH-2
        parts.erase(parts.begin());
    }
    for (auto& p : parts) seg.fields.push_back(make_field(p, ec.component));
    return seg;
}

}  // namespace

Message Er7Parser::parse(const std::string& raw) {
    if (raw.size() < 8 || raw.substr(0, 3) != "MSH") throw ParseError("message must start with MSH");
    Message m;
    m.ec.field = raw[3];
    m.ec.component = raw[4]; m.ec.repeat = raw[5]; m.ec.escape = raw[6]; m.ec.subcomponent = raw[7];
    m.raw = raw;
    std::istringstream iss(raw);
    std::string line;
    bool first = true;
    while (std::getline(iss, line, '\r')) {
        if (line.empty()) continue;
        m.segments.push_back(parse_segment(line, m.ec, first));
        first = false;
    }
    if (m.segments.empty()) throw ParseError("no segments");
    try { m.version = Terser::get(m, "/MSH-12"); } catch (...) { m.version.clear(); }
    return m;
}

std::string Er7Parser::encode(const Message& m) const {
    std::string out;
    for (size_t i = 0; i < m.segments.size(); ++i) {
        if (i) out.push_back('\r');
        const auto& s = m.segments[i];
        out += s.name;
        out.push_back(m.ec.field);
        size_t begin = (s.name == "MSH") ? 1 : 0;   // MSH 跳过 fields[0] 占位
        for (size_t j = begin; j < s.fields.size(); ++j) {
            if (j > begin) out.push_back(m.ec.field);
            const auto& comps = s.fields[j].components;
            for (size_t k = 0; k < comps.size(); ++k) {
                if (k) out.push_back(m.ec.component);
                out += comps[k];
            }
        }
    }
    return out;
}

}  // namespace lis

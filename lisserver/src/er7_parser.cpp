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

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

unsigned hex_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    return static_cast<unsigned>(c - 'A' + 10);
}

// HL7 v2 §2.7 转义序列：\F\ \S\ \R\ \T\ \E\ 与 \Xdd..\；转义符取本报文 MSH-2 第 4 字符。
// 解析时还原成字面量，encode 时再转义出去——内存模型统一存解码后文本。
std::string decode_escapes(const std::string& text, const EncodingChars& ec) {
    if (text.find(ec.escape) == std::string::npos) return text;
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != ec.escape) { out.push_back(text[i]); continue; }
        const size_t close = text.find(ec.escape, i + 1);
        if (close == std::string::npos) { out.push_back(text[i]); continue; }  // 孤立转义符按字面保留
        const std::string seq = text.substr(i + 1, close - i - 1);
        bool known = true;
        if (seq == "F") out.push_back(ec.field);
        else if (seq == "S") out.push_back(ec.component);
        else if (seq == "R") out.push_back(ec.repeat);
        else if (seq == "T") out.push_back(ec.subcomponent);
        else if (seq == "E") out.push_back(ec.escape);
        else if (!seq.empty() && (seq[0] == 'X' || seq[0] == 'x') && seq.size() >= 3
                 && (seq.size() - 1) % 2 == 0) {
            for (size_t k = 1; k < seq.size(); k += 2) {
                if (!is_hex_digit(seq[k]) || !is_hex_digit(seq[k + 1])) { known = false; break; }
            }
            if (known) {
                for (size_t k = 1; k < seq.size(); k += 2) {
                    out.push_back(static_cast<char>(hex_value(seq[k]) * 16 + hex_value(seq[k + 1])));
                }
            }
        } else {
            known = false;   // 未定义序列（含本地 \Z..\）原样保留
        }
        if (!known) out.append(text, i, close - i + 1);
        i = close;
    }
    return out;
}

std::string encode_escapes(const std::string& text, const EncodingChars& ec) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const char* seq = nullptr;
        if (c == ec.escape) seq = "E";
        else if (c == ec.field) seq = "F";
        else if (c == ec.component) seq = "S";
        else if (c == ec.repeat) seq = "R";
        else if (c == ec.subcomponent) seq = "T";
        else if (c == '\r') seq = "X0d";
        else if (c == '\n') seq = "X0a";
        if (seq != nullptr) {
            out.push_back(ec.escape);
            out.append(seq);
            out.push_back(ec.escape);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

Field make_field(const std::string& text, const EncodingChars& ec) {
    Field f;
    for (const auto& rep_text : split(text, ec.repeat)) {
        Repetition rep;
        for (auto& c : split(rep_text, ec.component)) rep.components.push_back(decode_escapes(c, ec));
        f.reps.push_back(std::move(rep));
    }
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
        // MSH-2 是分隔符定义本身，按字面量整串保存（不能按 ~/^ 切）
        seg.fields.push_back(Field{Repetition{parts.front()}});
        parts.erase(parts.begin());
    }
    for (auto& p : parts) seg.fields.push_back(make_field(p, ec));
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
        if (!line.empty() && line.back() == '\n') line.pop_back();   // 容忍 \r\n 行尾
        if (!line.empty() && line.front() == '\n') line.erase(0, 1); // 段间 \r\n 的 \n 落在下一段行首
        if (line.empty()) continue;
        m.segments.push_back(parse_segment(line, m.ec, first));
        first = false;
    }
    if (m.segments.empty()) throw ParseError("no segments");
    try { m.version = Terser::get(m, "/MSH-12"); } catch (const TerserPathError&) { m.version.clear(); }
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
            // MSH-2 是分隔符定义本身，按字面量输出；其余组分写入前按协议转义
            const bool literal = (s.name == "MSH" && j == 1);
            const auto& reps = s.fields[j].reps;
            for (size_t r = 0; r < reps.size(); ++r) {
                if (r) out.push_back(m.ec.repeat);
                const auto& comps = reps[r].components;
                for (size_t k = 0; k < comps.size(); ++k) {
                    if (k) out.push_back(m.ec.component);
                    if (literal) out += comps[k];
                    else out += encode_escapes(comps[k], m.ec);
                }
            }
        }
    }
    return out;
}

}  // namespace lis

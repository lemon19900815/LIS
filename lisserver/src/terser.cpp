// src/terser.cpp —— get 与 set 一并实现（Task 4 只补 set 测试）
#include <lis/terser.h>
#include <cstdlib>

namespace lis {
namespace {

Segment& find_seg(Message& m, const std::string& name) {
    for (auto& s : m.segments) if (s.name == name) return s;
    m.segments.push_back(Segment{name, {}});
    return m.segments.back();
}

const Segment* find_seg(const Message& m, const std::string& name) {
    for (auto& s : m.segments) if (s.name == name) return &s;
    return nullptr;
}

struct Path { std::string seg; int field; int comp; };  // comp=0 表示未指定

Path parse_path(const char* path) {
    // 形如 "/MSH-9-2"
    std::string p(path);
    if (p.empty() || p[0] != '/') throw TerserPathError("path must start with /");
    Path out; out.field = 0; out.comp = 0;
    auto t1 = p.find('-', 1);
    if (t1 == std::string::npos) throw TerserPathError("missing field: " + p);
    out.seg = p.substr(1, t1 - 1);
    auto t2 = p.find('-', t1 + 1);
    out.field = std::atoi(p.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1).c_str());
    if (out.field <= 0) throw TerserPathError("bad field no: " + p);
    if (t2 != std::string::npos) out.comp = std::atoi(p.substr(t2 + 1).c_str());
    return out;
}

}  // namespace

std::string Terser::get(const Message& m, const char* path) {
    auto pp = parse_path(path);
    const Segment* s = find_seg(m, pp.seg);
    if (!s || static_cast<size_t>(pp.field) > s->fields.size()) return "";
    const auto& f = s->fields[pp.field - 1];
    if (pp.comp > 0) return pp.comp <= static_cast<int>(f.components.size()) ? f.components[pp.comp - 1] : "";
    std::string out;
    for (size_t i = 0; i < f.components.size(); ++i) {
        if (i) out.push_back(m.ec.component);
        out += f.components[i];
    }
    return out;
}

void Terser::set(Message& m, const char* path, const std::string& v) {
    auto pp = parse_path(path);
    auto& s = find_seg(m, pp.seg);
    while (s.fields.size() < static_cast<size_t>(pp.field)) s.fields.push_back(Field{});
    auto& f = s.fields[pp.field - 1];
    if (pp.comp > 0) {
        while (f.components.size() < static_cast<size_t>(pp.comp)) f.components.push_back("");
        f.components[pp.comp - 1] = v;
    } else {
        f.components.assign(1, v);
    }
}

}  // namespace lis

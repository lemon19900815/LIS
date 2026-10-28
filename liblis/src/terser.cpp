// src/terser.cpp —— get 与 set 一并实现（Task 4 只补 set 测试）
#include <lis/terser.h>
#include <cstdlib>
#include <vector>

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

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { out.push_back(cur); cur.clear(); } else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

struct Path { std::string seg; int field; int rep; bool rep_set; int comp; int sub; };  // comp/sub=0 表示未指定

// 任何 HL7 v2 段的字段/重复项/组分/子组分号都远小于 999；上限同时挡住 atoi 溢出与 set 补位内存爆炸
constexpr int kMaxNo = 999;

bool is_digits(const std::string& s) {
    return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
}

int parse_no(const std::string& token, const char* what, const std::string& p) {
    if (!is_digits(token) || token.size() > 3) throw TerserPathError(std::string(what) + " must be 1-999: " + p);
    int n = std::atoi(token.c_str());
    if (n < 1 || n > kMaxNo) throw TerserPathError(std::string(what) + " must be 1-999: " + p);
    return n;
}

// 形如 "/PID-3(2)-1-1"：字段号、重复序号（HAPI 风格，省略=1）、组分号、子组分号
Path parse_path(const char* path) {
    std::string p(path);
    if (p.empty() || p[0] != '/') throw TerserPathError("path must start with /");
    auto t1 = p.find('-', 1);
    if (t1 == std::string::npos) throw TerserPathError("missing field: " + p);

    Path out;
    out.seg = p.substr(1, t1 - 1);
    out.rep = 1; out.rep_set = false; out.comp = 0; out.sub = 0;
    if (out.seg.empty()) throw TerserPathError("empty segment name: " + p);

    std::vector<std::string> tokens = split(p.substr(t1 + 1), '-');
    if (tokens.size() > 3) throw TerserPathError("too many levels (max field(rep)-component-subcomponent): " + p);

    std::string field_token = tokens[0];
    auto lp = field_token.find('(');
    if (lp != std::string::npos) {
        if (field_token.back() != ')') throw TerserPathError("unclosed repeat index: " + p);
        out.rep = parse_no(field_token.substr(lp + 1, field_token.size() - lp - 2), "repeat no", p);
        out.rep_set = true;
        field_token.erase(lp);
    }
    out.field = parse_no(field_token, "field no", p);
    if (tokens.size() > 1) out.comp = parse_no(tokens[1], "component no", p);
    if (tokens.size() > 2) out.sub = parse_no(tokens[2], "subcomponent no", p);
    return out;
}

// MSH-2 就是分隔符定义本身，按组分/子组分访问会自指
void reject_msh2_component_access(const Path& pp, const char* path) {
    if (pp.seg == "MSH" && pp.field == 2 && pp.comp > 0)
        throw TerserPathError(std::string("MSH-2 defines separators; component access not supported: ") + path);
}

std::string join_components(const Repetition& rep, const EncodingChars& ec) {
    std::string out;
    const auto& comps = rep.components;
    for (size_t c = 0; c < comps.size(); ++c) {
        if (c) out.push_back(ec.component);
        out += comps[c];
    }
    return out;
}

std::string join_reps(const Field& f, const EncodingChars& ec) {
    std::string out;
    for (size_t r = 0; r < f.reps.size(); ++r) {
        if (r) out.push_back(ec.repeat);
        out += join_components(f.reps[r], ec);
    }
    return out;
}

}  // namespace

std::string Terser::get(const Message& m, const char* path) {
    auto pp = parse_path(path);
    reject_msh2_component_access(pp, path);
    const Segment* s = find_seg(m, pp.seg);
    if (!s || static_cast<size_t>(pp.field) > s->fields.size()) return "";
    const Field& f = s->fields[pp.field - 1];
    if (pp.comp == 0) {
        if (!pp.rep_set) return join_reps(f, m.ec);       // 整字段：所有重复项
        if (static_cast<size_t>(pp.rep) > f.reps.size()) return "";
        return join_components(f.reps[pp.rep - 1], m.ec); // 显式重复序号：该项文本
    }
    if (static_cast<size_t>(pp.rep) > f.reps.size()) return "";
    const auto& comps = f.reps[pp.rep - 1].components;
    if (static_cast<size_t>(pp.comp) > comps.size()) return "";
    const std::string& comp = comps[pp.comp - 1];
    if (pp.sub == 0) return comp;
    auto subs = split(comp, m.ec.subcomponent);
    return static_cast<size_t>(pp.sub) <= subs.size() ? subs[pp.sub - 1] : "";
}

void Terser::set(Message& m, const char* path, const std::string& v) {
    auto pp = parse_path(path);
    reject_msh2_component_access(pp, path);
    auto& s = find_seg(m, pp.seg);
    while (s.fields.size() < static_cast<size_t>(pp.field)) s.fields.push_back(Field{});
    Field& f = s.fields[pp.field - 1];

    while (f.reps.size() < static_cast<size_t>(pp.rep)) f.reps.push_back(Repetition{});
    if (pp.comp == 0) {
        f.reps[pp.rep - 1].components.assign(1, v);
        return;
    }
    auto& comps = f.reps[pp.rep - 1].components;
    while (comps.size() < static_cast<size_t>(pp.comp)) comps.push_back("");
    std::string& comp = comps[pp.comp - 1];
    if (pp.sub == 0) {
        comp = v;
        return;
    }
    auto subs = split(comp, m.ec.subcomponent);
    while (subs.size() < static_cast<size_t>(pp.sub)) subs.push_back("");
    subs[pp.sub - 1] = v;
    comp.clear();
    for (size_t i = 0; i < subs.size(); ++i) {
        if (i) comp.push_back(m.ec.subcomponent);
        comp += subs[i];
    }
}

}  // namespace lis

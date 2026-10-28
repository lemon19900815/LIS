// src/ack_builder.cpp
#include <lis/ack_builder.h>
#include <lis/response_builder.h>
#include <lis/terser.h>

namespace lis {
namespace {

// HL7 转义：err 文本里的分隔符/转义符映射为 <escape>X<escape> 序列（\r 用十六进制 \X0d\）。
// 单趟逐字符处理——连续 replace 会把上一轮产出的转义符再转义，故不可分趟。
std::string escape_err(const std::string& err, const EncodingChars& ec) {
    std::string out;
    out.reserve(err.size());
    for (char c : err) {
        const char* seq = nullptr;
        if (c == ec.escape) seq = "E";
        else if (c == ec.field) seq = "F";
        else if (c == ec.component) seq = "S";
        else if (c == ec.repeat) seq = "R";
        else if (c == ec.subcomponent) seq = "T";
        else if (c == '\r') seq = "X0d";
        else if (c == '\n') seq = "X0a";
        if (seq) {
            out.push_back(ec.escape);
            out.append(seq);
            out.push_back(ec.escape);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

Message build_ack(const Message& inbound, AckCode code, const std::string& err) {
    Message ack;
    ack.ec = inbound.ec;
    ack.version = inbound.version;
    const char* code_str = code == AckCode::AA ? "AA" : code == AckCode::AE ? "AE" : "AR";
    const std::string trigger = Terser::get(inbound, "/MSH-9-2");
    const std::string escaped = escape_err(err, inbound.ec);   // 按入站报文自身的分隔符转义

    Segment msh = build_response_msh(inbound, "ACK", trigger);
    // ACK 的 MSH-10 回带请求的 control-id（仪器侧按 MSH-10 关联应答，与 MSA-2 同值）
    msh.fields[9].reps.assign(1, Repetition{{Terser::get(inbound, "/MSH-10")}});
    ack.segments.push_back(std::move(msh));

    Segment msa{"MSA", {}};
    msa.fields.push_back(Field{{code_str}});                          // MSA-1
    msa.fields.push_back(Field{{Terser::get(inbound, "/MSH-10")}});   // MSA-2
    if (!err.empty()) msa.fields.push_back(Field{{escaped}});         // MSA-3
    ack.segments.push_back(std::move(msa));

    if (!err.empty()) {
        Segment seg_err{"ERR", {}};
        seg_err.fields.push_back(Field{{"", "", "", escaped}});        // ERR-1 组分最小形态
        ack.segments.push_back(std::move(seg_err));
    }
    return ack;
}

}  // namespace lis

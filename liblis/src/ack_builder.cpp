// src/ack_builder.cpp
#include <lis/ack_builder.h>
#include <lis/response_builder.h>
#include <lis/terser.h>

namespace lis {

Message build_ack(const Message& inbound, AckCode code, const std::string& err) {
    Message ack;
    ack.ec = inbound.ec;
    ack.version = inbound.version;
    const char* code_str = code == AckCode::AA ? "AA" : code == AckCode::AE ? "AE" : "AR";
    const std::string trigger = Terser::get(inbound, "/MSH-9-2");

    Segment msh = build_response_msh(inbound, "ACK", trigger);
    // ACK 的 MSH-10 回带请求的 control-id（仪器侧按 MSH-10 关联应答，与 MSA-2 同值）
    msh.fields[9].reps.assign(1, Repetition{{Terser::get(inbound, "/MSH-10")}});
    ack.segments.push_back(std::move(msh));

    Segment msa{"MSA", {}};
    msa.fields.push_back(Field{{code_str}});                          // MSA-1
    msa.fields.push_back(Field{{Terser::get(inbound, "/MSH-10")}});   // MSA-2
    // err 存解码后原文，分隔符/换行的转义由 Er7Parser::encode 按报文编码字符统一处理
    if (!err.empty()) msa.fields.push_back(Field{{err}});             // MSA-3
    ack.segments.push_back(std::move(msa));

    if (!err.empty()) {
        Segment seg_err{"ERR", {}};
        seg_err.fields.push_back(Field{{"", "", "", err}});            // ERR-1 组分最小形态
        ack.segments.push_back(std::move(seg_err));
    }
    return ack;
}

}  // namespace lis

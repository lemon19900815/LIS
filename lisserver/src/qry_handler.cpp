// src/qry_handler.cpp
#include <lis/qry_handler.h>
#include <lis/ack_builder.h>
#include <lis/response_builder.h>
#include <lis/terser.h>

namespace lis {

Message QryHandler::handle(const Message& in, const MessageMeta&) {
    QueryConditions qc;
    qc.barcode = Terser::get(in, "/QRD-5");          // 条码位置按目标仪器协议联调时可调
    auto info = provider_->query(qc);
    if (!info) return build_ack(in, AckCode::AR, "sample not found");

    Message out;
    out.ec = in.ec;
    out.version = in.version;

    out.segments.push_back(build_response_msh(in, "ORF", Terser::get(in, "/MSH-9-2")));

    out.segments.push_back(Segment{"MSA", {Field{{"AA"}}, Field{{Terser::get(in, "/MSH-10")}}}});

    for (const auto& s : in.segments)                // 回带 QRD/QRF
        if (s.name == "QRD" || s.name == "QRF") out.segments.push_back(s);

    Segment pid{"PID", {}};
    pid.fields.push_back(Field{{"1"}});
    pid.fields.push_back(Field{});
    pid.fields.push_back(Field{{info->patient_id}});  // PID-3
    pid.fields.push_back(Field{});
    pid.fields.push_back(Field{{info->patient_name}});// PID-5
    out.segments.push_back(std::move(pid));

    Segment obr{"OBR", {}};
    obr.fields.push_back(Field{{"1"}});
    obr.fields.push_back(Field{{info->barcode}});     // OBR-2
    out.segments.push_back(std::move(obr));

    for (size_t i = 0; i < info->items.size(); ++i) {
        Segment obx{"OBX", {}};
        obx.fields.push_back(Field{{std::to_string(i + 1)}});
        obx.fields.push_back(Field{{"ST"}});
        obx.fields.push_back(Field{{info->items[i]}}); // OBX-3
        out.segments.push_back(std::move(obx));
    }
    return out;
}

}  // namespace lis

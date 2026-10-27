// include/lis/response_builder.h
#pragma once
#include <lis/message.h>
#include <string>

namespace lis {

// 构造响应报文的 MSH 段（HAPI AbstractMessage.fillResponseHeader 规则）：
//   MSH-1 占位；MSH-2 原样复制（为空时按入站 ec 重建）；
//   MSH-3/4 = 入站 MSH-5/6（收发互换），MSH-5/6 = 入站 MSH-3/4；
//   MSH-7 当前时间；MSH-9 = msg_type[^trigger]（trigger 为空则不拖尾组分）；
//   MSH-10 新控制 ID；MSH-11 复制入站；MSH-12 复制入站（非空时）。
// msg_type 为空抛 std::invalid_argument（调用方错误）。本函数只造 MSH，调用方自行组其余段。
Segment build_response_msh(const Message& inbound, const std::string& msg_type,
                           const std::string& trigger = {});

}  // namespace lis

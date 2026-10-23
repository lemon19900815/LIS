// include/lis/ack_builder.h
#pragma once
#include <lis/message.h>
#include <string>

namespace lis {

enum class AckCode { AA, AE, AR };   // 对齐 liscomm LISCOMM_ACK_CODE

// 照抄 HAPI AbstractMessage.fillResponseHeader 规则：
// MSH-1/2 原样；MSH-7 当前时间；MSH-9=ACK^原触发；MSH-10 新 ID；MSH-11/12 复制入站；
// MSH-3/4/5/6 收发互换；MSA-1=code；MSA-2=入站 MSH-10；err 非空补 ERR 段与 MSA-3。
Message build_ack(const Message& inbound, AckCode code, const std::string& err = {});

}  // namespace lis

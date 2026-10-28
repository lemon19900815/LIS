// include/lis/qry_handler.h
#pragma once
#include <lis/provider.h>
#include <lis/router.h>
#include <memory>

namespace lis {

// QRY^Q02 -> ORF^Q02（回显触发事件）：MSH 收发互换、MSA=AA，回带 QRD/QRF，补 PID/OBR/OBX。
// 查无样本回 AR（设计 §5.9）。
class QryHandler : public IHandler {
public:
    explicit QryHandler(std::shared_ptr<ISampleProvider> provider)
        : provider_(std::move(provider)) {}
    Message handle(const Message& in, const MessageMeta&) override;
private:
    std::shared_ptr<ISampleProvider> provider_;
};

}  // namespace lis

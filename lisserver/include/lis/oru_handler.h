// include/lis/oru_handler.h
#pragma once
#include <lis/ack_builder.h>
#include <lis/router.h>
#include <functional>

namespace lis {

// ORU^R01：把结果报文交给上层回调，回 AA。
class OruHandler : public IHandler {
public:
    using ResultCb = std::function<void(const Message& oru)>;
    explicit OruHandler(ResultCb cb) : cb_(std::move(cb)) {}
    Message handle(const Message& in, const MessageMeta&) override {
        if (cb_) cb_(in);
        return build_ack(in, AckCode::AA);
    }
private:
    ResultCb cb_;
};

}  // namespace lis

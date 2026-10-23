// src/router.cpp
#include <lis/router.h>
#include <lis/ack_builder.h>
#include <lis/terser.h>

namespace lis {
namespace {

bool match(const std::string& pattern, const std::string& value) {
    return pattern == "*" || pattern == value;
}

}  // namespace

void Router::add(std::string msg_type, std::string trigger, std::string version,
                 std::shared_ptr<IHandler> handler) {
    bindings_.push_back(Binding{std::move(msg_type), std::move(trigger),
                                std::move(version), std::move(handler)});
}

Message Router::dispatch(const Message& in, const MessageMeta& meta) const {
    const std::string type = Terser::get(in, "/MSH-9-1");
    const std::string trigger = Terser::get(in, "/MSH-9-2");
    const std::string version = Terser::get(in, "/MSH-12");
    for (const auto& b : bindings_) {
        if (match(b.type, type) && match(b.trigger, trigger) && match(b.version, version)) {
            try {
                return b.handler->handle(in, meta);
            } catch (const std::exception& e) {
                return build_ack(in, AckCode::AE, e.what());
            } catch (...) {
                return build_ack(in, AckCode::AE, "handler unknown error");
            }
        }
    }
    return build_ack(in, AckCode::AR, "No appropriate destination could be found");
}

}  // namespace lis

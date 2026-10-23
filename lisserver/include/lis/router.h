// include/lis/router.h
#pragma once
#include <lis/message.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lis {

class IHandler {
public:
    virtual ~IHandler() = default;
    virtual Message handle(const Message& in, const MessageMeta& meta) = 0;  // 空 Message=不回
};

// 函数适配器：lambda 直接注册
class FnHandler : public IHandler {
public:
    explicit FnHandler(std::function<Message(const Message&, const MessageMeta&)> fn)
        : fn_(std::move(fn)) {}
    Message handle(const Message& in, const MessageMeta& meta) override { return fn_(in, meta); }
private:
    std::function<Message(const Message&, const MessageMeta&)> fn_;
};

// (消息类型, 触发事件, 版本) -> Handler；任一项 "*" 通配；先注册先命中。
// 无命中回 AR；Handler 抛异常回 AE。
// add 仅在启动前调用；dispatch 只读、可并发。
// ("*","*","*") 也会收到 MSH-9 为空的退化报文（空值只匹配通配）。
class Router {
public:
    void add(std::string msg_type, std::string trigger, std::string version,
             std::shared_ptr<IHandler> handler);
    Message dispatch(const Message& in, const MessageMeta& meta) const;

private:
    struct Binding { std::string type, trigger, version; std::shared_ptr<IHandler> handler; };
    std::vector<Binding> bindings_;
};

}  // namespace lis

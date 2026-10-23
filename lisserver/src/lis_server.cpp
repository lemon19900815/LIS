// src/lis_server.cpp
#include <lis/lis_server.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/mllp_codec.h>
#include <lis/terser.h>
#include <algorithm>

namespace lis {

LisServer::LisServer(Config cfg, Router router)
    : cfg_(cfg), router_(std::move(router)), pool_(cfg.workers, 256) {}

LisServer::~LisServer() { stop(); }

void LisServer::start() {
    if (!listener_.listen(cfg_.port))
        throw std::runtime_error("listen failed on port " + std::to_string(cfg_.port));
    stopping_ = false;
    acceptor_ = std::thread([this] { accept_loop(); });
}

uint16_t LisServer::local_port() const { return listener_.local_port(); }

void LisServer::set_conn_listener(ConnListener cb) { conn_cb_ = std::move(cb); }

void LisServer::accept_loop() {
    while (!stopping_.load()) {
        auto t = listener_.accept(500);          // 500ms 轮询式 accept，给停止标志让路
        if (!t) continue;
        auto conn = std::make_shared<Connection>();
        conn->transport = std::move(t);
        conn->open = true;
        {
            std::lock_guard<std::mutex> lk(conn_mu_);
            connections_.erase(              // 顺带剔除死连接（对齐 HAPI ConnectionCleaner）
                std::remove_if(connections_.begin(), connections_.end(),
                    [](const std::shared_ptr<Connection>& c) {
                        if (!c->open.load()) {
                            if (c->reader.joinable()) c->reader.join();
                            return true;
                        }
                        return false;
                    }),
                connections_.end());
            connections_.push_back(conn);
        }
        const std::string ip = conn->transport->remote_ip();
        if (conn_cb_) conn_cb_(ip, true);
        conn->reader = std::thread([this, conn] { read_loop(conn); });
    }
}

void LisServer::read_loop(std::shared_ptr<Connection> conn) {
    MllpCodec codec;
    uint8_t buf[4096];
    while (conn->open.load() && !stopping_.load()) {
        int n = conn->transport->read(buf, sizeof buf, cfg_.read_timeout_ms);
        if (n == 0) continue;                    // 读超时：继续（可中断循环）
        if (n < 0) break;                        // 对端断开/IO 错误
        std::optional<std::string> msg;
        try {
            for (int i = 0; i < n && !msg; ++i) msg = codec.feed(buf[i]);
        } catch (const ProtocolError&) {
            break;                               // 帧违规：只关本连接
        }
        if (!msg) continue;
        pool_.submit([this, conn, raw = std::move(*msg)] {
            Er7Parser p;
            Message m;
            auto framed_reply = [&](const Message& out) {
                if (out.segments.empty()) return;
                auto raw_out = p.encode(out);
                auto framed = MllpCodec{}.frame(raw_out);
                conn->transport->write(reinterpret_cast<const uint8_t*>(framed.data()),
                                       framed.size());
            };
            try {
                m = p.parse(raw);
            } catch (const ParseError&) {
                // 解析失败：速扫第一行 MSH 凑包头回 NAK（对齐 HAPI getCriticalResponseData）
                auto cr = raw.find('\r');
                try {
                    auto msh_only = p.parse(raw.substr(0, cr));
                    framed_reply(build_ack(msh_only, AckCode::AE, "message parse error"));
                } catch (...) {}
                return;
            }
            MessageMeta meta{conn->transport->remote_ip(), conn->transport->remote_port(),
                             Terser::get(m, "/MSH-10")};
            framed_reply(router_.dispatch(m, meta));
        });
    }
    const std::string ip = conn->transport->remote_ip();   // 先取再关（关闭后取为空串）
    conn->open = false;
    conn->transport->close();
    if (conn_cb_) conn_cb_(ip, false);
}

void LisServer::stop() {
    if (stopping_.exchange(true)) return;
    listener_.close();                           // 让 accept 立即出错退出
    if (acceptor_.joinable()) acceptor_.join();
    {
        std::lock_guard<std::mutex> lk(conn_mu_);
        for (auto& c : connections_) {
            c->open = false;
            if (c->transport) c->transport->close();   // 强迫读线程退出
        }
    }
    {
        std::lock_guard<std::mutex> lk(conn_mu_);
        for (auto& c : connections_)
            if (c->reader.joinable()) c->reader.join();
        connections_.clear();
    }
    pool_.stop();
}

}  // namespace lis

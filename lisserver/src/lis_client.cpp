// src/lis_client.cpp
#include <lis/lis_client.h>
#include <lis/er7_parser.h>
#include <lis/mllp_codec.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>
#include <optional>

namespace lis {
namespace {

std::string new_client_msg_id() {
    static std::atomic<unsigned long long> counter{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "CLI" + std::to_string(ms) + "_" + std::to_string(counter++);
}

// 速扫 MSA-2（对齐 HAPI PipeParser.getAckID：找 "\rMSA|" 取第 2 字段，不整报文解析）
std::string scan_ack_id(const std::string& raw) {
    auto pos = raw.find("\rMSA|");
    if (pos == std::string::npos) return "";
    std::string rest = raw.substr(pos + 5);
    auto f1 = rest.find('|');
    if (f1 == std::string::npos) return "";
    auto end = rest.find_first_of("|\r", f1 + 1);
    return end == std::string::npos ? rest.substr(f1 + 1)
                                    : rest.substr(f1 + 1, end - f1 - 1);
}

}  // namespace

LisClient::LisClient(Config cfg) : cfg_(std::move(cfg)) {}
LisClient::~LisClient() { stopping_ = true; disconnect(); pool_.stop(); }

void LisClient::ensure_connected() {
    std::lock_guard<std::mutex> lk(connect_mu_);
    if (connected_.load() || stopping_.load()) return;
    transport_.close();
    if (!transport_.connect(cfg_.host, cfg_.port, cfg_.timeout_ms))
        throw LisIoError("connect failed: " + cfg_.host + ":" + std::to_string(cfg_.port));
    if (reader_.joinable()) reader_.join();          // 旧 reader 已退出（connected_==false 的前提），先收尸再替换
    connected_ = true;
    reader_ = std::thread([this] { read_loop(); });
}

void LisClient::disconnect() {
    std::lock_guard<std::mutex> lk(connect_mu_);
    connected_ = false;
    transport_.close();
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    std::lock_guard<std::mutex> pl(pending_mu_);
    for (auto& [k, p] : pending_)
        p->prom.set_exception(std::make_exception_ptr(LisIoError("connection lost")));
    pending_.clear();
}

Message LisClient::send_and_receive(const Message& out) {
    Er7Parser parser;
    Message msg = out;
    std::string control_id = Terser::get(msg, "/MSH-10");
    if (control_id.empty()) {                          // MSH-10 必填（对齐 HAPI）
        control_id = new_client_msg_id();
        Terser::set(msg, "/MSH-10", control_id);
    }
    const std::string raw = parser.encode(msg);
    const auto framed = MllpCodec{}.frame(raw);

    for (int attempt = 0; attempt <= cfg_.retry; ++attempt) {
        ensure_connected();
        auto p = std::make_shared<Pending>();
        std::future<std::string> fut = p->prom.get_future();
        {
            std::lock_guard<std::mutex> pl(pending_mu_);
            auto inserted = pending_.emplace(control_id, p).second;   // 先登记后发送
            if (!inserted)
                throw LisIoError("MSH-10 already in flight: " + control_id);
        }
        if (!transport_.write(reinterpret_cast<const uint8_t*>(framed.data()), framed.size())) {
            disconnect();                              // IO 失败：丢弃连接，进入重试
            continue;
        }
        if (fut.wait_for(std::chrono::milliseconds(cfg_.timeout_ms)) != std::future_status::ready) {
            disconnect();                              // 应答超时：丢弃连接，进入重试
            continue;
        }
        try {
            return parser.parse(fut.get());
        } catch (const LisIoError&) {
            // 他方断开/清理在途登记（连接丢失）：与自身超时/写失败同样按可重试处理
            disconnect();
            continue;
        } catch (const ParseError& e) {
            throw LisIoError(std::string("bad response: ") + e.what());
        }
    }
    throw LisTimeoutError("timeout waiting response to " + control_id + " after " +
                          std::to_string(cfg_.retry + 1) + " attempts");
}

void LisClient::report_async(const Message& result, std::string msg_id, StatusCb cb) {
    pool_.submit([this, result, msg_id = std::move(msg_id), cb = std::move(cb)] {
        bool ok = false;
        try { send_and_receive(result); ok = true; } catch (...) {}
        if (cb) cb(msg_id, ok);
    });
}

void LisClient::read_loop() {
    MllpCodec codec;
    uint8_t buf[4096];
    while (connected_.load() && !stopping_.load()) {
        int n = transport_.read(buf, sizeof buf, cfg_.read_timeout_ms);
        if (n == 0) continue;                          // socket 读超时：继续轮询
        if (n < 0) break;                              // 对端断开/IO 错误
        try {
            for (int i = 0; i < n; ++i) {              // 同一段读到的多帧全部归位（流水线兼容）
                auto msg = codec.feed(buf[i]);
                if (!msg) continue;
                const std::string ack_id = scan_ack_id(*msg);
                std::lock_guard<std::mutex> pl(pending_mu_);
                auto it = pending_.find(ack_id);
                if (it != pending_.end()) {
                    it->second->prom.set_value(*msg);  // 仅当有人在等才投递（对齐 HAPI give()）
                    pending_.erase(it);
                }
                // 无人等待的应答：忽略（对齐 HAPI "Unexpected Message ... ignored"）
            }
        } catch (const ProtocolError&) { break; }      // 帧违规：断开
    }
    {
        std::lock_guard<std::mutex> pl(pending_mu_);
        for (auto& [k, p] : pending_)
            p->prom.set_exception(std::make_exception_ptr(LisIoError("connection lost")));
        pending_.clear();
    }
    connected_ = false;                                // 对端已死：立即唤醒在途等待者走重试
}

}  // namespace lis

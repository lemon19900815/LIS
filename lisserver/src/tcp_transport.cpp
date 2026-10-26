// src/tcp_transport.cpp
#include <lis/tcp_transport.h>

#include <chrono>

namespace lis {

TcpTransport::TcpTransport() : socket_(io_) {}

TcpTransport::TcpTransport(asio::io_context& io) : socket_(io) {}

TcpTransport::~TcpTransport() { close(); }

bool TcpTransport::connect(const std::string& host, uint16_t port, int timeout_ms) {
    close();
    std::error_code ec;
    asio::ip::tcp::resolver resolver(io_);
    auto eps = resolver.resolve(host, std::to_string(port), ec);
    if (ec) return false;
    bool done = false;
    asio::async_connect(socket_, eps,
        [&](std::error_code e, const asio::ip::tcp::endpoint&) { ec = e; done = true; });
    io_.restart();
    io_.run_for(std::chrono::milliseconds(timeout_ms));
    if (!done) {
        socket_.cancel();
        io_.restart();
        io_.run();          // 在局部变量存活期间消费被取消的 handler，避免悬垂
        close();
        return false;
    }
    if (ec) { close(); return false; }
    std::error_code opt_ec;
    socket_.set_option(asio::ip::tcp::no_delay(true), opt_ec);
    return true;
}

int TcpTransport::read(uint8_t* buf, size_t n, int timeout_ms) {
    if (!socket_.is_open()) return -1;
    DWORD ms = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 0;
    if (::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&ms), sizeof ms) != 0)
        return -1;
    std::error_code ec;
    size_t bytes = socket_.read_some(asio::buffer(buf, n), ec);
    if (ec == asio::error::timed_out) return 0;    // WSAETIMEDOUT -> 0（超时可重试）
    if (ec) return -1;                             // 对端关闭/IO 错误
    return static_cast<int>(bytes);
}

bool TcpTransport::write(const uint8_t* buf, size_t n) {
    if (!socket_.is_open()) return false;
    std::lock_guard<std::mutex> lk(write_mu_);        // 并发写串行化（T8 池任务/T9 多线程调用）
    std::error_code ec;
    asio::write(socket_, asio::buffer(buf, n), ec);   // 整帧单次写出
    return !ec;
}

void TcpTransport::close() {
    std::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);   // 规范打断阻塞读
        socket_.close(ec);
    }
}

std::string TcpTransport::remote_ip() const {
    std::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (ec) return {};
    return ep.address().to_string();
}

uint16_t TcpTransport::remote_port() const {
    std::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    if (ec) return 0;
    return ep.port();
}

TcpListener::TcpListener() = default;

TcpListener::~TcpListener() {
    std::error_code ec;
    if (acceptor_ && acceptor_->is_open()) acceptor_->close(ec);
}

bool TcpListener::listen(uint16_t port) {
    acceptor_.reset();                              // 重复 listen 时先弃旧 acceptor
    std::error_code ec;
    asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
    acceptor_.emplace(io_);
    acceptor_->open(ep.protocol(), ec);
    if (!ec) acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor_->bind(ep, ec);
    if (!ec) acceptor_->listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        acceptor_.reset();
        return false;
    }
    return true;
}

std::optional<Accepted> TcpListener::accept(int timeout_ms) {
    if (!acceptor_) return std::nullopt;
    std::unique_ptr<TcpTransport> t(new TcpTransport(io_));
    bool done = false;
    std::error_code ec;
    acceptor_->async_accept(t->socket_, [&](std::error_code e) { ec = e; done = true; });
    io_.restart();
    io_.run_for(std::chrono::milliseconds(timeout_ms));
    if (!done) {
        // 超时：取消挂起的 accept 并消费其 handler。若连接恰在超时边界完成，
        // handler 会以成功状态执行，仍取用该连接，避免静默丢弃对端已建立的连接。
        std::error_code cancel_ec;
        acceptor_->cancel(cancel_ec);
        io_.restart();
        io_.run();
    }
    if (!done || ec) return std::nullopt;
    std::error_code opt_ec;
    t->socket_.set_option(asio::ip::tcp::no_delay(true), opt_ec);
    Accepted a;
    a.remote_ip = t->remote_ip();      // 先取端点快照，再移走 transport
    a.remote_port = t->remote_port();
    a.transport = std::move(t);
    return a;
}

uint16_t TcpListener::local_port() const {
    if (!acceptor_) return 0;
    std::error_code ec;
    return acceptor_->local_endpoint(ec).port();
}

std::unique_ptr<IListener> make_tcp_listener() {
    return std::unique_ptr<IListener>(new TcpListener());
}

std::unique_ptr<ITransport> make_tcp_client() {
    return std::unique_ptr<ITransport>(new TcpTransport());
}

}  // namespace lis

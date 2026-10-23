# lislib 传输层抽象收口设计

日期：2026-10-23
状态：已与用户确认（RisDebug"LIS服务器页面"接入讨论中立项，驱动来源见 RisDebug 仓库 `docs/superpowers/specs/2026-10-23-lis-server-page-design.md`）
前置：基于 main @ 940b667（10 任务全部完成、64 用例全绿）

## 1. 背景与问题

lislib 公共头 `lis_server.h`、`lis_client.h` 直接 `#include <lis/tcp_transport.h>`（连带 `<asio.hpp>`），且按值持有具体 `TcpTransport`（`lis_server.h` 的 `listener_` 与 `Connection::transport`、`lis_client.h` 的 `transport_`）。后果：任何接入方只要包含 lislib 公共头，就必须在自己的工程里配 asio 头路径——传输实现（asio/TcpTransport）泄漏进了公共接口。

`ITransport` 抽象（`transport.h`，纯标准库）已存在但未被 server/client 头用足：服务端还多用了三个 ITransport 没有的能力（`listen()`、`accept()`、`local_port()`），以及 Accepted 连接的 remote_ip/port 快照。

## 2. 目标与范围

**目标**：`LisServer`/`LisClient` 依赖抽象而非具体 `TcpTransport`；asio 与 TcpTransport 退到库内（.cpp 侧），公共 include 图不含 asio。**公共 API 签名不变**（demo、既有测试、接入方零改动感知）。

**范围内**：
- 新抽象头 `listener.h`（IListener/Accepted/工厂）
- `transport.h` 追加 `make_tcp_client()` 工厂
- TcpListener/TcpTransport 职责拆分（tcp_transport.h/.cpp，仅库内使用）
- `lis_server.h`/`lis_client.h`/对应 .cpp 改造
- 测试辅助类型切换 + 新增 IListener 用例 + header probe

**范围外**：
- 通信语义（MLLP 帧、重试、ACK 契约、Router、Handler、解析层）一律不动
- 终评遗留 issue（ORF 补 MSH-10/11/12、QryHandler 出站转义、ISampleProvider 线程安全、Release gtest profile、显式 _WIN32_WINNT、/W3）
- 传输实现替换（无第二实现需求，工厂注入接口 YAGNI——测试用真实 TCP 回环）

## 3. 设计

### 3.1 新抽象头 `include/lis/listener.h`（纯标准库）

```cpp
#pragma once
#include <lis/transport.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lis {

struct Accepted {
    std::unique_ptr<ITransport> transport;
    std::string remote_ip;
    uint16_t remote_port = 0;
};

// 服务端监听抽象：accept 阻塞至超时（返回 nullopt）或新连接
class IListener {
public:
    virtual ~IListener() = default;
    virtual bool listen(uint16_t port) = 0;
    virtual std::optional<Accepted> accept(int timeout_ms) = 0;
    virtual uint16_t local_port() const = 0;
};

// 默认 TCP 实现：声明在抽象头，定义在库内（tcp_transport.cpp）
std::unique_ptr<IListener> make_tcp_listener();

}  // namespace lis
```

`transport.h` 同步追加（需补 `#include <memory>`）：

```cpp
std::unique_ptr<ITransport> make_tcp_client();   // 定义在库内
```

### 3.2 具体实现拆分（`tcp_transport.h/.cpp`，仅库内使用）

- **TcpListener : IListener**（新）：持有 `io_` + `acceptor_`；`listen()`（SO_REUSEADDR）、`accept(timeout_ms)`（async_accept + run_for 超时等待）返回 `Accepted{TcpTransport(io), remote_ip, remote_port}`——被接受的连接继续借用 listener 的 io_context（Windows 句柄不能跨 io_context 二次关联，维持现状），read/write 走同步路径（SO_RCVTIMEO）
- **TcpTransport : ITransport**（收窄）：删除 `listen()/accept()/acceptor_`；保留双构造（默认=客户端自有 io；`io_context&`=被接受连接借用）、`connect/read/write/close`、`remote_ip()/remote_port()`
- 线程契约不变：read 与 write 可并发（write 内部按 transport 串行化）；close 可从另一线程调用并打断阻塞中的 read

### 3.3 server/client 头文件改造（API 签名不变）

- `lis_server.h`：去掉 `#include <lis/tcp_transport.h>`，改 include `listener.h`；`TcpTransport listener_` → `std::unique_ptr<IListener> listener_`（构造时 `make_tcp_listener()`）；`Connection::transport` → `std::unique_ptr<ITransport>`；`accept_loop` 从 `Accepted` 取 transport 与 ip/port 快照；`local_port()` 走 `listener_->local_port()`。生命周期不变量保持：connections_（借用 listener 持有的 io_context）先于 listener_ 析构
- `lis_client.h`：去掉 `#include <lis/tcp_transport.h>`，改 include `transport.h`；`TcpTransport transport_` → `std::unique_ptr<ITransport> transport_`（`ensure_connected` 里 `make_tcp_client()` 重建）
- `lis_server.cpp`/`lis_client.cpp` 内部 include `tcp_transport.h`——asio 从此不出现在公共 include 图

### 3.4 边界规则（写入设计文档 v0.3）

**lislib 公共头不得引入 asio 或任何传输实现头。**

### 3.5 验证

- **回归**：既有 64 用例全绿（测试侧辅助类型跟随切换：`TcpTransport listener` → `make_tcp_listener()`，peers 改持 `Accepted`/`unique_ptr<ITransport>`）
- **新增 IListener 用例**：listen/local_port、accept 超时返回 nullopt、Accepted 的 remote_ip/remote_port 字段
- **header probe**：新增 `tools/header_probe.cpp`（包含全部公共头），验证命令 `cl /nologo /std:c++17 /EHsc /I include /c tools\header_probe.cpp`（不带 asio 路径）编译通过即证明边界干净

## 4. 验收标准

1. 既有 64 用例全绿 + 新增 IListener 用例绿
2. header probe 编译通过（仅 `-I include`，无 asio 路径）
3. 设计文档 v0.3 记录边界规则"公共头不得引入 asio"
4. `lis_server.h`/`lis_client.h` 的公共 API 签名与 v0.2 相同（diff 验证）

## 5. 决策记录

| 决策点 | 选择 | 被否备选 |
|---|---|---|
| 监听能力抽象形状 | 拆 TcpListener（监听与连接分职） | TcpTransport 同时实现 ITransport+IListener（改动面小但接口胖、被接受连接对象暴露 listen/accept） |
| 传输实现注入接口 | 不做（工厂函数即可，测试用真实 TCP 回环） | Config 注入自定义 ITransport 工厂（YAGNI） |
| API 兼容 | 公共签名不变 | 顺势改 API（会破坏 demo/测试/接入方） |

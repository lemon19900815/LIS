# lislib 传输层抽象收口重构 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `LisServer`/`LisClient` 改依赖 `IListener`/`ITransport` 抽象，asio 退到库内；公共 API 签名不变，既有用例全绿 + 新增 IListener 用例 + header probe 边界守卫。

**Architecture:** 新增纯标准库抽象头 `listener.h`（`IListener`/`Accepted`/`make_tcp_listener`），`transport.h` 追加 `make_tcp_client` 工厂；`tcp_transport.h/.cpp` 拆出专职监听的 `TcpListener` 并把 `TcpTransport` 收窄为连接端（expand → migrate → contract 三段推进）。设计全文见 `docs/2026-10-23-transport-abstraction-design.md`（本计划遵循其 §3/§4）。

**Tech Stack:** C++17 / standalone Asio（仅库内）/ MSVC v145 / MSBuild / GoogleTest

---

## 前置事实（执行者必读，零上下文假设）

- **仓库**：`D:\workspace\components\lis-server`，分支 `main`，基线 commit `3b9f102`（设计文档提交）。执行前用 superpowers:using-git-worktrees 建隔离工作区（建议分支名 `feature/transport-abstraction`）。
- **命令全部在 Git Bash、仓库根目录执行**。msbuild 不在 PATH，用全路径变量：
  ```bash
  MSB="C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
  "$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64     # 全量构建 lislib+listest+lisdemo
  lisserver/listest/x64/Debug/listest.exe --gtest_color=no                  # 全量测试（当前 64 用例全绿）
  "$MSB" tools/header_probe.vcxproj -p:Configuration=Debug -p:Platform=x64  # 边界守卫（T1 新建）
  ```
  注意 Git Bash 下 MSBuild 参数必须写 `-p:`（`/p:` 会被当路径）。
- **Directory.Build.props 陷阱**：`lisserver\Directory.Build.props` 会给 `lisserver\` 子树**所有**工程自动注入 conan asio include 路径。所以 header probe 工程必须放**仓库根** `tools\`（作用域之外），它只显式带 `-I lisserver\include`——这样 probe 编不过才能真实反映"接入方没配 asio"。
- **现状源码地图**（改前先读一遍这些文件）：

  | 文件 | 现状 |
  |---|---|
  | `lisserver\include\lis\tcp_transport.h` | TcpTransport 同时承担监听+连接：`listen()/accept()/local_port()` + `acceptor_` 成员；含 `#include <asio.hpp>` |
  | `lisserver\include\lis\transport.h` | 纯 `ITransport` 接口（connect/read/write/close） |
  | `lisserver\include\lis\lis_server.h` | `#include <lis/tcp_transport.h>`；`TcpTransport listener_` 按值；`Connection::transport` 为 `unique_ptr<TcpTransport>` |
  | `lisserver\include\lis\lis_client.h` | `#include <lis/tcp_transport.h>`；`TcpTransport transport_` 按值 |
  | `lisserver\src\tcp_transport.cpp` | asio 实现：listen/accept(async_accept+run_for)/read(SO_RCVTIMEO)/write(串行化)/close |
  | `lisserver\src\lis_server.cpp` | accept_loop 轮询 `listener_.accept(500)`；stop() 先 `listener_.close()` 再 join |
  | `lisserver\src\lis_client.cpp` | ensure_connected 里 `transport_.close(); transport_.connect(...)` |
  | `lisserver\src\thread_pool.cpp` | `stop()` 会 join 全部 worker 并清空队列（任务全部落地后才返回）——T3 stop() 顺序依赖此语义 |

- **asio 泄漏面已核实**：全部公共头中只有 `tcp_transport.h` 含 asio；公共头里只有 `lis_server.h`、`lis_client.h` 引用 `tcp_transport.h`。demo（`lisserver\demo\lis_demo.cpp`）不直接使用 TcpTransport，无需改动。
- **测试基线**：64 用例全绿。T2 新增 2 例 → 66；T5 删除 1 例被取代的旧用例 → 最终 **65**。
- **目标形态**（5 任务增量推进，每任务结束时解决方案全绿）：
  - T1 抽象头声明 + header probe 守卫（probe 红，测试不受影响）
  - T2 TcpListener + 双工厂 + 2 个 IListener 新用例
  - T3 LisServer 切 `unique_ptr<IListener>`（probe 仍红——lis_client.h 未动）
  - T4 LisClient 切 `unique_ptr<ITransport>`（**probe 转绿**，验收 #2 提前达成）
  - T5 TcpTransport 收窄（删 listen/accept/local_port）+ 测试迁移 + 设计文档 v0.3

---

### Task 1: 抽象头声明 + header probe 守卫（红）

**Files:**
- Create: `lisserver\include\lis\listener.h`
- Modify: `lisserver\include\lis\transport.h`（追加 `make_tcp_client` 工厂声明 + `<memory>`）
- Create: `tools\header_probe.cpp`
- Create: `tools\header_probe.vcxproj`
- Create: `.gitignore`（仓库根，当前不存在；`lisserver\.gitignore` 只管子树）

- [ ] **Step 1: 写 `lisserver\include\lis\listener.h`**（新抽象头，纯标准库，内容来自设计 §3.1 原文）

```cpp
// include/lis/listener.h
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

- [ ] **Step 2: 改 `lisserver\include\lis\transport.h`**（加 `<memory>` include 与工厂声明，其余不动）

```cpp
// include/lis/transport.h
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lis {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool connect(const std::string& host, uint16_t port, int timeout_ms) = 0;
    virtual int  read(uint8_t* buf, size_t n, int timeout_ms) = 0;  // >0 字节; 0 超时; <0 关闭/错误
    virtual bool write(const uint8_t* buf, size_t n) = 0;           // 整帧单次写出
    virtual void close() = 0;
};

// 默认 TCP 实现：声明在抽象头，定义在库内（tcp_transport.cpp）
std::unique_ptr<ITransport> make_tcp_client();

}  // namespace lis
```

- [ ] **Step 3: 写 `tools\header_probe.cpp`**（包含全部公共头，**不含** tcp_transport.h——它是库内实现头）

```cpp
// tools/header_probe.cpp
// 公共头边界守卫：编译通过 = 接入方只含 lislib 公共头即可使用（无需 asio）。
// 注意：不含 <lis/tcp_transport.h>——它是库内实现头（含 asio），接入方经 ITransport/IListener 消费。
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/listener.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/message.h>
#include <lis/mllp_codec.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/router.h>
#include <lis/terser.h>
#include <lis/thread_pool.h>
#include <lis/transport.h>

int main() { return 0; }
```

- [ ] **Step 4: 写 `tools\header_probe.vcxproj` 与仓库根 `.gitignore`**

`tools\header_probe.vcxproj`（独立样板：v145 工具集、C++17、唯一 include 路径是 `..\lisserver\include`，**不导任何 conan props**；`/utf-8` 必需——公共头含 UTF-8 中文注释，无它则 GBK 代码页解码会打碎 token 产生伪语法错误，红/绿证据被噪声污染）：

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><ProjectGuid>{A1B2C3D4-0002-4000-8000-000000000002}</ProjectGuid></PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <PlatformToolset>v145</PlatformToolset>
    <CharacterSet>MultiByte</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <PropertyGroup>
    <OutDir>$(MSBuildProjectDirectory)\obj\$(Platform)\$(Configuration)\</OutDir>
    <IntDir>$(MSBuildProjectDirectory)\obj\$(Platform)\$(Configuration)\</IntDir>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp17</LanguageStandard>
      <AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>
      <AdditionalIncludeDirectories>$(MSBuildProjectDirectory)\..\lisserver\include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="header_probe.cpp" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

仓库根 `.gitignore`（新建；`lisserver\.gitignore` 已存在但只覆盖 `lisserver\` 子树）：

```
tools/obj/
```

- [ ] **Step 5: 构建 probe，验证"红"（此刻必须失败，证明守卫真实）**

```bash
MSB="C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
"$MSB" tools/header_probe.vcxproj -p:Configuration=Debug -p:Platform=x64
```

Expected: **失败**，形如 `include\lis\tcp_transport.h(4): fatal error C1083: 无法打开包括文件: "asio.hpp": No such file or directory`（经 `lis_client.h`/`lis_server.h` → `tcp_transport.h` 引入）。这一步红是任务目标：先用守卫固定"公共头泄漏 asio"的事实。

- [ ] **Step 6: 全量构建 + 回归测试（本任务未动实现，必须维持 64 全绿）**

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
lisserver/listest/x64/Debug/listest.exe --gtest_color=no
```

Expected: `已成功生成`（3 个工程、0 错误）；测试末尾 `[  PASSED  ] 64 tests.`。工厂只有声明没有定义，无任何调用方，链接不受影响。

- [ ] **Step 7: 提交**

```bash
git add lisserver/include/lis/listener.h lisserver/include/lis/transport.h tools/header_probe.cpp tools/header_probe.vcxproj .gitignore
git commit -m "lisserver: 传输抽象头声明（IListener/Accepted/make_tcp_client）+ header probe 边界守卫（红）"
```

---

### Task 2: TcpListener + 双工厂 + IListener 用例（TDD）

**Files:**
- Modify: `lisserver\tests\test_transport.cpp`（追加 2 个 Listener 用例）
- Modify: `lisserver\include\lis\tcp_transport.h`（新增 TcpListener 类 + friend 声明）
- Modify: `lisserver\src\tcp_transport.cpp`（TcpListener 实现 + 两个工厂定义）

- [ ] **Step 1: 写失败测试**——在 `tests\test_transport.cpp` 头部 include 区加 `<lis/listener.h>`：

```cpp
#include <gtest/gtest.h>
#include <lis/listener.h>
#include <lis/tcp_transport.h>
#include <lis/thread_pool.h>
```

文件末尾追加两个用例：

```cpp
// 超时后重挂无残留 handler，仍能接受真连接（对齐旧 Transport.AcceptTimeoutRearms 的守护点）
TEST(Listener, AcceptTimeoutRearms) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    ASSERT_FALSE(listener->accept(200).has_value());   // 第一次超时
    ASSERT_FALSE(listener->accept(200).has_value());   // 第二次超时（重挂无残留 handler）
    lis::TcpTransport client;                          // 超时后仍能接受真连接
    ASSERT_TRUE(client.connect("127.0.0.1", listener->local_port(), 2000));
    ASSERT_TRUE(listener->accept(2000).has_value());
}

TEST(Listener, AcceptedCarriesRemoteEndpoint) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    ASSERT_NE(port, 0u);

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000));
    auto accepted = listener->accept(2000);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted->remote_ip, "127.0.0.1");
    ASSERT_GT(accepted->remote_port, 0u);
    ASSERT_NE(accepted->transport, nullptr);

    ASSERT_TRUE(accepted->transport->write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[8]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 2000), 2);   // Accepted.transport 可正常收发
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");
}
```

- [ ] **Step 2: 构建，验证"红"（链接错误）**

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
```

Expected: lislib 编译通过；**listest 链接失败**：`error LNK2019: 无法解析的外部符号 ... make_tcp_listener ...`（声明已有、定义未写，正是 TDD 红点）。

- [ ] **Step 3: 改 `lisserver\include\lis\tcp_transport.h`**——include 区加 listener.h、TcpTransport 私有区加 friend、文件末尾（`TcpTransport` 类定义之后、namespace 收尾之前）新增 `TcpListener` 类。改后完整文件：

```cpp
// include/lis/tcp_transport.h
#pragma once
#include <lis/listener.h>
#include <lis/transport.h>
#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace lis {

// standalone Asio 同步实现。
// io_ 仅驱动 connect/accept 的超时等待（async_* + run_for，单线程使用）。
// 被接受的连接借用 listener 的 io_context（Windows 下 socket 句柄不能跨 io_context
// 二次关联完成端口），其 read/write 走同步路径（SO_RCVTIMEO），从不 run io_。
// 线程契约：read 与 write 可并发（write 内部按 transport 串行化）；
// close 可从另一线程调用，会打断该 transport 上阻塞中的 read。
class TcpTransport : public ITransport {
public:
    TcpTransport();                                 // 客户端：自有 io_context
    explicit TcpTransport(asio::io_context& io);    // 服务端 accept 出的连接：借用 listener 的 io
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeout_ms) override;
    bool listen(uint16_t port);                          // 服务端建 acceptor（SO_REUSEADDR）
    std::unique_ptr<TcpTransport> accept(int timeout_ms);// 阻塞至超时(nullptr)或新连接
    int  read(uint8_t* buf, size_t n, int timeout_ms) override;
    bool write(const uint8_t* buf, size_t n) override;
    void close() override;

    uint16_t local_port() const;
    std::string remote_ip() const;
    uint16_t remote_port() const;

private:
    friend class TcpListener;                      // accept 直插 socket_（借用同一 io_context）
    asio::io_context io_{};
    asio::ip::tcp::socket socket_;
    std::optional<asio::ip::tcp::acceptor> acceptor_;
    std::mutex write_mu_;
};

// 专职监听端：持有 io_context 与 acceptor；accept 出的 TcpTransport 借用本对象 io_。
// 线程契约：析构前须保证被接受的连接已全部销毁（LisServer 的成员顺序不变量保证）。
class TcpListener : public IListener {
public:
    TcpListener();
    ~TcpListener() override;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool listen(uint16_t port) override;
    std::optional<Accepted> accept(int timeout_ms) override;
    uint16_t local_port() const override;

private:
    asio::io_context io_{};
    std::optional<asio::ip::tcp::acceptor> acceptor_;
};

}  // namespace lis
```

（本任务不动 TcpTransport 既有成员/方法，T5 才收窄。）

- [ ] **Step 4: 改 `lisserver\src\tcp_transport.cpp`**——在文件末尾 `}  // namespace lis` 之前追加（TcpListener 实现 + 两个工厂；`listen/accept` 逻辑从旧 `TcpTransport::listen/accept` 平移，`accept` 改返回 `Accepted`，**先取端点快照再 move transport**）：

```cpp
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
        acceptor_->cancel();
        io_.restart();
        io_.run();          // 消费被取消的 accept handler，避免下一轮 run 悬垂
        return std::nullopt;
    }
    if (ec) return std::nullopt;
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
```

- [ ] **Step 5: 构建 + 跑新用例 + 全量回归**

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
lisserver/listest/x64/Debug/listest.exe --gtest_filter='Listener.*' --gtest_color=no
lisserver/listest/x64/Debug/listest.exe --gtest_color=no
```

Expected: 构建成功；`Listener.AcceptTimeoutRearms`、`Listener.AcceptCarriesRemoteEndpoint` 两例 PASS；全量 `[  PASSED  ] 66 tests.`。

- [ ] **Step 6: 提交**

```bash
git add lisserver/include/lis/tcp_transport.h lisserver/src/tcp_transport.cpp lisserver/tests/test_transport.cpp
git commit -m "lisserver: TcpListener 专职监听器与双工厂（make_tcp_listener/make_tcp_client），IListener 用例 2 例"
```

---

### Task 3: LisServer 切 IListener

**Files:**
- Modify: `lisserver\include\lis\lis_server.h`
- Modify: `lisserver\src\lis_server.cpp`

行为保持型重构：64+2 例全量回归即验收，无新增用例。**关键语义变化**（内部实现，不改公共 API）：
- `stop()` 原来靠 `listener_.close()` 让 accept 立即出错退出；IListener 无 close，改为 accept 走完 ≤500ms 轮询超时后检查 `stopping_` 退出。
- 端口释放（`StopReleasesPort`/`StopWithLiveClientIsClean` 依赖）改为在 stop() **末尾** `listener_.reset()`。reset 安全的前提链：acceptor 线程已 join → 读者线程已 join 且 `connections_.clear()` → `pool_.stop()` 已把全部业务任务落地（任务闭包持有的 `shared_ptr<Connection>` 随之释放，即所有借用 io_ 的 TcpTransport 已析构）→ 才销毁 TcpListener（io_context/acceptor）。
- `start()` 每次创建全新监听器（保持既有"stop 后可重启"行为，与今日 `TcpTransport::listen` 先 close 再重绑等价）。

- [ ] **Step 1: 改 `lisserver\include\lis\lis_server.h`**——include 行、`Connection::transport`、`listener_` 三处（注释微调，公共区一字不动）。改后完整文件：

```cpp
// include/lis/lis_server.h
#pragma once
#include <lis/router.h>
#include <lis/listener.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace lis {

// HL7 v2.x over MLLP 服务器：1 accept 线程 + 每连接 1 读线程 + 有界业务线程池。
// Router 全连接共享；帧违规/IO 错误只关闭当前连接。
// 生命周期不变量：connections_（借用 listener_ 持有的 io_context）必须声明在 listener_ 之后
// （先析构）；pool_ 停止必须先于成员析构（~LisServer 经 stop() 保证）。
class LisServer {
public:
    struct Config { uint16_t port; size_t workers = 4; int read_timeout_ms = 500; };
    using ConnListener = std::function<void(const std::string& ip, bool online)>;

    LisServer(Config cfg, Router router);
    ~LisServer();
    LisServer(const LisServer&) = delete;
    LisServer& operator=(const LisServer&) = delete;

    void start();
    void stop();
    uint16_t local_port() const;
    // 连接回调：在 acceptor 与各 reader 线程并发调用，回调自身须线程安全；
    // 不得在回调内调用 start()/stop()（reader 线程内 stop 会自 join 死锁）；
    // 须在 start() 前设置（此后对 conn_cb_ 的读写无锁）。
    void set_conn_listener(ConnListener cb);

private:
    struct Connection {
        std::unique_ptr<ITransport> transport;
        std::thread reader;
        std::atomic<bool> open{false};
        std::string ip;          // accept 时快照（transport 关闭后仍可用）
        uint16_t port = 0;
    };
    void accept_loop();
    void read_loop(std::shared_ptr<Connection> conn);

    Config cfg_;
    Router router_;
    // 同一连接多帧并发处理，回复到达顺序不保证（客户端须按 MSA-2 配对）
    ThreadPool pool_;
    std::unique_ptr<IListener> listener_;   // start() 创建、stop() 末尾销毁（释放端口）
    std::thread acceptor_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::mutex conn_mu_;
    ConnListener conn_cb_;
    std::atomic<bool> stopping_{false};
};

}  // namespace lis
```

- [ ] **Step 2: 改 `lisserver\src\lis_server.cpp`**——只动 `start()`、`local_port()`、`accept_loop()` 开头 6 行、`stop()` 四个函数（`read_loop` 与构造/析构一字不动；本文件**不需要** include tcp_transport.h，工厂声明就在 lis_server.h 已带入的 listener.h 里）：

```cpp
void LisServer::start() {
    if (acceptor_.joinable()) throw std::runtime_error("LisServer already started");
    listener_ = make_tcp_listener();             // 每次启动用全新监听器；stop() 末尾销毁以释放端口
    if (!listener_->listen(cfg_.port))
        throw std::runtime_error("listen failed on port " + std::to_string(cfg_.port));
    stopping_ = false;
    acceptor_ = std::thread([this] { accept_loop(); });
}

uint16_t LisServer::local_port() const { return listener_ ? listener_->local_port() : 0; }
```

`accept_loop()` 改为（只换头部 6 行，收死连接/回调/起 reader 的尾段原样平移）：

```cpp
void LisServer::accept_loop() {
    while (!stopping_.load()) {
        auto acc = listener_->accept(500);         // 500ms 轮询式 accept，给停止标志让路
        if (!acc) continue;
        auto conn = std::make_shared<Connection>();
        conn->ip = acc->remote_ip;                 // accept 时快照（transport 关闭后仍可用）
        conn->port = acc->remote_port;
        conn->transport = std::move(acc->transport);
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
        if (conn_cb_) conn_cb_(conn->ip, true);
        conn->reader = std::thread([this, conn] { read_loop(conn); });
    }
}
```

`stop()` 改为：

```cpp
void LisServer::stop() {
    if (stopping_.exchange(true)) return;
    if (acceptor_.joinable()) acceptor_.join();    // accept 走 ≤500ms 轮询超时后见 stopping_ 退出
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
    pool_.stop();                                  // 业务任务全部落地（framed_reply 闭包随之析构）
    listener_.reset();                             // 此刻无借用 io_ 的连接存活，销毁监听器释放端口
}
```

- [ ] **Step 3: 构建 + 全量回归**（重点看用例：`Server.StopReleasesPort`、`Server.StopWithLiveClientIsClean`、`Server.ConnListenerReportsOnlineOffline`、`Client.RetryAfterReconnect`——它们全部压在 stop()/端口释放语义上；每例 stop 比之前多等最多 500ms 属预期）

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
lisserver/listest/x64/Debug/listest.exe --gtest_color=no
```

Expected: 构建成功；`[  PASSED  ] 66 tests.`。

- [ ] **Step 4: probe 复验（仍红，属预期）**——`lis_client.h` 还含 tcp_transport.h：

```bash
"$MSB" tools/header_probe.vcxproj -p:Configuration=Debug -p:Platform=x64
```

Expected: 仍报 `C1083: 无法打开包括文件: "asio.hpp"`（现在唯一来源是 `lis_client.h`）。

- [ ] **Step 5: 提交**

```bash
git add lisserver/include/lis/lis_server.h lisserver/src/lis_server.cpp
git commit -m "lisserver: LisServer 改依赖 IListener——端口释放改为 stop 末尾销毁监听器，accept 轮询退出"
```

---

### Task 4: LisClient 切 ITransport（probe 转绿）

**Files:**
- Modify: `lisserver\include\lis\lis_client.h`
- Modify: `lisserver\src\lis_client.cpp`

**线程安全要点**（实现里已体现，理解后照抄）：`transport_` 的**替换**只发生在 `ensure_connected()`，且替换前已 join 旧 reader（旧 reader 退出是 `connected_==false` 的前提），因此任何 reader 不会看到换对象；`disconnect()` 只 `close()` 不销毁对象——销毁必须等 reader 收尸，否则 reader 在已析构对象上 read 是 UB。

- [ ] **Step 1: 改 `lisserver\include\lis\lis_client.h`**——两处：include 行换掉 tcp_transport.h、成员 `transport_` 换类型（公共区一字不动）：

```cpp
// include/lis/lis_client.h
#pragma once
#include <lis/message.h>
#include <lis/transport.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
```

（类声明其余不动，仅成员行：）

```cpp
    Config cfg_;
    std::unique_ptr<ITransport> transport_;   // ensure_connected() 时经 make_tcp_client() 重建
```

- [ ] **Step 2: 改 `lisserver\src\lis_client.cpp`**——四处（本文件同样**不需要** include tcp_transport.h，工厂声明在 transport.h；`send_and_receive` 只改第 80 行的 `transport_.write` → `transport_->write`，`read_loop` 只改第 114 行的 `transport_.read` → `transport_->read`，其余不动）：

```cpp
void LisClient::ensure_connected() {
    std::lock_guard<std::mutex> lk(connect_mu_);
    if (connected_.load() || stopping_.load()) return;
    if (reader_.joinable()) reader_.join();          // 旧 reader 已退出（connected_==false 的前提），先收尸再换传输
    transport_ = make_tcp_client();                  // 每次连接用全新传输对象（旧对象随赋值析构）
    if (!transport_->connect(cfg_.host, cfg_.port, cfg_.timeout_ms))
        throw LisIoError("connect failed: " + cfg_.host + ":" + std::to_string(cfg_.port));
    connected_ = true;
    reader_ = std::thread([this] { read_loop(); });
}

void LisClient::disconnect() {
    std::lock_guard<std::mutex> lk(connect_mu_);
    connected_ = false;
    if (transport_) transport_->close();             // 打断阻塞读；对象存活到 ensure_connected 收尸后重建
    if (reader_.joinable() && reader_.get_id() != std::this_thread::get_id()) reader_.join();
    std::lock_guard<std::mutex> pl(pending_mu_);
    for (auto& [k, p] : pending_)
        p->prom.set_exception(std::make_exception_ptr(LisIoError("connection lost")));
    pending_.clear();
}
```

- [ ] **Step 3: 构建 + 全量回归**（重点：`Client.RetryAfterReconnect` 压断线重建、`Client.TimeoutThrowsAfterRetries` 压连接失败路径）

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
lisserver/listest/x64/Debug/listest.exe --gtest_color=no
```

Expected: 构建成功；`[  PASSED  ] 66 tests.`。

- [ ] **Step 4: probe 构建——验收 #2 在此达成（转绿）**

```bash
"$MSB" tools/header_probe.vcxproj -p:Configuration=Debug -p:Platform=x64
```

Expected: `已成功生成`、0 错误。此刻公共 include 图（probe 所含 14 头）不含 asio。

- [ ] **Step 5: 提交**

```bash
git add lisserver/include/lis/lis_client.h lisserver/src/lis_client.cpp
git commit -m "lisserver: LisClient 改依赖 ITransport 工厂——公共 include 图不再含 asio，header probe 转绿"
```

---

### Task 5: TcpTransport 收窄 + 测试迁移 + 收尾

**Files:**
- Modify: `lisserver\include\lis\tcp_transport.h`（删 `listen/accept/local_port/acceptor_`）
- Modify: `lisserver\src\tcp_transport.cpp`（删三个函数定义、close() 去 acceptor 分支）
- Modify: `lisserver\tests\test_transport.cpp`（4 例迁移 + 1 例删除）
- Modify: `lisserver\tests\test_client.cpp`（4 处监听角色迁移 + include 调整）
- Modify: `docs\2026-10-23-transport-abstraction-design.md`（v0.3 增补）
- 不动：`tests\test_server.cpp`（`transact()` 用的是 TcpTransport **客户端**角色 connect/write/read/close，全部保留）、`demo\lis_demo.cpp`（不直接用 TcpTransport）

- [ ] **Step 1: 收窄 `lisserver\include\lis\tcp_transport.h`**（删监听三方法与 `acceptor_`，双构造保留——`TcpListener::accept` 还要用 `TcpTransport(io)`；改后完整文件）：

```cpp
// include/lis/tcp_transport.h
#pragma once
#include <lis/listener.h>
#include <lis/transport.h>
#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace lis {

// standalone Asio 同步实现：连接端（客户端连接 / 服务端 accept 出的连接）。
// 客户端自有 io_ 仅驱动 connect 的超时等待（async_connect + run_for，单线程使用）；
// 被接受的连接借用 TcpListener 的 io_context（Windows 下 socket 句柄不能跨 io_context
// 二次关联完成端口），其 read/write 走同步路径（SO_RCVTIMEO），从不 run io_。
// 线程契约：read 与 write 可并发（write 内部按 transport 串行化）；
// close 可从另一线程调用，会打断该 transport 上阻塞中的 read。
class TcpTransport : public ITransport {
public:
    TcpTransport();                                 // 客户端：自有 io_context
    explicit TcpTransport(asio::io_context& io);    // 服务端 accept 出的连接：借用 listener 的 io
    ~TcpTransport() override;
    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeout_ms) override;
    int  read(uint8_t* buf, size_t n, int timeout_ms) override;
    bool write(const uint8_t* buf, size_t n) override;
    void close() override;

    std::string remote_ip() const;
    uint16_t remote_port() const;

private:
    friend class TcpListener;                      // accept 直插 socket_（借用同一 io_context）
    asio::io_context io_{};
    asio::ip::tcp::socket socket_;
    std::mutex write_mu_;
};

// 专职监听端：持有 io_context 与 acceptor；accept 出的 TcpTransport 借用本对象 io_。
// 线程契约：析构前须保证被接受的连接已全部销毁（LisServer 的成员顺序不变量保证）。
class TcpListener : public IListener {
public:
    TcpListener();
    ~TcpListener() override;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool listen(uint16_t port) override;
    std::optional<Accepted> accept(int timeout_ms) override;
    uint16_t local_port() const override;

private:
    asio::io_context io_{};
    std::optional<asio::ip::tcp::acceptor> acceptor_;
};

}  // namespace lis
```

- [ ] **Step 2: 收窄 `lisserver\src\tcp_transport.cpp`**——整体删除 `TcpTransport::listen`（约 L38-52）、`TcpTransport::accept`（约 L54-72）、`TcpTransport::local_port`（约 L104-108）三个函数定义；`TcpTransport::close()` 去掉 acceptor 分支：

```cpp
void TcpTransport::close() {
    std::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);   // 规范打断阻塞读
        socket_.close(ec);
    }
}
```

其余（构造/析构/connect/read/write/remote_ip/remote_port 与 Task 2 加的 TcpListener 全套、双工厂）不动。

- [ ] **Step 3: 构建验证"红"**（编译错误即迁移定位清单——此刻引用已删成员的只有测试文件，server/client/demo 均已切换）：

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
```

Expected: lislib 编译通过；`tests\test_transport.cpp` 与 `tests\test_client.cpp` 报 `error C2039: "listen": 不是 "lis::TcpTransport" 的成员`（及 accept/local_port 同类），逐个即迁移点。

- [ ] **Step 4: 迁移 `tests\test_transport.cpp`**——改后完整文件（变更：include 不变；`LoopbackEcho`/`ReadTimeoutThenStillUsable`/`ConnectTimeoutThenReusable`/`CloseUnblocksBlockedRead` 四例改用 `make_tcp_listener()` 与 `Accepted`；删除 `Transport.AcceptTimeoutRearms`（已被 Task 2 的 `Listener.AcceptTimeoutRearms` 取代）；原行尾 `listener.close()` 一律删除——unique_ptr 出作用域自毁即释放）：

```cpp
// tests/test_transport.cpp
#include <gtest/gtest.h>
#include <lis/listener.h>
#include <lis/tcp_transport.h>
#include <lis/thread_pool.h>
#include <atomic>
#include <chrono>
#include <thread>

TEST(Transport, LoopbackEcho) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));                      // 0 = 随机端口
    const auto port = listener->local_port();
    ASSERT_NE(port, 0u);

    std::thread server_thread([&] {
        auto peer = listener->accept(3000);
        ASSERT_TRUE(peer.has_value());
        uint8_t buf[64];
        int n = peer->transport->read(buf, sizeof buf, 3000);
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->transport->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");

    client.close();
    server_thread.join();
}

TEST(ThreadPool, CallerRunsWhenQueueFull) {
    lis::ThreadPool pool(1, 1);                            // 1 worker，队列上限 1
    std::atomic<int> done{0};
    pool.submit([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++done; });
    pool.submit([&] { ++done; });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_EQ(done.load(), 2);
}

TEST(ThreadPool, CallerRunsDeterministic) {
    lis::ThreadPool pool(1, 0);                            // 队列上限 0：任何 submit 都 CallerRuns
    std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> on_caller{false};
    pool.submit([&] { on_caller = std::this_thread::get_id() == caller; });
    ASSERT_TRUE(on_caller.load());
}

TEST(Transport, ReadTimeoutThenStillUsable) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();

    std::thread server_thread([&] {
        auto peer = listener->accept(3000);
        ASSERT_TRUE(peer.has_value());
        uint8_t buf[64];
        ASSERT_EQ(peer->transport->read(buf, sizeof buf, 200), 0);    // 第一次超时
        ASSERT_EQ(peer->transport->read(buf, sizeof buf, 200), 0);    // 第二次超时（无悬垂 handler 干扰）
        int n = peer->transport->read(buf, sizeof buf, 3000);         // 超时后连接仍可正常收数据
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->transport->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(600));     // 让 server 走完两次超时
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("ok"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "ok");

    client.close();
    server_thread.join();
}

// 原 Transport.AcceptTimeoutRearms 已由紧随其后的 Listener.AcceptTimeoutRearms 取代（T5 迁移）

TEST(Listener, AcceptTimeoutRearms) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    ASSERT_FALSE(listener->accept(200).has_value());       // 第一次超时
    ASSERT_FALSE(listener->accept(200).has_value());       // 第二次超时（重挂无残留 handler）
    lis::TcpTransport client;                              // 超时后仍能接受真连接
    ASSERT_TRUE(client.connect("127.0.0.1", listener->local_port(), 2000));
    ASSERT_TRUE(listener->accept(2000).has_value());
}

TEST(Listener, AcceptedCarriesRemoteEndpoint) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    ASSERT_NE(port, 0u);

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000));
    auto accepted = listener->accept(2000);
    ASSERT_TRUE(accepted.has_value());
    ASSERT_EQ(accepted->remote_ip, "127.0.0.1");
    ASSERT_GT(accepted->remote_port, 0u);
    ASSERT_NE(accepted->transport, nullptr);

    ASSERT_TRUE(accepted->transport->write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[8]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 2000), 2);      // Accepted.transport 可正常收发
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");
}

TEST(Transport, ConnectTimeoutThenReusable) {
    lis::TcpTransport t;
    ASSERT_FALSE(t.connect("192.0.2.1", 12345, 500));      // RFC5737 不可路由：超时或立即失败均可
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    ASSERT_TRUE(t.connect("127.0.0.1", listener->local_port(), 2000));  // 同一对象重连成功
    ASSERT_TRUE(listener->accept(2000).has_value());
}

TEST(Transport, CloseUnblocksBlockedRead) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 2000));
    auto peer = listener->accept(2000);
    ASSERT_TRUE(peer.has_value());

    std::atomic<int> read_result{-100};
    std::thread reader([&] {
        uint8_t buf[64];
        read_result = peer->transport->read(buf, sizeof buf, 10000);   // 阻塞最长 10s
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto start = std::chrono::steady_clock::now();
    peer->transport->close();                              // 另一线程 close 解锁阻塞读
    reader.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    ASSERT_LT(elapsed, 5000);                              // 远未到 10s 即返回
    ASSERT_EQ(read_result.load(), -1);
}
```

- [ ] **Step 5: 迁移 `tests\test_client.cpp`**——include 区把 `#include <lis/tcp_transport.h>` 换成 `<lis/listener.h>` + `<lis/transport.h>`；四个用例的监听角色改用 `make_tcp_listener()`（`peer` 由 `unique_ptr<TcpTransport>` 变为 `optional<Accepted>` / `unique_ptr<ITransport>`；行尾 `listener.close()` 删除）。四个用例改后全文：

```cpp
TEST(Client, TimeoutThrowsAfterRetries) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    std::thread t([&] {
        auto peer = listener->accept(3000);
        if (peer) std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    lis::LisClient client({"127.0.0.1", port, 400, 1});     // 1 次重试
    lis::Er7Parser p;
    ASSERT_THROW(client.send_and_receive(p.parse(kQry)), lis::LisTimeoutError);
    t.join();
}

// I-1：并发在途时任一方超时断开，"连接丢失"不得直抛给另一方——须同样走重试直到超时
TEST(Client, ConcurrentTimeoutAllRetryToTimeout) {
    auto listener = lis::make_tcp_listener();               // 永不应答对端：接受连接但既不回应也不关闭
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    std::atomic<bool> running{true};
    std::vector<std::unique_ptr<lis::ITransport>> peers;
    std::thread acceptor([&] {
        while (running.load())
            if (auto peer = listener->accept(200)) peers.push_back(std::move(peer->transport));
    });
    lis::LisClient client({"127.0.0.1", port, 400, 2});      // 2 次重试
    auto send = [](lis::LisClient& c, const char* qry, std::promise<std::string>& done) {
        try {
            lis::Er7Parser p;
            c.send_and_receive(p.parse(qry));
            done.set_value("ok");
        } catch (const lis::LisTimeoutError&) { done.set_value("timeout"); }
        catch (const lis::LisIoError&) { done.set_value("io"); }   // 修复前：他方断开直抛给本调用方
        catch (...) { done.set_value("other"); }
    };
    std::promise<std::string> ra, rb;
    auto fa = ra.get_future();
    auto fb = rb.get_future();
    std::thread ta([&] { send(client, kQryA, ra); });        // 独立控制 ID 并发在途（同 ID 复用会先触发 I-2 拒绝）
    std::thread tb([&] { send(client, kQryB, rb); });
    ta.join();
    tb.join();
    running = false;
    acceptor.join();
    for (auto& peer : peers) peer->close();
    ASSERT_EQ(fa.get(), "timeout");
    ASSERT_EQ(fb.get(), "timeout");
}

// I-2：并发复用在途 MSH-10 须立即响亮拒绝（不发送、不烧满超时）
TEST(Client, DuplicateControlIdRejected) {
    auto listener = lis::make_tcp_listener();
    ASSERT_TRUE(listener->listen(0));
    const auto port = listener->local_port();
    std::thread holder([&] {                                 // 单次 accept 持有即足：被拒方不建新连接
        if (auto peer = listener->accept(3000))
            std::this_thread::sleep_for(std::chrono::seconds(2));
    });
    lis::LisClient client({"127.0.0.1", port, 2000, 0});     // 0 次重试
    std::thread bg([&] {                                     // 后台先登记在途（结局超时，忽略）
        lis::Er7Parser p;
        try { client.send_and_receive(p.parse(kQryDup)); } catch (...) {}
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 确保对方已登记在途
    lis::Er7Parser p;
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_THROW(client.send_and_receive(p.parse(kQryDup)), lis::LisIoError);   // EXPECT：失败也要走到下方 join 清理
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsed, 1000);                              // 立即拒绝而非等待整个超时
    bg.join();
    holder.join();
}
```

`ReportAsyncCallbacks` 第二个块（死端口）改为：

```cpp
    {
        auto t = lis::make_tcp_listener();                 // 拿一个刚关闭的端口：连接必失败
        ASSERT_TRUE(t->listen(0));
        const auto dead_port = t->local_port();
        t.reset();                                          // 销毁监听器 = 释放端口
        lis::LisClient client({"127.0.0.1", dead_port, 500, 1});
        lis::Er7Parser p;
        std::promise<std::pair<std::string, bool>> done;
        auto fut = done.get_future();
        client.report_async(p.parse(kQry), "RA2",
                            [&](const std::string& id, bool ok) { done.set_value({id, ok}); });
        const auto r = fut.get();
        ASSERT_EQ(r.first, "RA2");
        ASSERT_FALSE(r.second);
    }
```

- [ ] **Step 6: 构建 + 全量测试（最终 65 = 64 既有 − 1 被取代 + 2 新增）**

```bash
"$MSB" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64
lisserver/listest/x64/Debug/listest.exe --gtest_color=no
```

Expected: 构建成功；`[  PASSED  ] 65 tests.`。

- [ ] **Step 7: probe 复验（绿）**

```bash
"$MSB" tools/header_probe.vcxproj -p:Configuration=Debug -p:Platform=x64
```

Expected: `已成功生成`、0 错误。

- [ ] **Step 8: 公共 API 签名 diff 验证（验收 #4）**

```bash
git diff 3b9f102 -- lisserver/include/lis/lis_server.h lisserver/include/lis/lis_client.h
```

Expected: 差异**只**出现在——include 行（tcp_transport.h → listener.h / transport.h）、私有区成员类型（`TcpTransport listener_` → `std::unique_ptr<IListener> listener_`、`Connection::transport`、`TcpTransport transport_` → `std::unique_ptr<ITransport> transport_`）与对应注释；`public:` 区（构造/析构/删拷贝/start/stop/local_port/set_conn_listener/send_and_receive/connected/report_async/Config/别名）逐字节一致。发现任何公共区 diff 即回退修正。

- [ ] **Step 9: 设计文档 v0.3 增补**——编辑 `docs\2026-10-23-transport-abstraction-design.md`：第 4 行"状态"改为：

```
状态：已与用户确认；2026-10-23 实施完成（v0.3 增补见 §6）
```

文件末尾追加：

```markdown
## 6. v0.3 增补（实施固化）

**边界规则：lislib 公共头不得引入 asio 或任何传输实现头。** `tcp_transport.h` 为库内实现头（接入方不直接包含），传输一律经 `ITransport`/`IListener` 与 `make_tcp_client()`/`make_tcp_listener()` 工厂消费。

守护机制：仓库根 `tools/header_probe.vcxproj` + `tools/header_probe.cpp`——包含全部公共头（不含 `tcp_transport.h`）；该工程位于 `lisserver\Directory.Build.props` 作用域之外（不获 conan asio 注入），仅以 `-I lisserver\include` 编译，能编过即边界未破。

实施与 §3.3 的一处简化：`lis_server.cpp`/`lis_client.cpp` 最终**不**需要 include `tcp_transport.h`——两个工厂的声明就在抽象头里，具体类型只在 `tcp_transport.cpp` 内出现。

实施与 §3.1 的一处接口内行为：`IListener` 无 `close()`；`LisServer::stop()` 末尾 `listener_.reset()` 释放监听端口（此刻借用 io_ 的连接已全部析构），accept 线程经 ≤500ms 轮询超时退出。
```

- [ ] **Step 10: 提交**

```bash
git add lisserver/include/lis/tcp_transport.h lisserver/src/tcp_transport.cpp lisserver/tests/test_transport.cpp lisserver/tests/test_client.cpp docs/2026-10-23-transport-abstraction-design.md
git commit -m "lisserver: TcpTransport 收窄为连接端、测试迁移至 IListener；设计文档 v0.3 固化边界规则"
```

---

## 验收对照（设计 §4）

| # | 验收标准 | 达成位置 |
|---|---|---|
| 1 | 既有用例全绿 + 新增 IListener 用例绿 | T2 新增 2 例；T2-T5 每任务全量回归；最终 65（64 − 1 取代 + 2 新增） |
| 2 | header probe 编译通过（仅 `-I include`，无 asio 路径） | T1 红证明守卫有效 → T4 转绿 → T5 复验 |
| 3 | 设计文档 v0.3 记录边界规则 | T5 Step 9 |
| 4 | `lis_server.h`/`lis_client.h` 公共 API 签名与改前相同 | T5 Step 8（对基线 3b9f102 diff 验证） |

## 执行注意事项

- 每个任务收尾时解决方案必须全绿再提交；红只在 T1 的 probe、T2 的链接错误、T5 的编译错误这三个"计划内红"出现，其余任何失败先停下排查，不得带病提交。
- 本计划全部命令假定 Git Bash、仓库根为工作目录；MSBuild 全路径含空格，务必加引号。
- 任务间有严格顺序依赖（expand → migrate → contract），不可并行、不可调换。

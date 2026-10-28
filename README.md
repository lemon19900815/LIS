# LIS

HL7 v2.x（ER7）over MLLP 的 LIS 通信库，C++17，同时提供**服务端**与**客户端**。
公共头只依赖标准库：消费方包含 `<lis/...>` 并链接 `liblis.lib` 即可，**不需要 asio**。

## 能力

- **服务端** `LisServer`：1 个 accept 线程 + 每连接 1 个读线程 + 有界业务线程池；多连接共享 `Router`；帧违规/IO 错误只关闭当前连接。
- **客户端** `LisClient`：同步 `send_and_receive`，按 MSH-10 / MSA-2 关联应答；超时或写失败后丢弃连接、原报文重试；多线程共享安全。
- **报文层**：`Er7Parser`（parse/encode）、`Message` 树（段 → 字段 → 重复项 → 组分）、`Terser` 路径读写；ER7 转义按报文自身分隔符处理。
- **内置处理器**：`QryHandler`（QRY^Q02 → ORF^Q02）、`OruHandler`（ORU^R01 → ACK）、`AckBuilder`、`ResponseBuilder`、`Router`（支持 `*` 通配，无命中回 AR，处理器抛异常回 AE）。
- **传输抽象**：`ITransport` / `IListener` + 默认 TCP 实现，可在测试或私有部署中替换。

## 目录结构

```
liblis/
  include/lis/     公共头（14 个，消费方唯一依赖）
  src/             实现与实现头（tcp_transport.h、mllp_codec.h 不对外分发）
  tests/           gtest 用例
  demo/            lis_demo.cpp（服务端 + 自连示例）
  target/          vendored 依赖（x64-windows）
tools/header_probe 公共头边界守卫（asio 不得可见）
build.bat          构建脚本
docs/              设计与实施记录
```

## 构建

环境：

- `build.bat`（默认）：VS2022，MSBuild 17.x，工具集 **v143**，产出供 VS2022 消费方链接的静态库。
- `build.bat -t sln`：VS18，MSBuild 18.x，工具集 **v145**，构建整个解决方案（含 `listest` / `lisdemo`）。

第三方依赖已 vendored 在 `liblis/target/x64-windows`（asio 1.30.2 头、gtest 1.14.0），构建不经过 conan。

```bat
build.bat                 :: 静态库 Debug + Release（v143）
build.bat -c Debug        :: 单配置
build.bat -t sln          :: 解决方案 Debug（v145，含测试与 demo）
```

产物：

| 配置 | 库 | 运行时 |
|---|---|---|
| Debug | `liblis/lib/x64/Debug-v143/liblis.lib` + `liblis.pdb` | `/MDd` |
| Release | `liblis/lib/x64/Release-v143/liblis.lib` + `liblis.pdb` | `/MD` |

退出码：`0` 成功、`1` 构建失败、`2` 用法错误、`3` 找不到所需 MSBuild、`4` 缺 vendored 依赖。

> `-t sln` 产出的 `liblis/lib/x64/Debug/liblis.lib` 是 v145，**不能**给 VS2022 消费方链接（工具集 ABI 不匹配）。

## 测试

```bat
build.bat -t sln
liblis\listest\x64\Debug\listest.exe
```

共 83 个用例，覆盖 MLLP 帧状态机、ER7 解析与转义、Terser、ACK/响应构造、路由、传输抽象、服务端并发、客户端重试与端到端流程。

Demo 同时是手工探针：

```bat
liblis\lisdemo\x64\Debug\lisdemo.exe              :: 起服务、自连、发 QRY^Q02 与 ORU^R01
liblis\lisdemo\x64\Debug\lisdemo.exe --connect 6889  :: 只作客户端连指定端口
```

## 快速上手

服务端：注册路由后启动。

```cpp
#include <lis/lis_server.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/router.h>

#include <memory>
#include <vector>

lis::Router router;
router.add("QRY", "Q02", "*",
           std::make_shared<lis::QryHandler>(
               std::make_shared<lis::SimProvider>(
                   std::vector<std::string>{"GLU", "ALT", "WBC"})));
router.add("ORU", "R01", "*",
           std::make_shared<lis::OruHandler>([](const lis::Message& oru) {
               // 业务池线程并发回调，实现须线程安全；抛异常会回 AE
           }));

lis::LisServer server({6889, /*workers=*/4, /*read_timeout_ms=*/500},
                      std::move(router));
server.set_conn_listener([](const std::string& ip, bool online) {
    // accept 线程与各读线程并发调用
});
server.start();
// ...
server.stop();
```

客户端：

```cpp
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/terser.h>

lis::Er7Parser parser;
lis::LisClient client({"127.0.0.1", 6889, /*timeout_ms=*/3000, /*retry=*/1});

lis::Message reply = client.send_and_receive(parser.parse(
    "MSH|^~\\&|I1000|I1000|LIS|LIS|20261026120001||QRY^Q02|E2|P|2.3.1\r"
    "QRD|||||BAR001"));

std::string ack_code = lis::Terser::get(reply, "/MSA-1");   // AA / AE / AR
```

内置 `QryHandler` 只处理 QRY^Q02。其它查询（如 QRY^Q01）用 `FnHandler` 自行注册：

```cpp
router.add("QRY", "Q01", "*",
           std::make_shared<lis::FnHandler>(
               [](const lis::Message& in, const lis::MessageMeta&) {
                   // 按业务自组 ORF/DSR 等段；返回空 Message 表示不回
                   return lis::build_ack(in, lis::AckCode::AA);
               }));
```

## 公共头

| 头文件 | 用途 |
|---|---|
| `lis/message.h` | `Message` / `Segment` / `Field` / `Repetition` / `EncodingChars` / `MessageMeta` |
| `lis/er7_parser.h` | `Er7Parser::parse/encode`，`ParseError` |
| `lis/terser.h` | `Terser::get/set`，路径 `/SEG-n(rep)-m-k`，`TerserPathError` |
| `lis/router.h` | `IHandler` / `FnHandler` / `Router` |
| `lis/ack_builder.h` | `build_ack`，`AckCode{AA, AE, AR}` |
| `lis/response_builder.h` | `build_response_msh`（收发互换、MSH-10 新控制 ID） |
| `lis/provider.h` | `ISampleProvider` / `QueryConditions` / `SampleInfo` / `SimProvider` |
| `lis/qry_handler.h` | `QryHandler`（QRY^Q02 → ORF^Q02） |
| `lis/oru_handler.h` | `OruHandler`（ORU^R01 → ACK） |
| `lis/lis_server.h` | `LisServer` |
| `lis/lis_client.h` | `LisClient`，`LisTimeoutError` / `LisIoError` |
| `lis/transport.h` | `ITransport`、`make_tcp_client` |
| `lis/listener.h` | `IListener`、`Accepted`、`make_tcp_listener` |
| `lis/thread_pool.h` | 有界 `ThreadPool`（队列满时 CallerRuns） |

## 关键语义

- **分隔符来自报文本身**：`EncodingChars` 由 MSH-1 / MSH-2 决定，不硬编码。
- **转义模型**：内存中的组分文本是解码后的字面量——`parse` 还原 `\F\ \S\ \R\ \T\ \E\ \Xdd..\`，`encode` 再按分隔符转义写线；MSH-2 原样输出；未知序列原样保留。字面 `&` 在组分内保留，写线时转义为 `\T\`。
- **Terser 路径**：`/SEG-n`（字段）、`/SEG-n(rep)`（重复项）、`/SEG-n(rep)-m`（组分）、`/SEG-n(rep)-m-k`（子组分）；编号 1..999，越界抛 `TerserPathError`。从零构造消息须先 `set("/MSH-2", ...)`。
- **路由**：先注册先命中；任一项 `*` 通配；无命中回 AR；处理器抛异常回 AE。
- **ACK**：MSA-2 与 MSH-10 都回带入站控制 ID，仪器据此配对。
- **线程契约**：`LisServer::set_conn_listener` 须在 `start()` 前设置，回调内不得调用 `start()`/`stop()`；`LisClient` 并发在途调用的 MSH-10 必须唯一，复用会以 `LisIoError` 拒绝且不发送。

## 依赖与分发

- 分发内容：`include/lis/*.h` + `liblis.lib`（建议同时带 `.pdb`）。
- 消费方不需要 asio、conan 或 vendored 目录；公共头禁 asio 由 `tools/header_probe.vcxproj` 守卫——它只包含公共目录，asio 一旦可见即编译失败。
- 刷新 vendored 依赖：`conan install liblis` → 从 conan 缓存按 `target/x64-windows` 布局拷入头/库，并更新版本记录。
- 公共头若新增，必须同步加入 `tools/header_probe.cpp` 的清单，否则守卫范围会静默缩小。

## 文档

- [liblis v143 构建与工具集分工](docs/2026-10-26-liblis-v143-build.md)
- [服务端/客户端设计](docs/2026-10-22-cpp-lis-server-design.md)
- [传输层抽象设计（含 v0.3 边界规则）](docs/2026-10-23-transport-abstraction-design.md)
- [目录改名与构建脚本设计](docs/2026-10-26-liblis-rename-and-build-script-design.md)
- [HAPI HL7 服务端/客户端调研](docs/hapi-hl7-server-research.md)、[客户端](docs/hapi-hl7-client-research.md)

## 维护约定

- 批处理脚本保持 CRLF（见 `.gitattributes`）。
- 客户协议文档（`docs/*.docx`）与 Word 临时锁文件不入库。

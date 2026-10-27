# C++ LIS 服务器/客户端 设计文档

> 日期：2026-10-22　状态：草案 v0.2，待评审
> 依据：`docs/hapi-hl7-server-research.md`、`docs/hapi-hl7-client-research.md`（hapi-hl7v2 一手源码调研）
> 已确认决策：架构走**方案 A 分层内核**；交付形态为**独立核心库 + demo**（不依赖 Qt）；**依赖经 Conan 管理，网络库使用独立 Asio**（不直接调用 WinSock）

---

## 1. 背景与目标

迈克生物仪器调试需要模拟 LIS 服务器能力（`liscomm/liscomm.hpp` 中 `LISCOMM_MSG_ACK` 注释已预留此用途）。现有 `liscomm.dll` 只覆盖仪器侧客户端（HL7/ASTM、网口/串口/文件），服务端缺位。

**目标**：用 C++17 实现一个可独立交付的核心库，提供：

1. **HL7 v2.x over MLLP 服务器**：接受仪器 TCP 连接，处理 `QRY^Q02` 查询（回样本申请信息）、`ORU^R01` 结果接收（回 ACK），支持多仪器并发连接；
2. **同内核的客户端**：用于自测与后续主动下发场景（对齐 liscomm 的同步查询/异步上报语义）；
3. **模拟数据源**：不连真实 LIS 数据库即可应答仪器查询，服务调试场景。

## 2. 非目标（v1 明确不做）

| 不做 | 理由 |
|---|---|
| ASTM 帧协议 | 另一套帧格式（ENQ/EOT/校验和），v1 只做 HL7 |
| 串口/文件传输 | 仅定义 `ITransport` 接口预留，v1 只实现 TCP |
| TLS | 内核传输层预留开关，v1 不实现 |
| HL7 over HTTP、XML 编码 | 仪器场景用不到 |
| 按 v2.x 版本生成的巨型结构类库 | 用通用消息树 + 路径取值代替（HAPI 的 GenericMessage 先例） |
| 真实 LIS 数据库对接 | 查询数据源抽象为接口，v1 内置模拟实现 |
| Asio 事件驱动（io_context）异步模型 | v1 按线程模型同步使用 asio（见 §8），不引入事件循环 |

## 3. 架构总览（分层内核）

```
┌─────────────────────────────────────────────────────┐
│ demo / 未来 RisDebug UI / 其他调用方                    │
├─────────────────────────────────────────────────────┤
│ 外壳层（可替换）： LisServer │ LisClient               │
│   server: accept线程 + 每连接读线程 + 业务线程池          │
│   client: 发送-等待 + 重试 + 连接状态                   │
├─────────────────────────────────────────────────────┤
│ 协议内核层（零 IO 依赖、不感知 TCP，可独立单测）：          │
│   Router → IHandler                                    │
│   AckBuilder                                           │
│   Er7Parser / Terser（消息树 Message/Segment/Field）     │
│   MllpCodec（帧状态机）                                 │
├─────────────────────────────────────────────────────┤
│ 传输层（抽象）： ITransport → TcpTransport(Asio)        │
└─────────────────────────────────────────────────────┘
```

内核层不调用任何 socket API；外壳层负责线程与 IO。客户端与服务器共用全部内核代码——这是 HAPI（`LowerLayerProtocol` 可插拔、`SimpleServer`/`Initiator` 共用 Parser/Router）验证过的架构形状。

## 4. 模块划分

新目录 `lisserver/`（名可评审时调整），编译为静态库（默认）或 DLL，附带 `demo/lis_demo.cpp` 控制台程序。

| 模块 | 职责 | 依赖 |
|---|---|---|
| `mllp/` MllpCodec | 帧编解码状态机：等 0x0B → 收正文 → 等 0x1C → 等 0x0D；违规抛协议异常；发送整帧单次 write+flush | 无 |
| `er7/` 消息树 + Er7Parser | 分隔符取自报文本身（第 4 字符为 MSH-1，第 5-8 为 MSH-2），递归拆 段→字段→组分→子组分；MSH/FHS/BHS 前两字段特殊处理；版本取 MSH-12 | 无 |
| `er7/` Terser | 路径取值/赋值：`"/MSH-9-2"`、`"/OBR-4(1)"` | 消息树 |
| `ack/` AckBuilder | 照抄 HAPI `AbstractMessage.fillResponseHeader` 规则生成 ACK/NAK | 消息树、Terser |
| `routing/` Router + IHandler | `(类型, 触发事件, 版本)` → Handler 映射，`*` 通配，先注册先命中；无命中回 AR | 消息树 |
| `transport/` ITransport + TcpTransport | `connect/read/write/close` 抽象；standalone Asio 实现（跨平台），读超时 500ms 支持可中断循环 | Asio |
| `server/` LisServer | 1 accept 线程 + 每连接 1 读线程 + 有界业务线程池；连接上线/下线回调；优雅关闭 | 全部上层 |
| `client/` LisClient | `send_and_receive`：MSH-10 登记 → 写帧 → 按 MSA-2 关联应答 → 超时/失败重试 | 全部上层 |
| `provider/` ISampleProvider + SimProvider | 查询数据源接口；模拟实现（按条码返回可配置项目列表） | 消息树 |

## 5. 核心接口设计（形状示意，非最终头文件）

命名空间 `lis`；接口风格为 C++ 类（同工具集编译的 DLL 替换场景下 ABI 安全，与 liscomm 一致；若未来要求跨工具集换 DLL，再包一层扁平 C API）。

### 5.1 消息模型

```cpp
struct EncodingChars {              // 取自报文本身，非硬编码 |^~\&
    char field = '|', component = '^', repeat = '~', escape = '\\', subcomponent = '&';
};

struct Field  { std::vector<std::string> components; };        // 组分=^, 子组分=&（先扁平存储，编码时再拼）
struct Segment { std::string name; std::vector<Field> fields; };// MSH 特殊：fields[0] 为空占位，field 分隔符在 EncodingChars

struct Message {
    EncodingChars ec;
    std::string version;            // MSH-12，如 "2.3.1"
    std::vector<Segment> segments;
    std::string raw;                // 原始报文（日志/校验用）
};

struct MessageMeta {                // 随消息传给 Handler 的上下文
    std::string remote_ip; uint16_t remote_port;
    std::string control_id;         // 入站 MSH-10
};
```

### 5.2 Er7Parser / Terser

```cpp
class Er7Parser {
public:
    Message parse(const std::string& raw);        // 非法帧结构抛 ParseError
    std::string encode(const Message& m);
};

class Terser {
public:
    static std::string get(const Message&, const char* path);   // "/MSH-9-2", "/OBR-4"
    static void set(Message&, const char* path, const std::string& v);
};
```

### 5.3 MllpCodec

```cpp
class MllpCodec {
public:
    // 增量解帧：feed 返回完整消息字节（无完整帧返回 nullopt）；帧字节违规抛 ProtocolError
    std::optional<std::string> feed(uint8_t byte);
    std::string frame(const std::string& payload);  // 0x0B + payload + 0x1C 0x0D
};
```

### 5.4 AckBuilder（HAPI 规则照抄）

```cpp
enum class AckCode { AA, AE, AR };   // 对齐 liscomm LISCOMM_ACK_CODE

// 规则：MSH-1/2 原样复制；MSH-7=当前时间；MSH-9="ACK^原触发事件[^ACK]"；MSH-10=新消息 ID；
// MSH-11/12 复制入站；MSH-3/4/5/6 收发互换；MSA-1=code；MSA-2=入站 MSH-10；err 非空时补 ERR 段
Message build_ack(const Message& inbound, AckCode code, const std::string& err = {});
```

MSH 头构造已提炼为公共助手（`lis/response_builder.h`），非 ACK 响应（ORF 等）与 `build_ack` 共用同一套规则，
外部自定义 handler 不必再抄 MSH 互换/时间戳/控制 ID 的样板：

```cpp
// MSH-1 占位；MSH-2 原样复制（空则按入站 ec 重建）；MSH-3/4/5/6 收发互换；
// MSH-7 当前时间；MSH-9 = msg_type[^trigger]；MSH-10 新控制 ID；MSH-11/12 复制入站
Segment build_response_msh(const Message& inbound, const std::string& msg_type,
                           const std::string& trigger = {});
```

### 5.5 Router / IHandler

```cpp
class IHandler {
public:
    virtual ~IHandler() = default;
    virtual Message handle(const Message& in, const MessageMeta& meta) = 0;  // 返回空 Message=不回
};

class Router {
public:
    void add(std::string msg_type, std::string trigger, std::string version,
             std::shared_ptr<IHandler> handler);   // 任一项可 "*"
    Message dispatch(const Message&, const MessageMeta&);  // 无命中→AR "No appropriate destination"
};
```

### 5.6 ITransport / TcpTransport

```cpp
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool connect(const std::string& host, uint16_t port, int timeout_ms) = 0;
    virtual int  read(uint8_t* buf, size_t n, int timeout_ms) = 0;   // ≤0 表示超时/关闭
    virtual bool write(const uint8_t* buf, size_t n) = 0;            // 整帧单次写出
    virtual void close() = 0;
};
// TcpTransport：standalone Asio 实现；socket 读超时 500ms（可中断读循环，对齐 HAPI）
```

### 5.7 LisServer

```cpp
class LisServer {
public:
    struct Config { uint16_t port; size_t workers = 4; int read_timeout_ms = 500; };
    LisServer(Config, Router router);              // Router 全连接共享（对齐 HAPI 全局路由）
    void start();                                  // 起 accept 线程
    void stop();                                   // 置标志→关 acceptor→关全部连接→等线程退出
    void set_conn_listener(std::function<void(const std::string& ip, bool online)>);
};
```

### 5.8 LisClient

```cpp
class LisClient {
public:
    struct Config {
        std::string host; uint16_t port;
        int timeout_ms = 5000;      // 业务应答超时，对齐 liscomm 默认
        int retry = 3;              // 对齐 liscomm retry_times
    };
    explicit LisClient(Config);

    Message send_and_receive(const Message& out);   // 同步阻塞；超时→关连接重建→原报文重试 N 次
    bool connected() const;                          // 连接状态查询（HAPI 无此回调，此处补齐）
    // 异步上报：内部线程池，每消息按控制 ID 关联完成状态后回调（对齐 liscomm_async_report_result 语义）
    using StatusCb = std::function<void(const std::string& msg_id, bool ok)>;
    void report_async(const Message& result, std::string msg_id, StatusCb cb);
};
```

### 5.9 ISampleProvider / SimProvider

```cpp
struct QueryConditions {                            // 由 QRD/QRF 段解析而来
    std::string barcode;                            // 样本条码
    std::string sample_no;                          // 样本号（序号模式）
};

class ISampleProvider {
public:
    virtual ~ISampleProvider() = default;
    // 返回空 optional → Handler 回 "查无此样本"（AR 或空应答，按仪器约定）
    virtual std::optional<Message> make_response(const QueryConditions&, const MessageMeta&) = 0;
};

// SimProvider：配置文件/内置规则（条码 → 患者+项目列表），供调试无需真实数据库
```

v1 内置两个 Handler 实现：`QryHandler`（`QRY^Q02` → 调 provider → 组 `ORF^R02` 应答）与 `OruHandler`（`ORU^R01` → 解析 PID/OBR/OBX 交上层 → 回 AA）。

## 6. 数据流

**场景 1：仪器查询样本申请**
```
仪器 --QRY^Q02--> MllpCodec 出帧 → Er7Parser → Router(QRY,Q02,*) → QryHandler
     → ISampleProvider 查数据 → 组 ORF^R02 → AckBuilder 补 MSH/MSA 头 → encode → MllpCodec 成帧 --> 仪器
```

**场景 2：仪器上报结果**
```
仪器 --ORU^R01--> MllpCodec → Er7Parser → Router(ORU,R01,*) → OruHandler
     → 解析出结果结构交上层 → build_ack(inbound, AA) → encode → 成帧 --> 仪器
```

**客户端自测（demo）**：内置 LisClient 连 127.0.0.1 的 LisServer，发 QRY 收 ORF、发 ORU 收 ACK。

## 7. 错误处理策略（照 HAPI 语义）

| 错误 | 处理 |
|---|---|
| MLLP 帧字节违规 / IO 错误 | 只关闭**当前连接**，服务继续；客户端标记断开并触发状态变化 |
| 报文解析失败 | 速扫入站 MSH 凑出包头 → 回 NAK（AE + ERR 文本） |
| Handler 抛异常 | AE + ERR 文本（可在 Router 配置为不回） |
| 无路由命中 | AR（"No appropriate destination could be found..."） |
| 客户端应答超时 / 发送失败 | 关闭失效连接 → 重建 → 原报文重试，最多 N 次（每次独立计时）；仍失败抛 `LisTimeout`/`LisIoError` |
| 收到对端 NAK（AE/AR） | **不抛异常**，正常返回给调用方解析（对齐 HAPI；liscomm 语义由封装层翻译） |

## 8. 线程模型

- 服务器：1 accept 线程；每条连接 1 读线程（循环 read → MllpCodec.feed → 提交业务线程池）；业务提交到有界队列线程池（队列满由调用线程执行，对齐 HAPI `CustomThreading` 示例策略）；周期清理死连接。
- 客户端：1 读线程（收帧 → 按 MSA-2 投递到 pending 表）；发送线程 `send_and_receive` 内 登记(MSH-10) → 写帧 → 等待；超时即失败。多线程共享客户端安全（按消息 ID 归位，对齐 HAPI Initiator 线程安全语义）。
- 关闭顺序：置停止标志 → 关 acceptor（端口及时释放）→ 关全部连接（socket 关闭强迫读线程退出）→ join。
- **Asio 使用方式**：v1 以**同步方式**使用 asio（acceptor 的 accept、socket 的 read_some/write 直接运行在上述线程上），不引入 io_context 事件驱动模型；asio 仅作为跨平台 socket 抽象，业务代码不直接出现 WinSock API。

## 9. 工程与构建约束

- 工具集：VS2022 (v143) / Windows SDK 10.0.22621.0 / **C++17** / x64 / `/utf-8 /bigobj`（与 RisDebug 一致）
- **依赖管理：Conan**——`conanfile.txt` 声明依赖，用 MSBuildDeps 生成器产出 props 对接现有 vcxproj 构建流程；依赖版本在 conanfile 锁定
- **网络库：standalone Asio**（header-only，Conan 包 `asio`）——代码不直接调用 WinSock API；Asio 在 Windows 下所需系统库链接由 Conan/工程配置处理
- 不引入 Qt、不引入 Boost
- 输出：静态库（默认）+ `demo` 控制台程序；DLL 形态预留导出宏（对齐 liscomm 风格）
- 代码组织：单目录模块（`src/<模块>/`），头文件接口与实现分离，公共头集中于 `include/lis/`

## 10. 测试策略

1. **单元层**：MllpCodec 状态机（含违规字节用例）、Er7Parser（分隔符取自报文、MSH 特殊字段、组分递归）、AckBuilder 字段回填逐条断言（照 HAPI 规则）、Router 通配与优先级。
2. **端到端**：`demo` 内置 127.0.0.1 自测——起服务端 + 客户端收发 QRY/ORU/ACK 全链路。
3. **真实联调（后续）**：用 `liscomm.dll` 作仪器侧，新库作服务端对接验证。

## 11. 风险与开放问题

| 项 | 说明 |
|---|---|
| 消息 ID 重试语义 | 重试时是否更换 MSH-10 需按仪器协议约定（HAPI 不管 ID 生成，liscomm 亦未明确）；v1 沿用原 ID，联调时确认 |
| 查询应答报文形态 | `ORF^R02` 还是仪器自定义段序，需按目标仪器协议确认；v1 以参数化组段实现 |
| 字符集 | v1 固定可配置字符集（默认 GBK/ASCII 可配），不做 MSH-18 协商 |
| 目录/库名 | `lisserver/`、`namespace lis` 为暂定，评审可改 |

## 12. 参考

- `docs/hapi-hl7-server-research.md`（服务端：MLLP/线程模型/路由/ACK 规则出处）
- `docs/hapi-hl7-client-research.md`（客户端：sendAndReceive/超时/重试/连接复用出处）
- `liscomm/liscomm.hpp`（对照锚点：结构体与回调语义）

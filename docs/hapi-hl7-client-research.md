# HAPI HL7 客户端实现调研

> 调研日期：2026-10-22
> 调研人：调研代理（AI research agent，一手源码调研）
> 调研对象：`hapifhir/hapi-hl7v2` master 分支（HEAD `de15036`，2026-03-26）的**客户端（client）侧**实现，即"主动连接对端 HL7 服务器、发送消息并等待应答"的能力
> 方法：GitHub raw 源码 + api.github.com 目录树/提交历史一手取证；未整库 clone，未改动本仓库任何已有代码，仅新建本报告
> 姊妹篇：`docs/hapi-hl7-server-research.md`（服务端调研，MLLP/路由/ACK/线程模型结论不再重复，本文只引用其结论处会注明）

---

## 1. 结论速览（TL;DR）

**客户端入口与惯用法**：当前 master 上 HL7v2 客户端没有、也从未有过（GitHub 历史范围内）一个名叫 `Client` 的类。惯用法是两条工厂方法 + 两个接口：

```java
HapiContext context = new DefaultHapiContext();
Connection  connection = context.newClient("host", port, false);  // 或 newLazyClient
Initiator   initiator  = connection.getInitiator();
Message     response   = initiator.sendAndReceive(message);       // 阻塞等应答
```

`HapiContext.newClient(...)` 最终落到 `ConnectionHub.attach(...)` 的连接池（见 §4）。`HapiContext.getConnectionHub()` 在接口里已被标注 `@deprecated use newClient(...)`。

三个关键语义：

| 语义 | HAPI 客户端的行为 | 关键证据 |
|---|---|---|
| **连接复用** | `ConnectionHub` 对同一目标地址**至多维护一条连接**；`newClient()` 重复调用返回同一条已连接实例（引用计数）；缓存 key = `ConnectionData(host, port, port2, parser 类, llp 类)`；**`tls` 标志不在 key 内** | `app/ConnectionHub.java`、`app/ConnectionData.java`（§4） |
| **超时** | 等应答超时**默认 10000ms**，由 `Initiator.setTimeout(long, TimeUnit)` 或系统属性 `ca.uhn.hl7v2.app.initiator.timeout` 配置；超时抛 `ca.uhn.hl7v2.app.TimeoutException`（继承 `HL7Exception`）。注意它与 socket 读超时（默认 500ms，只为可中断读循环）是**两个独立机制** | `app/ActiveInitiator.java`、`util/StandardSocketFactory.java`（§3） |
| **重试** | **没有任何内建自动重试**。对 `Initiator`/`ActiveInitiator`/`ConnectionHub`/`LazyConnection`/`ConnectionFactory`/`HapiContext`/`DefaultHapiContext`/`BlockingHashMap` 全部客户端源码做 "retry/retries" 关键字排查：**零命中**。失败后由业务层决定（官方示例的模式是"关闭连接、下条消息重建连接"） | §5 |

**与 liscomm 最重要的一条对照**：迈克生物 `liscomm.dll` 有内建重试（`retry_times` 默认 3、`timeout_milliseconds` 默认 5000，`liscomm/liscomm.hpp` 第 91-92 行），而 **HAPI 客户端完全没有**——HAPI 只保证"发出去、按 MSH-10↔MSA-2 关联、等 10 秒、超时抛异常"这一个原子动作，重试、退避、失败状态回调都要业务层自己做。C++ 客户端若要对齐 liscomm 语义，需在封装层自行实现重试。

---

## 2. 客户端架构与类关系（含 "Client.java 去哪了" 考据）

### 2.1 类关系总览（全部经 master 源码验证）

```
HapiContext (interface, ca.uhn.hl7v2)
│   newClient(host, port, tls)                     ─┐
│   newLazyClient(host, port, tls)                   │ 工厂
│   newClient(host, outboundPort, inboundPort, tls) ─┤
│   newLazyClient(host, outboundPort, inboundPort)  ─┘
│   getConnectionHub()  @deprecated "use newClient"
└── DefaultHapiContext  ── 第 334-348 行：以上方法全部委托给 ConnectionHub.attach/attachLazily

ConnectionHub (app/ConnectionHub.java, extends HapiContextSupport)
│   缓存 CountingMap<ConnectionData, Connection>（引用计数；key 相同则复用）
│   attach(...) / attachLazily(...)  →  ConnectionFactory.open(...)
│   detach(c) 引用计数-1；discard(c) 强制关闭剔除；discardAll()
│   同步：ConcurrentMap<String,String> connectionMutexes，按目标地址加互斥锁
│
├── ConnectionFactory (app/ConnectionFactory.java, 包私有静态工厂)
│       open():  isLazy ? new LazyConnection(...) : openEagerly(...)
│       openEagerly(): new ActiveConnection(parser, llp, socket[, socket2]) + activate()
│       createSocket(): tls ? socketFactory.createTlsSocket() : createSocket(); connect(InetSocketAddress)
│
├── ActiveConnection (app/ActiveConnection.java)  ← 真正的客户端连接
│   ├── ActiveInitiator implements Initiator   ← 发送-等待主流程（§3）
│   ├── Responder（本连接也兼收"主动推来的消息"，双工对称）
│   ├── Receiver ×1（单端口）或 ×2（双端口），每连接读线程（Service 循环）
│   └── responses = BlockingHashMap<消息控制ID, 应答串>（MSA-2 关联，§3.2）
│
└── LazyConnection (app/LazyConnection.java)
        LazyInitiator：首次 sendAndReceive 时才 establishConnection()（同步方法）
```

接口层：
- `app/Connection.java`：客户端面对的连接抽象，方法仅 `activate()/getInitiator()/isOpen()/getRemoteAddress()/getExecutorService()/getRemotePort()/close()`（继承 `Closeable`）。
- `app/Initiator.java`：仅 `sendAndReceive(Message)` + `setTimeout(long, TimeUnit)` + 已过时的 `setTimeoutMillis(int)`。

### 2.2 "Client.java 去哪了"考据结论

**结论：在 hapifhir/hapi-hl7v2 的 GitHub 仓库中，`ca.uhn.hl7v2.app.Client` 从首个 git commit 起就不存在；没有任何提交删除过它；当前 master 全树也搜不到任何 `Client.java`。** 网络上"早期有个 app.Client 类"的印象无法在本仓库证实——若它真存在过，只可能存在于 2007 年之前的 SourceForge CVS 时代（未迁移进 git），**这一点未能验证，不能断言**。

证据链（一手）：

1. **当前 master 全树搜索**：经 `https://api.github.com/repos/hapifhir/hapi-hl7v2/git/trees/master?recursive=1` 获取全量路径，grep `Client.java` **零命中**；唯一带 "Client" 字样的主代码类是 `protocol/impl/ClientSocketStreamSource.java`（protocol 包旧基础设施，与 app 客户端无关）和 hapi-hl7overhttp 的 HoH 客户端类（§6.5）。
2. **首个 git commit 即无此类**：仓库第一个提交 `a51b1bc`（2007-02-19，"Initial commit of mavenized build"）的完整树中，`app/` 包已有 `ConnectionHub.java`、`Initiator.java`、`ActiveConnection.java`、`Receiver.java`、`Responder.java`、`TwoPortService.java` 等 21 个类，**没有 `Client.java`**（该树反而有后来被移除的 `MessageTypeRouter.java`——证明"老类被移除"在 git 历史里是可查的，而 Client 从未出现）。
   - 获取方式：`https://api.github.com/repos/hapifhir/hapi-hl7v2/commits?until=2013-01-01T00:00:00Z&per_page=100&page=N` 翻页至 2007 年，再取 `https://api.github.com/repos/hapifhir/hapi-hl7v2/git/trees/a51b1bc3140b8bf8571380c613a6e1ae64fab7a7?recursive=1`。
   - 注意：GitHub commits API 不支持 `direction=asc`，直接 `per_page=1` 取到的是最新提交，考据时需按 `until` 翻页。
3. **deprecated 清单无记录**：官方 2.3 版 apidocs 的 deprecated-list（<https://hapifhir.github.io/hapi-hl7v2/base/apidocs/deprecated-list.html>）只列了 `ConnectionHub.getInstance()`（→ `HapiContext.getConnectionHub()`）、`Initiator.setTimeoutMillis()`（→ `setTimeout`）等，**没有任何 `app.Client` 的 deprecation/替代记录**。若 Client 是"被改名/删除的公共类"，按 HAPI 的惯例会留 deprecation 痕迹。
4. 间接旁证：`ConnectionHub` 的 javadoc 从早期就链接 "hl7api.sourceforge.net/xref/.../SendAndReceiveAMessage.html" 作为客户端示例（`ConnectionHub.java` 第 388、398、411、427 行），即"用 ConnectionHub + Initiator 发消息"这一惯用法 2007 年即定型。

**纠偏**：本任务启动前的预期是"早期版本有 app.Client、后来消失了"。源码考据结果是：**在可考的 18 年 git 历史内它从未存在过**；客户端入口自始就是 `ConnectionHub`/`Initiator` 体系。对 C++ 重写的启示是：客户端不需要一个胖 Client 类，"连接池（hub）+ 连接（connection）+ 发起器（initiator）"三层正交拆分即可。

---

## 3. 发送-等待主流程详解

### 3.1 `Initiator.sendAndReceive(...)` 的语义

出处：`hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveInitiator.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveInitiator.java>

类 javadoc 原文要点（第 51-68 行）：*"Performs the initiation role of a message exchange (i.e. sender of the first message; analogous to the client in a client-server interaction)... The `sendAndReceive(...)` method blocks until either a response is received with the matching message ID, or until a timeout period has passed. The timeout defaults to 10000 ms (10 sec)... enhanced mode, two-phase reply, continuation messages, and batch processing are unsupported."*

方法体（第 111-154 行）逐步语义：

```java
public Message sendAndReceive(Message out) throws HL7Exception, LLPException, IOException {
    // 1) 空消息 → HL7Exception("Can't encode null message", REQUIRED_FIELD_MISSING)
    // 2) Terser 取 /MSH-10 作为 messID；为空 →
    //    HL7Exception("MSH segment missing required field Control ID (MSH-10)")
    String messID = new Terser(out).get("/MSH-10");
    // 3) 编码成 ER7 字符串
    String outbound = conn.getParser().encode(out);
    // 4) 【先登记、后发送】在 BlockingHashMap 上以 messID 注册一个带超时的 Future
    Future<String> inbound = conn.waitForResponse(messID, timeoutMillis);
    // 5) 写出 MLLP 帧（MinLLPWriter → MllpEncoder：0x0B+payload+0x1C 0x0D 单次 write+flush）
    conn.getSendWriter().writeMessage(outbound);
    // 6) 阻塞等应答
    if (inbound != null && (message = inbound.get()) != null) {
        return conn.getParser().parse(message);   // 解析成 Message 返回
    }
    // 7) IOException → inbound.cancel(true); conn.close(); 原样抛出
    //    InterruptedException|ExecutionException → 【被空 catch 吞掉】
    // 8) 走到这里（超时/被取消/被中断）统一抛：
    throw new TimeoutException("Timeout waiting for response to message with control ID "
                    + messID + " after " + timeoutMillis + " ms.");
}
```

要点解读：

- **同步阻塞**：调用线程一直阻塞到拿到匹配应答或超时；接口本身线程安全（javadoc：*"This method is thread-safe - multiple threads can share an Initiator... Responses are returned to the calling thread on the basis of message ID"*），多线程共用一个 Initiator 靠消息 ID 区分各自应答。
- **"先登记后发送"的时序**（第 134-135 行）：先 `waitForResponse` 在 map 里放下 latch，再 `writeMessage`。这样即使对端应答极快、在 write 返回前就到货，Receiver 线程也能命中 latch，不会漏匹配。
- **纠偏点**：`InterruptedException | ExecutionException` 被空 catch 吞掉后**落入同一 `TimeoutException`**（第 149-153 行）——也就是说线程被打断时调用方看到的是 TimeoutException 而不是 InterruptedException，C++ 移植时不要照抄这个含糊语义。
- **MSH-10 必填**：消息控制 ID 由调用方在发送前填好（业务侧通常用 `ParserConfiguration` 的 ID 生成器；`ActiveInitiator.main` 测试桩演示了 `InMemoryIDGenerator` + `Terser.set("/MSH-10", ID)`，第 196-215 行）。

### 3.2 应答关联：`concurrent/BlockingHashMap`，key = 出站 MSH-10 ↔ 入站 MSA-2

出处：
- `hapi-base/src/main/java/ca/uhn/hl7v2/concurrent/BlockingHashMap.java`
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/concurrent/BlockingHashMap.java>
- `hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveConnection.java`（第 248-259 行）
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveConnection.java>
- `hapi-base/src/main/java/ca/uhn/hl7v2/app/Receiver.java`（第 113-127 行）
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/Receiver.java>

机制：

1. 发送线程：`ActiveConnection.waitForResponse(messageID, timeout)` → `responses.asyncPoll(messageID, timeout, MILLISECONDS)`（`BlockingHashMap` 第 110-113 行）——`latchFor(key)` 为 key 建一个 `CountDownLatch(1)`，然后向线程池提交一个轮询任务：`latch.await(timeout)` 超时则返回 null。
2. 接收线程：`Receiver.handle()` 每轮 `in.getMessage()` 读一帧 → `processMessage(String)`：用 `conn.getParser().getAckID(message)`（`PipeParser.getAckID`，纯字符串速扫 `\rMSA` 取 MSA-2，**不整报文解析**）拿到 ackID：
   - MSA-2 为空 → 主动消息 → 交 `Responder` 走业务路由（与服务端同一条路径，见服务端报告 §3.2/§3.4）；
   - MSA-2 非空 → `conn.isRecipientWaiting(ackID, message)` → `responses.give(ackID, message)`（`BlockingHashMap` 第 81-87 行：仅当该 key 的 latch 存在才 put 并返回 true）；**无人等待则记日志丢弃**（`Receiver.java` 第 121-122 行："Unexpected Message Received... it will be ignored"）。

即关联规则一句话：**出站报文 MSH-10 必须等于对端应答的 MSA-2**（HL7 标准 ACK 规则，`AbstractMessage.fillResponseHeader()` 保证，见服务端报告 §3.5）。`give()` 的"仅当有人在等"判断天然过滤了迟到的重复应答。

### 3.3 超时与 socket 超时（两个独立机制，勿混淆）

**① 业务响应超时（等 ACK 的超时）**：
- 默认 **10000ms**，硬编码于 `ActiveInitiator.timeoutMillis = 10000`（第 80 行），构造时被系统属性 `ca.uhn.hl7v2.app.initiator.timeout`（long，毫秒）覆盖（第 91-102 行）；
- 运行期用 `setTimeout(long, TimeUnit)`（`Initiator` 接口，第 60 行；`ActiveInitiator` 第 172-174 行）；
- 超时即抛 `app/TimeoutException`——注意它**是 `HL7Exception` 的子类**（`TimeoutException.java` 第 34 行：`public class TimeoutException extends HL7Exception`），不是 `java.util.concurrent.TimeoutException`。

**② socket 读超时（线程可中断性）**：
- `hapi-base/src/main/java/ca/uhn/hl7v2/util/StandardSocketFactory.java`
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/util/StandardSocketFactory.java>
- 常量 `public static final int DEFAULT_ACCEPTED_SOCKET_TIMEOUT = 500;`（第 13 行）。**纠偏**：常量名带 "ACCEPTED" 且 javadoc 写着 "set on newly accepted sockets in server applications"，但 `createSocket()`/`createTlsSocket()`（第 37-54 行）**同样对它 setSoTimeout**——客户端 socket 也是 500ms 读超时。经 `ConnectionData` 构造函数（第 84-90 行：socketFactory 为 null 时 `new StandardSocketFactory()`）和 `HapiContext.setSocketFactory`（`DefaultHapiContext.java` 第 119 行默认 `StandardSocketFactory`）落到每个客户端连接。
- 500ms 到点 `MllpDecoder` 捕获 `SocketTimeoutException`：缓冲为空返回 null（Receiver 记 debug 再读一轮，使读循环可停止/可超时），非空则丢缓冲（见服务端报告 §3.1）。**它不参与 ACK 超时判定**——ACK 超时只由 ① 的 latch 等待决定。

---

## 4. 连接管理

### 4.1 `ConnectionHub.attach(...)` 的连接缓存语义

出处：`hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionHub.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionHub.java>

类 javadoc（第 49-61 行）：*"Provides access to shared HL7 Connections. The ConnectionHub has at most one connection to any given address at any time"*；多线程安全靠**按目标地址的互斥锁**：两个线程连不同目标互不阻塞，连同一目标则一个等另一个（系统属性 `ConnectionHub.class.getName() + ".maxSize"` 可调互斥锁表上限，默认 1000，第 74 行）。

**缓存 key 与容器**：
- 存储是内部类 `CountingMap<ConnectionData, Connection>`（第 452-549 行）：`ConcurrentHashMap` + **引用计数**；`put(key)` 已存在则计数 +1，否则 `open(key)` 新建；`remove`（对应 `detach`）计数 -1 到 0 才 `dispose`（=`connection.close()`）；`removeAllOf`（对应 `discard`）立即关闭剔除。
- key 是 `app/ConnectionData.java`（包私有）：
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionData.java>
  - `equals/hashCode` 参与字段（第 134-181 行）：**host、port、port2、parser 的类、llp 的类**；
  - **注意：`tls` 标志、`socketFactory`、`lazy` 不参与 equals**——同一个 host/port 先 attach(tls=false) 再 attach(tls=true)，第二条会复用第一条的明文连接（要 TLS 必须换 HapiContext 或用带显式 SocketFactory/不同 parser 类加以区分）。C++ 重写时这是值得修正的坑。
  - `toString()` = `host:port[,port2][(s)]`，同时作为互斥锁表的 key。

**attach 主流程**（第 112-129 行）：

```java
connectionMutexes.putIfAbsent(data.toString(), data.toString());
String mutex = connectionMutexes.get(data.toString());
synchronized (mutex) {
    discardConnectionIfStale(connections.get(data));   // 缓存连接已死 → 丢弃
    conn = connections.put(data);                      // 复用计数+1 或新建
}
// 异常统一包成 HL7Exception("Cannot open connection to host:port/port2", e)
```

**`HapiContext.newClient(...)` 就是 `attach` 的薄封装**：`DefaultHapiContext.java` 第 334-348 行（`newClient`→`hub.attach(this,...)`，`newLazyClient`→`hub.attachLazily(this,...)`）。`HapiContext` 接口 javadoc 明确："Note that connections are pooled by the HapiContext by default"（第 354 行）、"If a connection to this server already exists, it is reused"（第 373 行）。示例 `SendAndReceiveAMessage`（§7）也演示了第二次 `newClient` 返回同一实例。

**生命周期 API**：
- `detach(Connection)`（第 342-346 行）：*"you are done with the given Connection - if no other code is using it, it will be closed"*——引用计数 -1；
- `discard(Connection)`（第 353-357 行）：*"to be used when there is a problem with a Connection, e.g. socket connection closed by remote host"*——强制关闭并剔除；
- `discardAll()`（第 362-366 行）：全部关闭；
- `getInstance()` 系列单例方法全部 `@deprecated`（第 390-418 行），推荐 `HapiContext.newClient`；`getNewInstance(context)` 可拿非单例 hub。

### 4.2 `LazyConnection` 的延迟连接

出处：`hapi-base/src/main/java/ca/uhn/hl7v2/app/LazyConnection.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/LazyConnection.java>

- 类 javadoc（第 38-43 行）：*"the actual socket connection is created lazily right before the first message is sent over the associated Initiator. Server-side connections are always non-lazy."*
- 构造时不建 socket；`getInitiator()` 在未建连时返回内部 `LazyInitiator`（第 61-64、108-135 行）；`LazyInitiator.sendAndReceive` 是 `synchronized`，首调时 `establishConnection()` → `ConnectionFactory.openEagerly(data, executor)`（新建 ActiveConnection 并 activate 收线程），然后把之前暂存的 timeout 透传给真正的 Initiator（第 117-123 行）。
- `close()` 只在已建连时关并置 null（第 66-71 行）；`isOpen()`/`getRemoteAddress()` 未建连时返回 false/null（`Connection` 接口 javadoc 特别注明 Lazy 时可能为 null）。
- 用途（示例 `LazilySendAMessage`，§7）：在目标服务器还没起来时就拿到 Connection/Initiator，首次发送时透明建连。

### 4.3 连接断开后的表现（逐项源码验证）

| 场景 | 行为 | 证据 |
|---|---|---|
| 对端正常/异常断开 | 该连接的 `Receiver` 读循环抛 `SocketException`/`IOException` → `conn.close()`（关 socket、停收线程、`open=false`），**只影响这一条连接** | `Receiver.java` 第 74-92 行 |
| 断线后**在同一 Connection 实例上**再发 | `sendWriter.writeMessage()` 抛 `IOException` → ActiveInitiator 取消等待 Future、`conn.close()`、**原样抛出 IOException** | `ActiveInitiator.java` 第 144-148 行 |
| 断线后**再次 `newClient()/attach()`** | `discardConnectionIfStale` 检测到 `!conn.isOpen()` → `discard` → 重建新连接 | `ConnectionHub.java` 第 119、368-374 行 |
| 发送途中 IO 失败 | 同上：关连接、抛 IOException；**不重发、不重连**（无重试，§5） | `ActiveInitiator.java` 第 144-148 行 |
| 连接状态通知 | **客户端侧没有连接监听器**。`ConnectionListener`（`app/ConnectionListener.java`，`connectionReceived/connectionDiscarded`）只能注册到 `HL7Service`（服务端）由 `ConnectionCleaner` 触发；客户端 Connection/ConnectionHub 均无回调钩子，断线只能以"下次调用抛异常"的方式感知 | `ConnectionListener.java` 全文；`HL7Service.java` 第 424-461 行（服务端报告 §3.6） |

对 C++ 的重要推论：liscomm 有 `liscomm_connection_status_handler` 连接状态回调，**HAPI 客户端没有等价物**——C++ 客户端要提供该回调，需在自有读线程里检测断开并主动上报。

### 4.4 `ConnectionFactory` 与 socket 创建

出处：`hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionFactory.java`（包私有）
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionFactory.java>

- `open()`（第 39-46 行）：`isLazy ? new LazyConnection(...) : openEagerly(...)`；`openEagerly()` 建 `ActiveConnection` 后立即 `activate()` 启动收线程（第 48-67 行）。
- `createSocket()`（第 70-79 行）：`tls ? socketFactory.createTlsSocket() : socketFactory.createSocket()`，然后 `socket.connect(new InetSocketAddress(host, port))`（**阻塞 connect，无独立 connect 超时设置**；连接超时只能靠 OS TCP 行为）。socket 参数（keepAlive、tcpNoDelay、soTimeout=500）在 `StandardSocketFactory` 里设置（见 §3.3、§6.1）。

---

## 5. 重试与可靠性：无内建重试（证据）

**结论：`Initiator.sendAndReceive` 是一次性原子动作——发送失败/超时即抛异常，不重试、不重连、不排队；重试策略完全由业务层实现。**

证据（全部一手，2026-10-22 于 master 验证）：

1. **关键字排查零命中**：对本次下载的全部客户端相关源码 `Initiator.java`、`ActiveInitiator.java`、`ConnectionHub.java`、`LazyConnection.java`、`ConnectionFactory.java`、`ConnectionData.java`、`Connection.java`、`ActiveConnection.java`、`Receiver.java`、`TimeoutException.java`、`BlockingHashMap.java`、`HapiContext.java`、`DefaultHapiContext.java`、`SocketFactory.java`、`StandardSocketFactory.java` 执行 `grep -in "retry|retries"`：**0 条命中**。唯一的超时处理就是 `ActiveInitiator` 末尾抛 `TimeoutException`。
2. **`ActiveInitiator.sendAndReceive` 的失败路径**（第 144-153 行）：IOException → cancel + close + 抛出；超时 → 抛 `TimeoutException`。**没有任何循环、回退或重新 attach**。
3. **`ConnectionHub` 只有缓存与重建，没有重发**：`discardConnectionIfStale` 的语义是"下次 attach 换新连接"，发生在**下一次**发送之前；本次失败的消息不会被自动重发。
4. **官方示例正是"业务层自己处理"的写法**：`hapi-examples/.../SendLotsOfMessages.java`（<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/SendLotsOfMessages.java>）第 55-70 行——`catch (IOException)` 后打印 "Didn't send out this message!"，`conn.close(); conn = null;`，**继续下一条**（失败的这条直接丢弃，连重发都没有）：

```java
try {
    Message next = iter.next();
    Message response = conn.getInitiator().sendAndReceive(next);
    System.out.println("Sent message. Response was " + response.encode());
} catch (IOException e) {
    System.out.println("Didn't send out this message!");
    e.printStackTrace();
    try { conn.close(); } catch (IOException ignored) {}
    conn = null;   // 下轮循环重新 newClient
}
```

**与 liscomm 对照**（`liscomm/liscomm.hpp` 第 91-92、196-197 行）：liscomm 把 `timeout_milliseconds=5000`、`retry_times=3` 做成通信参数内建进 `liscomm_sync_query_request`；HAPI 把超时做进 Initiator（默认 10s 可改）、把重试留空。C++ 客户端若要"对齐 liscomm 体验"，需要在 HAPI 式原子 `sendAndReceive` 之上自行封装：失败 → 关闭失效连接 → 重建连接 → 原消息重发，最多 N 次，每次独立计时。

---

## 6. TLS、字符集、异常体系、双端口、HL7 over HTTP 客户端

### 6.1 TLS 与安全：`util/SocketFactory` 体系

出处：
- `hapi-base/src/main/java/ca/uhn/hl7v2/util/SocketFactory.java`（接口）
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/util/SocketFactory.java>
- `hapi-base/src/main/java/ca/uhn/hl7v2/util/StandardSocketFactory.java`（默认实现）
  <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/util/StandardSocketFactory.java>

- 接口 5 个方法：`createSocket()` / `createTlsSocket()` / `createServerSocket()` / `createTlsServerSocket()` / `configureNewAcceptedSocket(Socket)`（最后一个仅供服务端 accept 后调参）。
- 客户端 TLS 配置路径：`newClient(..., tls=true)` → `ConnectionFactory.createSocket` 调 `socketFactory.createTlsSocket()`；`StandardSocketFactory.createTlsSocket()` = `javax.net.ssl.SSLSocketFactory.getDefault().createSocket()` + keepAlive/tcpNoDelay/soTimeout——**即 JSSE 默认行为**：信任库用 JVM 默认 truststore（`javax.net.ssl.trustStore`），服务端认证默认开启、客户端证书默认不送；要自定义证书/密钥库必须换工厂实现。
- 换工厂的入口：`HapiContext.setSocketFactory(...)`（示例 `UsingTls.java`，§7）。常用搭配是复用 hapi-hl7overhttp 模块的 `hoh/sockets/CustomCertificateTlsSocketFactory`（可配 keystore 文件+口令，实现 hapi 的 `util/SocketFactory` 适配器 `hoh/util/HapiSocketTlsFactoryWrapper`），见 `hapi-examples/.../UsingTls.java` 第 56-64 行。
- 证书语义简述（与 liscomm 对照）：liscomm 的 `LISCOMM_TLS_PARAM{client_cert_file, client_key_file, ca_cert_file}` 是"PEM 文件 triple"模型；HAPI/JSSE 是"JKS/PKCS12 keystore + truststore + 口令"模型，证书不出现在客户端 API 里，全在工厂层。C++ 侧若用 OpenSSL，PEM triple 更贴近 liscomm 现状。

### 6.2 字符集与 MSH-18

- 出站编码：消息先由 Parser 编码成字符串，再经 `MinLLPWriter`（`llp/MinLLPWriter.java`，<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MinLLPWriter.java>）用 **LLP 层构造时绑定的 charset** 转字节（`MllpEncoder(charset, omitBOM)`）。charset 来源：`MinLowerLayerProtocol` 默认 US-ASCII；系统属性 `ca.uhn.hl7v2.llp.charset` 全局覆盖（设 `default` 用平台默认）；`llp.setCharset(...)` 单实例设置。**发送侧不读 MSH-18**。
- 入站解码：只有 `new MinLowerLayerProtocol(true)`（respectMSH18=true）才用 `ExtendedMinLLPReader` 扫 MSH-18 按 HL7 Table 0211 选解码字符集；而 `DefaultHapiContext` 两个构造函数都写死 `new MinLowerLayerProtocol(false)`（`DefaultHapiContext.java` 第 118、132 行），**默认根本不认 MSH-18**，双向固定配置字符集。
- 对 C++：客户端发送前按本地配置字符集编码即可，MSH-18 协商不是必做项；若仪器侧需要中文（GB18030/GBK），照 `HandlingCharacterEncodings` 示例设固定 charset 即可（示例见服务端报告 §3.8）。

### 6.3 异常体系

| 场景 | 抛出 | 出处 |
|---|---|---|
| 等应答超时 | `ca.uhn.hl7v2.app.TimeoutException`（**extends HL7Exception**） | `app/TimeoutException.java` 第 34 行；`ActiveInitiator.java` 第 152-153 行 |
| 消息为 null / MSH-10 为空 | `HL7Exception`（`ErrorCode.REQUIRED_FIELD_MISSING`） | `ActiveInitiator.java` 第 113-126 行 |
| 编码失败 | `HL7Exception`（Parser.encode 抛出） | `ActiveInitiator.java` 第 129 行 |
| MLLP 协议错误（帧字节违规） | `ca.uhn.hl7v2.llp.LLPException`（收侧表现为关闭该连接） | `llp/MllpDecoderState.java`；`Receiver.java` 第 74-81 行 |
| socket 连接/读写失败 | `java.io.IOException`（connect 失败被 `ConnectionHub.attach` 包成 `HL7Exception("Cannot open connection to ...")`） | `ConnectionFactory.java` 第 77 行；`ConnectionHub.java` 第 124-128 行；`ActiveInitiator.java` 第 144-148 行 |
| 线程中断 | **不单独抛**，被吞后落入 `TimeoutException`（见 §3.1 纠偏） | `ActiveInitiator.java` 第 149-153 行 |

- `HL7Exception extends AbstractHL7Exception`（`hapi-base/.../HL7Exception.java` 第 38 行）；`AbstractHL7Exception` 带 `ErrorCode`（HL7 表 0357）并可 `populateResponse` 填 ERR 段（服务端报告 §3.5）。
- **重要设计事实**：对端回的 NAK（MSA-1=AE/AR）**不是异常**，`sendAndReceive` 正常返回该 ACK Message，业务层必须自己读 MSA-1/MSA-3 判断（`ActiveInitiator.main` 测试桩示范了取 MSA-2 比对，第 211-220 行）。C++ 封装时建议把"传输成功 + 业务 NAK"翻译成 liscomm 式 `LISCOMM_MSG_ACK{ack_code, err_code}`。

### 6.4 双端口/双 socket 客户端

- 工厂：`HapiContext.newClient(host, outboundPort, inboundPort, tls)` / `newLazyClient(...)` / `ConnectionHub.attach(host, outboundPort, inboundPort, ...)`。
- 语义（`ConnectionFactory.openEagerly`，第 51-64 行 + `ActiveConnection` 双 socket 构造函数，第 119-133 行）：向**同一主机**建两条 socket——`outbound` 接 server's inbound 端口（`sendWriter` 用它写、即"发"），`inbound` 接 server's outbound 端口（`ackWriter` 用它写、即"收"与"回"）；两条 socket 各挂一个 `Receiver` 读线程。`getRemoteAddress()/getRemotePort()` 返回 **outbound** socket 的对端地址（第 177-190 行注释：*"always add outbound first ... see getRemoteAddress()"*）。
- 对应 `TwoPortService`（服务端双端口，服务端报告 §3.2）：服务端 inbound 端口收客户端命令、outbound 端口向客户端推消息；客户端双 socket 模式正是它的对端形态——与 liscomm 的单链路不同，C++ 侧若兼容双端口 LIS，传输层要支持"一对 socket"。

### 6.5 HL7 over HTTP 客户端（hapi-hl7overhttp / HoH）

模块 `hapi-hl7overhttp`（包 `ca.uhn.hl7v2.hoh`），客户端类（均在 `hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/` 下）：

- **raw 层**：`raw/client/AbstractRawClient.java`（<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/raw/client/AbstractRawClient.java>）——HTTP POST + `encoder/Hl7OverHttpRequestEncoder` 编码、`Hl7OverHttpResponseDecoder` 解码；关键常量：`DEFAULT_CHARSET = UTF-8`、`DEFAULT_CONNECTION_TIMEOUT = 10000`、`DEFAULT_RESPONSE_TIMEOUT = 60000`、socket soTimeout 默认 5000（第 44-66 行）。`sendAndReceive(ISendable)` 是 `synchronized`，connect 失败抛 IOException、读应答失败抛 `DecodeException`、编码失败抛 `EncodeException`，内部循环重试读直到 responseTimeout（第 150-185、216-245 行）。实现类：`HohRawClientSimple`（单 socket、单实例同时只发一条，`AutoClose` 默认 true，socket 失效下次自动重连，`provideSocket()` 第 96-101 行）、`HohRawClientMultithreaded`（socket 池）。
- **hapi 层**：`hapi/client/HohClientSimple.java`——包装 raw 层 + Parser，`sendAndReceiveMessage(...)` 直接收发 `ca.uhn.hl7v2.model.Message`；`HohClientMultithreaded` 对应多线程版。接口 `api/IClient、IClientSimple、IClientMultithreaded`。
- **安全**：`hoh/sockets/TlsSocketFactory.java`（`SSLSocketFactory.getDefault()`，实现 hoh 自己的 `ISocketFactory`，与 hapi 的 `util/SocketFactory` 是两套接口）；HTTP Basic 客户端回调 `api/IAuthorizationClientCallback` + `auth/SingleCredentialClientCallback(user, pass)`；CMS 数字签名 `sign/`。
- 与本项目关系：LIS 主链路走 MLLP/TCP 即可；HoH 客户端再次印证"传输层可插拔"（HAPI 内核只认 Message/Parser）。C++ 侧若将来要 HTTP 上行通道，照此分层。

---

## 7. 示例代码摘录（hapi-examples，文件名均经目录 API 核实）

客户端相关示例目录：<https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples>

1. **`SendAndReceiveAMessage.java`**（核心客户端示例，核实存在）
   <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/SendAndReceiveAMessage.java>

   ```java
   // A connection object represents a socket attached to an HL7 server
   Connection connection = context.newClient("localhost", port, useTls);
   // The initiator is used to transmit unsolicited messages
   Initiator initiator = connection.getInitiator();
   Message response = initiator.sendAndReceive(adt);
   String responseString = p.encode(response);
   // 期望收到：MSH|^~\&|...||ACK|...  MSA|AA|12345

   // 再次 newClient 同一地址 → 返回同一条（池化）连接实例
   connection = context.newClient("localhost", port, useTls);
   ...
   connection.close();   // 用完再关；注释明确建议"持续发送的系统不要早关连接"
   ```

2. **`LazilySendAMessage.java`**——`context.newLazyClient("localhost", port, useTls)` 在服务器启动**之前**就拿到连接，首次 `sendAndReceive` 时透明建连（印证 §4.2）。
   <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/LazilySendAMessage.java>

3. **`SendLotsOfMessages.java`**——长连接复用 + 失败处理范式（§5 已引）：连接复用到出错为止，`IOException` → close → 置 null → 下条重建。
   <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/SendLotsOfMessages.java>

4. **`UsingTls.java`**——`ctx.newClient(host, port, true)`；以及 `CustomCertificateTlsSocketFactory`（来自 hapi-hl7overhttp 模块）+ `HapiSocketTlsFactoryWrapper` 适配器 + `ctx.setSocketFactory(...)` 的自定义证书用法（§6.1 已引）。
   <https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/UsingTls.java>

5. **HoH 客户端示例**（`examples/hoh/` 包）：`HohClientSimpleExample.java`（`new HohClientSimple(host, port, uri, parser)` + `setSocketFactory(new TlsSocketFactory())` + `SingleCredentialClientCallback` + `sendAndReceiveMessage`）、`CustomCertificateClient.java`、`SignatureClient.java`。
   <https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/hoh>

**官方文档的说法**（任务第 10 项核实）：README 无 client 专章，只链接外部教程；官方站点 <https://hapifhir.github.io/hapi-hl7v2/> 的 "Sending and receiving" 章节（`src/site/apt/devbyexample.apt`，第 46-69 行）直接指向上述示例 xref（SendAndReceiveAMessage / SendLotsOfMessages / HandlingCharacterEncodings / UsingTls / CustomThreading）——**官方文档的客户端"规范"就是这些示例代码本身**，一切以源码为准。

---

## 8. 对 C++ 客户端的组件映射建议（对照 liscomm.hpp）

HAPI 客户端体系与 `liscomm/liscomm.hpp` 的语义对照：

| liscomm 概念（hpp 行号） | HAPI 对应 | C++ 客户端建议 |
|---|---|---|
| `liscomm_init` + 4 个回调（log/report_status/connection_status/msg_data，第 64-87 行） | **无对应物**：HAPI 只有 slf4j 日志与异常，客户端侧无连接状态/收发数据/上报状态回调 | 封装层自建回调体系。收发数据回调可挂在帧编解码出口；**连接状态回调需在读线程检测断开时主动触发**（HAPI 无此机制，见 §4.3） |
| `liscomm_sync_query_request`（同步查询，注释第 497-509 行） | `Initiator.sendAndReceive` 阻塞语义 + `BlockingHashMap` 应答关联 | 一个 `SyncClient::request(msg, timeout)`：发前登记 `control_id → promise`，写 MLLP 帧，future 等待；**关联 key 直接用 MSH-10/MSA-2** |
| `timeout_milliseconds=5000`（第 91、196 行） | `Initiator.setTimeout`，默认 10000ms | 封装参数透传；区分两层超时：socket 读超时（建议照 HAPI 用 500ms 做可中断循环）与**业务应答超时**（liscomm 语义 5s） |
| `retry_times=3`（第 92、197 行） | **无**（§5） | 自行实现：失败 → discard 失效连接 → 重建 → 原消息重发，重试 N 次各次独立计时；注意**重试消息应换新的 MSH-10 还是沿用**需按仪器协议约定（HAPI 不管，消息 ID 由调用方生成） |
| `liscomm_async_report_result`（异步上报 + `msg_unique_id` 状态回调，第 512-522 行） | 底层同为 `sendAndReceive`（多线程共享 Initiator 按消息 ID 归位）；`msg_unique_id` 对应 MSH-10 | 上报线程池 + 每消息一个 pending 项；完成（或超时/失败）后按 `msg_unique_id` 回调 `MSG_SUCCESS/MSG_FAIL`。HAPI 的 `give()`"无人等待即丢弃"语义提示：pending 表必须先登记再发送（§3.1 时序） |
| `LISCOMM_MSG_ACK{AA/AE/AR}`（第 245-258 行） | NAK 不抛异常，正常返回 ACK Message（§6.3） | 传输层成功 ≠ 业务成功：收到应答后解析 MSA-1 翻译成 `LISCOMM_ACK_CODE`，ERR/MSA-3 映射 `err_code` |
| `connection_mode`：网口/串口/文件（第 110-117 行） | **HAPI 只有 TCP/TLS**（+ HoH 的 HTTP）；串口/文件模式无对应 | 传输层抽象 `ITransport`（connect/read/write/close），MLLP 编解码之上挂 socket/串口/文件三种实现；这正是 HAPI "LLP 与传输分离"思路的扩展 |
| `liscomm_tls_param` PEM triple（第 169-174 行） | `SocketFactory` + JSSE keystore/truststore（§6.1） | OpenSSL 实现 PEM triple 更贴近 liscomm 现状；把 TLS 收敛在 `ITransport` 工厂内 |
| `liscomm_get_connection_status`（第 544-553 行） | 无客户端侧状态回调；`Connection.isOpen()` 可查 | 维护客户端连接状态机（未连/已连/断开重连中），供查询与回调 |
| 双端口（HAPI `newClient` 双端口变体） | 双 socket：outbound 发、inbound 收（§6.4） | 若目标 LIS 为 TwoPortService 风格，`ITransport` 需支持 socket 对；迈克仪器链路如为单端口则单 socket 即可 |

**HAPI 与 liscomm 的设计差异总结**：
1. **重试**：liscomm 内建（3 次）/ HAPI 无（业务层做）；
2. **超时**：liscomm 单一消息超时（5s）/ HAPI 分两层（socket 读超时 500ms 保活循环 + 应答超时 10s 默认）；
3. **连接语义**：liscomm 由库自持连接并回调状态 / HAPI 连接池化（每目标一条、引用计数、stale 自动重建）但**无状态通知**，断线=下次调用抛异常；
4. **应答关联**：两者都以消息控制 ID 为 key（liscomm 的 `msg_unique_id` ↔ HAPI 的 MSH-10/MSA-2），都支持异步多消息在途；
5. **ACK 处理**：liscomm 直接给 `LISCOMM_MSG_ACK` 结构 / HAPI 返回原始 ACK 报文由业务解析。

**C++ sendAndReceive 落地清单**（借鉴 HAPI 源码）：发前用 Terser 式路径取值生成/校验 MSH-10 → pending map（`control_id → {promise, deadline}`）先登记 → MllpCodec 整帧单 write+flush → 读线程解析 MSA-2 投递 pending → 等待方按 deadline 唤醒，超时即 `TimeoutException` 等价物；IO 错误关闭连接并通知所有在途 pending 失败；对外暴露 `setTimeout`、可选 lazy connect、每端点单连接复用。

---

## 9. 参考链接清单

### 一手源码（hapifhir/hapi-hl7v2，master，HEAD `de15036` 2026-03-26）

- 客户端主流程
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveInitiator.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveInitiator.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/Initiator.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/Initiator.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/TimeoutException.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/TimeoutException.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/concurrent/BlockingHashMap.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/concurrent/BlockingHashMap.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/Receiver.java`（MSA-2 分发）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/Receiver.java>
- 连接管理
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionHub.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionHub.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionData.java`（缓存 key 的 equals）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionData.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/LazyConnection.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/LazyConnection.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionFactory.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ConnectionFactory.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveConnection.java`（waitForResponse/give、双 socket）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveConnection.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/app/Connection.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/Connection.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/HapiContext.java`（newClient 等工厂）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/HapiContext.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/DefaultHapiContext.java`（第 334-348 行委托、118/132 行默认 LLP）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/DefaultHapiContext.java>
- TLS / socket
  - `hapi-base/src/main/java/ca/uhn/hl7v2/util/SocketFactory.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/util/SocketFactory.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/util/StandardSocketFactory.java`（DEFAULT_ACCEPTED_SOCKET_TIMEOUT=500）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/util/StandardSocketFactory.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/llp/MinLowerLayerProtocol.java`（respectMSH18 开关）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MinLowerLayerProtocol.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/llp/MinLLPWriter.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MinLLPWriter.java>
  - `hapi-base/src/main/java/ca/uhn/hl7v2/HL7Exception.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/HL7Exception.java>
- HoH 客户端
  - `hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/raw/client/AbstractRawClient.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/raw/client/AbstractRawClient.java>
  - `.../hoh/raw/client/HohRawClientSimple.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/raw/client/HohRawClientSimple.java>
  - `.../hoh/hapi/client/HohClientSimple.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/hapi/client/HohClientSimple.java>
  - `.../hoh/sockets/TlsSocketFactory.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/sockets/TlsSocketFactory.java>
- 示例
  - `hapi-examples/src/main/java/ca/uhn/hl7v2/examples/SendAndReceiveAMessage.java`：<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples/SendAndReceiveAMessage.java>
  - 同目录 `LazilySendAMessage.java`、`SendLotsOfMessages.java`、`UsingTls.java`、`hoh/HohClientSimpleExample.java`：<https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples>
- 考据用 API
  - master 全量树：<https://api.github.com/repos/hapifhir/hapi-hl7v2/git/trees/master?recursive=1>
  - 首个 commit `a51b1bc`（2007-02-19）树：<https://api.github.com/repos/hapifhir/hapi-hl7v2/git/trees/a51b1bc3140b8bf8571380c613a6e1ae64fab7a7?recursive=1>
  - 2.3 apidocs deprecated-list：<https://hapifhir.github.io/hapi-hl7v2/base/apidocs/deprecated-list.html>
  - 官方文档 "Sending and receiving"（`src/site/apt/devbyexample.apt`）：<https://github.com/hapifhir/hapi-hl7v2/blob/master/src/site/apt/devbyexample.apt>；站点 <https://hapifhir.github.io/hapi-hl7v2/>

### 本仓库对照锚点

- `liscomm/liscomm.hpp`：`DEFAULT_TIMEOUT_MILLISECONDS 5000`、`DEFAULT_RETRY_TIMES 3`（第 91-92 行）、`liscomm_sync_query_request`（第 509 行）、`liscomm_async_report_result`（第 522 行）、`liscomm_connection_status_handler`（第 79 行）、`LISCOMM_TLS_PARAM`（第 169-174 行）、`LISCOMM_CONNECTION_MODE` 网口/串口/文件（第 110-117 行）
- `docs/hapi-hl7-server-research.md`：服务端调研（MLLP 编解码、ACK 生成、路由、线程模型）

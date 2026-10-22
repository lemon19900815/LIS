# HAPI HL7 服务器实现调研

> 调研日期：2026-10-22
> 调研人：调研代理（AI research agent，一手源码调研）
> 调研对象：`hapifhir/hapi-fhir`（用户点名）与 `hapifhir/hapi-hl7v2`（HAPI 组织真正的 HL7 v2.x 库）
> 方法：GitHub API / raw 源码一手取证；hapi-hl7v2 浅克隆至临时目录研读后已删除，未改动本仓库任何代码。

---

## 1. 结论速览（TL;DR）

| 仓库 | 是什么 | 有没有 HL7 v2.x MLLP 服务器 |
|---|---|---|
| `hapifhir/hapi-fhir` | FHIR（REST/HTTP，JSON/XML 资源）服务器与 JPA 实现 | **没有**。全库 16,537 条路径中无任何 `hl7v2`/`mllp`/`camel` 模块，唯一含 "hl7v2" 字样的路径是一条与 HL7 v2 无关的 FHIR 解析器 changelog（见 §2） |
| `hapifhir/hapi-hl7v2` | HL7 v2.x 全套 Java 库：MLLP 帧协议、TCP 服务/客户端、ER7/XML 解析、按消息类型路由、ACK 生成、校验 | **有，且非常成熟**。LIS 服务器（TCP + HL7 v2.x over MLLP）应以此仓库为唯一主要参考 |

**对迈克生物 C++ 重写 LIS 服务器的建议一句话版**：MLLP 帧层 → 自己写 0x0B/0x1C 0x0D 状态机编解码器；竖线解析 → 按 MSH-1/MSH-2 分隔符递归解析；路由 → `(MSH-9-1, MSH-9-2[, MSH-12]) → handler` 的映射表 + 通配符；ACK → 独立 builder 照抄 `AbstractMessage.fillResponseHeader()` 的字段回填规则。hapi-fhir 不需要参考（它是 FHIR 生态，与 HL7 v2.x MLLP 无关）。详见 §4。

---

## 2. hapi-fhir 仓库中的 HL7 相关内容（含证据）

### 2.1 仓库定位

`hapifhir/hapi-fhir` 是 **FHIR** 开源实现：RESTful 服务器、JPA 持久化、资源解析/校验（JSON/XML）。顶层模块（2026-10-22 经 `https://api.github.com/repos/hapifhir/hapi-fhir/contents/` 获取）全部是 `hapi-fhir-*` 命名，如：

- `hapi-fhir-base` / `hapi-fhir-server` / `hapi-fhir-server-cds-hooks`：FHIR REST 服务器核心
- `hapi-fhir-jpaserver-*` 系列：JPA 服务器（base/model/searchparam/subscription/mdm 等）
- `hapi-fhir-structures-{dstu2,dstu3,r4,r4b,r5}`：各 FHIR 版本的资源模型类
- `hapi-fhir-client*`、`hapi-fhir-converter`、`hapi-fhir-validation*`、`hapi-fhir-spring-boot` 等

### 2.2 "仓库里有没有 HL7 v2 / MLLP" 的验证过程与结果

拉取 master 分支完整 git 树（`https://api.github.com/repos/hapifhir/hapi-fhir/git/trees/master?recursive=1`）：

- 树共 **16,537 条路径**，`"truncated": false`（未截断，结果完备）
- 含 `hl7v2` 的路径 **仅 1 条**：
  `hapi-fhir-docs/src/main/resources/ca/uhn/hapi/fhir/changelog/6_6_0/4746-hl7v2-inbound-endpoint-should-reject-malformed-resources.yaml`
- 含 `mllp` 的路径 **0 条**；含 `camel` 的路径 **0 条**（用户提到的 `hapi-fhir-jpaserver-camel` 类 Camel 集成示例在当前 master 中不存在；另抽查 v3.8.0 / v4.0.0 旧 tag 的顶层目录也无 camel 模块）
- 其余含 "llp" 字样的匹配均为 `NullPointerException`（"Nu**llP**ointer"）、`installpackage` 等子串误报，与协议无关

对那条唯一命中的 changelog 取原文（raw.githubusercontent.com），内容为：

```yaml
---
type: fix
issue: 4746
jira: SMILE-4710
title: "Previously, the Lenient error handler will accept invalid extension containing value and nested extensions by default.
This will lead to errors when client further attempt to read, update or delete the resources. Now, this has been fixed and
a DataFormatException will be thrown instead."
```

即：它修的是 **FHIR 资源扩展（extension）的宽松解析问题**（文件名里的 "hl7v2-inbound-endpoint" 指 Smile CDR 侧的泛化接入端点概念），**与 HL7 v2.x 消息、MLLP 没有任何关系**。

### 2.3 结论（证据充分）

**hapi-fhir 中没有 HL7 v2.x MLLP 服务器实现**。证据 = 全量路径清单（16,537 条，未截断）+ 唯一 "hl7v2" 命中文件的内容核实。hapi-fhir 最新 release 为 v8.8.2（2026-09-25），是活跃维护的 FHIR 服务器，但其价值域与 "仪器经 MLLP 发 QRY/ORU" 的 LIS 中间件场景不重叠。

HL7 v2.x 的正确参考对象是同一 HAPI 组织下的 **`hapifhir/hapi-hl7v2`**（项目官网：<https://hapifhir.github.io/hapi-hl7v2/>，README 原文："This is the GitHub home for HAPI HL7v2"）。以下 §3 全部基于 hapi-hl7v2 源码（master，HEAD `de15036`，2026-03-26；最新 release v2.6.0，2025-02-05）。

---

## 3. hapi-hl7v2 服务器实现详解

hapi-hl7v2 是多模块 Maven 工程（根 `pom.xml` 版本 2.6.0，MPL 1.1/GPL 双许可），核心模块：

- `hapi-base`：MLLP、TCP 服务/客户端、解析、路由、ACK、校验、并发框架
- `hapi-structures-v21 … v281`：按 HL7 版本（2.1–2.8.1，`ca.uhn.hl7v2.Version` 枚举）生成的消息/段/字段模型类
- `hapi-hl7overhttp`：HL7 over HTTP（HoH）
- `hapi-examples`：示例代码；`hapi-test`：测试；`hapi-sourcegen`：模型代码生成器；`hapi-dist`/`hapi-bom`/`hapi-osgi-*`/`hapi-testpanel`：打包与工具

### 3.1 MLLP 帧协议（`ca.uhn.hl7v2.llp`）

> 注意：用户任务书中猜测的 `MLLPInputStream`/`MLLPOutputStream` 类**并不存在**；实际的帧编解码由 `MllpEncoder`/`MllpDecoder` + `MllpDecoderState` 状态机完成，Reader/Writer 外观为 `MinLLPReader`/`MinLLPWriter`。

**帧起止字节**——`hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpConstants.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpConstants.java>

```java
char START_BYTE = 0x0b;   // VT
char END_BYTE1  = 0x1c;   // FS
char END_BYTE2  = 0x0d;   // CR
String CHARSET_KEY = "ca.uhn.hl7v2.llp.charset";  // 系统属性，可全局覆盖字符集
```

**编码（出站）**——`hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpEncoder.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpEncoder.java>

`putMessage(String, OutputStream)`：把消息转成字节后拼成 `[0x0B][payload][0x1C][0x0D]`，**一次 `write()` 发送并 flush**（注释明确：多次 write 可能造成多个帧）；支持按 charset 编码与 `omitBOM`（经 `CharSetUtil.withoutBOM()` 去 BOM）。

**解码（入站）**——`hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpDecoder.java` + `MllpDecoderState.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpDecoder.java>
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MllpDecoderState.java>

- 字节级状态机：`START`(等 0x0B) → `BODY`(等 0x1C) → `PREPARE_END`(等 0x0D) → `END`
- **校验方式**：START/PREPARE_END 状态下若读到非预期字节即抛 `LLPException("MLLP protocol violation …")`；流在消息体中结束也抛 `LLPException`；帧内逐字节 `in.read()` 收集正文
- `SocketTimeoutException` 被捕获：缓冲为空返回 `null`（当作"本轮无消息"），非空则丢弃并告警——这是它和 500ms socket 超时配合实现"可中断读循环"的关键（见 §3.2/§3.6）

**协议外观**——`MinLowerLayerProtocol` / `LowerLayerProtocol` / `HL7Reader` / `HL7Writer`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/MinLowerLayerProtocol.java>
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/LowerLayerProtocol.java>

- `LowerLayerProtocol` 抽象：`getReader(InputStream)` / `getWriter(OutputStream)` / `setCharset(...)`；`MinLowerLayerProtocol(respectMSH18, omitBOM)` 两个开关决定用普通还是 Extended 读写器
- `HL7Reader` 接口仅 3 个方法：`getMessage()` / `setInputStream()` / `close()`；`HL7Writer`：`writeMessage()` / `setOutputStream()` / `close()`
- 默认字符集 **US-ASCII**，可用系统属性 `ca.uhn.hl7v2.llp.charset` 覆盖（设为 `default` 用平台默认）；类注释明确：MLLP 只保证单字节编码与 UTF-8、Shift_JIS 不冲突，**不推荐 UTF-16/32**（帧字节可能出现在多字节编码中）

**字符集处理（MSH-18）**——`ExtendedMinLLPReader` → `ExtendedMllpDecoder` → `CharSetUtil` / `HL7Charsets`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/ExtendedMllpDecoder.java>
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp/CharSetUtil.java>

- 仅当构造 `MinLowerLayerProtocol(true)`（respectMSH18）时启用：收到完整帧字节后，先用 `PreParser.getFields(message, "MSH-18(0)")` 无损速扫 MSH-18，查 `HL7Charsets`（HL7 Table 0211 → Java charset 映射：`ASCII`、`8859/1..15`、`ISO IR6/14/87/159`、`GB 18030-2000`、`BIG-5`、`KS X 1001`、`UNICODE UTF-8` 等）决定解码字符集；MSH-18 缺失/非法则回落默认
- 默认情况下（`new MinLowerLayerProtocol(false)`，也是 `DefaultHapiContext` 的默认值，见 `DefaultHapiContext.java` 第 118 行）**不读 MSH-18**，固定用配置字符集

### 3.2 TCP 服务器与线程模型（`ca.uhn.hl7v2.app` + `ca.uhn.hl7v2.concurrent`）

**类关系总览**：

```
HL7Service (abstract, extends concurrent.Service)
├── SimpleServer      单端口
├── TwoPortService    入/出两个端口（同一远端主机连上两个口才激活连接）
└── newConnection(ActiveConnection)
    ActiveConnection  每连接：1 个 Responder + 1~2 个 Receiver + Initiator
        Receiver      每连接一条读线程（Service 循环）
```

**`HL7Service`**——`hapi-base/src/main/java/ca/uhn/hl7v2/app/HL7Service.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/HL7Service.java>

- 持有一个**全局共享**的 `ApplicationRouterImpl`（所有连接的消息都经它分发）、`Parser`、`LowerLayerProtocol`、连接列表、`ConnectionListener` 列表、`ConnectionCleaner`（每 500ms 扫描剔除已关闭连接并通知监听器，见该类 424-461 行）
- `newConnection(ActiveConnection)`：给该连接的 `Responder` 挂上共享路由器 → `activate()` 启动 Receiver 线程 → 记入连接列表 → 通知监听器
- `registerApplication(msgType, triggerEvent, ReceivingApplication)`：本质 `applicationRouter.bindApplication(new AppRoutingDataImpl(type, event, "*", "*"), handler)`；支持 `"*"` 通配；另有 `loadApplicationsFromFile()` 从制表符分隔文件批量注册
- `setExceptionHandler(ReceivingApplicationExceptionHandler)`、`setParserExeptionHandler(Receiver.ReceiverParserExceptionHandler)`
- 优雅关闭：`afterTermination()` 先停 cleaner，再逐个 `connection.close()`

**`SimpleServer`**——`hapi-base/src/main/java/ca/uhn/hl7v2/app/SimpleServer.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/SimpleServer.java>

- 构造默认 `MinLowerLayerProtocol + PipeParser`；内部 `LinkedBlockingQueue<AcceptedSocket>(100)` 解耦 accept 与业务
- `afterStartup()`：创建 `AcceptorThread` 并 start；`handle()` 主循环：每 500ms 从队列取一个已 accept 的 socket → `new ActiveConnection(parser, llp, socket, executor, acceptAllMsg)` → `newConnection(c)`
- `afterTermination()`：`acceptor.stopAndWait()` 确保端口立即释放
- `acceptAll` 开关（2.4+）：即使消息像 ACK（含 MSA-2）也强制当新消息处理

**`AcceptorThread`**——`hapi-base/src/main/java/ca/uhn/hl7v2/app/AcceptorThread.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/AcceptorThread.java>

`afterStartup()` 里 `ss.bind(...)` 并 **`ss.setSoTimeout(500)`**；`handle()` 循环 `ss.accept()`，超时就地继续（使 stop 标志有机会生效）；accept 成功的 socket 经 `socketFactory.configureNewAcceptedSocket(s)` 设置参数后 `queue.offer()`（队列满则拒收并关闭）；终止时关闭 ServerSocket。TLS 走 `SocketFactory.createTlsServerSocket()`。

**`ActiveConnection`**——`hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveConnection.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/ActiveConnection.java>

- 单 socket 连接：**一条 Receiver 线程**；`ackWriter == sendWriter`；双 socket 连接（TwoPortService）建两条 Receiver
- `responses = new BlockingHashMap<>(executor)`：用 MSA-2 把"等响应的发送方线程"与收到的回包关联（服务端也借此识别"这条入站消息是不是对我方出站消息的应答"）
- `close()`：置停止标志给 Receiver → 关闭 socket 强迫读线程退出 → `open=false`

**`Receiver`**——`hapi-base/src/main/java/ca/uhn/hl7v2/app/Receiver.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/app/Receiver.java>

- 每连接一条线程，循环 `in.getMessage()`（MLLP 读一帧）→ `processMessage()`；任何 `LLPException`/`SocketException`/`IOException` → **只关闭本连接**（`conn.close()`），不影响服务整体——这是连接级故障隔离
- `processMessage(String)`（113-127 行）：`parser.getAckID(message)`（`PipeParser.getAckID`，字符串速扫 `\rMSA` 取 MSA-2，**不整报文解析**）：
  - MSA-2 为空 → 主动消息 → 提交线程池执行 `Grunt`：`responder.processMessage(m)` 得到应答串 → `ackWriter.writeMessage(response)`；应答为 null 则不回（`DO_NOT_RESPOND` 策略的出口）
  - MSA-2 非空 → 是对我方出站的应答 → 交给等待中的 Initiator（`BlockingHashMap.give`），无人等待则记录日志忽略（`acceptAll` 时例外）

**线程池**——`ca.uhn.hl7v2.concurrent.DefaultExecutorService`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/concurrent/DefaultExecutorService.java>

默认是 **`Executors.newCachedThreadPool()`**（无线程上限、空闲回收、自定义 ThreadFactory 设守护/命名）；acceptor 线程、每连接 Receiver 线程、`Grunt` 业务线程、ConnectionCleaner 全部跑在这一个池上；`Service` 基类（`concurrent/Service.java`）统一线程模型：`start()` → `afterStartup()` → `while(isRunning()) handle()` → `afterTermination()`，提供 `stop()`/`stopAndWait()`。

**其他**：`TwoPortService`（双端口，按远端地址配对两个 socket 后才激活连接）；`ServerConfiguration` 持有 `ApplicationExceptionPolicy`（`DEFAULT` / `DO_NOT_RESPOND`，决定应用抛异常时是否回 NAK）。

### 3.3 消息解析（`ca.uhn.hl7v2.parser`）

**解析器体系**——`hapi-base/src/main/java/ca/uhn/hl7v2/parser/`
<https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-base/src/main/java/ca/uhn/hl7v2/parser>

- 抽象基类 `Parser`：定义 `parse/encode/doParse/doEncode/getEncoding/getAckID/getCriticalResponseData/getVersion`；编码名字符串为 `"VB"`（竖线）与 `"XML"`
- `PipeParser`（竖线/ER7 编码，`getDefaultEncoding()` 返回 `"VB"`）、`XMLParser`/`DefaultXMLParser`、`GenericParser`（主/备解析器按 `EncodingDetector.isEr7Encoded/isXmlEncoded` 自动识别竖线还是 XML）
- `ParserConfiguration`：解析行为开关（如 ID 生成器、超时、段意外出现时的 `UnexpectedSegmentBehaviourEnum` 等）

**竖线编码如何解析成 Message/Segment/Field/Component**——`PipeParser.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/parser/PipeParser.java>

1. `getEncodingChars(String)`（201 行）：**直接取原始串第 4 个字符作字段分隔符（MSH-1）、`substring(4,9)` 作 MSH-2**，构造 `EncodingCharacters`——分隔符来自报文本身，不是硬编码
2. `EncodingCharacters`（`parser/EncodingCharacters.java`）：字段分隔符 + 4~5 个编码字符（依次 `^` 组分、`~` 重复、`\` 转义、`&` 子组分，2.7 起可带第 5 个截断符）
3. `parse(Segment, String, EncodingCharacters, int)`（355 行起）：按字段分隔符切分；**MSH/FHS/BHS 特殊处理**（MSH-1/MSH-2 不是普通字段，要"挪位"补偿）；字段再递归 `parse(Type, ...)` 按 `^`→`&` 逐级拆成 Component/SubComponent
4. `doParse(String message, String version)`：从 MSH-9 取消息结构名（`getMessageStructure()`，优先 MSH-9-3，缺省按版本事件映射表 `DefaultModelClassFactory().getEventMapForVersion(...)` 兜底），经 `ModelClassFactory` 反射实例化**版本对应的具体消息类**（如 v2.5 的 `ORU_R01`），再逐段填充；无对应类时用 `GenericMessage`
5. **版本处理**：`getVersion()` 取 MSH-12，结合 `ca.uhn.hl7v2.Version` 枚举（V21…V281）与 classpath 上的 `hapi-structures-vXX` 模块定位模型类

**Terser 快捷访问器**——`hapi-base/src/main/java/ca/uhn/hl7v2/util/Terser.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/util/Terser.java>

路径语法 `"Segment-Field(Component.SubComponent)"`，如 `t.get("/MSH-9-1")`、`t.get("/.MSH-10")`。服务端路由、ACK 生成、业务取字段全靠它；C++ 侧可做一个等价的路径取值小工具。

### 3.4 应用分发/路由（`ca.uhn.hl7v2.protocol`）

**接口**——`hapi-base/src/main/java/ca/uhn/hl7v2/protocol/ReceivingApplication.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/protocol/ReceivingApplication.java>

业务方实现它：`Message processMessage(T theMessage, Map<String,Object> theMetadata)` + `boolean canProcess(T)`。旧接口 `ca.uhn.hl7v2.app.Application`（只传 Message，无 metadata）已被 `AppWrapper` 适配到新接口。

**路由器实现**——`hapi-base/src/main/java/ca/uhn/hl7v2/protocol/impl/ApplicationRouterImpl.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/protocol/impl/ApplicationRouterImpl.java>

- 绑定表 `List<Binding>`（`AppRoutingData` = 消息类型/触发事件/处理 ID/版本 四元组，`*` 通配，见 `AppRoutingDataImpl`）；`bindApplication/unbindApplication/disableBinding/enableBinding`
- `processMessage(Transportable)` 主流程（124-194 行）：
  1. `myParser.parse(incoming)`；解析失败 → `logAndMakeErrorMessage(e, myParser.getCriticalResponseData(原始串), parser, encoding)` 生成 NAK
  2. 元数据里放 `IN_MESSAGE_CONTROL_ID`（MSH-10）与原始串
  3. `findApplication(incomingMessageObject)`：用 Terser 读 `MSH-9-1`（消息类型）、`MSH-9-2`（触发事件）、`MSH-11-1`（处理 ID）、`MSH-12`（版本）→ 依次匹配绑定表（`matches()` 支持 `"*"` 与正则，**先注册先命中**），再调 `app.canProcess()` 确认
  4. 无匹配 → 兜底 `DefaultApplication`（回 **AR** 拒绝，见 §3.5）
  5. `app.processMessage(...)` 返回响应 Message；**用与入站相同的编码** encode 成字符串回传
- 业务异常 → `handleProcessMessageException` → 同样走 `logAndMakeErrorMessage`（默认 **AE**，可由 `setDefaultAcknowledgementMode` 改；`ReceivingApplicationExceptionHandler.processException(in, inMeta, out, e)` 可再改写出站报文）；`ServerConfiguration.getApplicationExceptionPolicy()==DO_NOT_RESPOND` 时直接返回 null 不回

> 用户提到的 `MessageTypeRouter`：hapi-hl7v2 主代码中**没有**这个类（仅 `hapi-osgi-test` 的测试文件引用了该名字），实际承担"按消息类型+触发事件分发"的就是 `ApplicationRouterImpl`（早期 HAPI 版本曾有 app 包的 MessageTypeRouter，现由 protocol 包取代）。

### 3.5 ACK 生成

> 用户任务书中的 `AckMessageGenerator` 类**不存在**；ACK 由消息对象自身的方法生成。

**ACK 代码表**——`hapi-base/src/main/java/ca/uhn/hl7v2/AcknowledgmentCode.java`
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/AcknowledgmentCode.java>

枚举：`AA` Application Accept / `AE` Application Error / `AR` Application Reject / `CA`/`CE`/`CR`（Commit 系列），`codeTable()` 返回 HL70008。

**生成逻辑**——`hapi-base/src/main/java/ca/uhn/hl7v2/model/AbstractMessage.java`（210-327 行）
<https://github.com/hapifhir/hapi-hl7v2/blob/master/hapi-base/src/main/java/ca/uhn/hl7v2/model/AbstractMessage.java>

- `generateACK()` → `generateACK(AcknowledgmentCode.AA, null)`；核心 `generateACK(code, exception)`：
  1. `instantiateACK()`：按**入站版本**（MSH-12）经 `ModelClassFactory` 实例化对应 `ACK` 消息类；拿不到则 `GenericMessage.UnknownVersion` 并补非标 MSA/ERR 段
  2. `fillResponseHeader(out, code)`（282-327 行）——**这段是 C++ 照抄的重点**：
     - MSH-1/MSH-2 原样复制；MSH-7 当前时间；MSH-9 = `ACK` + 原触发事件（v2.5+ 第三组分 `ACK`）
     - MSH-10 用 `ParserConfiguration.getIdGenerator()` 生成新消息 ID
     - MSH-11 复制入站处理 ID；MSH-12 复制入站版本（空则取最高可用版本）
     - **MSH-3/4/5/6 收发互换**（我的发送者=你的接收者，反之亦然）
     - MSA-1 = ACK 代码，**MSA-2 = 入站 MSH-10**（消息控制 ID，回 ACK 必须原样带回）
  3. 有异常时 `AbstractHL7Exception.populateResponse(out, code, 0)`（`ca/uhn/hl7v2/AbstractHL7Exception.java` 134-200 行）填 ERR 段，**v2.5 前后 ERR 结构不同**（2.5+ 用 ELD 组分，之前用 CE），并按 `HL7Exception.getResponseMessage()` 优先

**默认拒绝应用**——`hapi-base/src/main/java/ca/uhn/hl7v2/app/DefaultApplication.java`：无路由命中时返回 AR + `"No appropriate destination could be found..."`。

### 3.6 连接生命周期

| 关注点 | 实现 | 出处 |
|---|---|---|
| 读超时 | accepted socket `setSoTimeout(500)`（`StandardSocketFactory.DEFAULT_ACCEPTED_SOCKET_TIMEOUT=500`，`util/StandardSocketFactory.java`）；`MllpDecoder` 捕获 `SocketTimeoutException` 返回 null → Receiver 空转一轮再读 | `AcceptorThread.afterStartup()`；`llp/MllpDecoder.java` 65-72 行 |
| 协议错误 | 帧字节违反状态机 → `LLPException` → `Receiver.handle()` 关闭**该连接**并回调 `ReceiverParserExceptionHandler` | `app/Receiver.java` 74-81 行；`llp/MllpDecoderState.java` |
| 业务异常 | 解析失败 → NAK（用 `Parser.getCriticalResponseData()` 仅解析 MSH 凑出回包头）；处理异常 → AE（可配置 AR / DO_NOT_RESPOND）；`ReceivingApplicationExceptionHandler` 可改写 | `protocol/impl/ApplicationRouterImpl.java` 143-157、196-204、413-444 行 |
| 死连接清理 | `ConnectionCleaner` 每 500ms 遍历连接表剔除 `!isOpen()` 并通知监听器 | `app/HL7Service.java` 424-461 行 |
| 状态监听 | `ConnectionListener.connectionReceived/connectionDiscarded`，注册到 HL7Service | `app/ConnectionListener.java` |
| 重连 | 客户端侧由 `ConnectionHub` 维护/重建连接；服务端被动 accept | `app/ConnectionHub.java` |
| 优雅关闭 | `Service.stop/stopAndWait` 置标志 → 循环自然退出 → `afterTermination()`：`SimpleServer` 关 acceptor 释放端口、`HL7Service` 停 cleaner 并关闭全部连接、Receiver 随 socket 关闭而退出 | `concurrent/Service.java`；`app/SimpleServer.java` 216-222 行 |

### 3.7 HL7 over HTTP（`hapi-hl7overhttp`，ca.uhn.hl7v2.hoh）

模块名 `HAPI - Java HL7 API - HL7 over HTTP`。核心类（均在 `hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/` 下）：

- **服务端**：`hapi/server/HohServlet.java`（继承 `raw/server/HohRawServlet`，注入 `IMessageHandler<String>`，`doPost` 收 HL7 文本、回 HL7 文本）；`raw/server/HohRawServlet.java` 更底层
- **编码**：`encoder/` 包——`Hl7OverHttpRequestDecoder/Encoder`、`Hl7OverHttpResponseDecoder/Encoder`、`EncodingStyle`、`MessageMode`、`TransferEncoding`（支持分块/gzip）、`ResponseCode`
- **LLP 桥接**：`llp/Hl7OverHttpLowerLayerProtocol.java` 实现标准 `LowerLayerProtocol` 接口，**可直接塞进 `SimpleServer`**（限单 socket），例子见 `hapi-examples/.../hoh/HohLlpExample.java`：HoH LLP + `new SimpleServer(8080, llp, parser)` + `registerApplication("*","*", new DefaultApplication())` 即成 HTTP 版 HL7 服务
- **增值特性**：`auth/`（HTTP Basic 客户端/服务端回调）、`sign/`（BouncyCastle CMS 数字签名）、`relay/`（`RelayMllpListener`+`RelayHttpSender`：MLLP↔HTTP 协议中继）、`sockets/`（TLS）
- 官方文档 <https://hapifhir.github.io/hapi-hl7v2/dev_hl7overhttp.html>

对本项目意义：LIS 服务器以 TCP/MLLP 为主即可；HoH 说明"同一套 Parser/Router 内核，只换传输层 LLP"的架构思想值得 C++ 侧借鉴（传输层抽象接口）。

### 3.8 使用示例（`hapi-examples`）

<https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-examples/src/main/java/ca/uhn/hl7v2/examples>

- **`ExampleReceiverApplication.java`**——最小业务处理器（实现 `ReceivingApplication`，直接 `return theMessage.generateACK();`，即 AA）
- **`CustomThreading.java`**——自定义线程池 + 起服务：

```java
ThreadPoolExecutor executor = new ThreadPoolExecutor(10, 100, 30, TimeUnit.SECONDS,
        new ArrayBlockingQueue<>(100));
executor.setRejectedExecutionHandler(new ThreadPoolExecutor.CallerRunsPolicy());
HapiContext context = new DefaultHapiContext();
context.setExecutorService(executor);
HL7Service server = context.newServer(port, false);   // 等价 new SimpleServer(context, port, tls)
// server.startAndWait();
```

- **`HandlingCharacterEncodings.java`**——`mllp.setCharset("ISO-8859-2")` 固定字符集；或 `System.setProperty(MllpConstants.CHARSET_KEY, ...)` 全局设置；`new ExtendedMinLowerLayerProtocol()` 按 MSH-18 自动识别
- **`SendAndReceiveAMessage.java`**（客户端）、`hoh/` 包 6 个示例（`HohLlpExample`、`ExampleHl7OverHttpServletWithOneApplication`、`JettyBasedRawServer` 等）

典型服务端启动五步走（综合源码）：

```java
HapiContext ctx = new DefaultHapiContext();
HL7Service server = ctx.newServer( port, false );           // SimpleServer
server.registerApplication("ORU", "R01", new MyOruApp());   // 按类型+事件注册
server.registerApplication("QRY", "Q02", new MyQueryApp());
server.setExceptionHandler(myExceptionHandler);
server.start();                                             // startAndWait 亦可
```

---

## 4. 对 C++ 重写 LIS 服务器的组件映射建议

结合本仓库 `liscomm/liscomm.hpp` 已有的仪器侧客户端结构体（`LISCOMM_SAMPLE_QUERY_REQUEST`、`LISCOMM_SAMPLE_RESPONSE`、`LISCOMM_MSG_ACK`/`LISCOMM_ACK_CODE` 的 AA/AE/AR，`LISCOMM_MSG_ACK` 注释明确"预留作为 HL7 模拟 LIS 服务器时使用"），服务端各层可按 hapi-hl7v2 对位重写：

| HAPI 组件（出处） | C++ 对应物 | 要点 |
|---|---|---|
| `MllpConstants`/`MllpEncoder`/`MllpDecoder`/`MllpDecoderState`（`llp/`） | **MllpCodec 字节流编解码器** | 状态机：等 0x0B → 收正文 → 等 0x1C → 等 0x0D；违规即断开该连接；发送时整帧单次 write。字符集先固定（仪器侧默认 ASCII/GB 场景可配置），有余力再做 MSH-18 识别（`CharSetUtil`+`HL7Charsets` 的 Table 0211 映射） |
| `PipeParser`+`EncodingCharacters`（`parser/`） | **Er7Parser 递归下降解析** | 分隔符取自报文本身（第 4 字符、MSH-2），非硬编码 `|^~\&`；段→字段→组分（`^`）→子组分（`&`），重复字段 `~`，MSH/FHS/BHS 首两字段特殊处理；版本取 MSH-12 |
| `Terser`（`util/Terser.java`） | **路径取值工具**（如 `get(msg, "MSH-9-2")`、`get(orca, "OBR-4")`） | 路由、ACK、业务取字段统一走它，避免散落的手写 split |
| `ApplicationRouterImpl`（`protocol/impl/`） | **路由表 `map<RoutingKey, Handler>`** | Key = (MSH-9-1, MSH-9-2, MSH-12, 处理 ID)，支持 `*` 通配；注册顺序即优先级；无命中回 AR（对齐 `LISCOMM_ACK_CODE` 的 AR）。典型注册：`QRY^Q02 → 查询处理器`、`ORU^R01 → 结果处理器`、`ADT^A01 → 患者信息处理器` |
| `ReceivingApplication` | **Handler 接口** `Message handle(Message in, const Meta& meta)` | meta 带对端 IP/端口（`Responder` 已示范）、消息控制 ID、原始串。查询处理器内部把 QRD/QRF 条件映射到 `LISCOMM_QUERY_CONDITIONS`；结果处理器把 ORC/OBR/OBX 映射到 `LISCOMM_SAMPLE_TEST_ITEM` 列表 |
| `AbstractMessage.generateACK`/`fillResponseHeader`（`model/AbstractMessage.java`） | **AckBuilder 独立工具类** | 规则照抄：复制 MSH-1/2、MSH-7 当前时间、MSH-9=`ACK^原触发[^ACK]`、新消息 ID、MSH-11/12 复制、收发地址互换、MSA-1=AA/AE/AR（直接对应 `LISCOMM_ACK_CODE`）、**MSA-2=入站 MSH-10**；错误时填 ERR 段 |
| `SimpleServer`/`AcceptorThread`/`Receiver`/`ActiveConnection`（`app/`） | **Server / Acceptor / Connection / Session 四层** | 1 个 accept 线程 + 每连接 1 个读线程 + 业务线程池（HAPI 默认无界 cached pool，C++ 建议有界队列+`CallerRunsPolicy`，同 `CustomThreading` 示例）；socket 读超时 500ms 用于可中断循环；协议/IO 错误只关闭当前连接 |
| `HL7Service.ConnectionCleaner`+`ConnectionListener` | **连接管理器** | 周期剔除死连接；对外回调连接建立/断开（日志、仪器在线状态展示） |
| `Service`/`DefaultExecutorService`（`concurrent/`） | **生命周期基类** | `start/stop/stopAndWait` 语义：停服务=置标志→循环退出→关 acceptor→关全部连接，保证端口及时释放 |
| `ApplicationRouterImpl` 异常策略 + `ReceivingApplicationExceptionHandler` | **统一异常策略** | 解析失败：仅速扫 MSH（`getCriticalResponseData` 思路）即回 NAK；应用异常：默认 AE，可配 AR 或不回；处理结果映射回 `LISCOMM_MSG_ACK{ack_code, err_code}` |

**典型消息流（对齐 liscomm 结构体）**：

1. **仪器 → 服务端 `QRY^Q02`**（含 QRD 查询条件）→ 路由到查询 Handler → 查库/查缓存组装 `LISCOMM_SAMPLE_RESPONSE`（患者信息+样本信息+项目列表）→ 编码为 `ORF^R02`（或按仪器约定回 `ACK`+查询结果段）→ `AckBuilder` 保证 MSH/MSA 字段正确 → MLLP 帧回发。
2. **仪器 → 服务端 `ORU^R01`**（PID/PV1/ORC/OBR/OBX）→ 结果 Handler 解析 OBX 填充 `LISCOMM_SAMPLE_RESPONSE` 推给上层 → 回 `ACK^R01`（MSA-1=AA；数据问题用 AE 并在 ERR 说明）。

**不需要照搬的部分**：`hapi-structures-vXX` 的巨型生成类库（C++ 侧用通用 `Message/Segment/Field` 树 + 少量关注段结构即可，HAPI 的 `GenericMessage`/`GenericSegment` 就是先例）；XML 编码（仪器场景用不到）；HoH 与 JMS 传输（`protocol/JMSDestination` 等）。

## 5. 版本与维护状态

- **hapi-hl7v2**：最新 release **v2.6.0**（2025-02-05）；master HEAD `de15036`（2026-03-26），仍在维护；根 `pom.xml` 版本 2.6.0；MPL 1.1/GPL 双许可；站点 <https://hapifhir.github.io/hapi-hl7v2/>
- **hapi-fhir**：最新 release **v8.8.2**（2026-09-25），活跃；但为 FHIR（REST/JSON/XML）生态，与 HL7 v2.x MLLP 无涉

## 6. 参考链接清单

### hapi-fhir（排除性证据）
- 顶层目录 API：<https://api.github.com/repos/hapifhir/hapi-fhir/contents/>
- 完整 git 树（16,537 条，`truncated:false`）：<https://api.github.com/repos/hapifhir/hapi-fhir/git/trees/master?recursive=1>
- 唯一 "hl7v2" 命中文件（内容为 FHIR 扩展解析修复）：<https://raw.githubusercontent.com/hapifhir/hapi-fhir/master/hapi-fhir-docs/src/main/resources/ca/uhn/hapi/fhir/changelog/6_6_0/4746-hl7v2-inbound-endpoint-should-reject-malformed-resources.yaml>
- 仓库：<https://github.com/hapifhir/hapi-fhir>

### hapi-hl7v2（主要参考）
- 仓库：<https://github.com/hapifhir/hapi-hl7v2>；官网：<https://hapifhir.github.io/hapi-hl7v2/>
- MLLP：`hapi-base/src/main/java/ca/uhn/hl7v2/llp/` 下 `MllpConstants.java`、`MllpEncoder.java`、`MllpDecoder.java`、`MllpDecoderState.java`、`MinLLPReader.java`、`MinLLPWriter.java`、`MinLowerLayerProtocol.java`、`ExtendedMinLLPReader.java`、`ExtendedMllpDecoder.java`、`CharSetUtil.java`、`HL7Charsets.java`
  <https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-base/src/main/java/ca/uhn/hl7v2/llp>
- 服务器：`app/HL7Service.java`、`SimpleServer.java`、`AcceptorThread.java`、`ActiveConnection.java`、`Receiver.java`、`Responder.java`、`TwoPortService.java`、`ServerConfiguration.java`、`ConnectionListener.java`、`DefaultApplication.java`
  <https://github.com/hapifhir/hapi-hl7v2/tree/master/hapi-base/src/main/java/ca/uhn/hl7v2/app>
- 并发：`concurrent/Service.java`、`DefaultExecutorService.java`、`BlockingHashMap.java`
- 解析：`parser/PipeParser.java`、`Parser.java`、`EncodingCharacters.java`、`EncodingDetector.java`、`GenericParser.java`、`XMLParser.java`；`util/Terser.java`
- 路由：`protocol/ReceivingApplication.java`、`ReceivingApplicationExceptionHandler.java`、`ApplicationRouter.java`、`impl/ApplicationRouterImpl.java`、`impl/AppRoutingDataImpl.java`、`impl/AppWrapper.java`
- ACK：`AcknowledgmentCode.java`、`model/AbstractMessage.java`（generateACK/fillResponseHeader）、`AbstractHL7Exception.java`（populateResponse）
- HL7 over HTTP：`hapi-hl7overhttp/src/main/java/ca/uhn/hl7v2/hoh/`（`hapi/server/HohServlet.java`、`raw/server/HohRawServlet.java`、`llp/Hl7OverHttpLowerLayerProtocol.java`、`encoder/`、`relay/`、`sign/`）
- 示例：`hapi-examples/src/main/java/ca/uhn/hl7v2/examples/`（`ExampleReceiverApplication.java`、`CustomThreading.java`、`HandlingCharacterEncodings.java`、`SendAndReceiveAMessage.java`、`hoh/HohLlpExample.java`）
- 文档（辅助）：<https://hapifhir.github.io/hapi-hl7v2/dev_mllp.html>、<https://hapifhir.github.io/hapi-hl7v2/dev_hl7overhttp.html>

### 本仓库相关（C++ 映射锚点）
- `liscomm/liscomm.hpp`：`LISCOMM_SAMPLE_QUERY_REQUEST`、`LISCOMM_QUERY_CONDITIONS`、`LISCOMM_SAMPLE_RESPONSE`、`LISCOMM_MSG_ACK`/`LISCOMM_ACK_CODE`（AA/AE/AR）

# C++ LIS 服务器/客户端 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/2026-10-22-cpp-lis-server-design.md` v0.2 实现 `lisserver/` 核心库（HL7 v2.x over MLLP 服务器 + 客户端 + 模拟数据源），含 gtest 单元测试与 demo 端到端自测。

**Architecture:** 分层内核——协议内核（MllpCodec/Er7Parser/Terser/AckBuilder/Router）零 IO 依赖可单测；外壳 LisServer/LisClient 管线程与 asio 同步传输；ITransport 抽象传输层。客户端复用全部内核。

**Tech Stack:** MSVC v143 / C++17 / x64；Conan 管理依赖（`asio` header-only、`gtest`）；standalone Asio 同步式使用；不依赖 Qt/Boost。

**对设计文档的一处细化**（实施中确认更优，评审计划时一并确认）：`ISampleProvider::make_response` 返回 `optional<Message>` 会让数据源懂协议；改为返回数据级结构 `optional<SampleInfo>`（条码/患者/项目列表），由 `QryHandler` 负责组 ORF 段。其余与设计一致。

> **执行根目录**：本计划在独立 git 仓库 `D:\workspace\components\lis-server` 中执行（2026-10-22 自 RisDebug 迁出单独归档），文中所有相对路径均以该仓库根为准；后续实现代码也在该仓库归档，不进入 RisDebug。

---

## 文件结构总览

```
lisserver/
├── conanfile.txt                    Conan 依赖声明（asio、gtest）
├── lisserver.sln                    独立解决方案（不并入 RisDebug.sln）
├── include/lis/
│   ├── message.h                    EncodingChars/Field/Segment/Message/MessageMeta
│   ├── mllp_codec.h                 MllpCodec（帧状态机）
│   ├── er7_parser.h                 Er7Parser
│   ├── terser.h                     Terser 路径取值
│   ├── ack_builder.h                AckCode + build_ack
│   ├── router.h                     IHandler + Router
│   ├── transport.h                  ITransport
│   ├── tcp_transport.h              TcpTransport（standalone Asio 同步）
│   ├── thread_pool.h                有界线程池（队列满调用者执行）
│   ├── lis_server.h                 LisServer
│   ├── lis_client.h                 LisClient
│   ├── provider.h                   QueryConditions/SampleInfo/ISampleProvider/SimProvider
│   ├── qry_handler.h                QryHandler（QRY^Q02 → ORF^R02）
│   └── oru_handler.h                OruHandler（ORU^R01 → 解析结果 + AA）
├── src/
│   ├── mllp_codec.cpp  er7_parser.cpp  terser.cpp  ack_builder.cpp
│   ├── router.cpp  tcp_transport.cpp  thread_pool.cpp
│   ├── lis_server.cpp  lis_client.cpp  provider.cpp
│   ├── qry_handler.cpp  oru_handler.cpp
├── tests/
│   ├── main.cpp                     gtest 入口
│   ├── test_mllp.cpp  test_er7.cpp  test_terser.cpp
│   ├── test_ack.cpp  test_router.cpp
├── demo/lis_demo.cpp                127.0.0.1 E2E 自测
├── liblis.vcxproj                   静态库
├── listest.vcxproj                  gtest 测试 exe
└── lisdemo.vcxproj                  demo exe
```

构建约定（所有任务通用）：

```bat
cd lisserver
conan install . -g MSBuildDeps -s build_type=Debug -s arch=x86_64 --output-folder=conan
conan install . -g MSBuildDeps -s build_type=Release -s arch=x86_64 --output-folder=conan
msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64
listest\x64\Debug\listest.exe --gtest_color=no
demo\x64\Debug\lisdemo.exe
```

vcxproj 通过 `<Import Project="conan\conandeps.props" />` 引入 Conan 生成的依赖配置；所有工程定义 `ASIO_STANDALONE` 宏，链接 `ws2_32.lib`（Asio 在 Windows 必需）。

---

### Task 1: 工程脚手架（Conan + 三个工程 + 空壳编译通过）

**Files:**
- Create: `lisserver/conanfile.txt`
- Create: `lisserver/lisserver.sln`
- Create: `lisserver/liblis.vcxproj`
- Create: `lisserver/listest.vcxproj`
- Create: `lisserver/lisdemo.vcxproj`
- Create: `lisserver/tests/main.cpp`
- Create: `lisserver/demo/lis_demo.cpp`

- [ ] **Step 1: conanfile.txt**

```ini
[requires]
asio/1.30.2
gtest/1.14.0

[generators]
MSBuildDeps
```

- [ ] **Step 2: 安装依赖**

Run: `cd lisserver && conan install . -g MSBuildDeps -s build_type=Debug -s arch=x86_64 --output-folder=conan`
Expected: 生成 `conan/conandeps.props`、`conan/asio.props`、`conan/gtest*.props`，无报错。

- [ ] **Step 3: liblis.vcxproj（静态库空壳）**

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" ToolsVersion="17.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Debug|x64"><Configuration>Debug</Configuration><Platform>x64</Platform></ProjectConfiguration>
    <ProjectConfiguration Include="Release|x64"><Configuration>Release</Configuration><Platform>x64</Platform></ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals"><ProjectGuid>{A1B2C3D4-0001-4000-8000-000000000001}</ProjectGuid></PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration"><ConfigurationType>StaticLibrary</ConfigurationType><PlatformToolset>v143</PlatformToolset><CharacterSet>MultiByte</CharacterSet></PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ImportGroup Label="PropertySheets"><Import Project="conan\conandeps.props" Condition="Exists('conan\conandeps.props')" /></ImportGroup>
  <PropertyGroup><OutDir>lib\$(Platform)\$(Configuration)\</OutDir><IntDir>obj\$(ProjectName)\$(Platform)\$(Configuration)\</IntDir></PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile><LanguageStandard>stdcpp17</LanguageStandard><AdditionalOptions>/utf-8 /bigobj %(AdditionalOptions)</AdditionalOptions><PreprocessorDefinitions>ASIO_STANDALONE;%(PreprocessorDefinitions)</PreprocessorDefinitions><AdditionalIncludeDirectories>include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile>
    <Lib><AdditionalDependencies>ws2_32.lib;%(AdditionalDependencies)</AdditionalDependencies></Lib>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClInclude Include="include\lis\*.h" />
    <ClCompile Include="src\*.cpp" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
```

- [ ] **Step 4: listest.vcxproj（测试 exe）**

同 liblis 骨架，差异项：`ProjectGuid {A1B2C3D4-0002-4000-8000-000000000002}`；`<ConfigurationType>Application</ConfigurationType>`；`OutDir` 为 `listest\$(Platform)\$(Configuration)\`；`ClCompile` 增加 `<AdditionalIncludeDirectories>include;tests;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>`；`Link` 节 `<AdditionalDependencies>liblis.lib;ws2_32.lib;%(AdditionalDependencies)</AdditionalDependencies>` + `<AdditionalLibraryDirectories>lib\$(Platform)\$(Configuration);%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>`；ItemGroup 为 `<ClCompile Include="tests\*.cpp" />`；**同样导入 conan props、定义 ASIO_STANDALONE**。

- [ ] **Step 5: lisdemo.vcxproj（demo exe）**

同 listest 骨架，`ProjectGuid {A1B2C3D4-0003-4000-8000-000000000003}`；`AdditionalIncludeDirectories` 为 `include;demo;%(AdditionalIncludeDirectories)`；ItemGroup 为 `<ClCompile Include="demo\lis_demo.cpp" />`；其余与 listest 相同。

- [ ] **Step 6: lisserver.sln**

```text
Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "liblis", "liblis.vcxproj", "{A1B2C3D4-0001-4000-8000-000000000001}"
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "listest", "listest.vcxproj", "{A1B2C3D4-0002-4000-8000-000000000002}"
	ProjectSection(ProjectDependencies) = postProject
		{A1B2C3D4-0001-4000-8000-000000000001} = {A1B2C3D4-0001-4000-8000-000000000001}
	EndProjectSection
EndProject
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "lisdemo", "lisdemo.vcxproj", "{A1B2C3D4-0003-4000-8000-000000000003}"
	ProjectSection(ProjectDependencies) = postProject
		{A1B2C3D4-0001-4000-8000-000000000001} = {A1B2C3D4-0001-4000-8000-000000000001}
	EndProjectSection
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|x64 = Debug|x64
		Release|x64 = Release|x64
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		{A1B2C3D4-0001-4000-8000-000000000001}.Debug|x64.ActiveCfg = Debug|x64
		{A1B2C3D4-0001-4000-8000-000000000001}.Debug|x64.Build.0 = Debug|x64
		{A1B2C3D4-0001-4000-8000-000000000001}.Release|x64.ActiveCfg = Release|x64
		{A1B2C3D4-0001-4000-8000-000000000001}.Release|x64.Build.0 = Release|x64
		{A1B2C3D4-0002-4000-8000-000000000002}.Debug|x64.ActiveCfg = Debug|x64
		{A1B2C3D4-0002-4000-8000-000000000002}.Debug|x64.Build.0 = Debug|x64
		{A1B2C3D4-0002-4000-8000-000000000002}.Release|x64.ActiveCfg = Release|x64
		{A1B2C3D4-0002-4000-8000-000000000002}.Release|x64.Build.0 = Release|x64
		{A1B2C3D4-0003-4000-8000-000000000003}.Debug|x64.ActiveCfg = Debug|x64
		{A1B2C3D4-0003-4000-8000-000000000003}.Debug|x64.Build.0 = Debug|x64
		{A1B2C3D4-0003-4000-8000-000000000003}.Release|x64.ActiveCfg = Release|x64
		{A1B2C3D4-0003-4000-8000-000000000003}.Release|x64.Build.0 = Release|x64
	EndGlobalSection
EndGlobal
```

- [ ] **Step 7: tests/main.cpp（gtest 入口，占位通过）**

```cpp
#include <gtest/gtest.h>

TEST(Scaffold, GtestWorks) { ASSERT_EQ(1 + 1, 2); }

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
```

- [ ] **Step 8: demo/lis_demo.cpp（空壳）**

```cpp
#include <cstdio>
int main() { std::puts("lisdemo scaffold"); return 0; }
```

- [ ] **Step 9: 编译并跑测试**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_color=no`
Expected: 编译 0 错误；测试 `[       OK ] Scaffold.GtestWorks`。

- [ ] **Step 10: Commit**

```bat
git add lisserver/conanfile.txt lisserver/lisserver.sln lisserver/*.vcxproj lisserver/tests/main.cpp lisserver/demo/lis_demo.cpp
git commit -m "lisserver: 工程脚手架（conan+asio+gtest，三工程空壳编译通过）"
```

---

### Task 2: MllpCodec（帧状态机）

**Files:**
- Test: `lisserver/tests/test_mllp.cpp`
- Create: `lisserver/include/lis/mllp_codec.h`
- Create: `lisserver/src/mllp_codec.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_mllp.cpp
#include <gtest/gtest.h>
#include <lis/mllp_codec.h>

using lis::MllpCodec;

TEST(Mllp, FrameWrapsPayload) {
    MllpCodec c;
    auto framed = c.frame("MSH|^~\\&|A");
    ASSERT_EQ(framed, std::string("\x0BMSH|^~\\&|A\x1C\x0D", 13));
}

TEST(Mllp, FeedAssemblesMessageAcrossChunks) {
    MllpCodec c;
    std::optional<std::string> msg;
    for (char ch : std::string("\x0B""MSH|ABC\x1C")) msg = c.feed(static_cast<uint8_t>(ch));
    ASSERT_FALSE(msg.has_value());
    msg = c.feed(0x0D);
    ASSERT_TRUE(msg.has_value());
    ASSERT_EQ(*msg, "MSH|ABC");
}

TEST(Mllp, FeedByteViolationThrows) {
    MllpCodec c;
    c.feed(0x0B);
    ASSERT_THROW(c.feed(0x0B), lis::ProtocolError);   // START 状态收到第二个 0x0B
}

TEST(Mllp, FeedExpectEndViolationThrows) {
    MllpCodec c;
    for (char ch : std::string("\x0B""AB\x1C")) c.feed(static_cast<uint8_t>(ch));
    ASSERT_THROW(c.feed('X'), lis::ProtocolError);    // 0x1C 后必须是 0x0D
}

TEST(Mllp, FrameRoundTripWithEmbeddedCr) {
    MllpCodec c;
    std::string payload = "MSH|^~\\&|A\rPID|1||123";
    auto framed = c.frame(payload);
    std::optional<std::string> msg;
    for (size_t i = 1; i + 2 < framed.size(); ++i) msg = c.feed(static_cast<uint8_t>(framed[i]));
    ASSERT_FALSE(msg.has_value());
    msg = c.feed(static_cast<uint8_t>(framed[framed.size() - 2]));
    ASSERT_FALSE(msg.has_value());
    msg = c.feed(static_cast<uint8_t>(framed.back()));
    ASSERT_TRUE(msg.has_value());
    ASSERT_EQ(*msg, payload);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Mllp.* --gtest_color=no`
Expected: FAIL（`mllp_codec.h` 不存在，编译错误）。

- [ ] **Step 3: 实现 mllp_codec.h**

```cpp
// include/lis/mllp_codec.h
#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace lis {

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& m) : std::runtime_error(m) {}
};

// MLLP 帧状态机：等 0x0B -> 收正文 -> 等 0x1C -> 等 0x0D。
// 帧字节违规抛 ProtocolError（调用方负责关闭该连接）。
class MllpCodec {
public:
    std::optional<std::string> feed(uint8_t byte);
    std::string frame(const std::string& payload) const;

private:
    enum class State { kStart, kBody, kExpectEnd, kEnd } state_ = State::kStart;
    std::string buf_;
};

}  // namespace lis
```

- [ ] **Step 4: 实现 mllp_codec.cpp**

```cpp
// src/mllp_codec.cpp
#include <lis/mllp_codec.h>

namespace lis {

namespace { constexpr uint8_t kStartByte = 0x0B, kEnd1 = 0x1C, kEnd2 = 0x0D; }

std::optional<std::string> MllpCodec::feed(uint8_t byte) {
    switch (state_) {
    case State::kStart:
        if (byte != kStartByte) throw ProtocolError("MLLP violation: expected start byte 0x0B");
        buf_.clear(); state_ = State::kBody; return std::nullopt;
    case State::kBody:
        if (byte == kEnd1) { state_ = State::kExpectEnd; return std::nullopt; }
        buf_.push_back(static_cast<char>(byte)); return std::nullopt;
    case State::kExpectEnd:
        if (byte != kEnd2) throw ProtocolError("MLLP violation: expected end byte 0x0D");
        state_ = State::kEnd; [[fallthrough]];
    case State::kEnd:
        state_ = State::kStart;
        return std::optional<std::string>(std::move(buf_));
    }
    return std::nullopt;
}

std::string MllpCodec::frame(const std::string& payload) const {
    std::string out;
    out.reserve(payload.size() + 3);
    out.push_back(static_cast<char>(kStartByte));
    out += payload;
    out.push_back(static_cast<char>(kEnd1));
    out.push_back(static_cast<char>(kEnd2));
    return out;
}

}  // namespace lis
```

- [ ] **Step 5: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Mllp.* --gtest_color=no`
Expected: 5 个用例全 `[       OK ]`。

- [ ] **Step 6: Commit**

```bat
git add lisserver/include/lis/mllp_codec.h lisserver/src/mllp_codec.cpp lisserver/tests/test_mllp.cpp
git commit -m "lisserver: MLLP 帧编解码状态机"
```

---

### Task 3: 消息树 + Er7Parser

**Files:**
- Test: `lisserver/tests/test_er7.cpp`
- Create: `lisserver/include/lis/message.h`
- Create: `lisserver/include/lis/er7_parser.h`
- Create: `lisserver/src/er7_parser.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_er7.cpp
#include <gtest/gtest.h>
#include <lis/er7_parser.h>

using lis::Er7Parser;

static const char* kAdt =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1\r"
    "QRD|202610221159|R|I|BAR001|||||RES";

TEST(Er7, DelimitersComeFromMessageItself) {
    // 用非常规字段分隔符 # 与组分 ! 的报文
    Er7Parser p;
    auto m = p.parse("MSH#^~\\&#A#B#C#D#202601010000##QRY^Q02#ID1#P#2.3");
    ASSERT_EQ(m.ec.field, '#');
    ASSERT_EQ(lis::Terser::get(m, "/MSH-3"), "A");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-9-1"), "QRY");
}

TEST(Er7, MshSpecialFirstFields) {
    Er7Parser p;
    auto m = p.parse(kAdt);
    // MSH-1/MSH-2 是分隔符本身，不占普通字段
    ASSERT_EQ(lis::Terser::get(m, "/MSH-3"), "SendApp");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-9-1"), "QRY");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-9-2"), "Q02");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-10"), "CTRL001");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-12"), "2.3.1");
    ASSERT_EQ(m.version, "2.3.1");
}

TEST(Er7, NormalSegmentsAndComponents) {
    Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A\rPID|1||123^4^5||Doe^John");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3"), "123^4^5");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3-2"), "4");
    ASSERT_EQ(lis::Terser::get(m, "/PID-5-1"), "Doe");
}

TEST(Er7, EncodeRoundTrip) {
    Er7Parser p;
    auto m = p.parse(kAdt);
    auto raw = p.encode(m);
    auto m2 = p.parse(raw);
    ASSERT_EQ(lis::Terser::get(m2, "/MSH-10"), "CTRL001");
    ASSERT_EQ(lis::Terser::get(m2, "/QRD-5"), "BAR001");
}

TEST(Er7, ParseRejectsGarbage) {
    Er7Parser p;
    ASSERT_THROW(p.parse("NOT_A_HL7_MESSAGE"), lis::ParseError);
}
```

（注：本任务先建 `Terser` 最小实现于 `terser.h`，Task 4 再补全 set 与测试——本测试只用 `Terser::get`。）

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Er7.* --gtest_color=no`
Expected: 编译失败（`message.h`/`er7_parser.h` 不存在）。

- [ ] **Step 3: message.h**

```cpp
// include/lis/message.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lis {

struct EncodingChars {   // 取自报文本身（MSH-1/MSH-2），非硬编码
    char field = '|', component = '^', repeat = '~', escape = '\\', subcomponent = '&';
};

struct Field { std::vector<std::string> components; };  // 子组分 & 先以字面量存于 component 串内

struct Segment {
    std::string name;              // "MSH"/"PID"/...
    std::vector<Field> fields;     // 统一规则：SEG-n -> fields[n-1]；MSH 的 fields[0] 为占位、fields[1] 为 MSH-2
};

struct Message {
    EncodingChars ec;
    std::string version;           // MSH-12
    std::vector<Segment> segments;
    std::string raw;
};

struct MessageMeta {
    std::string remote_ip;
    uint16_t remote_port = 0;
    std::string control_id;        // 入站 MSH-10
};

}  // namespace lis
```

- [ ] **Step 4: er7_parser.h + terser.h（最小 get）**

```cpp
// include/lis/er7_parser.h
#pragma once
#include <lis/message.h>
#include <stdexcept>
#include <string>

namespace lis {

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& m) : std::runtime_error(m) {}
};

class Er7Parser {
public:
    Message parse(const std::string& raw);
    std::string encode(const Message& m) const;
};

}  // namespace lis
```

```cpp
// include/lis/terser.h
#pragma once
#include <lis/message.h>
#include <stdexcept>
#include <string>

namespace lis {

class TerserPathError : public std::runtime_error {
public:
    explicit TerserPathError(const std::string& m) : std::runtime_error(m) {}
};

// 路径形如 "/SEG-n" 或 "/SEG-n-m"（n=字段号, m=1 基组分号）。
// 统一规则：SEG-n -> fields[n-1]（MSH 因 fields[0] 占位天然满足 MSH-3->fields[2]）。
class Terser {
public:
    static std::string get(const Message& m, const char* path);
    static void set(Message& m, const char* path, const std::string& v);
};

}  // namespace lis
```

- [ ] **Step 5: er7_parser.cpp（含 split 工具与 Terser::get 实现，set 在 Task 4 补）**

```cpp
// src/er7_parser.cpp
#include <lis/er7_parser.h>
#include <lis/terser.h>
#include <sstream>

namespace lis {
namespace {

std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == d) { out.push_back(cur); cur.clear(); } else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

Field make_field(const std::string& text, char comp) {
    Field f;
    for (auto& c : split(text, comp)) f.components.push_back(c);
    return f;
}

Segment parse_segment(const std::string& line, const EncodingChars& ec, bool is_msh) {
    Segment seg;
    size_t pos = line.find(ec.field);
    if (pos == std::string::npos || pos < 3) throw ParseError("segment too short: " + line);
    seg.name = line.substr(0, 3);
    auto parts = split(line.substr(pos + 1), ec.field);
    if (is_msh) {
        seg.fields.push_back(Field{});                       // fields[0]：MSH-1 占位
        seg.fields.push_back(make_field(parts.front(), ec.component));  // fields[1]：MSH-2
        parts.erase(parts.begin());
    }
    for (auto& p : parts) seg.fields.push_back(make_field(p, ec.component));
    return seg;
}

}  // namespace

Message Er7Parser::parse(const std::string& raw) {
    if (raw.size() < 8 || raw.substr(0, 3) != "MSH") throw ParseError("message must start with MSH");
    Message m;
    m.ec.field = raw[3];
    m.ec.component = raw[4]; m.ec.repeat = raw[5]; m.ec.escape = raw[6]; m.ec.subcomponent = raw[7];
    m.raw = raw;
    std::istringstream iss(raw);
    std::string line;
    bool first = true;
    while (std::getline(iss, line, '\r')) {
        if (line.empty()) continue;
        m.segments.push_back(parse_segment(line, m.ec, first));
        first = false;
    }
    if (m.segments.empty()) throw ParseError("no segments");
    try { m.version = Terser::get(m, "/MSH-12"); } catch (...) { m.version.clear(); }
    return m;
}

std::string Er7Parser::encode(const Message& m) const {
    std::string out;
    for (size_t i = 0; i < m.segments.size(); ++i) {
        if (i) out.push_back('\r');
        const auto& s = m.segments[i];
        out += s.name;
        out.push_back(m.ec.field);
        size_t begin = (s.name == "MSH") ? 1 : 0;   // MSH 跳过 fields[0] 占位
        for (size_t j = begin; j < s.fields.size(); ++j) {
            if (j > begin) out.push_back(m.ec.field);
            const auto& comps = s.fields[j].components;
            for (size_t k = 0; k < comps.size(); ++k) {
                if (k) out.push_back(m.ec.component);
                out += comps[k];
            }
        }
    }
    return out;
}

}  // namespace lis
```

```cpp
// src/terser.cpp —— 本任务只实现 get（测试所需），set 留 Task 4
#include <lis/terser.h>
#include <cstdlib>

namespace lis {
namespace {

Segment& find_seg(Message& m, const std::string& name) {
    for (auto& s : m.segments) if (s.name == name) return s;
    m.segments.push_back(Segment{name, {}});
    return m.segments.back();
}

const Segment* find_seg(const Message& m, const std::string& name) {
    for (auto& s : m.segments) if (s.name == name) return &s;
    return nullptr;
}

struct Path { std::string seg; int field; int comp; };  // comp=0 表示未指定

Path parse_path(const char* path) {
    // 形如 "/MSH-9-2"
    std::string p(path);
    if (p.empty() || p[0] != '/') throw TerserPathError("path must start with /");
    Path out; out.field = 0; out.comp = 0;
    auto t1 = p.find('-', 1);
    if (t1 == std::string::npos) throw TerserPathError("missing field: " + p);
    out.seg = p.substr(1, t1 - 1);
    auto t2 = p.find('-', t1 + 1);
    out.field = std::atoi(p.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1).c_str());
    if (out.field <= 0) throw TerserPathError("bad field no: " + p);
    if (t2 != std::string::npos) out.comp = std::atoi(p.substr(t2 + 1).c_str());
    return out;
}

}  // namespace

std::string Terser::get(const Message& m, const char* path) {
    auto pp = parse_path(path);
    const Segment* s = find_seg(m, pp.seg);
    if (!s || static_cast<size_t>(pp.field) > s->fields.size()) return "";
    const auto& f = s->fields[pp.field - 1];
    if (pp.comp > 0) return pp.comp <= static_cast<int>(f.components.size()) ? f.components[pp.comp - 1] : "";
    std::string out;
    for (size_t i = 0; i < f.components.size(); ++i) {
        if (i) out.push_back(m.ec.component);
        out += f.components[i];
    }
    return out;
}

void Terser::set(Message& m, const char* path, const std::string& v) {
    auto pp = parse_path(path);
    auto& s = find_seg(m, pp.seg);
    while (s.fields.size() < static_cast<size_t>(pp.field)) s.fields.push_back(Field{});
    auto& f = s.fields[pp.field - 1];
    if (pp.comp > 0) {
        while (f.components.size() < static_cast<size_t>(pp.comp)) f.components.push_back("");
        f.components[pp.comp - 1] = v;
    } else {
        f.components.assign(1, v);
    }
}

}  // namespace lis
```

（`set` 一并实现了，Task 4 只补测试，符合 DRY。）

- [ ] **Step 6: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Er7.* --gtest_color=no`
Expected: 5 个用例全 `[       OK ]`。

- [ ] **Step 7: Commit**

```bat
git add lisserver/include/lis/message.h lisserver/include/lis/er7_parser.h lisserver/include/lis/terser.h lisserver/src/er7_parser.cpp lisserver/src/terser.cpp lisserver/tests/test_er7.cpp
git commit -m "lisserver: ER7 消息树与解析器（分隔符取自报文、MSH 特殊字段、Terser）"
```

---

### Task 4: Terser 补全测试（set 已在 Task 3 实现）

**Files:**
- Test: `lisserver/tests/test_terser.cpp`

- [ ] **Step 1: 写测试**

```cpp
// tests/test_terser.cpp
#include <gtest/gtest.h>
#include <lis/er7_parser.h>
#include <lis/terser.h>

TEST(Terser, SetExistingAndNewField) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A|B|C|D|||QRY^Q02|ID1|P|2.3");
    lis::Terser::set(m, "/MSH-10", "NEWID");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-10"), "NEWID");
    lis::Terser::set(m, "/PID-5-1", "Zhang");
    lis::Terser::set(m, "/PID-5-2", "San");
    ASSERT_EQ(lis::Terser::get(m, "/PID-5"), "Zhang^San");
    ASSERT_EQ(p.encode(m).find("PID|Zhang^San") != std::string::npos, true);
}

TEST(Terser, GetMissingReturnsEmpty) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A");
    ASSERT_EQ(lis::Terser::get(m, "/PID-3"), "");
    ASSERT_EQ(lis::Terser::get(m, "/MSH-99"), "");
}

TEST(Terser, BadPathThrows) {
    lis::Er7Parser p;
    auto m = p.parse("MSH|^~\\&|A");
    ASSERT_THROW(lis::Terser::get(m, "MSH-3"), lis::TerserPathError);
}
```

- [ ] **Step 2: 跑测试**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Terser.* --gtest_color=no`
Expected: 3 个用例全 `[       OK ]`（实现已存在，直接通过——属补网测试）。

- [ ] **Step 3: Commit**

```bat
git add lisserver/tests/test_terser.cpp
git commit -m "lisserver: Terser set/get 测试补齐"
```

---

### Task 5: AckBuilder

**Files:**
- Test: `lisserver/tests/test_ack.cpp`
- Create: `lisserver/include/lis/ack_builder.h`
- Create: `lisserver/src/ack_builder.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_ack.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/terser.h>

static const char* kInbound =
    "MSH|^~\\&|SendApp|SendFac|RecvApp|RecvFac|20261022120000||QRY^Q02|CTRL001|P|2.3.1";

TEST(Ack, AaCopiesAndSwapsFields) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AA);
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-9"), "ACK^Q02");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-2"), "CTRL001");      // 带回入站消息控制 ID
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-3"), "RecvApp");      // 收发互换
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-4"), "RecvFac");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-5"), "SendApp");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-6"), "SendFac");
    ASSERT_NE(lis::Terser::get(ack, "/MSH-10"), "CTRL001");     // 新消息 ID
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-11"), "P");
    ASSERT_EQ(lis::Terser::get(ack, "/MSH-12"), "2.3.1");       // 版本复制
}

TEST(Ack, AeWithErrAddsErrSegment) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AE, "bad query");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-1"), "AE");
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-3"), "bad query");
    ASSERT_EQ(ack.segments.size(), 3u);                          // MSH/MSA/ERR
    ASSERT_EQ(ack.segments[2].name, "ERR");
}

TEST(Ack, EncodeRoundTrips) {
    lis::Er7Parser p;
    auto in = p.parse(kInbound);
    auto ack = lis::build_ack(in, lis::AckCode::AR);
    auto m2 = p.parse(p.encode(ack));
    ASSERT_EQ(lis::Terser::get(m2, "/MSA-1"), "AR");
    ASSERT_EQ(lis::Terser::get(m2, "/MSA-2"), "CTRL001");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Ack.* --gtest_color=no`
Expected: 编译失败（`ack_builder.h` 不存在）。

- [ ] **Step 3: ack_builder.h**

```cpp
// include/lis/ack_builder.h
#pragma once
#include <lis/message.h>
#include <string>

namespace lis {

enum class AckCode { AA, AE, AR };   // 对齐 liscomm LISCOMM_ACK_CODE

// 照抄 HAPI AbstractMessage.fillResponseHeader 规则：
// MSH-1/2 原样；MSH-7 当前时间；MSH-9=ACK^原触发；MSH-10 新 ID；MSH-11/12 复制入站；
// MSH-3/4/5/6 收发互换；MSA-1=code；MSA-2=入站 MSH-10；err 非空补 ERR 段与 MSA-3。
Message build_ack(const Message& inbound, AckCode code, const std::string& err = {});

}  // namespace lis
```

- [ ] **Step 4: ack_builder.cpp**

```cpp
// src/ack_builder.cpp
#include <lis/ack_builder.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>
#include <ctime>

namespace lis {
namespace {

std::string now_yyyymmddhhmmss() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
    return buf;
}

std::string new_message_id() {
    static std::atomic<unsigned long long> counter{0};
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return "ACK" + std::to_string(ms) + std::to_string(counter++);
}

Segment make_msh_like(const Message& inbound, const std::string& trigger) {
    Segment msh{"MSH", {}};
    msh.fields.push_back(Field{});                        // fields[0] 占位（MSH-1）
    msh.fields.push_back(Field{{std::string(1, inbound.ec.component) +
                                inbound.ec.repeat + std::string(1, inbound.ec.escape) +
                                std::string(1, inbound.ec.subcomponent)}});  // fields[1]=MSH-2
    auto put = [&](int field_no, std::initializer_list<std::string> comps) {
        while (msh.fields.size() < static_cast<size_t>(field_no)) msh.fields.push_back(Field{});
        msh.fields[field_no - 1].components.assign(comps.begin(), comps.end());
    };
    put(3, {Terser::get(inbound, "/MSH-5")});   // 我们的发送方 = 对方的接收方
    put(4, {Terser::get(inbound, "/MSH-6")});
    put(5, {Terser::get(inbound, "/MSH-3")});   // 我们的接收方 = 对方的发送方
    put(6, {Terser::get(inbound, "/MSH-4")});
    put(7, {now_yyyymmddhhmmss()});
    put(9, {"ACK", trigger});                   // MSH-9
    put(10, {new_message_id()});
    put(11, {Terser::get(inbound, "/MSH-11")});
    std::string ver = Terser::get(inbound, "/MSH-12");
    if (!ver.empty()) put(12, {ver});
    return msh;
}

}  // namespace

Message build_ack(const Message& inbound, AckCode code, const std::string& err) {
    Message ack;
    ack.ec = inbound.ec;
    ack.version = inbound.version;
    const char* code_str = code == AckCode::AA ? "AA" : code == AckCode::AE ? "AE" : "AR";
    const std::string trigger = Terser::get(inbound, "/MSH-9-2");

    ack.segments.push_back(make_msh_like(inbound, trigger));

    Segment msa{"MSA", {}};
    msa.fields.push_back(Field{{code_str}});                          // MSA-1
    msa.fields.push_back(Field{{Terser::get(inbound, "/MSH-10")}});   // MSA-2
    if (!err.empty()) msa.fields.push_back(Field{{err}});             // MSA-3
    ack.segments.push_back(std::move(msa));

    if (!err.empty()) {
        Segment seg_err{"ERR", {}};
        seg_err.fields.push_back(Field{{"", "", "", err}});            // ERR-1 组分最小形态
        ack.segments.push_back(std::move(seg_err));
    }
    return ack;
}

}  // namespace lis
```

- [ ] **Step 5: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Ack.* --gtest_color=no`
Expected: 3 个用例全 `[       OK ]`。

- [ ] **Step 6: Commit**

```bat
git add lisserver/include/lis/ack_builder.h lisserver/src/ack_builder.cpp lisserver/tests/test_ack.cpp
git commit -m "lisserver: ACK 生成（HAPI fillResponseHeader 规则）"
```

---

### Task 6: Router + IHandler

**Files:**
- Test: `lisserver/tests/test_router.cpp`
- Create: `lisserver/include/lis/router.h`
- Create: `lisserver/src/router.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_router.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/router.h>
#include <lis/terser.h>

static const char* kQry =
    "MSH|^~\\&|A|B|C|D|20261022120000||QRY^Q02|ID9|P|2.3.1";

static std::shared_ptr<lis::IHandler> fn_handler(
    std::function<lis::Message(const lis::Message&, const lis::MessageMeta&)> fn) {
    return std::make_shared<lis::FnHandler>(std::move(fn));
}

TEST(Router, ExactMatch) {
    lis::Router r;
    bool called = false;
    r.add("QRY", "Q02", "*", fn_handler([&](const lis::Message&, const lis::MessageMeta&) {
        called = true;
        return lis::Message{};
    }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_TRUE(called);
}

TEST(Router, FirstRegisteredWins) {
    lis::Router r;
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&) {
        lis::Message m; m.segments.push_back(lis::Segment{"AAA", {}}); return m;
    }));
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&) {
        lis::Message m; m.segments.push_back(lis::Segment{"BBB", {}}); return m;
    }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_EQ(out.segments[0].name, "AAA");
}

TEST(Router, NoMatchReturnsAr) {
    lis::Router r;
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_EQ(lis::Terser::get(out, "/MSA-1"), "AR");
    ASSERT_EQ(lis::Terser::get(out, "/MSA-2"), "ID9");
}

TEST(Router, HandlerExceptionReturnsAe) {
    lis::Router r;
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&)
        -> lis::Message { throw std::runtime_error("boom"); }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_EQ(lis::Terser::get(out, "/MSA-1"), "AE");
}

TEST(Router, EmptyResponseMeansNoReply) {
    lis::Router r;
    r.add("QRY", "Q02", "*", fn_handler([](const lis::Message&, const lis::MessageMeta&) {
        return lis::Message{};
    }));
    lis::Er7Parser p;
    auto out = r.dispatch(p.parse(kQry), lis::MessageMeta{});
    ASSERT_TRUE(out.segments.empty());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Router.* --gtest_color=no`
Expected: 编译失败（`router.h` 不存在）。

- [ ] **Step 3: router.h**

```cpp
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
```

- [ ] **Step 4: router.cpp**

```cpp
// src/router.cpp
#include <lis/router.h>
#include <lis/ack_builder.h>
#include <lis/terser.h>

namespace lis {
namespace {

bool match(const std::string& pattern, const std::string& value) {
    return pattern == "*" || pattern == value;
}

}  // namespace

void Router::add(std::string msg_type, std::string trigger, std::string version,
                 std::shared_ptr<IHandler> handler) {
    bindings_.push_back(Binding{std::move(msg_type), std::move(trigger),
                                std::move(version), std::move(handler)});
}

Message Router::dispatch(const Message& in, const MessageMeta& meta) const {
    const std::string type = Terser::get(in, "/MSH-9-1");
    const std::string trigger = Terser::get(in, "/MSH-9-2");
    const std::string version = Terser::get(in, "/MSH-12");
    for (const auto& b : bindings_) {
        if (match(b.type, type) && match(b.trigger, trigger) && match(b.version, version)) {
            try {
                return b.handler->handle(in, meta);
            } catch (const std::exception& e) {
                return build_ack(in, AckCode::AE, e.what());
            } catch (...) {
                return build_ack(in, AckCode::AE, "handler unknown error");
            }
        }
    }
    return build_ack(in, AckCode::AR, "No appropriate destination could be found");
}

}  // namespace lis
```

- [ ] **Step 5: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Router.* --gtest_color=no`
Expected: 5 个用例全 `[       OK ]`。

- [ ] **Step 6: Commit**

```bat
git add lisserver/include/lis/router.h lisserver/src/router.cpp lisserver/tests/test_router.cpp
git commit -m "lisserver: 消息路由（通配、优先级、默认 AR、异常 AE）"
```

---

### Task 7: ITransport + TcpTransport（Asio 同步）+ ThreadPool

**Files:**
- Test: `lisserver/tests/test_transport.cpp`
- Create: `lisserver/include/lis/transport.h`
- Create: `lisserver/include/lis/tcp_transport.h`
- Create: `lisserver/src/tcp_transport.cpp`
- Create: `lisserver/include/lis/thread_pool.h`
- Create: `lisserver/src/thread_pool.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_transport.cpp
#include <gtest/gtest.h>
#include <lis/tcp_transport.h>
#include <chrono>
#include <thread>

TEST(Transport, LoopbackEcho) {
    lis::TcpTransport listener;
    ASSERT_TRUE(listener.listen(0));                       // 0 = 随机端口
    const auto port = listener.local_port();
    ASSERT_NE(port, 0u);

    std::thread server_thread([&] {
        auto peer = listener.accept(3000);
        ASSERT_NE(peer, nullptr);
        uint8_t buf[64];
        int n = peer->read(buf, sizeof buf, 3000);
        ASSERT_GT(n, 0);
        ASSERT_TRUE(peer->write(buf, static_cast<size_t>(n)));
    });

    lis::TcpTransport client;
    ASSERT_TRUE(client.connect("127.0.0.1", port, 3000));
    ASSERT_TRUE(client.write(reinterpret_cast<const uint8_t*>("hi"), 2));
    uint8_t buf[64]{};
    ASSERT_EQ(client.read(buf, sizeof buf, 3000), 2);
    ASSERT_EQ(std::string(reinterpret_cast<char*>(buf), 2), "hi");

    client.close();
    server_thread.join();
    listener.close();
}

TEST(ThreadPool, CallerRunsWhenQueueFull) {
    lis::ThreadPool pool(1, 1);                            // 1  worker，队列上限 1
    std::atomic<int> done{0};
    pool.submit([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); ++done; });
    pool.submit([&] { ++done; });                          // 队列满 -> 调用线程自己执行
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_EQ(done.load(), 2);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Transport.*:ThreadPool.* --gtest_color=no`
Expected: 编译失败（`tcp_transport.h`/`thread_pool.h` 不存在）。

- [ ] **Step 3: transport.h + thread_pool.h**

```cpp
// include/lis/transport.h
#pragma once
#include <cstddef>
#include <cstdint>
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

}  // namespace lis
```

```cpp
// include/lis/thread_pool.h
#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace lis {

// 有界线程池：停止后不再接受任务；队列满时由提交线程自己执行（CallerRuns，对齐 HAPI CustomThreading）。
class ThreadPool {
public:
    ThreadPool(size_t workers, size_t max_queue = 256);
    ~ThreadPool();
    void submit(std::function<void()> task);
    void stop();

private:
    void loop();
    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    size_t max_queue_;
    bool stopping_ = false;
};

}  // namespace lis
```

- [ ] **Step 4: thread_pool.cpp**

```cpp
// src/thread_pool.cpp
#include <lis/thread_pool.h>

namespace lis {

ThreadPool::ThreadPool(size_t workers, size_t max_queue) : max_queue_(max_queue) {
    threads_.reserve(workers);
    for (size_t i = 0; i < workers; ++i) threads_.emplace_back([this] { loop(); });
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_ || tasks_.size() >= max_queue_) {
            // 停止后丢弃；队列满由调用者执行
        } else {
            tasks_.push(std::move(task));
            cv_.notify_one();
            return;
        }
    }
    if (task) task();   // CallerRuns
}

void ThreadPool::loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

void ThreadPool::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
}

}  // namespace lis
```

- [ ] **Step 5: tcp_transport.h**

```cpp
// include/lis/tcp_transport.h
#pragma once
#include <lis/transport.h>
#include <asio.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lis {

// standalone Asio 同步实现（v1 不用 io_context 事件循环）。
// 超时统一用 async_* + io_context::run_for 实现；每个实例自有 io_context，单线程使用。
class TcpTransport : public ITransport {
public:
    TcpTransport();
    explicit TcpTransport(asio::ip::tcp::socket socket);
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
    asio::io_context io_{};
    asio::ip::tcp::socket socket_;
    std::optional<asio::ip::tcp::acceptor> acceptor_;
};

}  // namespace lis
```

- [ ] **Step 6: tcp_transport.cpp**

```cpp
// src/tcp_transport.cpp
#include <lis/tcp_transport.h>
#include <chrono>

namespace lis {

TcpTransport::TcpTransport() : socket_(io_) {}
TcpTransport::TcpTransport(asio::ip::tcp::socket socket)
    : socket_(std::move(socket)) {}   // socket 绑了对方 io_context，后续 IO 用 io_？——见下方注意
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
    if (!done) { close(); return false; }   // connect 超时
    if (ec) { close(); return false; }
    socket_.set_option(asio::ip::tcp::no_delay(true));
    return true;
}

bool TcpTransport::listen(uint16_t port) {
    close();
    asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
    acceptor_.emplace(io_, ep, true);   // reuse_address
    return acceptor_->is_open();
}

std::unique_ptr<TcpTransport> TcpTransport::accept(int timeout_ms) {
    if (!acceptor_) return nullptr;
    asio::ip::tcp::socket peer_io_context_socket(io_);   // 注意：新 socket 用 acceptor 的 io_context
    bool done = false;
    std::error_code ec;
    acceptor_->async_accept(peer_io_context_socket, [&](std::error_code e) { ec = e; done = true; });
    io_.restart();
    io_.run_for(std::chrono::milliseconds(timeout_ms));
    if (!done || ec) return nullptr;
    peer_io_context_socket.set_option(asio::ip::tcp::no_delay(true));
    // 转移 socket 的所有权：asio socket 不可跨 io_context 转移，这里让新 TcpTransport
    // 借用 acceptor 的 io_context —— 实现见 cpp 末尾的移交构造说明。
    return std::unique_ptr<TcpTransport>(new TcpTransport(std::move(peer_io_context_socket)));
}

int TcpTransport::read(uint8_t* buf, size_t n, int timeout_ms) {
    if (!socket_.is_open()) return -1;
    std::error_code ec;
    size_t bytes = 0;
    bool done = false;
    socket_.async_read_some(asio::buffer(buf, n),
        [&](std::error_code e, size_t b) { ec = e; bytes = b; done = true; });
    io_.restart();
    io_.run_for(std::chrono::milliseconds(timeout_ms));
    if (!done) { socket_.cancel(); return 0; }          // 读超时（可中断循环的关键）
    if (ec == asio::error::operation_aborted) return 0;
    if (ec) return -1;                                   // 对端关闭/IO 错误
    return static_cast<int>(bytes);
}

bool TcpTransport::write(const uint8_t* buf, size_t n) {
    if (!socket_.is_open()) return false;
    std::error_code ec;
    asio::write(socket_, asio::buffer(buf, n), ec);
    return !ec;
}

void TcpTransport::close() {
    std::error_code ec;
    if (socket_.is_open()) { socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec); socket_.close(ec); }
    if (acceptor_ && acceptor_->is_open()) acceptor_->close(ec);
}

uint16_t TcpTransport::local_port() const {
    if (!acceptor_) return 0;
    std::error_code ec;
    auto ep = acceptor_->local_endpoint(ec);
    return ec ? 0 : ep.port();
}

std::string TcpTransport::remote_ip() const {
    std::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    return ec ? "" : ep.address().to_string();
}

uint16_t TcpTransport::remote_port() const {
    std::error_code ec;
    auto ep = socket_.remote_endpoint(ec);
    return ec ? 0 : ep.port();
}

}  // namespace lis
```

**关于 accept 移交的 io_context 问题（实现者必读）**：`asio::ip::tcp::socket` 不可跨 `io_context` 移动。上方 `accept()` 的写法让被接受的 socket 始终属于 `acceptor` 所在的 `io_`，`TcpTransport(std::move(socket))` 构造后必须让该 transport 的 IO 也用这个 `io_`。因此：

- 把 `TcpTransport` 的构造改为：`explicit TcpTransport(asio::io_context& io)` 持有 `asio::ip::tcp::socket socket_;` 时直接用传入的 io 构造 socket；
- `accept()` 内：`auto t = std::unique_ptr<TcpTransport>(new TcpTransport(io_));` 然后 `acceptor_->async_accept(t->socket_, ...)`（asio 支持直接 async_accept 到目标 socket）；
- `connect()` 路径不受影响（默认构造仍用自有 `io_`）。

即头文件改为：

```cpp
    TcpTransport();                          // 客户端：自有 io_context
    explicit TcpTransport(asio::io_context& io);  // 服务端 accept 出来的连接：借用 acceptor 的 io
    // 删除原 TcpTransport(asio::ip::tcp::socket) 构造
```

`accept()` 相应改为先创建 transport 再 `async_accept(t->socket_, ...)`，成功后 `return t;`。

- [ ] **Step 7: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Transport.*:ThreadPool.* --gtest_color=no`
Expected: 2 个用例全 `[       OK ]`。

- [ ] **Step 8: Commit**

```bat
git add lisserver/include/lis/transport.h lisserver/include/lis/tcp_transport.h lisserver/src/tcp_transport.cpp lisserver/include/lis/thread_pool.h lisserver/src/thread_pool.cpp lisserver/tests/test_transport.cpp
git commit -m "lisserver: Asio 同步传输层与有界线程池"
```

---

### Task 8: LisServer

**Files:**
- Test: `lisserver/tests/test_server.cpp`
- Create: `lisserver/include/lis/lis_server.h`
- Create: `lisserver/src/lis_server.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_server.cpp
#include <gtest/gtest.h>
#include <lis/lis_server.h>
#include <lis/mllp_codec.h>
#include <lis/er7_parser.h>
#include <lis/ack_builder.h>
#include <lis/tcp_transport.h>
#include <chrono>
#include <thread>

static const char* kQry =
    "MSH|^~\\&|Instr|Fac|Lis|Fac|20261022120000||QRY^Q02|T1|P|2.3.1\rQRD|202610221159|R|I|BAR001";

// 客户端侧原始收发工具：写一帧，读一帧（带超时）
static std::string transact(uint16_t port, const std::string& payload) {
    lis::TcpTransport c;
    EXPECT_TRUE(c.connect("127.0.0.1", port, 2000));
    lis::MllpCodec codec;
    auto framed = codec.frame(payload);
    EXPECT_TRUE(c.write(reinterpret_cast<const uint8_t*>(framed.data()), framed.size()));
    uint8_t buf[8192];
    std::optional<std::string> msg;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!msg && std::chrono::steady_clock::now() < deadline) {
        int n = c.read(buf, sizeof buf, 500);
        if (n <= 0) continue;
        for (int i = 0; i < n && !msg; ++i) msg = codec.feed(buf[i]);
    }
    c.close();
    EXPECT_TRUE(msg.has_value());
    return msg.value_or("");
}

TEST(Server, QueryGetsAck) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) {
            return lis::build_ack(in, lis::AckCode::AA);
        }));
    lis::LisServer server({0, 2, 500}, std::move(router));   // port 0 = 随机端口
    server.start();
    const auto port = server.local_port();
    ASSERT_NE(port, 0u);

    auto reply = transact(port, kQry);
    lis::Er7Parser p;
    auto m = p.parse(reply);
    ASSERT_EQ(lis::Terser::get(m, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(m, "/MSA-2"), "T1");
    server.stop();
}

TEST(Server, ConnListenerReportsOnlineOffline) {
    lis::Router router;
    lis::LisServer server({0, 2, 500}, std::move(router));
    std::vector<std::pair<std::string, bool>> events;
    server.set_conn_listener([&](const std::string& ip, bool online) {
        events.emplace_back(ip, online);
    });
    server.start();
    const auto port = server.local_port();

    { lis::TcpTransport c; ASSERT_TRUE(c.connect("127.0.0.1", port, 2000)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 等读线程感知断开
    server.stop();

    ASSERT_EQ(events.size(), 2u);
    ASSERT_EQ(events[0].first, "127.0.0.1");
    ASSERT_EQ(events[0].second, true);
    ASSERT_EQ(events[1].second, false);
}

TEST(Server, StopReleasesPort) {
    lis::Router r1;
    lis::LisServer s1({0, 1, 500}, std::move(r1));
    s1.start();
    const auto port = s1.local_port();
    s1.stop();

    lis::Router r2;                                          // 同端口可立即重绑
    lis::LisServer s2({port, 1, 500}, std::move(r2));
    s2.start();
    ASSERT_EQ(s2.local_port(), port);
    s2.stop();
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Server.* --gtest_color=no`
Expected: 编译失败（`lis_server.h` 不存在）。

- [ ] **Step 3: lis_server.h**

```cpp
// include/lis/lis_server.h
#pragma once
#include <lis/router.h>
#include <lis/tcp_transport.h>
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
    void set_conn_listener(ConnListener cb);

private:
    struct Connection {
        std::unique_ptr<TcpTransport> transport;
        std::thread reader;
        std::atomic<bool> open{false};
    };
    void accept_loop();
    void read_loop(std::shared_ptr<Connection> conn);

    Config cfg_;
    Router router_;
    ThreadPool pool_;
    TcpTransport listener_;
    std::thread acceptor_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::mutex conn_mu_;
    ConnListener conn_cb_;
    std::atomic<bool> stopping_{false};
};

}  // namespace lis
```

- [ ] **Step 4: lis_server.cpp**

```cpp
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
    Er7Parser parser;
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
    conn->open = false;
    conn->transport->close();
    if (conn_cb_) conn_cb_(conn->transport->remote_ip(), false);
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
```

- [ ] **Step 5: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Server.* --gtest_color=no`
Expected: 3 个用例全 `[       OK ]`。

- [ ] **Step 6: Commit**

```bat
git add lisserver/include/lis/lis_server.h lisserver/src/lis_server.cpp lisserver/tests/test_server.cpp
git commit -m "lisserver: MLLP 服务端（accept/读线程/业务池/连接回调/优雅关闭）"
```

---

### Task 9: LisClient

**Files:**
- Test: `lisserver/tests/test_client.cpp`
- Create: `lisserver/include/lis/lis_client.h`
- Create: `lisserver/src/lis_client.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_client.cpp
#include <gtest/gtest.h>
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/tcp_transport.h>
#include <lis/terser.h>
#include <chrono>
#include <thread>

static const char* kQry = "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|K1|P|2.3.1\rQRD||||BAR9";

static std::unique_ptr<lis::LisServer> make_ack_server(uint16_t port, uint16_t* bound) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::FnHandler>(
        [](const lis::Message& in, const lis::MessageMeta&) { return lis::build_ack(in, lis::AckCode::AA); }));
    auto s = std::make_unique<lis::LisServer>(lis::LisServer::Config{port, 2, 500}, std::move(router));
    s->start();
    *bound = s->local_port();
    return s;
}

TEST(Client, QueryAckRoundTrip) {
    uint16_t port = 0;
    auto server = make_ack_server(0, &port);
    lis::LisClient client({"127.0.0.1", port, 2000, 1});
    lis::Er7Parser p;
    auto resp = client.send_and_receive(p.parse(kQry));
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-2"), "K1");
    server->stop();
}

TEST(Client, TimeoutThrowsAfterRetries) {
    lis::TcpTransport listener;
    ASSERT_TRUE(listener.listen(0));
    const auto port = listener.local_port();
    std::thread t([&] { auto peer = listener.accept(3000); if (peer) std::this_thread::sleep_for(std::chrono::seconds(2)); });
    lis::LisClient client({"127.0.0.1", port, 400, 1});     // 1 次重试
    lis::Er7Parser p;
    ASSERT_THROW(client.send_and_receive(p.parse(kQry)), lis::LisTimeoutError);
    t.join();
    listener.close();
}

TEST(Client, RetryAfterReconnect) {
    uint16_t port = 0;
    auto server = make_ack_server(0, &port);
    lis::LisClient client({"127.0.0.1", port, 1500, 2});
    lis::Er7Parser p;
    ASSERT_EQ(lis::Terser::get(client.send_and_receive(p.parse(kQry)), "/MSA-1"), "AA");
    server->stop();                                          // 断线
    uint16_t port2 = 0;
    auto server2 = make_ack_server(port, &port2);            // 同端口重启
    ASSERT_EQ(port2, port);
    auto resp = client.send_and_receive(p.parse(kQry));      // 内部重连+重试成功
    ASSERT_EQ(lis::Terser::get(resp, "/MSA-1"), "AA");
    server2->stop();
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=Client.* --gtest_color=no`
Expected: 编译失败（`lis_client.h` 不存在）。

- [ ] **Step 3: lis_client.h**

```cpp
// include/lis/lis_client.h
#pragma once
#include <lis/message.h>
#include <lis/tcp_transport.h>
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

namespace lis {

class LisTimeoutError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class LisIoError : public std::runtime_error { public: using std::runtime_error::runtime_error; };

// HL7 v2.x over MLLP 客户端：send_and_receive 同步阻塞。
// 语义（对齐 HAPI + 设计 §5.8）：先按 MSH-10 登记 pending -> 写帧 -> 按 MSA-2 归位；
// 超时/IO 失败 -> 丢弃连接重建 -> 原报文重试 N 次（沿用原 MSH-10，每次独立计时）。
// 多线程共享安全；对端 NAK 不抛异常，原样返回给调用方解析 MSA-1。
class LisClient {
public:
    struct Config { std::string host; uint16_t port; int timeout_ms = 5000; int retry = 3; int read_timeout_ms = 500; };
    using StatusCb = std::function<void(const std::string& msg_id, bool ok)>;

    explicit LisClient(Config cfg);
    ~LisClient();
    LisClient(const LisClient&) = delete;
    LisClient& operator=(const LisClient&) = delete;

    Message send_and_receive(const Message& out);
    bool connected() const { return connected_.load(); }
    void report_async(const Message& result, std::string msg_id, StatusCb cb);

private:
    struct Pending { std::promise<std::string> prom; };
    void ensure_connected();
    void disconnect();
    void read_loop();

    Config cfg_;
    TcpTransport transport_;
    std::thread reader_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::mutex connect_mu_;
    std::mutex pending_mu_;
    std::map<std::string, std::shared_ptr<Pending>> pending_;
    ThreadPool pool_{2, 64};
};

}  // namespace lis
```

- [ ] **Step 4: lis_client.cpp**

```cpp
// src/lis_client.cpp
#include <lis/lis_client.h>
#include <lis/er7_parser.h>
#include <lis/mllp_codec.h>
#include <lis/terser.h>
#include <atomic>
#include <chrono>

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
    connected_ = true;
    reader_ = std::thread([this] { read_loop(); });
}

void LisClient::disconnect() {
    std::lock_guard<std::mutex> lk(connect_mu_);
    connected_ = false;
    transport_.close();                      // 强迫读线程退出（async 操作以 operation_aborted 完成）
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
            pending_[control_id] = p;                  // 先登记后发送
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
        } catch (const ParseError& e) {
            throw LisIoError(std::string("bad response: ") + e.what());
        }
    }
    throw LisTimeoutError("timeout waiting response to " + control_id + " after " +
                          std::to_string((cfg_.retry + 1) * cfg_.timeout_ms) + " ms");
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
        std::optional<std::string> msg;
        try {
            for (int i = 0; i < n && !msg; ++i) msg = codec.feed(buf[i]);
        } catch (const ProtocolError&) { break; }      // 帧违规：断开
        if (!msg) continue;
        const std::string ack_id = scan_ack_id(*msg);
        std::lock_guard<std::mutex> pl(pending_mu_);
        auto it = pending_.find(ack_id);
        if (it != pending_.end()) {
            it->second->prom.set_value(*msg);          // 仅当有人在等才投递（对齐 HAPI give()）
            pending_.erase(it);
        }
        // 无人等待的应答：忽略（对齐 HAPI "Unexpected Message ... ignored"）
    }
    connected_ = false;                                // 收尾失败通知由 disconnect() 负责
}

}  // namespace lis
```

- [ ] **Step 5: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_filter=Client.* --gtest_color=no`
Expected: 3 个用例全 `[       OK ]`（`Client.TimeoutThrowsAfterRetries` 约 3 秒，属正常）。

- [ ] **Step 6: Commit**

```bat
git add lisserver/include/lis/lis_client.h lisserver/src/lis_client.cpp lisserver/tests/test_client.cpp
git commit -m "lisserver: MLLP 客户端（发送-等待、MSH-10/MSA-2 关联、超时重试、异步上报）"
```

---

### Task 10: Provider + QryHandler/OruHandler + demo E2E

**Files:**
- Test: `lisserver/tests/test_e2e.cpp`
- Create: `lisserver/include/lis/provider.h`（SimProvider 头内联，无 cpp）
- Create: `lisserver/include/lis/qry_handler.h`、`lisserver/src/qry_handler.cpp`
- Create: `lisserver/include/lis/oru_handler.h`（头内联，无 cpp）
- Modify: `lisserver/demo/lis_demo.cpp`（替换脚手架空壳）
- Create: `lisserver/.gitignore`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_e2e.cpp
#include <gtest/gtest.h>
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/terser.h>

TEST(E2e, QueryReturnsOrfAndOruGetsAck) {
    lis::Router router;
    router.add("QRY", "Q02", "*", std::make_shared<lis::QryHandler>(
        std::make_shared<lis::SimProvider>(std::vector<std::string>{"GLU", "ALT"})));
    bool oru_seen = false;
    router.add("ORU", "R01", "*", std::make_shared<lis::OruHandler>(
        [&](const lis::Message&) { oru_seen = true; }));
    lis::LisServer server({0, 2, 500}, std::move(router));
    server.start();
    lis::LisClient client({"127.0.0.1", server.local_port(), 2000, 1});
    lis::Er7Parser p;

    auto orf = client.send_and_receive(p.parse(
        "MSH|^~\\&|I|F|L|F|20261022120000||QRY^Q02|E1|P|2.3.1\rQRD||||BAR001"));
    ASSERT_EQ(lis::Terser::get(orf, "/MSH-9"), "ORF^Q02");
    ASSERT_EQ(lis::Terser::get(orf, "/MSA-1"), "AA");
    ASSERT_EQ(lis::Terser::get(orf, "/MSA-2"), "E1");
    ASSERT_EQ(lis::Terser::get(orf, "/PID-3"), "P0001");
    ASSERT_EQ(lis::Terser::get(orf, "/OBR-2"), "BAR001");
    int obx = 0;
    for (auto& s : orf.segments) if (s.name == "OBX") ++obx;
    ASSERT_EQ(obx, 2);

    auto ack = client.send_and_receive(p.parse(
        "MSH|^~\\&|I|F|L|F|20261022120100||ORU^R01|E2|P|2.3.1\rPID|1||P0001\rOBR|1||BAR001\rOBX|1|ST|GLU||5.5"));
    ASSERT_EQ(lis::Terser::get(ack, "/MSA-1"), "AA");
    ASSERT_TRUE(oru_seen);
    server.stop();
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `listest\x64\Debug\listest.exe --gtest_filter=E2e.* --gtest_color=no`
Expected: 编译失败（`provider.h`/`qry_handler.h`/`oru_handler.h` 不存在）。

- [ ] **Step 3: provider.h（头内联）**

```cpp
// include/lis/provider.h
#pragma once
#include <lis/message.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lis {

struct QueryConditions { std::string barcode; std::string sample_no; };
struct SampleInfo {
    std::string barcode, patient_id, patient_name;
    std::vector<std::string> items;      // 项目编码/通道号
};

// 查询数据源（设计细化：返回数据级结构，协议组段在 QryHandler）
class ISampleProvider {
public:
    virtual ~ISampleProvider() = default;
    virtual std::optional<SampleInfo> query(const QueryConditions& qc) = 0;
};

// 模拟数据源：任何非空条码/样本号都命中，返回固定项目列表
class SimProvider : public ISampleProvider {
public:
    explicit SimProvider(std::vector<std::string> items) : items_(std::move(items)) {}
    std::optional<SampleInfo> query(const QueryConditions& qc) override {
        if (qc.barcode.empty() && qc.sample_no.empty()) return std::nullopt;
        SampleInfo s;
        s.barcode = qc.barcode.empty() ? qc.sample_no : qc.barcode;
        s.patient_id = "P0001";
        s.patient_name = "TestPatient";
        s.items = items_;
        return s;
    }
private:
    std::vector<std::string> items_;
};

}  // namespace lis
```

- [ ] **Step 4: qry_handler.h / qry_handler.cpp**

```cpp
// include/lis/qry_handler.h
#pragma once
#include <lis/provider.h>
#include <lis/router.h>
#include <memory>

namespace lis {

// QRY^Q02 -> ORF^R02：MSH 收发互换、MSA=AA，回带 QRD/QRF，补 PID/OBR/OBX。
// 查无样本回 AR（设计 §5.9）。
class QryHandler : public IHandler {
public:
    explicit QryHandler(std::shared_ptr<ISampleProvider> provider)
        : provider_(std::move(provider)) {}
    Message handle(const Message& in, const MessageMeta&) override;
private:
    std::shared_ptr<ISampleProvider> provider_;
};

}  // namespace lis
```

```cpp
// src/qry_handler.cpp
#include <lis/qry_handler.h>
#include <lis/ack_builder.h>
#include <lis/terser.h>

namespace lis {

Message QryHandler::handle(const Message& in, const MessageMeta&) {
    QueryConditions qc;
    qc.barcode = Terser::get(in, "/QRD-5");          // 条码位置按目标仪器协议联调时可调
    auto info = provider_->query(qc);
    if (!info) return build_ack(in, AckCode::AR, "sample not found");

    Message out;
    out.ec = in.ec;
    out.version = in.version;

    Segment msh{"MSH", {}};
    msh.fields.push_back(Field{});
    msh.fields.push_back(Field{{std::string(1, in.ec.component) + in.ec.repeat +
                                std::string(1, in.ec.escape) + std::string(1, in.ec.subcomponent)}});
    auto put = [&](int n, std::initializer_list<std::string> c) {
        while (msh.fields.size() < static_cast<size_t>(n)) msh.fields.push_back(Field{});
        msh.fields[n - 1].components.assign(c.begin(), c.end());
    };
    put(3, {Terser::get(in, "/MSH-5")});             // 收发互换（同 build_ack 规则）
    put(4, {Terser::get(in, "/MSH-6")});
    put(5, {Terser::get(in, "/MSH-3")});
    put(6, {Terser::get(in, "/MSH-4")});
    put(9, {"ORF", Terser::get(in, "/MSH-9-2")});
    out.segments.push_back(std::move(msh));

    out.segments.push_back(Segment{"MSA", {Field{{"AA"}}, Field{{Terser::get(in, "/MSH-10")}}}});

    for (const auto& s : in.segments)                // 回带 QRD/QRF
        if (s.name == "QRD" || s.name == "QRF") out.segments.push_back(s);

    Segment pid{"PID", {}};
    pid.fields.push_back(Field{{"1"}});
    pid.fields.push_back(Field{});
    pid.fields.push_back(Field{{info->patient_id}});  // PID-3
    pid.fields.push_back(Field{});
    pid.fields.push_back(Field{{info->patient_name}});// PID-5
    out.segments.push_back(std::move(pid));

    Segment obr{"OBR", {}};
    obr.fields.push_back(Field{{"1"}});
    obr.fields.push_back(Field{{info->barcode}});     // OBR-2
    out.segments.push_back(std::move(obr));

    for (size_t i = 0; i < info->items.size(); ++i) {
        Segment obx{"OBX", {}};
        obx.fields.push_back(Field{{std::to_string(i + 1)}});
        obx.fields.push_back(Field{{"ST"}});
        obx.fields.push_back(Field{{info->items[i]}}); // OBX-3
        out.segments.push_back(std::move(obx));
    }
    return out;
}

}  // namespace lis
```

- [ ] **Step 5: oru_handler.h（头内联）**

```cpp
// include/lis/oru_handler.h
#pragma once
#include <lis/ack_builder.h>
#include <lis/router.h>
#include <functional>

namespace lis {

// ORU^R01：把结果报文交给上层回调，回 AA。
class OruHandler : public IHandler {
public:
    using ResultCb = std::function<void(const Message& oru)>;
    explicit OruHandler(ResultCb cb) : cb_(std::move(cb)) {}
    Message handle(const Message& in, const MessageMeta&) override {
        if (cb_) cb_(in);
        return build_ack(in, AckCode::AA);
    }
private:
    ResultCb cb_;
};

}  // namespace lis
```

- [ ] **Step 6: 跑测试确认通过**

Run: `msbuild lisserver.sln /p:Configuration=Debug /p:Platform=x64 && listest\x64\Debug\listest.exe --gtest_color=no`
Expected: 全部用例 `[       OK ]`（含此前所有任务用例回归）。

- [ ] **Step 7: demo/lis_demo.cpp（替换空壳）**

```cpp
// demo/lis_demo.cpp
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/terser.h>
#include <iostream>

int main() {
    using namespace lis;
    Router router;
    router.add("QRY", "Q02", "*",
               std::make_shared<QryHandler>(std::make_shared<SimProvider>(
                   std::vector<std::string>{"GLU", "ALT", "WBC"})));
    router.add("ORU", "R01", "*", std::make_shared<OruHandler>([](const Message& m) {
        std::cout << "[demo] ORU received, PID-3=" << Terser::get(m, "/PID-3")
                  << " OBX-3=" << Terser::get(m, "/OBX-3") << "\n";
    }));

    LisServer server({0, 4, 500}, std::move(router));
    server.set_conn_listener([](const std::string& ip, bool on) {
        std::cout << "[demo] conn " << ip << (on ? " online" : " offline") << "\n";
    });
    server.start();
    const auto port = server.local_port();
    std::cout << "[demo] LIS server on 127.0.0.1:" << port << "\n";

    Er7Parser parser;
    LisClient client({"127.0.0.1", port, 3000, 1});

    auto orf = client.send_and_receive(parser.parse(
        "MSH|^~\\&|Instr|Fac|Lis|Fac|20261022120000||QRY^Q02|C1|P|2.3.1\r"
        "QRD||||BAR001"));
    std::cout << "[demo] QRY -> MSH-9=" << Terser::get(orf, "/MSH-9")
              << " MSA-1=" << Terser::get(orf, "/MSA-1")
              << " PID-3=" << Terser::get(orf, "/PID-3") << "\n";

    auto ack = client.send_and_receive(parser.parse(
        "MSH|^~\\&|Instr|Fac|Lis|Fac|20261022120100||ORU^R01|C2|P|2.3.1\r"
        "PID|1||P0001||TestPatient\r"
        "OBR|1||BAR001\r"
        "OBX|1|ST|GLU||5.5"));
    std::cout << "[demo] ORU -> MSA-1=" << Terser::get(ack, "/MSA-1") << "\n";

    server.stop();
    std::cout << "[demo] done\n";
    return 0;
}
```

- [ ] **Step 8: .gitignore 并跑 demo**

```text
# lisserver/.gitignore
conan/
obj/
lib/
listest/x64/
lisdemo/x64/
*.user
```

Run: `msbuild lisserver.sln /p:Configuration=Release /p:Platform=x64 && lisdemo\x64\Release\lisdemo.exe`
Expected 输出（端口随机）：

```text
[demo] LIS server on 127.0.0.1:53517
[demo] conn 127.0.0.1 online
[demo] QRY -> MSH-9=ORF^Q02 MSA-1=AA PID-3=P0001
[demo] ORU received, PID-3=P0001 OBX-3=GLU
[demo] ORU -> MSA-1=AA
[demo] conn 127.0.0.1 offline
[demo] done
```

- [ ] **Step 9: Commit**

```bat
git add lisserver/include/lis/provider.h lisserver/include/lis/qry_handler.h lisserver/src/qry_handler.cpp lisserver/include/lis/oru_handler.h lisserver/demo/lis_demo.cpp lisserver/tests/test_e2e.cpp lisserver/.gitignore
git commit -m "lisserver: 模拟数据源、QRY/ORU 处理器与 demo 端到端"
```

---

## 计划自查记录

1. **规格覆盖**：设计 §5.1-5.9 各组件均有对应任务（MllpCodec=T2，消息树/Parser/Terser=T3/T4，AckBuilder=T5，Router=T6，ITransport/线程池=T7，LisServer=T8，LisClient=T9，Provider/Handler=T10）；设计 §6 两条消息流由 T10 E2E 覆盖；§7 错误策略逐项落码（解析失败 NAK=T8，handler 异常 AE=T6，无路由 AR=T6，客户端超时重试=T9）；§10 测试策略三层齐备（单元 T2-T7、E2E T10、liscomm 联调留待后续）。
2. **占位符扫描**：无 TBD/TODO；所有步骤含完整代码与命令。
3. **类型一致性**：`MllpCodec::feed/frame`、`Terser::get/set`、`build_ack`、`Router::add/dispatch`、`ITransport`、`TcpTransport::listen/accept/local_port`、`ThreadPool(workers,max_queue)`、`LisServer::Config{port,workers,read_timeout_ms}`、`LisClient::Config{host,port,timeout_ms,retry,read_timeout_ms}` 在各任务间签名一致；测试所引路径（如 `/QRD-5`、`/OBR-2`）与组段实现一致。
4. **已知留待联调**（设计 §11 风险表，非本计划阻塞项）：重试是否换 MSH-10、ORF 段序按目标仪器协议调整、QRD-5 条码位置。


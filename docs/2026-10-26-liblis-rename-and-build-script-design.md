# liblis 改名、第三方依赖 vendoring 与 VS2022 编译脚本设计

日期：2026-10-26
状态：待评审
范围：lis-server 仓库（连带 RisDebug 侧 vendored 产物同步）

## 1. 背景与目标

lis-server 目前用 conan 管理 asio 与 gtest，生成的 props 里是用户缓存绝对路径
（如 `C:\Users\maccura\.conan2\p\...`），换机/换用户后构建依赖缓存状态。本次改动：

1. 静态库名 `lislib.lib` → `liblis.lib`（工程文件、解决方案、消费者依赖、文档一并改名）。
2. conan 管理的 asio/gtest 头与库 **vendor 到 `lisserver/target/x64-windows`（vcpkg 风格
   installed 布局）并入库**，工程改用手写相对路径 props，构建不再依赖 conan 缓存与生成的 props。
   与 RisDebug 的 `target/x64-windows` 同构，两个仓库统一用 target 管理第三方依赖。
3. 新增一条命令的编译脚本 `build.bat`，**默认用 VS2022（v143）**产出库，供 RisDebug 直接使用。

非目标：不改依赖版本、不迁 CMake、不改任何运行时行为、不给 RisDebug 引入 asio。

## 2. 关键约束（本机实测）

- 仓库默认工具集为 v145（`lisserver/Directory.Build.props`），v143 需显式覆盖；本机已装
  VS2022 Professional（MSVC 14.44.35207）与 VS18。
- conan 缓存的 gtest 1.14.0 二进制为 MSVC 195（VS18）构建。实测把 listest 切到 v143：

      MSBuild.exe lisserver/listest.vcxproj -p:Configuration=Debug -p:Platform=x64 -p:PlatformToolset=v143
      error LNK2019: 无法解析的外部符号 __std_rotate ...
        gmock_main.lib(gtest-all.obj) ... fatal error LNK1120

  即 v143 链接 MSVC 195 的 gtest 属"旧链新"，不受支持。
- 工具集与 MSBuild 的绑定是单向的：VS2022 的 MSBuild 构建 v145 报
  `error MSB8020: 无法找到 v145 的生成工具`；VS18 的 MSBuild 构建 v143/v145 均可（实测）。
- 依赖体积：asio 头 6.3 MB；gtest 头 1.1 MB，Debug 库 gtest.lib 9.8 MB + gtest_main.lib 0.1 MB。
- 测试只包含 `<gtest/gtest.h>`，不使用 gmock（全仓库无 gmock/EXPECT_CALL/MOCK_METHOD）。
- 结论：**本次不把仓库默认工具集改为 v143**。目录默认仍是 v145（测试/日常构建不变）；
  脚本只负责用 VS2022/v143 产出库；整解决方案（v145）由脚本调用 VS18 的 MSBuild 完成。

## 3. 改名范围

MSBuild 静态库输出名由工程文件名决定，改工程文件名即得 `liblis.lib`，无需 `TargetName`。

| 文件 | 改动 |
| --- | --- |
| `lisserver/lislib.vcxproj` | `git mv` → `lisserver/liblis.vcxproj` |
| `lisserver/lislib.vcxproj.user` | 本地忽略文件，同步改名为 `liblis.vcxproj.user`，保留本地调试设置 |
| `lisserver/lisserver.sln` | 工程显示名 `lislib` → `liblis`，工程路径指向 `liblis.vcxproj` |
| `lisserver/lisdemo.vcxproj` | `AdditionalDependencies` 中 `lislib.lib` → `liblis.lib` |
| `lisserver/listest.vcxproj` | `AdditionalDependencies` 中 `lislib.lib` → `liblis.lib` |
| `lisserver/Directory.Build.props` | 注释中的 `lislib` → `liblis`（工具集 v145 保持不变） |
| `lisserver/src/lis_placeholder.cpp` | 注释中的 `lislib` → `liblis` |
| `tools/header_probe.cpp` | 注释中的 `lislib` → `liblis` |
| `docs/**` | 既有文档文本中 `lislib` 统一改为 `liblis`（本设计文档与改写后的构建记录为说明改名背景可保留旧名） |

构建记录文档 `docs/2026-10-26-lislib-v143-static-build.md` 重命名为
`docs/2026-10-26-liblis-v143-build.md`，改写为"脚本用法 + 工具集分工 + target 依赖来源"。

副产品：中间目录由 `obj/lislib/...` 变为 `obj/liblis/...`（`Directory.Build.props` 按工程名分目录）。

## 4. 第三方依赖 vendoring（vcpkg 风格 target）

### 4.1 目录布局（入库，约 17 MB）

参考 vcpkg `installed/<triplet>` 的目录约定：头文件共用 `include`，Debug 库在 `debug/lib`，
Release 库在 `lib`，许可证在 `share/<包名>/copyright`。当前只 vendored Debug 版 gtest，
因此 `lib` 目录为空（不创建），与"Release 测试暂不支持"的现状一致。

    lisserver/target/x64-windows/
      include/asio.hpp              # asio 1.30.2 纯头
      include/asio/                 # asio 头树
      include/gtest/                # gtest 1.14.0 头
      debug/lib/gtest.lib           # MSVC 195、Debug（/MDd）
      debug/lib/gtest_main.lib
      share/asio/copyright          # Boost Software License
      share/gtest/copyright         # BSD 3-Clause

只 vendor 测试实际使用的 `gtest`/`gtest_main`，不带 gmock 的三个库与头（34 MB 中的 24 MB）；
不 vendor PDB。

### 4.2 工程接线

新增两个 props，均放 `lisserver/` 根，路径全部用 `$(MSBuildThisFileDirectory)` 拼相对路径，
无任何绝对路径：

- `lisserver/third_party.props`（由 `Directory.Build.props` 全局导入）：
  定义 `ThirdPartyRoot = $(MSBuildThisFileDirectory)target\x64-windows\`，
  把 `$(ThirdPartyRoot)include` 加入 `AdditionalIncludeDirectories`。
- `lisserver/gtest.props`（仅 `listest.vcxproj` 导入）：
  Debug 用 `$(MSBuildThisFileDirectory)target\x64-windows\debug\lib`、Release 用
  `...\target\x64-windows\lib` 作为 `AdditionalLibraryDirectories`，链接
  `gtest.lib;gtest_main.lib`；自带路径不依赖 `ThirdPartyRoot`，导入顺序无关。

配套改动：

- `lisserver/Directory.Build.props`：删除 `conan\conandeps.props` 导入，新增
  `third_party.props` 导入；保留 `ASIO_STANDALONE` 与工具集 v145。
- `lisserver/liblis.vcxproj`、`lisserver/lisdemo.vcxproj`：删除 Release 下
  `conan\conan_asio_vars_debug_x64.props` 的 ImportGroup（asio 路径改由 third_party.props 提供）。
- `lisserver/listest.vcxproj`：导入 `gtest.props`，gtest 库不再由 Directory.Build.props 隐式注入。
- `lisserver/conanfile.txt`：去掉 `[generators]` 段（不再生成 props），保留 `[requires]`
  与注释，作为依赖版本的来源记录。

### 4.3 版本刷新流程（换 asio/gtest 时）

`conan install lisserver` 把包下载到本机缓存后，用 `conan cache path <ref>` 找到包目录，
按 4.1 的 vcpkg 布局重新拷头/库/许可证，并同步更新本节与构建记录中的版本号。日常构建
**不需要** conan，也不需要 `lisserver/conan/` 下的生成文件。

### 4.4 与工具集的关系

- vendored asio 是纯头，任何工具集可用 → liblis 的 v143 构建完全脱离 conan。
- vendored gtest 是 MSVC 195 的 Debug 二进制 → 仅 v145 的 Debug 测试可用；v143 测试仍不支持
  （见 §2）。Release 测试仍缺 gtest Release 包，维持既有缺口。

## 5. `build.bat`

位置：仓库根 `build.bat`（双击或 `cmd /c build.bat` 均可）。输出信息用 ASCII，避免 cmd 代码页问题。

### 5.1 命令行契约

    build.bat                     # liblis Debug + Release（v143）
    build.bat -c Debug            # liblis 单配置（v143）
    build.bat -c Release
    build.bat -t sln              # 整解决方案 Debug（默认 v145，含 listest/lisdemo）

- `-c` 取值 `Debug|Release`（大小写不敏感）。默认：`-t lib` 时为 `all`，`-t sln` 时为 `Debug`。
- `-t` 取值 `lib|sln`，默认 `lib`。
- `-t sln -c Release`：明确拒绝（Release gtest 依赖缺位，见 §8），打印原因并退出码 2。
- 未知参数：打印用法，退出码 2。

### 5.2 工具集与 MSBuild 选择

- `-t lib`：使用 **VS2022 MSBuild**，显式传 `-p:PlatformToolset=v143`，产物写入
  `lisserver\lib\x64\<Configuration>-v143\`（以 `-p:OutDir=lib\x64\<Configuration>-v143\` 覆盖）。
  与目录默认 v145 产物（`lib\x64\<Configuration>\`）物理隔离，避免混用。
- `-t sln`：使用 **VS18 MSBuild**（目录默认工具集 v145，VS2022 MSBuild 无法构建 v145）。

### 5.3 环境检查

- MSBuild 定位（两条路径各自回退到固定安装目录）：
  - VS2022（`-t lib`）：`vswhere.exe -latest -version "[17.0,18.0)" -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`，
    回退 `%ProgramFiles%\Microsoft Visual Studio\2022\*\MSBuild\Current\Bin\MSBuild.exe`。
  - VS18（`-t sln`）：同命令但版本范围 `[18.0,19.0)`，回退
    `%ProgramFiles%\Microsoft Visual Studio\18\*\MSBuild\Current\Bin\MSBuild.exe`。
  - 对应 MSBuild 找不到时打印所需版本并退出（退出码 3）。
- vendored 依赖：要求以下文件存在，缺失时打印缺失路径与 §4.3 的刷新说明，退出码 4：
  `target\x64-windows\include\asio.hpp`、`target\x64-windows\include\gtest\gtest.h`、
  `target\x64-windows\debug\lib\gtest.lib`、`target\x64-windows\debug\lib\gtest_main.lib`。

### 5.4 产物校验与清理

- 每个配置构建结束后校验 `liblis.lib` 与 `liblis.pdb` 存在，打印路径与大小；
  MSBuild 失败则立即以退出码 1 结束。
- 全部成功后删除旧名残留 `lisserver\lib\x64\*\lislib.lib|pdb`（构建产物，均被 gitignore）。
  旧的 `Debug-v143`/`Release-v143` 目录保留，仅清掉其中的旧名文件。

退出码：0 成功；1 构建失败；2 用法错误；3 找不到所需版本的 MSBuild；4 缺 vendored 依赖。

## 6. RisDebug 侧同步（同批完成）

1. 用 `build.bat` 重编出 `liblis.lib/pdb`（Debug-v143、Release-v143）。
2. vendored 副本替换：
   - `RisDebug/target/x64-windows/lib/lislib.lib|pdb` → `liblis.lib|pdb`
   - `RisDebug/target/x64-windows/debug/lib/lislib.lib|pdb` → `liblis.lib|pdb`
   - 以 `git mv` 改名后覆盖为新产物。
3. `RisDebug/RisDebug.vcxproj` 两处 `AdditionalDependencies` 的 `lislib.lib` → `liblis.lib`。
4. `RisDebug/docs/superpowers/specs/2026-10-23-lis-server-page-design.md` 文本与路径同步改名。
   （`RisDebug/docs/superpowers/plans/` 下的计划文档是未跟踪的历史执行记录，不改。）
5. Debug 与 Release 各 `-t:Rebuild` 一次，确认链接 `liblis.lib` 成功。

## 7. 验证清单

lis-server：

1. `target` 布局完整（§4.1 的 8 项），且工程文件中无缓存绝对路径：
   `git grep -I -n "\.conan2" -- lisserver tools` 无命中；`git grep -n "conandeps" -- lisserver tools` 无命中。
   （`-I` 跳过二进制：vendored gtest 库的调试信息/`__FILE__` 断言串里带有 conan 构建路径，
   不影响链接与运行，见 §8。）
2. `build.bat` → `lib/x64/Debug-v143/liblis.lib|pdb` 与 `lib/x64/Release-v143/liblis.lib|pdb` 均存在。
3. `build.bat -c Debug` / `-c Release` 单独执行均成功。
4. `build.bat -t sln -c Debug` 成功；运行 `lisserver/listest/x64/Debug/listest.exe`，65 个用例全绿。
5. `build.bat -t sln -c Release` 被明确拒绝（退出码 2，提示 gtest Release 依赖缺位）。
6. `tools/header_probe.vcxproj`（v145）构建通过，公共头边界守卫不退化（probe 不导入
   third_party.props，asio 路径不得注入）。
7. `git grep -n lislib -- lisserver tools` 无命中（文档里的改名说明除外）。

RisDebug：

1. Debug、Release 均 `-t:Rebuild` 通过，产物时间戳更新。
2. `git grep -n lislib -- RisDebug.vcxproj target` 无命中。
3. vendored `liblis.lib` 与 lis-server 的 `-v143` 产物字节一致。

两个仓库分别提交，不推送远端。

## 8. 已知边界

- 不经脚本、直接构建 `lisserver.sln` 仍产出 v145 的 `lib/x64/<Configuration>/liblis.lib`；
  该目录与脚本产物目录隔离，vendoring 必须取自 `-v143` 目录。
- `-t sln` 依赖 VS18（v145 工具集）；只装了 VS2022 的机器可正常用默认 `-t lib`，但不能用脚本跑测试。
- Release 测试依赖（gtest Release 包）未配置，属既有缺口；脚本因此拒绝 `-t sln -c Release`。
  `target/x64-windows/lib` 因此为空，vendored 目录只覆盖 Debug。
- vendored gtest 是 MSVC 195 Debug 二进制；升级工具集或补 Release 测试时需按 §4.3 重新 vendor。
- 未 vendor gmock；以后写 mock 测试时按 §4.3 补 gmock 头与库。
- `target/x64-windows` 入库约 17 MB；`lisserver/conan/` 下的旧生成文件不再被引用，可在本机删除
  （仓库本就不跟踪该目录）。
- 改名只影响构建标识（文件/工程/产物名），不改任何符号名、命名空间或运行时行为，接入方只需改链接库名。
- vendored gtest 二进制内嵌 conan 构建路径（调试信息与 gtest 断言消息里的 `C:\Users\<user>\.conan2\p\...`），
  属构建产物自带信息；验收用 `git grep -I` 跳过二进制，如需彻底清除须自行用 `/pathmap` 重建 gtest。

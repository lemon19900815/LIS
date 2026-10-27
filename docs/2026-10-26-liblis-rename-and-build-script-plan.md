# liblis 改名、target vendoring 与 VS2022 编译脚本 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 lis-server 静态库改名为 `liblis`，把 asio/gtest 以 vcpkg 风格 vendor 到 `lisserver/target/x64-windows` 并入库，新增默认 VS2022（v143）的 `build.bat`，并同步 RisDebug 的 vendored 库与链接配置。

**Architecture:** 依赖从 conan 缓存拷入工程内 target 树，工程用两个手写相对路径 props（`third_party.props` 全局加 include，`gtest.props` 仅 listest）接线，构建不再依赖 `lisserver/conan/`。库改名靠重命名 vcxproj（MSBuild 以工程名产出 `liblis.lib`）。脚本只负责 v143 产库（VS2022 MSBuild）；整解决方案（v145 测试）走 VS18 MSBuild，因为 conan 的 gtest 二进制是 MSVC 195。

**Tech Stack:** MSBuild（VS2022 v143 / VS18 v145）、C++17、conan 2（仅作依赖来源）、Qt 5.12.8（RisDebug 侧）。

**Spec:** `docs/2026-10-26-liblis-rename-and-build-script-design.md`（lis-server，commit 75dc83c）

---

## 文件结构

lis-server 仓库（`D:\workspace\components\lis-server`，当前工作目录为 RisDebug 时用绝对路径）：

- Create: `lisserver/target/x64-windows/include/{asio.hpp,asio/,gtest/}`
- Create: `lisserver/target/x64-windows/debug/lib/{gtest.lib,gtest_main.lib}`
- Create: `lisserver/target/x64-windows/share/{asio,gtest}/copyright`
- Create: `lisserver/third_party.props`（全局 asio include）
- Create: `lisserver/gtest.props`（仅 listest：gtest 库）
- Create: `build.bat`（仓库根）
- Rename: `lisserver/lislib.vcxproj` → `lisserver/liblis.vcxproj`
- Rename: `docs/2026-10-26-lislib-v143-static-build.md` → `docs/2026-10-26-liblis-v143-build.md`（并改写内容）
- Modify: `lisserver/Directory.Build.props`、`lisserver/liblis.vcxproj`、`lisserver/lisdemo.vcxproj`、`lisserver/listest.vcxproj`、`lisserver/lisserver.sln`、`lisserver/conanfile.txt`、`lisserver/src/lis_placeholder.cpp`、`tools/header_probe.cpp`、`docs/2026-10-22-cpp-lis-server-implementation-plan.md`、`docs/2026-10-23-transport-abstraction-design.md`、`docs/2026-10-23-transport-abstraction-implementation-plan.md`

RisDebug 仓库（`D:\workspace\components\0-middleware\rebecka\RisDebug`）：

- Rename: `target/x64-windows/lib/lislib.lib|pdb` → `liblis.lib|pdb`
- Rename: `target/x64-windows/debug/lib/lislib.lib|pdb` → `liblis.lib|pdb`（内容替换为 lis-server 新产物）
- Modify: `RisDebug.vcxproj`、`docs/superpowers/specs/2026-10-23-lis-server-page-design.md`

---

## Task 1: vendored 依赖树 + 相对路径 props 接线

**Files:**
- Create: `lisserver/target/x64-windows/**`
- Create: `lisserver/third_party.props`
- Create: `lisserver/gtest.props`
- Modify: `lisserver/Directory.Build.props`
- Modify: `lisserver/lislib.vcxproj`
- Modify: `lisserver/lisdemo.vcxproj`
- Modify: `lisserver/listest.vcxproj`
- Modify: `lisserver/conanfile.txt`

- [ ] **Step 1: 从 conan 缓存拷贝 asio/gtest 到 target（vcpkg 风格布局）**

在 lis-server 仓库根执行：

```bash
mkdir -p lisserver/target/x64-windows/include \
         lisserver/target/x64-windows/debug/lib \
         lisserver/target/x64-windows/share/asio \
         lisserver/target/x64-windows/share/gtest
ASIO_PKG=$(conan cache path asio/1.30.2:da39a3ee5e6b4b0d3255bfef95601890afd80709)
GTEST_PKG=$(conan cache path gtest/1.14.0:7d050ace7e9e82c4ee9a2426096887742f164906)
cp -r "$ASIO_PKG/include/asio" "$ASIO_PKG/include/asio.hpp" lisserver/target/x64-windows/include/
cp -r "$GTEST_PKG/include/gtest" lisserver/target/x64-windows/include/
cp "$GTEST_PKG/lib/gtest.lib" "$GTEST_PKG/lib/gtest_main.lib" lisserver/target/x64-windows/debug/lib/
cp "$ASIO_PKG/licenses/LICENSE_1_0.txt" lisserver/target/x64-windows/share/asio/copyright
cp "$GTEST_PKG/licenses/LICENSE" lisserver/target/x64-windows/share/gtest/copyright
```

校验（应看到 `asio.hpp`、`asio/`、`gtest/` 三个条目；总大小约 17 MB；无 gmock）：

```bash
ls lisserver/target/x64-windows/include
ls -l lisserver/target/x64-windows/debug/lib
du -sh lisserver/target/x64-windows
```

- [ ] **Step 2: 新建 `lisserver/third_party.props`**

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- vendored 第三方依赖（vcpkg 风格 target 树）；路径全部相对本文件，不得引用 conan 缓存 -->
  <PropertyGroup>
    <ThirdPartyRoot>$(MSBuildThisFileDirectory)target\x64-windows\</ThirdPartyRoot>
  </PropertyGroup>
  <ItemDefinitionGroup>
    <ClCompile>
      <AdditionalIncludeDirectories>$(ThirdPartyRoot)include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
</Project>
```

- [ ] **Step 3: 新建 `lisserver/gtest.props`**

```xml
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <!-- 仅 listest 导入：vendored gtest（当前只有 Debug 版库） -->
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Debug' And '$(Platform)'=='x64'">
    <Link>
      <AdditionalLibraryDirectories>$(MSBuildThisFileDirectory)target\x64-windows\debug\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>gtest.lib;gtest_main.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
  <ItemDefinitionGroup Condition="'$(Configuration)'=='Release' And '$(Platform)'=='x64'">
    <Link>
      <AdditionalLibraryDirectories>$(MSBuildThisFileDirectory)target\x64-windows\lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories>
      <AdditionalDependencies>gtest.lib;gtest_main.lib;%(AdditionalDependencies)</AdditionalDependencies>
    </Link>
  </ItemDefinitionGroup>
</Project>
```

- [ ] **Step 4: 改 `lisserver/Directory.Build.props`：去掉 conan，接入 third_party.props**

把这两行：

```xml
  <!-- conan MSBuildDeps 产物（路径相对本文件，勿直接 conan install . 到别的目录） -->
  <Import Project="conan\conandeps.props" Condition="Exists('conan\conandeps.props')" />
```

替换为：

```xml
  <!-- vendored 第三方依赖（vcpkg 风格 target 树；路径相对本文件） -->
  <Import Project="third_party.props" Condition="Exists('third_party.props')" />
```

`PlatformToolset`（v145）、`ASIO_STANDALONE`、`/utf-8 /bigobj` 全部保持不变。

- [ ] **Step 5: 改 `lisserver/lislib.vcxproj`：删除两处 conan asio 接线**

删除这两块（Release 下的 conan props 导入与 include 追加）：

```xml
  <!-- Release: asio 为 header-only，include 路径与配置无关，复用 conan Debug 变量 props（不硬编码用户缓存绝对路径） -->
  <ImportGroup Condition="'$(Configuration)' == 'Release' And '$(Platform)' == 'x64'">
    <Import Project="conan\conan_asio_vars_debug_x64.props" />
  </ImportGroup>
```

```xml
  <ItemDefinitionGroup Condition="'$(Configuration)' == 'Release'">
    <ClCompile>
      <AdditionalIncludeDirectories>$(ConanasioIncludeDirectories)%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
```

- [ ] **Step 6: 改 `lisserver/lisdemo.vcxproj`：删除两处 conan asio 接线**

删除：

```xml
  <!-- Release: asio 为 header-only，include 路径与配置无关，复用 conan Debug 变量 props（不硬编码用户缓存绝对路径） -->
  <ImportGroup Condition="'$(Configuration)' == 'Release' And '$(Platform)' == 'x64'">
    <Import Project="conan\conan_asio_vars_debug_x64.props" />
  </ImportGroup>
```

```xml
  <ItemDefinitionGroup Condition="'$(Configuration)' == 'Release'">
    <ClCompile>
      <AdditionalIncludeDirectories>$(ConanasioIncludeDirectories)%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
  </ItemDefinitionGroup>
```

- [ ] **Step 7: 改 `lisserver/listest.vcxproj`：导入 gtest.props**

在 `<Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />` 之后插入：

```xml
  <Import Project="gtest.props" />
```

- [ ] **Step 8: 改 `lisserver/conanfile.txt`：不再生成 props**

全文替换为：

```ini
# 依赖已 vendor 到 target/x64-windows（vcpkg 风格布局）；本文件仅记录来源版本。
# 日常构建不需要 conan；刷新流程见 docs/2026-10-26-liblis-v143-build.md。
[requires]
asio/1.30.2
gtest/1.14.0
```

- [ ] **Step 9: 验证构建已脱离 conan**

```bash
cd /d/workspace/components/lis-server
mv lisserver/conan "$TEMP/lis-conan-backup"
MSB18="C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
MSB22="C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/MSBuild.exe"
"$MSB18" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64 -v:m -nologo
./lisserver/listest/x64/Debug/listest.exe
"$MSB22" lisserver/lislib.vcxproj -p:Configuration=Release -p:Platform=x64 \
  -p:PlatformToolset=v143 -p:OutDir=lib/x64/Release-v143/ -v:m -nologo
mv "$TEMP/lis-conan-backup" lisserver/conan
```

Expected：解决方案构建成功；listest 输出 `65 tests from ...` 且 `[  PASSED  ] 65 tests`、退出码 0；
Release 产物为 `lisserver\lib\x64\Release-v143\lislib.lib`。全程不读 `lisserver/conan/`。

- [ ] **Step 10: 提交**

```bash
git add lisserver/target lisserver/third_party.props lisserver/gtest.props \
        lisserver/Directory.Build.props lisserver/lislib.vcxproj lisserver/lisdemo.vcxproj \
        lisserver/listest.vcxproj lisserver/conanfile.txt
git commit -m "构建依赖：asio/gtest vendor 到 target/x64-windows，构建脱离 conan 缓存"
```

---

## Task 2: lislib → liblis 全量改名

**Files:**
- Rename: `lisserver/lislib.vcxproj` → `lisserver/liblis.vcxproj`
- Modify: `lisserver/lisserver.sln`、`lisserver/lisdemo.vcxproj`、`lisserver/listest.vcxproj`、
  `lisserver/Directory.Build.props`、`lisserver/src/lis_placeholder.cpp`、`tools/header_probe.cpp`、
  三个 `docs/2026-10-2*.md`

- [ ] **Step 1: 重命名工程文件（含本地 user 文件）**

```bash
cd /d/workspace/components/lis-server
git mv lisserver/lislib.vcxproj lisserver/liblis.vcxproj
[ -f lisserver/lislib.vcxproj.user ] && mv lisserver/lislib.vcxproj.user lisserver/liblis.vcxproj.user
```

- [ ] **Step 2: 改 `lisserver/lisserver.sln`**

把：

```
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "lislib", "lislib.vcxproj", "{A1B2C3D4-0001-4000-8000-000000000001}"
```

改为：

```
Project("{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}") = "liblis", "liblis.vcxproj", "{A1B2C3D4-0001-4000-8000-000000000001}"
```

- [ ] **Step 3: 改两个消费者的链接依赖**

`lisserver/lisdemo.vcxproj` 与 `lisserver/listest.vcxproj` 中：

```xml
      <AdditionalDependencies>lislib.lib;ws2_32.lib;%(AdditionalDependencies)</AdditionalDependencies>
```

改为：

```xml
      <AdditionalDependencies>liblis.lib;ws2_32.lib;%(AdditionalDependencies)</AdditionalDependencies>
```

- [ ] **Step 4: 改注释中的库名**

- `lisserver/Directory.Build.props` 第 3 行：`三工程公共编译骨架：lislib / listest / lisdemo 共享` → `三工程公共编译骨架：liblis / listest / lisdemo 共享`
- `lisserver/src/lis_placeholder.cpp`：`让 lislib 静态库在空壳阶段也能产出 lislib.lib` → `让 liblis 静态库在空壳阶段也能产出 liblis.lib`
- `tools/header_probe.cpp` 第 2 行：`接入方只含 lislib 公共头` → `接入方只含 liblis 公共头`

- [ ] **Step 5: 历史文档文本改名**

```bash
cd /d/workspace/components/lis-server
sed -i -e 's/lislib/liblis/g' \
       -e 's/2026-10-26-liblis-v143-static-build\.md/2026-10-26-liblis-v143-build.md/g' \
  docs/2026-10-22-cpp-lis-server-implementation-plan.md \
  docs/2026-10-23-transport-abstraction-design.md \
  docs/2026-10-23-transport-abstraction-implementation-plan.md
```

注意：**不要**对 `docs/2026-10-26-liblis-rename-and-build-script-design.md` 执行替换（它需要保留改名前后对照）；
构建记录文档在 Task 3 单独重命名并改写。

- [ ] **Step 6: 验证改名与构建**

```bash
cd /d/workspace/components/lis-server
git grep -n lislib -- lisserver tools ; echo "grep_exit=$?"      # 期望：无输出，grep_exit=1
MSB18="C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
MSB22="C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/MSBuild.exe"
"$MSB22" lisserver/liblis.vcxproj -p:Configuration=Debug -p:Platform=x64 \
  -p:PlatformToolset=v143 -p:OutDir=lib/x64/Debug-v143/ -v:m -nologo
"$MSB22" lisserver/liblis.vcxproj -p:Configuration=Release -p:Platform=x64 \
  -p:PlatformToolset=v143 -p:OutDir=lib/x64/Release-v143/ -v:m -nologo
"$MSB18" lisserver/lisserver.sln -p:Configuration=Debug -p:Platform=x64 -v:m -nologo
./lisserver/listest/x64/Debug/listest.exe
```

Expected：两次 v143 构建各输出 `liblis.vcxproj -> ...\lib\x64\{Debug,Release}-v143\liblis.lib`；
解决方案构建成功；65 个测试全绿。

- [ ] **Step 7: 提交**

```bash
git add lisserver/liblis.vcxproj lisserver/lisserver.sln lisserver/lisdemo.vcxproj \
        lisserver/listest.vcxproj lisserver/Directory.Build.props \
        lisserver/src/lis_placeholder.cpp tools/header_probe.cpp \
        docs/2026-10-22-cpp-lis-server-implementation-plan.md \
        docs/2026-10-23-transport-abstraction-design.md \
        docs/2026-10-23-transport-abstraction-implementation-plan.md
git commit -m "lislib 更名 liblis：工程/解决方案/消费者依赖/文档同步"
```

（`lisserver/lib`、`lisserver/obj`、`lisserver/conan` 均被 gitignore，不会误入提交。）

---

## Task 3: `build.bat` + 构建记录文档

**Files:**
- Create: `build.bat`
- Rename + rewrite: `docs/2026-10-26-lislib-v143-static-build.md` → `docs/2026-10-26-liblis-v143-build.md`

- [ ] **Step 1: 新建 `build.bat`（仓库根）**

```bat
@echo off
setlocal EnableExtensions

set "ROOT=%~dp0"
set "CFG=all"
set "TGT=lib"

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-c" goto parse_c
if /i "%~1"=="-t" goto parse_t
if /i "%~1"=="-h" goto usage
if /i "%~1"=="--help" goto usage
echo [error] unknown argument: %~1
goto usage

:parse_c
if "%~2"=="" goto usage
set "CFG=%~2"
if /i "%CFG%"=="debug" set "CFG=Debug"
if /i "%CFG%"=="release" set "CFG=Release"
if /i "%CFG%"=="all" set "CFG=all"
shift
shift
goto parse

:parse_t
if "%~2"=="" goto usage
set "TGT=%~2"
shift
shift
goto parse

:parsed
if /i "%TGT%"=="lib" goto target_lib
if /i "%TGT%"=="sln" goto target_sln
echo [error] unknown target: %TGT%
goto usage

:target_lib
if /i "%CFG%"=="all" (
  set "CONFIGS=Debug Release"
  goto check_deps
)
if /i "%CFG%"=="debug" (
  set "CONFIGS=Debug"
  goto check_deps
)
if /i "%CFG%"=="release" (
  set "CONFIGS=Release"
  goto check_deps
)
echo [error] unknown configuration: %CFG%
goto usage

:target_sln
if /i not "%CFG%"=="debug" (
  echo [error] -t sln supports Debug only: no gtest Release package is vendored
  goto usage
)
set "CONFIGS=Debug"
goto check_deps

:check_deps
if not exist "%ROOT%lisserver\target\x64-windows\include\asio.hpp" goto missing_deps
if not exist "%ROOT%lisserver\target\x64-windows\include\gtest\gtest.h" goto missing_deps
if not exist "%ROOT%lisserver\target\x64-windows\debug\lib\gtest.lib" goto missing_deps
if not exist "%ROOT%lisserver\target\x64-windows\debug\lib\gtest_main.lib" goto missing_deps
if /i "%TGT%"=="lib" goto find_vs2022
goto find_vs18

:missing_deps
echo [error] vendored dependency missing under lisserver\target\x64-windows
echo         refresh: conan install lisserver, then copy headers/libs per
echo         docs\2026-10-26-liblis-v143-build.md
exit /b 4

:find_vs2022
call :find_msbuild 17.0 18.0 2022
if not defined MSBUILD exit /b 3
for %%c in (%CONFIGS%) do (
  call :build_lib %%c
  if errorlevel 1 exit /b 1
)
call :cleanup_old_libs
exit /b 0

:find_vs18
call :find_msbuild 18.0 19.0 18
if not defined MSBUILD exit /b 3
echo [build] lisserver.sln Debug (v145, VS18, tests)
"%MSBUILD%" "%ROOT%lisserver\lisserver.sln" -p:Configuration=Debug -p:Platform=x64 -v:m -nologo
if errorlevel 1 exit /b 1
if not exist "%ROOT%lisserver\listest\x64\Debug\listest.exe" (
  echo [error] missing lisserver\listest\x64\Debug\listest.exe
  exit /b 1
)
echo [ok] %ROOT%lisserver\listest\x64\Debug\listest.exe
exit /b 0

:build_lib
echo [build] liblis %~1 (v143, VS2022)
"%MSBUILD%" "%ROOT%lisserver\liblis.vcxproj" -p:Configuration=%~1 -p:Platform=x64 -p:PlatformToolset=v143 -p:OutDir=lib\x64\%~1-v143\ -v:m -nologo
if errorlevel 1 (
  echo [error] build failed: liblis %~1
  exit /b 1
)
set "OUT=%ROOT%lisserver\lib\x64\%~1-v143"
if not exist "%OUT%\liblis.lib" (
  echo [error] missing %OUT%\liblis.lib
  exit /b 1
)
for %%f in ("%OUT%\liblis.lib" "%OUT%\liblis.pdb") do echo [ok] %%~ff  %%~zf bytes
exit /b 0

:cleanup_old_libs
for /d %%d in ("%ROOT%lisserver\lib\x64\*") do (
  if exist "%%d\lislib.lib" del /q "%%d\lislib.lib"
  if exist "%%d\lislib.pdb" del /q "%%d\lislib.pdb"
)
exit /b 0

:find_msbuild
set "MSBUILD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -version "[%~1,%~2)" -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MSBUILD=%%i"
if defined MSBUILD exit /b 0
for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\%~3\*") do (
  if exist "%%d\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD=%%d\MSBuild\Current\Bin\MSBuild.exe"
)
if defined MSBUILD exit /b 0
echo [error] MSBuild for VS %~3 not found
exit /b 1

:usage
echo Usage: build.bat [-c Debug^|Release^|all] [-t lib^|sln]
echo.
echo   build.bat              Build liblis Debug+Release with VS2022 (v143)
echo   build.bat -c Debug     Build liblis Debug only (v143)
echo   build.bat -t sln       Build lisserver.sln Debug with VS18 (v145, tests)
exit /b 2
```

- [ ] **Step 2: 验证默认构建与单配置构建**

```bash
cd /d/workspace/components/lis-server
cmd //c build.bat ; echo "exit=$?"
ls -l lisserver/lib/x64/Debug-v143/liblis.lib lisserver/lib/x64/Debug-v143/liblis.pdb \
      lisserver/lib/x64/Release-v143/liblis.lib lisserver/lib/x64/Release-v143/liblis.pdb
cmd //c "build.bat -c Debug" ; echo "exit=$?"
cmd //c "build.bat -c Release" ; echo "exit=$?"
```

Expected：每个命令 `exit=0`；四个产物存在；输出含 `[ok] ... liblis.lib  <bytes> bytes`。
旧名残留应已清理：`ls lisserver/lib/x64/*/lislib.lib` 不存在。

- [ ] **Step 3: 验证 sln 构建、参数校验与缺依赖退出码**

```bash
cd /d/workspace/components/lis-server
cmd //c "build.bat -t sln" ; echo "exit=$?"
./lisserver/listest/x64/Debug/listest.exe ; echo "exit=$?"
cmd //c "build.bat -t sln -c Release" ; echo "exit=$?"
cmd //c "build.bat -z" ; echo "exit=$?"
mv lisserver/target/x64-windows/include/asio.hpp "$TEMP/asio.hpp.bak"
cmd //c "build.bat -c Debug" ; echo "exit=$?"
mv "$TEMP/asio.hpp.bak" lisserver/target/x64-windows/include/asio.hpp
```

Expected：`-t sln` 构建成功且 listest 65 绿（exit=0）；`-t sln -c Release` 打印
`-t sln supports Debug only` 且 exit=2；`-z` 打印 `unknown argument` 且 exit=2；
缺 asio.hpp 时打印 `vendored dependency missing` 且 exit=4（最后一步恢复文件）。

- [ ] **Step 4: 重命名并改写构建记录**

```bash
git mv docs/2026-10-26-lislib-v143-static-build.md docs/2026-10-26-liblis-v143-build.md
```

把文件内容整体替换为：

```markdown
# liblis 构建（VS2022 默认）

日期：2026-10-26

## 工具集分工

- 仓库默认工具集 v145（`Directory.Build.props`）：测试与日常 sln 构建。
- RisDebug 直链的库用 v143（VS2022）：`build.bat` 默认产出，写入 `lib\x64\<Config>-v143`。
- 原因：conan 的 gtest 二进制是 MSVC 195；v143 链接会 `LNK2019 __std_rotate`（见设计文档 §2）。

## 用法

    build.bat                 # liblis Debug+Release（v143）
    build.bat -c Debug        # 单配置（v143）
    build.bat -t sln          # 整解决方案 Debug（VS18/v145，含 65 个测试）

产物：

- `lisserver\lib\x64\Debug-v143\liblis.lib`（/MDd）+ `liblis.pdb`
- `lisserver\lib\x64\Release-v143\liblis.lib`（/MD）+ `liblis.pdb`

脚本退出码：0 成功；1 构建失败；2 用法错误；3 找不到所需版本的 MSBuild；4 缺 vendored 依赖。

## 第三方依赖

- vendored 在 `lisserver\target\x64-windows`（vcpkg 风格）：asio 1.30.2（头）、gtest 1.14.0（头 + Debug 库）。
- 工程通过 `third_party.props`（asio include，全局）与 `gtest.props`（gtest 库，仅 listest）以相对路径引用；
  构建不需要 conan，也不需要 `lisserver\conan\` 下的生成文件。
- 刷新：`conan install lisserver` → `conan cache path <ref>` → 按上述布局拷头/库/许可证，更新版本号。
- 公共头边界守卫：`tools\header_probe.vcxproj`（v145）不导入 `third_party.props`，asio 不得可见。
```

- [ ] **Step 5: 提交**

```bash
git add build.bat docs/2026-10-26-liblis-v143-build.md
git commit -m "构建脚本：build.bat 默认 VS2022 产出 liblis，补构建记录"
```

---

## Task 4: RisDebug 同步（独立仓库提交）

**Files:**
- Rename + replace: `RisDebug/target/x64-windows/lib/lislib.lib|pdb`、`RisDebug/target/x64-windows/debug/lib/lislib.lib|pdb`
- Modify: `RisDebug/RisDebug.vcxproj`
- Modify: `RisDebug/docs/superpowers/specs/2026-10-23-lis-server-page-design.md`

- [ ] **Step 1: 改名 vendored 库并覆盖为新产物**

```bash
cd /d/workspace/components/0-middleware/rebecka/RisDebug
git mv target/x64-windows/lib/lislib.lib          target/x64-windows/lib/liblis.lib
git mv target/x64-windows/lib/lislib.pdb          target/x64-windows/lib/liblis.pdb
git mv target/x64-windows/debug/lib/lislib.lib    target/x64-windows/debug/lib/liblis.lib
git mv target/x64-windows/debug/lib/lislib.pdb    target/x64-windows/debug/lib/liblis.pdb
cp /d/workspace/components/lis-server/lisserver/lib/x64/Release-v143/liblis.lib target/x64-windows/lib/liblis.lib
cp /d/workspace/components/lis-server/lisserver/lib/x64/Release-v143/liblis.pdb target/x64-windows/lib/liblis.pdb
cp /d/workspace/components/lis-server/lisserver/lib/x64/Debug-v143/liblis.lib    target/x64-windows/debug/lib/liblis.lib
cp /d/workspace/components/lis-server/lisserver/lib/x64/Debug-v143/liblis.pdb    target/x64-windows/debug/lib/liblis.pdb
cmp target/x64-windows/lib/liblis.lib          /d/workspace/components/lis-server/lisserver/lib/x64/Release-v143/liblis.lib
cmp target/x64-windows/debug/lib/liblis.lib    /d/workspace/components/lis-server/lisserver/lib/x64/Debug-v143/liblis.lib
```

Expected：两个 `cmp` 均无输出（字节一致）。

- [ ] **Step 2: 改 `RisDebug.vcxproj` 的两处链接依赖**

Debug 与 Release 的 `AdditionalDependencies` 中 `lislib.lib` → `liblis.lib`：

```xml
      <AdditionalDependencies>zlibd.lib;libthrift.lib;mlinebus.lib;liscomm.lib;liblis.lib;%(AdditionalDependencies)</AdditionalDependencies>
```

```xml
      <AdditionalDependencies>zlib.lib;libthrift.lib;mlinebus.lib;liscomm.lib;liblis.lib;$(CoreLibraryDependencies);%(AdditionalDependencies)</AdditionalDependencies>
```

- [ ] **Step 3: 改 RisDebug 的 spec 文档文本**

```bash
cd /d/workspace/components/0-middleware/rebecka/RisDebug
sed -i 's/lislib/liblis/g' docs/superpowers/specs/2026-10-23-lis-server-page-design.md
git grep -n lislib -- RisDebug.vcxproj target docs/superpowers/specs ; echo "grep_exit=$?"
```

Expected：无输出，`grep_exit=1`。

- [ ] **Step 4: Debug 与 Release 各 Rebuild 一次**

```bash
cd /d/workspace/components/0-middleware/rebecka/RisDebug
MSB22="C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/MSBuild.exe"
"$MSB22" RisDebug.vcxproj -t:Rebuild -p:Configuration=Debug -p:Platform=x64 "-p:QtMsBuild=$LOCALAPPDATA/QtMsBuild" -v:m -nologo
"$MSB22" RisDebug.vcxproj -t:Rebuild -p:Configuration=Release -p:Platform=x64 "-p:QtMsBuild=$LOCALAPPDATA/QtMsBuild" -v:m -nologo
ls -l RisDebug/x64/Debug/RisDebug.exe RisDebug/x64/Release/RisDebug.exe
```

Expected：两次构建 `0 Error(s)`；两个 exe 时间戳为本次构建时间。

- [ ] **Step 5: 提交（排除本地 user 文件与构建产物）**

```bash
git add RisDebug/target/x64-windows/lib/liblis.lib RisDebug/target/x64-windows/lib/liblis.pdb \
        RisDebug/target/x64-windows/debug/lib/liblis.lib RisDebug/target/x64-windows/debug/lib/liblis.pdb \
        RisDebug/RisDebug.vcxproj RisDebug/docs/superpowers/specs/2026-10-23-lis-server-page-design.md
git status --short
git commit -m "依赖：lislib 更名 liblis，同步 vendored 库与链接配置"
```

`git status` 中若只剩 `RisDebug.vcxproj.user`（构建时间戳）与既有未跟踪产物，属预期，不提交。

---

## Task 5: 终验（spec §7 全量清单）

**Files:** 无改动；只做验证。

- [ ] **Step 1: lis-server 依赖与路径检查**

```bash
cd /d/workspace/components/lis-server
git grep -n "\.conan2" -- lisserver tools ; echo "conan_path_exit=$?"      # 期望 1
git grep -n "conandeps" -- lisserver tools ; echo "conandeps_exit=$?"      # 期望 1
git grep -n lislib -- lisserver tools ; echo "lislib_exit=$?"              # 期望 1
git status --short
```

Expected：三个 grep 均无输出且 exit=1；`git status` 干净（构建输出均被忽略）。

- [ ] **Step 2: lis-server 构建矩阵复跑**

```bash
cd /d/workspace/components/lis-server
cmd //c build.bat ; echo "exit=$?"                          # 0，双配置产物
cmd //c "build.bat -t sln" ; echo "exit=$?"                 # 0
./lisserver/listest/x64/Debug/listest.exe ; echo "exit=$?"  # 0，65 绿
cmd //c "build.bat -t sln -c Release" ; echo "exit=$?"      # 2
MSB18="C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
"$MSB18" tools/header_probe.vcxproj -p:Configuration=Debug -p:Platform=x64 -v:m -nologo
echo "probe_exit=$?"                                        # 0（asio 不可见守卫通过）
```

- [ ] **Step 3: RisDebug 复跑**

```bash
cd /d/workspace/components/0-middleware/rebecka/RisDebug
git grep -n lislib -- RisDebug.vcxproj target ; echo "grep_exit=$?"         # 1
MSB22="C:/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/MSBuild.exe"
"$MSB22" RisDebug.vcxproj -t:Rebuild -p:Configuration=Debug -p:Platform=x64 "-p:QtMsBuild=$LOCALAPPDATA/QtMsBuild" -v:m -nologo
"$MSB22" RisDebug.vcxproj -t:Rebuild -p:Configuration=Release -p:Platform=x64 "-p:QtMsBuild=$LOCALAPPDATA/QtMsBuild" -v:m -nologo
```

- [ ] **Step 4: 汇报**

汇总：两个仓库的提交号、`build.bat` 产物路径与大小、65 测试结果、RisDebug 两个 exe 路径与时间戳。

---

## 说明

- `lisserver/lib`、`lisserver/obj`、`lisserver/conan`、`tools/obj` 均被 gitignore；构建产物不入库。
- 本计划只改构建标识与依赖来源，不改任何符号名、命名空间或运行时行为。
- 两个仓库各自提交，不推送远端。

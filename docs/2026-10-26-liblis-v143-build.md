# liblis 构建（VS2022 默认）

日期：2026-10-26

## 前置条件

- `-t lib` 需要 VS2022（MSBuild 17.x，v143 工具集）；`-t sln` 需要 VS18（MSBuild 18.x，v145 工具集），
  只装了 VS2022 的机器跑不了测试（退出码 3）。
- 给 RisDebug 的库只能取 `lisserver\lib\x64\<Config>-v143\liblis.lib`；`-t sln` 会另外产出
  v145 的 `lisserver\lib\x64\Debug\liblis.lib`，**不可**用于 RisDebug（工具集 ABI 不匹配）。

## 工具集分工

- 仓库默认工具集 v145（`Directory.Build.props`）：测试与日常 sln 构建。
- RisDebug 直链的库用 v143（VS2022）：`build.bat` 默认产出，写入 `lib\x64\<Config>-v143`，
  中间目录也隔离在 `obj\liblis\x64\<Config>-v143`。
- 原因：vendored gtest 二进制必须保持 MSVC 195，否则 v143 链接会 `LNK2019 __std_rotate`
  （见 docs/2026-10-26-liblis-rename-and-build-script-design.md §2）。

## 用法

    build.bat                 # liblis Debug+Release（v143）
    build.bat -c Debug        # 单配置（v143）
    build.bat -t sln          # 整解决方案 Debug（VS18/v145，构建 listest，用例需手动运行）

产物：

- `lisserver\lib\x64\Debug-v143\liblis.lib`（/MDd）+ `liblis.pdb`
- `lisserver\lib\x64\Release-v143\liblis.lib`（/MD）+ `liblis.pdb`

脚本退出码：0 成功；1 构建失败；2 用法错误；3 找不到所需版本的 MSBuild；4 缺 vendored 依赖。
构建成功后会删除 `lisserver\lib\x64\*\lislib.lib|pdb` 旧名残留与 `obj\lislib` 旧中间目录（仅 `-t lib` 构建成功后执行）。

## 第三方依赖

- vendored 在 `lisserver\target\x64-windows`（vcpkg 风格）：asio 1.30.2（头）、gtest 1.14.0（头 + Debug 库）。
- 工程通过 `third_party.props`（asio include，全局）与 `gtest.props`（gtest 库，仅 listest）以相对路径引用；
  构建不需要 conan，也不需要 `lisserver\conan\` 下的生成文件。
- 刷新：`conan install lisserver` → `conan cache path <ref>` → 按上述布局拷头/库/许可证，更新版本号。
- vendored gtest 二进制内嵌 conan 构建路径（调试信息与断言消息），不影响构建；验收扫描用 `git grep -I`（`-I` 跳过二进制文件）。
- 公共头边界守卫：`tools\header_probe.vcxproj`（v145）不导入 `third_party.props`，asio 不得可见。
- 公共头目录 `lisserver\include\lis` 只放对消费者可见的头（14 个）；库内实现头
  `tcp_transport.h`（含 asio）、`mllp_codec.h` 位于 `lisserver\src`，不随 `target` 分发。

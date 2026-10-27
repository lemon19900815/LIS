# liblis 构建（VS2022 默认）

日期：2026-10-26

## 工具集分工

- 仓库默认工具集 v145（`Directory.Build.props`）：测试与日常 sln 构建。
- RisDebug 直链的库用 v143（VS2022）：`build.bat` 默认产出，写入 `lib\x64\<Config>-v143`，
  中间目录也隔离在 `obj\liblis\x64\<Config>-v143`。
- 原因：conan 的 gtest 二进制是 MSVC 195；v143 链接会 `LNK2019 __std_rotate`（见设计文档 §2）。

## 用法

    build.bat                 # liblis Debug+Release（v143）
    build.bat -c Debug        # 单配置（v143）
    build.bat -t sln          # 整解决方案 Debug（VS18/v145，含 65 个测试）

产物：

- `lisserver\lib\x64\Debug-v143\liblis.lib`（/MDd）+ `liblis.pdb`
- `lisserver\lib\x64\Release-v143\liblis.lib`（/MD）+ `liblis.pdb`

脚本退出码：0 成功；1 构建失败；2 用法错误；3 找不到所需版本的 MSBuild；4 缺 vendored 依赖。
构建成功后会删除 `lib\x64\*\lislib.lib|pdb` 旧名残留与 `obj\lislib` 旧中间目录。

## 第三方依赖

- vendored 在 `lisserver\target\x64-windows`（vcpkg 风格）：asio 1.30.2（头）、gtest 1.14.0（头 + Debug 库）。
- 工程通过 `third_party.props`（asio include，全局）与 `gtest.props`（gtest 库，仅 listest）以相对路径引用；
  构建不需要 conan，也不需要 `lisserver\conan\` 下的生成文件。
- 刷新：`conan install lisserver` → `conan cache path <ref>` → 按上述布局拷头/库/许可证，更新版本号。
- vendored gtest 二进制内嵌 conan 构建路径（调试信息与断言消息），不影响构建；验收扫描用 `git grep -I`。
- 公共头边界守卫：`tools\header_probe.vcxproj`（v145）不导入 `third_party.props`，asio 不得可见。

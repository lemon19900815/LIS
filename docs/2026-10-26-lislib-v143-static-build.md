# lislib v143 静态库构建（供 RisDebug 直链）

日期：2026-10-26

## 为什么需要

RisDebug 用 v143（VS2022）构建，需要直接链接 lislib 静态库。按微软 C++ binary
compatibility 规则，链接器只能消费同版本或更旧工具集的输入；lislib 默认 v145（VS18）
产物不能给 v143 使用。因此额外产出 v143 静态库；日常开发与 65 个用例测试仍在 v145 下进行。

## 命令（仓库根目录执行）

    msbuild lisserver/lislib.vcxproj -p:Configuration=Debug   -p:Platform=x64 -p:PlatformToolset=v143 -p:OutDir=lib\x64\Debug-v143\
    msbuild lisserver/lislib.vcxproj -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v143 -p:OutDir=lib\x64\Release-v143\

产物：

- lisserver\lib\x64\Debug-v143\lislib.lib（/MDd，配 Debug）
- lisserver\lib\x64\Release-v143\lislib.lib（/MD，配 Release）

`lisserver\lib\` 已 gitignore，产物不入库。asio 头来自既有 conan props，与工具集无关；
接入方只需要 `lisserver\include` 公共头 + 对应 lib，不需要 asio 路径。

# lislib v143 静态库构建（供 RisDebug 直链）

日期：2026-10-26

## 为什么需要

RisDebug 用 v143（VS2022）构建，需要直接链接 lislib 静态库。按微软 C++ binary
compatibility 规则，链接器只能消费同版本或更旧工具集的输入；lislib 默认 v145（VS18）
产物不能给 v143 使用。因此额外产出 v143 静态库；日常开发与 65 个用例测试仍在 v145 下进行。

## 命令（仓库根目录、Git Bash 执行）

asio 头由 `lisserver/conan/` 下 conan install 生成的 props 提供（该目录被 gitignore），换机需先执行 conan install。

    MSB="C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
    "$MSB" lisserver/lislib.vcxproj -p:Configuration=Debug   -p:Platform=x64 -p:PlatformToolset=v143 -p:OutDir=lib/x64/Debug-v143/ -v:m -nologo
    "$MSB" lisserver/lislib.vcxproj -p:Configuration=Release -p:Platform=x64 -p:PlatformToolset=v143 -p:OutDir=lib/x64/Release-v143/ -v:m -nologo

产物：

- lisserver\lib\x64\Debug-v143\lislib.lib（/MDd，配 Debug；同目录有 lislib.pdb）
- lisserver\lib\x64\Release-v143\lislib.lib（/MD，配 Release；同目录有 lislib.pdb）

`lisserver\lib\` 已 gitignore，产物不入库。asio 头来自 conan props，与工具集无关；
RisDebug 用到的公共头（`lis_server.h`、`qry_handler.h` 等）不引用 asio，接入时只需
`lisserver\include` + 对应 lib；`tcp_transport.h` 等传输实现头仍引用 asio，接入方不要包含。

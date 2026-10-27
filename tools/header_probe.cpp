// tools/header_probe.cpp
// 公共头边界守卫：编译通过 = 接入方只含 liblis 公共头即可使用（无需 asio）。
// 注意：库内实现头（tcp_transport.h、mllp_codec.h）已移入 src\，不在公共目录；接入方经
//       ITransport/IListener/ILisServer 等抽象消费。
// 维护约定：新增公共头必须同步加入下方清单，否则守卫范围静默缩水。
#if __has_include(<asio.hpp>)
#error "header_probe: asio 可见——隔离前提被破坏（工程被移入 lisserver 子树/祖先 props/INCLUDE 污染），本守卫已失效"
#endif
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/listener.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/message.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/response_builder.h>
#include <lis/router.h>
#include <lis/terser.h>
#include <lis/thread_pool.h>
#include <lis/transport.h>

int main() { return 0; }

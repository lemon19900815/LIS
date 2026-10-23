// tools/header_probe.cpp
// 公共头边界守卫：编译通过 = 接入方只含 lislib 公共头即可使用（无需 asio）。
// 注意：不含 <lis/tcp_transport.h>——它是库内实现头（含 asio），接入方经 ITransport/IListener 消费。
#include <lis/ack_builder.h>
#include <lis/er7_parser.h>
#include <lis/listener.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/message.h>
#include <lis/mllp_codec.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/router.h>
#include <lis/terser.h>
#include <lis/thread_pool.h>
#include <lis/transport.h>

int main() { return 0; }

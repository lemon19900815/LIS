// demo/lis_demo.cpp
#include <lis/er7_parser.h>
#include <lis/lis_client.h>
#include <lis/lis_server.h>
#include <lis/oru_handler.h>
#include <lis/provider.h>
#include <lis/qry_handler.h>
#include <lis/terser.h>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

// 验收工具模式：连本机指定端口，依次发四条报文（Q01/Q02 通配、空条码 AR、ORU 结果）。
int run_client(uint16_t port) {
    using namespace lis;
    Er7Parser parser;
    LisClient client({"127.0.0.1", port, 3000, 1});
    struct Case { const char* name; const char* raw; };
    const Case cases[] = {
        {"QRY^Q01 BAR001",
         "MSH|^~\\&|I1000|I1000|LIS|LIS|20261026120001||QRY^Q01|E1|P|2.3.1\r"
         "QRD|||||BAR001"},
        {"QRY^Q02 BAR001",
         "MSH|^~\\&|I1000|I1000|LIS|LIS|20261026120002||QRY^Q02|E2|P|2.3.1\r"
         "QRD|||||BAR001"},
        {"QRY^Q01 empty",
         "MSH|^~\\&|I1000|I1000|LIS|LIS|20261026120003||QRY^Q01|E3|P|2.3.1\r"
         "QRD|||||"},
        {"ORU^R01 BAR001",
         "MSH|^~\\&|I1000|I1000|LIS|LIS|20261026120004||ORU^R01|E4|P|2.3.1\r"
         "PID|1||P0001||TestPatient\r"
         "OBR|1||BAR001\r"
         "OBX|1|NM|GLU||5.5|mmol/L||N||F\r"
         "OBX|2|NM|ALT||32.0|U/L||N||F"},
    };
    for (const auto& item : cases) {
        try {
            const auto reply = client.send_and_receive(parser.parse(item.raw));
            std::cout << "[client] " << item.name
                      << " -> MSH-9=" << Terser::get(reply, "/MSH-9")
                      << " MSA-1=" << Terser::get(reply, "/MSA-1") << "\n";
        } catch (const std::exception& e) {
            std::cout << "[client] " << item.name << " -> error: " << e.what() << "\n";
            return 1;
        }
    }
    std::cout << "[client] 按回车断开连接..." << std::flush;
    std::cin.get();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lis;
    if (argc == 3 && std::string(argv[1]) == "--connect") {
        return run_client(static_cast<uint16_t>(std::atoi(argv[2])));
    }

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
        "QRD|||||BAR001"));
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

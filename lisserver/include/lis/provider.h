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

#pragma once
#include "master_agent/runtime/reference_runtime.h"

namespace master_agent::reference::detail {
struct StoredRequest {
    Turn turn;
    std::string state;
    std::string tool;
    Json arguments;
    Result<Reply> result;
};
struct Snapshot {
    std::vector<StoredRequest> requests;
    std::map<std::string, std::vector<Json>> histories;
};

class DurableStore {
public:
    DurableStore();
    ~DurableStore();
    Status open(const std::string& path);
    Result<Snapshot> load(const Limits& limits);
    Status begin(const Turn& turn);
    Status dispatch(const Turn& turn, const std::string& tool, const Json& arguments);
    Status complete(const Turn& turn, const Result<Reply>& result,
                    const std::vector<Json>& history, const std::string& note = "");
    Status clear(const std::string& session_id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace master_agent::reference::detail

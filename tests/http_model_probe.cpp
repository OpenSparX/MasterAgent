#include "master_agent/runtime/http_model.h"
#include <iostream>
using namespace master_agent::reference;
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    HttpModelConfig config; config.endpoint = argv[1]; config.timeout_ms = 50;
    auto result = makeHttpModel(config)(Json::array({{{"role", "user"}, {"content", argv[2]}}}));
    if (!result) { std::cout << result.error->code << '\n'; return 1; }
    std::cout << *result << '\n';
    return 0;
}

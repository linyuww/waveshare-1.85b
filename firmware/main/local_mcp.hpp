#pragma once
#include <functional>
#include <string>
#include "cJSON.h"

namespace local_mcp {
using Invoke = std::function<bool(const char *, const cJSON *, std::string &)>;
cJSON *dispatch(const cJSON *request, const char *tools_json, const Invoke &invoke);
}

#include "local_mcp.hpp"
#include <cmath>
#include <cstring>

namespace local_mcp {
namespace {
cJSON *error(cJSON *response, int code, const char *message)
{
    auto *value = cJSON_AddObjectToObject(response, "error");
    cJSON_AddNumberToObject(value, "code", code);
    cJSON_AddStringToObject(value, "message", message);
    return response;
}
bool valid(const cJSON *arguments, const cJSON *schema)
{
    if (arguments && !cJSON_IsObject(arguments)) return false;
    const auto *properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    const auto *required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, required) {
        if (!cJSON_GetObjectItemCaseSensitive(arguments, item->valuestring)) return false;
    }
    cJSON_ArrayForEach(item, arguments) {
        const auto *definition = cJSON_GetObjectItemCaseSensitive(properties, item->string);
        if (!definition) return false;
        const auto *type = cJSON_GetObjectItemCaseSensitive(definition, "type");
        if (!cJSON_IsString(type)) return false;
        if (strcmp(type->valuestring, "integer") == 0) {
            if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || floor(item->valuedouble) != item->valuedouble) return false;
            const auto *minimum = cJSON_GetObjectItemCaseSensitive(definition, "minimum");
            const auto *maximum = cJSON_GetObjectItemCaseSensitive(definition, "maximum");
            if ((minimum && item->valuedouble < minimum->valuedouble) || (maximum && item->valuedouble > maximum->valuedouble)) return false;
        } else if (strcmp(type->valuestring, "string") == 0) {
            if (!cJSON_IsString(item)) return false;
            const size_t length = strlen(item->valuestring);
            const auto *minimum = cJSON_GetObjectItemCaseSensitive(definition, "minLength");
            const auto *maximum = cJSON_GetObjectItemCaseSensitive(definition, "maxLength");
            if ((minimum && length < minimum->valuedouble) || (maximum && length > maximum->valuedouble)) return false;
            const auto *options = cJSON_GetObjectItemCaseSensitive(definition, "enum");
            if (options) {
                bool found = false;
                const cJSON *option = nullptr;
                cJSON_ArrayForEach(option, options) found |= strcmp(option->valuestring, item->valuestring) == 0;
                if (!found) return false;
            }
        } else return false;
    }
    return true;
}
}

cJSON *dispatch(const cJSON *request, const char *tools_json, const Invoke &invoke)
{
    const auto *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    const auto *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    const auto *version = cJSON_GetObjectItemCaseSensitive(request, "jsonrpc");
    const bool is_valid = cJSON_IsObject(request) && cJSON_IsString(method) &&
        cJSON_IsString(version) && strcmp(version->valuestring, "2.0") == 0 &&
        (!id || cJSON_IsString(id) || cJSON_IsNumber(id) || cJSON_IsNull(id));
    if (is_valid && !id) return nullptr;
    auto *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "jsonrpc", "2.0");
    cJSON_AddItemToObject(response, "id", id && is_valid ? cJSON_Duplicate(id, true) : cJSON_CreateNull());
    if (!is_valid) return error(response, -32600, "Invalid Request");
    const auto *params = cJSON_GetObjectItemCaseSensitive(request, "params");
    if (strcmp(method->valuestring, "initialize") == 0) {
        auto *result = cJSON_AddObjectToObject(response, "result");
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
        cJSON_AddObjectToObject(cJSON_AddObjectToObject(result, "capabilities"), "tools");
        auto *info = cJSON_AddObjectToObject(result, "serverInfo");
        cJSON_AddStringToObject(info, "name", "waveshare-local-tools");
        cJSON_AddStringToObject(info, "version", "1.0.0");
        return response;
    }
    if (strcmp(method->valuestring, "ping") == 0) {
        cJSON_AddObjectToObject(response, "result");
        return response;
    }
    auto *tools = cJSON_Parse(tools_json);
    if (!cJSON_IsArray(tools)) {
        cJSON_Delete(tools);
        return error(response, -32603, "Tool catalog unavailable");
    }
    if (strcmp(method->valuestring, "tools/list") == 0) {
        if (params && (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 0)) {
            cJSON_Delete(tools);
            return error(response, -32602, "Unsupported list parameters");
        }
        cJSON_AddItemToObject(cJSON_AddObjectToObject(response, "result"), "tools", tools);
        return response;
    }
    if (strcmp(method->valuestring, "tools/call") != 0) {
        cJSON_Delete(tools);
        return error(response, -32601, "Method not found");
    }
    const auto *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    const auto *arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    const cJSON *selected = nullptr;
    const cJSON *tool = nullptr;
    cJSON_ArrayForEach(tool, tools) {
        if (cJSON_IsString(name) && strcmp(cJSON_GetObjectItemCaseSensitive(tool, "name")->valuestring, name->valuestring) == 0) selected = tool;
    }
    if (!selected || !valid(arguments, cJSON_GetObjectItemCaseSensitive(selected, "inputSchema"))) {
        cJSON_Delete(tools);
        return error(response, -32602, "Unknown tool or invalid arguments");
    }
    std::string message;
    const bool success = invoke(name->valuestring, arguments, message);
    cJSON_Delete(tools);
    auto *result = cJSON_AddObjectToObject(response, "result");
    cJSON_AddBoolToObject(result, "isError", !success);
    auto *content = cJSON_AddArrayToObject(result, "content");
    auto *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "type", "text");
    cJSON_AddStringToObject(entry, "text", message.c_str());
    cJSON_AddItemToArray(content, entry);
    return response;
}
}

#include "local_mcp.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int count, char **arguments)
{
    assert(count == 2);
    std::ifstream file(arguments[1]);
    const std::string catalog((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    unsigned calls = 0;
    bool succeeded = true;
    std::string last;
    auto dispatch = [&](const char *text) {
        auto *request = cJSON_Parse(text);
        auto *response = local_mcp::dispatch(request, catalog.c_str(), [&](const char *name, const cJSON *, std::string &message) {
            ++calls;
            last = name;
            message = succeeded ? "accepted" : "busy";
            return succeeded;
        });
        cJSON_Delete(request);
        return response;
    };
    auto *response = dispatch(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "capabilities"));
    cJSON_Delete(response);
    response = dispatch(R"({"jsonrpc":"2.0","id":"list","method":"tools/list"})");
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "tools")) == 4);
    assert(catalog.find("self.music.") == std::string::npos);
    assert(std::string(cJSON_GetObjectItem(response, "id")->valuestring) == "list");
    cJSON_Delete(response);
    assert(!dispatch(R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
    assert(!dispatch(R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"self.codex.open"}})"));
    assert(calls == 0);
    auto *tools = cJSON_Parse(catalog.c_str());
    const cJSON *tool = nullptr;
    cJSON_ArrayForEach(tool, tools) {
        const auto *name = cJSON_GetObjectItem(tool, "name");
        auto *request = cJSON_CreateObject();
        cJSON_AddStringToObject(request, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(request, "id", 0);
        cJSON_AddStringToObject(request, "method", "tools/call");
        auto *params = cJSON_AddObjectToObject(request, "params");
        cJSON_AddStringToObject(params, "name", name->valuestring);
        auto *values = cJSON_AddObjectToObject(params, "arguments");
        if (std::string(name->valuestring) == "self.apps.open") cJSON_AddStringToObject(values, "app", "codex");
        if (std::string(name->valuestring) == "self.audio.set_volume") cJSON_AddNumberToObject(values, "volume", 100);
        char *text = cJSON_PrintUnformatted(request);
        response = dispatch(text);
        assert(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "isError")));
        assert(last == name->valuestring);
        free(text);
        cJSON_Delete(request);
        cJSON_Delete(response);
    }
    cJSON_Delete(tools);
    assert(calls == 4);
    for (const char *text : {
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.music.play","arguments":{}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.music.open","arguments":{}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.music.pause","arguments":{}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.music.stop","arguments":{}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.apps.open","arguments":{}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.apps.open","arguments":{"app":42}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.apps.open","arguments":{"app":"music"}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.audio.set_volume","arguments":{"volume":101}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.audio.set_volume","arguments":{"volume":-1}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.audio.set_volume","arguments":{"volume":3.5}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.apps.open","arguments":{"app":"shell"}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.codex.open","arguments":{"command":"format"}}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.codex.open","arguments":[]}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"unknown"}})",
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"cursor":"123"}})"
    }) {
        response = dispatch(text);
        assert(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "code")->valueint == -32602);
        cJSON_Delete(response);
    }
    assert(calls == 4);
    succeeded = false;
    response = dispatch(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"self.codex.open"}})");
    assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "result"), "isError")));
    cJSON_Delete(response);
    response = dispatch(R"({"id":1,"method":"tools/list"})");
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "code")->valueint == -32600);
    cJSON_Delete(response);
    response = dispatch(R"({"jsonrpc":"2.0","id":1,"method":"unknown"})");
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(response, "error"), "code")->valueint == -32601);
    cJSON_Delete(response);
    puts("PASS: four tool schemas, disabled music tools, validation, notifications and execution failures");
}

#include "http_helpers.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using namespace audio_http;
    assert(validUrl("https://example.com/music"));
    assert(validUrl("ws://192.168.1.2/voice", true));
    assert(!validUrl("https://") && !validUrl("file:///music") && !validUrl("https://host\r\nInjected: header"));
    assert(encode("A B&+") == "A%20B%26%2B");
    StreamHeaders headers;
    esp_http_client_event_t header = {HTTP_EVENT_ON_HEADER, &headers, nullptr, 0, "content-type", "Audio/PCM;rate=16000"};
    assert(streamEvent(&header) == ESP_OK && rawPcm(headers.content_type));
    assert(rawPcm(" \taudio/x-raw \t; rate=16000"));
    assert(rawPcm("application/octet-stream"));
    assert(!rawPcm("") && !rawPcm("audio/mpeg") && !rawPcm("not-audio/pcm") && !rawPcm("audio/pcm-fake"));
    header.header_key = "Content-Length";
    header.header_value = "123";
    assert(streamEvent(&header) == ESP_OK && rawPcm(headers.content_type));
    const std::string oversized(257, 'x');
    header.header_key = "CONTENT-TYPE";
    header.header_value = oversized.c_str();
    assert(streamEvent(&header) == ESP_OK && !rawPcm(headers.content_type));
    Response response;
    char body[] = "{}";
    esp_http_client_event_t data = {HTTP_EVENT_ON_DATA, &response, body, 2, nullptr, nullptr};
    assert(event(&data) == ESP_OK && response.body == "{}");
    response.body.resize(16384);
    assert(event(&data) == ESP_ERR_INVALID_SIZE && response.overflow && response.body.size() == 16384);
    puts("PASS: HTTP response headers, PCM MIME checks, bounded JSON, URL validation and encoding");
}

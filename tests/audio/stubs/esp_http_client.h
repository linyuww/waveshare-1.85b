#pragma once
#include "esp_err.h"
enum { HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_HEADER };
struct esp_http_client_event_t {
    int event_id;
    void *user_data;
    void *data;
    int data_len;
    const char *header_key;
    const char *header_value;
};

#pragma once
struct esp_app_desc_t { const char *version; const char *idf_ver; };
inline const esp_app_desc_t *esp_app_get_description() { static const esp_app_desc_t d{"test", "5.5.3"}; return &d; }

#include "fitness_storage.hpp"
#include "esp_log.h"
#include "nvs.h"

fitness::Storage::Load FitnessStorage::read(std::vector<uint8_t> &bytes)
{
    nvs_handle_t handle;
    auto error = nvs_open("fitness", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return Load::Missing;
    if (error != ESP_OK) { ESP_LOGW("fitness", "NVS open: %s", esp_err_to_name(error)); return Load::Failed; }
    size_t size = 0;
    error = nvs_get_blob(handle, "state_v1", nullptr, &size);
    if (error == ESP_OK && size <= 4096) {
        bytes.resize(size);
        error = nvs_get_blob(handle, "state_v1", bytes.data(), &size);
    } else if (error == ESP_OK) error = ESP_ERR_INVALID_SIZE;
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return Load::Missing;
    if (error != ESP_OK) { ESP_LOGW("fitness", "NVS read: %s", esp_err_to_name(error)); return Load::Failed; }
    return Load::Loaded;
}
bool FitnessStorage::write(const std::vector<uint8_t> &bytes)
{
    nvs_handle_t handle;
    auto error = nvs_open("fitness", NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, "state_v1", bytes.data(), bytes.size());
        if (error == ESP_OK) error = nvs_commit(handle);
        nvs_close(handle);
    }
    if (error != ESP_OK) ESP_LOGW("fitness", "NVS save: %s", esp_err_to_name(error));
    return error == ESP_OK;
}

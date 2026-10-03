#include "app_navigation.hpp"
namespace { QueueHandle_t requests; }
bool AppNavigation::initialize()
{
    requests = xQueueCreate(8, sizeof(AppTarget));
    return requests != nullptr;
}
bool AppNavigation::request(AppTarget target)
{
    return requests && xQueueSend(requests, &target, 0) == pdTRUE;
}
bool AppNavigation::poll(AppTarget &target)
{
    return requests && xQueueReceive(requests, &target, 0) == pdTRUE;
}

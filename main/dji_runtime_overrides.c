#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "DJI_RUNTIME";

BaseType_t __real_xTaskCreatePinnedToCore(
    TaskFunction_t task_code, const char * const task_name,
    const configSTACK_DEPTH_TYPE stack_depth, void * const parameters,
    UBaseType_t priority, TaskHandle_t * const task_handle,
    const BaseType_t core_id);

BaseType_t __wrap_xTaskCreatePinnedToCore(
    TaskFunction_t task_code, const char * const task_name,
    configSTACK_DEPTH_TYPE stack_depth, void * const parameters,
    UBaseType_t priority, TaskHandle_t * const task_handle,
    BaseType_t core_id) {
    if (task_name != NULL && strcmp(task_name, "notify_processing_task") == 0 &&
        stack_depth < 8192) {
        ESP_LOGW(TAG, "Raising DJI notification worker stack from %u to 8192 bytes",
                 (unsigned)stack_depth);
        stack_depth = 8192;
    }

    return __real_xTaskCreatePinnedToCore(task_code, task_name, stack_depth,
                                          parameters, priority, task_handle,
                                          core_id);
}

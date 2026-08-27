#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "HELLO_TEST";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 started successfully!");

    int counter = 0;

    while (1) {
        ESP_LOGI(TAG, "Hello world! Counter = %d", counter++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
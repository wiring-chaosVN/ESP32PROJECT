#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_system.h"

void app_main(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    printf("\nESP32-OS bootstrap application\n");
    printf("App version: %s\n", desc ? desc->version : "unknown");
    printf("Bootloader monitor returned control to ESP-IDF boot flow.\n");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

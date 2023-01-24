#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"

TaskHandle_t transmitTaskHandle;
int configProfile;
esp_err_t network_connect(void);
void http_tx_task(void *pvParameters);


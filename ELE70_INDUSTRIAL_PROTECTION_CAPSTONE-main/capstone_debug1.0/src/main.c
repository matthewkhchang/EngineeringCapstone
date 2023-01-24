#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "sdkconfig.h"
#include "esp_wifi_default.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "network.h"
#include "sensor-i2c.h"

//Defines
#define CONFIG1 1000000
#define CONFIG2 5000000
#define CONFIG3 10000000
#define LIGHT_EN 25
#define TEMP_EN 26
#define GAS_EN 27

//Private Variables
static const char *TAG = "main";
static sensor_struct *light;
static sensor_struct *temp;
static sensor_struct *gas;
static esp_timer_handle_t periodic_timer;
static int tx_flag = 0;
static int profile_flag = 0;

//Public Functions
void app_main(void);

//Private Functions
static void main_task_core0(void *pvParameters);
static void main_task_core1(void *pvParameters);
static void hw_en_pins_init();
static void timer_init();
static void periodic_timer_callback(void* arg);
static void change_profile();
static void IRAM_ATTR light_isr_handler(void*par);
static void IRAM_ATTR temp_isr_handler(void*par);
static void IRAM_ATTR gas_isr_handler(void*par);

//****************************************************************************
//Public Functions
//****************************************************************************

/**
 * @brief Application entry
 */
void app_main(void)
{    
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    hw_en_pins_init();
    if (gpio_get_level(LIGHT_EN) == 1){
        light = (sensor_struct *) malloc(sizeof(sensor_struct));
        light->id = 1;
        light->value = 0;

    }
    if (gpio_get_level(TEMP_EN) == 1){
        temp = (sensor_struct *) malloc(sizeof(sensor_struct));
        temp->id = 2;
        temp->value = 0;
    }
    if (gpio_get_level(GAS_EN) == 1){
        gas = (sensor_struct *) malloc(sizeof(sensor_struct));
        gas->id = 3;
        gas->value = 0;
    }
    sensor_i2c_init();
    timer_init();

    ESP_ERROR_CHECK(network_connect());

    xTaskCreatePinnedToCore(main_task_core1, "main_task_core1", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(main_task_core0, "main_task_core0", 4096, NULL, 5, &transmitTaskHandle, 0);
}

//****************************************************************************
//Private Functions
//****************************************************************************

/**
 * @brief This is the main routine ran on CORE 0 (Set affinity to CORE 0), which schedules packet transmission
 * based on status of its transmission request flag
 */
static void main_task_core0(void *pvParameters)
{
    int counter = 0;
    while(1){
        ESP_LOGI(TAG, "Counter...%d ", counter);
        counter++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        if (tx_flag == 1){
            tx_flag = 0;
            if (gpio_get_level(LIGHT_EN) == 1){
                xTaskCreatePinnedToCore(http_tx_task, "http_tx_task", 2048, light, 10, NULL, 0);
                vTaskDelay(10);
            }
            if (gpio_get_level(TEMP_EN) == 1){
                xTaskCreatePinnedToCore(http_tx_task, "http_tx_task", 2048, temp, 10, NULL, 0);
                vTaskDelay(10);
            }
        }
    }
}

/**
 * @brief This is the main routine ran on CORE 1 (Set affinity to CORE 1), which polls sensors at a set interval
 * and parses received HTTP messages for configuration profile changes
 */
static void main_task_core1(void *pvParameters)
{
    while(1){
        vTaskDelay(1000 / portTICK_PERIOD_MS);//Set polling interval
        if (gpio_get_level(LIGHT_EN) == 1){
            light->value = light_read();
        }
        if (gpio_get_level(TEMP_EN) == 1){
            temp->value = temp_read();
        }
        if (profile_flag != configProfile){
            change_profile();
        }
    }
}

/**
 * @brief Configures the GPIO pins for sensor enable detection
 */
static void hw_en_pins_init(){
    gpio_reset_pin(LIGHT_EN);
    gpio_reset_pin(TEMP_EN);
    gpio_reset_pin(GAS_EN);

    gpio_set_direction(LIGHT_EN, GPIO_MODE_INPUT);
    gpio_set_direction(TEMP_EN, GPIO_MODE_INPUT);
    gpio_set_direction(GAS_EN, GPIO_MODE_INPUT);

    gpio_intr_enable(LIGHT_EN);
    gpio_intr_enable(TEMP_EN);
    gpio_intr_enable(GAS_EN);

    gpio_set_pull_mode(LIGHT_EN, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(TEMP_EN, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(GAS_EN, GPIO_PULLDOWN_ONLY);

    gpio_set_intr_type(LIGHT_EN, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(TEMP_EN, GPIO_INTR_ANYEDGE);
    gpio_set_intr_type(GAS_EN, GPIO_INTR_ANYEDGE);

    gpio_install_isr_service (7);
    gpio_isr_handler_add(LIGHT_EN, light_isr_handler, NULL);
    gpio_isr_handler_add(TEMP_EN, temp_isr_handler, NULL);
    gpio_isr_handler_add(GAS_EN, gas_isr_handler, NULL);
}

/**
 * @brief Initialize periodic timer to signal and scheduled HTTP transmits
 */
static void timer_init(){

    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &periodic_timer_callback,
            .name = "periodic"
    };
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000000));
}

/**
 * @brief Timer interrupt callback: Set transmission flag to make HTTP call
 */
static void periodic_timer_callback(void* arg){
    tx_flag = 1;
}

/**
 * @brief Sets transmission to make HTTP call to update most recent sensor values, then restarts timer 
 * with new periodic interval determined by configuration profile selected
 */
static void change_profile(){
    tx_flag = 1;
    ESP_ERROR_CHECK(esp_timer_stop(periodic_timer));
    switch(configProfile)
    {
        case 1:
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, CONFIG1));
            break;
        case 2:
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, CONFIG2));
            break;
        case 3:
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, CONFIG3));
            break;
        default:
            ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, CONFIG2));
    }
    profile_flag = configProfile;
}

/**
 * @brief ISR routine for light sensor enable pin edge trigger, positive edge will register sensor
 * negative edge will deregister sensor (To be implemented)
 */
static void IRAM_ATTR light_isr_handler(void*par){
    if(gpio_get_level(LIGHT_EN)==1){//posedge transition
        light = (sensor_struct *) malloc(sizeof(sensor_struct));
        light->id = 1;
        light->value = 0;
    }
    else{
        //free(light);
    }
}

/**
 * @brief ISR routine for temperature sensor enable pin edge trigger, positive edge will register sensor
 * negative edge will deregister sensor (To be implemented)
 */
static void IRAM_ATTR temp_isr_handler(void*par){
    if(gpio_get_level(TEMP_EN)==1){//posedge transition
        temp = (sensor_struct *) malloc(sizeof(sensor_struct));
        temp->id = 2;
        temp->value = 0;
    }
    else{
        //free(temp);
    }
}

/**
 * @brief ISR routine for gas sensor enable pin edge trigger, positive edge will register sensor
 * negative edge will deregister sensor (To be implemented)
 */
static void IRAM_ATTR gas_isr_handler(void*par){
    if(gpio_get_level(GAS_EN)==1){//posedge transition
        gas = (sensor_struct *) malloc(sizeof(sensor_struct));
        gas->id = 3;
        gas->value = 0;
    }
    else{
        //free(gas);
    }
}
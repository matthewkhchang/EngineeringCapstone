#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "esp_wifi_default.h"
#include "lwip/dns.h"
#include "sensor-i2c.h"

//Defines
#define WEB_SERVER "192.168.2.77"
#define WEB_PORT "80"
#define WEB_PATH "/?sensor_id=1&measurement=100"
#define NETWORK_ID ""
#define NETWORK_PW ""
#define GOT_IPV4_BIT BIT(0)
#define GOT_IPV6_BIT BIT(1)
#define CONFIG_EXAMPLE_CONNECT_WIFI 1
#define CONNECTED_BITS (GOT_IPV4_BIT)

//Public Variables
TaskHandle_t transmitTaskHandle = NULL;
int configProfile = 1;

//Private Variables
static EventGroupHandle_t s_connect_event_group;
static esp_ip4_addr_t s_ip_addr;
static const char *s_connection_name;
static esp_netif_t *s_example_esp_netif = NULL;

//Public Function Declarations
esp_err_t network_connect(void);
void http_tx_task(void *pvParameters);

//Private Function Declarations
static void start(void);
static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static char* construct_payload(int id, int value);

//****************************************************************************
//Public Functions
//****************************************************************************
/**
 * @brief Connect to WiFi
 */
esp_err_t network_connect()
{
    if (s_connect_event_group != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_connect_event_group = xEventGroupCreate();
    start();
    xEventGroupWaitBits(s_connect_event_group, CONNECTED_BITS, true, true, portMAX_DELAY);
    return ESP_OK;
}


/**
 * @brief Task creates a socket and makes a HTTP call with the payload passed as pvParameters,
 *  and then retrieves configuration profile data
 */
void http_tx_task(void *pvParameters)
{
    //Block parent task
    vTaskSuspend(transmitTaskHandle);

    //Construct payload
    sensor_struct dataOut = *(sensor_struct *) pvParameters;
    char *payload = construct_payload(dataOut.id, dataOut.value);

    //HTTP call
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[100];

    getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;

    s = socket(res->ai_family, res->ai_socktype, 0);
    connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
        
    write(s, payload, strlen(payload));

    struct timeval receiving_timeout;
    receiving_timeout.tv_sec = 5;
    receiving_timeout.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout, sizeof(receiving_timeout));

    do {
        bzero(recv_buf, sizeof(recv_buf));
        r = read(s, recv_buf, sizeof(recv_buf)-1);
    } while(r == sizeof(recv_buf)-1);

    close(s);

    //Retreive config profile response
    char *ch = "#";
    char *ret;
    ret = (char *)memmem(recv_buf, sizeof(recv_buf), ch, 1) + 1;
    configProfile = atoi(ret);

    //Return   
    free(payload);
    vTaskResume(transmitTaskHandle);
    vTaskDelete(NULL); 
}       

//****************************************************************************
//Private Functions
//****************************************************************************

/**
 * @brief Establish WiFi connection
 */
static void start()
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();

    esp_netif_t *netif = esp_netif_new(&netif_config);

    assert(netif);

    esp_netif_attach_wifi_station(netif);
    esp_wifi_set_default_wifi_sta_handlers();

    s_example_esp_netif = netif;

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = NETWORK_ID,
            .password = NETWORK_PW,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    s_connection_name = NETWORK_ID;
}

static void on_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    memcpy(&s_ip_addr, &event->ip_info.ip, sizeof(s_ip_addr));
    xEventGroupSetBits(s_connect_event_group, GOT_IPV4_BIT);
}


/**
 * @brief Structures an HTTP call
 *  -packet format is hard coded for what our software expects in an HTTP call
 */
static char* construct_payload(int id, int value){

    char *payload = malloc(150);
    char *id_str = malloc(12);
    char *val_str = malloc(12);
    sprintf(id_str,"%d", id);
    sprintf(val_str,"%d", value);

    payload[0] = '\0';
    strcat(payload,"GET /?sensor_id=");
    strcat(payload, id_str);
    strcat(payload,"&measurement=");
    strcat(payload,val_str);
    strcat(payload," HTTP/1.0\r\n"
    "Host: "WEB_SERVER":"WEB_PORT"\r\n"
    "User-Agent: esp-idf/1.0 esp32\r\n"
    "\r\n");

    free(id_str);
    free(val_str);

    return payload;
}
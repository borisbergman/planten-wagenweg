#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"
#include <string.h>
#include "driver/gpio.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
//#include "protocol_examples_common.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "softapp_wag.h"

static const char *TAG = "wagenweg";

extern const uint8_t bcb_pem_start[] asm("_binary_letsencryptroot_pem_start");
extern const uint8_t bcb_pem_end[] asm("_binary_letsencryptroot_pem_end");
#define GPIO_OUTPUT_PIN_SEL ((1ULL << 12) | (1ULL << 27) | (1ULL << 32) | (1ULL << 33))

static int solenoids[4] = {12, 27, 32, 33};
static volatile int openingtime = 6000;

static void close_solenoid(TimerHandle_t xTimer)
{
    int ulCount;
    ulCount = (int)pvTimerGetTimerID(xTimer);
    gpio_set_level(solenoids[ulCount], 0);
    xTimerStop(xTimer, 0);
    ESP_LOGI(TAG, "closed %d", ulCount);
    xTimerDelete( xTimer, portMAX_DELAY );
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "/wagenweg/planten", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/wagenweg/planten/time", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

        msg_id = esp_mqtt_client_subscribe(client, "/wagenweg/planten/off", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        
        msg_id = esp_mqtt_client_publish(client, "/wagenweg/planten/online", "1", 0, 0, 0);
        ESP_LOGI(TAG, "online mtqq send, msg_id=%d", msg_id);
        
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        // printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        // printf("DATA=%.*s\r\n", event->data_len, event->data);
        char buffer[] = {0,0,0,0,0,0,0,0};
        //char topic[event->topic_len];
        strncpy(buffer, event->data, event->data_len);
        //strncpy(topic, event->topic, event->topic_len);
        
        int num = strtol(buffer, NULL, 10);
        ESP_LOGI(TAG, "received data: %i", num);
        if (strncmp("/wagenweg/planten", event->topic, event->topic_len) == 0)
        {   
            ESP_LOGI(TAG, "topic /wagenweg/planten");                        
            gpio_set_level(solenoids[num], 1);
            
            TimerHandle_t timerq = xTimerCreate("closetimer", pdMS_TO_TICKS(3000), pdFALSE, (void *)num, close_solenoid);
            xTimerChangePeriod( timerq, openingtime / portTICK_PERIOD_MS, 100);
            xTimerStart(timerq, 0);
        } else if(strncmp("/wagenweg/planten/time", event->topic, event->topic_len) == 0) {                        
            ESP_LOGI(TAG, "topic /wagenweg/planten/time %i", num);              
            openingtime = num;
        } else if(strncmp("/wagenweg/planten/off", event->topic, event->topic_len) == 0) {
            ESP_LOGI(TAG, "topic /wagenweg/planten/off ");            
            gpio_set_level(solenoids[num], 0);
        } else {
            ESP_LOGI(TAG, "UNKNOWN TOPIC %.*s",event->data_len, event->topic);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        }
        else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        }
        else
        {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

void pinOutput()
{
    gpio_config_t io_conf = {};
    // disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    // set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    // disable pull-down mode
    io_conf.pull_down_en = 0;
    // disable pull-up mode
    io_conf.pull_up_en = 0;
    // configure GPIO with the given settings
    gpio_config(&io_conf);
}

static void mqtt_app_start(void)
{
    pinOutput();

    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URI,
        .cert_pem = (const char *)bcb_pem_start,
        .password = (const char *)mqttpassword,
        .username = (const char *)mqttuser};

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    wifi_softap();
    // ESP_ERROR_CHECK(esp_event_loop_create_default());

    mqtt_app_start();
    while (1)
    {
        vTaskDelay(20000 / portTICK_PERIOD_MS);
    }
}

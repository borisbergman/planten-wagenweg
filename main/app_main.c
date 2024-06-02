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

#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "softapp_wag.h"
#include "../proto/solenoid.pb.h"
#include "../proto/alive.pb.h"
#include "../proto/pb_encode.h"
#include "../proto/pb_decode.h"

#define GPIO_OUTPUT_PIN_SEL ((1ULL << 2) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5))
#define numsolenoids 4

static const char *TAG = "wagenweg";
extern const uint8_t bcb_pem_start[] asm("_binary_isrgrootx1_pem_start");
extern const uint8_t bcb_pem_end[] asm("_binary_isrgrootx1_pem_end");

static gpio_num_t solenoid[numsolenoids] = {GPIO_NUM_2, GPIO_NUM_3, GPIO_NUM_4, GPIO_NUM_5};
static int32_t timers[numsolenoids] = {0,0,0,0};
static volatile int openingtime = 6000;

static void close_solenoid(uint32_t id)
{    
    gpio_set_level(solenoid[id], 1);        
}

static void immediate_stop() {
    for(int i = 0; i<numsolenoids; i++) {
        close_solenoid(i);
        timers[i] = -1;
    }
}

bool status;
bool _mqttConnected;

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
        msg_id = esp_mqtt_client_subscribe(client, "/wagenweg/planten/", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);        

        msg_id = esp_mqtt_client_subscribe(client, "/wagenweg/planten/stop", 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        
        msg_id = esp_mqtt_client_publish(client, "/wagenweg/planten/online", "1", 0, 0, 0);
        ESP_LOGI(TAG, "online mtqq send, msg_id=%d", msg_id);
        _mqttConnected = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        _mqttConnected = false;
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
        if (strncmp("/wagenweg/planten/", event->topic, event->topic_len) == 0)
        {   
            ESP_LOGI(TAG, "topic /wagenweg/planten/");            
            Solenoid message = Solenoid_init_zero;
            pb_istream_t stream = pb_istream_from_buffer((uint8_t *)event->data, event->data_len);
            status = pb_decode(&stream, Solenoid_fields, &message);
    
            /* Check for errors... */
            if (!status)
            {
                ESP_LOGE(TAG, "Decoding failed: %s\n", PB_GET_ERROR(&stream));            
            } else {                
                ESP_LOGI(TAG, "received open, id:%i, time:%i", message.id, message.time);
                timers[message.id] = message.time;            
                gpio_set_level(solenoid[message.id], 0);
            }
                            
        } else if(strncmp("/wagenweg/planten/stop", event->topic, event->topic_len) == 0) {
            ESP_LOGI(TAG, "topic /wagenweg/planten/stop ");            
            immediate_stop();
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
    //set initial to 1, inverted solenoid relays
    for(uint8_t x =0; x<numsolenoids; x++) {
        gpio_set_level(solenoid[x], 1);
    }    
}

static esp_mqtt_client_handle_t _client;

static void mqtt_app_start(void)
{
    pinOutput();

    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtts://boriscornelisbergman.nl",
        .cert_pem = (const char *)bcb_pem_start,
        .password = (const char *)mqttpassword,
        .username = (const char *)mqttuser};

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    _client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(_client);
}

void app_main(void)
{

    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    uint8_t buffer[128];
    wifi_softap();
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    static int keepaliveTimer =0;
    mqtt_app_start();
    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        //check which solenoid to close
        for(int i =0; i<numsolenoids; i++) {
            if(timers[i] <= 0) {
                close_solenoid(i);                        
            } else {
                timers[i]--;
            }
        }        
        
        if(keepaliveTimer % 600 == 0) {
            //send alive to server
            keepaliveTimer = 0;
            
            if(_mqttConnected)        {
                Alive message = Alive_init_zero;
                pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
                message.time = esp_timer_get_time();    
                
                status = pb_encode(&stream, Alive_fields, &message);                
                /* Then just check for any errors.. */
                if (!status)
                {
                    printf("Encoding failed: %s\n", PB_GET_ERROR(&stream));                    
                } else {
                    if(esp_mqtt_client_publish(_client, "/wagenweg/planten/online", (char*)buffer, stream.bytes_written, 0, 0) == -1) {
                        ESP_LOGI(TAG, "failed to send alive");
                    };
                }
            }
        }
        keepaliveTimer++;
    }
}

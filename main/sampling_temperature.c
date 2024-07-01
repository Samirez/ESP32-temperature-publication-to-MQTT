#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include <esp_adc_cal.h>
#include <driver/adc.h>

static const char *TAG = "MQTT_TASK";
#define MAX_RETRY 10
static int retry_cnt = 0;
#define DEFAULT_VREF 1100
#define TX_BUFFER_SIZE 10

QueueHandle_t tx_queue;

#define DELAY(ms) ((ms) / portTICK_PERIOD_MS)

static esp_adc_cal_characteristics_t *adc1_chars;
static uint32_t MQTT_CONNECTED = 0;
// command topic variables
static int time_interval = 1;
static int samples = 1;
esp_mqtt_client_handle_t client = NULL;
SemaphoreHandle_t xSemaphore = NULL;

void TaskSample(void* pvParameters){
    QueueHandle_t output_queue = (QueueHandle_t) pvParameters;
    adc1_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, DEFAULT_VREF, adc1_chars);
    adc1_config_channel_atten(ADC1_CHANNEL_6, ADC_ATTEN_DB_11);
    if(xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE) {
        for (int i = 1; i <= samples; i++){
            int read_raw = adc1_get_raw(ADC1_CHANNEL_6);
            if (read_raw) {
                printf("%d\n", read_raw );
                vTaskDelay(DELAY(time_interval));
                int value = read_raw;
                xQueueSend(output_queue, &value, portMAX_DELAY);
            } else {
                printf("ADC1 used by Wi-Fi.\n");
            }
        }
    }
    free(adc1_chars);
    vTaskDelete(NULL);
}


void TaskTransmit(void* pvParameters){
    QueueHandle_t input_queue = (QueueHandle_t) pvParameters;
    int value;
    for (int i = 1; i <= samples; i++){
        xQueueReceive(input_queue, &value, portMAX_DELAY);
        uint32_t voltage = 0;
        voltage = esp_adc_cal_raw_to_voltage(value, adc1_chars);
        printf("ADC1_CHANNEL_6: %" PRIu32 " mV\n", voltage);
        float voltage_mV = (float)voltage;
        // converting the read voltage to temperature
        float temperature = ((10.888-sqrt(pow(-10.888, 2.0)+(4*0.00347*(1777.3-voltage_mV))))/(2*(-0.00347)))+30;
        printf("Temperature is %.2f°C\n", temperature);

        // Convert temperature to string
        char temperature_str[20];
        sprintf(temperature_str, "%.2f", temperature);

        //creat payload of uptime, amount of messages remaining and temperature
        char payload[50];
        int remaining_messages = samples-i;
        sprintf(payload, "%d,%s,%lld", remaining_messages, temperature_str, esp_timer_get_time()/1000);

        // Publish temperature to MQTT
        if (MQTT_CONNECTED)
        {
            int msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_RESPONSE_TOPIC, payload, strlen(payload), 2, 0);
            if (msg_id == -1){
                printf(TAG, "failed to publish message");
            }
        }
        else
        {
            printf(TAG, "MQTT Not connected");
        }
    }
}


void mqtt_app_start(void);


esp_err_t wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            printf(TAG, "Trying to connect with Wi-Fi\n");
            break;
        case WIFI_EVENT_STA_CONNECTED:
            printf(TAG, "Wi-Fi connected\n");
            break;
        case IP_EVENT_STA_GOT_IP:
            printf(TAG, "got ip: starting MQTT Client\n");
            mqtt_app_start();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            printf(TAG, "disconnected: Retrying Wi-Fi\n");
            if (retry_cnt++ < MAX_RETRY) {
                esp_wifi_connect();
            }
            else {
                ESP_LOGI(TAG, "Max Retry Failed: Wi-Fi Connection\n");
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}


void wifi_init(void)
{
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
            .sta = {
                    .ssid = CONFIG_WIFI_SSID,
                    .password = CONFIG_WIFI_PASSWORD,
                    .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
    };

    esp_netif_init();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
}


void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            MQTT_CONNECTED = 1;
            msg_id = esp_mqtt_client_subscribe(client, CONFIG_MQTT_COMMAND_TOPIC, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            retry_cnt = 0;
            break;

        case MQTT_EVENT_DATA:
            if (strncmp(event->topic, CONFIG_MQTT_COMMAND_TOPIC, event->topic_len) == 0)
            {
                sscanf(event->data, "measure: %d , %d", &samples, &time_interval);
                xSemaphoreGive(xSemaphore);
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            MQTT_CONNECTED = 0;
            // attempting to reconnect
            if (retry_cnt < CONFIG_ESP_MAXIMUM_RETRY)
            {
                ESP_LOGI(TAG, "Attempting to reconnect (%d/%d)...", retry_cnt + 1, CONFIG_ESP_MAXIMUM_RETRY);
                esp_mqtt_client_reconnect(client);
                retry_cnt++;
            }
            else
            {
                ESP_LOGI(TAG, "Max reconnection attempts reached. Not trying to reconnect.");
            }
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;

        default:
            printf(TAG, "Other event id:%d", event->event_id);
            break;
    }
}


void mqtt_app_start(void)
{
    ESP_LOGI(TAG, "STARTING MQTT");
    esp_mqtt_client_config_t mqttConfig = {
            .broker.address.uri = CONFIG_MQTT_BROKER,
    };

    // Initialize MQTT client and register event handler
    client = esp_mqtt_client_init(&mqttConfig);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);

    // Start MQTT client
    esp_mqtt_client_start(client);
    printf(TAG, "MQTT Publisher_Task is up and running\n");
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    wifi_init();
    xSemaphore = xSemaphoreCreateBinary();
    tx_queue = xQueueCreate(TX_BUFFER_SIZE, sizeof(int));
    xTaskCreate(TaskSample, "Sample", 4096, tx_queue, 1, NULL);
    xTaskCreate(TaskTransmit, "Transmit", 4096, tx_queue, 1, NULL);
}
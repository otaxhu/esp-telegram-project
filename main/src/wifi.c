#include "main/wifi.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include <string.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <math.h>
#include "esp_dht_driver.h"

static const char * const TAG = "main/wifi";

extern dht_handle_t dht_handle;

static esp_err_t http_client_event_handler(esp_http_client_event_t *event) {
    static char *data = NULL;
    static size_t data_len = 0;

    static uint64_t timestamp_last_message = 0;

    cJSON *json_root = NULL;

    esp_http_client_handle_t handle = NULL;

    char *url = NULL;

    esp_err_t err = ESP_OK;

    switch (event->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "got HTTP_EVENT_ERROR");
        err = ESP_FAIL;
        goto exit;
    case HTTP_EVENT_ON_DATA:
        char *temp = realloc(data, event->data_len + data_len);
        if (temp == NULL) {
            ESP_LOGE(TAG, "There is no memory to allocate response");
            err = ESP_ERR_NO_MEM;
            goto exit;
        }
        data = temp;
        memcpy(data + data_len, event->data, event->data_len);
        data_len += event->data_len;
        break;
    case HTTP_EVENT_ON_FINISH:
        json_root = cJSON_ParseWithLength(data, data_len);
        if (json_root == NULL) {
            ESP_LOGE(TAG, "failed cJSON_ParseWithLenght()");
            err = ESP_FAIL;
            goto exit;
        }
        cJSON *json_ok = cJSON_GetObjectItem(json_root, "ok");
        if (cJSON_IsBool(json_ok) && cJSON_IsFalse(json_ok)) {
            ESP_LOGE(TAG, "ok is false");
            err = ESP_FAIL;
            goto exit;
        }
        cJSON *message_object =
            cJSON_GetObjectItem(
                cJSON_GetArrayItem(
                    cJSON_GetObjectItem(json_root, "result")
                    ,0
                )
                ,"message"
            );

        if (message_object == NULL) {
            // break because there is no message during polling to Telegram
            // goes to return ESP_OK statement
            goto exit;
        }

        uint64_t date =
            (uint64_t)cJSON_GetNumberValue(
                cJSON_GetObjectItem(message_object, "date")
            );

        if (date <= timestamp_last_message) {
            goto exit;
        }

        timestamp_last_message = date;

        uint64_t chat_id =
            (uint64_t)cJSON_GetNumberValue(
                cJSON_GetObjectItem(
                    cJSON_GetObjectItem(message_object, "chat")
                    ,"id"
                )
            );

        int id_str_len;

        if (chat_id > 0) {
            id_str_len = ((int)log10f(chat_id)) + 1;
        } else {
            id_str_len = 1;
        }

        char action1[] = "/sendMessage?parse_mode=MarkdownV2&chat_id=";
        char action2[] = "&text=Your+temparature+is+";

        url = malloc(strlen(getenv("TELEGRAM_API_KEY")) +
                           (sizeof(action1) - 1) +
                           id_str_len +
                           sizeof(action2) +
                           12/* 20+0+%C2%B0C ocuppies 12 chars */);
        if (url == NULL) {
            ESP_LOGE(TAG, "failed to allocate url action");
            err = ESP_ERR_NO_MEM;
            goto exit;
        }

        dht_data_t dht_data;

        if (err = dht_read(dht_handle, &dht_data)) {
            ESP_LOGE(TAG, "failed dht_read()");
            goto exit;
        }

        sprintf(url, "%s%s%lld%s%hhd+%hhu+%%C2%%B0C", getenv("TELEGRAM_API_KEY"), action1, chat_id, action2, dht_data.temperature.integral, dht_data.temperature.decimal);

        printf("%s\n", url);

        esp_http_client_config_t config = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .crt_bundle_attach = esp_crt_bundle_attach
        };

        handle = esp_http_client_init(&config);

        if (handle == NULL) {
            ESP_LOGE(TAG, "failed esp_http_client_init()");
            err = ESP_FAIL;
            goto exit;
        }

        if (err = esp_http_client_perform(handle)) {
            ESP_LOGE(TAG, "failed esp_http_client_perform()");
            goto exit;
        }
        goto exit;
    default:
    }
    if (0) {
exit:
        free(data);
        data = NULL;
        data_len = 0;
        cJSON_Delete(json_root);
        esp_http_client_cleanup(handle);
        free(url);
    }
    return err;
}

static void perform_http_request(void *client) {
    esp_http_client_handle_t c = (esp_http_client_handle_t)client;
    while (1) {
        if (esp_http_client_perform(c)) {
            ESP_LOGE(TAG, "failed esp_http_client_perform()");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_http_client_cleanup(c);
    vTaskDelete(NULL);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    static int retry_num = 0;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_num++ < 10) {
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "failed to connect to wifi");
        }
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *data = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&data->ip_info.ip));
        retry_num = 0;

        esp_http_client_handle_t client = NULL;

        char updates_action[] = "/getUpdates?offset=-1&allowed_updates=[\"message\"]";

        char *url = malloc(strlen(getenv("TELEGRAM_API_KEY")) +
            sizeof(updates_action) /*Already adds the NUL terminator*/);

        if (url == NULL) {
            ESP_LOGE(TAG, "failed memory allocation for url");
            goto fail_ip_event;
        }

        sprintf(url, "%s%s", getenv("TELEGRAM_API_KEY"), updates_action);

        esp_http_client_config_t http_client_config = {
            .url = url,
            .method = HTTP_METHOD_GET,
            .event_handler = http_client_event_handler,
            // .cert_pem = (const char *)certificate_pem,
            .crt_bundle_attach = esp_crt_bundle_attach
        };

        client = esp_http_client_init(&http_client_config);

        if (client == NULL) {
            ESP_LOGE(TAG, "failed esp_http_client_init()");
            goto fail_ip_event;
        }

        int ret = xTaskCreate(perform_http_request, "perform_http_request()", 4096, client, tskIDLE_PRIORITY, NULL);

        if (ret != pdPASS) {
            ESP_LOGE(TAG, "failed xTaskCreate(perform_http_request)");
            goto fail_ip_event;
        }
fail_ip_event:
        free(url);
    }
}

esp_err_t main_wifi_init_sta(void) {
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "failed esp_netif_init()");

    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "failed esp_event_loop_create_default()");

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "failed esp_wifi_init()");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                   ESP_EVENT_ANY_ID,
                                                   event_handler,
                                                   NULL), TAG, "failed registering WIFI_EVENT");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                   IP_EVENT_STA_GOT_IP,
                                                   event_handler,
                                                   NULL), TAG, "failed registering WIFI");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "failed esp_wifi_set_mode()");
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, (const char *)getenv("WIFI_SSID"), sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, (const char *)getenv("WIFI_PASSWORD"), sizeof(wifi_config.sta.password));
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "failed esp_wifi_set_config()");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed esp_wifi_start()");

    return ESP_OK;
}

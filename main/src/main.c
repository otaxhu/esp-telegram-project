#include "nvs_flash.h"
#include "nvs_dotenv.h"
#include "main/wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_dht_driver.h"

dht_handle_t dht_handle;

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(nvs_dotenv_load());

    dht_config_t dht_conf = {
        .gpio_pin = GPIO_NUM_4,
    };
    ESP_ERROR_CHECK(dht_new(&dht_conf, &dht_handle));

    ESP_ERROR_CHECK(main_wifi_init_sta());

    vTaskDelete(NULL);
}

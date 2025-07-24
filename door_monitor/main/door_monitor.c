#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_sntp.h"

// --- CONFIGURATION ---
#define SENSOR_GPIO     GPIO_NUM_4
#define LED_GPIO        GPIO_NUM_2
#define WIFI_SSID       "Ace & King of Hearts"
#define WIFI_PASS       "MagicMusicMorals"
#define TELEGRAM_BOT_TOKEN "8147801516:AAGXa3En0T2wQTTtV2APgHGWyMSEyCK_1sE"
#define TELEGRAM_CHAT_ID_1 "1245560165"
#define TELEGRAM_CHAT_ID_2 "9999999999"

#define WIFI_CONNECTED_BIT BIT0
#define ALERT_INTERVAL_MS 60000

// --- GLOBALS ---
static const char *TAG = "DOOR_MONITOR";
static EventGroupHandle_t wifi_event_group;
static bool away_mode = true;  // change to false if default is at-home mode

// --- URL Encode Helper ---
void url_encode(const char *src, char *dst, size_t dst_size) {
    static const char *hex = "0123456789ABCDEF";
    size_t len = 0;
    while (*src && len + 3 < dst_size) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[len++] = c;
        } else {
            dst[len++] = '%';
            dst[len++] = hex[c >> 4];
            dst[len++] = hex[c & 15];
        }
        src++;
    }
    dst[len] = '\0';
}

// --- Telegram Notification ---
void send_telegram_alert(bool is_open) {
    const char *chat_ids[] = { TELEGRAM_CHAT_ID_1, TELEGRAM_CHAT_ID_2 };
    const char *greetings[] = { "Hello Mr Stark", "Hello Mrs Suseendran" };

    for (int i = 0; i < sizeof(chat_ids) / sizeof(chat_ids[0]); ++i) {
        char msg[256];
        if (is_open) {
            snprintf(msg, sizeof(msg),
                     "%s, your door or window is OPEN! Please reply with 'away' or 'here'",
                     greetings[i]);
        } else {
            snprintf(msg, sizeof(msg), "%s, your door or window is CLOSED.", greetings[i]);
        }

        char encoded_msg[512];
        url_encode(msg, encoded_msg, sizeof(encoded_msg));

        char full_url[1024];
        snprintf(full_url, sizeof(full_url),
                 "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
                 TELEGRAM_BOT_TOKEN, chat_ids[i], encoded_msg);

        esp_http_client_config_t config = {
            .url = full_url,
            .method = HTTP_METHOD_GET,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "✅ Alert sent to user %d", i + 1);
            } else {
                ESP_LOGE(TAG, "❌ HTTP request failed for user %d: %s", i + 1, esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }
    }
}

// --- Wi-Fi Event Handler ---
void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// --- Wi-Fi Initialization ---
void wifi_init() {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "📡 Connecting to Wi-Fi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "✅ Wi-Fi connected!");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

}

// --- Main ---
void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    gpio_config_t sensor_cfg = {
        .pin_bit_mask = (1ULL << SENSOR_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&sensor_cfg);

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_cfg);

    int64_t last_sent_time = 0;
    bool last_state = true;

    while (1) {
        bool is_closed = gpio_get_level(SENSOR_GPIO) == 0;
        gpio_set_level(LED_GPIO, is_closed ? 0 : 1);

        int64_t now = esp_timer_get_time() / 1000;

        if (is_closed != last_state) {
            ESP_LOGW(TAG, "Door/Window is %s", is_closed ? "CLOSED" : "OPEN");

            if (!is_closed && away_mode) {
                send_telegram_alert(true);  // 🔔 Instant alert
                last_sent_time = now;
            }

            last_state = is_closed;
        }

        if (!is_closed && away_mode && (now - last_sent_time >= ALERT_INTERVAL_MS)) {
            send_telegram_alert(true);  // 🔁 Repeat reminder
            last_sent_time = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

}

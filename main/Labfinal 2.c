#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "camera_pins.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_http_server.h"


#define WIFI_SSID      "mamy poko"
#define WIFI_PASS      "22345678"

#define BOT_TOKEN      "8529357402:AAGL5ltggaYVo-P6MPozIp4DTmpH_l93UdI"
#define CHAT_ID        "8533832577"

#define PIR_PIN 13
#define MOTION_COOLDOWN_MS 15000

static const char *TAG = "SECURITY_CAM";
static int64_t last_motion_time = 0;

/* ================= WIFI ================= */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {

        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_connect();
        }

    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}                         
static void wifi_init(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                    ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                    IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init finished.");
}

/* ================= CAMERA ================= */

static esp_err_t camera_init(void)
{
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_0,
        .ledc_timer = LEDC_TIMER_0,
        .pin_d0 = Y2_GPIO_NUM,
        .pin_d1 = Y3_GPIO_NUM,
        .pin_d2 = Y4_GPIO_NUM,
        .pin_d3 = Y5_GPIO_NUM,
        .pin_d4 = Y6_GPIO_NUM,
        .pin_d5 = Y7_GPIO_NUM,
        .pin_d6 = Y8_GPIO_NUM,
        .pin_d7 = Y9_GPIO_NUM,
        .pin_xclk = XCLK_GPIO_NUM,
        .pin_pclk = PCLK_GPIO_NUM,
        .pin_vsync = VSYNC_GPIO_NUM,
        .pin_href = HREF_GPIO_NUM,
        .pin_sccb_sda = SIOD_GPIO_NUM,
        .pin_sccb_scl = SIOC_GPIO_NUM,
        .pin_pwdn = PWDN_GPIO_NUM,
        .pin_reset = RESET_GPIO_NUM,
        .xclk_freq_hz = 20000000,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

/* ================= TELEGRAM ================= */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

void send_photo_telegram(camera_fb_t *fb)
{
    char post_url[256];
    sprintf(post_url,
            "https://api.telegram.org/bot%s/sendPhoto",
            BOT_TOKEN);

    esp_http_client_config_t config = {
        .url = post_url,
        .event_handler = http_event_handler,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);

    char boundary[] = "ESP32CAMBOUNDARY";
    char header[512];

    sprintf(header,
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
            "%s\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"photo\"; filename=\"image.jpg\"\r\n"
            "Content-Type: image/jpeg\r\n\r\n",
            boundary, CHAT_ID, boundary);

    char footer[64];
    sprintf(footer, "\r\n--%s--\r\n", boundary);

    int content_length = strlen(header) + fb->len + strlen(footer);

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=ESP32CAMBOUNDARY");

    esp_http_client_open(client, content_length);
    esp_http_client_write(client, header, strlen(header));
    esp_http_client_write(client, (char *)fb->buf, fb->len);
    esp_http_client_write(client, footer, strlen(footer));

    esp_http_client_fetch_headers(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Photo sent to Telegram");
}

/* ================= MAIN ================= */

static esp_err_t capture_handler(httpd_req_t *req)
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_send(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    return ESP_OK;
}
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {

        httpd_uri_t capture_uri = {
            .uri       = "/capture",
            .method    = HTTP_GET,
            .handler   = capture_handler,
            .user_ctx  = NULL
        };

        httpd_register_uri_handler(server, &capture_uri);
    }

    return server;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Security Camera");

    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(5000));


    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return;
    }
    start_webserver();   // ✅ เพิ่มตรงนี้
    ESP_LOGI(TAG, "Web server started");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    while (1) {
        if (gpio_get_level(PIR_PIN)) {

            int64_t now = esp_timer_get_time() / 1000;

            if (now - last_motion_time > MOTION_COOLDOWN_MS) {
                last_motion_time = now;

                ESP_LOGI(TAG, "Motion detected!");

                camera_fb_t *fb = esp_camera_fb_get();
                if (fb) {
                    send_photo_telegram(fb);
                    esp_camera_fb_return(fb);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
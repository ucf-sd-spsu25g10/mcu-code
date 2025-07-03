/* NAVIS MCU Code - UCF Senior Design Project - ECE Group 10 - Spring 2025 / Summer 2025
  Maintained by Michael Castglia with contributions from Pavan Senthil and Aden McKinney.
*/

// C++ headers
#include <inttypes.h>
#include <string.h>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <string>

#include "drv2605.hpp"
#include "format.hpp"
#include "logger.hpp"

// ESP-IDF Headers
extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/dac_continuous.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "rom/ets_sys.h"

#include "navigate_to.h"
#include "navigation_complete.h"
#include "turn.h"
#include "left.h"
#include "right.h"
#include "around.h"
#include "go_straight.h"
#include "meters.h"
#include "one.h"
#include "two.h"
#include "three.h"
#include "four.h"
#include "five.h"
#include "six.h"
#include "seven.h"
#include "eight.h"
#include "nine.h"
#include "ten.h"
#include "item_is_on.h"
#include "aisle.h"
#include "shelf.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "cJSON.h"
}

#include "credentials.h"

static const char *TAG = "NAVIS_MCU";

// Pin definitions
static constexpr gpio_num_t HAPTIC_EN1_PIN = GPIO_NUM_16;
static constexpr gpio_num_t HAPTIC_EN2_PIN = GPIO_NUM_17;
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_18;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_19;
static constexpr i2c_port_t I2C_PORT = I2C_NUM_0;

// Haptic effect definitions
static constexpr espp::Drv2605::Waveform STRONG_BUZZ_EFFECT = espp::Drv2605::Waveform::STRONG_BUZZ;
static constexpr int EFFECT_DURATION_MS = 1000; // Shorter duration for responsiveness

// Haptic Driver Instances
std::unique_ptr<espp::Drv2605> haptic1;
std::unique_ptr<espp::Drv2605> haptic2;

// Audio settings
static const int CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE = 48000;

// Shared data
const int MAX_NUMBERS = 20;
int receivedNumbers[MAX_NUMBERS];
int numbersCount = 0;
SemaphoreHandle_t xMutex = NULL;
QueueHandle_t hapticQueue;
QueueHandle_t audioQueue;
volatile bool webServerActive = true;

// UART settings
#define UART_NUM UART_NUM_0
#define UART_BUFFER_SIZE 128

// Task Handles
TaskHandle_t webServerTaskHandle = NULL;
TaskHandle_t uartTaskHandle = NULL;
TaskHandle_t hapticTaskHandle = NULL;
TaskHandle_t dacTaskHandle = NULL;

// Event group for WiFi connection
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (webServerActive) { // Only retry if web server is still active
            esp_wifi_connect();
            printf("retry to connect to the AP\n");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        printf("got ip:\n" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Web Server Handlers
esp_err_t root_get_handler(httpd_req_t *req) {
    std::string html = "<html><body><h1>ESP32 API Server</h1><p>Send a POST request to /api/cartList with a JSON array of numbers</p>";
    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
        if (numbersCount > 0) {
            html += "<h2>Received Numbers:</h2><ul>";
            for (int i = 0; i < numbersCount; i++) {
                html += "<li>" + std::to_string(receivedNumbers[i]) + "</li>";
            }
            html += "</ul>";
        }
        xSemaphoreGive(xMutex);
    }
    html += "</body></html>";
    httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t cartlist_post_handler(httpd_req_t *req) {
    char content[req->content_len + 1];
    int ret = httpd_req_recv(req, content, req->content_len);
    if (ret <= 0) {  /* 0 for EOF, -1 for HTPD_SOCK_ERR */
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req); // Respond with 408 timeout if no data is sent within the timeout period
        }
        return ESP_FAIL;
    }
    content[req->content_len] = '\0';
    printf("Received POST data: %s\n", content);

    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        const char *response = "{\"error\":\"JSON parse failed\"}";
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response);
        return ESP_FAIL;
    }

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        const char *response = "{\"error\":\"Expected JSON array\"}";
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, response);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
        numbersCount = 0;
        int array_size = cJSON_GetArraySize(root);
        for (int i = 0; i < array_size; i++) {
            cJSON *item = cJSON_GetArrayItem(root, i);
            if (cJSON_IsNumber(item) && numbersCount < MAX_NUMBERS) {
                receivedNumbers[numbersCount++] = item->valueint;
            }
        }
        xSemaphoreGive(xMutex);
    }
    cJSON_Delete(root);

    const char *response = "{\"status\":\"success\"}";
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    webServerActive = false;
    printf("Valid JSON received. Web server will disconnect.\n");
    return ESP_OK;
}

esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_FAIL;
}

// Tasks
void webServerTask(void *parameter) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, WIFI_SSID);
    strcpy((char*)wifi_config.sta.password, WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    printf("Waiting for WiFi connection...\n");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        printf("WiFi connected.\n");
    } else if (bits & WIFI_FAIL_BIT) {
        printf("WiFi connection failed.\n");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    

    printf("Starting web server\n");
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t cartlist_uri = {
            .uri       = "/api/cartList",
            .method    = HTTP_POST,
            .handler   = cartlist_post_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &cartlist_uri);

        printf("HTTP server started\n");
    } else {
        ESP_LOGE(TAG, "Error starting web server!");
    }

    while (webServerActive) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to allow other tasks to run
    }

    printf("Web server task finished. Stopping web server and disconnecting WiFi.\n");
    if (server != NULL) {
        httpd_stop(server);
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelete(NULL);
}

void uartTask(void *parameter) {
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_driver_install(UART_NUM, UART_BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    uint8_t* data = (uint8_t*) malloc(UART_BUFFER_SIZE);
    bool streamingStarted = false;

    for (;;) {
        if (!webServerActive) {
            if (!streamingStarted) {
                if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
                    std::string uart_message = "";
                    for (int i = 0; i < numbersCount; i++) {
                        uart_message += std::to_string(receivedNumbers[i]);
                        if (i < numbersCount - 1) {
                            uart_message += ",";
                        }
                    }
                    uart_message += "\n";
                    uart_write_bytes(UART_NUM, uart_message.c_str(), uart_message.length());
                    printf("Sent numbers over UART: %s\n", uart_message.c_str());
                    xSemaphoreGive(xMutex);
                }
                streamingStarted = true;
            }

            int len = uart_read_bytes(UART_NUM, data, UART_BUFFER_SIZE - 1, 20 / portTICK_PERIOD_MS);
            if (len > 0) {
                data[len] = '\0';
                if (data[0] == 'h') {
                    float value = strtof((const char *)(data + 1), NULL);
                    printf("Received haptic command: %s, value: %f\n", (const char*)data, value);
                    if (hapticQueue != NULL) {
                        xQueueSend(hapticQueue, &value, pdMS_TO_TICKS(10));
                    }
                } else if (data[0] == 'a') {
                    int value = atoi((const char *)(data + 1));
                    printf("Received audio command: %s, value: %d\n", (const char*)data, value);
                    if (audioQueue != NULL) {
                        xQueueSend(audioQueue, &value, pdMS_TO_TICKS(10));
                    }
                } else {
                    float value = strtof((const char *)data, NULL);
                    printf("Received unknown feedback value via UART: %f\n", value);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}


void setHapticEnable(gpio_num_t enablePin, bool enable) {
    gpio_set_level(enablePin, enable ? 1 : 0);
    ets_delay_us(500);
}

void playEffect(espp::Drv2605* drv, gpio_num_t enablePin, espp::Drv2605::Waveform effectId, int duration_ms) {
    while (!drv){
        vTaskDelay(pdMS_TO_TICKS(10));
        printf("Waiting for DRV2605 driver to be initialized...\n");
    } // Wait for the driver to be initialized
    setHapticEnable(enablePin, true);
    vTaskDelay(pdMS_TO_TICKS(5));

    std::error_code ec;
    drv->set_waveform(0, effectId, ec);
    drv->set_waveform(1, espp::Drv2605::Waveform::END, ec);
    drv->start(ec);
    if (ec) ESP_LOGE(TAG, "Haptic error: %s", ec.message().c_str());

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    drv->stop(ec);
    setHapticEnable(enablePin, false);
}

void haptic_task(void *pvParameters) {
    gpio_reset_pin(HAPTIC_EN1_PIN);
    gpio_set_direction(HAPTIC_EN1_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(HAPTIC_EN2_PIN);
    gpio_set_direction(HAPTIC_EN2_PIN, GPIO_MODE_OUTPUT);
    setHapticEnable(HAPTIC_EN1_PIN, false);
    setHapticEnable(HAPTIC_EN2_PIN, false);

    auto i2c_write = [](uint8_t addr, const uint8_t* data, size_t len) -> bool {
        return i2c_master_write_to_device(I2C_PORT, addr, data, len, pdMS_TO_TICKS(100)) == ESP_OK;
    };
    auto i2c_read_reg = [](uint8_t addr, uint8_t reg, uint8_t* data, size_t len) -> bool {
        return i2c_master_write_read_device(I2C_PORT, addr, &reg, 1, data, len, pdMS_TO_TICKS(100)) == ESP_OK;
    };

    setHapticEnable(HAPTIC_EN1_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    haptic1 = std::make_unique<espp::Drv2605>(espp::Drv2605::Config{
        .device_address = espp::Drv2605::DEFAULT_ADDRESS, .write = i2c_write, .read_register = i2c_read_reg,
        .motor_type = espp::Drv2605::MotorType::ERM, .log_level = espp::Logger::Verbosity::INFO
    });
    std::error_code ec;
    haptic1->select_library(espp::Drv2605::Library::ERM_1, ec);
    haptic1->set_mode(espp::Drv2605::Mode::INTTRIG, ec);
    setHapticEnable(HAPTIC_EN1_PIN, false);
    printf("DRV2605 on GPIO%d initialized.\n", HAPTIC_EN1_PIN);

    setHapticEnable(HAPTIC_EN2_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    haptic2 = std::make_unique<espp::Drv2605>(espp::Drv2605::Config{
        .device_address = espp::Drv2605::DEFAULT_ADDRESS, .write = i2c_write, .read_register = i2c_read_reg,
        .motor_type = espp::Drv2605::MotorType::ERM, .log_level = espp::Logger::Verbosity::INFO
    });
    haptic2->select_library(espp::Drv2605::Library::ERM_1, ec);
    haptic2->set_mode(espp::Drv2605::Mode::INTTRIG, ec);
    setHapticEnable(HAPTIC_EN2_PIN, false);
    printf("DRV2605 on GPIO%d initialized.\n", HAPTIC_EN2_PIN);

    float feedbackData = 0.0;
    const int DEADZONE = 5;

    while (1) {
        if (xQueueReceive(hapticQueue, &feedbackData, pdMS_TO_TICKS(10)) == pdTRUE) {
            printf("[HAPTIC] Received feedback value: %f\n", feedbackData);
            int feedbackInt = (int)feedbackData;
            
            if (feedbackInt < -DEADZONE) {
                playEffect(haptic1.get(), HAPTIC_EN1_PIN, STRONG_BUZZ_EFFECT, EFFECT_DURATION_MS);
            } else if (feedbackInt > DEADZONE) {
                playEffect(haptic2.get(), HAPTIC_EN2_PIN, STRONG_BUZZ_EFFECT, EFFECT_DURATION_MS);
            }
        }
    }
}

static void dac_audio_task(void *pvParameters) {
    dac_continuous_handle_t dac_handle = (dac_continuous_handle_t)pvParameters;
    printf("DAC audio task started\n");

    size_t audio_sizes[] = { sizeof(navigate_to), sizeof(navigation_complete), sizeof(turn), sizeof(left), sizeof(right), sizeof(around), sizeof(go_straight), sizeof(meters), sizeof(one), sizeof(two), sizeof(three), sizeof(four), sizeof(five), sizeof(six), sizeof(seven), sizeof(eight), sizeof(nine), sizeof(ten), sizeof(item_is_on), sizeof(aisle), sizeof(shelf) };
    uint8_t *audio_tracks[] = { (uint8_t *)navigate_to, (uint8_t *)navigation_complete, (uint8_t *)turn, (uint8_t *)left, (uint8_t *)right, (uint8_t *)around, (uint8_t *)go_straight, (uint8_t *)meters, (uint8_t *)one, (uint8_t *)two, (uint8_t *)three, (uint8_t *)four, (uint8_t *)five, (uint8_t *)six, (uint8_t *)seven, (uint8_t *)eight, (uint8_t *)nine, (uint8_t *)ten, (uint8_t *)item_is_on, (uint8_t *)aisle, (uint8_t *)shelf };
    size_t num_tracks = sizeof(audio_tracks) / sizeof(audio_tracks[0]);
    int audio_index;

    while(1) {
        if (xQueueReceive(audioQueue, &audio_index, portMAX_DELAY) == pdTRUE) {
            if (audio_index >= 0 && audio_index < num_tracks) {
                printf("Playing audio track: %d\n", audio_index);
                ESP_ERROR_CHECK(dac_continuous_write(dac_handle, audio_tracks[audio_index], audio_sizes[audio_index], NULL, -1));
            }
        }
    }
}


extern "C" void app_main(void)
{
    printf("Main app start\n");
    printf("--------------------------------------\n");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // Initialize GPIO for amp enable
    gpio_reset_pin(GPIO_NUM_33);
    gpio_set_direction(GPIO_NUM_33, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_33, 1);

    // Create mutex and queue
    xMutex = xSemaphoreCreateMutex();
    hapticQueue = xQueueCreate(10, sizeof(float));
    audioQueue = xQueueCreate(10, sizeof(int));

    // Initialize I2C
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {.clk_speed = 400000},
        .clk_flags = 0, // Initialize this field
    };
    i2c_param_config(I2C_PORT, &conf);
    i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0);

    // Configure DAC
    dac_continuous_handle_t dac_handle;
    dac_continuous_config_t cont_cfg = {
        .chan_mask = DAC_CHANNEL_MASK_CH0,
        .desc_num = 4,
        .buf_size = 2048,
        .freq_hz = CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
    ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
    printf("DAC initialized success, DAC DMA is ready\n");

    // Create tasks
    xTaskCreate(webServerTask, "WebServerTask", 8192, NULL, 5, &webServerTaskHandle);
    xTaskCreate(uartTask, "UartTask", 4096, NULL, 5, &uartTaskHandle);
    xTaskCreate(haptic_task, "haptic_task", 4096, NULL, 5, &hapticTaskHandle);
    xTaskCreate(dac_audio_task, "dac_audio_task", 4096, dac_handle, 5, &dacTaskHandle);
    
    printf("All tasks created.\n");
}
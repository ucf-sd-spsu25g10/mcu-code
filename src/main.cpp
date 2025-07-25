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
#include "driver/dac_oneshot.h"
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
#include "obstacle_is.h"

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
static constexpr gpio_num_t AUDIO_EN_PIN = GPIO_NUM_33; // GPIO pin to enable audio DAC
static constexpr gpio_num_t LED_PIN = GPIO_NUM_27; // GPIO pin for LED indicator

// Haptic effect definitions

static constexpr int EFFECT_DURATION_MS = 350; // Shorter duration for responsiveness

// Haptic Driver Instances
std::unique_ptr<espp::Drv2605> haptic1;
std::unique_ptr<espp::Drv2605> haptic2;

// Audio settings
static const int CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE = 48000 / 2;

// Shared data
const int MAX_NUMBERS = 20;
int receivedNumbers[MAX_NUMBERS];
int numbersCount = 0;
SemaphoreHandle_t xMutex = NULL;
QueueHandle_t hapticQueue;
QueueHandle_t audioQueue;
volatile bool webServerActive = true;



// UART settings
#define UART_NUM UART_NUM_2
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
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
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

esp_err_t cartlist_options_handler(httpd_req_t *req) {
    // Set CORS headers
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");

    // Respond with 204 No Content
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);

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
    ESP_LOGI(TAG, "Received POST data: %s", content);

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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    webServerActive = false;
    ESP_LOGI(TAG, "Valid JSON received. Web server will disconnect.");
    return ESP_OK;
}

esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_FAIL;
}

// Tasks
void webServerTask(void *parameter) {
    vTaskDelay(pdMS_TO_TICKS(2000)); // Delay to allow other tasks to initialize
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
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected.");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "WiFi connection failed.");
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    

    ESP_LOGI(TAG, "Starting web server");
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

        httpd_uri_t cartlist_uri_options = {
            .uri       = "/api/cartList",
            .method    = HTTP_OPTIONS,
            .handler   = cartlist_options_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &cartlist_uri_options);

        ESP_LOGI(TAG, "HTTP server started");
    } else {
        ESP_LOGE(TAG, "Error starting web server!");
    }

    while (webServerActive) {
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to allow other tasks to run
    }

    ESP_LOGI(TAG, "Web server task finished. Stopping web server and disconnecting WiFi.");
    if (server != NULL) {
        httpd_stop(server);
    }
    esp_wifi_disconnect();
    esp_wifi_stop();
    vTaskDelete(NULL);
}

void uartTask(void *parameter) {
    uart_config_t uart_config = {};
    uart_config.baud_rate = 115200;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = 122;
    uart_config.source_clk = UART_SCLK_APB; // Use APB clock for UART
    esp_err_t err = uart_driver_install(UART_NUM, UART_BUFFER_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    err = uart_param_config(UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART parameters: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    err = uart_set_pin(UART_NUM, GPIO_NUM_23, GPIO_NUM_22, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

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
                    uart_message += ";\n";
                    int len = uart_write_bytes(UART_NUM, uart_message.c_str(), uart_message.length());
                    ESP_LOGI(TAG, "Sent numbers over UART: %s", len > 0 ? uart_message.c_str() : "Failed to send");
                    xSemaphoreGive(xMutex);
                }
                streamingStarted = true;
            }

            int len = uart_read_bytes(UART_NUM, data, UART_BUFFER_SIZE - 1, 20 / portTICK_PERIOD_MS);
            if (len > 0) {
                ESP_LOGI(TAG, "Read %d bytes from UART", len);
                data[len] = '\0';
                std::string uart_input(reinterpret_cast<char*>(data));
                size_t start = 0, end = 0;
                while ((end = uart_input.find('\n', start)) != std::string::npos) {
                    std::string cmd = uart_input.substr(start, end - start);
                    start = end + 1;
                    if (cmd.empty()) continue;
                    if (cmd[0] == 'h') {
                        float value = strtof(cmd.c_str() + 1, NULL);
                        ESP_LOGI(TAG, "Received haptic command: %s, value: %f", cmd.c_str(), value);
                        if (hapticQueue != NULL) {
                            xQueueSend(hapticQueue, &value, pdMS_TO_TICKS(10));
                        }
                    } else if (cmd[0] == 'a') {
                        int value = atoi(cmd.c_str() + 1);
                        ESP_LOGI(TAG, "Received audio command: %s, value: %d", cmd.c_str(), value);
                        if (audioQueue != NULL) {
                            xQueueSend(audioQueue, &value, pdMS_TO_TICKS(10));
                        }
                    } else {
                        float value = strtof(cmd.c_str(), NULL);
                        ESP_LOGW(TAG, "Received unknown feedback value via UART: %f", value);
                    }
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



void haptic_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(1000));
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
    haptic1->set_mode(espp::Drv2605::Mode::REALTIME, ec);
    if (ec) {
        ESP_LOGE(TAG, "Failed to set DRV2605 (HAPTIC_EN1_PIN) to REALTIME mode: %s", ec.message().c_str());
    } else {
        ESP_LOGI(TAG, "DRV2605 on GPIO%d set to REALTIME mode.", HAPTIC_EN1_PIN);
    }
    setHapticEnable(HAPTIC_EN1_PIN, false);
    ESP_LOGI(TAG, "DRV2605 on GPIO%d initialized.", HAPTIC_EN1_PIN);

    setHapticEnable(HAPTIC_EN2_PIN, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    haptic2 = std::make_unique<espp::Drv2605>(espp::Drv2605::Config{
        .device_address = espp::Drv2605::DEFAULT_ADDRESS, .write = i2c_write, .read_register = i2c_read_reg,
        .motor_type = espp::Drv2605::MotorType::ERM, .log_level = espp::Logger::Verbosity::INFO
    });
    haptic2->select_library(espp::Drv2605::Library::ERM_1, ec);
    haptic2->set_mode(espp::Drv2605::Mode::REALTIME, ec);
    if (ec) {
        ESP_LOGE(TAG, "Failed to set DRV2605 (HAPTIC_EN2_PIN) to REALTIME mode: %s", ec.message().c_str());
    } else {
        ESP_LOGI(TAG, "DRV2605 on GPIO%d set to REALTIME mode.", HAPTIC_EN2_PIN);
    }
    setHapticEnable(HAPTIC_EN2_PIN, false);
    ESP_LOGI(TAG, "DRV2605 on GPIO%d initialized.", HAPTIC_EN2_PIN);

    #ifdef DEBUG_MODE
    int8_t pwm_value = 0;
    bool haptic1_turn = true;
    while (1) {
        if (haptic1_turn) {
            ESP_LOGI(TAG, "[DEBUG] Haptic 1 PWM: %d", pwm_value);
            setHapticEnable(HAPTIC_EN2_PIN, false);
            haptic2->set_rtp_pwm_signed(0, ec);
            setHapticEnable(HAPTIC_EN1_PIN, true);
            haptic1->set_rtp_pwm_signed(pwm_value, ec);
        } else {
            ESP_LOGI(TAG, "[DEBUG] Haptic 2 PWM: %d", pwm_value);
            setHapticEnable(HAPTIC_EN1_PIN, false);
            haptic1->set_rtp_pwm_signed(0, ec);
            setHapticEnable(HAPTIC_EN2_PIN, true);
            haptic2->set_rtp_pwm_signed(pwm_value, ec);
        }

        haptic1_turn = !haptic1_turn;
        
        if(haptic1_turn) { // after haptic 2 has run, increase pwm
            pwm_value += 10;
            if (pwm_value > 120 || pwm_value < 0) { // Handle increment and overflow
                pwm_value = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    #else
    float feedbackData = 0.0;

    while (1) {
        if (xQueueReceive(hapticQueue, &feedbackData, pdMS_TO_TICKS(10)) == pdTRUE) {
            ESP_LOGI(TAG, "[HAPTIC] Received feedback value: %f", feedbackData);
            float scaled_value = 0;
            if (feedbackData < 0) {
                float abs_val = feedbackData * -1.0f;
                // Exponential scaling: y = 127 * (1 - exp(-k * abs_val / 255)), k=4
                float k = 4.0f;
                scaled_value = 127.0f * (1.0f - expf(-k * abs_val / 255.0f));
                if (scaled_value > 127.0f) scaled_value = 127.0f; // clamp
                if (scaled_value < 5.0f) scaled_value = 0.0f; // Deadzone
                int8_t pwm_value = static_cast<int8_t>(scaled_value);
                std::error_code ec;
                setHapticEnable(HAPTIC_EN1_PIN, true);
                haptic1->set_rtp_pwm_signed(pwm_value, ec);
                vTaskDelay(pdMS_TO_TICKS(EFFECT_DURATION_MS));
                haptic1->set_rtp_pwm_signed(0, ec);
                setHapticEnable(HAPTIC_EN1_PIN, false);
            } else if (feedbackData > 0) {
                // Exponential scaling: y = 127 * (1 - exp(-k * feedbackData / 255)), k=4
                float k = 4.0f;
                scaled_value = 127.0f * (1.0f - expf(-k * feedbackData / 255.0f));
                if (scaled_value > 127.0f) scaled_value = 127.0f; // clamp
                if (scaled_value < 5.0f) scaled_value = 0.0f; // Deadzone
                int8_t pwm_value = static_cast<int8_t>(scaled_value);
                std::error_code ec;
                setHapticEnable(HAPTIC_EN2_PIN, true);
                haptic2->set_rtp_pwm_signed(pwm_value, ec);
                vTaskDelay(pdMS_TO_TICKS(EFFECT_DURATION_MS));
                haptic2->set_rtp_pwm_signed(0, ec);
                setHapticEnable(HAPTIC_EN2_PIN, false);
            }
        }
    }
#endif
}

void dac_output_task(void *pvParameters) {
    #ifndef DEBUG_MODE // Enables laser DAC immediately if in DEBUG_MODE
    while (webServerActive) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    #endif

    // Configure and set DAC1 for constant output
    dac_oneshot_handle_t dac1_handle;
    dac_oneshot_config_t dac1_cfg = {
        .chan_id = DAC_CHAN_1,
    };
    ESP_ERROR_CHECK(dac_oneshot_new_channel(&dac1_cfg, &dac1_handle));
    // Set DAC output to max value (255) which is ~3.3V, scaled to 250mV.
    ESP_ERROR_CHECK(dac_oneshot_output_voltage(dac1_handle, 255 * (0.25 / 3.3)));
    ESP_LOGI(TAG, "DAC1 (GPIO26) set to constant output.");

    vTaskDelete(NULL);
}

static void dac_audio_task(void *pvParameters) {
    dac_continuous_handle_t dac_handle = (dac_continuous_handle_t)pvParameters;
    ESP_LOGI(TAG, "DAC audio task started");

    size_t audio_sizes[] = { sizeof(navigate_to_audio_data), sizeof(navigation_complete_audio_data), sizeof(turn_audio_data), sizeof(left_audio_data), sizeof(right_audio_data), sizeof(around_audio_data), sizeof(go_straight_audio_data), sizeof(meters_audio_data), sizeof(one_audio_data), sizeof(two_audio_data), sizeof(three_audio_data), sizeof(four_audio_data), sizeof(five_audio_data), sizeof(six_audio_data), sizeof(seven_audio_data), sizeof(eight_audio_data), sizeof(nine_audio_data), sizeof(ten_audio_data), sizeof(item_is_on_audio_data), sizeof(aisle_audio_data), sizeof(shelf_audio_data), sizeof(obstacle_is_audio_data) };
    uint8_t *audio_tracks[] = { (uint8_t *)navigate_to_audio_data, (uint8_t *)navigation_complete_audio_data, (uint8_t *)turn_audio_data, (uint8_t *)left_audio_data, (uint8_t *)right_audio_data, (uint8_t *)around_audio_data, (uint8_t *)go_straight_audio_data, (uint8_t *)meters_audio_data, (uint8_t *)one_audio_data, (uint8_t *)two_audio_data, (uint8_t *)three_audio_data, (uint8_t *)four_audio_data, (uint8_t *)five_audio_data, (uint8_t *)six_audio_data, (uint8_t *)seven_audio_data, (uint8_t *)eight_audio_data, (uint8_t *)nine_audio_data, (uint8_t *)ten_audio_data, (uint8_t *)item_is_on_audio_data, (uint8_t *)aisle_audio_data, (uint8_t *)shelf_audio_data, (uint8_t *)obstacle_is_audio_data };
    size_t num_tracks = sizeof(audio_tracks) / sizeof(audio_tracks[0]);

    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Number of audio tracks: %d", num_tracks);
    for (size_t i = 0; i < num_tracks; ++i) {
        ESP_LOGI(TAG, "Audio track %d size: %d", i, audio_sizes[i]);
    }

    while(1) {
        #ifdef DEBUG_MODE // Loop through all audio tracks in DEBUG_MODE
        for (size_t i = 0; i < num_tracks; ++i) {
            ESP_LOGI(TAG, "DEBUG_MODE: Playing audio track: %d, size: %d", i, audio_sizes[i]);
            esp_err_t err = dac_continuous_write(dac_handle, audio_tracks[i], audio_sizes[i], NULL, -1);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "dac_continuous_write failed for track %d with error: %s", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(audio_sizes[i] * 1000 / CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE + 500)); // Add 500ms buffer
            ESP_ERROR_CHECK(dac_continuous_disable(dac_handle));
            ESP_ERROR_CHECK(dac_continuous_del_channels(dac_handle));
            dac_continuous_config_t cont_cfg = {
                .chan_mask = DAC_CHANNEL_MASK_ALL, // Use all channels for DAC
                .desc_num = 4,
                .buf_size = 2048,
                .freq_hz = CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE,
                .offset = 0,
                .clk_src = DAC_DIGI_CLK_SRC_APLL,
                .chan_mode = DAC_CHANNEL_MODE_ALTER, // Use alternating mode for single channel
            };
            ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
            ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
        }
        #else // Normal operation waits for audio index from queue
        int audio_index;
        if (xQueueReceive(audioQueue, &audio_index, portMAX_DELAY) == pdTRUE) {
            if (audio_index >= 0 && (size_t)audio_index < num_tracks) {
                ESP_LOGI(TAG, "Playing audio track: %d", audio_index);
                ESP_ERROR_CHECK(dac_continuous_write(dac_handle, audio_tracks[audio_index], audio_sizes[audio_index], NULL, -1));
                vTaskDelay(pdMS_TO_TICKS(audio_sizes[audio_index] * 1000 / CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE + 500)); // Add 500ms buffer
                ESP_ERROR_CHECK(dac_continuous_disable(dac_handle));
                ESP_ERROR_CHECK(dac_continuous_del_channels(dac_handle));
                dac_continuous_config_t cont_cfg = {
                    .chan_mask = DAC_CHANNEL_MASK_ALL, // Use all channels for DAC
                    .desc_num = 4,
                    .buf_size = 2048,
                    .freq_hz = CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE,
                    .offset = 0,
                    .clk_src = DAC_DIGI_CLK_SRC_APLL,
                    .chan_mode = DAC_CHANNEL_MODE_ALTER, // Use alternating mode for single channel
                };
                ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));
                ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
            }
        }
        #endif
    }
}

static void led_blink_task(void *pvParameters) {
    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);
    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Main app start in 2 seconds...");

    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG, "Starting NAVIS MCU application...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    

    // Initialize GPIO for amp enable
    gpio_reset_pin(AUDIO_EN_PIN);
    gpio_set_direction(AUDIO_EN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_EN_PIN, 1);

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
        .chan_mask = DAC_CHANNEL_MASK_CH0, // Use CH0 for audio
        .desc_num = 4,
        .buf_size = 2048,
        .freq_hz = CONFIG_EXAMPLE_AUDIO_SAMPLE_RATE,
        .offset = 0,
        .clk_src = DAC_DIGI_CLK_SRC_APLL,
        .chan_mode = DAC_CHANNEL_MODE_ALTER, // Use alternating mode for single channel
    };
    ESP_ERROR_CHECK(dac_continuous_new_channels(&cont_cfg, &dac_handle));

    

    ESP_ERROR_CHECK(dac_continuous_enable(dac_handle));
    ESP_LOGI(TAG, "DAC initialized success, DAC DMA is ready");

    // Create tasks
    xTaskCreate(webServerTask, "WebServerTask", 8192, NULL, 5, &webServerTaskHandle);
    xTaskCreate(uartTask, "UartTask", 4096, NULL, 5, &uartTaskHandle);
    xTaskCreate(haptic_task, "haptic_task", 4096, NULL, 5, &hapticTaskHandle);
    xTaskCreate(dac_audio_task, "dac_audio_task", 4096, dac_handle, 5, &dacTaskHandle);
    //xTaskCreate(dac_output_task, "dac_output_task", 2048, NULL, 5, NULL);
    xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "All tasks created.");
}